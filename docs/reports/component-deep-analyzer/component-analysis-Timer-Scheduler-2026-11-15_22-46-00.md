# Component Deep Analysis Report - Timer Scheduler

- **Component name:** Timer Scheduler
- **Primary location:** `Code/TMSrv/ProcessSecMinTimer.cpp` (2,314 lines, 63,912 bytes; 63,915 bytes on disk)
- **Secondary locations (contract surface):** `Code/TMSrv/Server.h:105-106` / `:329-330` (declarations and counter externs), `Code/TMSrv/Server.cpp:3631-3632` (`SetTimer` registration), `Code/TMSrv/Server.cpp:3723-3729` (`WM_TIMER` dispatch), `Code/Basedef.h:136-137` (timer ids)
- **Project scope analysed:** repository root `E:\wyd\test-setup\w2pp` (W2PP - decompiled C/C++ Win32 MMORPG game-server emulator for W2)
- **Ignored folders (per parameters):** `.agents`, `.codegraph`, `.opencode`, `openspec`
- **Analysis date:** 2026-11-15
- **Analysis type:** read-only static analysis of source, project files, and configuration contracts. No project file was modified, no code was executed, no test was run.
- **Scope note:** this report covers exactly one component. The `Endpoints` section defined by the analysis guidelines is **omitted** because the component exposes no REST / GraphQL / gRPC / socket protocol surface. Its only entry points are the two Win32 `WM_TIMER` window messages, documented in Section 2 and Section 7 instead.

---

## 1. Executive Summary

### 1.1 Component purpose

The Timer Scheduler is the periodic-execution body of TMSrv. It is a single translation unit containing two free functions and no classes, no state of its own, and no public API beyond those two functions:

- `ProcessSecTimer()` - `Code/TMSrv/ProcessSecMinTimer.cpp:40-2017`, 1,978 lines, registered to fire every **500 ms** (`TIMER_SEC = 0`, `Code/Basedef.h:136`, `SetTimer` at `Code/TMSrv/Server.cpp:3631`).
- `ProcessMinTimer()` - `Code/TMSrv/ProcessSecMinTimer.cpp:2019-2314`, 296 lines, registered to fire every **12,000 ms** (`TIMER_MIN = 1`, `Code/Basedef.h:137`, `SetTimer` at `Code/TMSrv/Server.cpp:3632`).

Everything the component does is dispatched from `MainWndProc` (`Code/TMSrv/Server.cpp:3723-3729`), which routes `WM_TIMER` by `wParam`. The component owns no data structure of its own; it mutates the world state that `Code/TMSrv/Server.cpp` defines and that `Code/TMSrv/Server.h` exports (`pUser`, `pMob`, `pItem`, `pMobGrid`, `pItemGrid`, `mNPCGen`, `pMobMerc`, `Pista`, `g_pGuildZone`, `GuildInfo`). In that sense it is a **scheduler-and-orchestrator component**, not a domain component: its business logic is mostly *when* something runs and *in what order*, and the domain rules it applies are delegated to `Server.cpp` services (`RegenMob`, `ProcessAffect`, `ApplyHp`, `ApplyMp`, `ProcessDecayItem`, `ProcessRanking`, `GuildProcess`, `CloseUser`, `SaveUser`, `CharLogOut`, `ClearArea*`, `DoRecall`, `DoTeleport`, `GenerateMob`, `DeleteMob`) and to two event modules (`CCastleZakum`, `CWarTower`).

### 1.2 Architectural role in the system

`Code/TMSrv/Server.cpp:3650` runs a classic `GetMessage`/`DispatchMessage` pump on one thread. `WSAAsyncSelect` posts socket events (`WSA_ACCEPT`, `WSA_READ`, `WSA_READDB`, `WSA_READBILL`) into the same queue as the two timer messages. The direct consequence - confirmed by the architecture report (`docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:21,82`) and by the absence of any threading primitive in the repository - is that **the whole server is cooperatively multitasked by this component**. Every client packet is processed between two ticks; a tick body that runs long delays packet processing, and a packet handler that runs long delays the next tick. The Timer Scheduler is therefore the de-facto *scheduling policy* of the entire server, and the timing quality of the server is exactly the duration distribution of these two functions.

The architecture report estimates afferent coupling ~7 and efferent coupling ~9 for this component. Section 6 re-measures both from the source: the afferent side is small when counted in *translation units* (exactly one caller translation unit, `Server.cpp`) but very large when counted in *referenced global symbols* (`pUser`/`pMob` are shared with 65/64 TUs, and `SecCounter` - a counter this component exclusively increments - is read by five other TUs as a wall-clock substitute). The efferent side is far larger than the estimate: the file calls **~105 distinct external functions** across ~15 translation units plus the Win32 API (Section 6.3).

### 1.3 Key findings

1. **The counters are not what their names say.** `SecCounter` (`Code/TMSrv/Server.cpp:384`) increments once per 500 ms tick (`ProcessSecMinTimer.cpp:123`), i.e. **twice per second**; `MinCounter` (`Server.cpp:385`) increments once per 12 s tick (`:2282`), i.e. **five times per minute**. Every modulus in the file (`% 2`, `% 4`, `% 6`, `% 8`, `% 12`, `% 16`, `% 20`, `% 32`, `% 120`, `% 1200`) and every external consumer of these two globals inherits this mismatch. Section 2.6 tabulates the real durations.
2. **There is no drift compensation, no accumulator, and no catch-up.** Scheduling is `SecCounter % N == 0`, where `SecCounter` is incremented by the tick body itself. Windows `WM_TIMER` is a low-priority, coalescing message: when a tick body exceeds 500 ms the missed messages are merged, and the counter therefore advances **slower than wall-clock**. All "seconds" in this component are tick counts, not elapsed time. The only absolute-time source used inside the tick is `time()`/`localtime()` in the calendar blocks (`:644-647`, `:2024-2025`), and those blocks are still gated by a tick modulus (`SecCounter % 2`), so a calendar boundary can be missed entirely.
3. **Calendar events are edge-detected by wall-clock fields, not by state.** Kefra (`:649`), the three nightmare resets (`:660`, `:669`, `:679`), the rune-track entry/exit windows (`:690`, `:758`) and the nightmare start gates (`:1040`, `:1060`, `:1079`) all require an exact `tm_min` plus `tm_sec == 0`. Because the block that contains them runs on `SecCounter % 2 == 0` (once per second of tick time, and only when the OS delivers ticks), a whole event can be skipped if no tick lands inside `tm_sec == 0`. The commentary in the source ("two minutes untill the server closes", `:42`) shows the author already reasoned in wall-clock units while coding in tick units.
4. **The component freezes its own clock during shutdown.** `ProcessSecTimer` begins with `if (ServerDown == 120)` and, in that branch, never reaches `SecCounter++` (`:123`): the entire rest of the server keeps running but every tick-modulus subsystem stops advancing (`:42-94`). The drain loop closes exactly one account per tick and calls `PostQuitMessage` only when `UserCount` reaches `MAX_USER`.
5. **A hot-path file read is guarded by an inverted test.** `if(SecCounter % 120) CReadFiles::ReadMacblock();` (`:127-128`) executes the file open/parse **119 out of every 120 ticks** (~2.4 times per second), because the condition is truthy whenever the counter is *not* a multiple of 120. This is the single highest-frequency disk I/O in the server.
6. **Several event reward paths contain index and identity defects** (Section 9): a prize granted to `Pista[3].Party[3]` - a one-past-the-end element that aliases `Pista[4].Party[0]` (`:980`); a level-2 rune granted to `Pista[1].Party[0]` from the party-3 branch (`:882`); a duplicate `Pista[4].Party[1]` check in `SendDamageLeader` that never tests `Party[2]` (`Code/TMSrv/Server.cpp:5573-5582`).
7. **The "child account hours" rule is permanently true.** The forced-logout gate at `:178` tests `(g_Hour <= 12 || g_Hour >= 19)`; `g_Hour` is defined as `0` (`Code/TMSrv/Server.cpp:288`) and **never assigned anywhere in the repository**, so the window is always satisfied.
8. **The combat body writes diagnostic files from the tick loop.** `ProcessSecTimer` opens `AttackDieTeste.txt` (`:1652`) and `Gethalf.txt` (`:1674`) with `fopen("a+")` inside the per-mob combat loop, and `Log()` writes with `fprintf` **plus `SetWindowText(hWndMain, ...)`** (`Code/TMSrv/Server.cpp:6722-6725`) synchronously on the same thread, including inside `DeleteMob` loops (`:1368`, `:1381`, `:1572`).
9. **Two full-table combat/NPC passes run on every 500 ms tick** over `MAX_MOB = 25,000` slots: the idle/peace pass visits 1/6 of NPC slots (`:1357`, stride 6) and the combat pass visits 1/4 (`:1557`, stride 4), plus up to 12 `SendEnvEffect*` rectangle scans and 46 rectangle-damage scans every 5-6 ticks (`:1247-1337`). Each rectangle scan is a per-cell loop over `pMobGrid`.
10. **No automated verification exists.** There is no test project in `W2PP Code Project.sln` (only `DBSrv` and `TMSrv`), no test framework, no fixture, no CI definition, no CMake/Makefile (Section 10), so none of the 54 business rules below is covered by an executable assertion.

---

## 2. Data Flow Analysis

### 2.1 Tick registration and dispatch

```
1. WinMain: SetTimer(hWndMain, TIMER_SEC=0, 500 ms, NULL)        Server.cpp:3631
2. WinMain: SetTimer(hWndMain, TIMER_MIN=1, 12000 ms, NULL)      Server.cpp:3632
3. ListenSocket.StartListen(..., WSA_ACCEPT)                     Server.cpp:3634
4. GetMessage loop (single thread)                               Server.cpp:3650-3654
5. DispatchMessage -> MainWndProc                                Server.cpp:3718
6. WM_TIMER + wParam == TIMER_SEC -> ProcessSecTimer()           Server.cpp:3725-3726
7. WM_TIMER + wParam == TIMER_MIN -> ProcessMinTimer()           Server.cpp:3727-3728
8. Shutdown path rewrites the tick: SetTimer(hWndMain, TIMER_SEC, 200, 0)   ProcessSecMinTimer.cpp:115
```

Both timers are ordinary Win32 timers, so they are *messages*, not callbacks: they queue behind client socket messages and DB socket messages, are dropped rather than accumulated when the queue is saturated, and are never re-entrant (single-threaded message pump).

### 2.2 `ProcessSecTimer` master sequence (authoritative order)

Ordering matters because later blocks observe state written by earlier blocks; the sequence below is the exact execution order for a 500 ms tick in steady state (`ServerDown == -1000`).

```
 1. Shutdown drain short-circuit      if (ServerDown == 120)                       :42-94    -> return
 2. Shutdown countdown                if (ServerDown > -1000)                      :96-119   -> may return at 120
 3. SecCounter++                      (half-second counter, not seconds)           :123
 4. CurrentTime = timeGetTime()                                                     :125
 5. MAC-block reload (inverted guard) if (SecCounter % 120)                         :127-128
 6. Billing reconnect countdown       BILLING && BillCounter && Sock==0             :130-167
 7. Round-robin persistence           if (SecCounter % 8 == 0)                      :169-199
 8. Per-user flush + per-user mob tick for bb in [1, MAX_USER)                       :201-231
    a. cSock.SendMessageA() when nSendPosition != 0, CloseUser on failure
    b. pMob[bb].ProcessorSecTimer() when USER_PLAY   (empty body, CMob.cpp:51-54)
    c. Nightmare-zone Celestial recall (3 rectangles, level >= 180)
 9. Balrog rune-track portal routing  (Pista[5], ticket item 4032, rand()%3)         :233-494
10. Secret-room mob counters          if (SecCounter % 12 == 0)                      :497-518
11. 10-minute quest sweep             if (SecCounter % 1200 == 0)                    :520-538
12. Premium item expiry               if (SecCounter % 120 == 0)                     :539-599
13. Vitals application                if (SecCounter % 2 == 0)  ApplyHp/ApplyMp       :601-619
14. Vitals accumulation               if (SecCounter % 20 == 0) ReqHp/ReqMp += Lv+30  :621-640
15. Calendar block                    if (SecCounter % 2 == 0)                       :642-1099
    a. Kefra weekly spawn (Tue 12:00:00-02)                                          :649-658
    b. Nightmare A/M/N resets (min 9/29/49, 4/24/44, 19/39/59 at sec 0)              :660-687
    c. Rune-track entry (min 0/20/40 at sec 0) teleports + spawns                    :690-755
    d. Rune-track exit (min 15/35/55 at sec 0) purge + rewards + Pista reset          :758-1036
    e. Nightmare start gates (min 14/34/54, 9/29/49, 24/44/4 at sec 0)               :1040-1097
16. Ground-item decay                 if (SecCounter % 2 == 0) ProcessDecayItem()     :1101-1102
17. Ranking + water + carta           if (SecCounter % 4 == 0)                       :1108-1171
18. CCastleZakum::ProcessSecTimer()   (unconditional)                                :1172
19. Per-user staggered pass           i%32==Sec32 -> CheckIdle;                       :1174-1210
                                      i%16==Sec16 -> RegenMob + ProcessAffect
                                      + 5 quest-zone recall guards
20. Battle Royale env damage          BrState==2 && Sec16==0 && BRItem>0             :1212-1231
21. Kingdom tower env damage          if (SecCounter % 6 == 0)                       :1234-1245
22. Leader-room env damage            if (SecCounter % 5 == 0)                       :1247-1337
23. Mercenary stock/reborn loop       for k in [0, MAX_MOB_MERC)                      :1339-1349
24. NPC idle/peace pass (stride 6)    index = Sec6+MAX_USER, +6                       :1353-1552
25. Mob combat pass (stride 4)        index = Sec4+MAX_USER, +4                       :1557-2015
```

### 2.3 `ProcessMinTimer` master sequence

```
 1. time(&rawtime); localtime()                                   :2024-2025
 2. Daily log rotation       mday != LastLogDay      -> StartLog()         :2027-2028
                             mday != LastChatLogDay -> StartChatLog()      :2030-2031
                             mday != LastItemLogDay  -> StartItemLog()     :2033-2034
 3. if (MinCounter % 2 == 0)                                              :2036-2072
    a. NewbieEventServer rotation: NewbieServerID = (mday-1) % NumServerInGroup
    b. non-newbie servers: RemoveTrade() for every trading session, clear flag
    c. CastleServer = (wMod==0 && (wDay%2)==(ServerIndex%2))               :2062-2069
 4. Guild tax NPC persistence: for each GuildImpostoID -> ./npc/<name>     :2074-2104
 5. ForceWeekMode != -1 -> timeinfo->tm_wday = -1   (dead store)          :2107-2108
 6. CCastleZakum::ProcessMinTimer()                                        :2110
 7. Kingdom1Clear / Kingdom2Clear two-phase clear                          :2113-2133
 8. if (MinCounter % 10 == 0) memset(FailAccount)                          :2136-2137
 9. CurrentTime = timeGetTime()                                            :2139
10. Castle/keep gates re-lock over g_pInitItem[17..g_dwInitItem)           :2142-2209
11. NPC generator respawns by MinuteGenerate modulus + dungeon prize       :2212-2280
12. MinCounter++                                                           :2282
13. Weather roll (rand()%1200) or forced weather                           :2284-2311
14. GuildProcess()  -> castle / BR / colosseum / RvR / CWarTower           :2313
```

### 2.4 Shutdown flow

```
Menu "Sistema > Reboot"  -> ServerDown = 1                       Server.cpp:4093-4095
GM command "reboot"/timer -> ServerDown = -1*ServerIndex*(sec*2), clamped to 1 if >1 or < -1000
                                                                  imple.cpp:651-658
WSA_ACCEPT gate: ServerDown != -1000 -> send _NN_ServerReboot_Cant_Connect, CloseUser
                                                                  Server.cpp:4032-4039
WSA_READ gate:  ServerDown >= 120 -> ProcessClientMessage refuses  ProcessClientMessage.cpp:53
Tick: ServerDown in [1..119]
        ServerDown % 20 == 1  -> SendNotice(messageTable[ServerDown/20 + 17])
        ServerDown++  (once per 500 ms tick => ~60 s to reach 120)
        ServerDown == 120 -> SetTimer(TIMER_SEC, 200) and return  (no SecCounter++)
Tick: ServerDown == 120
        SendBilling2(zeros, 4) once; BILLING = 0
        loop UserCount: skip USER_EMPTY; first occupied slot -> CloseUser(); UserCount++; return
        UserCount >= MAX_USER -> Log("sys,server down complete") + DeleteObject(hFont)
                                + PostQuitMessage(NULL)
```

### 2.5 Data ownership and mutation map

The component writes the following state; the "reader" column lists the principal consumers, which is what makes this file's timing observable across the whole server.

| Data | Written at | Effect | Principal readers |
|------|-----------|--------|-------------------|
| `SecCounter` | `:123` | Global tick clock (0.5 s units) | `CCastleZakum.cpp:316`, `ProcessClientMessage.cpp:57`, `ProcessDBMessage.cpp:450`, `_MSG_CharacterLogin.cpp:97`, `Server.cpp:4306-4317` |
| `MinCounter` | `:2282` | Calendar tick clock (12 s units) | none outside this TU |
| `CurrentTime` | `:125`, `:2139`, `Server.cpp:2139` | ms timestamp used by socket/DB code | `CPSock`, DB paths |
| `ServerDown` | `:100`, `:111` | Shutdown state machine | `Server.cpp:4032`, `ProcessClientMessage.cpp:53` |
| `BillCounter` | `:132` | Billing retry countdown | none outside this TU |
| `SaveCount` | `:174`, `:183`, `:191`, `:197` | Round-robin save cursor | none outside this TU |
| `pUser[].ReqHp / ReqMp` | `:627-631` (+`ApplyHp/ApplyMp`) | Regen accumulator | combat, `SetReqHp` |
| `pMob[].MOB.CurrentScore.Hp/Mp` | via `ApplyHp`, `ApplyMp`, affect types 17/20, damage helpers | Vitals | whole server |
| `pMob[].Affect[]` | via `ProcessAffect` (duration decrement `Server.cpp:5377`) | Buff/debuff clock | combat, `SendScore` |
| `pItem[]`, `pItemGrid[][]` | `ProcessDecayItem` (`Server.cpp:8353`) | Ground item lifetime | pickup paths |
| `pMobMerc[].Stock[]` | `:1345` | Vendor restock | `_MSG_Buy` |
| `Pista[0..6].Party[].MobCount/LeaderID` | `:693-697`, `:746`, `:1025-1034`, `:1393`, `:1541`, `:1586` | Rune-track event state | `MobKilled`, `_MSG_Action`, `SendFunc` |
| `CartaTime`, `CartaSala` | `:1132-1153` | Secret-room progression | `MobKilled`, `_MSG_UseItem` |
| `WaterClear1[3][10]` | `:1116-1127` | Water-room auto-clear countdown | `_MSG_UseItem`, `MobKilled` |
| `KefraLive` | `:651` | One-shot weekly boss latch | boot spawn `Server.cpp:3638` |
| `Pista` reset block | `:1025-1034` | Clears the whole event record | event handlers |
| `BrState`, `BrMod`, `BrGrid` | via `GuildProcess` (`Server.cpp:6290-6291`) | Battle Royale phase | `SendPKInfo`, `_MSG_PKMode` |
| `CurrentWeather` | `:2288-2308` | Weather broadcast | `SendWeather` |
| `mNPCGen.pList[].MinuteGenerate` | `:2230`, `:2237`, `:2244`, `:2251` | Respawn re-randomisation | `GenerateMob` |
| `GuildImpostoID[]` NPC files | `:2096-2103` | Tax NPC persistence | `CReadFiles` |
| `FailAccount` | `:2137` (memset) | Login-failure window reset | `Server.cpp:1334` |
| `Kingdom1Clear/2Clear` | `:2115-2131` | Two-phase area clear | none outside this TU |
| `NewbieEventServer`, `CastleServer` | `:2041`, `:2056`, `:2067` | Server-role flags | `SendPKInfo`, `CWarTower`, `GuildProcess` |

### 2.6 Real durations (tick unit conversions)

`SecCounter` advances 2 per second; `MinCounter` advances 5 per minute. All "second/minute" semantics below are derived, not documented in code.

