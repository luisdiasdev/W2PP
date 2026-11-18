# Component Deep Analysis Report

**Component:** Action Handler
**Primary artifact:** `Code/TMSrv/_MSG_Action.cpp` (340 lines, 9,161 bytes, single function `Exec_MSG_Action`)
**Project:** W2PP (decompiled C/C++, Win32 / MSVC v143, MMORPG server emulator for W2)
**Host process:** TMSrv (game / world server)
**Analysis date:** 2026-11-15 22:22:31
**Analysis scope:** entire project root `E:\wyd\test-setup\w2pp`, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Architectural role (given):** move/position desync checks and grid updates. Afferent 1, efferent 14 (see section 6 for the independently measured figures).

---

## 1. Executive Summary

`Action Handler` is the TMSrv subsystem that turns a client movement frame into authoritative world position. It is the only writer of a player's `pMob[].TargetX/TargetY` that is driven directly by player input, and therefore the single chokepoint where movement validation, zone gating, grid occupancy and area-of-interest broadcast meet. The component is one 320-line function, `Exec_MSG_Action(int conn, char *pMsg)` at `Code/TMSrv/_MSG_Action.cpp:21`, reached from the client dispatcher at `Code/TMSrv/ProcessClientMessage.cpp:96-100`, which funnels three distinct wire types (`_MSG_Action` = 876, `_MSG_Action2` = 870, `_MSG_Action3` = 872, `Code/Basedef.h:1693-1695`) into the same body.

The function has four sequential phases. First a state-guard ladder (mode, HP, trade, celestial recall) at `:25-60`. Second a per-type tick audit (`:63-145`) that distinguishes "walk" from "stop" from the mage "Ilusao" skill move. Third an authorisation ladder of eight zone/territory rules (`:147-238`) implemented as magic coordinate rectangles and map-attribute bit tests. Fourth a commit phase (`:240-338`) that rewrites the mob route, resolves grid collisions, calls `GridMulticast` (which is where position is actually committed, `Code/TMSrv/SendFunc.cpp:620-823`), re-encodes a route digit into the mob's merchant byte, and applies two late "event zone" recall rules.

The critical characteristic of this component is that **the movement itself is never measured**. There is no distance-per-time validation, no server-side path recomputation for the accepted route, and no reconstruction of the client's claimed displacement. The only displacement ceiling is the visible-area test of 33 cells per packet (`:160`), the only speed input is the client-declared `m->Speed` (`:147`), and the only cadence limiter (900 ms, `:92` and `:115`) is permanently disabled by a login-time sentinel (`pUser[conn].LastActionTick = SKIPCHECKTICK`, `Code/TMSrv/ProcessDBMessage.cpp:825`, never reset). In practice a client may relocate up to 33 cells per action packet, at packet rate, by declaring any speed value at or below its own run nibble.

Key findings:

- **Confirmed defect: `sizeof(m)` used as a packet length** at `_MSG_Action.cpp:272` and `:278`. Independently verified against the build configuration (`TMSrv.vcxproj` targets `Win32` only, lines 6 and 10, so `sizeof(MSG_Action*) == 4`). `MSG_Action` is 52 bytes, not the 48 stated by the sibling `Client Handler Interface Contract` report. Because `CPSock::AddMessage` overwrites the header `Size` with the passed length and copies exactly four bytes (`Code/CPSock.cpp:538`, `:586`, `:588`), the two "correction" frames reach the wire as 4-byte frames whose `Type`/`ID`/`ClientTick` are never written. They are unparseable and are silently dropped by the client.
- **No packet-length validation and no payload sanitisation.** `m->Size` is never read. `strlen(m->Route)` at `:251` runs over a 24-byte client field with no guaranteed terminator inside a 128 KB receive buffer (`CPSock.h:35`). The receive buffer is then re-broadcast to every player in view through `GridMulticast`, which forwards it with the attacker-controlled `msg->Size` (`SendFunc.cpp:714`, `:799`). A declared size up to `MAX_MESSAGE_SIZE` (8,192, `CPSock.h:38`) causes `AddMessage` to advance the send pointer by that amount, so up to 8,188 bytes of stale send-buffer memory are delivered to nearby clients. This is a credible information-disclosure and stream-desynchronisation vector initiated through this handler.
- **`m->ID` is never rewritten.** The relayed frame keeps whatever `ID` the client put in the header, so clients in view animate a mob that the server never moved, while the server moves `conn`. The dispatcher only range-checks `ID` (`ProcessClientMessage.cpp:42-51`); no handler-level identity binding exists here.
- **The 900 ms cadence limiter is dead code.** `LastActionTick` is written in exactly one place in the whole project (`ProcessDBMessage.cpp:825`, value `SKIPCHECKTICK`), and the guards at `:92` and `:115` are of the form `LastActionTick != SKIPCHECKTICK && ...`. Both are always false for a logged-in character. Additionally the `_MSG_Action` branch stores its timestamp into `LastIllusionTick` (`:133`), not `LastActionTick`.
- **`_MSG_Action2` (stop) is unchecked and still moves the player.** Its only audit is "two consecutive stops are refused" (`:138-144`); after that the packet falls through into the entire movement pipeline with no NUL-window and no 15 s clock check.
- **`pMob[].LastSpeed`, `LastTime`, `LastX`, `LastY` are write-only.** They are set by `GridMulticast` (`SendFunc.cpp:815-819`) and read nowhere in the project (verified by exhaustive grep). The state needed for a speed-hack detector exists but is never consulted.
- **The handler mutates its input buffer beyond the declared frame.** `m->Speed` is normalised in place at `:154` and `m->TargetX/m->TargetY` at `:267-278`, both without regard to `m->Size`; with a 12-byte frame those writes land in the next queued packet inside `pRecvBuffer`.
- **Two late recall rules and five earlier ones overlap incoherently.** Eight independent coordinate/attribute policies can each terminate the move, four of them by teleporting the player; the ordering means a player inside the 2604-2648 x 1708-1744 event rectangle is first subject to the guild-zone test (`:223`) and only afterwards to the battle-royale/Colo tests (`:309-338`).
- **Zero test coverage exists in the repository** - no test project, no test framework, no fixtures, no CI configuration (evidence in section 11).

---

## 2. Data Flow Analysis

```
 1. Client sends _MSG_Action (876), _MSG_Action2 (870) or _MSG_Action3 (872)
      payload MSG_Action { Size, KeyWord, CheckSum, Type, ID, ClientTick,
                           PosX, PosY, Effect, Speed, Route[24], TargetX, TargetY }  (52 bytes)
 2. CPSock::ReadMessage (CPSock.cpp:353) validates 12 <= Size <= 8192, decrypts in place,
      returns a pointer INTO pRecvBuffer (128 KB) -> the handler receives a mutable view
 3. Server.cpp:3975-4001 (WSA_READ socket event) -> ProcessClientMessage(User, Msg, FALSE)
 4. ProcessClientMessage.cpp:42-64 validates ID range, ServerDown, refreshes LastReceiveTime,
      rejects ClientTick == SKIPCHECKTICK for client-sourced frames
 5. ProcessClientMessage.cpp:96-100 switch(std->Type) -> Exec_MSG_Action(conn, pMsg)
 6. _MSG_Action.cpp:23 cast to MSG_Action*, NO size check
 7. State gate ladder:
      :25 Mode != USER_PLAY            -> SendHpMode (no-op) + return
      :32 CurrentScore.Hp == 0         -> SendHpMode + AddCrackError(5,3) + return
      :40 Trade.OpponentID != 0        -> RemoveTrade(both) + return
      :46 TradeMode != 0               -> RemoveTrade(conn) + return
      :52 TargetX/128,TargetY/128 in {(9,1),(8,2),(10,2)} and celestial>=180 -> DoRecall + return
 8. Clock window: movetime = m->ClientTick; checktime = CurrentTime-120000 (0 if CurrentTime<=120000)   (:63-69)
 9. Per-type audit:
      _MSG_Action3 :74-79 class 3 + LearnedSkill&2, else AddCrackError(10,28) with logout abort
                   :81-90 MP cost g_pSpell[73].ManaSpent, deduct Mp and pUser.ReqMp
                   :92-99 900 ms limiter (inert)  | :100-109 15 s future / 120 s past window
                   :110 LastIllusionTick = movetime
      _MSG_Action  :115-122 900 ms limiter (inert) | :123-132 15 s / 120 s window
                   :133 LastIllusionTick = movetime (LastActionTick untouched)
      _MSG_Action2 :138-144 refuse if previous frame was also _MSG_Action2
10. Declared-speed clamp: m->Speed > (CurrentScore.AttackRun & 0xF) -> AddCrackError(5,4),
      log, m->Speed = (AttackRun & 0xF)   (:147-155)
11. Displacement gate: |TargetX-posX| > 33 or |TargetY-posY| > 33   (:157-175)
      beyond 66 -> emit corrective MSG_Action3 snap-back (GetAction + Speed=6)  [malformed, see D-01]
      always AddCrackError(1,5) + return
12. Arena gate: rect 3378..3398 x 1427..1448, non-registered party leader -> DoTeleport(PistaPos[2][rand%3])  (:177-190)
13. Coordinate domain: 1..4095 both axes, else log + silent return   (:192-196)
14. Trade-only gate: OnlyTrade -> require village zone (<5) or cell attribute 0x80, else message + DoRecall   (:198-209)
15. Newbie-zone gate: cell 0x80 blocks (35 <= level <= 999) or non-MORTAL   (:215-221)
16. Guild-zone gate: cell 0x20 and level <= 399 and guild != ChargeGuild -> message + DoRecall   (:223-236)
17. Castle quest gate: CCastleZakum::CheckMove(conn, TargetX, TargetY)   (:238)
18. Bookkeeping: LastMove = m->Type; LastSpeed = 0; strncpy(pMob.Route, m->Route, 24)   (:240-244)
19. Grid collision: if target cell holds another mob -> GetEmptyMobGrid + BASE_GetRoute recompute,
      Effect = 0, self correction packet [malformed] or self echo [malformed]   (:246-279)
20. Route commit: memcpy(pMob.Route, m->Route, 24)   (:281)
21. GRID COMMIT + BROADCAST: GridMulticast(conn, TargetX, TargetY, pMsg)   (:283)
      SendFunc.cpp:620-823
        a. clear old cell, heal stolen cell via GetEmptyMobGrid
        b. pMobGrid[ty][tx] = conn
        c. old-window vs new-window diff -> SendRemoveMob (both directions), SendRemoveItem
        d. new-only window -> SendCreateMob (both directions), SendPKInfo, relay the frame
        e. commit: pMob.LastTime = sm->ClientTick, LastSpeed = sm->Speed,
                   LastX/LastY = sm->PosX/PosY, TargetX = tx, TargetY = ty
22. _MSG_Action3 only: echo frame to caster (correct sizeof(MSG_Action)) + SendSetHpMp   (:285-289)
23. Merchant re-encoding: last route digit -> nibble 0x10..0x90 OR-ed into CurrentScore.Merchant/BaseScore.Merchant  (:292-307)
24. Battle Royale gate: BrState && BRItem>0, rect 2604..2648 x 1708..1744, level>=100 (BrGrid 0) or >=200 (BrGrid 1) -> DoRecall  (:309-324)
25. Colo-150 gate: Colo150Limit, same rect, level>=150 -> DoRecall   (:326-338)
```

Note that step 21 is the **only** place a player's authoritative position changes; every earlier `return` leaves `pMob[conn].TargetX/TargetY` untouched, and every successful pass ends there. `DoRecall` and `DoTeleport` are the two alternative commit paths (they call `GetAction` + `GridMulticast` themselves, `Code/TMSrv/Server.cpp:7558-7602` and `:8212-8230`).

---

## 3. Business Rules & Logic

### Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation / state | Only a session in `USER_PLAY` may move; otherwise the frame is discarded after a status resend | `_MSG_Action.cpp:25-30` |
| BR-02 | Validation / state | A character with `CurrentScore.Hp == 0` may not move; +5 crack points recorded silently (type 3 is exempt from logging in `AddCrackError`) | `:32-38` |
| BR-03 | Business logic | Any movement while a trade is open cancels the trade (both sides) and aborts the move | `:40-45` |
| BR-04 | Business logic | Any movement during an auto-trade session cancels that session and aborts the move | `:46-50` |
| BR-05 | Business logic | Celestial-tier characters at level >= 180 present in map blocks (9,1), (8,2) or (10,2) of the 128-cell block grid are recalled to their spawn | `:52-60` |
| BR-06 | Validation / anti-cheat | Movement timestamps must lie inside `[CurrentTime-120000, CurrentTime+15000]` ms | `:66-69`, `:100-109`, `:123-132` |
| BR-07 | Validation / anti-cheat | Illusion-move (`_MSG_Action3`) requires class 3 and learned-skill bit `0x2`; otherwise a crack event is recorded and, below `MAX_LEVEL`, the session may be terminated | `:72-79` |
| BR-08 | Business logic | Illusion-move costs `g_pSpell[73].ManaSpent` MP, unconditionally, before any rate check | `:81-90` |
| BR-09 | Validation / anti-cheat | Illusion-move cadence limited to one per 900 ms (INERT: guarded by `LastActionTick != SKIPCHECKTICK`) | `:92-99` |
| BR-10 | Validation / anti-cheat | Walk cadence limited to one per 900 ms (INERT, same sentinel; also stores the wrong field) | `:115-122`, `:133` |
| BR-11 | Validation / anti-cheat | Two consecutive `_MSG_Action2` (stop) frames are refused; the timestamp audit of BR-06/BR-09/BR-10 is skipped entirely for this type | `:136-145` |
| BR-12 | Validation | The client-declared `Speed` may not exceed the low nibble of `CurrentScore.AttackRun`; excess is clamped in place and logged | `:147-155` |
| BR-13 | Validation / anti-cheat | The requested cell must be within +/-33 cells of the current cell in both axes; beyond +/-66 the server sends a snap-back | `:157-175` |
| BR-14 | Business logic | Only the three registered party leaders of arena slot 2 may stand in rect 3378..3398 x 1427..1448; anyone else is teleported to a random slot-2 arena position | `:177-190` |
| BR-15 | Validation | Target coordinates must satisfy `1 <= X <= 4095` and `1 <= Y <= 4095`; violations are logged and the frame is dropped silently | `:192-196` |
| BR-16 | Business logic | Multi-client trade-only accounts may only enter a village zone (index 0..4) or a cell carrying attribute `0x80`; otherwise they are recalled | `:198-209` |
| BR-17 | Business logic | Newbie-zone cells (attribute `0x80`) are closed to characters with `35 <= level <= 999` and to every non-MORTAL class master | `:215-221` |
| BR-18 | Business logic | Guild-zone cells (attribute `0x20`) are closed to characters whose guild is not the charging guild of that zone (only enforced for level <= `MAX_LEVEL`) | `:223-236` |
| BR-19 | Business logic | Castle-quest zone rectangles are closed to players not registered in the castle quest party | `:238` -> `CCastleZakum.cpp:377-394` |
| BR-20 | State bookkeeping | `LastMove` records the wire type of the last accepted move; `LastSpeed` is reset to 0 before the commit | `:240-242` |
| BR-21 | State mutation | The client-supplied `Route[24]` is copied verbatim into `pMob[conn].Route` twice (before and after collision resolution) | `:244`, `:281` |
| BR-22 | Business logic | An occupied destination triggers a relocation attempt: find a free cell, then re-derive the route from `m->PosX/m->PosY`; `Effect` is downgraded to 0 (walk) | `:246-279` |
| BR-23 | Business logic | Grid commit and area-of-interest update: the mob is moved in `pMobGrid`, its old/new 33x33 windows are diffed and mobs/items are created or removed on both sides | `:283` -> `SendFunc.cpp:620-811` |
| BR-24 | Business logic | The route digit found after the last route character (`Route[len] + 3 - 48`) is re-encoded as `0x10 * N` into bits 4-7 of `CurrentScore.Merchant` and `BaseScore.Merchant` | `:292-307` |
| BR-25 | Business logic | During Battle Royale (`BrState` and `BRItem > 0`) the event rectangle 2604..2648 x 1708..1744 is closed to level >= 100 (open grid) or level >= 200 (closed grid) | `:309-324` |
| BR-26 | Business logic | When `Colo150Limit` is set, the same rectangle is closed to every level >= 150 | `:326-338` |
| BR-27 | Audit | Each rejection contributes to `pUser[].NumError`; at 2,000,000,000 the session is terminated with `_NN_Bad_Network_Packets` | `:36`, `:77`, `:97`, `:107`, `:120`, `:130`, `:141`, `:149`, `:173` -> `Server.cpp:684-707` |
| BR-28 | Audit | Every clock/coordinate rejection writes a `Log()` line containing the offending tick values | `:94-96`, `:102-105`, `:117-119`, `:125-128`, `:151-152`, `:194` |
| BR-29 | Business logic | `_MSG_Action3` frames are echoed back to the caster with a full HP/MP resend after the commit | `:285-289` |
| BR-30 | Validation | `Effect` (0 = walking, 1 = teleporting, `Basedef.h:1702`) is not validated at all; the check is commented out | `:160` |

---

### Business Rule: State-gate ladder (BR-01, BR-02, BR-03, BR-04)

**Overview**:
Before any coordinate is examined, the handler refuses the frame unless the session is in the playing state, the character is alive, and no trade session is active. Trade activity does not merely block the move; it is destructively cancelled.

**Detailed description**:
`pUser[conn].Mode != USER_PLAY` (`:25`) is the outermost gate. The handler then calls `SendHpMode(conn)` to push the authoritative mode/HP pair back at the client. That call is a no-op in exactly the case it is meant to serve: `SendHpMode` begins with the same test (`Code/TMSrv/SendFunc.cpp:1627-1628`) and returns immediately when `Mode != USER_PLAY`. The rule still enforces "no movement outside play", but the compensating status push never happens, so a client that believes it is in-game receives nothing and the divergence is not corrected. The same no-op applies to the dead-character path at `:32-38`, except there the mode is `USER_PLAY` by construction, so the resend does reach the client and `AddCrackError(conn, 5, 3)` records five points. Type 3 is one of the three audit types that `AddCrackError` deliberately does not log (`Server.cpp:686`), so dead-client movement attempts are silent in the log while still accumulating points.

The trade rules are stronger than a simple veto. If `pUser[conn].Trade.OpponentID` is non-zero (`:40`), the handler calls `RemoveTrade` for the opponent first and then for the mover and returns. `RemoveTrade` (`Server.cpp:7319-7352`) zeroes the trade structure, resets the 15 inventory positions to -1, clears the auto-trade slot list, sends signal `900` to the client, and - when the session was in `TradeMode` - rebuilds and broadcasts the player's appearance so the trade pose is dropped. Because both sides are torn down, a single movement packet from one participant aborts the transaction for both; this is the server's answer to "walk away from a trade" and simultaneously a denial vector, since any movement spam ends legitimate trades.

`TradeMode` at `:46` covers the auto-trade shop state instead of a bilateral trade; only the mover is cleaned up. The two guards are deliberately separate because the structures differ (`STRUCT_MSG_Trade` versus `MSG_SendAutoTrade`, both embedded in `CUser`, `Code/TMSrv/CUser.h:59-65`), and because an auto-trade session legitimately has no opponent. Both paths return without moving, which means an open trade freezes movement rather than merely being interrupted mid-move; the client must re-issue its action after the trade closes.

**Rule workflow**:
```
Mode != USER_PLAY          -> SendHpMode(no-op) -> return
Hp == 0                    -> SendHpMode         -> AddCrackError(5, type 3, silent) -> return
Trade.OpponentID != 0      -> RemoveTrade(opponent) -> RemoveTrade(conn) -> return
TradeMode != 0             -> RemoveTrade(conn) -> return
```

---

### Business Rule: Celestial block recall (BR-05)

**Overview**:
Celestial-tier characters of level 180 or above may not remain inside three specific 128x128 map blocks; the move is converted into a recall to their spawn point.

**Detailed description**:
The test at `:52` divides both target coordinates by 128 and compares the resulting block index against three pairs: (9,1), (8,2) and (10,2). These correspond to a 128-cell macro grid, which is a coarser granularity than every other zone test in the component (all others use raw cell rectangles). The rule fires only when `extra.ClassMaster` is `CELESTIAL` (3), `CELESTIALCS` (4) or `SCELESTIAL` (5) - `Code/Basedef.h:178-182` - and `CurrentScore.Level >= 180`. Note that the check reads `pMob[conn].TargetX/TargetY`, i.e. the *current* position, not `m->TargetX/TargetY`. A character who succeeds in entering one of those blocks therefore triggers the recall on the *next* action packet rather than on the entering one, and a character already inside is recalled on any subsequent move attempt, including a stop frame.

`DoRecall` (`Server.cpp:7558-7602`) is not a plain teleport: it picks the city from `(MOB.Merchant & 0xC0) >> 6`, randomises a position within 15 cells of that city's spawn (`g_pGuildZone[CityID].CitySpawnX/Y`), overrides it with the guild spawn if the player's guild charges any zone, and overrides it again with the fixed newbie spawn (2112, 2042) for MORTAL characters below `FREEEXP`. It then calls `GetEmptyMobGrid` to find a free cell, builds a `MSG_Action` with `Effect = 1`, sends it to the caster and multicasts it. The handler returns immediately after, so the original move never reaches the grid commit.

The practical effect is a high-tier containment rule for three zones (most plausibly celestial-only maps or boss rooms). It is worth flagging that the block arithmetic is unbounded: `pMob[conn].TargetX / 128` on a negative coordinate truncates toward zero, so -1/128 == 0; a corrupted or uninitialised coordinate can never match (9,1)/(8,2)/(10,2) through the negative range, but the rule is nonetheless dependent on the coordinate domain that BR-15 validates only later in the function for the *target*, not for the current position.

**Rule workflow**:
```
block = (TargetX/128, TargetY/128)
if block in {(9,1),(8,2),(10,2)}:
    if ClassMaster in {3,4,5} and Level >= 180:
        DoRecall(conn)      # city spawn or guild spawn or newbie spawn, Effect = 1
        return
```

---

### Business Rule: Clock-window validation (BR-06)

**Overview**:
The client-supplied millisecond timestamp must not be more than 15 s in the future and not more than 120 s in the past relative to the server clock.

**Detailed description**:
`CurrentTime` is a millisecond counter maintained from `timeGetTime()` (`Server.cpp:3625`, `ProcessSecMinTimer.cpp:125/136/145/2139`) and therefore wraps every 2^32 ms (about 49.7 days). The handler derives a lower bound `checktime` at `:66-69`: while `CurrentTime <= 0x1D4C0` (120,000 ms) the bound is zero, so during the first two minutes after process start no past-timestamp rejection is possible; afterwards it is `CurrentTime - 120000`. The upper bound is `CurrentTime + 15000`, evaluated as `(unsigned int)movetime > CurrentTime + 15000` so that a negative `movetime` (any `ClientTick` with the high bit set) is rejected as an enormous unsigned value rather than accepted. Both comparisons are `int`-vs-`unsigned` mixes, which is why the explicit cast is required for correctness; the lower bound `movetime < checktime` has no cast and relies on signed comparison against a non-negative `checktime`.

The window is generous by design: 15 s of clock skew forward and 2 minutes of latency/backlog behind. It is not a cadence limiter (that is BR-09/BR-10, which are inert) and it does not relate the timestamp to the previous one; a client that sends 1,000 packets per second with `ClientTick = CurrentTime` passes every time. Its only real function is to reject obviously forged absolute timestamps and stale replays older than two minutes, which makes it a coarse anti-replay guard rather than a movement-rate guard. `SKIPCHECKTICK` (`0xE0A1ACA`) can never appear here because the dispatcher drops client frames carrying it (`ProcessClientMessage.cpp:63`); internal server-side injections that use it are also routed through the dispatcher with `isServer = TRUE` and are therefore not subject to the handler's own clock test either, since `_MSG_Action3`/`_MSG_Action` sent by the server carry the real `CurrentTime` from `GetAction` (`GetFunc.cpp:1242`).

The failure path writes a formatted line containing both timestamps and increments `NumError` by 1 with audit code 104 (`_MSG_Action3`) or 102 (`_MSG_Action`), and additionally writes a second, distinct log line for `_MSG_Action3`. A player that forges far-future timestamps is therefore visible in the log but is not rate-limited; only 2 milliard accumulated points terminate the session (`Server.cpp:694-704`), which at +1 per rejection is unreachable in practice.

**Rule workflow**:
```
movetime  = m->ClientTick
checktime = (CurrentTime <= 120000) ? 0 : CurrentTime - 120000
if (unsigned)movetime > CurrentTime + 15000  -> log + AddCrackError(1, 104|102) + return
if movetime < checktime                      -> log + AddCrackError(1, 104|102) + return
```

---

### Business Rule: Illusion move - class gate, mana cost, cadence (BR-07, BR-08, BR-09)

**Overview**:
`_MSG_Action3` is not a plain walk; it is the mage "Ilusao" movement skill (`//Skill Ilusao`, `:71`). It requires class 3 plus learned-skill bit `0x2`, costs the mana value of spell 73, and is intended to be limited to one use per 900 ms.

**Detailed description**:
The class gate at `:74-79` is written as three nested conditions: `if (Class != 3 || (LearnedSkill & 2) == 0)` then `if (Level < MAX_LEVEL)` then `if (AddCrackError(conn, 10, 28) == TRUE) return;`. Because `AddCrackError` returns `TRUE` only when it has just logged the character out, the effective semantics are: non-mages are charged 10 crack points, and if that pushes the account over the threshold the handler stops before touching movement state. If the account survives, the handler continues into the mana logic and commits the move - the skill is not actually refused. Characters at or above `MAX_LEVEL` (399, `Basedef.h:117`) skip even the crack event, which is the project-wide pattern of exempting high-tier characters from anti-cheat accounting. This is the only `AddCrackError` call in the file whose return value is tested, and it is the one place where the component respects the "session already torn down" contract of `CharLogOut`.