| Construct | Source line | Tick period | Real period |
|-----------|-------------|-------------|-------------|
| `SecCounter++` | `:123` | every tick | 0.5 s |
| `SecCounter % 2` | `:601`, `:642`, `:1101` | 2 ticks | 1 s |
| `SecCounter % 4` | `:1108`, `Sec4` | 4 ticks | 2 s |
| `SecCounter % 5` | `:1247` | 5 ticks | 2.5 s |
| `SecCounter % 6` | `:1234`, `Sec6` | 6 ticks | 3 s |
| `SecCounter % 8` | `:169` | 8 ticks | 4 s |
| `SecCounter % 12` | `:497` | 12 ticks | 6 s |
| `SecCounter % 16` / `% 32` | `:1174-1175` | 16 / 32 ticks | 8 s / 16 s per user |
| `SecCounter % 20` | `:621` | 20 ticks | 10 s |
| `SecCounter % 120` | `:539` | 120 ticks | 60 s |
| `SecCounter % 1200` | `:520` | 1200 ticks | 600 s (10 min) |
| `MinCounter++` | `:2282` | every min-tick | 12 s |
| `MinCounter % 2` | `:2036` | 2 min-ticks | 24 s |
| `MinCounter % 10` | `:2136` | 10 min-ticks | 120 s |
| `pMobMerc.RebornTime = value*2` | `Server.cpp:9347-9350` | 2×value ticks | value seconds (correct) |
| `MinuteGenerate` | `:2214-2224` | value min-ticks | value × 12 s |
| `WaterClear1 = 30 / 15` | `_MSG_UseItem.cpp:1735` | 30 / 15 quad-ticks | 60 s / 30 s |
| `CartaTime = 60` | `MobKilled.cpp:1094` | 60 quad-ticks | 120 s per room |
| `BillCounter = 360` | `Server.cpp:3738` | 360 ticks | 180 s |
| `CheckIdle` 720 / 1440 | `Server.cpp:4312-4315` | 720 / 1440 ticks | 360 s / 720 s |

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Scheduling | Two fixed-resolution Win32 timers (500 ms / 12,000 ms) with ids 0 and 1 dispatched from `MainWndProc` | `Server.cpp:3631-3632`, `:3723-3729`; `Basedef.h:136-137` |
| BR-02 | Scheduling | `SecCounter` is a half-second counter and `MinCounter` a 12-second counter despite their names | `ProcessSecMinTimer.cpp:123`, `:2282` |
| BR-03 | Scheduling | Fixed, hard-coded ordering of ~25 sub-passes inside one tick; early returns bypass the whole remainder | `:40-2017` |
| BR-04 | Scheduling | No accumulator/catch-up and no elapsed-time base: scheduling is pure modulus on a counter the tick itself increments | `:1108`, `:1174-1175`, `:127` |
| BR-05 | Scheduling | Per-subsystem periodicity map (2/4/5/6/8/12/16/20/32/120/1200) | whole file, see 2.6 |
| BR-06 | Scheduling | Round-robin phase staggering: user slot `i` runs when `i % N == SecCounter % N`; NPC slot `j` when `j % N == SecCounter % N` | `:1179-1189`, `:1353-1357`, `:1554-1557` |
| BR-07 | Concurrency | All ticks, socket reads, DB reads and disk writes serialize on one thread (`GetMessage` pump) | `Server.cpp:3650-3654`, `:3723-3729` |
| BR-08 | Lifecycle | Shutdown countdown state machine with a notice every 20 ticks and a tick-rate change to 200 ms at 120 | `:96-119` |
| BR-09 | Lifecycle | Shutdown drain closes exactly one occupied account per tick, then `PostQuitMessage`; counter clock frozen | `:42-94` |
| BR-10 | Resilience | Billing socket reconnect countdown, terminal failure disables billing (`BILLING = 0`) | `:130-167` |
| BR-11 | Persistence | Round-robin save: exactly one playable account saved per 8 ticks via a persistent cursor | `:169-199` |
| BR-12 | Billing | Child-account forced logout when `BILLING==2` and `Unk_2728==1` and level >= `FREEEXP` and the `g_Hour` window matches | `:178-186` |
| BR-13 | Transport | Per-tick outbound flush for every non-empty session; send failure disconnects that session | `:201-220` |
| BR-14 | Session | Idle timeout: 720 ticks (360 s) without receives closes the session, with a 1440-tick forgiveness reset | `Server.cpp:4304-4322` |
| BR-15 | Configuration | MAC block list is reloaded from disk on 119 of every 120 ticks (inverted guard) | `:127-128`, `CReadFiles.cpp:678-715` |
| BR-16 | Vitals | Passive regen accumulator adds `Level + 30` to `ReqHp` and `ReqMp` every 20 ticks | `:621-640` |
| BR-17 | Vitals | `ApplyHp`/`ApplyMp` move at most 2,000 points per call, clamp to `MaxHp/MaxMp`, cap at 2×10^9 | `:601-619`; `Server.cpp:8591-8657` |
| BR-18 | Affects | Affect tick: per-type effects, then `Time--`, then expiry cleanup and score refresh | `Server.cpp:4513-5435`, expiry `:5376-5400` |
| BR-19 | Zone policy | Celestial/SCelestial at level >= 180 is recalled out of the three nightmare-zone rectangles | `:226-231` |
| BR-20 | Zone policy | Five quest rectangles recall players whose `QuestFlag` does not authorize them and whose level < 1000 | `:1191-1209` |
| BR-21 | Zone policy | 10-minute sweep clears nine quest rectangles and one event area, and resets `QuestFlag` for every player | `:520-538` |
| BR-22 | Economy | Premium/dated equipment expires on a 60-second check; backpack capacity is recomputed | `:539-599` |
| BR-23 | Event | Kefra world boss chain spawns once at Tuesday 12:00:00-12:00:02 and latches `KefraLive` | `:649-658` |
| BR-24 | Event | Nightmare arena resets at fixed minute/second boundaries for A/M/N instances | `:660-687` |
| BR-25 | Event | Nightmare arenas start only when players are physically inside the entry area and the exact minute/second matches | `:1040-1097` |
| BR-26 | Event | Rune-track entry at minutes 0/20/40: leader validation, zone check, party teleport, per-stage spawns | `:690-755` |
| BR-27 | Event | Rune-track exit at minutes 15/35/55: mob purge, player teleport/revive, then full event-state reset | `:758-792`, `:1025-1036` |
| BR-28 | Event | Rune-track winner resolution by `MobCount` majority, with per-member rune and leader prize | `:793-1023` |
| BR-29 | Event | Balrog rune-track portals: ticket item 4032 consumed, random branch table routes the party | `:233-494` |
| BR-30 | Event | Secret-room remaining-mob counters broadcast to four card rooms, the rune track and RvR | `:497-518` |
| BR-31 | Event | Secret-room (Carta) progression across 4 rooms, 120 s each, ending with a full clear | `:1132-1170` |
| BR-32 | Event | Water-scroll rooms auto-clear after 60 s (30 s for boss rooms) with radius 8 or 12 | `:1112-1130` |
| BR-33 | Event | Ranking/arena engine runs on a 2-second cadence with a countdown, survivor counting and visual buckets | `:1108-1110`; `Server.cpp:7847-8167` |
| BR-34 | Event | Battle Royale environmental damage when `BrState == 2`, gated by `BRItem > 0` and a 16-tick phase | `:1212-1231` |
| BR-35 | Event | Kingdom tower zones deal 10 % max-HP damage every 6 ticks to players of the opposing clan | `:1234-1245`; `Server.cpp:5491-5546` |
| BR-36 | Event | Leader-room zones deal 2,000 flat damage every 5 ticks unless a rune-track leader is inside | `:1247-1337`; `Server.cpp:5548-5639` |
| BR-37 | NPC | Mercenary stock renewal and reborn timers are read from `MobMerc.txt` and converted to ticks | `:1339-1349`; `Server.cpp:9346-9350` |
| BR-38 | NPC | Idle/peace NPC pass processes 1/6 of the NPC slot range per tick and purges zero-HP NPCs | `:1353-1398` |
| BR-39 | NPC | `StandingByProcessor` returns a bit mask that the tick loop interprets (delete, move, flee-say, roam, teleport, boss-chamber count, engage) | `CMob.cpp:56-207`; `:1406-1548` |
| BR-40 | NPC | Level-6 boss chamber kills award rune progress and delete the mob on contact | `:1388-1398`, `:1581-1591` |
| BR-41 | Combat | Combat pass processes 1/4 of the NPC slot range per tick and resolves target validity, guild/clan relations and aggro | `:1557-1627` |
| BR-42 | Combat | Mob attack resolution applies parry, mount damage reduction, mana-shield conversion and fada damage division | `:1629-1942` |
| BR-43 | Combat | Kefra uses an area-attack path for skills 109/110/111 reached through a `goto` inside dead code | `:1715-1717`, `:1944-1953` |
| BR-44 | Combat | Successful damage propagates battle state to both parties and dispatches `MobKilled` at zero HP | `:1770-1941` |
| BR-45 | Persistence | Daily rotation of main, chat and item log files by day-of-month comparison | `:2027-2034` |
| BR-46 | Event | Newbie-event server and castle-host server rotate by day-of-month and week parity | `:2036-2072` |
| BR-47 | Persistence | Guild tax NPC records are rewritten to `./npc/<name>` every min-tick | `:2074-2104` |
| BR-48 | Event | Kingdom clear two-phase and `FailAccount` reset cadences | `:2113-2137` |
| BR-49 | World | Castle/keep gates are re-locked from the init-item table, one tick of delay per gate | `:2142-2209` |
| BR-50 | NPC | NPC generator respawn uses `MinCounter % MinuteGenerate` with dynamic re-randomisation bands 500/1000/2000/3800 | `:2212-2280` |
| BR-51 | World | Weather is rolled from `rand()%1200` into three bands, or forced by a GM command | `:2284-2311` |
| BR-52 | Event | `GuildProcess()` is the calendar engine for castle, BR, Colosseum, RvR and `CWarTower` | `:2313`; `Server.cpp:6203-6660`; `CWarTower.cpp:42-108` |
| BR-53 | Event | `CCastleZakum::ProcessSecTimer/ProcessMinTimer` are tick delegates with their own countdown/clear phases | `:1172`, `:2110`; `CCastleZakum.cpp:314-375` |
| BR-54 | Configuration | `ForceWeekMode` writes `timeinfo->tm_wday = -1` on a stack copy that is never read (inoperative override) | `:2107-2108` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 - Dual fixed-resolution timer registration and dispatch

**Overview:**
Two Win32 timers are created once in `WinMain` and are the only periodic drivers in TMSrv: `TIMER_SEC` (id 0) at 500 ms (`Code/TMSrv/Server.cpp:3631`) and `TIMER_MIN` (id 1) at 12,000 ms (`:3632`). `MainWndProc` maps them to `ProcessSecTimer()` and `ProcessMinTimer()` (`:3723-3729`).

**Detailed description:**
The registration happens after configuration, guild, castle-quest and socket initialisation, and immediately before `ListenSocket.StartListen` (`:3634`), which means the world is already fully materialised (heightmap, NPC generator list, init items, Kefra chain) when the first tick fires. The timer IDs are compile-time constants `0` and `1` (`Code/Basedef.h:136-137`), so the dispatch is a simple `wParam` comparison and there is no dynamic timer creation anywhere else in the component - a single, non-parameterised clock hierarchy.

Because these are Win32 window timers rather than multimedia timers, their accuracy is bounded by `USER_TIMER_MINIMUM` (10 ms) and, more importantly, by message-queue pressure: `WM_TIMER` is generated only when the queue is otherwise empty of higher-priority messages. In a server whose queue also carries `WSA_READ` for every client, this makes the effective tick period a *lower bound*. Nothing in the component measures lateness, so the design implicitly accepts that "every 500 ms" means "at most once per 500 ms, when convenient".

The practical consequence for reviewers is that the two timers do not form a stable 24:1 ratio. `TIMER_MIN` is 24 × `TIMER_SEC` nominally, but because each is independently coalesced, the ratio drifts and `MinCounter` has no fixed relationship with `SecCounter`. Any cross-timer assumption (for example "one min-tick is exactly 24 sec-ticks") is not guaranteed by the code.

**Rule workflow:**
```
WinMain:
  ... full world init ...
  SetTimer(hWndMain, TIMER_SEC, 500, NULL)
  SetTimer(hWndMain, TIMER_MIN, 12000, NULL)
  StartListen(...)
  GetMessage/DispatchMessage loop

MainWndProc(WM_TIMER):
  wParam == TIMER_SEC -> ProcessSecTimer()
  wParam == TIMER_MIN -> ProcessMinTimer()
```
*Confidence: HIGH.*

---

### Business Rule: BR-02 - Counter unit semantics (`SecCounter` = 0.5 s, `MinCounter` = 12 s)

**Overview:**
`SecCounter` increments once per `ProcessSecTimer` call (`:123`) and `MinCounter` once per `ProcessMinTimer` call (`:2282`). With 500 ms and 12,000 ms tick periods, the two globals advance at 2 Hz and 0.0833 Hz respectively.

**Detailed description:**
The naming is inherited from an earlier revision in which the logic timer and the calendar timer were presumably 1 s and 60 s; the periods were shortened (or the ids renamed) without renaming the counters or re-scaling the constants. Every constant expressed "in seconds" in this component is therefore consumed in half-second units, and every constant expressed "in minutes" in `MinCounter` units is consumed in 12-second units. Section 2.6 tabulates the resulting real durations; the most consequential are: idle disconnect after 360 s instead of 720 s, billing retry after 180 s instead of 360 s, and `MinuteGenerate` meaning "N × 12 s" instead of "N minutes".

The mismatch also leaks out of the component. `CheckIdle` (`Server.cpp:4304-4322`) stores `LastReceiveTime` as a `SecCounter` snapshot and compares against `SecCounter - 720`, so the constant is effectively hard-wired to a 2 Hz clock; `CCastleZakum::ProcessSecTimer` (`CCastleZakum.cpp:316`) uses `SecCounter % 2` as "1 second"; `ProcessDBMessage.cpp:450` and `_MSG_CharacterLogin.cpp:97` store `SecCounter` for a 10-tick window that is really 5 s. Renaming the counters or changing the `SetTimer` periods would silently rescale all of these rules, which makes the tick period a de-facto global constant of the whole server.

There is no compile-time guard, comment, or assertion linking the `SetTimer` period to any constant. The only textual hint is the comment `// two minutes untill the server closes` at `:42`, which contradicts the arithmetic (120 ticks × 500 ms = 60 s) and shows the units were already drifting in the author's mental model.

**Rule workflow:**
```
SetTimer(TIMER_SEC, 500)  ->  ProcessSecTimer  ->  SecCounter++   (2 per second)
SetTimer(TIMER_MIN, 12000)->  ProcessMinTimer  ->  MinCounter++   (5 per minute)
consumer: SecCounter % N == 0  =>  real period = N * 0.5 s
consumer: MinCounter % N == 0  =>  real period = N * 12 s
```
*Confidence: HIGH for the mechanism; HIGH for the consequences listed in 2.6.*

---

### Business Rule: BR-03 - Fixed tick ordering with early returns

**Overview:**
`ProcessSecTimer` is not a set of independent handlers: it is one straight-line body with ~25 ordered passes (Section 2.2). Several branches `return` early, which suppresses *all* later passes for that tick.

**Detailed description:**
Three early exits exist. The first is the shutdown drain (`:42-94`), which returns before `SecCounter++` and therefore freezes every downstream modulus. The second is `ServerDown == 120` inside the countdown (`:113-118`), which returns after re-arming the timer at 200 ms - again before the counter increment. The third and most subtle is inside the NPC loop: `continue` statements (for example `:1370`, `:1383`, `:1397`, `:1470`, `:1545`) skip only the current slot, not the tick, so those are benign.

Ordering creates real coupling. The vitals accumulator (pass 14) writes `ReqHp/ReqMp`; the vitals application (pass 13) runs *before* it in the same tick, so a freshly accumulated request is only materialised on the following even tick. The rune-track exit pass (15d) reads `Pista[].MobCount` that the NPC passes (24, 25) update later in the same tick, so a mob killed in the tick after the exit window still increments `MobCount` for the *next* cycle, not the resolved one. The calendar block (15) runs before `ProcessDecayItem` (16) and before the per-user pass (19), so an event that teleports a player (15c) can be followed in the same tick by `CheckIdle`/`RegenMob`/`ProcessAffect` on that player's new position.

Because the body is 1,978 lines with no sub-function decomposition, the ordering is implicit in the textual order and there is no place in the code that documents it. Any change to the order silently changes gameplay timing.

**Rule workflow:**
```
tick:
  if shutdown-drain  -> return (counter frozen)
  if countdown==120  -> re-arm 200 ms, return (counter frozen)
  counter++
  ... 25 passes in textual order ...
  pass 13 (apply HP/MP) precedes pass 14 (accumulate HP/MP)
  pass 15d (event resolution) precedes passes 24/25 (mob strafing)
```
*Confidence: HIGH (order read directly from source); MEDIUM on which orderings were intentional.*

---

### Business Rule: BR-04 - No drift compensation, no catch-up, no elapsed-time base

**Overview:**
All periodicity is `SecCounter % N == 0` / `MinCounter % N == 0`, evaluated against counters that the tick bodies themselves increment. There is no "next due time" variable, no accumulated-lateness term, and no re-entrancy guard.

**Detailed description:**
A monotonic scheduler typically keeps a "next run" timestamp and compares it against `timeGetTime()`; this component instead counts its own invocations. `timeGetTime()` is in fact called (`:125`, `:2139`) but only to populate the `CurrentTime` global consumed by socket/DB code - it is never used to decide whether a subsystem is due. Consequently, if the OS delivers 10 ticks in 6 seconds instead of 12, `SecCounter` advances 10 and every modulus fires proportionally less often: the *rate* of all timed behaviour is tied to message throughput, not to wall-clock.

Two behaviours follow. First, there is no spurious burst: a slow tick does not trigger a queue of missed moduli, because `% N == 0` matches at most once per increment. A dropped tick is simply lost forever. Second, a *slow* tick body lengthens the effective period of every later pass, which is why the component's cost profile is itself a gameplay parameter: adding expensive work into a tick (for example the per-mob `fopen` at `:1652`) stretches the interval between affect ticks and idle timeouts.

The one place where the code switches from modulus to absolute time is the calendar block (`:644-647`), which calls `time()`/`localtime()` and compares `tm_min`/`tm_sec`. That block is still wrapped in `SecCounter % 2 == 0`, so the absolute time is sampled only on even ticks; an event whose window is exactly one second wide can be missed when a tick is delayed.

**Rule workflow:**
```
every tick:  SecCounter++
period test: if (SecCounter % N == 0) -> run
no state:    nothing records "last run" or "lateness"
calendar:    if (SecCounter % 2 == 0) { time(); localtime(); compare tm_min/tm_sec }
missing tick -> modulus missed for that cycle, not replayed
```
*Confidence: HIGH.*

---

### Business Rule: BR-05 - Per-subsystem periodicity map

**Overview:**
The tick body assigns a distinct modulus to each subsystem; the map is the component's real "configuration". Table 2.6 is the authoritative conversion; this rule records the intent of each band.

**Detailed description:**
The bands are chosen by cost and by the precision the gameplay needs. Highest-frequency work is the cheapest: outbound socket flush and the per-user mob tick run every tick (`:201`), outbound traffic being the server's responsiveness budget. 1-second work (`% 2`) is vitals application and the calendar block. 2-second work (`% 4`) is the ranking engine, the water-room countdowns and the secret-room countdown. 2.5-second work (`% 5`) is the leader-room environmental damage; 3-second work (`% 6`) is kingdom damage plus the NPC strafe pass. 4-second work (`% 8`) is the round-robin save. 6-second work (`% 12`) is the secret-room broadcast. 8-second work (`% 16`) and 16-second work (`% 32`) are the per-user stagger. 10-second work (`% 20`) is the regen accumulator. 60-second work (`% 120`) is premium expiry; 600-second work (`% 1200`) is the quest sweep.

On the min-timer side the bands are `% 2` (24 s) for server role rotation, `% 10` (120 s) for the `FailAccount` window, and the per-generator `MinuteGenerate` modulus (multiples of 12 s). Two of the min-timer passes are unconditional: guild-tax persistence (`:2074`), castle/keep gate re-lock (`:2142`), NPC respawn (`:2212`), `MinCounter++` (`:2282`), weather (`:2284`) and `GuildProcess` (`:2313`) run on every min-tick, i.e. every 12 s.

The map is not documented anywhere and there is no single table in the code: the reader must scan 1,978 lines to recover it. Two entries are anomalous and are covered separately: the `% 120` MAC reload is inverted (BR-15) and the `% 2` calendar block contains second-precision event windows (BR-23 through BR-25).

**Rule workflow:**
```
per tick   : flush sockets, per-user mob tick, balrog portals, merc check
%2  (1 s)  : ApplyHp/ApplyMp, calendar events, ProcessDecayItem
%4  (2 s)  : ProcessRanking, water countdown, carta countdown, CastleZakum tick
%5  (2.5s) : leader-room damage
%6  (3 s)  : kingdom damage, NPC strafe (Sec6)
%8  (4 s)  : round-robin save
%12 (6 s)  : secret-room counters
%16 (8 s)  : per-user RegenMob + ProcessAffect
%20 (10 s) : ReqHp/ReqMp += Level+30
%32 (16 s) : per-user CheckIdle
%120 (60 s): premium item expiry     (MAC reload is inverted, see BR-15)
%1200(10 m): quest area sweep
min-tick   : gate re-lock, NPC respawn, weather, GuildProcess, tax NPC save
```
*Confidence: HIGH.*

---

### Business Rule: BR-06 - Round-robin phase staggering across slots

**Overview:**
Instead of iterating all 1,000 user slots and all 25,000 mob slots every tick, the component processes a rotating slice: a slot is eligible when slot-index `% N` equals `SecCounter % N`.

**Detailed description:**
For users the pattern is `if (i % 32 == Sec32) CheckIdle(i);` and `if (i % 16 == Sec16) { RegenMob(i); ProcessAffect(i); }` (`:1174-1189`). Because `SecCounter % 16` cycles through 0..15, each user index is visited exactly once every 16 ticks (8 s) for regen and once every 32 ticks (16 s) for idle checking. This is a classic time-sliced scheduler: it bounds per-tick cost to 1/16 and 1/32 of the table at the price of up-to-8-second quantisation of affect and regen ticks - a gameplay-visible property (a poison affect loses HP in 8-second steps per target, not continuously).

For NPCs the pattern uses an offset start rather than a per-index modulus: `int initial = Sec6 + MAX_USER; for (index = initial; index < MAX_MOB; index += 6)` (`:1356-1357`) and `for (index = Sec4 + MAX_USER; index < MAX_MOB; index += 4)` (`:1557`). Both give each NPC slot exactly one visit per 6 or 4 ticks (3 s / 2 s) but distribute the work evenly across ticks. Note that `Sec4` is computed twice (`:1106` and `:1554`); the first assignment is dead because between them the code tests `SecCounter % 4` directly, and the second is the one the combat loop uses.

The staggering has one correctness consequence worth flagging: `Sec16` is computed once per tick (`:1174`) and reused by the Battle Royale damage gate (`:1212`), where the loop variable `i` has already been exhausted (see BR-34).

**Rule workflow:**
```
Sec16 = SecCounter % 16 ; Sec32 = SecCounter % 32 ; Sec4 = SecCounter % 4 ; Sec6 = SecCounter % 6
for i in [1, MAX_USER):  i%32==Sec32 -> CheckIdle(i)
                         i%16==Sec16 -> RegenMob(i), ProcessAffect(i)
for index = Sec6+MAX_USER; < MAX_MOB; index += 6 : idle/peace pass
for index = Sec4+MAX_USER; < MAX_MOB; index += 4 : combat pass
=> each user every 16/32 ticks; each NPC every 6/4 ticks
```
*Confidence: HIGH.*

---

### Business Rule: BR-07 - Single-threaded serialization of ticks, sockets and I/O

**Overview:**
There is exactly one thread (`Server.cpp:3650-3654`). Timer bodies, client messages (`WSA_READ`), DB messages (`WSA_READDB`), billing messages (`WSA_READBILL`) and all disk I/O run sequentially on it.

**Detailed description:**
This is a structural rule rather than a coded one, but it is the most important constraint on this component: the tick body cannot block, cannot sleep, and cannot be preempted by socket traffic. Because the tick body performs synchronous file I/O (`CReadFiles::ReadMacblock` at `:128` opens and parses a file on 119 of every 120 ticks; `fopen("AttackDieTeste.txt","a+")` at `:1652` and `fopen("Gethalf.txt","a+")` at `:1674` inside the combat loop; `Log()` at `:1368`, `:1381`, `:1572`), a slow disk directly delays client packet processing. `Log()` is especially heavy: it formats a timestamp, `fprintf`s the log file and then calls `SetWindowText(hWndMain, ...)` (`Server.cpp:6722-6725`), a synchronous window-manager call, on every invocation.

The rule also means the component needs no locking and uses none - which is correct today and is the main invariant any future change must preserve. It equally means that any long-running work added to a tick (for example iterating all 25,000 NPC slots in one pass) degrades the entire server, including the network path, rather than just the timer.

Chapter-level evidence for "no threading primitives" is in the architecture report (`docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:21`); this analysis additionally confirmed that `ProcessSecMinTimer.cpp` contains no critical section, thread or mutex reference.

**Rule workflow:**
```
one thread:
  GetMessage -> [WSA_READ | WSA_READDB | WSA_READBILL | WM_TIMER] -> handler -> return
WM_TIMER handler runs to completion before any socket message is dequeued
=> tick duration is added latency for every client
```
*Confidence: HIGH.*

---

### Business Rule: BR-08 - Shutdown countdown state machine

**Overview:**
`ServerDown` is a tri-state machine: `-1000` = running, `1..119` = counting down with periodic notices, `120` = draining. The countdown advances one unit per 500 ms tick (`:96-119`).

**Detailed description:**
The machine is driven only by `ProcessSecTimer`. When `ServerDown` becomes positive (menu Reboot `Server.cpp:4094`, or the GM command at `imple.cpp:651-658`), each tick performs three steps: if `ServerDown % 20 == 1` it broadcasts a notice whose string index is `ServerDown/20 + 17`, so the six notices fire at 1, 21, 41, 61, 81 and 101 (message table rows 17..22 - a 6-entry warning ladder); then increments; then, when the value reaches exactly 120, re-arms `TIMER_SEC` at 200 ms and returns before `SecCounter++`.

Two properties are worth recording precisely. First, the countdown is *not* time-based: it takes 120 delivered ticks, so under load the "one minute" warning can stretch well beyond a minute. Second, the `200` ms re-arm is applied through `SetTimer(hWndMain, TIMER_SEC, 200, 0)` - the `lParam` timer procedure is `0`, which is consistent with the original `NULL`, so the WM_TIMER dispatch continues to work.

The `ServerDown <= 0` branch at `:98-103` (increment then `goto lbl_PST1`) is reachable only if some other module sets a negative-but-greater-than-`-1000` value; the GM command does exactly that (`-1 * ServerIndex * (sec*2)`), which staggers shutdowns across servers in a group. The `goto` jumps to `lbl_PST1` immediately before `SecCounter++`, so the counter still advances normally during staggered shutdown.

**Rule workflow:**
```
ServerDown = 1 (menu) or -Index*(sec*2) (GM command, clamped to [ -1000 .. 1 ])
tick:
  if ServerDown == 120 -> drain mode (BR-09)
  if ServerDown > -1000:
      if ServerDown <= 0: ServerDown++; goto counter++ path
      if ServerDown % 20 == 1: SendNotice(table[ServerDown/20 + 17])
      ServerDown++
      if ServerDown == 120: SetTimer(TIMER_SEC, 200); return
  counter++ ... normal body ...
```
*Confidence: HIGH.*

---

### Business Rule: BR-09 - Shutdown drain: one account per tick, then process exit

**Overview:**
At `ServerDown == 120` the tick body stops doing world work entirely and closes exactly one occupied account per tick, using the persistent cursor `UserCount`. When the cursor reaches `MAX_USER` the process posts a quit message (`:42-94`).

**Detailed description:**
The branch first sends one billing logout (`SendBilling2(zeros, 4)`) and disables billing (`BILLING = 0`) - guarded by `if (BILLING != 0)` so it happens once. It then enters `while (TRUE)`: if `UserCount >= MAX_USER` it logs `"sys,server down complete"`, deletes the log font, calls `PostQuitMessage(NULL)` and returns; if the slot at `UserCount` is `USER_EMPTY` it advances and continues; otherwise it breaks out. After the loop it truncates the account name, logs `"sys,save %d - %d - %s"`, calls `CloseUser(UserCount)` and advances the cursor.

`CloseUser` (`Server.cpp:6834`) is the full teardown path: it performs the mode-dependent DB save/no-save request, trade and party cleanup and socket close, so the drain is a graceful, one-account-at-a-time shutdown rather than a process kill. The cost is that a full 1,000-slot server needs up to 1,000 ticks; with the 200 ms re-arm that is ~200 s in the worst case, but the tick rate applies only while the timer keeps firing, and this branch `return`s before `SecCounter++`, so the world's timeline is frozen for the whole drain.

Two consequences are worth highlighting for operators. Because `SecCounter` is frozen, all other subsystems (affect expiry, idle timeout, item decay, calendar events) stop advancing during the drain even though `ProcessMinTimer` keeps firing on its own timer, which can produce inconsistent state if the drain is long (min-timer-driven `GuildProcess` continues to evaluate wall-clock conditions while sec-timer-driven state is frozen). And because `UserCount` is a global that is also the cursor used by the drain, a second invocation of the branch after an early `PostQuitMessage` failure would resume from where it stopped rather than restart.

**Rule workflow:**
```
if (ServerDown == 120):
    if (BILLING != 0) { SendBilling2(zeros,4); BILLING = 0; }
    loop:
        if UserCount >= MAX_USER: Log("sys,server down complete")
                                  DeleteObject(hFont); PostQuitMessage(NULL); return
        if pUser[UserCount].Mode == USER_EMPTY: UserCount++; continue
        break
    truncate AccountName; Log("sys,save ..."); CloseUser(UserCount); UserCount++
    return        (SecCounter NOT incremented)
```
*Confidence: HIGH.*

---

### Business Rule: BR-10 - Billing reconnect countdown with terminal disable

**Overview:**
When the billing socket is down and `BillCounter` is positive, `ProcessSecTimer` decrements it and, at zero, makes one reconnect attempt; failure sets `BILLING = 0` permanently (`:130-167`).

**Detailed description:**
The guard is a three-way conjunction: `BILLING != 0 && BillCounter > 0 && BillServerSocket.Sock == 0`. `BillCounter` is set to 360 by `MainWndProc` on every billing socket failure (`Server.cpp:3738`, `:3752`, `:3770`, `:3783`), so the retry delay is 360 ticks = 180 s, not the 360 s the constant suggests. When the counter reaches zero the component calls `BillServerSocket.ConnectBillServer(BillServerAddress, BillServerPort, LocalIP, WSA_READBILL)`; `ret == 0` is treated as failure, logs `"err,Reconnect BILL Server(x2) fail."` and sets `BILLING = 0` (billing permanently off for this process); success sends `_AUTH_GAME` with `SendBilling2(&Unk, 4)`.

The `else BillCounter = 0;` at `:166-167` means the countdown is abandoned as soon as billing is disabled or a socket is present, so a reconnect loop cannot spin. Timing of the two identical log lines (`"err,start reconnect BILL %d"` before and after the connect call, `:138` and `:147`) suggests the author used them as latency probes; they both fire on a single attempt, which is where the `(x2)` in the failure message originates even though only one attempt is made.

Note that `ProcessBILLMessage` (`Server.cpp:4300-4302`) is an empty function: billing responses are dequeued and discarded. This rule therefore only maintains connectivity, not authorisation.

**Rule workflow:**
```
billing socket error -> BillCounter = 360 (Server.cpp, four sites)
tick:
  if (BILLING != 0 && BillCounter > 0 && Sock == 0):
      BillCounter--
      if BillCounter <= 0:
          ret = ConnectBillServer(...)
          ret == 0 -> Log("Reconnect BILL Server(x2) fail."); BILLING = 0
          else     -> SendBilling2(_AUTH_GAME, 4)
  else: BillCounter = 0
```
*Confidence: HIGH.*

---

### Business Rule: BR-11 - Round-robin persistence: one account per 8 ticks

**Overview:**
Every 8th tick (4 s) exactly one playable account is saved, chosen by the persistent cursor `SaveCount`, which wraps inside the loop (`:169-199`).

**Detailed description:**
The outer test is `if (SecCounter % 8 == 0)`, then a loop over `i` in `[0, MAX_USER)` that *ignores* `i` for indexing: it uses `SaveCount` and advances it on every iteration, breaking as soon as it saves. The wrap `if (SaveCount >= MAX_USER) SaveCount = 1;` sits inside the loop, so the cursor cycles through slots 1..999 and never uses slot 0 (reserved null slot). The effect is "scan forward from the cursor to the next `USER_PLAY` slot with a live mob, save it, stop", which spreads 1,000 accounts over 8,000 ticks (4,000 s ≈ 66 min) in a full server - an important durability property: a crash can lose up to ~66 minutes of progress for a given account.

The save branch forks on billing: if `BILLING == 2 && pUser[SaveCount].Unk_2728 == 1 && pMob[SaveCount].MOB.CurrentScore.Level >= FREEEXP && (g_Hour <= 12 || g_Hour >= 19)` the account is forcibly logged out (`SendClientMessage(_NN_Child_Pay)` then `CharLogOut`) instead of saved. Otherwise `SaveUser(SaveCount, 0)` is called, which queues a `_MSG_DBSaveMob` containing score, cargo, coin, donate, slot, short skills, affects and extra (`Server.cpp:7056-7084`).

A defect worth recording: because the loop's counter `i` is unused, a full loop (no eligible user) still advances `SaveCount` by up to 1,000 positions per tick, which is harmless but obscures the intended round-robin; and `SaveCount` is a cursor, so a boot with all slots empty leaves it wherever the last scan stopped.

**Rule workflow:**
```
if (SecCounter % 8 == 0):
    for i in [0, MAX_USER):
        if SaveCount >= MAX_USER: SaveCount = 1
        if pUser[SaveCount].Mode == USER_PLAY and pMob[SaveCount].Mode != MOB_EMPTY:
            if billing child-hours gate (BR-12): CharLogOut(SaveCount)
            else: SaveUser(SaveCount, 0)
            SaveCount++
            break
        SaveCount++
```
*Confidence: HIGH.*

---

### Business Rule: BR-12 - Child-account forced logout gate (permanently satisfied)

**Overview:**
`BILLING == 2` (child/prepaid billing mode) plus the per-user child flag plus `Level >= FREEEXP` plus an hour window causes a forced logout instead of a save (`:178-186`).

**Detailed description:**
The composite predicate is `BILLING == 2 && pUser[SaveCount].Unk_2728 == 1 && pMob[SaveCount].MOB.CurrentScore.Level >= FREEEXP && (g_Hour <= 12 || g_Hour >= 19)`. The intent, read from the message used (`_NN_Child_Pay`), is "outside the child's allowed play hours, disconnect a paid-level character on a child account". `FREEEXP` comes from `gameconfig.txt` and represents the level at which an account is considered paid (`Server.cpp:895-922`, `:1363-1375`).

The hour window, however, is evaluated against `g_Hour`, which is defined as `int g_Hour = 0;` (`Server.cpp:288`) and is **never written anywhere in the repository** (verified by exhaustive search across `Code/TMSrv/*.cpp`). With `g_Hour == 0`, `(g_Hour <= 12 || g_Hour >= 19)` is always true, so the window clause is inert and the actual rule is "any child-flagged character at or above the free-experience level is logged out on its next scheduled save slot". The same dead variable is used by the login path (`_MSG_CharacterLogin.cpp:69`, `g_Hour > 7 && g_Hour < 23`) with the opposite polarity, so login admission and session eviction are both governed by a constant 0.

Practical impact: in `BILLING == 2` deployments the rule fires every 8 ticks against a different account, so a paid child account is evicted at most 4 s after its turn comes up in the round robin. In `BILLING != 2` deployments the rule is dead and normal saving applies.

**Rule workflow:**
```
on scheduled save of account S:
  if BILLING == 2 and Unk_2728 == 1 and Level >= FREEEXP and (g_Hour <= 12 or g_Hour >= 19):
      SendClientMessage(S, _NN_Child_Pay); CharLogOut(S)
  else:
      SaveUser(S, 0)
g_Hour is never assigned -> condition reduces to BILLING==2 && flag && level>=FREEEXP
```
*Confidence: HIGH for the code path; HIGH that `g_Hour` has no writer (grep evidence).*

---

### Business Rule: BR-13 - Per-tick outbound flush and disconnect on send failure

**Overview:**
Every tick, each non-empty session with pending bytes is flushed via `cSock.SendMessageA()`; a failure logs the buffer positions and calls `CloseUser` (`:201-220`).

**Detailed description:**
The loop runs `for (int bb = 1; bb < MAX_USER; bb++)` and gates on `pUser[bb].Mode` (any non-zero mode, not only `USER_PLAY`), then on `cSock.nSendPosition` being non-zero. `SendMessageA` (`Code/CPSock.cpp:617-666`) validates socket and buffer positions, refreshes the send buffer when a partial send occurred, performs a single `send()` and returns `FALSE` on any inconsistency, on `SOCKET_ERROR`, or when `Sock <= 0`. A `FALSE` results in a log line that prints the slot, socket, `nSendPosition` and `nSentPosition`, and then `CloseUser(bb)`.

This is the server's only outbound flush point for *buffered* traffic: handlers call `AddMessage` and rely on this tick to actually transmit (or pass `bSend` to helpers). It therefore fixes the minimum outbound latency at one tick (≤500 ms) and is the reason a tick stall is immediately visible to clients as latency, not just as a missed game event.

The same loop also performs two unrelated jobs (the per-user mob tick and the nightmare-zone recall, BR-19), so a send failure that closes the session still allows the later blocks in the same iteration to touch that slot; `CloseUser` sets the mode to empty, and the subsequent calls are guarded by `pUser[bb].Mode == USER_PLAY`, so the ordering is safe but not obvious.

**Rule workflow:**
```
for bb in [1, MAX_USER):
    if pUser[bb].Mode and pUser[bb].cSock.nSendPosition:
        if not cSock.SendMessageA():
            Log("err,send fail close ..."); CloseUser(bb)
    if pUser[bb].Mode == USER_PLAY: pMob[bb].ProcessorSecTimer()   (empty body)
    nightmare-zone Celestial recall check (BR-19)
```
*Confidence: HIGH.*

---

### Business Rule: BR-14 - Idle timeout with a forgiveness band

**Overview:**
`CheckIdle` (`Server.cpp:4304-4322`) closes a session when its last receive stamp is older than 720 ticks, but first rewrites the stamp when the difference exceeds 1440 ticks or is negative.

**Detailed description:**
`ser = SecCounter`, `lst = pUser[conn].LastReceiveTime`. If `lst > ser` (stamp in the future - possible after a counter wrap or a restored session) the stamp is reset. If `lst < ser - 1440` the stamp is reset, which *forgives* an idle period between 720 and 1440 ticks: the session is not closed, it is re-stamped and survives. Only when `720 <= ser - lst <= 1440` does it disconnect, logging `"sys,disconnect last:%d server:%d mode:%d conn:%d"`.

The rule is executed for exactly one thirty-second of the user table per tick (`i % 32 == Sec32`, BR-06), so the effective detection latency is up to 16 s beyond the nominal threshold. `LastReceiveTime` is written by the receive path on every client message (`ProcessClientMessage.cpp:57`, `Server.cpp:4310-4313`), which makes this a genuine "no input at all" timeout, not an AFK detector.

Because `SecCounter` is a half-second counter, the practical thresholds are 360 s (disconnect) and 720 s (forgiveness). The forgiveness band means a bot that sends one packet every 11 minutes persists indefinitely, while a player who sends nothing for 6 minutes is dropped at the next slice - an asymmetry worth noting when evaluating anti-idle behaviour.

**Rule workflow:**
```
CheckIdle(conn) [every 32 ticks per slot]:
   ser = SecCounter ; lst = LastReceiveTime
   if lst > ser      -> LastReceiveTime = SecCounter
   if lst < ser-1440 -> LastReceiveTime = SecCounter      (forgive)
   if lst < ser-720  -> Log + CloseUser(conn)             (band 720..1440 only)
```
*Confidence: HIGH.*

---

### Business Rule: BR-15 - MAC block list reload on an inverted guard

**Overview:**
`if(SecCounter % 120) CReadFiles::ReadMacblock();` (`:127-128`) reloads the MAC block list on every tick whose counter is **not** a multiple of 120, i.e. 119 of every 120 ticks.

**Detailed description:**
`CReadFiles::ReadMacblock` (`Code/TMSrv/CReadFiles.cpp:678-715`) opens `MAC_BLOCK_PATH`, reads line by line, replaces dots with spaces, `sscanf`s an index and four octets and writes `pMac[idx].Mac[0..3]` for indices in `[0, MAX_MAC=200)`. It closes the file at the end. There is no dirty flag, no `stat` comparison and no caching: it unconditionally re-reads and re-parses.

The intent of the guard is clearly the opposite of what is coded - the sibling pattern in the same file (`SecCounter % 120 == 0` at `:539`, `% 1200 == 0` at `:520`) shows the author's convention. As written, the component performs two file opens and up to 200 `sscanf` calls **per second** on the server's only thread, forever, whether or not the file changed. Because a missing file is handled gracefully (`fopen` NULL -> return), the cost on a deployment without the file is one failed `fopen` per tick, which is still a syscall on the hot path.

This is the highest-frequency I/O in the server and the clearest evidence that tick bodies are not profiled. It also means the MAC list is effectively always current (a benefit), so removing the inversion without adding a reload trigger would change behaviour.

**Rule workflow:**
```
tick:
  SecCounter++
  if (SecCounter % 120 != 0):        # truthy remainder => almost always
      fopen(MAC_BLOCK_PATH) -> parse lines -> pMac[idx] = {a,b,c,d} -> fclose
```
*Confidence: HIGH.*

---

### Business Rule: BR-16 - Passive regeneration accumulator

**Overview:**
Every 20 ticks (10 s) each living player's `ReqHp` and `ReqMp` are increased by `BaseScore.Level + 30` (`:621-640`).

**Detailed description:**
The pass runs over `i` in `[1, MAX_USER)` and requires `pUser[i].Mode == USER_PLAY && pMob[i].MOB.CurrentScore.Hp > 0`. It adds `pMob[i].MOB.BaseScore.Level + 30` to both request counters, then immediately calls `ApplyHp(i)` and `ApplyMp(i)`; if HP was applied it sends `SendScore(i)`, otherwise if MP was applied it sends `SendSetHpMp(i)`.

The two-counter design (`ReqHp` as "requested/accumulated HP", `CurrentScore.Hp` as "materialised HP") exists so that regeneration, potions, poison and environmental damage can all express themselves as deltas against a single accumulator, with `ApplyHp` bounding how fast the materialised value may catch up (BR-17). `BaseScore` (not `CurrentScore`) is used for the level, so buffs/debuffs that change `CurrentScore.Level` do not change regen.

Because both HP and MP receive the same increment and `ApplyHp` is called first, a player at full HP still regenerates MP; the `else if` only affects which packet is sent. `SetReqHp`/`SetReqMp` are called from `ProcessAffect` (`Server.cpp:5407-5408`) to keep the accumulator consistent whenever the materialised value is rewritten directly, which is why the two representations usually agree.

**Rule workflow:**
```
if (SecCounter % 20 == 0):
    for i in [1, MAX_USER):
        if USER_PLAY and Hp > 0:
            ReqHp += BaseScore.Level + 30
            ReqMp += BaseScore.Level + 30
            if ApplyHp(i) != 0: SendScore(i)
            elif ApplyMp(i) != 0: SendSetHpMp(i)
```
*Confidence: HIGH.*

---

### Business Rule: BR-17 - Bounded vitals application

**Overview:**
`ApplyHp`/`ApplyMp` (`Server.cpp:8591-8657`) clamp the request to `MaxHp/MaxMp`, move at most 2,000 points per call, cap the absolute value at 2×10^9 and return whether anything changed. The 2-second vitals pass (`:601-619`) additionally zeroes `pUser[i].Unk_2688` before applying.

**Detailed description:**
The function first clamps `ReqHp` down to `MaxHp`. If the request exceeds the materialised HP it computes `hpdif = ReqHp - Hp`, caps it at 2,000, adds it, caps the result at 2,000,000,000 and finally clamps back down to `ReqHp`, returning `TRUE`. If the request is *below* the materialised value it returns `FALSE` and does nothing - healing is one-directional; reductions are performed by writing `CurrentScore.Hp` directly (damage paths, environmental damage) and then calling `SetReqHp` to re-synchronise the accumulator.

The 2,000-per-call bound interacts with the 2-second cadence (BR-05) and the 10-second accumulator (BR-16): a level-1 player requesting 31 HP per accumulator tick is applied instantly, while a level-370 player requesting 400 per accumulator is also applied instantly, but a large potion request above 2,000 is spread over calls. Because the vitals pass runs on `SecCounter % 2`, the worst-case latency between "request registered" and "HP visible" is one tick for the accumulator path and up to two ticks when the accumulator lands on an odd tick.

`Unk_2688 = 0` is written before each application (`:607`); the field is not documented anywhere and has no other writer in the component, so its meaning could not be established (information ambiguity, MEDIUM confidence that it is a "HP changed since last broadcast" latch).

**Rule workflow:**
```
ApplyHp(conn):
   if ReqHp > MaxHp: ReqHp = MaxHp
   if ReqHp > Hp:  dif = min(ReqHp - Hp, 2000); Hp = min(Hp + dif, 2e9); Hp = min(Hp, ReqHp); return TRUE
   return FALSE
tick % 2: Unk_2688 = 0; ApplyHp; ApplyMp; SendScore or SendSetHpMp
```
*Confidence: HIGH for mechanics; LOW for `Unk_2688` semantics.*

---

### Business Rule: BR-18 - Affect (buff/debuff) ticking and expiry

**Overview:**
`ProcessAffect` (`Server.cpp:4513-5435`) is invoked for players every 16 ticks and for NPCs on their combat slice. It applies per-type effects, decrements each affect's `Time`, and on expiry clears the slot, optionally changing the face (type 16) or deleting a summon (type 24).

**Detailed description:**
The function begins with the "fada" (fairy) auto-attack path: for players whose equip slot 13 is item 753, 769 or 1726 with a current target, it validates map attribute (`0x40`), PK mode, guild/clan relations and range (`HALFGRIDX`), then synthesises a `MSG_AttackOne` with skill 32/34/36 and re-enters the normal attack pipeline through `ProcessClientMessage(idx, ..., TRUE)`. This is the only place in the component where a *player* is made to attack automatically.

The per-affect loop then handles type 17 (HP aura: `Hp += Level/2 + Value`, clamped to `[1, MaxHp]`, `regen = 1`), type 20 (poison/drain: percentage loss with a `Perda` floor of 1,000 for players or `10*(100 - LearnedSkill/MAX_USER)` for NPCs; drains HP toward 1, adjusts `ReqHp`, and mirrors the loss into a mount via `LinkMountHp`), type 22 (thunder, with a full area-damage branch) and type 23. Every path funnels through the `LessAffect` label, which decrements `Time` unless it is already at the sentinel `32400000`, and on `Time <= 0` clears `Type/Time/Value/Level`, sets `upScore`, records a face change for type 16 and deletes summon-type NPCs.

Finally the function re-computes and broadcasts: `SetReqHp`/`SetReqMp` for players when `regen`, a `MSG_SetHpDam` `GridMulticast` with the delta, `GetCurrentScore` + `SendScore` when `upScore`, and `SendEquip` on a face change. Because it is called on a 16-tick slice (8 s), an affect with `Time = 10` lasts up to 80 s of wall clock; affect durations are therefore expressed in "affect ticks" whose length depends on the slice, not in seconds.

**Rule workflow:**
```
ProcessAffect(idx):
   fada path (item 753/769/1726, target, map 0x40, guild check, range) -> synthetic attack
   for i in [0, MAX_AFFECT):
       Type == 0 -> skip
       Type 17 -> Hp += Level/2 + Value, clamp, regen = 1
       Type 20 -> percentage drain, floor 1, ReqHp += delta, mount link, regen = 1
       Type 22 -> thunder area damage
       LessAffect: if Time < 32400000: Time--
                   if Time <= 0: type 16 -> face change; type 24 && NPC -> DeleteMob
                                 clear slot; upScore = 1
   if regen: SetReqHp/SetReqMp + GridMulticast(MSG_SetHpDam)
   if upScore: GetCurrentScore + SendScore (+ SendEquip if face changed)
```
*Confidence: HIGH for types 17/20 expiry; MEDIUM for the full extent of types 22/23 (long, decompiled control flow).*

---

### Business Rule: BR-19 - Nightmare-zone recall for high-level Celestial classes

**Overview:**
Any slot whose mob coordinates fall in one of three nightmare rectangles and whose class is `CELESTIAL`, `CELESTIALCS` or `SCELESTIAL` with level >= 180 is recalled to its bind point (`:226-231`).

**Detailed description:**
The zone predicate is `(TargetX/128 == 9 && TargetY/128 == 1) || (TargetX/128 == 8 && TargetY/128 == 2) || (TargetX/128 == 10 && TargetY/128 == 2)` - i.e. the 128×128 "sector" coordinates of the three nightmare instances (A, M, N), which matches the `MapaMulticast(9,1)`, `(8,2)`, `(10,2)` arguments used by the nightmare scheduler (`:1050`, `:1070`, `:1089`). `DoRecall` (`Server.cpp:7558-7602`) computes the destination from `g_pGuildZone[(Merchant & 0xC0) >> 6]` (city), overrides it with the guild's castle spawn when the player's guild owns a zone, and overrides it again with the newbie start (2112, 2042) for `MORTAL` characters below `FREEEXP`.

The check runs inside the per-user loop but **outside** any `pUser[bb].Mode == USER_PLAY` guard, so it is evaluated for empty slots as well. For a slot that has never been occupied, `TargetX/TargetY/ClassMaster/Level` are zero and the predicate is false, which masks the issue; for a slot vacated while standing in a nightmare instance, the stale fields can still satisfy the predicate, and `DoRecall` then runs with no session (it guards only the packet send, `Server.cpp:7598-7599`). This is a latent, low-frequency defect rather than a visible one, but it is evidence that the loop's preconditions are implicit.

Class-based rather than level-based gating means a level-180 Arch or Mortal character may enter, while an equivalent Celestial cannot; that asymmetry is a game-design choice recorded here because it is not commented in the source.

**Rule workflow:**
```
for bb in [1, MAX_USER):
    if pUser[bb].Mode == USER_PLAY: pMob[bb].ProcessorSecTimer()
    if sector(TargetX,TargetY) in {(9,1),(8,2),(10,2)}
       and ClassMaster in {CELESTIAL, CELESTIALCS, SCELESTIAL}
       and Level >= 180:
           DoRecall(bb)
```
*Confidence: HIGH.*

---

### Business Rule: BR-20 - Quest-zone recall guards

**Overview:**
Five quest rectangles recall any slot standing inside them unless the corresponding `QuestFlag` value authorizes it and the base level is below 1000 (`:1191-1209`).

**Detailed description:**
The five zones are the gravedigger (2379..2426 × 2076..2133), the garden/carbuncle (2228..2257 × 1700..1728), Kaizen (459..497 × 3887..3916), the hydras (658..703 × 3728..3762) and the elves (1312..1348 × 4027..4055). Each test requires `QuestFlag != N` **and** all four coordinate bounds **and** `BaseScore.Level < 1000`; if all hold, `DoRecall(i)` moves the slot out. The level-1000 ceiling exempts NPC-like/admin slots from the sweep.

`QuestFlag` is a per-mob integer that quest handlers set when a player is authorized to be in the area (`_MSG_Quest.cpp` sets it for the corresponding quest stage) and that the 10-minute sweep clears (BR-21). This makes the rule a "membership token" model: being inside without a valid flag of the right value is equivalent to trespassing.

The pass is inside the same per-user loop as `CheckIdle`/`RegenMob` but executes unconditionally (no `USER_PLAY` guard, no HP check), once per tick for all 1,000 slots - the only per-user work in the component that is not staggered. It is cheap (five integer comparisons per slot) but it is fully executed 2× per second, which is the largest per-tick user-table scan in the file.

**Rule workflow:**
```
for i in [1, MAX_USER):
    if QuestFlag != 1 and inside(2379,2076,2426,2133) and Level < 1000: DoRecall(i)
    if QuestFlag != 2 and inside(2228,1700,2257,1728) and Level < 1000: DoRecall(i)
    if QuestFlag != 3 and inside(459,3887,497,3916)   and Level < 1000: DoRecall(i)
    if QuestFlag != 4 and inside(658,3728,703,3762)   and Level < 1000: DoRecall(i)
    if QuestFlag != 5 and inside(1312,4027,1348,4055) and Level < 1000: DoRecall(i)
```
*Confidence: HIGH.*

---

### Business Rule: BR-21 - Ten-minute quest sweep and flag reset

**Overview:**
Every 1200 ticks (600 s) nine quest rectangles are force-cleared and every player's `QuestFlag` is reset to 0 (`:520-538`).

**Detailed description:**
The pass calls `ClearAreaQuest` on nine rectangles - gravedigger, green cape, "reset habilidades" (two separate areas), Carbuncle, Kaizen, hydras, elves and the gargoyle quest - and `ClearArea` (without quest semantics) on the Lanhouse area (3570..3965 × 3446..3711). `ClearAreaQuest` (`Server.cpp:5730-5752`) recalls `USER_PLAY` slots inside the rectangle after reviving dead ones to 2 HP, and additionally zeroes `QuestFlag` for each victim; `ClearArea` is the same without the flag write.

The second half iterates all slots and clears `QuestFlag` for every `USER_PLAY` session, unconditionally. Combined with BR-20, this means a player who legitimately stands in a quest area has at most ten minutes of authorized presence per sweep, after which the flag is cleared and the recall guard fires within one tick (0.5 s). The `Log` calls inside `ClearArea*` are absent, so the sweep is silent; the only observable effect is teleportation of all affected players.