The mana rule at `:81-90` charges `g_pSpell[73].ManaSpent` exactly, with no `BASE_GetManaSpent` scaling by `SaveMana`/`Special` (contrast `_MSG_Attack`, which routes every skill through `BASE_GetManaSpent`, `Basedef.cpp:5415-5417`). Both `pMob[conn].MOB.CurrentScore.Mp` and `pUser[conn].ReqMp` are decremented by the same amount, keeping the client's predicted MP in step with the authoritative value; `ReqMp` is an `int` with no floor, so it can be driven negative by repeated attempts. Crucially the deduction happens *before* the cadence and clock tests at `:92-109`, so a rejected packet still burns mana: a client can drain its own pool by replaying `_MSG_Action3` as fast as it likes, and a legitimate player who is rejected by the 15 s window also pays. This ordering is the clearest sign that the function grew by insertion rather than design, and it makes the mana cost a self-inflicted denial rather than an anti-abuse cost.

The cadence rule at `:92-99` reads `pUser[conn].LastActionTick != 235543242 && movetime < pUser[conn].LastIllusionTick + 900`. `LastActionTick` is initialised to `SKIPCHECKTICK` (the same literal `235543242`) on every character login (`ProcessDBMessage.cpp:825`) and is written nowhere else in the project, so the first conjunct is always false and the 900 ms limit never runs. Even if it did, the comparison would be against `LastIllusionTick`, which is the field the handler stores at `:110`, so the two branches share one timestamp: an illusion move and a walk would suppress each other. When the check does (hypothetically) fire it logs `err,illusion %d %d 900ms limit` and records audit code 105.

**Rule workflow**:
```
if Type == _MSG_Action3:
    if (Class != 3 or LearnedSkill & 0x2 == 0) and Level < 399:
        if AddCrackError(10, 28) == TRUE: return        # logged out
    mana = g_pSpell[73].ManaSpent
    if CurrentScore.Mp < mana: SendSetHpMp(conn); return
    CurrentScore.Mp -= mana; ReqMp -= mana
    if LastActionTick != SKIPCHECKTICK and movetime < LastIllusionTick + 900:   # never true
        log; AddCrackError(1, 105); return
    if movetime outside window: log; AddCrackError(1, 104); return
    LastIllusionTick = movetime
```

---

### Business Rule: Walk cadence (BR-10)

**Overview**:
The walk branch intends to apply the same 900 ms cadence limit as the illusion branch; it is dead for the same reason and additionally stores its timestamp into the illusion field.

**Detailed description**:
`:113-134` duplicates the illusion block almost literally, including the log strings (`err,illusion ...`), which is direct evidence of copy-paste. The differences are the audit codes (103 for the 900 ms case, 102 for the window case) and the absence of any mana or class logic. `pUser[conn].LastActionTick` is *not* updated here - `:133` writes `LastIllusionTick = movetime` - so the field the guard reads is never advanced by walks, and the field walks do advance is read by both branches. The result is that the component records nothing about walk cadence at all; the only surviving effect of the branch is the 15 s / 120 s clock window of BR-06.

The consequence is significant for the component's stated architectural role ("move/position desync checks"): the desync check that would compare the client's reported tick with its previous tick is absent. The state needed for it exists (`LastAction`, `LastActionTick`, `LastMove`, `pMob.LastTime`, `pMob.LastSpeed` on `CMob`, `Code/TMSrv/CMob.h:46-49`) and is partly maintained, but no read path exists. A single-packet speed hack, a 33-cell jump per frame, or a client that simply never sleeps are all accepted as long as the absolute timestamp stays inside the 15 s / 120 s window and the declared `Speed` does not exceed the run nibble.

`LastActionTick` being pinned to the sentinel has a second consequence: `_MSG_Action3` and `_MSG_Action` can never be distinguished by cadence, so the mana-charging skill is spammable at packet rate until the pool empties, and the pool refills through the normal regeneration loop. This is the single highest-value rule to restore before any anti-cheat work on this component.

**Rule workflow**:
```
if Type == _MSG_Action:
    if LastActionTick != SKIPCHECKTICK and movetime < LastIllusionTick + 900:  # always false
        log; AddCrackError(1, 103); return
    if movetime outside window: log; AddCrackError(1, 102); return
    LastIllusionTick = movetime      # LastActionTick never updated
```

---

### Business Rule: Stop-frame suppression (BR-11)

**Overview**:
`_MSG_Action2` is the "stop" verb. Two consecutive stop frames are refused, and the frame is otherwise exempt from every clock rule.

**Detailed description**:
`:136-145` is the whole of the type-specific handling: if `pUser[conn].LastMove == _MSG_Action2` the frame is dropped with log text `err,can't send MSG_Stop continuously` and audit code 101. `LastMove` is updated only at `:240`, inside the branch that requires the target cell to differ from the current cell, so a stop frame whose `TargetX/TargetY` equal the current position never updates `LastMove` and can therefore be replayed indefinitely: the first stop leaves `LastMove` at whatever the last real walk set, the second stop sees `LastMove != _MSG_Action2` only if no walk intervened. In practice the suppression catches only a stop that follows a stop that itself moved the player.

The important structural property is that after the check the function does not return - it falls through into BR-12 to BR-26 with the same payload. A client can therefore use `_MSG_Action2` as a movement verb, and by doing so skips the 900 ms limiter, the 15 s clock window and the 120 s replay window entirely. The only remaining constraints are the declared-speed clamp, the 33-cell displacement gate, the coordinate domain, the zone rules and the grid commit. Since the type is also what other components use to stop a mob (`pMob[idx].Route[0] = 0` in `_MSG_Attack.cpp:902` resets route state), treating a stop as a displacement command is a semantic inconsistency that the handler does not police.

**Rule workflow**:
```
if Type == _MSG_Action2:
    if LastMove == _MSG_Action2: log; AddCrackError(1, 101); return
    # no clock checks
# fall through to the shared movement pipeline
```

---

### Business Rule: Declared-speed clamp (BR-12)

**Overview**:
The `Speed` field in the frame is compared with the low nibble of the character's current `AttackRun` score; values above it are clamped and logged.

**Detailed description**:
`pMob[conn].MOB.CurrentScore.AttackRun & 0xF` (`:147`) is the run component of the speed score, spanning 0..15. The comparison is `m->Speed > nibble`, so a negative or zero `Speed` passes untouched, and only an over-large value is clamped downward. On violation the handler records 5 crack points with audit code 4, logs `etc,diffrent movement. %d-%d`, and then **rewrites the field inside the received packet** (`:154`). Because the packet is later multicast verbatim, the rewrite is what the neighbours receive, so all observers see a plausible speed instead of the attacker's declared value - a rare case in this component of defensive normalisation that actually reaches the wire.

That protection is easily bypassed: nothing relates `Speed` to the actual displacement. The client controls both fields independently, and the handler never compares `Speed` with `BASE_GetDistance(LastX, LastY, TargetX, TargetY)` nor with the elapsed tick. Declaring `Speed = 1` while moving 33 cells therefore passes the clamp, is broadcast as a slow walk, and the client-side interpolation of the receiving players is what makes the movement look unnatural, not the server. The clamped value is also stored into `pMob[conn].LastSpeed` by `GridMulticast` (`SendFunc.cpp:816`) where it is never read again.

Note the asymmetry with the nine other speed consumers in the project: the NPC movement code computes a per-tick distance from `BASE_GetSpeed(&MOB.CurrentScore) * SECBATTLE / 4` (`CMob.cpp:867`, `:932`, `:1171`) and clamps it to `MAX_ROUTE - 1`. The player path has the same primitives available and does not use them. This is the gap between "speed checks" as the component's declared role and the code that exists.

**Rule workflow**:
```
if m->Speed > (CurrentScore.AttackRun & 0xF):
    AddCrackError(5, 4); log("etc,diffrent movement. declared-run")
    m->Speed = CurrentScore.AttackRun & 0xF
# no displacement <-> speed <-> time cross-check anywhere
```

---

### Business Rule: Viewport / displacement gate (BR-13)

**Overview**:
The destination must lie inside the mover's 33-cell half-window; beyond twice that window the server sends a corrective snap-back and rejects the frame.

**Detailed description**:
`VIEWGRIDX`/`VIEWGRIDY` are 33 and `HALFGRIDX`/`HALFGRIDY` are 16 (`Basedef.h:95-98`), so the "view" is a 33x33 block but the test at `:160` uses the full constant as a half-extent: the accepted target range is `posX +/- 33` and `posY +/- 33`. That is a 67x67 acceptance box, roughly twice the rendered neighbourhood, and it is the only displacement ceiling in the component. Because the test is per-packet and there is no cadence limiter (BR-09/BR-10 inert), the achievable speed is "33 cells per action frame, times frames per second", i.e. effectively unbounded for a client that pipelines frames.

The nested condition at `:162` distinguishes hard violations: if the target is also beyond `2 * VIEWGRID` in either axis, the server builds a `MSG_Action` at the current position via `GetAction(conn, posx, posy, ...)`, forces `Type = _MSG_Action3` and `Speed = 6`, and pushes it through `SendOneMessage` (`:167-171`). This is the "you are desynced, snap back" correction, but it is emitted with `sizeof(MSG_Action)` at `:171` - the only correctly sized send in this block - and with `Type = _MSG_Action3`, the illusion skill verb, which conflates a correction with a mana-charging skill in the client's state machine. `Speed = 6` is also hardcoded and may exceed the character's own run nibble that BR-12 just enforced, and `Effect` is left at the `GetAction` default of 1 (teleport), so the client is instructed to play a teleport rather than a walk.

Every viewport violation, hard or soft, records 1 crack point with audit code 5 and returns. A soft violation (target between 34 and 66 cells) is therefore rejected with no correction packet at all - the client is never told why, and since the server did not move, the client's local prediction stays wrong until the next authoritative broadcast of its own position. This is the exact "position desync" the component exists to police, and the police action for the common case is silence.

The commented-out prefix on the condition, `/*m->Effect != 1 && m->Effect != 2 &&*/` (`:160`), shows that an `Effect` gate was once part of this expression and was removed; BR-30 documents the consequence.

**Rule workflow**:
```
if TargetX < posX - 33 or TargetX > posX + 33 or TargetY < posY - 33 or TargetY > posY + 33:
    if TargetX beyond posX +/- 66 or TargetY beyond posY +/- 66:
        GetAction(conn, posX, posY) -> Type=_MSG_Action3, Speed=6 -> SendOneMessage
    AddCrackError(1, 5)
    return
```

---

### Business Rule: Arena slot-2 leader lock (BR-14)

**Overview**:
The 21x22 rectangle 3378..3398 x 1427..1448 is reserved for the three registered party leaders of arena slot 2; anybody else who targets a cell inside it is teleported to one of slot 2's three spawn pads.

**Detailed description**:
`Pista[7]` (`Server.cpp:448`) is the seven-slot arena registry, each slot holding three parties with a `short LeaderID` and a name (`STRUCT_PISTA`, `Basedef.h:646-656`). `PistaPos[7][3][2]` holds three spawn coordinates per slot (`Server.cpp:154-...`); slot 2's pads are (3410,1453), (3419,1426) and (3358,1436), which sit immediately around the guarded rectangle - consistent with the rectangle being slot 2's entrance. The rule first resolves the mover's party leader (`pMob[conn].Leader`, or the mover himself when the field is <= 0), then applies `if (partyleader != conn || conn != Pista[2].Party[0].LeaderID && conn != Pista[2].Party[1].LeaderID && conn != Pista[2].Party[2].LeaderID)`. Precedence makes this `A || (B && C && D)`: recall when the mover is not his own party leader, **or** when he is not one of the three registered leaders.

Two consequences follow. First, a registered leader standing at the rectangle is recalled if he is currently a party member rather than the leader - the `partyleader != conn` conjunct alone is enough. Second, the rule reads only the three `LeaderID` fields and never the slot's `Sala` or party membership, so a stale or spoofed registration is enough to authorise entry. Registration is written by the quest component (`Pista[s].Party[t].LeaderID/LeaderName`, `RemoveParty` at `Server.cpp:7414-7424`, and the arena quest branches), which means the trust boundary of this rule lives in a different component.

The failure action is `DoTeleport(conn, PistaPos[2][rd][0], PistaPos[2][rd][1])` with `rd = rand() % 3` (`:186-187`), a forced relocation inside the same arena rather than a recall out of it. `DoTeleport` (`Server.cpp:8212-8230`) calls `GetEmptyMobGrid`, and if that fails it silently does nothing while the handler returns anyway - leaving the intruder standing in the guarded rectangle with no log line and no crack event. That makes this rule both the softest of the zone rules and the only one with a random outcome.

**Rule workflow**:
```
if 3378 <= TargetX <= 3398 and 1427 <= TargetY <= 1448:
    leader = (pMob[conn].Leader > 0) ? pMob[conn].Leader : conn
    if leader != conn or conn not in {Pista[2].Party[0..2].LeaderID}:
        DoTeleport(conn, PistaPos[2][rand()%3][0], PistaPos[2][rand()%3][1])
        return
```

---

### Business Rule: Coordinate domain (BR-15)

**Overview**:
Target coordinates must be strictly inside the map: `1..4095` on both axes.