The 10-minute period is itself a tick-derived value: 1200 ticks at the *nominal* 500 ms is 600 s, but because the counter only advances on delivered ticks, the sweep is late under load. Nothing in the function records when it last ran, so the interval is "1200 ticks", not "ten minutes".

**Rule workflow:**
```
if (SecCounter % 1200 == 0):
    ClearAreaQuest(2379,2076,2426,2133)  // gravedigger
    ClearAreaQuest(2232,1564,2263,1592)  // green cape
    ClearAreaQuest(2640,1966,2670,2004)  // skill reset
    ClearAreaQuest(2228,1700,2257,1728)  // carbuncle
    ClearAreaQuest(1950,1586,1988,1614)  // skill reset
    ClearAreaQuest(459,3887,497,3916)    // Kaizen
    ClearAreaQuest(658,3728,703,3762)    // hydras
    ClearAreaQuest(1312,4027,1348,4055)  // elves
    ClearAreaQuest(793,4046,827,4080)    // gargoyle
    ClearArea(3570,3446,3965,3711)       // Lanhouse
    for x in [1, MAX_USER): if QuestFlag and USER_PLAY: QuestFlag = 0
```
*Confidence: HIGH.*

---

### Business Rule: BR-22 - Premium item expiry and backpack capacity recomputation

**Overview:**
Every 120 ticks (60 s), four dated equipment slots and two dated backpack slots are checked; expired items are cleared and sent to the client, and `MaxCarry` is recomputed for the two backpack slots (`:539-599`).

**Detailed description:**
The pass iterates all slots and skips anything that is not `USER_PLAY`. It then applies four independent checks: equip slot 14 for item indices 3980..3989, equip slot 12 for 4150..4188, equip slot 13 for 3900..3913 (fairy - time-checked but not cleared), and carry slots 60/61 for item 3467 (capacity tokens). The first two log `"etc,premium item end %d"`, call `BASE_ClearItem`, and push the now-empty slot with `SendItem(user, ITEM_PLACE_EQUIP, slot, ...)`.

The fairy branch differs: it calls `BASE_CheckFairyDate(&Equip[13])` (which presumably zeroes the item internally when the date passed), then *always* sends the slot and recomputes the character score with `pMob[user].GetCurrentScore(user)`. That unconditional re-send means every player with a fairy-equipped slot gets an item packet and a score recomputation once per minute, which is a deliberate (if chatty) way to keep the fairy buff in sync.

The carry slots implement a capacity token: when a token in slot 60 or 61 expires, `MaxCarry` is reset to 30 and then increased by 15 for each of the two slots that still holds item 3467. Because the code clears the slot and then re-tests the same slot, the just-cleared slot can never contribute, so `MaxCarry` correctly drops by 15 for that slot. The base value 30 and increments of 15 are hard-coded and are the only place backpack capacity is derived from items in this component.

**Rule workflow:**
```
if (SecCounter % 120 == 0):
    for user in [0, MAX_USER):
        if not USER_PLAY: continue
        if Equip[14].sIndex in [3980..3989] and BASE_CheckItemDate: log, clear, SendItem slot 14
        if Equip[12].sIndex in [4150..4188] and BASE_CheckItemDate: log, clear, SendItem slot 12
        if Equip[13].sIndex in [3900..3913]: BASE_CheckFairyDate; SendItem slot 13; GetCurrentScore
        if Carry[60].sIndex == 3467 and expired: clear; MaxCarry = 30 + 15*(surviving tokens)
        if Carry[61].sIndex == 3467 and expired: clear; MaxCarry = 30 + 15*(surviving tokens)
```
*Confidence: HIGH for mechanics; MEDIUM for the exact effect of `BASE_CheckFairyDate` (external to this component).*

---

### Business Rule: BR-23 - Kefra weekly world-boss spawn (Tuesday 12:00)

**Overview:**
When the wall clock is Tuesday, 12:00, second 0..2, and `KefraLive` is set, the component clears the latch and spawns the Kefra boss plus its full mob range (`:649-658`).

**Detailed description:**
The condition is `when.tm_wday == 2 && when.tm_hour == 12 && when.tm_min == 0 && when.tm_sec >= 0 && when.tm_sec <= 2 && KefraLive`. Because the enclosing block runs on `SecCounter % 2 == 0` (about once per second of tick time), and the window is exactly three wall-clock seconds wide, the event requires at least one even tick to land in `tm_sec <= 2`. If the server is busy such that no even tick occurs in that window, the spawn is skipped for the entire week - the latch `KefraLive` is only cleared when the spawn actually executes.

On execution it sets `KefraLive = 0`, spawns `KEFRA_BOSS` (396) and then every generator index from `KEFRA_MOB_INITIAL` (397) through `KEFRA_MOB_END` (400) via `GenerateMob(id, 0, 0)` (coordinates 0,0 means "let the generator record place it"). `KefraLive` is also a configuration value read from `gameconfig.txt` (`Server.cpp:1106`) and is used at boot to decide whether to pre-spawn the chain (`Server.cpp:3638-3644`), and by the GM commands `createkefra`/`killkefra` (`imple.cpp:1724-1736`, `:1657-1663`).

The name "Live" plus the latch semantics suggest the original design was "spawn once when the flag is set", with operators toggling the flag; the weekly schedule is layered on top of that. The consequence is that a server whose `KefraLive` starts at 0 will never run the scheduled spawn (the latch is already consumed at boot), which is a configuration trap worth recording.

**Rule workflow:**
```
if (SecCounter % 2 == 0):
    when = localtime(now)
    if when.tm_wday == 2 and when.tm_hour == 12 and when.tm_min == 0 and when.tm_sec in [0..2] and KefraLive:
        KefraLive = 0
        GenerateMob(KEFRA_BOSS)
        for x in [KEFRA_MOB_INITIAL .. KEFRA_MOB_END]: GenerateMob(x)
```
*Confidence: HIGH for mechanics; MEDIUM on whether the weekly schedule is the current operator intent.*

---

### Business Rule: BR-24 - Nightmare (Pesa) arena resets

**Overview:**
Three nightmare instances are torn down at fixed minute boundaries: A at minutes 9/29/49, M at 4/24/44, N at 19/39/59, each at second 0 (`:660-687`).

**Detailed description:**
Each reset is `DeleteMobMapa(sectorX, sectorY)` followed by `ClearMapa(sectorX, sectorY)` and zeroing of the corresponding `PartyPesa` entry, then a log line `"etc,nightmare_<kind> clear -%d:%d"`. `DeleteMobMapa` (`Server.cpp:8716-8729`) deletes every mob whose sector matches, with delete type 1; `ClearMapa` (`Server.cpp:8689-8714`) recalls players in that sector. Together they guarantee an empty instance with no players before the next start window.

The three schedules are offset by five minutes and each cycle is 20 minutes (A: 9, 29, 49; M: 4, 24, 44; N: 19, 39, 59), which interleaves three concurrent instances each lasting 15 minutes of play (BR-25 starts them five minutes after the reset). The `when.tm_sec == 0` requirement makes the reset a one-second-wide edge - the tightest of all calendar rules, and therefore the most likely to be missed under load, because it must coincide with an even tick.

`PartyPesa[]` is the instance's participant ledger. Resetting it here (rather than at start) means a party that never left the instance loses its slot bookkeeping at teardown, which is the correct ordering: teardown is unconditional, start is conditional on presence.

**Rule workflow:**
```
each even tick:
    when = localtime(now)
    if when.tm_min in {9,29,49} and when.tm_sec == 0: DeleteMobMapa(9,1); ClearMapa(9,1); PartyPesa[2] = 0; log
    if when.tm_min in {4,24,44} and when.tm_sec == 0: DeleteMobMapa(8,2); ClearMapa(8,2); PartyPesa[1] = 0; log
    if when.tm_min in {19,39,59} and when.tm_sec == 0: DeleteMobMapa(10,2); ClearMapa(10,2); PartyPesa[0] = 0; log
```
*Confidence: HIGH.*

---

### Business Rule: BR-25 - Nightmare start gates and countdown broadcast

**Overview:**
Instances start only if at least one player is inside the corresponding entry area and the minute is 14/34/54 (A), 9/29/49 (M) or 24/44/4 (N) at second 0; the start broadcasts a 900-unit countdown and spawns the instance mob range (`:1040-1097`).

**Detailed description:**
The gate is `GetUserInArea(x1,y1,x2,y2,temp)` (truthy for A, `> 0` for M and N) combined with a parenthesised minute/second disjunction. `GetUserInArea` (`Server.cpp:4157-4176`) counts `USER_PLAY` slots with a non-empty mob inside the inclusive rectangle and copies the last matching name into `temp`; the name is then unused, so the call doubles as a "is anyone here" probe with a wasted `strncpy`. The entry rectangles are A: 1152..1280 × 128..256, M: 1024..1152 × 256..384, N: 1280..1408 × 256..384 (sector 9,1 / 8,2 / 10,2 respectively).

On success the block builds a `MSG_STANDARDPARM` with `Type = _MSG_StartTime`, `ID = ESCENE_FIELD`, `Parm = 900` and `MapaMulticast`s it to the instance sector - a 15-minute countdown displayed to clients. It then spawns the whole generator range for the instance (`NIGHTMARE_A_INITIAL..END`, etc.) and logs `"etc,nigthmare_arcan started"` (the misspelling is in the source).

Because the start is conditioned on presence and the reset is not, an instance whose entry area is empty at the start boundary simply does not run that cycle; the reset five minutes later still executes. There is no state variable recording "instance running", so nothing prevents a second start if two different minutes match in one cycle - impossible with fixed minutes, but not structurally prevented.

**Rule workflow:**
```
each even tick:
  if GetUserInArea(1152,128,1280,256) and tm_min in {14,34,54} and tm_sec == 0:
      MapaMulticast(9,1, _MSG_StartTime Parm=900); spawn NIGHTMARE_A_INITIAL..END; log
  if GetUserInArea(1024,256,1152,384) > 0 and tm_min in {9,29,49} and tm_sec == 0:
      MapaMulticast(8,2, 900); spawn NIGHTMARE_M_INITIAL..END; log
  if GetUserInArea(1280,256,1408,384) > 0 and tm_min in {24,44,4} and tm_sec == 0:
      MapaMulticast(10,2, 900); spawn NIGHTMARE_N_INITIAL..END; log
```
*Confidence: HIGH.*

---

### Business Rule: BR-26 - Rune-track entry (minutes 0/20/40)

**Overview:**
At minutes 0, 20 and 40 (second 0) the component teleports registered party leaders and their members from the staging sector (25,13) into their assigned room, and spawns the stage-specific mobs (`:690-755`).

**Detailed description:**
The window first (re)initialises the shared counters: `Pista[4].Party[0..2].MobCount = 1` (the level-4 "rune track" race counter) and `Pista[6].Party[0].MobCount = 100` (the level-6 rabbit counter, which counts *down* via kills). It then loops stages `s in [0..6)` and parties `t in [0..2)` and applies a four-part eligibility test: `LeaderID != 0` and the stored `LeaderName` equals the current `pMob[LeaderID].MOB.MobName` (identity check against stale registrations), the leader's sector is exactly (25,13) (`TargetX/128 == 25 && TargetY/128 == 13`), and the leader is actually a leader (`Leader == 0` or `Leader == -1`).

For each eligible party it teleports the leader to `PistaPos[Pista[s].Party[t].Sala][t]` and then every party member that is also in sector (25,13), so members who did not enter the staging area are left behind. Spawns are then stage-specific: `s == 0` spawns the Lich for parties 0 and 1; `s == 1` spawns three towers at fixed coordinates plus the whole `RUNEQUEST_LV1_MOB` range *per party* (so three parties spawn three sets); `s == 2` spawns the level-2 boss; `s == 4` sets the race counter to `8 + rand()%8` and spawns the level-4 mob range for party 0 only.

Two structural notes: the spawns are inside the per-party loop, so the "for each party" iteration multiplies world mobs rather than sharing them; and the `s == 4` counter is deliberately re-randomised per party, which is what makes the level-4 leg a race rather than a fixed objective.

**Rule workflow:**
```
if tm_min in {20,40,0} and tm_sec == 0:
    Pista[4].Party[0..2].MobCount = 1 ; Pista[6].Party[0].MobCount = 100
    for s in [0..7), t in [0..3):
        skip unless LeaderID != 0, name matches, sector == (25,13), Leader in {0,-1}
        DoTeleport(leader, PistaPos[Sala][t])
        for each party member in sector (25,13): DoTeleport(member, same)
        s==0,t==0 -> LICH2 ; s==0,t==1 -> LICH1
        s==1 -> 3 towers (3358,1582 / 3386,1548 / 3418,1582) + LV1 mob range
        s==2 -> LV2 boss
        s==4 -> MobCount = 8 + rand()%8 ; t==0 -> LV4 mob range
```
*Confidence: HIGH.*

---

### Business Rule: BR-27 - Rune-track exit, purge and event-state reset (minutes 15/35/55)

**Overview:**
At minutes 15, 35 and 55 (second 0) the component deletes every NPC in the rune-track region, teleports and revives every player in it, awards the winners, and resets the whole `Pista` table (`:758-792`, `:1025-1036`).

**Detailed description:**
The purge iterates `c` in `[MAX_USER, MAX_MOB)` and calls `DeleteMob(c, 3)` for every non-empty NPC whose coordinates fall in 3310..3588 × 1005..1663 - the rune-track map. It deliberately skips player slots (`c` starts at `MAX_USER`), because players are handled by the next loop, which for each `USER_PLAY` slot inside the same rectangle revives the dead to 1 HP (`SendScore`) and then `DoTeleport(x, 3294, rand()%1 == 0 ? 1701 : 1686)`. `rand()%1` is always 0, so the conditional is dead and the destination is always (3294, 1701) - a defect that makes the "random exit" comment misleading.

After the purge the reward logic runs (BR-28) and finally the reset block sets, for all `x in [0..7)` and `z in [0..3)`: `LeaderID = 0`, `LeaderName = " "` (via `strncpy` of a 2-byte literal into 16 bytes), `MobCount = 0`, `Sala = 0`. Because the reset is inside the same `if` as the purge, a tick that misses the one-second window skips both - and then the next entry window (20/40/0) teleports parties into a map that was never purged or re-initialised, which is the failure mode most likely to be observed in production.

`DeleteMob(c, 3)` (type 3) is the "no drop, no counter" deletion used for event cleanup; it also clears the grid cell and party links (`Server.cpp:7020-7054`).

**Rule workflow:**
```
if tm_min in {15,35,55} and tm_sec == 0:
    for c in [MAX_USER, MAX_MOB):
        if mob in (3310..3588, 1005..1663): DeleteMob(c, 3)
    for x in [0, MAX_USER):
        if USER_PLAY and mob in rectangle:
            if Hp <= 0: Hp = 1; SendScore(x)
            DoTeleport(x, 3294, 1701)          // rand()%1 == 0 is constant
    <winner resolution, BR-28>
    for s in [0..7), t in [0..3): Pista[s].Party[t] = {LeaderID 0, name " ", MobCount 0, Sala 0}
```
*Confidence: HIGH.*

---

### Business Rule: BR-28 - Rune-track winner resolution and rewards

**Overview:**
For the level-1 (Torre) and level-3 (Sulrang) legs, the party with the strictly greatest `MobCount` wins; every party member receives a random rune and the leader additionally receives item 5134 with effect 43 (`:793-1023`).

**Detailed description:**
Each leg uses a three-way `if / else if / else if` chain of strict pairwise comparisons (`Party[0] > Party[1] && Party[0] > Party[2]`, etc.), so a tie awards nothing - there is no tiebreak. The winning branch then re-validates identity (`LeaderID != 0 && strcmp(pMob[LeaderID].MOB.MobName, LeaderName) == 0`) before granting anything, which protects against a leader slot being reused by a different character between registration and award.

Rewards are: a rune whose index is drawn from `PistaRune[1][rand()%5]` (level 1) or `PistaRune[3][rand()%5]` (level 3) given to the leader and to each `USER_PLAY` member of the leader's `PartyList`; plus a prize item `sIndex = 5134`, `stEffect[0].cEffect = 43`, `stEffect[0].cValue = 2` (level 1) or `4` (level 3) given to the leader only. The rune is re-drawn for every recipient (`Runa.sIndex = PistaRune[...][rand()%5]` inside the loop), so party members do not receive identical runes.

Three defects are visible in this rule and are documented in Section 9: the level-1 party-3 branch grants the leader rune to `Pista[1].Party[0].LeaderID` instead of `Party[2]` (`:882`); the level-3 party-2 branch grants the prize to `Pista[3].Party[3].LeaderID` (`:980`), an out-of-bounds element that aliases `Pista[4].Party[0].LeaderID`; and the member loop indexes `pMob[leader].PartyList[i]` and passes the result to `pUser[...]` without a bounds check (`:810-817`, `:848-855`, `:886-893`, `:926-933`, `:964-971`, `:1002-1009`), relying on `Mode` being 0 for out-of-range slots.

**Rule workflow:**
```
for leg in {1 (Torre), 3 (Sulrang)}:
    if P0 > P1 and P0 > P2 -> winner = 0
    elif P1 > P0 and P1 > P2 -> winner = 1
    elif P2 > P0 and P2 > P1 -> winner = 2
    else -> no award (tie)
    if winner valid and LeaderID != 0 and name matches:
        rune = PistaRune[leg][rand()%5]; PutItem(leader, rune)
        for i in [0, MAX_PARTY): member = PartyList[i]; if USER_PLAY: PutItem(member, new random rune)
        prize = {sIndex 5134, effect 43, value 2|4}; PutItem(leader, prize)
        Log("etc,questRune complete +<leg> party<winner>")
```
*Confidence: HIGH for the logic; HIGH for the three defects (verified against `STRUCT_PISTA` layout).*

---

### Business Rule: BR-29 - Balrog rune-track portal routing with ticket consumption

**Overview:**
Every tick, each registered `Pista[5]` party leader who has no leader-of-its-own, stands on a portal tile (4-cell aligned), and carries item 4032 is teleported to the next room according to a per-portal random table; the ticket is consumed (`:233-494`).

**Detailed description:**
The routing key is `xv = TargetX & 0xFFFC`, `yv = TargetY & 0xFFFC` - i.e. the tile is snapped to a 4×4 cell, so standing anywhere in the 4×4 block around a portal coordinate triggers it. For each of the three parties the code checks name identity against `Pista[5].Party[x].LeaderName`, requires `USER_PLAY` and `Hp > 0`, and requires the character not to be a follower (`Leader` in `{0, -1}`). It then draws `_rd = rand()%3` and selects a destination from a hand-written table covering eight portals across four rooms: room 1 portal 1; room 2 portals 1 and 2; room 3 portals 1, 2 and 3; room 4 portals 1..4. Each branch expresses a probability distribution over "stay", "advance" and "go back" - for example room 2 portal 1 sends `_rd <= 1` forward, `_rd == 2` two rooms forward and otherwise back.

The ticket check is a linear scan of the leader's carry slots bounded by `min(MaxCarry, MAX_CARRY)`: on finding `sIndex == 4032` it calls `BASE_ClearItem` and `SendItem` on that slot and breaks; if the scan falls off the end (`inv == MaxCarry`) the party is not teleported. Note that `inv == MAX_CARRY` also satisfies the guard, so a leader with a full backpack whose ticket is in the last slot is treated as ticketless only if the ticket was not found - the comparison is correct but only because the loop bound is exactly `MaxCarry`.

Entering room 4 through three of its four portals has a side effect: if `Pista[5].Party[0].MobCount == 0`, the level-5 boss is spawned (`:385`, `:409`, `:433`, `:457`). Because `MobCount` is only set to 1 by `MobKilled.cpp:1612` when the boss dies, this expresses "spawn the boss when the party has not yet killed it", and it re-arms automatically when the party re-enters.

**Rule workflow:**
```
for bb in [1, MAX_USER):
    xv = TargetX & 0xFFFC ; yv = TargetY & 0xFFFC
    for x in [0,3):
        skip unless USER_PLAY, Hp > 0, Pista[5].Party[x].LeaderID == bb
                  and names match and Leader in {0,-1}
        _rd = rand()%3 ; (tx,ty) = table lookup for (xv,yv)   // else continue
        scan Carry[0..min(MaxCarry,MAX_CARRY)) for sIndex 4032
            found -> BASE_ClearItem + SendItem ; break
        if not found -> continue
        if entering room 4 and Pista[5].Party[0].MobCount == 0 -> GenerateMob(RUNEQUEST_LV5_MOB_BOSS)
        DoTeleport(leader, tx, ty)
        for each party member with USER_PLAY: DoTeleport(member, tx, ty)
```
*Confidence: HIGH.*

---

### Business Rule: BR-30 - Secret-room mob-count broadcast (every 12 ticks)

**Overview:**
Every 12 ticks (6 s) the component sums the live mob counts of each secret-room generator group and broadcasts the remainder to the players inside the room, plus the rune-track and RvR counters (`:497-518`).

**Detailed description:**
The four sums are computed from `mNPCGen.pList[...].CurrentNumMob` over 6 generators per room (room 4 uses 12), covering `SECRET_ROOM_{N,M,A}_SALA{1..4}_MOB_{1..4}`. Each sum is broadcast with `SendSignalParmArea(rect, ESCENE_FIELD, _MSG_MobLeft, sum)`. `SendSignalParmArea` (`Code/TMSrv/SendFunc.cpp:1693-1713`) walks the *grid cells* of the rectangle, reads `pMobGrid[y][x]`, skips NPCs and empty cells, and sends `SendClientSignalParm` to the player occupying that cell. Because it iterates cells rather than user slots, a rectangle that contains overlapping players sends one message per occupied cell, and a player is addressed by the grid cell they occupy.

The rune-track (rabbit, `Pista[6].Party[0].MobCount`) and RvR (`RvRRedPoint + 1024`, `RvRBluePoint + 1024`) counters are broadcast the same way, with `SendShortSignalParm2Area` used for RvR to carry two values. The `+1024` bias is a client-protocol encoding of a signed value in a short.

`CurrentNumMob` is maintained by `CreateMob`/`DeleteMob` (`Server.cpp:7020-7054` decrements it with a floor of 0), so the broadcast is a derived view, not a stored event state; that makes it self-healing but means a generator-count corruption is immediately visible to clients.

**Rule workflow:**
```
if (SecCounter % 12 == 0):
    Sala1 = sum(CurrentNumMob of 6 generators)
    Sala2 = sum(...)  Sala3 = sum(...)  Sala4 = sum(12 generators)
    SendSignalParmArea(778,3651,832,3699, ESCENE_FIELD, _MSG_MobLeft, Sala1)
    SendSignalParmArea(836,3651,890,3699, ..., Sala2)
    SendSignalParmArea(832,3594,890,3644, ..., Sala3)
    SendSignalParmArea(780,3594,832,3645, ..., Sala4)
    SendSignalParmArea(3330,1475,3448,1525, ..., Pista[6].Party[0].MobCount)
    SendShortSignalParm2Area(1017,1911,1290,2183, ..., RvRRedPoint+1024, RvRBluePoint+1024)
```
*Confidence: HIGH.*

---

### Business Rule: BR-31 - Secret-room (Carta) progression across four rooms

**Overview:**
`CartaTime` counts down on a 4-tick cadence; at 1 the current room is cleared, the counter resets to 60 and the room index advances; room 4 performs a final full clear (`:1132-1170`).

**Detailed description:**
The block runs inside `if (SecCounter % 4 == 0)` and first decrements `CartaTime` when it is above 1. When it reaches exactly 1, `CartaTime` is set back to 60 and the room indicated by `CartaSala` is cleared with `ClearAreaTeleport(room, CartaPos[sala])`, which teleports every player in the room rectangle to the room's exit coordinate. Then `CartaSala` advances - except when it was 4, where the code takes the `CartaSala == 4` branch (`:1149`) and performs `ClearArea(776,3595,834,3648)` (a recall, not a teleport), resets both counters to 0 and logs `"etc,clear secretroom"`. Finally a `MSG_STANDARDPARM` with `Parm = CartaTime*2` is multicast to sector (6,28) as the visible countdown.

The control flow deserves note: the room-clear chain is `if (1) ... else if (2) ... else if (3) ...` followed by a *separate* `if (CartaSala == 4) {...} else CartaSala++;`. Because the second `else` binds to `if (CartaSala == 4)`, rooms 1..3 are cleared and then advanced, and room 4 is cleared and terminated - which is the intended behaviour, but only by accident of the `if` chain not being merged.

`CartaTime` starts at 0, so the block is inert until a room-entry item sets `CartaTime = 60` (`_MSG_UseItem.cpp:2791`) or a boss kill sets 60/3 (`MobKilled.cpp:1094`, `:1169`, `:1258`, `:1347`). A value of 3 is used to give the final room a short timeout, which is why the `== 1` test (rather than `<= 1`) is safe in practice but fragile.

**Rule workflow:**
```
if (SecCounter % 4 == 0):
    if CartaTime > 1: CartaTime--
    if CartaTime == 1:
        CartaTime = 60
        sala 1 -> ClearAreaTeleport(778,3652,832,3698, CartaPos[1])
        sala 2 -> ClearAreaTeleport(836,3652,890,3698, CartaPos[2])
        sala 3 -> ClearAreaTeleport(834,3595,889,3645, CartaPos[3])
        if CartaSala == 4: ClearArea(776,3595,834,3648); CartaTime = 0; CartaSala = 0; log
        else:             CartaSala++
        MapaMulticast(6,28, _MSG_StartTime, CartaTime*2)
```
*Confidence: HIGH.*

---

### Business Rule: BR-32 - Water-scroll room auto-clear countdowns

**Overview:**
`WaterClear1[3][10]` holds per-room countdowns that decrement every 4 ticks; at 1 the room is cleared and teleported out, with a smaller radius for the last two slots (`:1112-1130`).

**Detailed description:**
For each of the three instances (`i`) and each of ten rooms (`j`) the code decrements while above 1. When the value is exactly 1 it calls `ClearAreaTeleport` with a ±8 rectangle for `j <= 7` and a ±12 rectangle for `j > 7`, centred on `WaterScrollPosition[i][j]`, sending occupants to (1965, 1769), then zeroes the slot. The two radii correspond to normal rooms versus boss rooms, which the callers encode by writing `30` (normal, 60 s) or `15` (boss, 30 s) at `_MSG_UseItem.cpp:1735`, `:1929`, `:2034`.

The countdown is a *wall-clock approximation*: 30 units × 4 ticks × 0.5 s = 60 s only if ticks are delivered on time. The client is told the same value doubled (`WaterClear1 * 2`, `_MSG_UseItem.cpp:1738`), so the visible countdown matches the tick arithmetic rather than real time, and the discrepancy is invisible to players until the server is loaded.

`WaterClear1[i][j] == 1 && j > 7` is unreachable for `j == 8, 9` unless the caller wrote those indices; the water-scroll handler writes `vol-21`, `vol-131` and `vol-161`, which cover specific ranges, so indices 8 and 9 are used by the boss rooms of the N/A instances. `MobKilled` also clamps the value downward when a boss dies (`> 15 -> 15`, `> 5 -> 5`), which is how killing a boss shortens the remaining time for the party.

**Rule workflow:**
```
if (SecCounter % 4 == 0):
    for i in [0,3), j in [0,10):
        if WaterClear1[i][j] > 1: WaterClear1[i][j]--
        elif == 1 and j <= 7: ClearAreaTeleport(pos ±8, 1965,1769); WaterClear1[i][j] = 0
        elif == 1 and j >  7: ClearAreaTeleport(pos ±12, 1965,1769); WaterClear1[i][j] = 0
```
*Confidence: HIGH.*

---

### Business Rule: BR-33 - Ranking / arena engine cadence

**Overview:**
`ProcessRanking()` is invoked every 4 ticks (2 s) and implements the duel/arena countdown, survivor counting and visual warning phases; it is a no-op when `RankingTime <= 0` (`:1108-1110`; `Server.cpp:7847-8050`).

**Detailed description:**
`ProcessRanking` immediately returns when `RankingTime <= 0`, otherwise decrements it and, every fifth decrement (`RankingTime % 5 == 0`), counts survivors inside the arena rectangle per side. Side identity depends on `RankingProgress`: in 1v1 mode it compares the slot index against `Ranking1`/`Ranking2`, otherwise it compares guild ids. If either side has zero survivors, `RankingTime` is forced to 0, which ends the round immediately rather than waiting for the countdown.

The visual layer is expressed as remaining-time buckets: `>= 60` triggers a full-perimeter environmental effect and damage, `>= 120` a narrower one, `>= 180` (with `< 180` guard) another; the same pattern repeats for the final countdown stage. Because `ProcessRanking` is called on a 4-tick cadence, `RankingTime` is in units of 2 s, and the buckets (60/120/180) therefore correspond to 120 s / 240 s / 360 s of arena time.

The arena is a *timer consumer* rather than a timer owner: `RankingTime` is set by the duel/arena start handlers elsewhere in `Server.cpp`, and this component only advances it. That division means the game's most timing-sensitive subsystem (a 1v1 duel) is driven by a `% 4` modulus that can be stretched arbitrarily by load, with no compensation.

**Rule workflow:**
```
if (SecCounter % 4 == 0): ProcessRanking()
ProcessRanking:
    if RankingTime <= 0: return
    RankingTime--
    if RankingTime % 5 == 0: count survivors in arena rect per side; 0 on either -> RankingTime = 0
    if RankingTime >= 60: ... >= 120 ... < 180 -> effects/damage buckets ...
    else -> final-stage effects
```
*Confidence: HIGH for the cadence and countdown; MEDIUM for the exact bucket semantics (visual layer).*

---

### Business Rule: BR-34 - Battle Royale environmental damage phase

**Overview:**
While `BrState == 2` and `BRItem > 0`, two rectangular zones inflict environmental damage and effects on a 16-tick phase, with the rectangle size chosen by `BrMod` (`:1212-1231`).

**Detailed description:**
The gate as written is `if (BrState == 2 && i % 8 == Sec16 && BRItem > 0)`, where `i` is the loop variable of the *preceding* `for (i = 1; i < MAX_USER; i++)` loop that has already finished. At that point `i == MAX_USER == 1000`, so `i % 8 == 0` and the condition reduces to `Sec16 == 0`, i.e. the pass runs once every 16 ticks (8 s) instead of "for users whose index matches the phase". The intent was almost certainly a per-phase slot selection, so this is a defect with a benign observable effect (the pass still runs, just at a different cadence than written).

Inside, `if (BrMod < 9)` selects a "closing" geometry: for `BrMod` in 7..8 the two damage rectangles are (2608,1708)-(2622,1711) and (2608,1739)-(2622,1743); otherwise (BrMod 9+) the rectangles are (2608,1708)-(2622,1718) and (2608,1733)-(2622,1743). Each rectangle gets `SendDamage` and `SendEnvEffect(...,32,0)` - environment effect type 32 is the client's "damage field" visual. `SendDamage` (`Server.cpp:5437-5489`) deals a flat 2,000 HP (leaving at least 1 HP) to `USER_PLAY` slots inside the rectangle and multicasts `MSG_SetHpDam`.

`BrMod` is written by `GuildProcess` (`Server.cpp:6290`, `BrMod = tm_min % 20`) and `BrGrid = tm_min / 20`, so the geometry advances with the clock, and this pass consumes it as a phase selector. The dependency is one-way (min-timer writes, sec-timer reads) with no synchronisation, which is safe only because both run on the same thread.

**Rule workflow:**
```
after the per-user loop (i == MAX_USER == 1000):
  if BrState == 2 and Sec16 == 0 and BRItem > 0:
      if BrMod < 9:
          if BrMod >= 7: SendDamage(2608,1708,2622,1711); SendDamage(2608,1739,2622,1743); effects
      else:
          SendDamage(2608,1708,2622,1718); SendDamage(2608,1733,2622,1743); effects
```
*Confidence: HIGH for the mechanics; HIGH that the `i % 8` test uses the leaked loop variable.*

---

### Business Rule: BR-35 - Kingdom tower environmental damage

**Overview:**
Every 6 ticks, four rectangles in the two kingdom war bases damage every player whose clan differs from the owning kingdom: 10 % of max HP per application (`:1234-1245`; `Server.cpp:5491-5546`).

**Detailed description:**
Two rectangles belong to the blue kingdom - (1050,2108)-(1070,2146) and (1066,2133)-(1098,2146) - and two to the red kingdom - (1230,1947)-(1245,1988) and (1204,1948)-(1231,1962). Each is given both `SendEnvEffectKingdom(..., 32, 0, REINO_BLUE|REINO_RED)` and `SendDamageKingdom(..., REINO_*)`. The effect helper (`Code/TMSrv/SendFunc.cpp:387-431`) walks the grid cells, counts occupants whose clan differs from the argument and multicasts only when at least one such player is present - an optimisation that avoids broadcasting an effect nobody needs to see.

`SendDamageKingdom` iterates user slots, skips non-players, dead players, players outside the rectangle and players of the owning clan, then computes `dam_base = MaxHp / 10`, floors the result at 1 HP, writes the new HP, sets `ReqHp = new HP` (absolute, not delta), re-synchronises with `SetReqHp`/`SetReqMp` and multicasts `MSG_SetHpDam`. Using `MaxHp` rather than `Hp` as the base means the damage is flat relative to the maximum: a nearly-dead player still loses 10 % of max, immediately floored at 1 HP, which makes these zones lethal very quickly for low-HP builds.

Both rectangles of a kingdom overlap on the boundary column, so a player at the overlap receives two damage applications per pass (up to 20 % of max HP per 3 s). That is presumably intended as a "deeper inside = more damage" gradient, but it is not documented.

**Rule workflow:**
```
if (SecCounter % 6 == 0):
    SendEnvEffectKingdom(1050,2108,1070,2146, 32, 0, REINO_BLUE); SendDamageKingdom(..., REINO_BLUE)
    SendEnvEffectKingdom(1066,2133,1098,2146, 32, 0, REINO_BLUE); SendDamageKingdom(..., REINO_BLUE)
    SendEnvEffectKingdom(1230,1947,1245,1988, 32, 0, REINO_RED);  SendDamageKingdom(..., REINO_RED)
    SendEnvEffectKingdom(1204,1948,1231,1962, 32, 0, REINO_RED);  SendDamageKingdom(..., REINO_RED)

SendDamageKingdom: skip dead, outside, same clan
                   newHp = max(Hp - MaxHp/10, 1); Hp = newHp; ReqHp = newHp; SetReqHp; SetReqMp; multicast
```
*Confidence: HIGH.*

---

### Business Rule: BR-36 - Leader-room environmental damage with leader immunity

**Overview:**
Every 5 ticks the rune-track level-4 area is swept: if any of the `Pista[4]` party leaders is inside, no damage is applied at all; otherwise every player inside takes 2,000 flat damage (`:1247-1337`; `Server.cpp:5548-5639`).

**Detailed description:**
The pass emits 46 `SendEnvEffectLeader` + `SendDamageLeader` pairs at hard-coded 14×14 cell rectangles covering x 3335..3452, y 1285..1401 - the level-4 rune-track map, laid out as a grid of tiles. `SendEnvEffectLeader` (`SendFunc.cpp:433-...`) multicasts effect type 32 to the tile centre; `SendDamageLeader` first performs an "immunity" scan: it walks the cells of *its* rectangle, and if any occupant is `Pista[4].Party[0].LeaderID` or `Pista[4].Party[1].LeaderID` it increments a counter and returns without damaging anyone.

That immunity design has a defect: `Party[1]` is tested twice (`Server.cpp:5573` and `:5578`) and `Party[2]` never, so a level-4 party whose leader is registered in slot 2 receives no immunity and is damaged like anyone else. The effect is asymmetric rather than catastrophic, but it changes the fairness of the encounter depending on which registration slot the party received.

The damage itself mirrors `SendDamage`: 2,000 flat HP with a floor of 1, `ReqHp` set to the new absolute value, `MSG_SetHpDam` multicast. Because 46 rectangles are processed per pass and each performs a cell walk plus a player walk, this is the most expensive periodic block in the file by a wide margin (~46 × (cell walk + 1,000-slot scan)) every 2.5 s.

**Rule workflow:**
```
if (SecCounter % 5 == 0):
    for 46 tiles in [3335..3452] x [1285..1401]:
        SendEnvEffectLeader(tile, 32, 0)
        SendDamageLeader(tile)
SendDamageLeader(tile):
    walk tile cells; if occupant == Pista[4].Party[0].LeaderID or Party[1].LeaderID (twice) -> HaveUser
    if HaveUser: return
    for i in [0, MAX_USER): if USER_PLAY, alive, inside: Hp = max(Hp-2000, 1); ...
```
*Confidence: HIGH.*

---

### Business Rule: BR-37 - Mercenary stock renewal and reborn timers

**Overview:**
For each `pMobMerc[k]` entry with all of `RenewTime`, `RebornTime` and `GenerateIndex` non-zero, the stock vector is refilled when `SecCounter % RenewTime == 0` and the mob is respawned when `SecCounter % RebornTime == 0` (`:1339-1349`).

**Detailed description:**
`pMobMerc` is `STRUCT_MERC pMobMerc[MAX_MOB_MERC=128]` (`Code/Basedef.h:158`, `:672-682`), populated by `CReadFiles::ReadMobMerc` from `../../Common/Settings/MobMerc.txt` (`Code/TMSrv/CReadFiles.cpp:38`, `:591`). The parser stores `RENEWTIME: value * 2` and `REBORNTIME: value * 2` (`Server.cpp:9346-9350`), which is the **only correct** seconds-to-ticks conversion in the component: a configured 60 s becomes 120 ticks = 60 s of 500 ms ticks.

The renewal copies `MaxStock` over `Stock` wholesale (`memcpy` of the whole array), so a vendor's inventory is fully restored rather than incrementally replenished; the reborn branch calls `GenerateMob(GenerateIndex, 0, 0)` once, which recreates the vendor NPC if the generator has a free slot. Because the loop runs on every tick (it is not gated by a modulus of its own), each of the 128 entries is tested twice per tick against its own period - a small but constant cost, and the check short-circuits on the first zero field.

Two edge cases are unguarded: a negative `RenewTime`/`RebornTime` read from the file would make the modulo meaningless (`SecCounter % negative` is well-defined but never 0 in practice), and a `RenewTime == 1` would refill every tick. The parser performs no range validation on these fields.

**Rule workflow:**
```
for k in [0, MAX_MOB_MERC):
    if RenewTime == 0 or RebornTime == 0 or GenerateIndex == 0: continue
    if SecCounter % RenewTime == 0: memcpy(Stock, MaxStock, sizeof(Stock))
    if SecCounter % RebornTime == 0: GenerateMob(GenerateIndex, 0, 0)
file: ../../Common/Settings/MobMerc.txt -> RENEWTIME/REBORNTIME stored as value*2 ticks
```
*Confidence: HIGH.*

---

### Business Rule: BR-38 - Idle/peace NPC pass (1/6 of the slot range per tick)

**Overview:**
The pass starting at `Sec6 + MAX_USER` and stepping by 6 handles `MOB_IDLE` and `MOB_PEACE` NPCs: purge zero-HP mobs, run affect ticks, re-register the grid cell, run the AI processor and execute its action bits (`:1353-1552`).

**Detailed description:**
For `MOB_IDLE` (`CMob.h:29`) the code checks `CurrentScore.Hp <= 0`, logs `"err,standingby processer delete hp zero mob"` and deletes with type 1; otherwise it calls `ProcessAffect(index)` and then **re-writes** `pMobGrid[TargetY][TargetX] = index`, a defensive re-registration of the mob's occupancy cell. That write has no bounds check, so a mob with a corrupted coordinate outside `[0, MAX_GRIDX/Y)` would write outside the grid - a latent memory-safety risk that depends entirely on coordinate integrity elsewhere.

For `MOB_PEACE` the pass first clamps `SegmentProgress` into `[0, MAX_SEGMENT)` (resetting to 0 when out of range), then calls `StandingByProcessor()` (BR-39) and interprets the returned mask. Before that, it applies the level-6 boss-chamber rule (BR-40): a peaceful mob whose generator is not `RUNEQUEST_LV6_MOB_BOSS` standing inside (3423..3442, 1492..1511) increments `Pista[6].Party[0].MobCount` when its generator is in the "objective" range and is then deleted with type 3.

The stride-6 distribution means each NPC slot is visited every 3 s. Combined with `ProcessAffect` being called for both idle and peace states (but only on the same stride), NPC affect durations are in "3 s units", and peaceful NPC speech/patrol steps (`WaitSec -= 6`, `CMob.cpp:153`, `:168`, `:181`) are decremented by 6 per visit - the constant 6 is exactly the stride, which shows the author coded `WaitSec` in ticks and compensated for the stride manually.

**Rule workflow:**
```
Sec6 = SecCounter % 6
for index = MAX_USER + Sec6; index < MAX_MOB; index += 6:
    if Mode == MOB_IDLE:
        if Hp <= 0: Log + DeleteMob(index, 1); continue
        ProcessAffect(index); pMobGrid[TargetY][TargetX] = index
    if Mode == MOB_PEACE:
        if Hp <= 0: Log + DeleteMob(index, 1); continue
        ProcessAffect(index)
        if generator not LV6 boss and inside (3423..3442,1492..1511):
            if generator in LV6 objective range and 0 < MobCount < 100: MobCount++
            DeleteMob(index, 3); continue
        clamp SegmentProgress; mask = StandingByProcessor(); interpret mask (BR-39)
```
*Confidence: HIGH.*

---

### Business Rule: BR-39 - `StandingByProcessor` bit contract

**Overview:**
`CMob::StandingByProcessor` (`CMob.cpp:56-207`) returns a bit mask that the tick loop interprets as commands: `0x100` delete, `0x1` move one step, `0x10` flee-speech, `0x1000` random roam, `0x2` teleport, `0x10000` delete-and-count, `0x10000000 | target` engage.

**Detailed description:**
The first branch handles summoned/mercenary mobs (`RouteType == 5` or affect type 24): a mob with no leader and a mount face (315..345) or Samaritan affect is deleted (`0x100`); a mob whose leader or summoner is invalid is stepped (`0x1`); a mount more than 13 tiles from its summoner is teleported to it (`0x2`); a mount within 4..13 tiles walks toward the summoner (`0x1` after `GetTargetPos`). The second branch handles ordinary NPCs: with no leader and an enemy in view it returns `enemy | 0x10000000` (engage); otherwise it advances along its route (`SetSegment`, `GetNextPos`) and returns `0x1` when a step is available, `0x10` when a running attack is configured (`BaseScore.AttackRun & 0xF`), and `0x10000` when it has reached the final segment of a `RouteType == 3` patrol with `WaitSec` exhausted.

The tick loop then translates the mask, in this order: `0x10000000` -> `SetBattle` for the NPC and for every party member of both sides (with dead-member cleanup and party-list compaction); else `0x1` -> `GetEmptyMobGrid` + `GetAction` + `GridMulticast`, plus segment speech (`SendSay` of `SegmentAction`), plus the level-2 chamber deletion rule; `0x10` -> flee speech (`FleeAction`); `0x100` -> `DeleteMob(index, 3)`; `0x1000` -> `GetRandomPos` + move (random roam); `0x2` -> `DoTeleport(index, NextX, NextY)`; `0x10000` -> rune-track rabbit counter increment + `DeleteMob(index, 3)`.

The mask design is a compact command protocol inside one translation unit, and it is undocumented except by this interpretation. Note the asymmetries: `0x10` is checked with `if` then followed by an `else` containing `0x100`/`0x1000`/`0x2`/`0x10000`, so a mob returning both `0x10` and `0x1000` never roams; and `0x1` and `0x10` can both be set, causing a move plus a flee speech in the same visit.

**Rule workflow:**
```
mask = StandingByProcessor()
if mask & 0x10000000: SetBattle(index, target); engage both parties (party lists compacted)
else:
    if mask & 1:      choose empty grid, GetAction, broadcast; segment speech; LV2 chamber delete
    if mask & 0x10:   SendSay(FleeAction[progress])
    else:
        if mask & 0x100:   DeleteMob(index, 3); continue
        if mask & 0x1000:  GetRandomPos + GetAction + broadcast
        if mask & 2:       DoTeleport(index, NextX, NextY)
        if mask & 0x10000: rabbit counter++ ; DeleteMob(index, 3); continue
```
*Confidence: HIGH for the mask values; MEDIUM for the intended semantics of `0x10` vs `0x1` ordering.*

---

### Business Rule: BR-40 - Level-6 boss chamber: contact counting and purge

**Overview:**
In the level-6 rune-track boss chamber (3423..3442 × 1492..1511), non-boss NPCs of the "objective" generator range increment the rabbit counter and are deleted; the boss itself is exempt (`:1388-1398`, `:1581-1591`).

**Detailed description:**
The rule appears twice - once in the peace pass and once in the combat pass - with identical logic: `GenerateIndex != RUNEQUEST_LV6_MOB_BOSS` and coordinates inside the chamber; then, if `GenerateIndex` is between `RUNEQUEST_LV6_MOB_INITIAL2` and `RUNEQUEST_LV6_MOB_END2`, and `0 < Pista[6].Party[0].MobCount < 100`, increment the counter; then `DeleteMob(index, 3)` and `continue`. The `MobCount != 0` guard means the counter is only maintained while the run is active (it is set to 100 at the entry window, BR-26, and counts down as mobs are killed), so the increment can never resurrect a finished run.

The chamber acts as a "kill zone": any objective mob that walks into it is consumed and counted, which lets a party finish the objective by luring rather than fighting. Whether that is intended is a design question, but the code makes it unconditional and applies it to NPCs in both peace and combat states, so a mob chasing a player into the chamber is also consumed.

Because the counter saturates at 100 (the entry value), the rule is idempotent at the ceiling: a party that has already finished (counter 0) stops accumulating, and a party at 100 stops incrementing.

**Rule workflow:**
```
if GenerateIndex != RUNEQUEST_LV6_MOB_BOSS
   and TargetX in [3423..3442] and TargetY in [1492..1511]:
       if GenerateIndex in [RUNEQUEST_LV6_MOB_INITIAL2 .. RUNEQUEST_LV6_MOB_END2]:
           if 0 < Pista[6].Party[0].MobCount < 100: MobCount++
       DeleteMob(index, 3); continue
```
*Confidence: HIGH.*

---

### Business Rule: BR-41 - Combat pass: target validity, relations and aggro (1/4 per tick)

**Overview:**
The second NPC pass (stride 4) handles `MOB_COMBAT` mobs: it purges zero-HP slots, applies affects on a 16-tick slice, removes invalid enemies (same leader or same guild), interprets `BattleProcessor` and executes movement/attack bits (`:1557-1627`).

**Detailed description:**
The pass starts at `Sec4 + MAX_USER` with `Sec4 = SecCounter % 4` recomputed immediately before the loop (`:1554`), so each NPC slot is processed every 4 ticks (2 s). For each `MOB_COMBAT` mob it first forces `Hp = 0` when HP is already non-positive and logs a detailed diagnostic (index, leader, two party slots) before `DeleteMob(index, 1)` - the log line is the most verbose in the component and fires on every combat mob that dies outside the normal kill path.

It then applies `ProcessAffect(index)` when `index % 16 == Sec16` (so NPC affect ticks are 8 s), applies the level-6 chamber rule (BR-40), and performs enemy-list hygiene: it resolves leaders and guilds (honouring `GuildDisable`, and treating "both guildless" as equal by forcing `-1`), and removes the target from the enemy list if the attacker and target share a leader or a guild. This is the mechanism that stops friendly fire between party members and guild mates even if aggro was assigned.

`BattleProcessor` (`CMob.cpp:209-304`) then selects the nearest enemy within a distance that depends on clan (`6`, `12` for clans 4/7/8, `8` in a specific map sector, `HALFGRIDX` for the Kefra boss), drops targets outside the mob's segment box, and returns `0x20` (delete), `0x1000` (attack), `0x100`/`0x10`/`0x1` (movement variants) or `0` (idle). The Intel check `if (BaseInt < rand() % 100) return 0x010000;` makes higher-intelligence mobs skip turns probabilistically.

**Rule workflow:**
```
Sec4 = SecCounter % 4
for index = MAX_USER + Sec4; index < MAX_MOB; index += 4:
    if Mode != MOB_COMBAT: continue
    if Hp <= 0: Hp = 0; Log(...); DeleteMob(index, 1); continue
    if index % 16 == Sec16: ProcessAffect(index)
    level-6 chamber rule (BR-40)
    resolve leader/guild of self and target (GuildDisable -> 0; both 0 -> -1)
    if same leader or same guild: RemoveEnemyList(target)
    mode = BattleProcessor()
    if mode & 0x20  : DeleteMob(index, 3); continue
    if mode & 0x1000: attack resolution (BR-42)
    if mode & 0x100 : move toward target position
    if mode & 0x1   : move one step toward target
    if mode & 0x2   : DoTeleport
    if mode & 0x10  : move with effect
```
*Confidence: HIGH.*

---

### Business Rule: BR-42 - Mob attack resolution and damage modifiers

**Overview:**
When `BattleProcessor` returns `0x1000`, the component resolves the attack: half-grid validation, skill/affect application, parry, mount reduction, mana-shield conversion, fada mitigation and HP application, then broadcasts the damage (`:1629-1942`).

**Detailed description:**
The attack path begins with a `GetInHalf(Target, index)` check; on failure it appends a diagnostic block to `Gethalf.txt` (including a grid dump around the attacker) and skips the attack entirely - a debug path left enabled in production. On success it calls `GetAttack(index, Target, &sm)`, then resolves the *actual* target from `sm.Dam[0].TargetID`, and applies skill effects: for skills inside `[0, MAX_SKILLINDEX)` with `SkillParm == 0`, it derives `kind = (skill % MAX_SKILL / 8) + 1` and, for kinds 1..3, calls `SetAffect`/`SetTick` with the attacker's `CurrentScore.Special[kind]` and sends a score update if anything changed. Skill 33 is remapped to motion 253.