**Detailed description**:
`:192` uses `<= 0 || >= 4096` on both axes, which is exactly right for `pMobGrid[MAX_GRIDY][MAX_GRIDX]` declared as `[4096][4096]` (`Server.h:54-56`), and it is the *only* place the component protects a grid index. The rejection writes `err,action - viewgrid` with the mob name and IP (`:194`) and returns with no crack points, so an out-of-range attempt is logged but not penalised. Because the test sits at `:192`, after the viewport gate and before every zone rule, every subsequent `GetAttribute`, `BASE_GetVillage`, `BASE_GetGuild` and `pMobGrid` access is bounded - but only for `m->TargetX/m->TargetY`.

The *other* client-supplied coordinates in the same frame are not covered. `m->PosX/m->PosY` are used as the route origin at `:262` and are never range-checked; `BASE_GetRoute` internally refuses any origin outside `[1, 4094]` (`Basedef.cpp:5549-5554`) and breaks out without writing the endpoint, so the exposure is contained by the callee rather than by this handler. Symmetrically, `pMob[conn].TargetX/TargetY` (read at `:52`, `:157`, `:211`) are trusted as already-validated state. That invariant depends on every writer: `ProcessDBMessage.cpp:806-807` seeds them from the character record, `GridMulticast` writes whatever `tx/ty` its caller passed, and 53 call sites pass coordinates from quests, NPC AI and the item/ground systems. The handler therefore inherits a safety property that it does not enforce.

A subtle asymmetry: the check is performed only when `m->TargetX != pMob[conn].TargetX || m->TargetY != pMob[conn].TargetY` for the *zone* rules at `:211`, but the bounds test at `:192` runs unconditionally. A frame that targets the current cell is therefore bounds-checked, grid-tested at `:246` (where the cell trivially contains `conn`), and then fails the `:211` guard so no commit occurs - a pure no-op frame that still costs full validation.

**Rule workflow**:
```
if TargetX <= 0 or TargetX >= 4096 or TargetY <= 0 or TargetY >= 4096:
    log("err,action - viewgrid", MobName, IP)
    return
```

---

### Business Rule: Trade-only account confinement (BR-16)

**Overview**:
An account flagged `OnlyTrade` - set when another client on the same machine MAC logs in a different account (`ProcessDBMessage.cpp:157-167`) - may only stand inside a village or on a cell carrying attribute `0x80`; every other destination triggers a recall.

**Detailed description**:
The flag is written by the multi-client detector in the DB message path (`ProcessDBMessage.cpp:162` clears it for the primary account, `:166` sets it for every other account sharing the MAC, `:395` sets it on a second path) and is consumed by the whisper, party, attack, use-item and movement handlers. In this handler the rule is: resolve `BASE_GetVillage(TargetX, TargetY)` (returns 0..4 for the five configured city rectangles, 5 when outside any, `Basedef.cpp:4258-4267`) and the `0x80` bit of the target cell. If the target is neither inside a village nor on an `0x80` cell, send `_NN_OnlyVillage` and `DoRecall`.

The `0x80` clause is the interesting half. Bit `0x80` is the same bit the newbie-zone rule (BR-17) uses to *exclude* normal characters, so trade-only accounts are permitted exactly the cells that ordinary characters are forbidden: the intent is evidently to confine multi-client accounts to the newbie/trade areas. Because both rules read the same bit with opposite polarity, any change to that bit's meaning breaks one of the two silently. The village test is index-based (`< 0 || >= 5`) although `BASE_GetVillage` can only return 0..5, so the effective test is `Village == 5 && (attribute & 0x80) == 0`.

Note the ordering: BR-16 runs at `:198`, before the newbie-zone rule at `:215`, and both can fire for the same frame. For a trade-only account in a village, BR-17 is then evaluated on the same target cell; a village cell that also carries `0x80` will recall the account it was just allowed to enter. The two policies were written independently and their interaction is not reconciled in code.

**Rule workflow**:
```
if OnlyTrade:
    village = BASE_GetVillage(TargetX, TargetY)
    if (village < 0 or village >= 5) and (GetAttribute(TargetX,TargetY) & 0x80) == 0:
        SendClientMessage(_NN_OnlyVillage); DoRecall(conn); return
```

---

### Business Rule: Newbie-zone exclusion (BR-17)

**Overview**:
Cells with attribute `0x80` are closed to characters from level 35 to 999 and to every class master other than MORTAL.

**Detailed description**:
`:213-221` reads the full attribute byte and tests `mapAttribute & 0x80 && (level >= FREEEXP && level <= 999 || extra.ClassMaster != MORTAL)`. `FREEEXP` is a process global initialised to 35 (`Server.cpp:43`) and configurable, so the threshold is a tunable rather than a literal; the upper bound 999 is a literal that sits between `MAX_LEVEL` (399) and the celestial level space. The parenthesisation is deliberate: the exclusion applies when either the character is inside the level band *or* its class master is not MORTAL (2), so ARCH, CELESTIAL, CELESTIALCS and SCELESTIAL are excluded at any level, including level 0. The class-master test alone makes the level band redundant for every tier except MORTAL, whose exclusion is purely `35..999`.

The failure path is a message plus recall (`_NN_Newbie_zone`, `:217-219`), not a silent rejection, so the client learns why. Because the check runs only inside the `m->TargetX != pMob[conn].TargetX || ...` guard at `:211`, a character already standing on an `0x80` cell can send stop frames indefinitely without being recalled; the rule polices entry, not presence. Combined with BR-16's polarity, an `0x80` cell admits trade-only accounts and excludes everyone else, which is consistent only under the reading "these cells are the newbie/trade safe areas".

One more asymmetry: the level comparison uses `CurrentScore.Level`, which for celestial tiers runs above `MAX_LEVEL`, while `extra.ClassMaster` is what actually distinguishes tiers. The `<= 999` bound therefore exists to keep the rule from silently excluding very high celestial levels through the level band - the class test already covers them, making the bound dead weight in every branch except an impossible MORTAL level above 999.

**Rule workflow**:
```
if target cell != current cell:
    if GetAttribute(TargetX,TargetY) & 0x80
       and ((Level >= FREEEXP and Level <= 999) or ClassMaster != MORTAL):
        SendClientMessage(_NN_Newbie_zone); DoRecall(conn); return
```

---

### Business Rule: Guild-zone ownership (BR-18)

**Overview**:
Cells carrying attribute `0x20` belong to one of the five guild zones; a character whose guild is not that zone's charging guild is recalled, but only while his level is at or below `MAX_LEVEL`.

**Detailed description**:
`:223-236` masks `0x20`, applies the level guard `CurrentScore.Level <= MAX_LEVEL` (399), resolves the zone with `BASE_GetGuild(TargetX, TargetY)` (`Basedef.cpp:4164-4173`, which scans the same five `g_pGuildZone` rectangles and returns 5 when none matches), and then compares `pMob[conn].MOB.Guild` against `g_pGuildZone[Zone].ChargeGuild`. The second half of the guard, `Zone >= 0 && Zone < MAX_GUILDZONE`, is what stops a cell with `0x20` but no matching rectangle from indexing `g_pGuildZone[5]`; without it the function would read one entry past the array. That defensive check is necessary precisely because attribute and rectangle data are independent inputs.

The level guard inverts the intent of the rule in the same way BR-17's does: characters above `MAX_LEVEL` (i.e. ARCH and celestial tiers, whose `CurrentScore.Level` exceeds 399) are exempt from guild-zone ownership entirely and may walk into any charging guild's territory. This is consistent with the project-wide pattern of exempting high tiers from zone restrictions (`:52` requires level >= 180 for celestials, `:76` skips the crack event at `MAX_LEVEL`, `_MSG_Attack` does the same) but it is worth stating explicitly as a business decision embedded in a comparison rather than in a table.

The response is `_NN_Only_Guild_Members` plus `DoRecall`, which relocates the player to his city or guild spawn. Note that the charging guild is a mutable global updated by `RegenMob` on each second-tick when a guild leader stands in the castle throne room (`Server.cpp:4367-4411`), so the same cell may admit a guild at one moment and reject it moments later. The handler reads it without any snapshot or lock, which is safe only because TMSrv is single-threaded.

**Rule workflow**:
```
if GetAttribute(TargetX,TargetY) & 0x20:
    if Level <= MAX_LEVEL:
        zone = BASE_GetGuild(TargetX, TargetY)
        if zone >= 0 and zone < 5 and MOB.Guild != g_pGuildZone[zone].ChargeGuild:
            SendClientMessage(_NN_Only_Guild_Members); DoRecall(conn); return
```

---

### Business Rule: Castle-quest area lock (BR-19)

**Overview**:
Movement into the nine castle-quest rectangles (`CastleQuestPosition`, `CCastleZakum.cpp:46-63`) is refused, by teleporting the player back to his own current cell, unless he belongs to the castle quest party.

**Detailed description**:
The handler delegates at `:238` to `CCastleZakum::CheckMove(conn, m->TargetX, m->TargetY)` (`CCastleZakum.cpp:377-394`). That function scans `CastleQuestParty[0..MAX_PARTY]` (13 entries) for `conn`; if not found, `i` remains `MAX_PARTY + 1` (13) and the second loop tests each rectangle for containment. On a hit it calls `DoTeleport(conn, pMob[conn].TargetX, pMob[conn].TargetY)` - a teleport to the player's *current* cell, which is the project's idiom for "cancel the client's predicted movement" - and returns.

Two defects are visible in the callee and directly affect this component. First, the first rectangle, `{2221, 1270, 2259, 1246}` (`CCastleZakum.cpp:49`), has an inverted Y range: the containment test at `:388` requires `targety >= 1270 && targety <= 1246`, which is unsatisfiable, so that block of the castle quest is not protected at all. The other eight entries are written as `{x1, y1, x2, y2}` with `y1 < y2` and behave as intended. Second, the party search compares `CastleQuestParty[i] == conn` for `i` in `0..12` but never validates `conn` against `MAX_USER`; the handler has already passed the dispatcher's ID check, so `conn` is in `0..999` and the comparison is safe, but the list holds 13 entries for `MAX_PARTY == 12`, so index 12 is an intentional overflow slot whose semantics are not documented.

The call is placed before the bookkeeping at `:240`, so a refused move leaves `LastMove` and `Route` untouched, but it is placed *after* the eight other zone rules, so a castle-quest area that also carries `0x20` will be handled by BR-18's recall first, producing a different destination (city spawn vs. own cell) depending on map data. The handler ignores whether `CheckMove` teleported the player and continues into the commit block regardless - `DoTeleport` has already moved him via `GridMulticast`, and then `:283` runs a second `GridMulticast` for the original target. The second call wins: the player ends up at the target the castle rule just rejected.

**Rule workflow**:
```
CCastleZakum::CheckMove(conn, TargetX, TargetY)
  -> not in CastleQuestParty[0..12]
     and target inside any CastleQuestPosition rectangle
        -> DoTeleport(conn, currentX, currentY)
  -> returns void; caller does not check, does not return
```

---

### Business Rule: Grid collision relocation (BR-22)

**Overview**:
If the destination cell is occupied by another mob, the handler searches for a nearby free cell, re-derives the route from the client-reported origin, downgrades the effect to walking, and sends the corrected frame back to the client.

**Detailed description**:
`:246-279` runs only when `pMobGrid[TargetY][TargetX]` is non-zero and not `conn`. It snapshots the requested destination, measures the route with `strlen(m->Route)`, and calls `GetEmptyMobGrid(conn, &destx, &desty)` (`GetFunc.cpp:1901-2001`), which accepts the cell if it is empty and its height is not 127 (impassable), otherwise spirals outward through radius 1, 2, 3 and 4 looking for a cell that is unoccupied and whose *original* target's height is not 127 - note the height test at `:1929`, `:1949`, `:1969` and `:1989` re-reads `pHeightGrid[*ty][*tx]` after `*ty/*tx` may have been reassigned by earlier iterations, so the test is on the candidate rather than the original cell in most paths. For a player whose own cell is occupied, the call at `:255` passes `conn` as the mob id, so the "cell belongs to me" shortcut at `:1910` applies.

The loop at `:257-263` is the component's least coherent code. It iterates `i` from `len` down to 0 and, on each iteration, first tests whether `destx/desty` is free; if it is free it executes `continue`, which does nothing but decrement `i`. Only when the cell is *not* free does it call `BASE_GetRoute(m->PosX, m->PosY, &destx, &desty, m->Route, i, pHeightGrid)`, whose return value is ignored and which overwrites `destx/desty` with the endpoint reached after `i` steps from the client-reported origin. The net effect is that, for a free cell, the loop spins without work; for an occupied one, the destination is recomputed from an arbitrary client-supplied origin and a truncated route length. When `i` reaches -1 the branch at `:265` sets `TargetX/TargetY = destx/desty`, forces `Effect = 0` and sends a self-correction with `sizeof(m)` (the defect of section 10, D-01); otherwise the same assignment happens at `:275-276` and the frame is queued with `sizeof(m)`.