Player targets then get defence: `attackdex = Dex/5`, plus 100 if `LearnedSkill & 0x1000000`, plus 500 if `Rsv & 0x40`; `GetParryRate` is called and skills 79/22 reduce parry to 30 %. `rand()%1000+1 < parryretn` sets damage to `-3` (parried), upgraded to `-4` when `Rsv & 0x200` and the roll is below 100. Clan-4 (mount) attackers deal 40 % damage.

Damage application then applies three mitigations in order: mount damage transfer (target wearing 2360..2389 with HP effect > 0 takes 75 %, remainder goes to the mount via `ProcessAdultMount`), mana shield (affect type 18 converts half of the remaining damage into MP loss while MP > 10 % of max, then rescales damage by `((d >> 1) + (d << 4)) / 80`), and fada mitigation (slot-13 items 786, 1936, 1937 divide damage by `2*cValue`, `10*cValue`, `1000*cValue` with `cValue` floored at 2). Finally HP is reduced (or floored at 0), mount HP is linked for clan-4 targets, the damage packet is `GridMulticast`, `ReqHp` is reduced and `SendSetHpMp`/`SendScore` is sent.

**Rule workflow:**
```
if GetInHalf(Target,index) == 0: append Gethalf.txt diagnostic; continue
GetAttack(index, Target, &sm); Target = sm.Dam[0].TargetID
skill 109/110/111 -> Kefra area path (BR-43)
skill in range and SkillParm == 0: SetAffect + SetTick (+ SendScore)
if player target and damage > 0:
    parry roll (attacker dex/5 +100 learned +500 Rsv) -> damage -3 / -4
    else if clan 4 -> damage = damage * 2 / 5
if damage > 0:
    battle state for both parties (SetBattle), party members swept
    mount transfer (2360..2389) -> 3/4 damage, damage goes to mount
    mana shield (affect 18) -> half to MP, damage rescaled
    fada 786/1936/1937 -> divide damage
    Hp -= damage (floor 0); LinkMountHp; ProcessAdultMount
GridMulticast(damage); ReqHp -= damage; SetReqHp; SendSetHpMp / SendScore
if Hp <= 0: Hp = 0; MobKilled(Target, index, 0, 0)
```
*Confidence: HIGH for the sequence; MEDIUM for the exact intent of the mana-shield rescaling formula.*

---

### Business Rule: BR-43 - Kefra area-attack path (skills 109/110/111)

**Overview:**
If the resolved skill index is 109, 110 or 111 the code jumps (`goto KefraAttackLabel`) into a block that builds an area attack and calls `MobAttack`, instead of the single-target path. The block is guarded by `if (0 == 1)` and is therefore reachable only through the `goto` (`:1715-1717`, `:1944-1953`).

**Detailed description:**
The three skills correspond to "Raio Vermelho", "Empurrão" and "Chão de espinhos" (comments at `:1712-1714`). They are outside the configured skill table (`MAX_SKILLINDEX = 103`), so the normal affect-application block would skip them anyway; the `goto` exists to route them into `GetAttackArea`/`MobAttack`, which resolves an area-of-effect hit for all targets around the mob.

The `if (0 == 1)` wrapper is a decompiler artifact or a deliberately disabled guard: as written, there is no way to reach the label except by the `goto`, and the `goto` is the only transfer into that region of the function. Semantically the construct is a manually marked "cold" block placed at the end of the loop body so that the single-target code can fall through to the remaining movement bits. Any refactor that removes the `goto` would orphan the block, and any change to the `0 == 1` constant would make the area attack fire for every attack.

Because the Kefra boss has a special selection range (`HALFGRIDX`, `CMob.cpp:399-400`) and area skills, this path is the mechanism by which the world boss hits multiple players per attack cycle, which is why it is on the timer's critical path.

**Rule workflow:**
```
skill = sm.SkillIndex
if skill in {109, 110, 111}: goto KefraAttackLabel
...
if (0 == 1) {
  KefraAttackLabel:
      MSG_Attack sm; memset
      GetAttackArea(index, &sm)
      MobAttack(index, sm)
}
```
*Confidence: HIGH for reachability; MEDIUM on whether the `if (0 == 1)` was ever `if (KefraLive)` or similar.*

---

### Business Rule: BR-44 - Battle-state propagation and death dispatch

**Overview:**
On positive damage the component sets battle state on the target's leader and back onto the attacker, sweeps the target party for dead members, and dispatches `MobKilled` when HP reaches zero (`:1770-1941`).

**Detailed description:**
`SetBattle(TargetLeader, index)` makes the target's group retaliate, and `SetBattle(index, TargetLeader)` makes the attacker hostile to that group; the second call is skipped for clan-4 (mount) attackers so that a pet does not become the aggressor. The party sweep then iterates `pMob[TargetLeader].PartyList[z]` for members above `MAX_USER` and either sets battle on live members or deletes dead ones and compacts the list to `MOB_EMPTY`. Summoned attackers (`Clan == 4`) with a valid summoner also compute a damage share inside a 46-tile box, but the computed `summdam` is never used - dead code.

Death dispatch is `if (pMob[Target].MOB.CurrentScore.Hp <= 0) { Hp = 0; MobKilled(Target, index, 0, 0); }`. `MobKilled` (`Code/TMSrv/MobKilled.cpp`) is the single central reward/quest/event handler for kills, so this line is how the timer hands control to the domain layer for drops, experience, quest progress, castle quests, rune-track counters, war-tower kills and PK consequences.

The ordering matters: damage is broadcast (`GridMulticast`) and the target's `ReqHp` updated *before* `MobKilled` runs, so client-visible state is consistent even if the kill handler performs further mutations, and the kill handler is called with the attacker index so attribution is preserved for party and PK logic.

**Rule workflow:**
```
if damage > 0:
    TargetLeader = pMob[Target].Leader or Target
    SetBattle(TargetLeader, index)
    if attacker clan != 4: SetBattle(index, TargetLeader)
    for z in [0, MAX_PARTY):
        party = PartyList[z]; if party <= MAX_USER: continue
        if dead/empty: delete if present; PartyList[z] = MOB_EMPTY
        else: SetBattle(party, index); SetBattle(index, party)
    (summoned share computed, unused)
apply damage / broadcast / ReqHp
if Hp <= 0: MobKilled(Target, index, 0, 0)
```
*Confidence: HIGH.*

---

### Business Rule: BR-45 - Daily log rotation by day-of-month comparison

**Overview:**
On every min-tick the component compares `tm_mday` with `LastLogDay`, `LastChatLogDay` and `LastItemLogDay` and rotates the corresponding log files when they differ (`:2027-2034`).

**Detailed description:**
`StartLog`, `StartChatLog` and `StartItemLog` (`Server.cpp:6654`, `:9208`, and the item-log opener) each close the previous handle and open a timestamped file. The comparison is against a day-of-month integer rather than a date, so the rotation triggers on the first min-tick of a new day (within 12 s) and also triggers spuriously whenever the month changes backwards or the clock is adjusted to a day that differs from the stored value - a behaviour that is harmless (a new file) but produces extra files after NTP corrections.

Because the check runs on every min-tick, `LastLogDay` is effectively a latch that must be written by `StartLog`; the component itself does not write it, so correctness depends on the callee. The rotation is the only persistent state the min-timer maintains unconditionally, and it is the reason log files are day-scoped.

Note also that `Log()` writes to `fLogFile` and to the window title on every call (`Server.cpp:6722-6725`); a rotation that closes the handle while a tick is logging would produce a write to a closed file, which is prevented only by the single-threaded execution order (rotation happens at the start of the min-tick, before any other logging in that tick).

**Rule workflow:**
```
time(&rawtime); timeinfo = localtime(&rawtime)
if timeinfo->tm_mday != LastLogDay    : StartLog()
if timeinfo->tm_mday != LastChatLogDay: StartChatLog()
if timeinfo->tm_mday != LastItemLogDay: StartItemLog()
```
*Confidence: HIGH.*

---

### Business Rule: BR-46 - Newbie-event server and castle-host server rotation

**Overview:**
Every other min-tick (24 s) the component computes `NewbieServerID = (tm_mday - 1) % NumServerInGroup` and, separately, `CastleServer` from the week number's parity against `ServerIndex` (`:2036-2072`).

**Detailed description:**
The newbie rotation sets `NewbieEventServer = 1` on the server whose index matches today's rotation slot, or unconditionally when `LOCALSERVER != 0`; when the flag was set on a *previous* day and the current server is no longer the designated one, it clears the flag and, for every connected session in trade mode, calls `RemoveTrade(i)` (the "change newbie channel" action; the `CEncampment::ChangeNewBieChannel()` call is commented out at `:2045`). `NumServerInGroup` is clamped to `[1..10]` at boot (`Server.cpp:3572-3582`), so the modulo is safe.

The castle rotation computes `wNum = BASE_GetWeekNumber()`, `wMod = wNum % 7`, `wDay = wNum / 7` and sets `CastleServer = 1` when `wMod == 0 && (wDay % 2) == (ServerIndex % 2)` - i.e. the castle siege host alternates between odd and even server indices every other week. `CastleServer` is consumed by `GuildProcess` to decide whether the 21:00 siege state machine runs on this server (`Server.cpp:6223`).

Both rotations are evaluated on `MinCounter % 2`, which is 24 s - far more often than needed, but the computation is idempotent. The `if (1 != 0)` wrapper at `:2060` is a decompiler `if (TRUE)` artifact that disables nothing.

**Rule workflow:**
```
if (MinCounter % 2 == 0):
    NewbieServerID = (tm_mday - 1) % NumServerInGroup
    if LOCALSERVER != 0 or ServerIndex == NewbieServerID: NewbieEventServer = 1
    elif NewbieEventServer == 1 and ServerIndex != NewbieServerID:
        for i: if session and TradeMode == 1: RemoveTrade(i)
        NewbieEventServer = 0
    wNum = BASE_GetWeekNumber(); wMod = wNum % 7; wDay = wNum / 7
    CastleServer = (wMod == 0 and (wDay % 2) == (ServerIndex % 2)) ? 1 : 0
```
*Confidence: HIGH.*

---

### Business Rule: BR-47 - Guild tax NPC persistence

**Overview:**
Every min-tick, each registered guild tax NPC (`GuildImpostoID[0..MAX_GUILDZONE)`) that is alive has its `STRUCT_MOB` written to `./npc/<name with spaces replaced by underscores>` (`:2074-2104`).

**Detailed description:**
The loop skips zero ids and dead NPCs, builds the path from the literal `"./npc/"` plus the mob name with every space replaced by `_` (a 16-byte `NAME_LENGTH` scan), opens the file with `_open(..., _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE)` and `_write`s exactly `sizeof(STRUCT_MOB)` bytes, then closes. The binary record is therefore a raw dump of the mob structure, matching the format the loader expects (the loader is `CreateMob`, which reads `./<folder>/<name>`).

This is a write-per-12-seconds persistence loop with no dirty check: five files are rewritten on every min-tick even when nothing changed, which is ~300 rewrites per minute of ~1 KB each. It is the second highest-frequency disk writer after the MAC reload, and it is the mechanism by which tax NPC state (collected gold, presumably) survives a restart.

Failure handling is inconsistent: `_open` returning `-1` logs `"-system", "fail - save npc imposto file", 0` (arguments reversed relative to the intended "str1, str2" order used elsewhere) and **returns from the whole min-timer**, skipping the castle-quest tick, kingdom clears, gate re-lock, NPC respawn, weather and `GuildProcess` for that tick - a single unwritable file therefore suppresses the entire calendar engine for 12 s, repeatedly.

**Rule workflow:**
```
for i in [0, MAX_GUILDZONE):
    if GuildImpostoID[i] == 0: continue
    if pMob[GuildImpostoID[i]].Hp <= 0: continue
    path = "./npc/" + name(MobName with ' ' -> '_')
    h = _open(path, _O_CREAT|_O_RDWR|_O_BINARY, _S_IREAD|_S_IWRITE)
    if h == -1: Log(...); return                      // aborts the rest of the min-tick
    _write(h, &pMob[GuildImpostoID[i]].MOB, sizeof(STRUCT_MOB)); _close(h)
```
*Confidence: HIGH.*

---

### Business Rule: BR-48 - Kingdom clear two-phase and `FailAccount` reset cadence

**Overview:**
`Kingdom1Clear` and `Kingdom2Clear` are two-phase flags advanced once per min-tick, clearing the two kingdom base areas on the second phase; `FailAccount` is zeroed every 10 min-ticks (`:2113-2137`).

**Detailed description:**
The two kingdom flags implement a delayed action: when a flag is 1 it is advanced to 2 (no side effect) and on the next min-tick, when it is 2, the corresponding area is cleared and the flag returns to 0. The cleared rectangles are (1676,1556)-(1776,1636) and (1676,1816)-(1776,1892). `ClearArea` recalls players and revives the dead to 2 HP, so this is the mechanism that ends a kingdom war by evacuating both bases with a two-tick (24 s) delay between the event decision and the evacuation - presumably to let the announcement land first.

`FailAccount` is the circular table of 16 failed login attempts (`Server.cpp:1334-1361`); zeroing it every `MinCounter % 10` (120 s) is the anti-bruteforce window reset. Because `MinCounter` is a 12 s counter, the window is two minutes, not ten.

Both behaviours are stateless with respect to the tick: they are driven solely by the flags and the counter, so a missed min-tick simply delays them by 12 s. That makes them among the most robust timing rules in the component.

**Rule workflow:**
```
if Kingdom1Clear == 1: Kingdom1Clear = 2
elif Kingdom1Clear == 2: Kingdom1Clear = 0; ClearArea(1676,1556,1776,1636)
if Kingdom2Clear == 1: Kingdom2Clear = 2
elif Kingdom2Clear == 2: Kingdom2Clear = 0; ClearArea(1676,1816,1776,1892)
if (MinCounter % 10 == 0): memset(FailAccount, 0, sizeof(FailAccount))
```
*Confidence: HIGH.*

---

### Business Rule: BR-49 - Castle/keep gate re-lock from the init-item table

**Overview:**
Every min-tick, every init item from index 17 to `g_dwInitItem` that sits on the grid, has a "key" ability (item ability 39) and is in `STATE_OPEN` is re-locked after a one-tick delay (`:2142-2209`).

**Detailed description:**
The loop resolves each configured gate position (`g_pInitItem[i].PosX/PosY`) to the item occupying that grid cell (`pItemGrid[ipy][ipx]`), skips empty or out-of-range item indices, reads `BASE_GetItemAbility(&pItem[ipg].ITEM, 39)` as `iKey` and ignores items without a key ability. For keyed items below 15 in `STATE_OPEN`, the first min-tick sets `Delay = 1` and `continue`s; the next min-tick sees `Delay != 0`, calls `UpdateItem(ipg, 3, &Height)`, sets `State = STATE_LOCKED`, resets `Delay = 0` and multicasts a `MSG_CreateItem` so clients re-render the closed door.

The `Delay` indirection is a deliberate one-tick latch: it gives the client one min-tick (12 s) to see the door open before the server slams it shut. Because the loop is unconditional, doors can never be left open permanently - the server enforces the closed state on a 12 s cadence even if a handler opened them.

Two safety notes: `pItem[ipg]` is dereferenced without validating `ipg` against `MAX_ITEM` (the grid is trusted), and `g_dwInitItem` must be within the table bounds. Neither is checked here.

**Rule workflow:**
```
for i in [17, g_dwInitItem):
    ipx = g_pInitItem[i].PosX ; ipy = g_pInitItem[i].PosY ; ipg = pItemGrid[ipy][ipx]
    if pItem[ipg].ITEM.sIndex <= 0 or >= MAX_ITEMLIST: continue
    iKey = BASE_GetItemAbility(IITEM, 39)
    if iKey != 0 and State == STATE_OPEN and iKey < 15:
        if Delay == 0: Delay = 1; continue
        UpdateItem(ipg, 3, &Height); State = STATE_LOCKED; Delay = 0
        MSG_CreateItem -> GridMulticast(ipx, ipy)
```
*Confidence: HIGH.*

---

### Business Rule: BR-50 - NPC generator respawn scheduling with dynamic re-randomisation

**Overview:**
Every min-tick, each generator whose index is not in `{0,1,2,5,6,7}` and whose `MinuteGenerate > 0` respawns when `MinCounter % MinuteGenerate == i % MinuteGenerate`; after respawning, generator periods in four bands are re-randomised within their band, and the `>= 3800` band additionally spawns dungeon prizes (`:2212-2280`).

**Detailed description:**
The phase-offset test (`MinCounter % MG == i % MG`, where `mod = i % MinuteGenerate`) staggers generators so that they do not all fire on the same min-tick: generator `i` fires on the min-tick whose counter is congruent to `i` modulo its own period. Because `MinCounter` advances every 12 s, `MinuteGenerate = 5` means "every 5 min-ticks" = 60 s, `300` means 3,600 s (1 h), and `5000` means 100,000 s - the field name is the main source of the "minutes" misconception.

The re-randomisation is the interesting rule: after a successful respawn, a generator whose period is in `[500,1000)` is reset to `rand()%500 + 500`; `[1000,2000)` to `rand()%1000 + 1000`; `[2000,3800)` to `rand()%1800 + 2000`; and `>= 3800` to `rand()%1000 + 3800`. Because the code tests the *old* value and assigns within the band, a generator never leaves its band, and a band-3 generator keeps a period between 3,800 and 4,799 min-ticks (12.7 h to 16 h). This is a jitter mechanism to decorrelate spawn times across many generators.

The `>= 3800` band also hosts the dungeon event: if `DUNGEONEVENT` is set, it picks one of 30 `DungeonPos` locations, spawns 5..9 items, each with a random index from `DungeonItem[rand()%10]`, random bonuses via `SetItemBonus`, and a random rotation via `CreateItem(dpX, dpY, &PrizeItem, rand()%4, 1)`. The fixed exclusions `0,1,2,5,6,7` are the generators reserved for scripted events (secret room, rune track, etc.), which must never be auto-respawned on a timer.

**Rule workflow:**
```
for i in [0, mNPCGen.NumList):
    MG = pList[i].MinuteGenerate
    if i in {0,1,2,5,6,7}: continue
    if MG <= 0: continue
    mod = i % MG
    if MinCounter % MG == mod:
        GenerateMob(i, 0, 0)
        if MG in [500,1000):    MG = rand()%500  + 500   ; continue
        if MG in [1000,2000):   MG = rand()%1000 + 1000  ; continue
        if MG in [2000,3800):   MG = rand()%1800 + 2000  ; continue
        if MG >= 3800:
            MG = rand()%1000 + 3800
            if DUNGEONEVENT: pick DungeonPos[rand()%30]; for 5..9 items: CreateItem with SetItemBonus
```
*Confidence: HIGH.*

---

### Business Rule: BR-51 - Weather roll and forced weather

**Overview:**
Every min-tick a `rand()%1200` draw selects one of three weather bands unless `ForceWeather != -1`, in which case the forced value is applied (`:2284-2311`).

**Detailed description:**
With `ForceWeather == -1` the draw is compared against three overlapping bands, each guarded by "current weather differs": `[0,260)` sets clear (0), `[30,50)` sets 1, `[55,60)` sets 2. The overlap is deliberate in effect - because the first branch is skipped when `CurrentWeather == 0`, a draw in 30..49 while already clear selects weather 1, and a draw in 55..59 while already clear selects 2. The resulting probabilities per min-tick are therefore roughly 21.7 % for clear, 1.7 % for weather 1 and 0.4 % for weather 2 when clear, and 21.7 % for clear otherwise; weather 1 and 2 can only be entered from clear.

`SendWeather()` (`Code/TMSrv/SendFunc.cpp:1561-1588`) broadcasts `_MSG_UpdateWeather` to every session with a live socket, and closes a session whose buffer rejects the message. With `ForceWeather != -1` (set by the GM command `weather`, `imple.cpp:1121-1126`, which also sets `CurrentWeather` directly and broadcasts) the roll is bypassed and the forced value is applied once, then held.

Because the roll happens on every min-tick (12 s), weather is *expected* to change about every 46 s on average - a fast cycle for a cosmetic state, and a broadcast to all players each time it does change.

**Rule workflow:**
```
rnd = rand() % 1200
if ForceWeather == -1:
    if rnd in [0,260) and CurrentWeather != 0: CurrentWeather = 0; SendWeather()
    elif rnd in [30,50) and CurrentWeather != 1: CurrentWeather = 1; SendWeather()
    elif rnd in [55,60) and CurrentWeather != 2: CurrentWeather = 2; SendWeather()
else:
    if ForceWeather != CurrentWeather: CurrentWeather = ForceWeather; SendWeather()
```
*Confidence: HIGH for the code path; MEDIUM on the intended probabilities (the bands overlap by design).*

---

### Business Rule: BR-52 - `GuildProcess()` calendar engine, including the war tower

**Overview:**
The last statement of the min-tick calls `GuildProcess()` (`Server.cpp:6203-6660`), which evaluates six calendar state machines against `localtime()` and delegates the war-tower machine to `CWarTower::GuildProcess(timeinfo)` (`CWarTower.cpp:42-108`).

**Detailed description:**
Because `GuildProcess` re-reads the clock itself and is called every 12 s, every `tm_min`-keyed branch inside it is re-evaluated up to five times per minute; correctness therefore depends on each state variable (`CastleState`, `BrState`, `ColoState`, `RvRState`, `WeekMode`, `GTorreState`) being idempotent for repeated evaluations within the same minute. The castle machine (`:6223-6282`) is gated on `CastleServer == 1 && tm_hour == 21` and steps 1 -> 2 -> 3 on minute thresholds 5/50/55, i.e. exactly the kind of edge that survives repeated evaluation because it tests ranges, not equality.

The Battle Royale machine (`:6284-6384`) is gated on `(TESTSERVER || LOCALSERVER || ServerIndex == NewbieServerID) && BRItem > 0`, derives `tmmin2 = tm_min/20` and `tmmin = tm_min%20`, writes `BrMod`/`BrGrid` and steps states on exact minutes (5 and 13). BR-34 consumes `BrMod` and `BrState` from the sec-timer, which is the only place in the component where the two clocks share mutable state.

`CWarTower::GuildProcess` implements the guild war tower: only on non-weekend days at `GTorreHour` (config, default 22) and only while `NewbieEventServer == 1`. State 0 -> 1 at `tm_min <= 5` (announcement, `GTorreGuild = 0`), 1 -> 2 at `tm_min >= 6` (`ClearArea` of the tower map, `GenerateMob(GTORRE)`, start notice), and 2 -> 0 at `tm_min == 59` (clear area, award 100 fame to the owning guild through a `_MSG_GuildInfo` to DBSrv, delete the tower, `GTorreState = 0`). Because the transition to 0 requires `tm_min == 59` exactly and `GuildProcess` is called five times inside minute 59, the transition is reached on the first of those five evaluations and the subsequent four find `GTorreState == 0` and do nothing - an idempotence that works only because the state is mutated before the next evaluation.

The remaining machines (Colosseum `:6386-6502`, RvR `:6609-6650`, guild war `:6511-6607`) follow the same pattern. This rule is recorded here because the *cadence* is owned by this component while the *semantics* belong to the Server Core and CWarTower components (analysed separately).

**Rule workflow:**
```
min-tick:
    ... everything else ...
    GuildProcess()

GuildProcess:
    time(); localtime()
    guild-name re-init at 03:00 (once per day)
    CastleServer && hour == 21 -> castle state machine (minutes 5/50/55)
    BR gate -> BrState 0..3 (minutes 5, 13, 20), writes BrMod/BrGrid
    Colosseum gate -> ColoState 0..8 (minutes 3,4,5,7,9,11,13,15)
    RvR gate -> RvRState 1..3 (RvRHour)
    guild war -> WeekMode
    CWarTower::GuildProcess(timeinfo):
        NewbieEventServer && weekday && hour == GTorreHour:
            GTorreState 0->1 at min <= 5 ; 1->2 at min >= 6 ; 2->0 at min == 59 (+100 fame)
```
*Confidence: HIGH for the cadence and the war-tower machine; HIGH for the delegation (`Server.cpp:6651`).*

---

### Business Rule: BR-53 - `CCastleZakum` tick delegates

**Overview:**
`ProcessSecTimer` calls `CCastleZakum::ProcessSecTimer()` unconditionally on every tick (`:1172`) and `ProcessMinTimer` calls `CCastleZakum::ProcessMinTimer()` unconditionally (`:2110`); both maintain the castle-quest instance (`CCastleZakum.cpp:314-375`).

**Detailed description:**
The sec delegate runs its own `SecCounter % 2` gate: when `CastleQuestTime == 0` it latches the value to `-1`, clears the quest area (2180..2296 × 1160..1269) and, for every grid cell in that area, deletes the occupying NPC and sets its generator's `MinuteGenerate` to `-1` (permanently disabling that generator). When `CastleQuestTime > 0` it decrements - so the quest countdown is in 1-second units because the delegate applies its own `% 2` on top of the 500 ms tick.

The min delegate implements a two-phase clear (`CastleQuestClear` 1 -> 2 -> 0) with a notice on the first phase and the actual area clear on the second, mirroring the kingdom-clear pattern (BR-48).

Both are called unconditionally, which means the `CCastleZakum` module's own state machine advances on the same schedule as the rest of the world regardless of whether a castle quest is active; the guards are internal. This is the only external module whose tick methods are called directly by name from this component (as opposed to being reached through `GuildProcess`).

**Rule workflow:**
```
sec-tick: CCastleZakum::ProcessSecTimer()
            if SecCounter % 2 == 0:
                if CastleQuestTime == 0: CastleQuestTime = -1; ClearArea(2180,1160,2296,1269);
                                         for cells: generator.MinuteGenerate = -1; DeleteMob(tmob,3)
                if CastleQuestTime > 0 : CastleQuestTime--
min-tick: CCastleZakum::ProcessMinTimer()
            if CastleQuestClear == 1: = 2; SendNoticeArea(...)
            elif == 2: = 0; ClearArea(...); for cells: generator.MinuteGenerate = -1; DeleteMob(tmob,3)
```
*Confidence: HIGH.*

---

### Business Rule: BR-54 - Inoperative `ForceWeekMode` override

**Overview:**
`if (ForceWeekMode != -1) timeinfo->tm_wday = -1;` (`:2107-2108`) writes to a stack-local `struct tm` that is never read afterwards, so the GM command that sets `ForceWeekMode` (`imple.cpp:828`, reset at `:836`) has no effect on this component.

**Detailed description:**
`ProcessMinTimer` obtains `timeinfo` from `localtime(&rawtime)` at `:2024-2025` and uses it for the log rotation (`:2027-2034`) and the newbie-server computation (`:2038`). The `ForceWeekMode` assignment occurs *after* all those reads and before calls that re-read the clock themselves (`CCastleZakum::ProcessMinTimer` takes no argument, and `GuildProcess` calls `time()`/`localtime()` on its own at `:6208-6209`). The local copy is therefore written and discarded.