Because `m->PosX/m->PosY` come straight from the client and are never range-checked, this is the only place where a client can choose the origin of a server-computed route. `BASE_GetRoute` bounds its work to the height grid (`Basedef.cpp:5549`), so the result cannot leave `1..4094`, but the route is a straight-line walk computed from a lie: a client that reports `PosX/PosY` elsewhere in the map can obtain a destination unrelated to its actual position, subject only to the earlier 33-cell viewport test on `m->TargetX/TargetY` and the fact that `destx/desty` must remain free.

**Rule workflow**:
```
if pMobGrid[TargetY][TargetX] not in {0, conn}:
    destx,desty = TargetX,TargetY
    len = strlen(m->Route)                      # unbounded read
    GetEmptyMobGrid(conn, &destx, &desty)
    for i = len down to 0:
        if cell free: continue
        BASE_GetRoute(m->PosX, m->PosY, &destx, &desty, m->Route, i, pHeightGrid)
    if i == -1: TargetX/Y = destx/desty; Effect = 0; SendOneMessage(m, sizeof(m)); return
    TargetX/Y = destx/desty; AddMessage(m, sizeof(m))
```

---

### Business Rule: Grid commit and area-of-interest update (BR-23)

**Overview**:
`GridMulticast(conn, tx, ty, msg)` is the authoritative commit: it moves the mob's entry in `pMobGrid`, diffs the old and new 33x33 windows, creates and removes mobs and items on both sides, relays the frame to the new neighbours, and finally writes `TargetX/TargetY` plus the movement bookkeeping from the frame.

**Detailed description**:
`SendFunc.cpp:620-823` is a 53-caller function that this handler invokes once per accepted move (`:283`). It first heals a corrupted grid by testing whether `pMobGrid[TargetY][TargetX]` still points at `conn` and, if not, trying `GetEmptyMobGrid` to re-seat the mob; it then clears that cell (`:643`). It next checks the destination cell, and if another mob holds it, calls `GetEmptyMobGrid` on `pMob[conn].TargetX/TargetY` - note this relocates the *mob*, not the `tx/ty` the caller passed - logs `PC step in other mob's grid`, and then, crucially, assigns `pMobGrid[ty][tx] = conn` anyway (`:657`). The occupant's grid entry is thus overwritten while its `TargetX/TargetY` still point at that cell, producing exactly the grid/state divergence the function is trying to repair; the occupant is only reconciled later, when something else calls `GetEmptyMobGrid` for it.