The intent is inferable from the assignment (`tm_wday = -1`): forcing an invalid weekday so that weekday-gated branches (`CWarTower`'s `tm_wday != 0 && != 6` test) behave as if the day were "unknown", effectively suppressing weekend checks. That intent cannot be realised by mutating a discarded copy.

This is recorded as a business rule because it is a *configuration surface presented to operators* (a GM command exists for it) that silently does nothing; the ambiguity is whether the feature is unimplemented, broken by the decompilation, or intended for a code path removed in this build. Confidence that it is currently inoperative is HIGH; confidence about the original intent is LOW.

**Rule workflow:**
```
ProcessMinTimer:
    timeinfo = localtime(&rawtime)
    ... uses timeinfo for log rotation and newbie rotation ...
    if ForceWeekMode != -1: timeinfo->tm_wday = -1     // local copy, never read
    CCastleZakum::ProcessMinTimer()                    // no timeinfo argument
    GuildProcess()                                     // re-acquires its own timeinfo
```
*Confidence: HIGH for inoperativeness; LOW for original intent.*

---

## 4. Component Structure

### 4.1 File-level structure

```
Code/TMSrv/ProcessSecMinTimer.cpp        2,314 lines / 63,912 bytes / 2 functions / no classes
├── header block (GPL notice)                                             :1-18
├── includes                                                              :19-38
│   ├── <Windows.h>, <stdio.h>, <fcntl.h>, <time.h>, <math.h>, <io.h>, <errno.h>
│   ├── "..\Basedef.h"                                                    shared domain contract
│   ├── "..\CPSock.h"                                                     socket transport
│   ├── "..\ItemEffect.h"                                                 item effect dictionary
│   ├── "Language.h"                                                      message string table
│   ├── "CItem.h"                                                         ground item entity
│   ├── "Server.h"                                                        world state + service prototypes
│   ├── "GetFunc.h" / "SendFunc.h"                                        outbound projection helpers
│   ├── "ProcessClientMessage.h" / "ProcessDBMessage.h"                   message dispatchers
│   └── "CWarTower.h"                                                     included, only used transitively
├── ProcessSecTimer()                                   1,978 lines      :40-2017
│   ├── shutdown drain                                   53 lines         :42-94
│   ├── shutdown countdown                               24 lines         :96-119
│   ├── counter + timestamp                               4 lines         :121-125
│   ├── MAC reload (inverted)                             2 lines         :127-128
│   ├── billing reconnect                                38 lines         :130-167
│   ├── round-robin save                                 31 lines         :169-199
│   ├── per-user flush / mob tick / celestial recall     31 lines         :201-231
│   ├── #pragma region Pista de Runas Balrog Portais    262 lines         :233-494
│   ├── secret-room broadcasts                           22 lines         :497-518
│   ├── ten-minute quest sweep                           19 lines         :520-538
│   ├── premium item expiry                              61 lines         :539-599
│   ├── vitals application                               19 lines         :601-619
│   ├── vitals accumulator                               20 lines         :621-640
│   ├── calendar block (5 pragma regions)               458 lines         :642-1099
│   │   ├── Kefra weekly spawn                                           :649-658
│   │   ├── #pragma region Pesa Reset                                    :659-688
│   │   ├── #pragma region Pista de runas entrada                        :689-756
│   │   ├── #pragma region Pista de runas saida                          :757-1037
│   │   └── #pragma region Pesa Start                                    :1039-1098
│   ├── ground item decay                                 2 lines         :1101-1102
│   ├── ranking + water + carta                          64 lines         :1108-1171
│   ├── CCastleZakum::ProcessSecTimer()                   1 line          :1172
│   ├── per-user stagger + quest recalls                 37 lines         :1174-1210
│   ├── BR environmental damage                          20 lines         :1212-1231
│   ├── kingdom environmental damage                     12 lines         :1234-1245
│   ├── #pragma region leader environmental damage       91 lines         :1247-1337
│   ├── mercenary renewal                                11 lines         :1339-1349
│   ├── #pragma region Movimento / fala dos NPCS        197 lines         :1355-1552
│   └── #pragma region MOB_ATTACK_SPEACH_ACTION         460 lines         :1556-2016
└── ProcessMinTimer()                                     296 lines         :2019-2314
    ├── log rotation                                     10 lines         :2027-2034
    ├── server role rotation                             38 lines         :2036-2072
    ├── guild tax NPC persistence                        31 lines         :2074-2104
    ├── ForceWeekMode dead store                          2 lines         :2107-2108
    ├── CCastleZakum::ProcessMinTimer()                   1 line          :2110
    ├── #pragma region Clear Area                        23 lines         :2112-2134
    ├── FailAccount reset                                 2 lines         :2136-2137
    ├── #pragma region Close Gates                        69 lines         :2141-2210
    ├── #pragma region GenerateMobs                       71 lines         :2211-2281
    ├── weather                                          28 lines         :2284-2311
    └── GuildProcess()                                    1 line          :2313
```

### 4.2 Size and complexity metrics (measured)

| Metric | `ProcessSecTimer` | `ProcessMinTimer` | File |
|--------|-------------------|-------------------|------|
| Lines of code (physical) | 1,978 | 296 | 2,314 |
| `if (` occurrences | 273 | 37 | 310 |
| `else if` chains | 36 | 9 | 36 (file) |
| `for (` loops | 40 | 6 | 46 |
| `while (` loops | 1 | 0 | 1 |
| `goto` | 2 (one label) | 0 | 2 |
| `&&` / `\|\|` operators | — | — | 191 / 50 |
| Approximate McCabe complexity | ~316 | ~44 | ~360 |
| Functions defined | 2 | — | 2 |
| Classes / structs defined | 0 | — | 0 |
| `#pragma region` blocks | 9 | 3 | 12 |
| Distinct external functions called | ~100 | ~25 | ~105 |
| Distinct external globals referenced | ~40 | ~20 | ~45 |

The complexity concentration is unambiguous: 98.7 % of the file's decision points are inside `ProcessSecTimer`, a single 1,978-line function whose nesting reaches 8 levels (the Balrog portal table at `:251-467` inside `for x` inside `for bb` inside the function, with a further `if/else if` ladder). No function is longer than this one anywhere else in TMSrv except `SetItemBonus` in `Server.cpp` (941 lines) and the message handlers.

### 4.3 Relationship to the rest of the tree

```
Code/
├── Basedef.h/.cpp                 shared domain contract, grids, buffer sizes
├── CPSock.h/.cpp                  socket transport used by SendMessageA
└── TMSrv/
    ├── Server.cpp/.h              world state, service library, WM_TIMER dispatch
    ├── ProcessSecMinTimer.cpp     THIS COMPONENT (scheduler + tick bodies)
    ├── CMob.cpp/.h                per-entity AI: StandingByProcessor, BattleProcessor
    ├── CNPCGene.cpp/.h            generator table (MinuteGenerate, CurrentNumMob)
    ├── CCastleZakum.cpp/.h        castle-quest tick delegate
    ├── CWarTower.cpp/.h           war-tower calendar machine (called via GuildProcess)
    ├── CReadFiles.cpp/.h          ReadMacblock (called from this component), MobMerc loader
    ├── CUser.cpp/.h               session entity, socket buffers
    ├── CItem.cpp/.h               ground item entity
    ├── GetFunc.cpp/.h             derivation helpers
    ├── SendFunc.cpp/.h            outbound projection (MapaMulticast, SendSignalParmArea, SendWeather)
    ├── MobKilled.cpp              central kill resolution (invoked from the combat pass)
    ├── ProcessClientMessage.cpp   inbound dispatch (invoked from the fada path)
    ├── ProcessDBMessage.cpp       DB responses
    ├── imple.cpp                  GM command interpreter (sets ServerDown, ForceWeather, ForceWeekMode)
    └── _MSG_*.cpp                 per-message handlers (read/write the same event state)
```

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains (call graph, direction = caller -> callee)

```
MainWndProc                                  Server.cpp:3718
  -> ProcessSecTimer                         ProcessSecMinTimer.cpp:40
       -> SendBilling2                       Server.cpp:1382        (billing logout)
       -> Log                                Server.cpp:6701        (file + SetWindowText)
       -> CloseUser                          Server.cpp:6834        (drain, send failure)
       -> CReadFiles::ReadMacblock           CReadFiles.cpp:678
       -> BillServerSocket.ConnectBillServer CPSock.cpp
       -> SendClientMessage / CharLogOut / SaveUser                 (billing gate + save)
       -> cSock.SendMessageA                 CPSock.cpp:617
       -> pMob[].ProcessorSecTimer           CMob.cpp:51            (empty body)
       -> DoRecall                           Server.cpp:7558
       -> GenerateMob / DeleteMob            Server.cpp:2995 / 7020
       -> DoTeleport                         Server.cpp:8212
       -> SendItem / BASE_ClearItem / PutItem
       -> SendSignalParmArea                 SendFunc.cpp:1693
       -> SendShortSignalParm2Area           SendFunc.cpp:1715
       -> ClearAreaQuest / ClearArea         Server.cpp:5730 / 5754
       -> ClearAreaTeleport                  Server.cpp:5832
       -> BASE_CheckItemDate / BASE_CheckFairyDate / BASE_GetItemAbility
       -> ApplyHp / ApplyMp                  Server.cpp:8591 / 8625
       -> GetUserInArea                      Server.cpp:4157
       -> MapaMulticast                      SendFunc.cpp:1662
       -> DeleteMobMapa / ClearMapa          Server.cpp:8716 / 8689
       -> ProcessDecayItem                   Server.cpp:8353
       -> ProcessRanking                     Server.cpp:7847
       -> CCastleZakum::ProcessSecTimer      CCastleZakum.cpp:314
       -> CheckIdle                          Server.cpp:4304
       -> RegenMob                           Server.cpp:4362
       -> ProcessAffect                      Server.cpp:4513
       -> SendDamage / SendEnvEffect         Server.cpp:5437 / SendFunc.cpp:348
       -> SendDamageKingdom / SendEnvEffectKingdom   Server.cpp:5491 / SendFunc.cpp:387
       -> SendDamageLeader / SendEnvEffectLeader     Server.cpp:5548 / SendFunc.cpp:433
       -> StandingByProcessor                CMob.cpp:56
       -> SetBattle / GetAction / GetEmptyMobGrid / GridMulticast / SendSay
       -> BattleProcessor                    CMob.cpp:209
       -> GetAttack / GetInHalf / GetParryRate / GetAttribute / BASE_GetVillage
       -> SetAffect / SetTick / SetItemBonus
       -> ProcessClientMessage               ProcessClientMessage.cpp:38   (fada path)
       -> MobAttack / GetAttackArea          (Kefra area path)
       -> MobKilled                          MobKilled.cpp                 (death dispatch)
       -> LinkMountHp / ProcessAdultMount / ProcessClientMessage / SendScore
  -> ProcessMinTimer                         ProcessSecMinTimer.cpp:2019
       -> StartLog / StartChatLog / StartItemLog      Server.cpp
       -> RemoveTrade                                Server.cpp
       -> BASE_GetWeekNumber                         Basedef.cpp
       -> _open / _write / _close                    CRT
       -> CCastleZakum::ProcessMinTimer              CCastleZakum.cpp:345
       -> ClearArea                                  Server.cpp:5754
       -> UpdateItem / GridMulticast / BASE_GetItemAbility
       -> GenerateMob / SetItemBonus / CreateItem
       -> SendWeather                                SendFunc.cpp:1561
       -> GuildProcess                               Server.cpp:6203
             -> time / localtime
             -> BASE_InitializeGuildName
             -> SendNotice / SendNoticeArea / SendClientSignalParm
             -> ClearAreaGuild / ClearAreaTeleport / SetCastleDoor
             -> GenerateMob / CreateItem / DeleteColoseum / GenerateColoseum
             -> FinishCastleWar
             -> DBServerSocket.SendOneMessage
             -> CWarTower::GuildProcess              CWarTower.cpp:42
                   -> SendNotice / ClearArea / GenerateMob / DeleteMob
                   -> BASE_GetGuildName / DBServerSocket.SendOneMessage
```

### 5.2 External dependencies

| Dependency | Type | Purpose in this component | Protocol / Format | Error handling |
|------------|------|---------------------------|-------------------|----------------|
| `Win32 User32` (`SetTimer`, `PostQuitMessage`, `SetWindowText`, `DeleteObject`) | OS API | Timer registration, process exit, log rendering, font cleanup | C API | none (return values ignored except `SetWindowText` which returns void here) |
| `Win32 Kernel32` (`timeGetTime`) | OS API | `CurrentTime` global for socket/DB code | C API | none |
| `CRT time` (`time`, `localtime`) | OS API | Calendar decisions | `struct tm` | none; `localtime` result dereferenced without null check |
| `CRT stdio` (`fopen`, `fprintf`, `fclose`) | OS API | Diagnostic files `AttackDieTeste.txt`, `Gethalf.txt` | text, append | `Gethalf` checks `fp != NULL`; `AttackDieTeste` does not (`:1652-1656`) |
| `CRT io` (`_open`, `_write`, `_close`) | OS API | Guild tax NPC persistence | binary `STRUCT_MOB` | `-1` -> log + abort min-tick |
| DBSrv (TCP 7514) | Internal service | Indirect: `SaveUser`, `CharLogOut`, `CloseUser`, `GuildInfo`, `_MSG_MessageDBRecord` all go through `DBServerSocket` | proprietary binary messages | delegated to `Server.cpp`/`CPSock` |
| Billing (BISrv) | External service | Reconnect countdown, logout on shutdown | proprietary `_AUTH_GAME` | failure sets `BILLING = 0` |
| Client sockets | External clients | Outbound flush, broadcasts, damage packets | proprietary message structs | `SendMessageA` failure -> `CloseUser` |
| `../../Common/Settings/MobMerc.txt` | Local file (read once at boot) | `pMobMerc` renewal/reborn periods | line-oriented text with `KEY: value` | missing file -> `MessageBoxA` |
| `MAC_BLOCK_PATH` | Local file (read repeatedly) | `pMac[]` block list | text lines `idx a.b.c.d` | missing -> silent return |
| `./npc/<name>` | Local file (written) | Guild tax NPC state | binary `STRUCT_MOB` | open failure aborts min-tick |
| `gameconfig.txt` | Local file (read at boot) | `KefraLive`, `FREEEXP`, `BRItem`, `BRHour`, `GTorreHour`, `RvRHour`, `DUNGEONEVENT` | positional text | fail-open (see Server Core report) |
| `NPCGenerator` file (via `mNPCGen`) | Local file (read at boot) | `MinuteGenerate`, `CurrentNumMob` | text | delegated to `CNPCGene` |

### 5.3 Notable dependency characteristics

- **Bidirectional coupling with `Server.cpp`.** This component calls ~40 services defined in `Server.cpp`; `Server.cpp` calls this component's two functions. The relationship is a call-back loop rather than a layered dependency, which is why neither file can be understood alone.
- **Implicit dependency on ordering inside `Server.cpp`.** `RegenMob` expects `SecCounter` to be current (it increments `Unk_2736` once per call); `CheckIdle` reads `SecCounter` directly; `CCastleZakum::ProcessSecTimer` applies its own `% 2`. All three assume they are called from this component at the documented cadence.
- **State coupling through four global aggregates** (`Pista`, `CartaTime/CartaSala`, `WaterClear1`, `BrState/BrMod`) written by `_MSG_UseItem`, `MobKilled`, `GuildProcess` and read here. These are the component's real "inputs".
- **Compile-time coupling to `CWarTower.h`** (`:38`) which is not used directly: the include is unnecessary in this translation unit (`GuildProcess` lives in `Server.cpp`), a minor hygiene issue that adds a header-order dependency.
- **No dependency injection, no interfaces, no mocks.** Every collaborator is a free function or global, so substituting any of them for testing would require linking alternative symbols.

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the granularity that matches the code: C++ free functions and global data structures (the codebase has no interfaces or abstract classes in TMSrv; `CMob`, `CUser`, `CItem`, `CCastleZakum`, `CWarTower` are concrete classes with no virtuals).

### 6.1 Component-level (translation unit)

| Direction | Measured | Detail |
|-----------|----------|--------|
| Afferent (TUs that call into this component) | 1 | `Server.cpp` (`:3726`, `:3728`). The functions are also declared in `Server.h:105-106`, but no other TU calls them. |
| Afferent (TUs that read this component's *state*) | 5 | `SecCounter`: `CCastleZakum.cpp:316`, `ProcessClientMessage.cpp:57`, `ProcessDBMessage.cpp:450`, `_MSG_CharacterLogin.cpp:97`, `Server.cpp:4306-4317`. `MinCounter`: none. |
| Efferent (TUs called into) | ~15 | `Server.cpp`, `CMob.cpp`, `CCastleZakum.cpp`, `CWarTower.cpp`, `CNPCGene.cpp` (data), `CReadFiles.cpp`, `CPSock.cpp`, `SendFunc.cpp`, `GetFunc.cpp`, `MobKilled.cpp`, `ProcessClientMessage.cpp`, `Basedef.cpp`, `CItem.h` (data), `CUser.h` (data), CRT/Win32 |
| Efferent (distinct functions called) | ~105 | measured by extracting every call identifier in the file |
| Efferent (distinct external globals referenced) | ~45 | `pUser`, `pMob`, `pItem`, `pItemGrid`, `pMobGrid`, `mNPCGen`, `pMobMerc`, `Pista`, `PistaPos`, `PistaRune`, `PistaBalrogPos`, `PistaBalrogPortalPos`, `CartaPos`, `CartaTime`, `CartaSala`, `WaterClear1`, `WaterScrollPosition`, `PartyPesa`, `BrState`, `BrMod`, `BRItem`, `KefraLive`, `GuildImpostoID`, `GuildInfo`, `g_pGuildZone`, `g_pInitItem`, `g_dwInitItem`, `g_pMessageStringTable`, `FailAccount`, `Kingdom1Clear`, `Kingdom2Clear`, `NewbieEventServer`, `CastleServer`, `ServerIndex`, `ServerGroup`, `NumServerInGroup`, `LOCALSERVER`, `TESTSERVER`, `ForceWeather`, `ForceWeekMode`, `CurrentWeather`, `LastLogDay`, `LastChatLogDay`, `LastItemLogDay`, `BillServerSocket`, `DBServerSocket`, `hWndMain`, `hFont`, `temp`, `CurrentTime`, `ItemCount`, `MAX_*` constants |

The architecture report's estimate of ~7 afferent / ~9 efferent is consistent with a *module-boundary* count (7 modules import the component, 9 modules it imports) but understates the function-level fan-out by an order of magnitude. Both views are recorded because they answer different questions: 1/15 for "who must change together", ~105/45 for "what can break this file".

### 6.2 Symbol-level coupling

`Ca` = number of *other* translation units that reference the symbol (measured across `Code/**/*.cpp` and `Code/**/*.h`). `Ce` = distinct symbols the function depends on (callees + globals). "Critical" is an assessment of blast radius if the symbol's behaviour changes.

| Symbol | Kind | Ca | Ce | Critical | Notes |
|--------|------|----|----|----------|-------|
| `ProcessSecTimer` | function | 1 | ~100 fns + ~40 globals | Critical | The server's entire periodic behaviour |
| `ProcessMinTimer` | function | 1 | ~25 fns + ~20 globals | Critical | Calendar engine entry |
| `SecCounter` | global | 5 | — | Critical | Doubles as the server-wide wall clock for idle/DB/login logic |
| `MinCounter` | global | 0 | — | Low | Confined to this TU |
| `CCastleZakum::ProcessSecTimer` | function | 1 | 6 | High | Owns castle-quest timing |
| `CCastleZakum::ProcessMinTimer` | function | 1 | 5 | Medium | Two-phase clear |
| `GuildProcess` | function | 1 direct (+ `CWarTower` sibling) | ~40 | Critical | Castle/BR/Colosseum/RvR/war-tower |
| `RegenMob` | function | 1 | ~15 | High | Castle capture, PK points, mount feeding |
| `ProcessAffect` | function | 1 | ~25 | High | All buff/debuff timing |
| `ApplyHp` / `ApplyMp` | function | 1 / 1 | 0 / 0 | Medium | Pure data functions, easy to isolate |
| `CheckIdle` | function | 1 | 3 | Medium | Reads `SecCounter` directly |
| `ProcessDecayItem` | function | 1 | 5 | High | Item lifecycle; contains an index runaway |
| `ProcessRanking` | function | 1 | ~20 | Medium | Arena resolution |
| `SendDamage` / `SendDamageKingdom` / `SendDamageLeader` | function | 1 each | 6 / 6 / 6 | Medium | Environmental damage |
| `GenerateMob` | function | 7 | ~20 | Critical | Used by this component ~40 times |
| `DeleteMob` | function | 8 | ~10 | Critical | ~30 call sites in this component |
| `DoTeleport` | function | 12 | ~8 | Critical | Widest movement dependency |
| `DoRecall` | function | 8 | ~10 | High | City/guild/newbie destination logic |
| `CloseUser` | function | 8 | ~15 | Critical | Session teardown from timer paths |
| `SaveUser` | function | 4 | ~5 | Critical | Durable state |
| `CharLogOut` | function | 8 | ~6 | High | Billing-gate eviction |
| `Pista[7]` | global array | 9 | — | High | Rune-track event state shared with `MobKilled`, `_MSG_*`, `SendFunc`, `CReadFiles`, `GetFunc` |
| `CartaTime` / `CartaSala` | global | 3 | — | Medium | Secret-room progression |
| `WaterClear1[3][10]` | global | 3 | — | Medium | Water-room countdowns |
| `pMobMerc[128]` | global array | 3 | — | Low | Vendor restock |
| `KefraLive` | global | 5 | — | Medium | One-shot spawn latch, also config and GM command |
| `ServerDown` | global | 5 | — | Critical | Admission gate + drain |
| `pUser` / `pMob` | global arrays | 65 / 64 (per architecture report) | — | Critical | Shared world state |

### 6.3 Coupling assessment

**Cohesion is low at the function level and high only by accident of grouping.** `ProcessSecTimer` performs persistence, transport, event scheduling, combat resolution, NPC AI dispatch, item lifecycle and zone enforcement in one body. There is no shared abstraction between those concerns other than "runs every 500 ms", which is the definition of coincidental cohesion. `ProcessMinTimer` is more cohesive (it is entirely calendar/persistence work) but still mixes file rotation, binary persistence, area clears, spawning and weather.

**Coupling is high in both directions and unstable.** Efferent coupling reaches 105 functions, so any signature change in `Server.cpp`, `CMob.cpp` or `SendFunc.cpp` propagates here. Afferent coupling on `SecCounter` is small in TU count but the semantics are load-bearing for idle timeouts, login windows and castle-quest timing, so changing the tick period would silently re-scale four other modules.

**Architectural boundary status:** the component sits *inside* the TMSrv tier and has no boundary enforcement. It reaches directly into `CCastleZakum` and (through `GuildProcess`) `CWarTower`, i.e. the scheduler knows about concrete event modules, which means adding an event module requires editing the scheduler. There is no registry, no table of callbacks, and no dispatch-by-name.

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| `WM_TIMER` / `TIMER_SEC` | OS message | 500 ms logic tick | Win32 message loop | `wParam == 0` | none (message loss is silent) |
| `WM_TIMER` / `TIMER_MIN` | OS message | 12 s calendar tick | Win32 message loop | `wParam == 1` | none |
| `CPSock::SendMessageA` | Internal transport | Flush queued outbound bytes per session | TCP via Winsock | proprietary message buffer | `FALSE` -> log + `CloseUser` |
| `SendBilling2` / `BillServerSocket` | External service | Logout on shutdown, reconnect, auth | proprietary TCP | `_AUTH_GAME` | failure -> `BILLING = 0` |
| `DBServerSocket.SendOneMessage` | Internal service | Guild fame updates (war tower), `_MSG_DBMessage` | proprietary TCP to DBSrv:7514 | binary structs | delegated |
| `SaveUser` / `CharLogOut` / `CloseUser` | Internal service | Persistence and teardown | in-process | binary structs to DBSrv | delegated |
| `mNPCGen.pList[]` | In-memory table | Spawn scheduling and live counts | direct memory access | `NPCGENLIST` (2,988 bytes/entry) | none; indices trusted |
| `pMobMerc[]` | In-memory table | Vendor restock/reborn | direct memory access | `STRUCT_MERC` | guarded on zero fields |
| `pMobGrid[][]` | In-memory 4096×4096 grid | Occupancy queries for broadcasts and damage | direct memory access | `int` slot index | bounds checked in `SendSignalParmArea`/`SendEnvEffect*`, **not** in the `MOB_IDLE` re-registration (`:1373`) |
| `MAC_BLOCK_PATH` | Local file | MAC block list | text, read ~2.4×/s | `idx a.b.c.d` lines | missing file -> silent return |
| `../../Common/Settings/MobMerc.txt` | Local file | Vendor periods and stock | text, read at boot | `KEY: value` lines | missing -> `MessageBoxA` |
| `./npc/<mobname>` | Local file | Guild tax NPC persistence | binary, written every 12 s | raw `STRUCT_MOB` | `-1` -> log + abort min-tick |
| `AttackDieTeste.txt` | Local file | Diagnostic for PvP-in-village attacks | text, appended from the combat loop | free-form | **no null check on `fopen`** (`:1652-1656`) |
| `Gethalf.txt` | Local file | Diagnostic for `GetInHalf` failures, with grid dump | text, appended from the combat loop | free-form + grid listing | `fp != NULL` checked |
| `Log()` | Internal service | Server log + window title | file + `SetWindowText` | text | none |
| `SendNotice` / `SendClientMessage` | Internal projection | Player-facing notices (shutdown, castle, events) | in-process -> `AddMessage` | `MSG_MessageChat` etc. | delegated |
| `g_pMessageStringTable` | In-memory table | Localised strings | index lookup | `char*` table | index arithmetic unchecked (`ServerDown/20 + 17`) |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
|---------|----------------|----------|---------|------------|
| Deterministic modulus scheduler (time-sliced) | `SecCounter % N == 0` bands | whole file | Cheap periodic dispatch without a timer per subsystem | Works, but ties gameplay rate to message throughput (BR-04) |
| Round-robin / circular slice | `i % N == SecCounter % N`, `SaveCount` cursor, stride loops | `:169-199`, `:1174-1189`, `:1357`, `:1557` | Bound per-tick cost on 1k users / 25k mobs | Effective; introduces up-to-8 s quantisation |
| Two-phase flag (deferred action) | `Kingdom1Clear` 1→2→0, `CastleQuestClear` 1→2→0 | `:2113-2133`, `CCastleZakum.cpp:347-374` | Separate "announce" from "execute" by one tick | Simple and robust; state is a bare int |
| State machine keyed on wall clock | `CastleState`, `BrState`, `ColoState`, `RvRState`, `GTorreState`, `CastleQuestTime`, `CartaTime` | `:1132-1170`, `Server.cpp:6203-6660`, `CWarTower.cpp:42-108` | Event lifecycle without a scheduler object | Works because transitions mutate state before re-evaluation; assumes idempotence |
| Command bit mask | `StandingByProcessor` / `BattleProcessor` return bitfields interpreted by the caller | `CMob.cpp:56-304`, `:1406-1548`, `:1621-2014` | Avoid virtual dispatch / callbacks | Compact but implicit; ordering asymmetries are easy to break |
| Callback delegation by convention | `CCastleZakum::ProcessSecTimer/ProcessMinTimer` called by name | `:1172`, `:2110` | Let an event module own its timing | No registry; adding a module means editing the scheduler |
| Two-counter vitals model | `ReqHp/ReqMp` (requested) vs `CurrentScore.Hp/Mp` (materialised) | `:601-640`, `Server.cpp:8591-8687` | Allow bounded application of accumulated deltas | Clean separation, but requires `SetReqHp/SetReqMp` discipline |
| Latch / one-shot flag | `KefraLive`, `GuildNameInitialized`, `CastleServer`, `NewbieEventServer` | `:649-658`, `Server.cpp:6211-6221` | Ensure a once-only action | Latches are never re-armed automatically; a missed window needs operator action |
| Compensation for stride in entity code | `WaitSec -= 6` in `CMob` (stride 6), `ProcessAffect` called on slice | `CMob.cpp:153,168,181` | Keep entity clocks in tick units | Manual and fragile if the stride changes |
| Debug-in-production tracing | `fopen` + `fprintf` diagnostics, `Log` on every anomaly | `:1652`, `:1674`, `:1368`, `:1572` | Post-mortem evidence for rare desyncs | Considerable hot-path cost (Section 9) |
| Dead-code guard from decompilation | `if (0 == 1)`, `if (1 != 0)`, `// CEncampment::...` comments | `:1945`, `:2060`, `:1351`, `:2045` | Preserve original structure across decompiles | Documents nothing; risks being "fixed" incorrectly |

**Architectural decisions observed (not recommendations, just records):**

1. **Two coarse clocks instead of many fine ones.** One 500 ms and one 12 s timer, with intra-tick banding by modulus. This keeps the message queue small and gives a single place to reason about cost, at the price of a single point of failure for all timing.
2. **The tick body is a top-level script, not a set of components.** There is no scheduler registry, no priority, no dependency graph, no cancellation. Ordering is textual.
3. **Event modules own their state, the scheduler owns their cadence.** `CCastleZakum` and `CWarTower` are invoked from fixed points; they cannot choose their own frequency.
4. **Absolute-time decisions are confined to the calendar blocks.** Everything else is tick-counted. This is a deliberate simplification that makes the code readable but makes wall-clock accuracy dependent on tick delivery.
5. **No locking, ever.** Justified by the single-threaded design; it is the invariant that makes the direct global writes safe.

---

## 9. Technical Debt & Risks

### 9.1 Risk table

| Risk Level | Component Area | Issue | Impact | Evidence |
|------------|----------------|-------|--------|----------|
| High | Tick model | `SecCounter` is 0.5 s and `MinCounter` 12 s while all names/constants say seconds/minutes | Every timeout, save interval, event window and generator period is off by 2× or 5× | `:123`, `:2282`, `Server.cpp:3631-3632` |
| High | Tick model | No accumulator, no lateness tracking, no catch-up | Gameplay rate degrades with load; one-second event windows can be missed entirely | `:1108`, `:1174-1175`; BR-04 |
| High | Calendar events | One-second-wide triggers (`tm_sec == 0`) gated by an even-tick modulus | Kefra, nightmare resets/starts and rune-track entry/exit can silently skip a cycle | `:649`, `:660`, `:690`, `:758`, `:1040` |
| High | Performance | MAC block file re-read on 119 of 120 ticks | Continuous disk I/O on the only thread; 2.4 opens/s forever | `:127-128`, `CReadFiles.cpp:678-715` |
| High | Performance | `fopen("a+")`+`fprintf` inside the combat loop | Disk I/O per failed `GetInHalf` and per village attack; can stall all packet processing | `:1652-1656`, `:1674-1704` |
| High | Performance | `Log()` calls `SetWindowText` synchronously | Every anomaly log blocks on the window manager | `Server.cpp:6725` |
| High | Performance | 46 rectangle passes × (cell walk + 1,000-slot scan) every 5 ticks | Dominant periodic cost; scales with population | `:1247-1337`, `Server.cpp:5548-5639` |
| High | Memory safety | `Pista[3].Party[3]` written (`Party` has 3 elements) | Aliases `Pista[4].Party[0]`, so a level-3 prize is granted to the wrong player | `:980`, `Basedef.h:646-656` |
| High | Memory safety | Unchecked `pMobGrid[TargetY][TargetX] = index` for idle mobs | Out-of-bounds write if a mob coordinate is corrupt | `:1373` |
| High | Correctness | `ProcessDecayItem` mutates `ItemCount` while iterating it | The loop walks the item table at double rate and can skip entries; affects all ground items | `Server.cpp:8355-8360` |
| Medium | Persistence | One account saved per 8 ticks with a cursor | Up to ~66 minutes of progress loss per account on a full server | `:169-199`; BR-11 |
| Medium | Persistence | Guild tax NPC files rewritten every 12 s with no dirty check | ~300 writes/min; a single unwritable file aborts the whole min-tick | `:2074-2104` |
| Medium | Configuration | `g_Hour` never assigned, so the child-hours window is always true | Forced logouts happen at all hours in `BILLING == 2` | `:178`, `Server.cpp:288` |
| Medium | Correctness | BR damage pass uses the loop variable `i` after its loop (`i == MAX_USER`) | Intended per-phase selection becomes "every 16 ticks" | `:1212` |
| Medium | Correctness | `rand()%1 == 0` in the rune-track exit teleport | Branch is dead; always one exit coordinate | `:790` |
| Medium | Correctness | Duplicate `Pista[4].Party[1]` immunity check, `Party[2]` never checked | Slot-2 parties are damaged inside their own room | `Server.cpp:5573-5582` |
| Medium | Correctness | Rune-track party-3 leader rune granted to `Party[0]` | Wrong recipient on a win | `:882` |
| Medium | Robustness | `AttackDieTeste.txt` `fopen` result used without null check | Null dereference on a read-only/full disk | `:1652-1656` |
| Medium | Robustness | `GetUserInArea` result `temp` unused; `strncpy` per matching user | Wasted copy per user per nightmare start check | `Server.cpp:4157-4176` |
| Medium | Coupling | ~105 external functions called from one 1,978-line function | Any signature change propagates; untestable in isolation | Section 6 |
| Medium | Maintainability | 8-level nesting, 273 `if`s, 2 `goto`s, 12 pragma regions, no sub-functions | Review and change risk; a single edit can reorder the world | Section 4.2 |
| Medium | Encoding | Portuguese comments stored in a non-UTF-8 code page render as mojibake (`sa\uFFFdda`, `n\uFFFdo`) | Grep/review friction; risk of corruption if the file is re-saved as UTF-8 | `:757`, `CReadFiles.cpp:597` |
| Low | Robustness | `Lista`-style index arithmetic without validation: `pItem[ipg]` in the gate loop | Reading outside `pItem` if the grid holds a stale index | `:2146-2153` |
| Low | Correctness | `UsersDie` incremented, never used; `Sec4` assigned then overwritten; `UNK = 1048576000` unused in three helpers | Dead state; misleading to readers | `:1104`, `:1106`, `:1554`, `Server.cpp:5450`, `:5507`, `:5600` |
| Low | Correctness | `ForceWeekMode` writes a discarded `struct tm` | GM command silently does nothing | `:2107-2108` |
| Low | Correctness | `summdam` computed for summoned attackers then discarded | Summon damage share is not applied | `:1804-1823` |
| Low | Dead code | `CEncampment` calls commented out; `CWarTower.h` included but unused here | Misleads dependency reasoning | `:1351`, `:38`, `:2045` |
| Low | Security | `pMobMerc` renew/reborn values read from file without range validation | Negative/1-tick values make vendors restock every tick | `Server.cpp:9346-9350` |
| Low | Security | Counter arithmetic mixes `unsigned int` (`SecCounter`) with `int` (`LastReceiveTime`) | Narrowing/wrap after ~68 years of uptime (theoretical) | `Server.cpp:4306-4307` |

### 9.2 Detailed defect notes (verified behaviours, not opinions)

**D-01 - MAC reload inversion (`:127-128`).** The guard `if(SecCounter % 120)` is truthy for every remainder except 0, so the file parse runs ~2.4 times per second. The sibling patterns in the same file (`% 120 == 0`, `% 1200 == 0`) establish the intended convention. Observable effect: constant CPU and I/O cost, and a hard dependency on the file being readable (open failure returns silently).

**D-02 - `ProcessDecayItem` index runaway (`Server.cpp:8355-8360`).** The function iterates `for (i = 0; i < ItemCount; i++)` and increments `ItemCount` *inside* the loop, wrapping at 5,000 to `g_dwInitItem + 1`. Because the bound is the same variable being incremented, the loop runs roughly `ItemCount` iterations while advancing the cursor twice per iteration; combined with the wrap, the effective scan window shrinks and some items are skipped on any given pass. Since the pass runs on `SecCounter % 2` (twice per second), the cursor travels the whole table ~4 times per second instead of once.

**D-03 - Rune-track prize aliasing (`:980`).** `STRUCT_PISTA` contains `Party[3]`, so `Pista[3].Party[3]` designates the memory of `Pista[4].Party[0]`. The level-3 party-2 branch therefore reads `Pista[4].Party[0].LeaderID` as the prize recipient; if that slot is empty the prize is silently dropped, and if it is occupied by another party the prize is granted to them.

**D-04 - Rune-track party-3 rune misdirection (`:882`).** Inside the `Pista[1].Party[2]` winner branch, the leader rune is granted with `PutItem(Pista[1].Party[0].LeaderID, &Runa)`; the party-3 leader never receives the leader rune (members still do, via the loop over `Party[2]`).

**D-05 - Leader-room immunity duplicate (`Server.cpp:5573-5582`).** `Party[1]` is tested twice and `Party[2]` never; a level-4 party registered in slot 2 is not immune inside its own room.

**D-06 - Dead `rand()%1` branch (`:790`).** `DoTeleport(x, 3294, rand()%1 == 0 ? 1701 : 1686)` always uses 1701 because `rand()%1` is 0 by definition. The comment-free intent was presumably `rand()%2`.

**D-07 - `i` leaked from the user loop (`:1212`).** `if (BrState == 2 && i % 8 == Sec16 && BRItem > 0)` evaluates `i == MAX_USER` (1000), so the condition is `Sec16 == 0`. This is the only place in the file where a loop variable is used after its loop.

**D-08 - Dead `ForceWeekMode` (`:2107-2108`).** See BR-54.

**D-09 - Unchecked `fopen` (`:1652-1656`).** `FILE *fpp = fopen("AttackDieTeste.txt", "a+"); fprintf(fpp, ...); fclose(fpp);` with no null test, unlike the neighbouring `Gethalf.txt` block which does check.

**D-10 - `pMobGrid` write without bounds check (`:1373`).** The idle-mob pass re-registers its own cell: `pMobGrid[pMob[index].TargetY][pMob[index].TargetX] = index;`. Every other grid access in the component either validates against `MAX_GRIDX/Y` (the `SendSignalParmArea`/`SendEnvEffect*` helpers) or is a read; this is the only unguarded write.

**D-11 - `AttackDieTeste` logic (`:1646-1658`).** The diagnostic fires when the target is inside a village (0..4), the map attribute lacks both `0x04` and `0x40`, and the attacker is an NPC - i.e. exactly the "mob attacks player in a safe zone" condition. It logs and then *continues to apply the attack*, so the diagnostic does not prevent the very anomaly it records.

**D-12 - `summdam` dead computation (`:1804-1823`).** The summoned-mount damage share inside a 46-tile box is computed into a local and never used, so summons deal no share damage.

**D-13 - `UsersDie` / `Sec4` / `UNK` dead locals.** `UsersDie` (`:1104`, incremented `:1188`, never read), the first `Sec4` assignment (`:1106`, overwritten at `:1554`), and `int UNK = 1048576000;` in `SendDamage`/`SendDamageKingdom`/`SendDamageLeader`.

**D-14 - `CloseUser` inside the flush loop followed by same-slot work (`:207-231`).** After a send failure closes session `bb`, the same iteration still calls `pMob[bb].ProcessorSecTimer()` (empty, harmless) and the Celestial recall check (not mode-guarded, see BR-19).

**D-15 - Nightmare start name probe (`:1040`).** `GetUserInArea(..., temp)` copies the last matching player's mob name into the shared `temp` buffer on every call (up to 1,000 `strncpy` per call) and then discards it; the buffer is also the global `temp` used by `Log`/`sprintf` elsewhere, so its contents are silently clobbered.

---

## 10. Test Coverage Analysis

### 10.1 Evidence of absence

The repository contains **no automated tests of any kind** for this component, or for any other. Evidence gathered by exhaustive search:

| Check | Result |
|-------|--------|
| Test projects in `W2PP Code Project.sln` | 2 projects only: `DBSrv`, `TMSrv` (`Select-String "Project("`) |
| Files matching `*test*`, `*spec*`, `*mock*`, `*fixture*`, `*assert*` (excluding ignored folders) | none |
| Files matching `gtest`, `catch2`, `cppunit`, `doctest`, `boost/test` | none |
| CI configuration (`.github`, `azure-pipelines`, `appveyor`, `travis`, `jenkins`) | none |
| `CMakeLists.txt`, `Makefile`, `*.props`, `*.targets` | none |
| Test-runner references in `.gitignore` (`MSTest`, `NUnit`, `Chutzpah`) | present only as boilerplate patterns copied from the Visual Studio template; no corresponding files exist |
| Build/verify scripts (`*.ps1`, `*.bat`, `*.sh`) | none in the repository root or under `Code/` |

### 10.2 Coverage by subsystem

Because there are no tests, the table records what *would* be the unit of test for each area and the observability that would make such a test possible today. "Coverage" is 0 % throughout; "Testability" records whether the code is separable without modification.

| Subsystem | Function(s) | Unit tests | Integration tests | Coverage | Testability | Test quality |
|-----------|-------------|-----------|-------------------|----------|-------------|--------------|
| Tick infrastructure | `ProcessSecTimer`, `ProcessMinTimer` | 0 | 0 | 0 % | Untestable in isolation: both read/write ~45 globals and call ~105 functions; no seams | N/A |
| Vitals | `ApplyHp`, `ApplyMp` | 0 | 0 | 0 % | Good: pure data functions with explicit return values; only `pUser`/`pMob` needed | N/A |
| Idle timeout | `CheckIdle` | 0 | 0 | 0 % | Medium: reads two globals, calls `CloseUser` | N/A |
| Affect ticking | `ProcessAffect` | 0 | 0 | 0 % | Poor: 900 lines, re-enters `ProcessClientMessage`, mutates 6 structures | N/A |
| Decay | `ProcessDecayItem` | 0 | 0 | 0 % | Medium: only items and the grid, but the counter runaway makes the observable state non-deterministic | N/A |
| Ranking | `ProcessRanking` | 0 | 0 | 0 % | Poor: depends on `RankingTime`, arena rectangles and global participant ids | N/A |
| Environmental damage | `SendDamage*` | 0 | 0 | 0 % | Good: rectangle + clan + HP arithmetic, one output packet | N/A |
| Rune track | `Pista` logic in `ProcessSecTimer` | 0 | 0 | 0 % | Very poor: 800 lines inline in the tick, `rand()` in the routing table, cross-module state | N/A |
| Secret room / water | `CartaTime`, `WaterClear1` | 0 | 0 | 0 % | Medium: pure counters, but live inside the tick body | N/A |
| NPC AI dispatch | `StandingByProcessor`, `BattleProcessor` | 0 | 0 | 0 % | Medium: bit-mask returns are testable as pure functions over `CMob` state | N/A |
| Combat resolution | tick combat block + `GetAttack` | 0 | 0 | 0 % | Poor: writes files, re-enters handlers, calls `MobKilled` | N/A |
| Generator scheduling | `MinuteGenerate` block | 0 | 0 | 0 % | Medium: pure arithmetic except `rand()` and `GenerateMob` | N/A |
| Calendar engine | `GuildProcess`, `CWarTower::GuildProcess` | 0 | 0 | 0 % | Poor: reads the clock inside, so tests would need clock injection that does not exist | N/A |
| Persistence | `SaveUser`, tax NPC write | 0 | 0 | 0 % | Poor: real file and socket I/O, no abstraction | N/A |

### 10.3 Testability assessment

The component is **structurally untestable without modification**, and the reasons are specific rather than stylistic:

1. **No parameters and no return values.** Both entry points are `void f(void)`; their entire input is global mutable state and the system clock, and their entire output is global mutable state plus network/file side effects. There is no observable result to assert on except by inspecting the world arrays.
2. **Time is implicit.** `SecCounter`/`MinCounter` are globals incremented inside the function; the calendar path calls `time()`/`localtime()` directly. A test cannot set "now" or "tick 120" without writing globals (`SecCounter` is `extern` in `Server.h:329`, so it *is* assignable - the only available seam).
3. **Side effects are non-idempotent.** Each tick sends packets (`GridMulticast`, `SendClientMessage`), writes files (`Log`, `AttackDieTeste.txt`, `Gethalf.txt`, `./npc/*`) and may close sessions. A test would need a socket and a filesystem harness.
4. **`rand()` is unseeded and untestable.** The portal routing table, dungeon prizes, weather, lottery percentages and rune selection all draw from the global PRNG with no injection point, so the same run cannot be reproduced.
5. **Cross-module state.** `Pista`, `CartaTime` and `WaterClear1` are written by `_MSG_UseItem`/`MobKilled` and read here, so an integration test needs the message layer, and a unit test needs those globals seeded by hand.
6. **No assertions anywhere in the codebase.** There is no `assert`, no invariant check, and no defensive validation of the state the tick consumes (mob coordinates, party indices, generator indices).

The only practical verification available today is **manual, in-game observation**, supplemented by the diagnostic files the component itself writes (`AttackDieTeste.txt`, `Gethalf.txt`) and the log stream. Those artifacts are evidence that the original authors verified behaviour this way.

---

## 11. Analysis Coverage and Limitations

**Coverage achieved:** every line of `Code/TMSrv/ProcessSecMinTimer.cpp` was read (all 2,314). The dispatch surface (`Server.cpp:3631-3632`, `:3723-3729`), the counter definitions (`Server.cpp:384-385`), and every service the component calls were read at their definitions to confirm behaviour rather than infer it (`ApplyHp`/`ApplyMp` `:8591-8687`, `CheckIdle` `:4304-4322`, `RegenMob` `:4362-4511`, `ProcessAffect` `:4513-5435`, `SendDamage*` `:5437-5639`, `ClearArea*` `:5730-5864`, `ProcessDecayItem` `:8353-8396`, `GuildProcess` `:6203-6660`, `ProcessRanking` `:7847-7926`, `DoRecall` `:7558-7602`, `GetUserInArea` `:4157-4176`), plus `CCastleZakum.cpp:314-375`, `CWarTower.cpp:42-145`, `CMob.cpp:51-304`, `CReadFiles.cpp:678-715`, `SendFunc.cpp:1561-1735`, `CPSock.cpp:617-666`, and `imple.cpp:651-658`/`:1121-1126`/`:1724-1736`.

**Known ambiguities, explicitly recorded rather than resolved:**

- `pUser[].Unk_2688` (reset at `:607`) has no other writer in the component and no documentation; its purpose could not be determined (LOW confidence).
- `pUser[].Unk_2732` / `Unk_2736` / `Unk_2728` are used as time stamps, PK/mount counters and a child-account flag respectively; the field names are decompiler artifacts and the semantics are inferred from usage (MEDIUM confidence).
- The `if (0 == 1)` guard around the Kefra attack block (`:1945`) may be a decompiler artifact of an original `if (KefraLive)` or similar; the original predicate is unknown (LOW confidence).
- `BASE_CheckFairyDate` and `BASE_CheckItemDate` are in `Basedef.cpp`; only their call contract (return true when expired) is asserted here, not their internals (MEDIUM confidence).
- The intended semantics of the mana-shield rescaling `((DamageNow >> 1) + (DamageNow << 4)) / 80` (`:1864`) were not derived; it is recorded as implemented.
- `BASE_GetWeekNumber`'s epoch and week definition were not traced; the castle-host parity rule is described exactly as coded (`wMod == 0 && (wDay % 2) == (ServerIndex % 2)`) without asserting what week 0 is.
- Where behaviour depends on the client protocol (for example the `+1024` bias in the RvR counter broadcast), only the server-side encoding is described.

**Explicitly out of scope for this component (analysed in their own reports):** the semantics of `GuildProcess`'s castle/BR/Colosseum/RvR machines (Server Core), the affect/combat rules inside `MobKilled` (MobKilled Resolution Module), the `CCastleZakum` and `CWarTower` event rules (their own components), and the socket transport (CPSock Transport).

---

*End of report.*