The window diff is then computed twice: the "source" window from `pMob[conn].TargetX/TargetY` (the pre-commit position) and the "destination" window from `tx/ty`. For every mob in the source window that is not `conn`, the frame is queued (`:714`, using the frame's own `Size`), and if the cell lies outside the destination window both directions receive `SendRemoveMob` (`:716-723`). Items in the source-but-not-destination window are removed from the mover's view (`:728-743`). The mirror pass over the destination window creates mobs both ways (`SendCreateMob(conn, tmob)` and `SendCreateMob(tmob, conn)`), sends PK info, and queues the original frame to each new neighbour (`:745-811`). Finally `:813-822` commits: `LastTime = sm->ClientTick`, `LastSpeed = sm->Speed`, `LastX/LastY = sm->PosX/PosY`, `TargetX = tx`, `TargetY = ty`.

That final block is a standing contract violation for the other 52 callers: every one of them passes a non-`MSG_Action` structure (`MSG_CreateMob`, `MSG_UpdateScore`, `MSG_Motion`, `MSG_RemoveMob`, ...), and `GridMulticast` unconditionally reinterprets it as `MSG_Action`. Fields at offsets 12-15 (`PosX/PosY`) and 20-23 (`Speed`) of an unrelated message are written into `LastX/LastY/LastSpeed`. In this handler's own call that is harmless - the message really is a `MSG_Action` - but the function is shared, and the fields it corrupts are (currently) never read, so the bug is latent rather than active. The window-diff predicate also carries a precedence defect: `if (x < tx1 || x >= tx2 || y < ty1 || y >= ty2 && tx)` (`:716`, repeated at `:737`) parses as `... || (y >= ty2 && tx)`, disabling the Y test whenever `tx == 0`.

For this component the practical semantics are: the player becomes visible to, and visible from, a 33x33 window centred on the new cell; every player in the old-only ring receives a removal for him; every player in the new-only ring receives a creation for him and for the items there; and the movement frame reaches everyone in the destination window, including its attacker-controlled `ID` and `Size`.

**Rule workflow**:
```
GridMulticast(conn, tx, ty, pMsg):
  heal/clear old cell  -> pMobGrid[TargetY][TargetX] = 0
  if dest occupied by other -> GetEmptyMobGrid(this mob); log
  pMobGrid[ty][tx] = conn
  for cells in source window:
      relay msg; if outside dest window -> SendRemoveMob(both), SendRemoveItem(conn)
  for cells in dest window not in source window:
      SendCreateMob(both), SendPKInfo; relay msg
  commit LastTime/LastSpeed/LastX/LastY from msg; TargetX = tx; TargetY = ty
```

---

### Business Rule: Route-digit to merchant-nibble encoding (BR-24)

**Overview**:
After a successful commit the handler derives a digit from the byte that follows the last route character and stores `16 * digit` in bits 4-7 of the mob's current and base merchant score.

**Detailed description**:
`:292-307` computes `len = strlen(pMob[conn].Route)`, and when `0 < len < MAX_ROUTE - 3` (i.e. `len < 21`) reads `pMob[conn].Route[len]`, adds 3, subtracts 48 (the ASCII '0'), and accepts a result in `1..9`, multiplying it by 16 and OR-ing it into `CurrentScore.Merchant` with `& 0xF0`. The literal arithmetic `Route[len] + 3 - 48` is an obfuscated way of writing `Route[len] - '0' + 3`, so the accepted digits are '1'..'9' after a +3 bias, i.e. ASCII '4'..'<' - the route alphabet produced by `BASE_GetRoute` is '1'..'9' (`Basedef.cpp:5578-5808`), so the offset arithmetic is only meaningful if the client appends a three-character trailer after the route. The result is then copied from `CurrentScore.Merchant` into `BaseScore.Merchant` (`:307`), and `BaseScore.Merchant` is what `BASE_GetCurrentScore` folds into `CurrentScore.Merchant` on the next recomputation (`Basedef.cpp:4017`).

The field itself is heavily overloaded. `MSG_Action`'s own header comment defines `Effect` 0/1 as walk/teleport (`Basedef.h:1702`), but `MOB.Merchant` is simultaneously: the NPC merchant class (values 1, 2, 4, 19, 43, 100 used across `_MSG_Buy`, `_MSG_Sell`, `_MSG_REQShopList`, `_MSG_Attack`), the player's selected city in bits 6-7 (`(Merchant & 0xC0) >> 6` in `DoRecall`, `Server.cpp:7562`; written by `_MSG_ChangeCity.cpp:30-31`), and the "merchant flag" bit 0. The handler writes bits 4-7 from a route digit, overlapping the city bits. `DoRecall` reads `MOB.Merchant` rather than `CurrentScore.Merchant`, so the recall destination is not directly corrupted by this write - but `BASE_GetCurrentScore`'s `(CurrentScore.Merchant & 240) | MOB.Merchant` fold means the two views are one byte apart, and any future reader that consults the score instead of the mob field will see a city index derived from the last movement direction.

No client-visible consumer of `CurrentScore.Merchant` exists today besides the score projection (`MSG_UpdateScore.Score` is a `memcpy` of `CurrentScore`, `SendFunc.cpp:1152`), so the rule currently leaks into the score packet rather than into gameplay. Its purpose is undocumented; the variable name `Unk` (`:293`) records that the decompiler did not know it either. Confidence that this is a "last direction memory" for client-side replay: medium; confidence that it is a rule (and not an accident) is high, because the `1..9` validation and the factor 16 are deliberate.

**Rule workflow**:
```
len = strlen(pMob.Route)
if 0 < len < 21:
    Unk = pMob.Route[len] + 3 - 48
    if Unk < 1 or Unk > 9: Unk = 0
    else: Unk = 16 * Unk
CurrentScore.Merchant = (CurrentScore.Merchant | Unk) & 0xF0-masked
BaseScore.Merchant    = CurrentScore.Merchant
```

---

### Business Rule: Battle Royale and Colo-150 event exclusions (BR-25, BR-26)

**Overview**:
The rectangle 2604..2648 x 1708..1744 (a 45x37 area) is closed to players above level 100 or 200 during Battle Royale, and above level 150 whenever `Colo150Limit` is set.

**Detailed description**:
`:309-324` activates on `BrState && BRItem > 0` - both process globals defaulting to 0 (`Server.cpp:317`, `:389`) and presumably set by the event scheduler and by the event reward item. Inside the rectangle, the rule recalls a player when `!BrGrid && level >= 100 && level < 1000`, or when `BrGrid == 1 && level >= 200 && level < 1000`. `BrGrid` (default 0, `Server.cpp:391`) therefore selects between two brackets: an "open" grid that admits only characters below level 100, and a "restricted" grid that admits only characters below level 200. The `< 1000` upper bound keeps celestial-level characters (who exceed 999) from being recalled by the level test, mirroring BR-17's `<= 999`.

`:326-338` is independent and simpler: if `Colo150Limit` (default 0, `Server.cpp:393`) is set and the level is at least 150 and the target is inside the same rectangle, the player is recalled. The combination means the rectangle has three different admission thresholds depending on two globals and one level band, all hardcoded as literals, with no configuration table and no log line. Unlike every other recall in this component, neither of these two writes a message to the client, so the player is teleported without explanation.

Both rules run *after* the commit at `:283`. `GridMulticast` has already moved the player and broadcast the move; `DoRecall` then moves him again and broadcasts a second `MSG_Action` with `Effect = 1`. Every observer in both windows therefore sees a walk followed immediately by a teleport, and the player's own client receives both. This ordering is the least defensible part of the component's rule placement: the exclusion should precede the commit, as BR-14 to BR-18 do. It also means the early `return` at `:188`/`:206`/`:219`/`:233` (before commit) and the late recall at `:319`/`:335` (after commit) have materially different wire consequences for what is nominally the same policy.

**Rule workflow**:
```
after commit:
  if BrState and BRItem > 0 and TargetX in 2604..2648 and TargetY in 1708..1744:
      if (BrGrid == 0 and 100 <= Level < 1000) or (BrGrid == 1 and 200 <= Level < 1000):
          DoRecall(conn)                       # no message
  if Colo150Limit and Level >= 150 and same rectangle:
      DoRecall(conn)                           # no message
```

---

### Business Rule: Crack accounting and audit (BR-27, BR-28)

**Overview**:
Every protocol violation adds a weight to `pUser[].NumError` and writes a log line; a total of 2,000,000,000 terminates the session.

**Detailed description**:
`AddCrackError(conn, val, Type)` (`Server.cpp:684-707`) adds `val` to `NumError`, writes `cra point: %d type: %d` unless `Type` is 3, 8 or 15, and - when the total reaches 2,000,000,000 - sends `_NN_Bad_Network_Packets`, calls `CharLogOut`, logs `cra char logout type: %d` and returns `TRUE`. The handler uses nine distinct call sites with weights 1, 5 and 10 (`:36`, `:77`, `:97`, `:107`, `:120`, `:130`, `:141`, `:149`, `:173`) and audit codes 3, 4, 5, 28, 101, 102, 103, 104 and 105. Only `:77` tests the return value; the other eight continue after a potential logout, which for this handler means a position commit can happen for a character that has just been removed from play (`:149` is the clearest case: the clamp at `:154` and the commit at `:283` both run after a `AddCrackError(5, 4)` that could have logged the character out).

The threshold makes the accounting cosmetic. At +1 for a forged timestamp (`:107`, `:130`) or an out-of-window move, and +5 for a declared-speed excess, a client would need on the order of 10^8 to 10^9 rejected packets to be disconnected. The value `2000000000` looks like a 32-bit overflow guard rather than a policy, and `NumError` is reset only on login (`ProcessDBMessage.cpp:822`). The audit is therefore useful for forensics - each line carries account name and IP - and not as an enforcement mechanism. In this component the only behavioural consequence of a crack event is the `_MSG_Action3` class gate at `:77-78`, which aborts before touching movement state.

**Rule workflow**:
```
AddCrackError(conn, val, Type):
    if Type not in {3,8,15}: log("cra point: val type: Type")
    NumError += val
    if NumError >= 2000000000: message; CharLogOut(conn); log; return TRUE
    return FALSE
```

---

### Business Rule: Illusion echo and status resend (BR-29)

**Overview**:
A successful `_MSG_Action3` is echoed to the caster with a full HP/MP resend, after the grid commit.

**Detailed description**:
`:285-289` re-sends the frame to the caster with the correct length (`sizeof(MSG_Action)` at `:287`) and then calls `SendSetHpMp` (`SendFunc.cpp:1590-1620`), which recomputes `ReqHp/ReqMp` from the authoritative scores and queues `_MSG_SetHpMp`. This is the only path in the component that confirms the mana deduction to the client; a `_MSG_Action3` rejected before the commit (`:85-87`, `:92-109`) sends either `SendSetHpMp` alone (insufficient MP) or nothing at all (rate/window rejection), so the client's predicted MP can drift out of step with the server until the next unrelated resend. Since the mana was already deducted at `:89-90` before those rejections, the drift is real and one-directional.

The echo is also redundant: `GridMulticast` has already relayed the frame to every player in the destination window including `conn` (the mover is in his own window, and the skip in `SendFunc.cpp:710` only skips `conn` in the *source*-window pass - the destination pass at `:745-811` skips cells outside the source window and includes the mover's own cell only if it lies outside the source window, which it does not). In practice the echo is what guarantees the caster sees his own skill move, so the redundancy is in the wrong place rather than harmful: `GridMulticast`'s destination pass excludes the mover's new cell when that cell was already inside his old window, which is exactly the case for short moves.

**Rule workflow**:
```
if Type == _MSG_Action3 (after commit):
    SendOneMessage(m, sizeof(MSG_Action))
    SendSetHpMp(conn)
```

---

### Business Rule: Unvalidated Effect field (BR-30)

**Overview**:
`m->Effect` distinguishes walking from teleporting on the wire, but this handler does not enforce or use it.

**Detailed description**:
`MSG_Action.Effect` is documented as `0 = walking, 1 = teleporting` (`Basedef.h:1702`). The condition at `:160` retains the removal of the only test that ever consulted it: `/*m->Effect != 1 && m->Effect != 2 &&*/`. The handler writes `Effect` only in one direction - to 0 in the collision branch (`:270`) - and otherwise forwards whatever the client sent. `DoRecall` and `DoTeleport` set it to 1 with the comment `// teleport effect` (`Server.cpp:7596`, `:8223`), so the field is authoritative only when the server originates the frame.

The practical consequence is that a client may label any 33-cell displacement as a teleport, and the server has no rule that says "a teleport frame is only legitimate when the server generated it". Receiving clients use the field to choose between interpolated walk and snap, so an attacker controls the visual presentation of his own movement in every other player's client. The commented-out guard also shows the intent: `Effect` was once restricted to 0/1/2, i.e. an enum, and the removal of the check left the field fully client-controlled.

**Rule workflow**:
```
Effect is read only for logging/relay; never validated
server-originated frames set Effect = 1 (GetAction default) or 0 (collision path)
```

---

## 4. Component Structure

```
Code/TMSrv/
├── _MSG_Action.cpp                 # THE COMPONENT - 340 lines, one function Exec_MSG_Action (21-340)
│                                   #   phases: state gate 25-60 | type audit 63-145 | speed/clamp 147-155
│                                   #           zone ladder 157-238 | commit 240-338
├── ProcessClientMessage.h          # declaration of Exec_MSG_Action (line 84); extern CurrentTime, BrState
├── ProcessClientMessage.cpp        # dispatcher: 96-100 routes _MSG_Action/_MSG_Action2/_MSG_Action3
├── SendFunc.cpp                    # GridMulticast(conn,tx,ty,msg) 620-823  <- the real position commit
│                                   # SendHpMode 1622, SendSetHpMp 1590, SendOneMessage/AddMessage used via CPSock
├── Server.cpp                      # AddCrackError 684, RemoveTrade 7319, DoRecall 7558, DoTeleport 8212,
│                                   # PistaPos 154, Pista 448, BrState 389, BRItem 317, BrGrid 391,
│                                   # Colo150Limit 393, FREEEXP 43, CurrentTime 281/3625
├── ProcessDBMessage.cpp            # login: LastActionTick/LastIllusionTick = SKIPCHECKTICK (823-828)
│                                   # OnlyTrade set/cleared from MAC collision (157-167), CHAR login position seed (805-807)
├── GetFunc.cpp                     # GetAttribute 1257, GetEmptyMobGrid 1901, GetAction 1239, GetInView 640
├── CCastleZakum.cpp / .h           # CheckMove 377-394, CastleQuestPosition 46-63
├── CMob.h / CMob.cpp               # CMob: Route[24], LastX/LastY/LastTime/LastSpeed, TargetX/TargetY (46-55)
├── CUser.h                         # CUser: LastMove 71, LastActionTick 73, LastIllusionTick 74,
│                                   #        NumError 75, ReqMp 91, TradeMode 46, Trade 59, OnlyTrade 115, cSock 47
Code/
├── Basedef.h                       # MSG_Action 1696-1708, _MSG macro 925-930, _MSG_Action* 1693-1695,
│                                   # VIEWGRIDX/Y 95-96, MAX_ROUTE 124, MAX_LEVEL 117, MAX_GRIDX/Y 100-101,
│                                   # SKIPCHECKTICK 172, MORTAL/ARCH/CELESTIAL* 178-182, STRUCT_PISTA 646-656
├── Basedef.cpp                     # BASE_GetRoute 5538-5826, BASE_GetVillage 4258, BASE_GetGuild 4164,
│                                   # g_pSpell 744, g_Height* 759-761, attribute->height 2583-2606
└── CPSock.cpp / .h                 # ReadMessage 353 (12..8192 validation), AddMessage 513 (Size overwrite, 4-byte copy 586),
                                    # SendOneMessage 686, HEADER 42-50, RECV/SEND_BUFFER 128 KB, MAX_MESSAGE_SIZE 8192
```

The component itself has no internal structure to speak of: no helper functions, no local structs, no state. All of its logic is inline in one function, and all of its semantics live in the callees it invokes. The only artefacts it owns are nine crack-audit call sites, six `Log()` strings and three in-place mutations of the received packet (`m->Speed`, `m->TargetX/m->TargetY`, `m->Effect`).

---

## 5. Dependency Analysis

```
Internal Dependencies (call graph, outbound)

Exec_MSG_Action
├─ SendHpMode ............................. SendFunc.cpp:1622   (status resend; no-op when Mode != USER_PLAY)
├─ RemoveTrade ............................ Server.cpp:7319     (x2 on the trade-opponent path)
├─ AddCrackError .......................... Server.cpp:684      (x9)
├─ Log .................................... Server.cpp:6701     (x7)
├─ SendSetHpMp ............................ SendFunc.cpp:1590   (x2)
├─ DoRecall ............................... Server.cpp:7558     (x5: celestial, trade-only, newbie, guild, BR, colo)
├─ DoTeleport ............................. Server.cpp:8212     (arena slot 2, via CCastleZakum)
├─ GetAction .............................. GetFunc.cpp:1239    (snap-back frame construction)
├─ GetAttribute ........................... GetFunc.cpp:1257    (x2: trade-only bit 0x80, newbie/guild bits)
├─ BASE_GetVillage ........................ Basedef.cpp:4258
├─ BASE_GetGuild .......................... Basedef.cpp:4164
├─ BASE_GetRoute .......................... Basedef.cpp:5538    (collision path only)
├─ GetEmptyMobGrid ........................ GetFunc.cpp:1901    (collision path only)
├─ CCastleZakum::CheckMove ................ CCastleZakum.cpp:377 (-> DoTeleport -> GridMulticast)
├─ GridMulticast(conn,tx,ty,msg) .......... SendFunc.cpp:620    (-> GetEmptyMobGrid, SendCreateMob,
│                                                                  SendRemoveMob, SendRemoveItem, SendPKInfo)
└─ CPSock::SendOneMessage / CPSock::AddMessage .. CPSock.cpp:686 / :513

Shared mutable global state read or written
  pMob[MAX_MOB]            TargetX/TargetY/Route/LastSpeed/LastMove targets, ClassMaster, Guild, Level, Hp, Mp
  pUser[MAX_USER]          Mode, Trade, TradeMode, OnlyTrade, LastMove, LastActionTick, LastIllusionTick, NumError, ReqMp, cSock
  pMobGrid[4096][4096]     read at 246, written transitively by GridMulticast 643/657
  pHeightGrid[4096][4096]  read transitively by BASE_GetRoute and GetEmptyMobGrid
  g_pAttribute[1024][1024] read via GetAttribute
  g_pSpell[MAX_SKILLINDEX]  index 73 (ManaSpent) hardcoded at 81
  g_pGuildZone[MAX_GUILDZONE] read via BASE_GetVillage / BASE_GetGuild / DoRecall
  Pista[7], PistaPos[7][3][2] read at 184/187
  g_pMessageStringTable     _NN_Newbie_zone, _NN_OnlyVillage, _NN_Only_Guild_Members, _NN_Bad_Network_Packets
  BrState, BRItem, BrGrid, Colo150Limit, FREEEXP, CurrentTime, temp[1024] (shared scratch buffer)

External Dependencies
  None. The component performs no network I/O beyond the client's own send buffer, no file I/O beyond
  Log() appends, no DB messages, no timers. It is entirely an in-process state transformer.
```

Note the two indirect commit paths: `DoRecall` and `DoTeleport` both call `GetAction` + `GridMulticast` internally, so a rule that "returns" after a recall has still mutated the world position twice (once by the recall, once by the caller's commit at `:283` when the rule is one of the late ones). The dependency on `GridMulticast` is therefore not a single call but a five-way fan-out through three other translation units.

---

## 6. Afferent and Efferent Coupling

Measured over the whole project with the code-graph index and verified by grep for call sites.

| Component (symbol) | Kind | Afferent (who calls it) | Efferent (what it calls) | Critical |
|--------------------|------|-------------------------|--------------------------|----------|
| `Exec_MSG_Action` (whole component) | free function | 1 (`ProcessClientMessage.cpp:99`, one `case` spanning three wire types) | 15 named functions + 2 `CPSock` methods = 17 | High |
| `GridMulticast(int,int,int,MSG_STANDARD*)` | free function (overload) | 53 callers across 22 files | `GetEmptyMobGrid`, `SendCreateMob`, `SendRemoveMob`, `SendRemoveItem`, `SendPKInfo` | High |
| `DoRecall` | free function | 13 callers | `GetEmptyMobGrid`, `GetAction`, `GridMulticast` | High |
| `DoTeleport` | free function | 16 callers | `GetEmptyMobGrid`, `GetAction`, `GridMulticast` | High |
| `AddCrackError` | free function | 21 callers | `Log`, `SendClientMessage`, `CharLogOut` | Medium |
| `GetAction` | free function | 9 callers | none (pure packet filler) | Low |
| `GetAttribute` | free function | 10 callers | none (pure read of `g_pAttribute`) | Low |
| `BASE_GetRoute` | free function | 5 callers (`_MSG_Action` is 1 of them) | none | Medium (hot loop) |
| `GetEmptyMobGrid` | free function | 12+ callers | `Log` | Medium |
| `BASE_GetVillage` / `BASE_GetGuild` | free functions | 15 / 6 callers | none | Low |
| `CCastleZakum::CheckMove` | static method | 1 (`_MSG_Action.cpp:238` only) | `DoTeleport` | Low (unreconciled variant exists) |
| `SendHpMode` | free function | 23 callers | `CPSock::AddMessage`, `CloseUser` | Low |
| `RemoveTrade` | free function | 10 callers | `SendClientSignal`, `GetCreateMob`, `GridMulticast` | Medium |
| `SendSetHpMp` | free function | 9 callers | `SetReqHp`, `SetReqMp`, `CPSock::AddMessage` | Low |
| `MSG_Action` (struct) | data type | 6 writers / 5 readers (see below) | - | Medium |
| `pMob[].TargetX/TargetY` | global state | 100+ read sites | written by `GridMulticast` only for players | Critical |

Reconciliation with the given figures (afferent 1, efferent 14): afferent 1 is exactly right - the function has a single call site. Efferent 14 is one short of the 15 named functions I measured (`SendHpMode`, `RemoveTrade`, `DoRecall`, `AddCrackError`, `SendSetHpMp`, `Log`, `GetAction`, `GetAttribute`, `BASE_GetVillage`, `BASE_GetGuild`, `CheckMove`, `GetEmptyMobGrid`, `BASE_GetRoute`, `GridMulticast`, `DoTeleport`). The most likely explanation is that the earlier count excluded `GetAction` (used only in the hard-violation snap-back branch) or collapsed the two `BASE_Get*` lookups; either way it is not a material disagreement. Adding the two `CPSock` method calls raises the true count to 17.

Structural observations:

- **Instability**. With Ca = 1 and Ce = 17, the component is maximally unstable in Martin's metric (I = Ce/(Ca+Ce) = 0.94): it depends on almost everything and nothing depends on it. That is the correct shape for a leaf protocol handler, and it explains why none of its rules can be tested in isolation without the whole world mock.
- **Shared-state coupling dominates call coupling.** The publishes-to-subscribes ratio is misleading here: the real coupling is `pMobGrid`, a 32 MB global that 53 `GridMulticast` callers write and every movement-aware system reads. The component's correctness depends on an invariant (`pMobGrid[y][x] == conn` iff `pMob[conn].TargetX == x && TargetY == y`) that it maintains only through `GridMulticast` and that `GridMulticast` itself deliberately violates when it overwrites an occupied cell (`SendFunc.cpp:645-657`).
- **Temporal coupling with `ProcessDBMessage`.** The dead 900 ms limiter (BR-09/BR-10) exists because a login-time initialisation in another translation unit pins a field this file reads. The two files agree only by coincidence of the sentinel's value being duplicated as a literal in one of them.
- **Data coupling with `MSG_Action`.** Six producers (`GetAction`, `DoRecall`, `DoTeleport`, `DoSummon`, `DoGreatTeleport`, and this handler) and five consumers (`GridMulticast`, `GridMulticast(int,int,...)` reinterpretation, the client, `_MSG_Attack`'s route reset, and `ProcessDBMessage`'s initial seed) share a 52-byte struct whose `Size`, `ID`, `Effect` and `Speed` fields carry different authority depending on who filled them.

---

## 7. Endpoints

The component exposes no REST, GraphQL or gRPC surface. It exposes three binary client-message verbs on the TMSrv game channel, all funnelled through one handler.

| Endpoint | Protocol | Value | Payload | Description |
|----------|----------|-------|---------|-------------|
| `_MSG_Action` | TMSrv binary client message (`FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME`) | `108 \| 0x0100 \| 0x0200` = 876 (`Basedef.h:1693`) | `MSG_Action` (52 bytes): `{short Size; char KeyWord; char CheckSum; short Type; short ID; uint ClientTick; short PosX, PosY; int Effect; int Speed; char Route[24]; short TargetX, TargetY}` (`Basedef.h:1696-1708`) | Walk / run. Full validation pipeline including the 15 s / 120 s clock window |
| `_MSG_Action2` | same | `102 \| 0x300` = 870 (`Basedef.h:1694`) | `MSG_Action` | Stop / stand. Only two-consecutive-stop suppression; **no clock checks**, but still commits movement |
| `_MSG_Action3` | same | `104 \| 0x300` = 872 (`Basedef.h:1695`) | `MSG_Action` | Mage "Ilusao" skill move. Adds the class-3 + learned-skill gate and the `g_pSpell[73]` mana cost, then shares the whole pipeline |

Dispatch path: `Server.cpp:3975-4001` (`WSA_READ` handler, message extracted by `CPSock::ReadMessage`) -> `ProcessClientMessage.cpp:38` -> `case _MSG_Action: case _MSG_Action2: case _MSG_Action3:` (`:96-98`) -> `Exec_MSG_Action` (`:99`). Pre-handler validation: header `ID` in `0..MAX_USER-1` (`:42-51`), `ServerDown < 120` (`:53`), `LastReceiveTime` refresh (`:56-57`), `ClientTick != SKIPCHECKTICK` for client-sourced frames (`:63-64`). `CPSock::ReadMessage` (`CPSock.cpp:353-467`) enforces only `12 <= Size <= 8192`; the minimum accepted frame is therefore 12 bytes against a 52-byte struct, and neither the dispatcher nor the handler checks the type-specific length.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / mechanism | Data Format | Error Handling |
|-------------|------|---------|----------------------|-------------|----------------|
| Client session | Player I/O | Receive the action frame; send snap-back, self echo, HP/MP resend | `pUser[conn].cSock` (`SendOneMessage`, `AddMessage`) | `MSG_Action`, `MSG_SetHpMp`, `MSG_MessagePanel` | return value ignored for `SendOneMessage`; `AddMessage` failure only logged by the callee |
| Neighbouring clients | Broadcast | Movement propagation, mob/item creation and removal on window change | `GridMulticast(conn,tx,ty,msg)` -> per-recipient `AddMessage(msg, msg->Size)` | raw client frame relayed verbatim | none; the attacker-controlled `Size` is trusted |
| Position grid `pMobGrid[4096][4096]` | In-process state | Authoritative occupancy; written only through `GridMulticast` | direct array | `unsigned short` mob index | occupies-then-overwrites when relocation fails (`SendFunc.cpp:645-657`) |
| Height grid `pHeightGrid[4096][4096]` | In-process state | Impassability (`127`) and route step legality (`MH == 8`) | `BASE_GetRoute`, `GetEmptyMobGrid` | `char` per cell | bounds-checked inside callees, not by this handler |
| Attribute map `g_pAttribute[1024][1024]` | Configuration data | Zone semantics: `0x80` newbie/trade, `0x20` guild, `0x02` blocked height (elsewhere) | `GetAttribute(x,y)` -> `g_pAttribute[y/4][x/4]` | `unsigned char` per 4x4 cell | returns 0 for out-of-range; bit semantics undocumented in the repository |
| Guild zone table `g_pGuildZone[5]` | In-process configuration | City rectangles, charging guild, spawn points | `BASE_GetVillage`, `BASE_GetGuild`, `DoRecall` | `STRUCT_GUILDZONE` | zone index range-checked at `:229`; the "not found" value 5 is not always handled |
| Arena registry `Pista[7]` / `PistaPos[7][3][2]` | In-process shared state | Slot-2 leader authorisation and spawn pads | direct read | `STRUCT_PISTA` | no validation that the registration is current |
| Castle quest table `CastleQuestPosition[9][4]` / `CastleQuestParty[13]` | In-process state | Castle quest area lock | `CCastleZakum::CheckMove` | `int[9][4]` rectangles + conn list | first rectangle's Y range inverted (dead); no message on refusal |
| Spell table `g_pSpell[103]` | Configuration | Mana cost of the illusion move (hardcoded index 73) | direct indexed read | `STRUCT_SPELL` | index is a literal; `ManaSpent` is not scaled by `SaveMana`/`Special` |
| Message string table `g_pMessageStringTable` | Configuration | Player-facing rejection texts | indexed read | `char*` | compile-time indices only |
| Runtime globals `BrState`, `BRItem`, `BrGrid`, `Colo150Limit`, `FREEEXP`, `CurrentTime` | Process configuration | Event gates and the clock window | direct read | `int` / `unsigned int` | no validation at use site; `CurrentTime` comes from `timeGetTime` and wraps |
| File log | Audit | Nine crack codes, seven diagnostic lines | `Log()` -> `fLogFile` + `SetWindowText` | text lines | no error handling; `temp[1024]` is a shared scratch buffer |
| DBSrv | (none) | This component sends no DB message and triggers no persistence | - | - | position is persisted only on the periodic character save, never per move |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Message-handler dispatch (Command) | `switch (std->Type)` in the client dispatcher | `ProcessClientMessage.cpp:66-100` | route three wire verbs to `Exec_MSG_Action` |
| Type-tag branch inside the handler (manual downcast) | `if (m->Type == _MSG_Action3) ... else if (_MSG_Action) ... else if (_MSG_Action2)` | `_MSG_Action.cpp:72, :113, :136` | recover the verb from the shared struct after the dispatcher collapsed the cases |
| Guard-clause validation ladder | 20+ `if (...) { ...; return; }` blocks in a fixed order | `:25-238` | fail fast before any state change |
| Transaction script | one flat sequence: gate -> audit -> authorise -> commit -> notify -> encode | whole function | keeps the accepted path linear; no per-rule abstraction |
| Shared-state commit-at-exit | position is written only in `GridMulticast`, called at the end | `:283` -> `SendFunc.cpp:813-822` | single writer; but two rules run after it |
| Uniform spatial grid (spatial hashing) | `pMobGrid[4096][4096]`, one entry per cell, no buckets | `Server.h:54` | O(1) occupancy lookup for 25k mobs |
| Area-of-interest window diffing | old-33x33 vs new-33x33 set difference, create/remove on both sides | `SendFunc.cpp:659-811` | incremental interest management instead of full refresh |
| Zero-copy packet relay / in-place mutation | the received `pMsg` is rewritten and re-sent as the outbound frame | `:154`, `:267-278`, `:283` | avoids a copy and a re-serialisation; inherits every trust problem of the input |
| Sentinel-based limiter bypass | `SKIPCHECKTICK` (`0xE0A1ACA`) written at login, compared by value in the limiter guards | `ProcessDBMessage.cpp:825`, `_MSG_Action.cpp:92/115` | allow the first action after login; the reset is missing |
| Magic-rectangle policy engine | seven hardcoded rectangles + three attribute masks, evaluated in a fixed order | `:52, :177, :215, :223, :316, :334`, `CCastleZakum.cpp:46-63` | zone policy with no configuration file |
| Normalisation at the boundary | `m->Speed` clamped to the character's run nibble before relay | `:147-155` | prevents the attacker's value from reaching other clients |
| Crack-point accumulator | `NumError += weight`, threshold check, audit type filter | `Server.cpp:684-707` | forensic counter with a logout threshold |
| Recursive-descent route generator | `BASE_GetRoute` greedy 8-way step toward the target with height delta `MH` | `Basedef.cpp:5538-5826` | server-side path derivation from a client origin |
| Free-function "service layer" | all behaviour lives in `Server.cpp` / `SendFunc.cpp` / `GetFunc.cpp` / `Basedef.cpp` globals | throughout | no class owns movement; the handler is a coordinator |
| God-function anti-pattern (moderate) | 320 lines, 26 rules, 6 responsibilities, one body | whole file | consequence of decompilation and incremental feature growth |

Architectural assessment. The component is written in a "protocol adapter over global state" style that is appropriate for a 2000s-era C server: one handler per message, direct global access, no allocation, no exceptions. Within that style it is coherent - every rule is a guard clause, every mutation is at the end, and the accepted path is linear and readable. What it lacks is any notion of a **movement model**: there is no object that owns "position + velocity + last tick", no function that answers "is this displacement legal for this actor at this time", and no separation between *authorisation* (may this player be here) and *plausibility* (could this player get here). The eight zone rules are authorisation and are all present; plausibility is entirely absent. That is the architectural gap that the dead 900 ms limiter and the write-only `LastSpeed/LastTime` fields represent.

The second structural weakness is the shared commit function. `GridMulticast(conn,tx,ty,msg)` fuses four responsibilities - grid repair, occupancy resolution, AOI diffing and position commit - and is called by 53 sites with 52 different message types. It was clearly written for `MSG_Action` and then generalised by convention. In this component that is harmless; in the other 52 callers it means any call silently rewrites `LastX/LastY/LastSpeed` from unrelated payload bytes and re-commits `TargetX=tx, TargetY=ty`.

---

## 10. Technical Debt & Risks

| ID | Risk Level | Component Area | Issue | Impact |
|----|------------|----------------|-------|--------|
| D-01 | High | `_MSG_Action.cpp:272`, `:278` | `sizeof(m)` (4 bytes, pointer) used as the packet length instead of `sizeof(MSG_Action)` (52 bytes). Verified: the project builds `Win32` only (`TMSrv.vcxproj:6,10`), so the pointer is 32-bit. `CPSock::AddMessage` sets the header `Size` to the passed value (`CPSock.cpp:538`), copies exactly 4 bytes (`:586`) and advances the send pointer by that size (`:588`), so a 4-byte frame is emitted whose `Type`, `ID` and `ClientTick` are never written | Both collision-resolution sends (the snap-back correction at `:272` and the queued echo at `:278`) reach the client as unparseable 4-byte frames. The corrected position is never delivered, so the client's prediction desynchronises from the server exactly in the case this branch exists to fix. Note the correct form appears 15 lines later at `:287` (`sizeof(MSG_Action)`), which localises the defect to these two statements |
| D-02 | High | whole handler | `m->Size` is never validated. The 12-byte header alone is an accepted frame (`CPSock.cpp:386-411`), but the handler reads and writes fields up to offset 52 | Reads fields from stale buffer content; writes (`:154`, `:267-278`) land in the *next* queued packet inside `pRecvBuffer` when the declared size is short |
| D-03 | High | `:251`, `:283` -> `SendFunc.cpp:714/799` | `strlen(m->Route)` on a 24-byte client field with no guaranteed NUL, inside a 128 KB buffer; and the same frame is re-broadcast with the attacker's `msg->Size` | Up to 8,188 bytes of stale send-buffer memory can be delivered to every player in the destination window (information disclosure), plus arbitrary send-stream advance and client-side desync. `MAX_MESSAGE_SIZE` is 8,192 (`CPSock.h:38`) |
| D-04 | High | `:283` | `m->ID` is relayed without being bound to `conn`; the dispatcher only range-checks it (`ProcessClientMessage.cpp:42-51`) | Client-side spoofing: neighbours animate a different mob than the one the server moved, while the server's grid state belongs to `conn` |
| D-05 | High | `:92`, `:115` vs `ProcessDBMessage.cpp:825` | The 900 ms movement limiter is permanently disabled: `LastActionTick` is only ever set to `SKIPCHECKTICK` and never reset; both guards are `LastActionTick != SKIPCHECKTICK && ...` | No cadence limit on movement at all. Combined with D-06 this is a complete speed-hack path |
| D-06 | High | `:136-145` | `_MSG_Action2` skips every clock check but falls through into the full movement pipeline | A client can move using the "stop" verb and bypass the only surviving clock window; alternating `_MSG_Action` and `_MSG_Action2` also defeats the consecutive-stop suppression |
| D-07 | High | `:160` | No distance/time validation. 33 cells per packet is the only displacement ceiling; `LastSpeed`, `LastTime`, `LastX`, `LastY` are written (`SendFunc.cpp:815-819`) and read nowhere in the project | Teleport-style movement at packet rate; the declared-speed clamp is the sole speed input and is client-chosen |
| D-08 | Medium | `:27` | `SendHpMode(conn)` is a no-op in the branch that calls it, because `SendHpMode` tests `Mode != USER_PLAY` and returns (`SendFunc.cpp:1627-1628`) | The client that believes it is in play gets no corrective status; divergence persists |
| D-09 | Medium | `:81-90` vs `:92-109` | Mana is deducted before the cadence and clock rejection paths | A rejected `_MSG_Action3` still costs MP and decrements `ReqMp` (an `int`, so it can go negative). Self-inflicted drain; also lets a player be punished by a stale client clock |
| D-10 | Medium | `:167-171` | The snap-back correction is built with `Type = _MSG_Action3` (the mana-charging skill verb) and `Speed = 6`, which may exceed the character's own run nibble just enforced at `:147-155` | The client is told a skill was cast, not that it was corrected; `Speed=6` is unreachable for characters with `AttackRun & 0xF < 6` |
| D-11 | Medium | `:184` | `partyleader != conn || (conn != P0 && conn != P1 && conn != P2)` - a registered arena leader who is currently a party *member* is recalled out of his own arena | Registered leaders cannot walk their party into slot 2 unless they are the party leader at that moment |
| D-12 | Medium | `:238` -> `CCastleZakum.cpp:49, :388` | `CastleQuestPosition[0] = {2221, 1270, 2259, 1246}` has `y1 > y2`, so `targety >= 1270 && targety <= 1246` is unsatisfiable | One of the nine castle-quest protection rectangles is unenforced (dead rule) |
| D-13 | Medium | `:238` + `:283` | `CheckMove` teleports the intruder to his current cell, and then the handler performs the normal commit anyway | The castle-quest refusal is undone by the second `GridMulticast`: the player ends at the rejected target |
| D-14 | Medium | `:257-263` | Collision loop: `if (cell free) continue;` skips the recomputation, so the loop spins without effect; `BASE_GetRoute`'s return value is ignored; the origin is the unvalidated `m->PosX/PosY` | Destination derived from an arbitrary client-reported origin; the loop's intent is unrecoverable from source |
| D-15 | Medium | `:246-279` -> `SendFunc.cpp:645-657` | When relocation fails, `GridMulticast` assigns `pMobGrid[ty][tx] = conn` over another mob's entry without moving that mob | Grid/state divergence: the evicted mob keeps `TargetX/TargetY` for a cell that no longer references it |
| D-16 | Medium | `:292-307` | A route-derived nibble is OR-ed into bits 4-7 of `CurrentScore/BaseScore.Merchant`; the same byte's bits 6-7 encode the player's city (`_MSG_ChangeCity.cpp:30-31`, `Server.cpp:7562`, `Basedef.cpp:4017`) | Overlapping encodings on a documented-overloaded field; the value is projected to clients through `MSG_UpdateScore` |
| D-17 | Medium | `:244`, `:281` | `m->Route` is copied verbatim into `pMob[conn].Route` with no alphabet validation (only `'1'..'9'` are meaningful to `BASE_GetRoute`) | Persistent mob state holds arbitrary client bytes; the second `strlen` at `:292` can read past the 24-byte array if no NUL was present |
| D-18 | Medium | `:309-338` | Battle Royale and Colo-150 recalls run *after* the grid commit and send no message | Every observer sees a walk followed by a teleport; the player is relocated with no explanation |
| D-19 | Low | `:160` | `Effect` (0 walk / 1 teleport) is never validated; the check is commented out | Clients choose their own presentation; a 33-cell jump can be labelled a teleport |
| D-20 | Low | `:36, :97, :107, :120, :130, :141, :149, :173` | `AddCrackError` return value ignored (only `:77` tests it) | Possible post-`CharLogOut` state mutation, including a position commit for a logged-out character |
| D-21 | Low | `:92`, `:115` | The literal `235543242` is used instead of `SKIPCHECKTICK` | The sentinel can drift from its definition silently |
| D-22 | Low | `:66-69` | `checktime` special-cases the first 120 s of process life (`CurrentTime <= 0x1D4C0`) | During startup no past-timestamp rejection is possible; undocumented |
| D-23 | Low | `:285-289` | `_MSG_Action3` echo is sent after `GridMulticast`, which may already have delivered the frame | Duplicate delivery for some window geometries; harmless today |
| D-24 | Low | `:63-69` | `movetime` is `int` while `CurrentTime` is `unsigned int`; the upper bound needs an explicit cast, the lower bound is signed | Correct as written but fragile; a `ClientTick` with the high bit set relies on the cast for rejection |
| D-25 | Low | whole file | Magic numbers: `128`, `9/8/10`, `1/2`, `180`, `900`, `15000`, `120000`, `0x1D4C0`, `0x1F`, `33`, `66`, `3378/3398/1427/1448`, `4096`, `73`, `3`, `2`, `35`, `999`, `0x80`, `0x20`, `0xF0`, `48`, `3`, `16`, `2604/2648/1708/1744`, `100`, `200`, `150`, `6` | 30+ unexplained literals; every zone rectangle is a candidate for a wrong digit (D-12 is exactly that failure) |
| D-26 | Low | whole file | No logging on successful moves; nine rejection logs with inconsistent prefixes (`err,` / `etc,`) and duplicated text (the illusion copy-paste writes `err,illusion` for a walk) | Log-based traffic analysis cannot reconstruct movement at all |

Cross-component inconsistency worth recording: BR-18 (guild zone) and BR-17 (newbie zone) both exempt high-tier characters implicitly through level comparisons (`> MAX_LEVEL`, `> 999`), while BR-05 requires level >= 180 for celestials. Three different expressions of "high tier" coexist in one function.

---

## 11. Test Coverage Analysis

**Result: no tests exist for this component, and no test infrastructure exists in the repository.**

Evidence:

| Check | Command / artefact | Result |
|-------|--------------------|--------|
| Test source files | glob `**/*est*` over the whole tree | 3 hits, none of them tests: `_MSG_Restart.cpp`, `_MSG_Quest.cpp` (filename match only) and one generated report |
| Test framework symbols | grep `gtest|catch2|doctest|CppUnit|TEST(|TEST_F(|assert(|_ASSERT` under `Code/` | 0 matches |
| Test projects in the solution | `W2PP Code Project.sln` project list | 2 projects: `DBSrv` (`Code\DBSrv\DBSrv.vcxproj`), `TMSrv` (`Code\TMSrv\TMSrv.vcxproj`). No test project |
| Build targets | `TMSrv.vcxproj` | `ConfigurationType = Application` for both Debug and Release, `Win32` only. No post-build test invocation |
| CI configuration | root listing | no `.github`, no `.gitlab-ci.yml`, no `azure-pipelines.yml`, no build scripts |
| Fixtures / golden data | root listing | none; runtime data (`SkillData.bin`, `ItemList.bin`, `AttributeMap.dat`, `CastleQuest.txt`) is not in the repository either |
| Harness hooks | `_PACKET_DEBUG` conditional in `CPSock.cpp:544-552` and `Server.cpp:3980-3988` | a debug-only packet trace, not a test mechanism |

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `Exec_MSG_Action` (whole component) | 0 | 0 | 0% (not measurable; no harness) | N/A - untested |
| `GridMulticast` (the commit path this component relies on) | 0 | 0 | 0% | N/A |
| `BASE_GetRoute` (route derivation used in the collision path) | 0 | 0 | 0% | N/A |
| `GetEmptyMobGrid` (relocation used by the collision path and by recall/teleport) | 0 | 0 | 0% | N/A |
| `CCastleZakum::CheckMove` | 0 | 0 | 0% | N/A |

Risk assessment of the absence. This component is the highest-fan-in writer of player position in the server, and every one of its 26 rules is a pure decision over in-memory state (`pMob`, `pUser`, `pMobGrid`, `g_pAttribute`, `Pista`, four `int` globals). It is, in other words, close to the ideal target for characterisation tests: a test could set up a mob at (x, y) with level L, class master CM, guild G and attributes A, feed a synthetic `MSG_Action`, and assert on `pMob[conn].TargetX/TargetY`, on the bytes queued in a fake `CPSock`, and on the crack counter. The absence of any such harness means:

- The two `sizeof(m)` defects (D-01) survived because no test ever asserted on the emitted frame length.
- The inert limiter (D-05) survived because no test asserted that a second move inside 900 ms is refused.
- The inverted castle rectangle (D-12) survived because no test exercised the first of the nine rectangles.
- Every risk in section 10 is currently a code-reading finding rather than an observed failure.

The only executable evidence available without writing new code is the debug packet trace (`_PACKET_DEBUG`), which logs type and size for every frame and would at least surface D-01 as a 4-byte `_MSG_Action` on the wire.

---

## 12. Analysis Limitations

- Runtime behaviour was not observed. The server, its data files and a W2 client are not part of the repository, so every conclusion about zone semantics, event state (`BrState`, `BRItem`, `BrGrid`, `Colo150Limit`), spawn coordinates and the meaning of map-attribute bits is derived from source and from the shapes of the data structures.
- The `0x80` and `0x20` attribute bits have no documented definition in the repository. Their meaning is inferred from consistent use in this component (`0x80` newbie/trade cells, `0x20` guild zones) and from the negative use of `0x02` in `BASE_ApplyAttribute` (`Basedef.cpp:2601-2602`). The sibling `Attack Handler` report reaches the same conclusion about `0x40` and is equally unable to resolve `0x01`.
- `g_pSpell[73]` is a literal skill index. The skill's identity ("Ilusao", per the comment at `:71`) and its `ManaSpent` value live in `SkillData.bin`, which is absent. The class gate (`Class == 3` plus `LearnedSkill & 0x2`) is stated as written, not as it appears to a player.
- `sizeof(MSG_Action) == 52` is computed from the declared layout (`_MSG` macro = 12 bytes, then `short, short, int, int, char[24], short, short`) with 4-byte alignment. It is not measured at runtime. It differs from the 48 bytes quoted in the sibling `Client Handler Interface Contract` report; the 4-byte difference does not change the nature of D-01, only the arithmetic of "4 bytes out of N".
- The "efferent 14" figure supplied with the task could not be reproduced exactly; 15 named functions plus 2 socket methods were measured (section 6). The discrepancy is attributed to counting method, not to a missing dependency.
- Some code in `GridMulticast` was read through the code-graph index rather than the raw file for the window-diff passes; the line numbers cited for `SendFunc.cpp:659-811` should be treated as accurate to the current on-disk revision, which the index reported as fresh.
- The decompiled origin of the file means that variable names such as `Unk` (`:293`) and the commented-out condition at `:160` may reflect either upstream intent or decompilation artefacts. Where intent is unrecoverable (BR-24 in particular) the confidence level is stated in the rule text.

---

## 13. Summary of Referenced Files

| File | Role in this analysis | Key lines |
|------|-----------------------|-----------|
| `Code/TMSrv/_MSG_Action.cpp` | the component | 21-340 (all) |
| `Code/TMSrv/ProcessClientMessage.cpp` | dispatcher / pre-handler validation | 38-64, 96-100 |
| `Code/TMSrv/ProcessClientMessage.h` | handler declaration, externals | 51, 84 |
| `Code/TMSrv/SendFunc.cpp` | `GridMulticast` commit path, status sends | 560-618, 620-823, 843-972, 1590-1645 |
| `Code/TMSrv/Server.cpp` | `AddCrackError`, `RemoveTrade`, `DoRecall`, `DoTeleport`, globals, socket loop | 43, 154-177, 281, 317, 389-393, 684-707, 3625, 3975-4001, 7319-7352, 7558-7602, 8212-8230 |
| `Code/TMSrv/ProcessDBMessage.cpp` | login sentinels, `OnlyTrade`, position seed | 157-179, 805-831 |
| `Code/TMSrv/GetFunc.cpp` | `GetAttribute`, `GetEmptyMobGrid`, `GetAction`, `GetInView` | 640-656, 1239-1273, 1901-2001 |
| `Code/TMSrv/CCastleZakum.cpp` / `.h` | `CheckMove`, `CastleQuestPosition` | 46-63, 377-394 |
| `Code/TMSrv/CMob.h` / `CMob.cpp` | mob movement state, NPC route derivation (contrast) | `CMob.h:40-57`, `CMob.cpp:857-1032` |
| `Code/TMSrv/CUser.h` | session fields used by the rules | 46, 59-75, 91, 115 |
| `Code/TMSrv/Server.h` | grid declarations, runtime globals | 48-56, 196, 237, 274, 309-311, 334-338, 395-399 |
| `Code/Basedef.h` | `MSG_Action`, `_MSG` macro, constants, `STRUCT_PISTA`, class masters | 56-182, 646-656, 925-946, 1693-1708 |
| `Code/Basedef.cpp` | `BASE_GetRoute`, `BASE_GetVillage`, `BASE_GetGuild`, score fold, height init | 744-761, 2583-2606, 4017, 4164-4267, 5415-5417, 5538-5826 |
| `Code/CPSock.cpp` / `.h` | frame read/write, size limits, 4-byte copy defect mechanism | `CPSock.h:35-50`, `CPSock.cpp:353-467`, 513-591, 686-693 |
| `Code/TMSrv/TMSrv.vcxproj` | build configuration proving `Win32` (32-bit pointers) | 6, 10, 28, 111 |
| `W2PP Code Project.sln` | proof that no test project exists | 6-10 |

---

*End of report.*
