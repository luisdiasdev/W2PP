# Component Deep Analysis Report - TMSrv Server Core

- **Component name:** TMSrv Server Core
- **Primary location:** `Code/TMSrv/Server.cpp` (9,449 lines, 214,293 bytes) and `Code/TMSrv/Server.h` (418 lines, 11,421 bytes)
- **Project scope analysed:** repository root `E:\wyd\test-setup\w2pp` (W2PP - decompiled C/C++ Win32 MMORPG server emulator)
- **Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`
- **Analysis date:** 2026-11-15
- **Analysis type:** read-only static analysis of source, project files and configuration contracts. No files were modified, no code was executed.

---

## 1. Executive Summary

### 1.1 Component purpose

`TMSrv Server Core` is the process-level spine of the TMSrv game/world server. It is the only translation unit in TMSrv that owns the Win32 process entry point (`WinMain`, `Code/TMSrv/Server.cpp:3459`), the window class and window procedure (`InitApplication` `:3392`, `InitInstance` `:3414`, `MainWndProc` `:3718`), and the process-wide definition of every world-state global that the other ~60 TMSrv translation units consume through `Code/TMSrv/Server.h`.

Functionally the component performs five distinct jobs that are physically interleaved into a single 9.4 kLOC file:

1. **Process host and event loop.** Creates the Win32 window, the "Game Server" console-style window with a menu bar, a Fixedsys font used as an in-window log console, an `App` message pump (`:3650`), and two `SetTimer`-driven clocks (`TIMER_SEC` at 500 ms and `TIMER_MIN` at 12,000 ms - `:3631`-`:3632`). All socket activity arrives as window messages posted by `WSAAsyncSelect` (`WSA_ACCEPT`, `WSA_READ`, `WSA_READDB`, `WSA_READBILL` - defined in `Code/CPSock.h:25`-`28`).
2. **Owner of world state.** Defines `CUser pUser[MAX_USER]` (1,000 slots), `CMob pMob[MAX_MOB]` (25,000 slots), `CItem pItem[MAX_ITEM]` (5,000 slots), the 4096x4096 spatial grids `pMobGrid` / `pItemGrid` / `pHeightGrid`, the 65,536-entry guild relation tables `g_pGuildWar` / `g_pGuildAlly` / `GuildInfo`, and approximately 150 scalar tuning globals (`:43`-`:449`). 65 of the 80 C/C++ translation units in the repository reference `pUser`; 64 reference `pMob`.
3. **Configuration loader.** `ReadConfig` (`:761`) parses `gameconfig.txt` through a strict positional contract; `DrawConfig` (`:1117`) both renders and regenerates that file; `ReadLevelItemConfig` (`:8730`) parses `LevelItem.txt`; `WinMain` reads `localip.txt`, resolves the server identity against `g_pServerList` (serverlist.txt) and optionally reads `biserver.txt`.
4. **Gameplay service library.** ~110 free functions that other modules call: teleport/recall/summon, guild challenge and war lifecycle, castle siege, Battle Royale, Colosseum scheduling, RvR kingdom war, duel/ranking, mob creation and NPC generation, item creation/decay, affect and tick (buff/debuff) application, mount handling, HP/MP application, treasure map combination, logging. These are the *shared services* of TMSrv, not a self-contained subsystem: `Server.h` is the widest public header in the server.
5. **Scheduler.** `MainWndProc` routes `WM_TIMER` to `ProcessSecTimer` / `ProcessMinTimer` (`Code/TMSrv/ProcessSecMinTimer.cpp:40` and `:2019`, out of this component's file boundary but driven exclusively from `MainWndProc:3723`-`3729`), which in turn drive per-mob processing, round-robin persistence, the calendar-driven event state machines implemented here (`GuildProcess` `:6203`).

### 1.2 Architectural role in the system

The surrounding architecture report describes TMSrv as the game/world server (DBSrv owns accounts/characters, BISrv owns billing, `Code/Basedef.*` and `Code/CPSock.*` are shared). This component is the **de facto application core** of TMSrv: it is both the composition root (it wires DBSrv, Billing, the listening socket, the configuration, and the world arrays together) and the **global namespace** every other TMSrv module links against. Afferent coupling is extreme (65 dependent TUs for `pUser`, 53 for `Log`), efferent coupling is comparatively low but concentrated on five external contracts (DBSrv socket, Billing socket, client socket, configuration files, Win32 API).

### 1.3 Key findings

- **No automated tests exist anywhere in the repository.** Verified by pattern search for test sources, test frameworks, test projects, CI configuration and build scripts (evidence in Section 11). The component is entirely unverified by automation, including its 941-line item-bonus generator and its event schedulers.
- **All business logic is time-coupled to two hardcoded timer resolutions** (500 ms and 12,000 ms) while the code is written in terms of "seconds" and "minutes". `SecCounter` (`:384`) is *not* a seconds counter - it increments once per 500 ms tick (`:3631`, `ProcessSecMinTimer.cpp:123`), so every timeout expressed in `SecCounter` units (idle disconnect, round-robin save, PK-point accrual) is half the nominal wall-clock duration. `MinCounter` likewise advances every 12 s, not every 60 s.
- **Configuration is fail-open.** `ReadConfig` (`:761`) returns early on any contract violation after showing a `MessageBox`, leaving a partially applied configuration, while `WinMain` unconditionally sets `ConfigReady = 1` (`:3486`) and continues booting.
- **One enormous, table-driven, undocumented algorithm dominates the file**: `SetItemBonus` (`:1393`-`:2333`, 941 lines, nesting depth >= 6) generates random item bonuses from `nPos`, `nUnique`, `Level`, `a3` and `DropBonus`. It is called from 5 TUs and is the single highest-risk unit in the component.
- **Several verified defect classes are present** (not stylistic opinions): operator-precedence error in mount feed computation (`:4443`), self-assignment dead code in summon creation (`:2757`), source/destination overlap in `sprintf` inside `DrawConfig` (`:1279`), unbounded index derivation from an on-disk text file in `ReadLevelItemConfig` (`:8774`-`:8799`), inverted modulo guard for MAC-file reloading (`ProcessSecMinTimer.cpp:127`), and a runaway indexer in `ProcessDecayItem` (`:8355`-`:8360`).
- **Dead/stub surface is significant**: `GetAFont` (`:756`), `BuildList` (`:3713`), `WriteArmor` (`:3455`), `ProcessBILLMessage` (`:4300`), `IsImple` (`:6829`), `SendBilling` (`:1377`) are empty or constant; `ParseCastleString` and `ParseCampString` are declared in `Server.h:182`/`:184` but defined nowhere in the repository.
- **Security posture is perimeter-less inside the trust boundary.** The component trusts the DB server, the billing server and the client for many invariants; the only anti-tamper mechanism it owns is the `NumError` "crack point" accumulator (`AddCrackError` `:684`) with a 2x109 threshold, and its arithmetic is signed-`int` and can overflow.

---

## 2. Data Flow Analysis

### 2.1 Boot / composition flow

```
1. OS loader -> WinMain (Server.cpp:3459)
2. InitApplication (:3392) registers WNDCLASS "MainClass" -> MainWndProc
3. InitInstance (:3414) creates 900x600 "Game Server" window + HMENU
4. CreateFont "Fixedsys" 12pt -> hFont (in-window log console)
5. BASE_InitModuleDir() + BASE_InitializeHitRate()  [shared Basedef.cpp]
6. StartLog() (:6654) + StartChatLog() (:9208) -> ./Log/*.txt handles
7. memset(g_pTreasure), ReadConfig() (:761) <- gameconfig.txt
8. ReadLevelItemConfig() (:8730) <- LevelItem.txt
9. ConfigReady = 1 ; DrawConfig(0) (:1117) renders config into window
10. memset(g_pGuildWar/g_pGuildAlly/GuildInfo/pMac/pMobMerc)
11. ListenSocket.WSAInitialize() (:3496)  [CPSock.cpp, WSAStartup + WSAAsyncSelect]
12. Reboot() (:7127): clear 4096x4096 grids, reset towers, MobName.txt,
    NPC generator list, heightmap.dat (16 MB), BASE_InitializeBaseDef,
    BASE_ApplyAttribute, Language.txt, summon table, init-item placement
13. SetCastleDoor(1) (:6060)
14. localip.txt -> LocalIP[4]; match against g_pServerList -> ServerGroup/ServerIndex
    + DBServerAddress   (fatal MessageBox if no match, :3551)
15. DBServerSocket.ConnectServer(DBServerAddress, 7514, LocalIP, WSA_READDB)
    (fatal if NULL, :3564)
16. NumServerInGroup = count of configured servers, clamped to [1..10] (:3572-3582)
17. Optional biserver.txt -> BillServerSocket.ConnectBillServer(...) ; BILLING=0 on failure
18. SetColoseumDoor(1) / SetColoseumDoor2(3)
19. CurrentTime = timeGetTime() (:3625)
20. CReadFiles::CReadFiles() + CCastleZakum::ReadCastleQuest()
21. SetTimer(hWndMain, TIMER_SEC, 500) ; SetTimer(hWndMain, TIMER_MIN, 12000)
22. ListenSocket.StartListen(hWndMain, LocalIP, GAME_PORT=8281, WSA_ACCEPT)
23. GuildZoneReport() -> DBServerSocket (:739)
24. If KefraLive==0: GenerateMob(KEFRA_MOB_INITIAL..END) + GenerateMob(KEFRA_BOSS)
25. GetMessage loop (:3650)  -> DispatchMessage -> MainWndProc
```

### 2.2 Client request flow (steady state)

```
1. Kernel completes accept -> WSA_ACCEPT message -> MainWndProc:4006
2. GetEmptyUser() (:6743) scans pUser[1..999] for Mode==USER_EMPTY
3. pUser[User].AcceptUser(ListenSocket.Sock)            [CUser.cpp]
4. Admin-reserved tail check (User >= MAX_USER-ADMIN_RESERV) -> reject (:4022)
5. ServerDown gate: if ServerDown != -1000 -> reject with _NN_ServerReboot_Cant_Connect
6. Client bytes -> WSA_READ message -> MainWndProc:3919
7. GetUserFromSocket(wParam) (:6729) maps socket handle -> pUser index
8. WSAGETSELECTEVENT != FD_READ -> CloseUser(User) (:3934)
9. pUser[User].cSock.Receive() (+ one retry, then -1 path closes user)
10. Loop pUser[User].cSock.ReadMessage(&Error,&ErrorCode) until NULL
11. Error 1/2 -> log "err,read" and break
12. ProcessClientMessage(User, Msg, FALSE)              [ProcessClientMessage.cpp:38]
13. Message-type switch -> Exec_MSG_* handler (_MSG_*.cpp)
14. Handler mutates pUser/pMob/pItem/pItemGrid/pMobGrid and/or posts DB requests
15. Responses: pUser[].cSock.AddMessage / SendOneMessage, or GridMulticast(x,y,...)
16. Outbound bytes flushed by ProcessSecTimer -> cSock.SendMessageA() (ProcessSecMinTimer.cpp:207)
```

### 2.3 Timer-driven flow (authoritative scheduling path)

```
WM_TIMER / TIMER_SEC (every 500 ms) -> ProcessSecTimer (ProcessSecMinTimer.cpp:40)
  - graceful-shutdown drain (ServerDown state machine, one user per tick)
  - SecCounter++  (2 ticks == 1 real second)
  - CurrentTime = timeGetTime()
  - CReadFiles::ReadMacblock() on (SecCounter % 120) != 0   <-- inverted guard
  - Billing reconnect countdown (BillCounter, 360 ticks)
  - every 8th tick: round-robin SaveUser() over pUser (or forced logout for
    BILLING==2 child accounts above FREEEXP)
  - per-user: SendMessageA flush, CMob::ProcessorSecTimer(), nightmare-zone recall
    for Celestial >=180, Balrog rune-portal routing, per-mob affect processing
WM_TIMER / TIMER_MIN (every 12,000 ms) -> ProcessMinTimer (ProcessSecMinTimer.cpp:2019)
  - daily log rotation, NewbieEventServer/CastleServer rotation (MinCounter % 2)
  - guild tax NPC persistence (./npc/<name>)
  - CCastleZakum::ProcessMinTimer()
  - kingdom clear two-phase (Kingdom1Clear/Kingdom2Clear)
  - FailAccount reset every 10 ticks
  - castle gate re-lock loop over g_pInitItem[17..g_dwInitItem]
  - NPC generator: MinuteGenerate modulo -> GenerateMob(index) (+ dungeon prize spawn)
  - MinCounter++
  - weather roll (rand()%1200) unless ForceWeather != -1
  - GuildProcess()  (:6203)  <- guild war / BR / Colosseum / RvR / castle calendar
```

### 2.4 DB server flow

```
DBSrv bytes -> WSA_READDB -> MainWndProc:3812
  - non-FD_READ or Receive()==FALSE -> CloseSocket, 2 reconnect attempts (200 ms apart),
    Sleep(200), PostQuitMessage (process termination)
  - loop DBServerSocket.ReadMessage() -> ProcessDBMessage(Msg) [ProcessDBMessage.cpp:39]
Outbound DB traffic from this component:
  - GuildZoneReport (:739)            _MSG_GuildZoneReport  (g_pGuildZone[].ChargeGuild)
  - CloseUser (:6834)                 _MSG_DBNoNeedSave | _MSG_SavingQuit
  - SaveUser (:7056)                  _MSG_DBSaveMob (MOB, Cargo, Coin, Donate, Slot,
                                      ShortSkill, affect, extra, AccountName)
  - Challange (:2479)                 _MSG_GuildInfo (fame after -100)
  - ProcessRanking (:7847)            _MSG_MessageDBRecord ("RNK <server> ...")
  - CombineTreasureMap (:8398)        _MSG_MessageDBRecord ("ETC treasure_map ...")
```

### 2.5 Billing flow

```
biserver.txt -> BillServerAddress/BillServerPort -> ConnectBillServer(WSA_READBILL)
WSA_READBILL -> MainWndProc:3731 -> ReadBillMessage loop -> ProcessBILLMessage (:4300)
   *** ProcessBILLMessage is an empty function body: messages are dequeued and discarded ***
Outbound: SendBilling2 (:1382) writes server id "ServerIndex + 10*ServerGroup" into the
   _AUTH_GAME payload and pushes it through BillServerSocket.SendBillMessage.
Failure paths: socket close + BillCounter=360 -> periodic reconnect in ProcessSecTimer;
   terminal failure sets BILLING = 0 (billing disabled, server keeps running).
```

### 2.6 Shutdown flow

```
WM_CLOSE (:4116) -> MessageBox confirm
  -> if BILLING != 0: SendBilling2(...,4) and BILLING = 0
  -> CReadFiles::WriteGuild()   (guild + challenger persistence)
  -> fclose(fLogFile) ; DeleteObject(hFont)
  -> DefWindowProc -> WM_DESTROY (:4145) -> WSACleanup() + PostQuitMessage(0)
Alternative graceful path (menu "Sistema > Reboot" -> IDC_REBOOT :4093):
  ServerDown = 1 -> ProcessSecTimer counts to 120 (notices every 20 ticks)
  -> at 120 switches TIMER_SEC to 200 ms and closes one user per tick until UserCount
     reaches MAX_USER -> DeleteObject(font) -> PostQuitMessage
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Configuration contract | `gameconfig.txt` must match a positional line/token contract; mismatch aborts load | `Code/TMSrv/Server.cpp:761`-`:1115` |
| BR-02 | Configuration default | Missing `gameconfig.txt` -> defaults plus auto-generation via `DrawConfig(TRUE)` | `Server.cpp:766`-`:775`, `:1117` |
| BR-03 | Configuration generation | The server rewrites its own config file from in-memory state | `Server.cpp:1146`, `:1157`-`:1323` |
| BR-04 | Validation | Drop-bonus matrix values must be in [0..9999], 64 slots in 4 lines of 16 | `Server.cpp:938`-`:1014` |
| BR-05 | Validation | Party bonus must be in [50..200], otherwise silently reset to 100 | `Server.cpp:924`-`:925` |
| BR-06 | Validation | Treasure settings: 8 records, each 1 source + 5 targets (item + 2 effects) + 5 rates | `Server.cpp:1027`-`:1063` |
| BR-07 | Identity resolution | `localip.txt` must appear in `g_pServerList[*][1..]`; resolves ServerGroup/ServerIndex and DB address; otherwise boot aborts | `Server.cpp:3509`-`:3556` |
| BR-08 | Connectivity | DB port is fixed at 7514; DB connection failure is fatal | `Server.cpp:3558`-`:3570` |
| BR-09 | Resilience | DB disconnect -> 2 reconnect attempts 200 ms apart, then process termination | `Server.cpp:3815`-`:3847`, `:3850`-`:3881` |
| BR-10 | Resilience | Billing is optional: connect failure or disconnect disables billing without stopping the server | `Server.cpp:3599`-`:3618`; `:3735`-`:3742` |
| BR-11 | Environment specialisation | Hardcoded DB IPs `192.168.0.153` -> TESTSERVER, `192.168.0.50` -> LOCALSERVER | `Server.cpp:3584`-`:3588` |
| BR-12 | Capacity | Servers-in-group counter clamped to [1..10] | `Server.cpp:3572`-`:3582` |
| BR-13 | Scheduling | Two fixed timers: 500 ms (logic tick) and 12,000 ms (calendar tick); shutdown switches logic tick to 200 ms | `Server.cpp:3631`-`:3632`, `:115` in `ProcessSecMinTimer.cpp` |
| BR-14 | World init | Boot spawns the Kefra chain (396 boss, 397..399) only when `KefraLive == 0` | `Server.cpp:3638`-`:3644` |
| BR-15 | Admission control | User slots 1..999; last `ADMIN_RESERV`=10 slots are admin-reserved and rejected | `Server.cpp:4011`-`:4030`; `Code/Basedef.h:56`-`:58` |
| BR-16 | Admission control | While a shutdown is in progress (`ServerDown != -1000`) all new connections are refused | `Server.cpp:4032`-`:4039` |
| BR-17 | Anti-tamper | "Crack point" accumulator; types 3/8/15 are not logged; threshold 2,000,000,000 forces logout | `Server.cpp:684`-`:707` |
| BR-18 | Session timeout | Idle disconnect based on `LastReceiveTime` vs `SecCounter` (720/1440 tick correction) | `Server.cpp:4304`-`:4322` |
| BR-19 | Login failure tracking | Circular table of 16 failed accounts, cleared every 10 calendar ticks | `Server.cpp:1334`-`:1361`; `ProcessSecMinTimer.cpp:2136` |
| BR-20 | Persistence | Round-robin save of one user every 8 ticks; child-account billing gate at level >= FREEEXP outside 12:00-19:00 | `ProcessSecMinTimer.cpp:169`-`:199` |
| BR-21 | Persistence | Character logout persists score, cargo, coin, donate, short skills, affects, extra | `Server.cpp:7085`-`:7125` |
| BR-22 | Persistence | Socket close either requests no-save or full save depending on user mode; trade/party cleanup always performed | `Server.cpp:6834`-`:6926` |
| BR-23 | Validation | Item grant requires valid conn, `USER_PLAY`, and a free carry slot within `MaxCarry <= 64` | `Server.cpp:709`-`:737` |
| BR-24 | Item creation | Item index in [1..6499], target grid cell empty, free item slot, Delay 90, State OPEN | `Server.cpp:7254`-`:7317` |
| BR-25 | Item creation | Treasure box: Mode 2, Delay 0, State parameterised, Height forced to -204 | `Server.cpp:9392`-`:9449` |
| BR-26 | Item lifecycle | Ground-item decay walker over `ItemCount`, skips item 1727, honours Delay then Decay | `Server.cpp:8353`-`:8396` |
| BR-27 | Item generation | `SetItemBonus` random-bonus algorithm (level delta /25, grade effects, drop-bonus divisor, per-slot tables) | `Server.cpp:1393`-`:2333` |
| BR-28 | Item generation | `SetItemBonus2` per-slot bonus for helmets/armour with sanctuary clamp of 6 | `Server.cpp:2335`-`:2478` |
| BR-29 | Reward | `LevelItem.txt` defines level-up rewards per class/build; `cls=4`/`type=4` act as wildcards | `Server.cpp:8730`-`:8803` |
| BR-30 | Reward | Build type derived from highest base stat (STR/INT/DEX/else CON) | `Server.cpp:8805`-`:8853` |
| BR-31 | Reward | Circlet sub-god bonuses at Celestial levels 120/150/180/199, once each, slot 1 required | `Server.cpp:8855`-`:8912` |
| BR-32 | Guild | Crown / dragon insignia ownership is reconciled on demand (item 747, 3993, 3994; clan 7/8) | `Server.cpp:494`-`:682` |
| BR-33 | Guild | Challenge eligibility: NPC target, zone = mob base level, guild master only, not incumbent, correct citizen server, named guild, fame >= 100 | `Server.cpp:2479`-`:2570` |
| BR-34 | Guild | Challenge consumes exactly 100 fame and only replaces the challenger when `pChallangerMoney[zone]` is lower | `Server.cpp:2547`-`:2567` |
| BR-35 | Guild | Weekly war state machine `WeekMode` 0..5 gated by `GuildDay`/`GuildHour`, with arena door and notice transitions | `Server.cpp:6511`-`:6607` |
| BR-36 | Guild | War winner = guild with the greater sum of surviving member levels inside the war rectangle; draw keeps the incumbent | `Server.cpp:6108`-`:6201` |
| BR-37 | Guild | `Victory` counter capped at 4; zones 3 and 4 are excluded from resolution | `Server.cpp:6110`-`:6113`, `:6192`-`:6193` |
| BR-38 | Guild | Zone 4 (castle) is awarded to one guild only when it owns zones 0,1,2 and 3 simultaneously | `Server.cpp:6199`-`:6200` |
| BR-39 | Guild | Allies: one ally per guild, toggle semantics, broadcast to members | `Server.cpp:7697`-`:7753` |
| BR-40 | Guild | Wars require mutual declaration; `target == 0` cancels | `Server.cpp:7604`-`:7695` |
| BR-41 | Guild | Guild teleport caps: 30 members then offset (+1,+1), 45 members then (-2,-2), stop at `maxsummon` | `Server.cpp:5641`-`:5728` |
| BR-42 | Castle | Siege window at hour 21; states 1..3 (announce, open, close) keyed to minutes 5/50/55 | `Server.cpp:6223`-`:6282` |
| BR-43 | Castle | Capture requires 22 consecutive `RegenMob` ticks on tile (1046,1690) as guild master of a non-owning guild that owns another city | `Server.cpp:4362`-`:4413` |
| BR-44 | Battle Royale | `BrState` 0..3 with 20-minute buckets (`tm_min/20`, `tm_min%20`), door control, level-band area purge, prize item at minute 13 offset | `Server.cpp:6286`-`:6384` |
| BR-45 | Colosseum | `ColoState` 0..8 with door/mob generation steps at minutes 3,4,5,7,9,11,13,15; newbie hour uses ids 5..7 | `Server.cpp:6386`-`:6502` |
| BR-46 | Colosseum | Level-150 cap zone purge during newbie hour (`Colo150Limit`) | `Server.cpp:6499`-`:6500` |
| BR-47 | Events | Newbie event server rotates by `(tm_mday-1) % NumServerInGroup`; spawns mob 22 at minute 55 | `ProcessSecMinTimer.cpp:2038`-`:2057`; `Server.cpp:6503`-`:6510` |
| BR-48 | Events | Castle-host server rotates on week parity (`wMod==0` and day parity vs ServerIndex) | `ProcessSecMinTimer.cpp:2062`-`:2069` |
| BR-49 | RvR | Kingdom war state machine 1..3 on a weekday evening hour, tower mob spawn/cleanup, points reset | `Server.cpp:6609`-`:6650` |
| BR-50 | Duel | 1v1 (`tDuel=0`) alternates ranks 1v2 with 121-tick clock; team duels 5/10/100 use 301-tick clock | `Server.cpp:8052`-`:8167` |
| BR-51 | Ranking | Arena resolution after countdown: compare surviving participant counts, broadcast result, DB record, purge arena | `Server.cpp:7847`-`:8050` |
| BR-52 | Ranking | Arena visual/sound overlays at remaining-time buckets 60/120/180 | `Server.cpp:7896`-`:7942` |
| BR-53 | Movement | Teleport uses empty-grid check; great teleport uses a wider search; summon does not validate the grid result | `Server.cpp:8212`-`:8268` |
| BR-54 | Movement | Recall destination: city by `(Merchant & 0xC0)>>6`, castle spawn for owning guilds, newbie override (2112,2042) for MORTAL below FREEEXP | `Server.cpp:7558`-`:7602` |
| BR-55 | Combat | Battle assignment requires both actors alive, not identical, NPC attacker or playable target, and <=23 tiles per axis | `Server.cpp:7218`-`:7252` |
| BR-56 | Combat | Mob skill 109 randomly remaps to 110 (<40) or 111 (<80); non-109 damage rolled as 500 + rand()%200 | `Server.cpp:8927`-`:8958` |
| BR-57 | Combat | Parry: `GetParryRate(Dex/5 + 100 if learned 0x1000000 + 500 if Rsv&0x40)`; skills 79/22 use 30 % parry; parry yields damage -3, or -4 if `Rsv&0x200` and roll < 100 | `Server.cpp:8985`-`:9012` |
| BR-58 | Combat | Clan 4 (mount) attackers deal 40 % damage | `Server.cpp:9010`-`:9011` |
| BR-59 | Affects | `SetAffect` refuses merchants and aggressive effects on `RSV_BLOCK`; duration `(AffectTime+1)*time/100`; types 1/3/10 forced to 4; cap 2139062143 | `Server.cpp:8270`-`:8315` |
| BR-60 | Affects | `SetTick` duration `Delay*(AffectTime+1)/100`, capped at 500,000,000, types 1/3/10 forced to 2 when >=3 | `Server.cpp:8317`-`:8351` |
| BR-61 | Affects | Hide (type 28) and Samaritan (type 24) removal recomputes score and re-sends it | `Server.cpp:8168`-`:8210` |
| BR-62 | Affects | Per-tick affect processing: HP aura regen (type 17), poison drain (type 20), fada auto-attack (items 753/769/1726) | `Server.cpp:4513`-`:5436` |
| BR-63 | Mounts | Baby mounts: item 2320..2359 with `EF_MOUNTHP > 0` spawns a summon; adult mounts 2360..2389 tracked by HP in effect slot 0 | `Server.cpp:4178`-`:4298` |
| BR-64 | Mounts | Mount HP is mirrored into the rider's equip slot 14 when face matches (315..344) | `Server.cpp:4223`-`:4261` |
| BR-65 | Mounts | Feeding: -4 per 450 ticks when `MountDiv > 15`, else -2; at feed <=1 the mount dies (HP 0, feed 4) | `Server.cpp:4440`-`:4474` |
| BR-66 | PvP | PK point increments by 1 every 450 ticks up to 75; guilty counter decrements per tick and clears mob aggression at zero | `Server.cpp:4415`-`:4509` |
| BR-67 | NPC | `CreateMob` requires a free mob slot and readable `./<folder>/<name>` record; name `_` -> space, `@` -> terminator | `Server.cpp:2865`-`:2993` |
| BR-68 | NPC | Spawn scatter is +/-2 tiles unless the NPC face is 219/220/358; door 219 inherits the castle guild; WaitSec 10 (type 0) or 40 | `Server.cpp:2901`-`:2945` |
| BR-69 | NPC | Under the newbie event, NPCs below level 120 spawn at 75 % HP | `Server.cpp:2942`-`:2943` |
| BR-70 | NPC | Generator cadence uses `MinCounter % MinuteGenerate` with dynamic re-randomisation bands (500-999, 1000-1999, 2000-3799, >=3800) | `ProcessSecMinTimer.cpp:2212`-`:2280` |
| BR-71 | NPC | Dungeon event (probability band >=3800) also spawns 5-9 random dungeon items with generated bonuses | `ProcessSecMinTimer.cpp:2249`-`:2276` |
| BR-72 | NPC | Deletion decrements the generator's `CurrentNumMob` (floor 0) and clears HP/mode/grid/party | `Server.cpp:7020`-`:7054` |
| BR-73 | Area purge | `ClearArea*` affect only `USER_PLAY` players inside the rectangle; dead players are revived with 2 HP before teleport/recall | `Server.cpp:5730`-`:5866` |
| BR-74 | Summoning | `GenerateSummon` refuses when no NPC slot/free party slot, honours per-skill duplicates and party face checks, scales stats by `pSummonBonus` | `Server.cpp:2572`-`:2864` |
| BR-75 | Economy | Treasure map combination requires items 788/789/790/791/792/793 at relative offsets 0/1/9/10/18/19 | `Server.cpp:8398`-`:8507` |
| BR-76 | Economy | Fallback: any carry item in 788..793 becomes 485 and is counted; no match returns 0 | `Server.cpp:8488`-`:8506` |
| BR-77 | Economy | `Combine` needs 10 units, honouring effect 61 stacked quantities, and grants randomised effects 59 | `Server.cpp:8509`-`:8589` |
| BR-78 | Vitals | `ApplyHp`/`ApplyMp` add at most 2,000 per call, cap at 2x109, clamp to MaxHp/MaxMp; negative requests clamped to 0 | `Server.cpp:8591`-`:8687` |
| BR-79 | Free-to-play | `IsFree` returns true when FREEEXP <= 0 or any character on the account is >= FREEEXP and < 999 | `Server.cpp:1363`-`:1375` |
| BR-80 | Lookup | Name lookup: `+name` prefix means account name (uppercased), otherwise character name; only `USER_PLAY` sessions | `Server.cpp:6793`-`:6827` |
| BR-81 | Admin | Menu commands: ReloadMob, Reboot, SaveAll, ReadGuild, ReadGuildName, ReadGameConfig, Exit | `Server.cpp:4055`-`:4115` |
| BR-82 | Admin | Shutdown requires explicit confirmation; billing logout, guild flush, log close, font delete are mandatory steps | `Server.cpp:4116`-`:4148` |
| BR-83 | Logging | One log file per boot named `TM_dd_mm_yyyy_hh_mm_ss.txt`; the window title acts as a live tail | `Server.cpp:6654`-`:6727` |
| BR-84 | Logging | Daily rotation of main/chat/item logs plus per-IP and per-account prefixes | `ProcessSecMinTimer.cpp:2027`-`:2034`; `Server.cpp:6701`-`:6727` |
| BR-85 | World reset | `Reboot()` clears grids/towers, reloads MobName/NPC/heightmap/BaseDef/Language/summons, re-places init items with keys 1..14 locked | `Server.cpp:7127`-`:7207` |
| BR-86 | Guild persistence | Presenca opcional: `GuildZoneReport` pushes the 5 zone owners to DBSrv | `Server.cpp:739`-`:754` |
| BR-87 | Geometry | `GetAngle` = atan2*3600/2pi normalised to [0..3600]; `GetLength` clamps squared distance at 100 | `Server.cpp:6936`-`:6972` |
| BR-88 | Capacity | Challenge money table and `pChallangerMoney[6]` are cleared together with challenge guilds | `Server.cpp:7209`-`:7216` |
| BR-89 | Client gate | Clients sending `_MSG_UpdateScore` are treated as tampering (+2 crack points) | `ProcessClientMessage.cpp:106`-`:110` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01/02/03 - Configuration contract, fail-open default and self-generation

**Overview:**
`ReadConfig` (`Code/TMSrv/Server.cpp:761`) is the single entry point for tuning the server. It parses `gameconfig.txt` through a strict positional grammar: named section headers followed by fixed-order token/value lines. `DrawConfig` (`:1117`) is the mirror operation - it renders the same data into the window and, when called with `wb = TRUE`, rewrites the entire file. The pair therefore defines a bidirectional configuration contract that the server itself authors.

**Detailed description:**
The file is expected to contain, in order: `Drop Item Event Settings:` with `evindex/evdelete/evon/evitem/evrate/evstart` plus two extra integers and `evnotice` (`:799`-`:829`); `Etc Event Settings:` carrying `double/deadpoint/dungeonevent/statsapphire` and a battleroyal item (`:849`-`:875`); `Billing Settings:` carrying `billmode/freeexp/charselbill/potioncount/partybonus/guildboard` (`:895`-`:922`); `Item Drop Bonus Settings:` followed by four lines of sixteen integers (`:936`-`:1014`); `Treasure Settings:` with eight records of one source, five target item/effect quadruples and one line of five rates (`:1027`-`:1063`); and finally `Etc Settings:` carrying `partydif/kefrastatus/GTorreHour/RVRHour/DropItem/BRHour/maxNightmare/PotionDelay` (`:1083`-`:1112`). Every section validates both the *labels* (`strcmp` against literal strings) and the *numeric sentinel* (`value[i] == -1`), the latter being how the parser detects that `sscanf` failed to supply a field.

The failure policy is the rule's most consequential property. Each violation raises a modal `MessageBox` and returns from `ReadConfig` **without reverting previously assigned globals**, so a file that is valid up to the treasure section but corrupt in the `Etc Settings` section leaves drop bonuses, billing options and treasure tables applied and the remaining settings at their compiled defaults. `WinMain` then sets `ConfigReady = 1` and continues (`:3486`), so the server boots with a mixed configuration and no log entry describing the inconsistency. Only the completely missing file is handled gracefully: a message box explains that defaults will be generated and `DrawConfig(TRUE)` writes a fresh file (`:768`-`:773`).

Two further properties matter operationally. First, the file can be re-read at runtime from the menu (`IDC_READGAMECONFIG`, `:4109`), which re-runs the same fail-open path against a live world holding players. Second, because `DrawConfig` serialises the in-memory values, any value that was clamped in memory (for example `PARTYBONUS`, BR-05) is silently normalised the next time the file is written, meaning the on-disk file is not a faithful record of what the operator intended.

**Rule workflow:**
```
open gameconfig.txt
  | not found -> MessageBox("no gameconfig.txt ...") -> ConfigReady=1 -> DrawConfig(TRUE) -> return
  v
line 1 must equal "Drop Item Event Settings:\n"  else MessageBox + return
line 2: sscanf 6 name/value pairs + 3 scalars
        labels must be evindex/evdelete/evon/evitem/evrate/evstart else return
        any parsed value == -1 -> return
        assign evIndex..evNotice
line 3 must equal "Etc Event Settings:\n"          else return
line 4: double/deadpoint/dungeonevent/statsapphire (+battleroyal)
        assign DOUBLEMODE, DUNGEONEVENT, DEADPOINT, StatSapphire, BRItem
line 5 must equal "Billing Settings:\n"            else return
line 6: billmode/freeexp (+4 more)
        assign BILLING, FREEEXP, CHARSELBILL, POTIONCOUNT, PARTYBONUS, GUILDBOARD
        clamp PARTYBONUS to 100 when outside [50..200]
line 7 must equal "Item Drop Bonus Settings:\n"    else return
lines 8..11: 16 integers each; each must be in [0..9999] else return
        -> g_pDropBonus[0..15], [16..31], [32..47], [48..63]
line 12 must equal "Treasure Settings:\n"          else return
8 x (record line + rate line) -> g_pTreasure[0..7]
line must equal "Etc Settings:\n"                  else return
line: partydif/kefrastatus/GTorreHour/RVRHour/DropItem/BRHour/maxNightmare/PotionDelay
        labels checked, values checked for -1
        assign PARTY_DIF, KefraLive, GTorreHour, RvRHour, isDropItem, BRHour,
               maxNightmare, PotionDelay
close file
```
*Confidence: HIGH - the token order, labels and sentinels are all explicit in code.*

---

### Business Rule: BR-04 - Drop-bonus matrix validation

**Overview:**
`g_pDropBonus[64]` (`Code/Basedef.h:2448`, defined `Code/Basedef.cpp:204`) is the server-wide economy multiplier matrix consumed by `SetItemBonus` and every drop path. `ReadConfig:938`-`:1014` is its only writer from configuration.

**Detailed description:**
The rule is expressed as four repeated blocks. Each block reads one line of sixteen integers with a single `sscanf` of sixteen `%d` conversions (`:938`). It then iterates the sixteen values and rejects the *entire file* if any single value is outside the inclusive range `[0..9999]` (`:942`, `:961`, `:982`, `:1002`). Note the asymmetry with the failure policy of BR-01: the earlier blocks have already written `g_pDropBonus[0..15]` (and possibly `[16..31]`, `[32..47]`) before a later block fails, so the matrix can end up half-updated. The loop also resets each `value[k]` to `-1` after assignment (`:952`, `:973`, `:993`, `:1013`), a defensive reuse pattern that has no effect on the file.

The 64 slots are consumed positionally: `SetItemBonus` computes `addDropBonus = DropBonus / 8` clamped to `[0..2]` and uses that to shrink the modulo divisor of the bonus roll (`:1395`-`:1401`, `:1466`, `:1483`). Consequently the matrix is not a lookup table by item type but a *bounded bonus level* whose index is derived by dividing the caller's `DropBonus` by 8. Values above 9999 are meaningless in this arithmetic and are rejected; values of 0 are legal and mean "no bonus".

**Rule workflow:**
```
for block in 0..3:
    read line -> 16 ints into value[0..15]
    for k in 0..15:
        if value[k] < 0 or value[k] > 9999:
            MessageBox("... Line8/9/10/11") ; fclose ; return   (partial state retained)
        g_pDropBonus[block*16 + k] = value[k]
        value[k] = -1
```
*Confidence: HIGH for mechanics; the semantic meaning of each of the 64 slots is not documented in code (MEDIUM confidence that they are per-tier bonuses).*

---

### Business Rule: BR-06 - Treasure conversion configuration

**Overview:**
`g_pTreasure[8]` (`Code/TMSrv/Server.h:232`) is a fixed table of eight "source item -> up to five target items with rates" conversions, fully defined by `ReadConfig:1027`-`:1063` and serialised back by `DrawConfig:1269`-`:1298`. It is the only configuration-driven item-to-item exchange rule in the component.

**Detailed description:**
Each of the eight records occupies two lines. The first line supplies 26 integers parsed by a single `sscanf` with padding-tolerant whitespace (`:1038`-`:1041`): one source item index followed by five quintuples of `target index, effect0 type, effect0 value, effect1 type, effect1 value`. The second line supplies the five success rates (`:1059`-`:1062`). Before parsing, the record is `memset` to zero and the value array is reset to `-1`, so a short line yields zeroed targets rather than stale data from the previous record. Critically, this is the **only** section that performs no validation at all: unlike the drop-bonus matrix (BR-04) there is no `[0..9999]` range check, and unlike the labelled sections there is no `strcmp` of a section header per record - only the single `Treasure Settings:` header at `:1018` is verified. Item indices, effect types and rates are therefore accepted verbatim from disk.

The table's semantics are the reason the record has five targets and five rates: a source item can yield one of five outcomes, each with its own probability, its own item index and two pre-baked effects. The values are consumed by the treasure-map/rune flow rather than by `Combine` (BR-77), which builds its prize arithmetically. `g_pTreasure` is included in the boot reset (`:3481`), so a failed configuration load leaves all eight records zeroed and every conversion consequently inert.

**Rule workflow:**
```
line "Treasure Settings:" (unchecked beyond strcmp) else return
for k in 0..7:
    memset(&g_pTreasure[k])
    value[0..25] = -1
    read line -> value[0] = Source ; for i in 0..4: Target[i] = {value[1+5i] .. value[5+5i]}
    read line -> Rate[0..4]
    (no range validation)
    DrawConfig writes back one line per record + one line of rates
```
*Confidence: HIGH for the parsing contract; MEDIUM for the runtime consumption sites, which live outside this component.*

---

### Business Rule: BR-05 / BR-79 - Free-experience threshold and party-bonus clamp

**Overview:**
`FREEEXP` (`:43`, default 35) is the level below which an account is considered "free/newbie" and above which billing rules apply. `IsFree` (`:1363`) and `PARTYBONUS` clamping (`:924`) are the two rules that consume it.

**Detailed description:**
`IsFree` returns 1 immediately when `FREEEXP <= 0`, which is the operator's "everlasting free trial" switch (`:1365`-`:1366`). Otherwise it scans the four character slots of the selected-character screen (`MOB_PER_ACCOUNT = 4`, `Code/Basedef.h:71`), and returns 1 if any non-empty slot has `Level >= FREEEXP` **and** `Level < 999`. The upper bound of 999 is a sentinel guard: it excludes the "empty slot" level marker that the DB sends for unused characters, so an unoccupied slot cannot accidentally satisfy the paid-account test. If no slot qualifies, the account is not free and downstream billing checks apply.

`PARTYBONUS` is read from `gameconfig.txt` and clamped to the inclusive range `[50..200]`, defaulting to 100 when outside it (`:924`-`:925`). The clamp is silent: no message box, no log line. This is the only range-validated scalar in the billing section; `BILLING`, `CHARSELBILL`, `POTIONCOUNT` and `GUILDBOARD` accept any parsed integer, including negative values, and are used directly as arithmetic operands elsewhere.

The rule interacts with `ProcessSecTimer`'s per-tick billing gate (`ProcessSecMinTimer.cpp:178`), which uses `FREEEXP` together with `BILLING == 2`, the per-user `Unk_2728` child flag and `g_Hour` (<=12 or >=19) to force a logout of paid characters during "child hours". `DoRecall` (`:7583`) also uses `FREEEXP` to route below-threshold MORTAL characters to the newbie start location (2112, 2042).

**Rule workflow:**
```
IsFree(SELCHAR):
   FREEEXP <= 0 -> return 1
   for i in 0..MOB_PER_ACCOUNT-1:
       Name[i] != "" and Level[i] >= FREEEXP and Level[i] < 999 -> return 1
   return 0

ReadConfig PARTYBONUS:
   value < 50 or value > 200 -> PARTYBONUS = 100 (silent)
```
*Confidence: HIGH.*

---

### Business Rule: BR-07/08/09 - Server identity resolution and DB connectivity

**Overview:**
TMSrv instances are identified by their local IP. `WinMain:3509`-`:3570` implements a hard identity contract: the address in `localip.txt` must exist in the static server list, and the accompanying DB address must be reachable, or the process stops.

**Detailed description:**
`localip.txt` is read with a single `fscanf("%s")` into a 255-byte buffer (`:3511`-`:3518`). If the file is missing, the server only shows a message box and continues with an uninitialised `name` buffer (`:3519`-`:3520`) - a defect addressed in Section 10. The dotted quad is then split into `LocalIP[0..3]` with a single `sscanf("%d.%d.%d.%d")` and, critically, the code loops `for i in 0..MAX_SERVERGROUP-1` over `j in 1..MAX_SERVERNUMBER-1` comparing `g_pServerList[i][j]` against the *raw string* from the file (`:3532`-`:3549`). On a match it takes `g_pServerList[i][0]` as the DB address and records `ServerGroup = i`, `ServerIndex = j - 1`. Two structural consequences follow: the DB address is always the group's first entry, so all TMSrv instances of a group share one DBSrv; and `ServerIndex` is zero-based while `ServerGroup` is not offset, which is why the billing identifier is computed as `ServerIndex + 10 * ServerGroup` (`:1386`).

The inner loop uses `break` on match but the outer loop's termination test is `if (DBServerAddress[0] == 0) continue;` (`:3547`), which never terminates the outer loop when a match was found. The search therefore keeps scanning remaining groups; if a later group contains the same address string, it overwrites the earlier match. When no match is found at all, a modal error cites both files and `WinMain` returns `TRUE` (not `FALSE`) from `WinMain` (`:3551`-`:3556`), meaning the process exits with a success status after having done partial initialisation. The DB connection itself is hardcoded to port 7514 in two places (`:3558` assignment and `:3562` argument), and failure is fatal (`:3564`-`:3570`).

After a successful connect, the runtime resilience rule applies in `MainWndProc`: on any non-`FD_READ` event or a failed `Receive`, the socket is closed, `ConnectServer` is retried exactly twice with `Sleep(200)` between attempts, and if both fail the process logs `"reconnect DB fail."` and calls `PostQuitMessage` (`:3815`-`:3847`, mirrored at `:3850`-`:3881`). Note that the success branch also calls `PostQuitMessage` after `Sleep(200)` (`:3845`-`:3847`), so the "successful reconnect" path terminates the process as well; the reconnect loop therefore has no reachable successful continuation.

**Rule workflow:**
```
read localip.txt -> name (missing -> MessageBox, name uninitialised, continue)
parse into LocalIP[4]
DBServerAddress = ""
for each group i, each server j>=1:
    if g_pServerList[i][j] == name:
        DBServerAddress = g_pServerList[i][0]
        ServerGroup = i ; ServerIndex = j-1
if DBServerAddress == "":
    MessageBox(...) ; return TRUE (terminate with success code)
DBServerPort = 7514
ConnectServer(DBServerAddress, 7514, LocalIP, WSA_READDB)
if NULL: log + MessageBox -> return FALSE
on runtime failure: close, 2 x (connect + Sleep 200) -> PostQuitMessage
```
*Confidence: HIGH.*

---

### Business Rule: BR-10 - Optional billing connectivity

**Overview:**
Billing integration must never prevent the world from running. `BILLING` (`:320`, default 3) is both a feature switch and a connect-state flag; `biserver.txt` supplies the endpoint.

**Detailed description:**
`WinMain` reads `biserver.txt` with `fscanf("%s %d")` only if it exists (`:3590`-`:3597`); a missing file leaves `BillServerAddress` empty and `BillServerPort` zero, which is not treated as an error, only as a connect attempt that will fail. The connect attempt is gated on `BILLING > 0` (`:3599`). On failure the code logs `"erro, Nao foi possivel se conectar na BIServer"` and sets `BILLING = 0` - a state change that disables all further billing logic rather than aborting. On success it sends `_AUTH_GAME` with `SendBilling2(&sm, 4)` to announce the game server.

At runtime the same fail-soft rule is repeated: a non-`FD_READ` billing event or a failed `Receive` closes the socket and sets `BillCounter = 360` (`:3738`, `:3752`, `:3770`, `:3783`). `ProcessSecTimer` then counts `BillCounter` down, and on reaching zero attempts one reconnect; a second failure sets `BILLING = 0` permanently (`ProcessSecMinTimer.cpp:130`-`:165`). Because `BillCounter` is decremented per 500 ms tick, the practical retry delay is about 180 seconds, not the nominal 360 seconds implied by the name.

The rule's most important weakness is at the protocol level: `ProcessBILLMessage` (`:4300`) has an empty body, so every message successfully dequeued from `ReadBillMessage` is discarded. Any business rule that would have been enforced by billing responses (authorisation, remaining play time, account state) is effectively absent from this component, while the outbound announce/logout calls remain.

**Rule workflow:**
```
if biserver.txt exists -> read address + port
if BILLING > 0:
    ConnectBillServer(...)
    fail -> log ; BILLING = 0
    ok   -> SendBilling2(_AUTH_GAME, 4)
runtime: FD error / receive error -> CloseSocket ; BillCounter = 360
ProcessSecTimer: BILLING != 0 and BillCounter > 0 and Sock == 0:
    BillCounter--
    BillCounter <= 0 -> ConnectBillServer
        fail -> BILLING = 0
        ok   -> SendBilling2(_AUTH_GAME, 4)
```
*Confidence: HIGH.*

---

### Business Rule: BR-11 / BR-12 / BR-14 - Environment special-casing, group counting and boot spawn

**Overview:**
Three build-time/deployment rules sit immediately after the DB connection: hardcoded IP classifiers (`:3584`-`:3588`), the servers-in-group counter (`:3572`-`:3582`), and the conditional Kefra spawn at boot (`:3638`-`:3644`).

**Detailed description:**
The IP classifiers are literal string comparisons against the resolved `DBServerAddress`: `192.168.0.153` sets `TESTSERVER = 1` and `192.168.0.50` sets `LOCALSERVER = 1`. Both flags then act as global overrides throughout the server: they force the Battle Royale event to run regardless of the daily rotation (`:6286`), force `NewbieEventServer` on (`ProcessSecMinTimer.cpp:2040`), and `LOCALSERVER` also appears as the default value of `KefraKiller` (`:395`). Because the test is performed on the *DB* address rather than on `LocalIP`, every game server pointed at those two database hosts inherits the override; and because the strings are compile-time constants, a deployment on any other subnet cannot enable either mode without a rebuild.

`NumServerInGroup` is computed by counting non-empty `g_pServerList[ServerGroup][1..MAX_SERVERNUMBER-1]` entries, then clamped from below to 1 and from above to 10 (`:3578`-`:3582`). The value is a divisor in two rotation rules (newbie channel, and indirectly the BR gate at `:6284`), so the clamps are what prevent a division-by-zero when the server list is empty and guarantee that the rotation never exceeds the ten server indices the protocol supports.

The Kefra rule spawns the chain only when `KefraLive == 0`, iterating `KEFRA_MOB_INITIAL` (397) to `KEFRA_MOB_END` (400) exclusive and then spawning `KEFRA_BOSS` (396) once, all at coordinates (0,0) so the generator record decides the real position (`:3638`-`:3644`). `KefraLive` is itself a configuration value (`ReadConfig:1106`), so this is a configuration-driven spawn: setting it non-zero in `gameconfig.txt` suppresses the chain entirely, which is how operators disable the world boss without touching code. The identical value is never re-checked afterwards, so a server started with `KefraLive != 0` will not spawn the chain at all during that process lifetime.

**Rule workflow:**
```
NumServerInGroup = count of non-empty g_pServerList[ServerGroup][1..]
if <= 0 -> 1 ; if > 10 -> 10

if DBServerAddress == "192.168.0.153" -> TESTSERVER = 1
if DBServerAddress == "192.168.0.50"  -> LOCALSERVER = 1

after listener start and GuildZoneReport:
   if KefraLive == 0:
        for k in [KEFRA_MOB_INITIAL(397) .. KEFRA_MOB_END(400)): GenerateMob(k, 0, 0)
        GenerateMob(KEFRA_BOSS(396), 0, 0)
```
*Confidence: HIGH.*

---

### Business Rule: BR-13 - Fixed-resolution dual-clock scheduling

**Overview:**
All temporal behaviour of this component derives from two `SetTimer` calls (`:3631`-`:3632`). Their resolutions define the unit of every counter in the code base, including counters that are *named* in seconds and minutes.

**Detailed description:**
`TIMER_SEC` is registered with a 500 ms elapse and `TIMER_MIN` with 12,000 ms (`Code/Basedef.h:136`-`:137` define the timer ids 0 and 1). `MainWndProc:3723`-`:3729` dispatches them to `ProcessSecTimer` and `ProcessMinTimer`. `ProcessSecTimer` increments `SecCounter` (`ProcessSecMinTimer.cpp:123`), therefore **`SecCounter` advances twice per wall-clock second**. `ProcessMinTimer` increments `MinCounter` once per call (`:2282`), therefore **`MinCounter` advances five times per wall-clock minute**. Both names are misleading, and the consequences are concrete:

- `CheckIdle` (`:4304`) closes a session when `lst < SecCounter - 720`, i.e. after 360 real seconds, not 720; and it rewrites `LastReceiveTime` when `lst < SecCounter - 1440`, so an idle period between 720 and 1440 ticks is *silently forgiven* instead of causing a disconnect.
- Round-robin persistence runs on `SecCounter % 8` (`ProcessSecMinTimer.cpp:169`), i.e. every 4 real seconds.
- `RegenMob`'s PK-point and mount-feeding branch runs on `Unk_2736 % 450` (`:4417`), i.e. every 225 real seconds.
- `GuildProcess` uses `timeinfo->tm_min` comparisons but is entered five times per minute (`:6203`), so every minute-state branch is re-evaluated up to five times; correctness depends entirely on the state variables being idempotent.
- `mNPCGen.pList[i].MinuteGenerate` means "12-second ticks", so a configured value of 5 respawns roughly every minute while a value of 300 means an hour.

The only place where the resolution is deliberately changed is shutdown: when `ServerDown` reaches 120, `SetTimer(hWndMain, TIMER_SEC, 200, 0)` shortens the tick to 200 ms (`ProcessSecMinTimer.cpp:115`) so the one-user-per-tick drain completes faster.

**Rule workflow:**
```
WinMain: SetTimer(TIMER_SEC, 500) ; SetTimer(TIMER_MIN, 12000)
MainWndProc WM_TIMER:
    wParam == TIMER_SEC -> ProcessSecTimer()  -> SecCounter++ (x2 per second)
    wParam == TIMER_MIN -> ProcessMinTimer()  -> MinCounter++ (x5 per minute)
shutdown: ServerDown == 120 -> SetTimer(TIMER_SEC, 200)
```
*Confidence: HIGH for the mechanism; the original author's intent is inferred (MEDIUM) but the arithmetic consequence is unambiguous.*

---

### Business Rule: BR-15/16 - Connection admission control

**Overview:**
`WSA_ACCEPT` (`:4006`) is the only place where sessions are admitted. It enforces slot availability, an admin reservation tail, and a global shutdown gate.

**Detailed description:**
Admission begins by acquiring a slot with `GetEmptyUser` (`:6743`), which scans `pUser[1..MAX_USER-1]` and returns the first index with `Mode == USER_EMPTY`; index 0 is permanently excluded in every scan loop of the component, making it a reserved "null user" that doubles as the "not found" return value of `GetUserFromSocket`, `GetEmptyUser`, `GetEmptyItem` and `GetEmptyNPCMob`. Exhaustion is logged as `"err,accept fail - no empty"` and the accept is dropped without closing the listening socket (`:4013`-`:4018`).

The admin reservation is checked *after* `AcceptUser` has already completed the handshake (`:4020`-`:4029`): if the allocated slot is in the last `ADMIN_RESERV` (10) slots, the client receives `_NN_Reconnect`, the outbound buffer is flushed, and `CloseUser` is called. This ordering means the reserved band is only a soft boundary - the slots are still allocatable by any client, and the rejection happens post-accept. The second gate is the shutdown flag: while `ServerDown != -1000` the server is in a terminating state and every new client is answered with `_NN_ServerReboot_Cant_Connect` and closed (`:4032`-`:4039`). `ServerDown` is never reset to `-1000` anywhere in the component, so the gate is one-way: once a shutdown begins the server can no longer admit players.

A further implicit rule is the socket-to-slot binding: `pUser[User].cSock` is the per-session `CPSock`, and `GetUserFromSocket` linearly scans for a matching `Sock` handle on every inbound window message (`:6729`). If the lookup fails the raw socket is `closesocket`'d immediately (`:3924`), which is the only path where the component bypasses `CloseUser`'s bookkeeping.

**Rule workflow:**
```
WSA_ACCEPT (WSAGETSELECTERROR == 0):
   User = GetEmptyUser()          ; 0 -> log + drop
   AcceptUser(ListenSocket.Sock)
   if User >= MAX_USER - 10       -> send _NN_Reconnect, flush, CloseUser, return
   if ServerDown != -1000         -> send _NN_ServerReboot_Cant_Connect, CloseUser, return
WSA_READ:
   User = GetUserFromSocket(wParam) ; 0 -> closesocket(wParam)
   event != FD_READ -> CloseUser
```
*Confidence: HIGH.*

---

### Business Rule: BR-17 - Crack-point accumulation and forced logout

**Overview:**
`AddCrackError` (`:684`) is the component's only built-in anti-cheat counter. It is called from 22 translation units (client handlers, DB handler, movement code) to record protocol anomalies.

**Detailed description:**
Each call adds `val` to `pUser[conn].NumError` and, when the running total reaches 2,000,000,000, sends the `_NN_Bad_Network_Packets` message, forces `CharLogOut`, writes a `"cra char logout type: %d"` log line, and returns `TRUE` to tell the caller that the session has been terminated. Types 3, 8 and 15 are deliberately *not* logged (`:686`), which identifies them as high-frequency, expected anomalies (the comment structure implies movement or timing noise) and is the only triage rule in the mechanism.

Two properties limit the rule's effectiveness. The first is arithmetic: `NumError` is a signed `int`, and the guard is a positive comparison; a single large `val` (or the accumulated sum crossing `INT_MAX` at 2,147,483,647) produces signed overflow before or at the comparison, which is undefined behaviour in C++. Calls that pass negative `val` - the function performs no sign check - reduce the counter, so a client able to trigger a negative-valued call site can permanently suppress the threshold. The second is the absence of decay: the counter only resets when the `CUser` object is reconstructed, so legitimate players accumulate points for the entire session with no notion of "recent" behaviour.

The function also dereferences `pUser[conn]` without validating `conn` (`:692`) while its sibling `CrackLog` (`:6671`) does validate. Callers in the message handlers pass the connection index derived from the packet's `ID` field after `ProcessClientMessage`'s range check (`ProcessClientMessage.cpp:42`), so the invariant is upheld by a different unit rather than locally - a coupling that is easy to break.

**Rule workflow:**
```
AddCrackError(conn, val, Type):
   if Type not in {3,8,15}: log "cra point: val type: Type"
   pUser[conn].NumError += val
   if NumError >= 2000000000:
        SendClientMessage(_NN_Bad_Network_Packets)
        CharLogOut(conn)
        log "cra char logout type: Type"
        return TRUE
   return FALSE
```
*Confidence: HIGH.*

---

### Business Rule: BR-18/19/20 - Session timeout, failure table, and periodic persistence

**Overview:**
Three related session-hygiene rules live in different files but share the same tick base: idle disconnection (`CheckIdle` `:4304`), the failed-account ring (`AddFailAccount`/`CheckFailAccount` `:1334`/`:1347`), and round-robin saving (`ProcessSecMinTimer.cpp:169`).

**Detailed description:**
`CheckIdle` compares the user's `LastReceiveTime` (refreshed by `ProcessClientMessage` on every accepted packet, `ProcessClientMessage.cpp:56`-`:57`) against `SecCounter`. It first repairs a *forward* jump (`lst > ser` -> reset to now), then repairs a *large* gap (`lst < ser - 1440` -> reset to now), and only afterwards tests `lst < ser - 720`. Because the 1440 branch rewrites `lst` before the 720 branch executes, sessions idle for more than 1440 ticks are forgiven, and sessions idle between 720 and 1440 ticks are disconnected. With the 500 ms tick of BR-13 this yields a real inactivity window of roughly 6 minutes, forgiving anything longer than 12 minutes - the inverse of ordinary timeout semantics.

The failed-account table is a 16-entry fixed array of 16-byte strings (`FailAccount` `:369`). `AddFailAccount` writes into the first slot whose first byte is zero and silently drops the record when the table is full; `CheckFailAccount` counts byte-exact duplicates. `ProcessMinTimer` clears the whole table every tenth `MinCounter` tick (`ProcessSecMinTimer.cpp:2136`-`:2137`), i.e. every two wall-clock minutes, which makes the table a short-window throttle rather than a blacklist. Note that although `FailAccount` is declared in `Server.h:315` and referenced by two TUs, this component never reads a count to make a decision - the decision sites are in the login handler.

Round-robin persistence saves exactly one user per `SecCounter % 8` tick, advancing `SaveCount` and resetting it to 1 at `MAX_USER` (`:173`-`:198`). Two rules are layered on it: users in `USER_PLAY` whose mob is not `MOB_EMPTY` are saved; and if `BILLING == 2`, the user carries the child flag (`Unk_2728 == 1`), the character level is at or above `FREEEXP`, and the hour is outside 12:00-19:00, the user is instead messaged `_NN_Child_Pay` and logged out. The loop `break`s after handling one user, so the effective guarantee is "at most one save per tick, one forced logout per tick".

**Rule workflow:**
```
CheckIdle(conn):
   ser = SecCounter ; lst = LastReceiveTime
   if lst > ser        -> LastReceiveTime = ser
   if lst < ser - 1440 -> LastReceiveTime = ser
   if lst < ser - 720  -> log + CloseUser(conn)

every 8th SecCounter tick:
   advance SaveCount (wrap to 1 at MAX_USER)
   user in USER_PLAY and mob alive:
        child-billing gate -> message + CharLogOut
        else               -> SaveUser(idx, 0)
        break
```
*Confidence: HIGH.*

---

### Business Rule: BR-22/21 - Session close and character logout semantics

**Overview:**
`CloseUser` (`:6834`) and `CharLogOut` (`:7085`) are the two teardown paths. Their distinction encodes an important domain rule: closing a socket mid-login must not overwrite the DB with partial state, while closing an in-game session must persist it.

**Detailed description:**
`CloseUser` first clears the mob's grid cell when the user was playing and the coordinates are within `[0..4096)` (`:6839`-`:6841`), resets the admin flag, and emits a billing disconnect through `SendBilling` when the session was billing-connected (`:6845`-`:6846`; note `SendBilling` is a stub returning `TRUE`, `:1377`). It then closes the socket and branches on `pUser[conn].Mode`. If the mode is neither `USER_PLAY` nor `USER_SAVING4QUIT` but is non-zero, it sends `_MSG_DBNoNeedSave` with the connection id, marks the mob slot empty and calls `CUser::CloseUser()` (`:6854`-`:6867`) - the rule being "a half-authenticated session has nothing worth saving". Otherwise it performs full cleanup: `RemoveParty`, a symmetric `RemoveTrade` when both sides still reference each other, construction of `MSG_SavingQuit` with mob, cargo, short skills, affects, extra, account name, coin, donate and slot, and finally `DeleteMob(conn, 2)` with the mode set to `USER_SAVING4QUIT` (`:6869`-`:6919`). Two invariants are enforced before the build: the connection index must be inside `[0..MAX_MOB]` (`:6885`) and the character slot inside `[0..MAX_CLASS)` (`:6893`); violations are logged through `CrackLog` and the function returns *without* closing the socket or the user record, leaving a half-torn-down session.

`CharLogOut` is the in-game "back to character select" path. It rejects non-`USER_PLAY` sessions with an error log plus a `_MSG_CNFCharacterLogout` signal (`:7092`-`:7099`), charges a billing logout when `CHARSELBILL == 0` (the config flag that keeps the account billed across character selection), removes a symmetric trade, switches the mode to `USER_SELCHAR`, copies the character level into the selection-screen score, calls `SaveUser(conn, 1)` (export flavour), `DeleteMob(conn, 2)`, restores the last safe position from `MOB.SPX/SPY`, marks the mob slot empty, and confirms to the client. The `Export = 1` flag is the only difference from the periodic save and is what makes the DB server write the character back to permanent storage.

**Rule workflow:**
```
CloseUser(conn):
   if playing and 0<=x,y<4096 -> clear pMobGrid
   Admin = 0 ; if IsBillConnect -> SendBilling(...,2,0)
   cSock.CloseSocket()
   if Mode in {0} or == USER_ACCEPT -> CUser::CloseUser(); return
   if Mode not in {USER_PLAY, USER_SAVING4QUIT}:
        send _MSG_DBNoNeedSave ; mob.Mode = empty ; CUser::CloseUser()
   else:
        RemoveParty ; symmetric RemoveTrade
        validate conn (< MAX_MOB) and Slot (< MAX_CLASS) else CrackLog + abort
        build MSG_SavingQuit ; send to DB ; Mode = USER_SAVING4QUIT ; DeleteMob(conn,2)

CharLogOut(conn):
   validate conn ; require USER_PLAY
   optional billing logout ; symmetric RemoveTrade
   Mode = USER_SELCHAR ; SelChar.Score[Slot].Level = BaseScore.Level
   SaveUser(conn, 1) ; DeleteMob(conn, 2)
   mob position = MOB.SPX/SPY ; mob.Mode = 0 ; signal _MSG_CNFCharacterLogout
```
*Confidence: HIGH.*

---

### Business Rule: BR-23/24/25 - Item granting, world-drop creation and treasure boxes

**Overview:**
Three functions form the item-materialisation rule set: `PutItem` (into inventory, `:709`), `CreateItem` (onto the ground, `:7254`) and `CreateTreasureBox` (a special ground item, `:9392`).

**Detailed description:**
`PutItem` is the strictest of the three. It rejects any `conn <= 0` or `conn >= MAX_USER`, rejects sessions that are not `USER_PLAY`, then searches `Carry[0..max(MaxCarry, MAX_CARRY))` for the first slot with `sIndex == 0`. The dual bound is significant: `MaxCarry` is a per-character dynamic limit (`CMob::MaxCarry`, `Code/TMSrv/CMob.h:96`) that can be smaller than the physical `MAX_CARRY = 64`; the loop honours both, so a character with a reduced carry capacity cannot use the tail slots even if they are physically free. If a slot is found the item is copied and pushed to the client with `SendItem(..., ITEM_PLACE_CARRY, pos, ...)`; otherwise the player is told `_NN_You_Have_No_Space_To_Trade` and the function returns `FALSE`. Callers use the return value to decide whether to destroy the source item, so this function is the single arbiter of "did the player receive it".

`CreateItem` validates only the item index (`<= 0` or `>= MAX_ITEMLIST` -> `FALSE`), then calls the shared `GetEmptyItemGrid(&x, &y)` helper to relocate the drop to a free neighbouring cell, re-checks the grid (`if (pItemGrid[y][x]) return FALSE`), and acquires an item slot with `GetEmptyItem` (which scans `pItem[1..MAX_ITEM-1]`). On success it stamps `Mode = 1`, position, item payload, `Rotate`, `State = STATE_OPEN`, `Delay = 90` (45 real seconds of decay immunity given the 500 ms tick), `Decay = 0`, `GridCharge` from the item's `EF_GROUND` ability, the terrain height, and multicasts a `_MSG_CreateItem` with `ItemID = slot + 10000`. A commented-out height check at `:7261` shows a disabled rule: drops on steep or low terrain used to be refused and are now allowed.

`CreateTreasureBox` mirrors `CreateItem` but stamps `Mode = 2`, `Delay = 0`, takes the state from its caller, and forces the wire height to `-204` (`:9444`), which is the client's marker for a "box" rendering. Crucially it does not honour the "first free slot" contract in the same way - it also calls `GetEmptyItemGrid` first, so a box can displace to a neighbouring cell.

**Rule workflow:**
```
PutItem(conn, item):
   conn in (0, MAX_USER) and Mode == USER_PLAY else FALSE
   pos = first i < min(MaxCarry, MAX_CARRY) with Carry[i].sIndex == 0
   success -> memcpy + SendItem(ITEM_PLACE_CARRY, pos)
   failure -> SendClientMessage(_NN_You_Have_No_Space_To_Trade) ; FALSE

CreateItem(x,y,item,rotate,Create):
   item->sIndex in [1 .. MAX_ITEMLIST-1] else FALSE
   GetEmptyItemGrid(&x,&y) ; grid occupied -> FALSE
   slot = GetEmptyItem() ; 0 -> FALSE
   Mode 1 ; Delay 90 ; Decay 0 ; State STATE_OPEN ; GridCharge = EF_GROUND ; Height = terrain
   multicast _MSG_CreateItem (ItemID = slot + 10000, Height -204 if Create == 2)
```
*Confidence: HIGH.*

---

### Business Rule: BR-26 - Ground item decay

**Overview:**
`ProcessDecayItem` (`:8353`) implements the only garbage-collection rule for the world: ground items disappear after their `Delay` and `Decay` counters expire.

**Detailed description:**
The rule enumerates a *window* of the item array rather than the whole array, driven by the shared global `ItemCount` (`:328`, initialised to 100). The loop header is `for (i = 0; i < ItemCount; i++)` while the body immediately performs `ItemCount++` and wraps it back to `g_dwInitItem + 1` when it passes 5000 (`:8355`-`:8360`). The consequence is that the iteration count grows while it executes: each pass both advances the cursor and increases the bound, so the loop runs roughly 5,000 iterations on the first call and progressively more (bounded by the `>= 5000` reset), and the effective "slot being examined" is always `ItemCount`, not `i`. Two additional invariants are enforced inside: item index 1727 is exempt from decay (`:8362`), and slots with `Mode == 0` (empty) or `Mode == 2` (treasure box) are skipped (`:8365`).

For eligible items the rule is a two-stage timer. While `Delay >= 1` the counter is decremented and the item survives (`:8368`-`:8372`). Once `Delay == 0`, the item is removed only if `Decay != -1` (`:8374`): the payload is cleared with `BASE_ClearItem`, the owning grid cell is zeroed, `Mode` becomes 0, and a `_MSG_DecayItem` is multicast with `ItemID = slot + 10000`. A `Decay` of `-1` therefore means "permanent", which is exactly how `Reboot` keeps the `g_pInitItem` scenery items alive. Since `CreateItem` starts items at `Delay = 90` and `Decay = 0`, every normal drop is destroyed 45 real seconds after being created unless some other rule raises its `Decay`.

**Rule workflow:**
```
for i in 0 .. ItemCount (ItemCount grows each pass, wraps to g_dwInitItem+1 at >=5000):
    ItemCount++ ; if ItemCount >= 5000 -> ItemCount = g_dwInitItem + 1
    slot = ItemCount
    if pItem[slot].ITEM.sIndex == 1727 -> continue
    if Mode in {0,2} -> continue
    if Delay >= 1 -> Delay-- ; continue
    if Decay != -1:
        BASE_ClearItem ; pItemGrid[x][y] = 0 ; Mode = 0
        multicast _MSG_DecayItem(slot + 10000)
```
*Confidence: HIGH for the mechanics; the intended batching semantics are unclear (LOW).*

---

### Business Rule: BR-27 - Random item bonus generation (`SetItemBonus`)

**Overview:**
`SetItemBonus(Dest, Level, a3, DropBonus)` (`:1393`-`:2333`) is the server's item-enchantment rule. It is a single 941-line function that rewrites the three effect slots of any item based on the item definition (`g_pItemList`), a level, an "event/forced" flag (`a3`) and a drop-bonus scalar. It is called by 5 translation units, including drop creation, quest rewards, dungeon events and combination results.

**Detailed description:**
The function is organised in four stages. **Stage 1 - inputs and grade extraction** (`:1395`-`:1445`): `addDropBonus = DropBonus / 8` clamped to `[0..2]`; if effect slot 0 currently holds a "grade" effect (`EF_GRADE0..EF_GRADE5`), the grade index becomes `add1` and the accumulated value becomes `add2`, and slot 0 is cleared. Then the *level difference* is computed as `lvdif = (Level - ReqLvl + 1) / 25`, overridden by `add1` when present, exported through the global `g_dLevel`, and clamped to `[0..3]`; levels of 210 and above are first shifted down by 47 unless `a3` is set (`:1418`-`:1419`); `a3` additionally demotes a tier-3 item to tier 2 (`:1444`-`:1445`). So the rule's real parameter is a *tier* 0..3, not a raw level.

**Stage 2 - slot-0 (main) bonus** (`:1447`-`:2272`): gated by `nPos & 0xFE && !LOBYTE(stEffect[0].sValue) && nPos != 128`, i.e. equipment positions only, only when there is no pre-existing per-item value. A random roll `v35 = rand()%101` is reduced modulo a **tier-dependent divisor**: `a3` -> `%3`; tier 1 and 2 -> `% (6 - addDropBonus)`; tier >=3 -> `%4`; tier 0 -> `% (8 - addDropBonus)` (`:1456`-`:1485`). That divisor is the primary rarity control: higher bonus tiers see fewer distinct outcome buckets. The bucket is then mapped through a `switch (nPos)` table that encodes per-equipment-slot outcome sets, with helmet (`2`), body/legs (`4`, `8`), shield (`16`), weapon (`32`) and two-handed/other (`64`, `192`) branches, plus a special case for `nUnique` 44 and 47 (`:1524`). A second independent roll `v33` (halved under `a3`, `:1570`-`:1572`) drives a second `switch (nPos)` producing a paired sub-effect (`v31`/`v30`/`v29`), most often AC/HP-style effect pairs.

**Stage 3 - grade/quantity** (`:2150`-`:2271`): when no per-slot table match occurs, slot 0 is set to effect `43` with a value derived either from `g_pBonusValue[v20][lvdif][1]` in a bounded random range (`v17 .. v16`), or from a grade distribution table keyed on `add2` (the previous grade value 3..7) with progressively shifted probability bands (`:2176`-`:2268`). **Stage 4 - post-processing** (`:2274`-`:2332`): the item's own definition is re-read over twelve effect slots, and if any definition effect is `0x2B` (43), `0x3D` (61) or `0x4E` (78) the corresponding value is *forced* onto slot 0 (with `0x4E` randomised into `[value..value+3]` capped at 9); finally, four hardcoded item groups - `412/413/419/420/753` and `447..450`, `692..695` - have any empty effect slot filled with effect `59` and a random value.

The rule's impact is architectural: item power is not derived from item definition alone but from a stochastic function of level tier, drop bonus, equipment slot, "uniqueness" and an event flag, with effect codes (43, 59, 61, 78, `EF_GRADE*`) acting as an inline mini-language. Because the function is global-state dependent (`g_dLevel`, `g_pItemList`, `g_pBonusValue`, `g_pDropBonus`, `rand()`) and has no reporting or clamping for the outer range, reproducing a given item requires replaying the RNG.

**Rule workflow:**
```
SetItemBonus(Dest, Level, a3, DropBonus):
   addDropBonus = clamp(DropBonus / 8, 0, 2)
   extract grade from stEffect[0] if EF_GRADE0..5 -> add1 (grade), add2 (value), clear slot 0
   if !a3 and Level >= 210: Level -= 47
   lvdif = (Level - g_pItemList[idx].ReqLvl + 1) / 25
   if add1 != -1: lvdif = add1
   g_dLevel = lvdif ; pForc = (lvdif >= 4)
   clamp lvdif to [0..3] ; if a3 and lvdif >= 3: lvdif = 2
   if equipment slot and slot0 value empty and nPos != 128:
        roll1 -> bucket via tier divisor -> per-nPos effect table
        roll2 -> bucket via tier divisor -> per-nPos paired effect table
        write stEffect[0] (and possibly paired slot)
   otherwise: grade distribution on add2 (3..7) with shifted bands
   for each of 12 definition effects:
        0x2B -> effect 43, value from definition
        0x3D -> effect 61, value from definition
        0x4E -> effect 78, value in [definition .. definition+3] capped 9
   if sIndex in {412,413,419,420,753}: fill empty slots with effect 59 + rand()
   if sIndex in 447..450 or 692..695: same, last value rand()%256
```
*Confidence: HIGH for control flow; MEDIUM for the intended gameplay meaning of each per-slot table entry.*

---

### Business Rule: BR-28 - Secondary per-slot bonus (`SetItemBonus2`)

**Overview:**
`SetItemBonus2` (`:2335`-`:2478`) is the companion generator used where an item must be given a *complete*, slot-appropriate bonus set rather than a single random effect. It switches on `g_pItemList[Dest->sIndex].nPos` and writes all three effect slots.

**Detailed description:**
For helmets (`nPos == 2`) the rule first normalises sanctuary: a fixed roll `rand()%25` selects a row of the static `g_pBonusValue3` table; if slot 0 already holds `EF_SANC` the existing sanctuary value is read and, when below 6, increased by `rand()%2` and clamped at 6 (`:2343`-`:2356`); if slot 0 holds anything else it is replaced by `EF_SANC` with `rand()%2`. Slots 1 and 2 are then filled from `g_pBonusValue3[_rand][0..3]`, i.e. effect/value pairs from a table indexed by the same roll, so the helmet always ends with one sanctuary effect and two table-driven bonuses. Body and leg armour (`nPos == 4` or `8`, `:2368` onward) follow the same shape with position-specific handling, and the remaining positions are handled analogously.

The rule's distinguishing property versus BR-27 is deterministic structure with stochastic magnitude: the *set* of effects is fixed by the slot, while only the row of `g_pBonusValue3` and the sanctuary increment are random. That makes `SetItemBonus2` the correct comparison baseline for the far more random `SetItemBonus`, and it explains why the sanctuary value is capped at 6 rather than left unbounded - the cap matches the client's maximum displayed sanctuary tier.

**Rule workflow:**
```
nPos = g_pItemList[Dest->sIndex].nPos
if nPos == 2 (helmet):
    roll = rand()%25
    if stEffect[0] is EF_SANC: sanc = GetItemSanc ; if sanc < 6: sanc += rand()%2 ; clamp 6 ; SetItemSanc
    else: stEffect[0] = (EF_SANC, rand()%2)
    stEffect[1] = (g_pBonusValue3[roll][0], g_pBonusValue3[roll][1])
    stEffect[2] = (g_pBonusValue3[roll][2], g_pBonusValue3[roll][3])
if nPos in {4, 8} (body / legs): position-specific rewriting follows the same pattern
```
*Confidence: HIGH for the helmet branch; HIGH that the remaining positions are structurally analogous (the tail of the function was inspected at a coarser granularity).*

---

### Business Rule: BR-29/30 - Level-up item reward contract

**Overview:**
`LevelItem[4][4][400]` (`Code/TMSrv/Server.h:233`) is a compiled reward table populated from `LevelItem.txt` by `ReadLevelItemConfig` (`:8730`) and consumed by `DoItemLevel` (`:8805`), which is invoked by the quest/level-up path.

**Detailed description:**
The loader reads the file line by line with `sscanf("%d %d %d %d %d %d %d %d %d %d")` and no validation whatsoever: the first three integers are interpreted as class, build-type and level, the remaining seven as item index and three effect pairs. It then applies wildcard expansion - `cls == 4` with `type != 4` writes the same item for all four classes at the given build; `cls == 4 && type == 4` writes the item for all 16 class/build combinations; `type == 4` alone writes it for all four builds of the given class; otherwise a single cell is written (`:8772`-`:8799`). Because neither `cls`, `type` nor `level` is range-checked before indexing `LevelItem[cls][type][level]`, a malformed or hostile file writes outside the array (the array's bounds are 4/4/400 while `level` can be any integer from the file).

`DoItemLevel` derives the build type from the character's *base* stats with a strict-greater-than cascade: STR dominates -> type 0, else INT dominates -> 1, else DEX dominates -> 2, else (CON or a tie) -> 3 (`:8809`-`:8819`). Ties therefore always fall to type 3, which is a meaningful and undocumented rule. It then looks up `LevelItem[Class][type][BaseScore.Level]`; if the entry exists, it finds the first free cargo slot among `0..MAX_CARGO-2` (the last cargo slot is excluded) and writes the item into `pUser[conn].Cargo[i]`, sends it, prints `_NN_Item_Arrived` with the item name, writes a `tra,<account> <slot><itemcode>` log line tagged `_lvlreward_`, and - only when the session is still on the character-selection screen - performs `SaveUser(conn, 0)`. If no free slot exists the reward is silently skipped (no retry queue), and if the table cell is empty the function returns silently, so the caller cannot distinguish "already granted" from "not configured".

**Rule workflow:**
```
ReadLevelItemConfig:
   open LevelItem.txt (missing -> MessageBox, table stays zeroed)
   memset LevelItem
   per line: cls, type, level, itemIndex, eff0, val0, eff1, val1, eff2, val2
       cls==4 && type!=4 -> all classes at [type]
       cls==4 && type==4 -> all 16 combinations
       type==4           -> all builds of that class
       else              -> LevelItem[cls][type][level]
DoItemLevel(conn):
   type = argmax(Str, Int, Dex, Con) with ties -> 3
   idx = LevelItem[Class][type][BaseScore.Level].sIndex
   if idx != 0:
       for i in 0 .. MAX_CARGO-3:
            first empty Cargo[i] -> grant, send, message, log, optional SaveUser
```
*Confidence: HIGH.*

---

### Business Rule: BR-31 - Circlet sub-god progression

**Overview:**
`SetCircletSubGod` (`:8855`) grants one-time flat bonus upgrades to the "sub-god" circlet (equipment slot 1) at four Celestial milestones.

**Detailed description:**
The rule is a sequence of four independent, idempotent gates. Each requires `extra.ClassMaster == SCELESTIAL`, an exact base level (120, 150, 180, 199), and a dedicated persisted flag (`QuestInfo.Celestial.Add120`, `Add150`, `Add180`, `Add200`). The flag is set *before* the equipment check, so a character who reaches the threshold while not wearing a circlet permanently loses that milestone's bonus - the flag can never be re-earned. The bonuses are applied to effect slot 1 as `EF_AC` (30/35/45/50) and to slot 2 as `EF_HP` (40/50/60/70) respectively, overwriting whatever was there, and the item is pushed to the client with `SendItem`.

The level-199 gate is the notable exception: it uses the `Add200` flag and 199 (not 200) as the trigger, so the "level 200" milestone is actually granted one level early. Because each gate overwrites two effect slots without inspecting their contents, a player who has customised the circlet with other effects loses them, and the values are not cumulative - they replace, so the progression is 30/35/45/50 AC and 40/50/60/70 HP rather than increments.

**Rule workflow:**
```
if ClassMaster == SCELESTIAL:
   Level == 120 and Add120 == 0 -> Add120 = 1 ; if Equip[1] exists:
        eff1 = (EF_AC, 30) ; eff2 = (EF_HP, 40) ; SendItem
   Level == 150 and Add150 == 0 -> ... (EF_AC 35, EF_HP 50)
   Level == 180 and Add180 == 0 -> ... (EF_AC 45, EF_HP 60)
   Level == 199 and Add200 == 0 -> ... (EF_AC 50, EF_HP 70)
```
*Confidence: HIGH.*

---

### Business Rule: BR-32 - Crown and dragon-insignia reconciliation

**Overview:**
`ClearCrown` (`:494`) is the invariant enforcement function for the castle-crown items (747 crown, 3993/3994 dragon insignia) and is called for every user when a castle changes hands (`:4409`-`:4410`).

**Detailed description:**
The function is skipped for non-playing sessions. It then determines whether the player's guild currently holds a crown entitlement by scanning `ChargedGuildList[i][4]` for `MAX_SERVER` entries, requiring a non-zero guild id, a non-zero charged guild, equality, **and** `GuildLevel == 9` (`:506`-`:516`) - i.e. only guild masters of the currently charged guild keep the crown. For entitled players it performs a three-pass reconciliation over equipment (`MAX_EQUIP`), carry (`MAX_CARRY`) and cargo (`MAX_CARGO`) slots: any slot holding 747 sets `HaveCrow`; any slot holding 3993 or 3994 sets `HaveDrag`; and a *clan-based swap* converts 3993->3994 for clan 7 and 3994->3993 for clan 8, logging each conversion as `etc,crown guild:... charge:... name:...`. After the passes, a missing crown is re-issued through `PutItem` with effect 43/value 9, and a missing insignia is re-issued with the index appropriate to the player's clan (3993 for clan 8, 3994 for clan 7).

For non-entitled players the same three passes run in *removal* mode: every 747/3993/3994 found is logged, zeroed, and re-sent as an empty slot. The rule therefore ensures exactly one crown and at most one insignia per entitled master, and none for anyone else, on every castle transition. Two gaps are visible: a player whose clan is neither 7 nor 8 receives an item with `sIndex == 0` from the re-issue branch (the `if/else if` at `:631`-`:635` leaves the index unset), and the entitlement scan's outer loop terminates at `MAX_SERVER` while `ChargedGuildList` is indexed by `ServerIndex`, so only the first ten servers are considered.

**Rule workflow:**
```
if Mode != USER_PLAY -> return
entitled = exists i<MAX_SERVER: ChargedGuildList[i][4] == Guild != 0 && GuildLevel == 9
scan Equip/Carry/Cargo: detect 747 (crown) and 3993/3994 (dragon)
if entitled:
    swap 3993->3994 for clan 7, 3994->3993 for clan 8 (log each)
    if no crown   -> PutItem(747, effect 43 value 9)
    if no dragon  -> PutItem(clan==8 ? 3993 : clan==7 ? 3994 : 0)
else:
    for each of the three containers: zero and resend 747/3993/3994 (log each)
```
*Confidence: HIGH.*

---

### Business Rule: BR-33/34 - Guild challenge eligibility and fame auction

**Overview:**
`Challange(conn, mob, coin)` (`:2479`) is the guild siege enrolment rule, triggered when a player talks to the challenge NPC.

**Detailed description:**
The rule is a cascade of seven hard gates, each producing a distinct player-facing message or a silent `0` return: the target must be an NPC index in `[MAX_USER..MAX_MOB)`; the challenger's guild must exist (`Guild > 0` implied by later checks); the zone is derived as the NPC's **base** level and must be inside `[0..ValidGuild)` with zone 4 (castle) excluded (`:2487`-`:2491`); the player must be guild master (`GuildLevel == 9`, `:2494`); the guild must not already own that zone (`:2501`); the player must be a citizen of this server (`extra.Citizen == ServerIndex + 1`, `:2507`); the guild must have a name registered for its server-group slot (`g_pGuildName[Group][Server][usGuild][0] != 0`, `:2519`); and guild fame must be at least 100 (`:2530`).

Passing all gates charges a **flat fee of 100 fame** - not the `coin` argument, which is overwritten by the current fame value (`:2547`) - and sends the reduced `GuildInfo` to DBSrv as `_MSG_GuildInfo`, so the fee is persisted immediately. It then applies the auction rule: the challenger only becomes the recorded challenger when its fame exceeds the previously recorded `pChallangerMoney[ZoneChall]` (`:2560`-`:2567`), and on success the challenger list and guild files are flushed through `CReadFiles::WriteChallanger()` and `WriteGuild()`. The fame is deducted even when the challenger loses the auction, so repeated challenges are a fame sink by design.

**Rule workflow:**
```
Challange(conn, mob, coin):
   mob in [MAX_USER .. MAX_MOB) else 0
   zone = pMob[mob].BaseScore.Level ; zone in [0..ValidGuild) and != 4 else 0
   GuildLevel == 9                       else say _NN_Only_Guild_Master_can
   g_pGuildZone[zone].ChargeGuild != Guild else say _NN_Champions_Cant_Challange
   extra.Citizen == ServerIndex + 1      else say _DN_ANOTHER_TOWNSPEOPLE
   guild name exists for (Group, Server, usGuild) else say _NN_Only_Named_Guild
   GuildInfo[Guild].Fame >= 100          else say _DN_Need_1000000_For_Challange
   GuildInfo[Guild].Fame -= 100 ; send _MSG_GuildInfo to DB
   message _NN_Challanged ; log "etc,challange guild:%d fame:%d zone:%d"
   if pChallangerMoney[zone] < fame:
        g_pGuildZone[zone].ChallangeGuild = Guild
        pChallangerMoney[zone] = fame
        WriteChallanger() ; WriteGuild()
   return 1
```
*Confidence: HIGH.*

---

### Business Rule: BR-35/36/37/38 - Guild war lifecycle and winner determination

**Overview:**
`GuildProcess` (`:6511`-`:6607`) implements the weekly guild-war calendar as a five-state machine (`WeekMode` 0..5), and `DecideWinner` (`:6108`) implements the resolution rule invoked at the end of the war.

**Detailed description:**
The calendar derives `day = GuildDay - 1` (with a `+7` wrap, `:6511`-`:6514`) and reads the current weekday; `ForceWeekMode` (a config-driven override) replaces the weekday with `-1` so that no weekday comparison can be true. The states then advance through nested `if` chains that each combine a weekday test, an hour test and a minute threshold: state 0 announces the upcoming battle, state 1 (`:6605`) announces and locks the arena, state 2 (`:6576`-`:6601`) clears the PK zone, sets `WeekMode = 2`, locks the arena, and teleports the incumbent defenders and then the challengers into their configured spawn rectangles with `NumGuildBattle` (default 26) as the per-guild cap; state 3 opens the arena; state 4 (minute > 20, `:6556`-`:6565`) announces the result, calls `DecideWinner`, clears the PK zone and challengers, writes challenger state, locks the arena, sets `WeekMode = 4` and reports zones to DB; state 5 detects that the battle window has passed and increments `Victory` for zones without a challenger (capped below 4) before returning to state 0.

`DecideWinner` iterates zones `[0..ValidGuild)` while explicitly skipping zones 3 and 4 (`:6110`-`:6113`), skips zones without a challenger, resolves both guild names, and then walks the zone's war rectangle cell-by-cell over `pMobGrid`, considering only entries that are player indices in `[1..MAX_USER)`, `USER_PLAY`, alive, in a guild, and belonging to either of the two belligerents. It sums member **levels**, using `Level + MAX_LEVEL` for any character whose `ClassMaster` is not `MORTAL` (and not `ARCH`, per the duplicated condition at `:6152`) - the "sub-god/arch counts as above max level" rule. The guild with the strictly lesser sum loses; ties keep the incumbent and increment `Victory` up to a cap of 4. Both guilds receive the result notice, `ChallangeGuild` is cleared, and finally zone 4 (castle) is assigned to a guild only if it holds zones 0, 1, 2 and 3 simultaneously (`:6199`-`:6200`).

**Rule workflow:**
```
GuildProcess (called per 12 s tick, but expressed in minutes):
   day = (GuildDay - 1 + 7) % 7 ; fweek = (ForceWeekMode != -1) ? -1 : tm_wday
   WeekMode 0 -> notice 1 ; WeekMode = 1
   WeekMode 1 -> notice 2 ; ClearGuildPKZone ; arena locked ; summon defenders then challengers
   WeekMode 2 -> notice 3 ; arena open ; WeekMode = 3
   WeekMode 3 -> notice 4/5 ; WeekMode = 4/5 at minute thresholds
   WeekMode 4 -> notice 5 ; DecideWinner ; ClearGuildPKZone ; ClearChallanger ;
                 WriteChallanger ; arena locked ; GuildZoneReport
   WeekMode 5 -> notice 6 ; WeekMode = 0 ; Victory++ for zones with no challenger (< 4)

DecideWinner:
   for zone in 0..ValidGuild-1, skipping 3 and 4, skipping no-challenger:
       sum levels of living players of both guilds inside WarArea
       if challengerSum < chargeSum -> ChallangeGuild takes the zone, Victory = 0
       else                          -> Victory = min(Victory+1, 4)
       clear ChallangeGuild ; send _SSNN_GuildWarResult to both guilds
   if zones 0,1,2,3 all owned by the same guild -> zone 4 = that guild
```
*Confidence: HIGH.*

---

### Business Rule: BR-39/40/41 - Alliance, war declaration, and guild teleport caps

**Overview:**
`DoAlly` (`:7697`), `DoWar` (`:7604`) and `SummonGuild`/`SummonGuild2` (`:5683`/`:5641`) are guild diplomacy and mobilisation rules.

**Detailed description:**
`DoAlly` validates both guild ids against `[0..65536)`, then applies a single-slot alliance model: if the caller currently has no ally, the target becomes the ally; otherwise the caller's existing ally is dropped regardless of whether the target matches. A notice is broadcast and, for the declaring guild only, each member receives a refreshed war-info packet. There is no reciprocity check and no per-guild limit other than the single occupied slot, so alliances are one-directional in storage but presented as mutual.

`DoWar` is a mutual-consent rule expressed through two tables. For a non-zero target, the function first evaluates whether the caller already has a war (`myGuildWar != 0`) or whether the target has already declared against the caller (`targetGuildWar == myguild`). The nested conditions (`:7630`-`:7651`) mean the "war started" branch is only reached when the caller has no war **and** the target has already declared war back; declaring against a guild that has not declared earlier falls into the log-only branch, while declaring against a guild that has *not* declared produces the `_SS_War_Declared` notice. With `target == 0`, it is a cancellation: if the counterpart declared against the caller as well, both entries are cleared and war info is refreshed; otherwise only the caller's entry is cleared and the counterpart receives a cancellation notice. The complexity of these conditions is a legibility defect, but the observable rule is "war is a two-step handshake, cancel is unilateral".

`SummonGuild` teleports guild members to a coordinate, honouring a party/class filter: `village` 0/2 are excluded from the generic filter; `village == 3` requires the character to be in that village (`BASE_GetVillage`); `village == 2` additionally requires `ClassMaster == MORTAL` (non-mortals receive `_NN_3rd_village_limit`). Members with `OnlyTrade` set are skipped. The cap is enforced in two geometric steps: after 30 teleports the destination increments by one tile on each axis, and after 45 it steps back by two - a crude crowd-spreading rule. `SummonGuild2` is a variant without the class/village-3 distinction.

**Rule workflow:**
```
DoAlly(my, target):
   validate ids ; if my ally == 0 -> ally = target, notice _SS_Ally_Declared
   else -> notice _SS_Ally_Canceled, ally = 0
   refresh war info for all members of my guild

DoWar(my, target):
   validate ids
   target != 0:
       if target declared back -> start war, notice _SS_War_Started, refresh members
       else                    -> declare, notice _SS_War_Declared to both guilds
   target == 0:
       if counterpart declared back -> cancel both, notice, refresh members
       else                         -> clear my entry, notice _SS_War_declare_canceled

SummonGuild(Guild, tx, ty, max, village):
   for each USER_PLAY of Guild without OnlyTrade:
        village filter / mortal filter
        DoTeleport(tx, ty) ; n++
        n == 30 -> (tx,ty)++ ; n == 45 -> (tx,ty) -= 2
        n >= max -> stop
```
*Confidence: HIGH.*

---

### Business Rule: BR-42/43 - Castle siege schedule and capture counter

**Overview:**
The castle zone (guild zone index 4) is governed by two rules: a time window advanced by `GuildProcess` (`:6223`-`:6282`) and a capture counter advanced by `RegenMob` (`:4362`-`:4413`).

**Detailed description:**
The schedule runs only on servers flagged as `CastleServer == 1` and only during hour 21. It uses `CastleState` as a three-value progression: from a resting state it announces `_DN_Castle_will_be_open` and sets `CastleState = 1`, broadcasting the state to every playing client (`:6272`-`:6280`); between minutes 5 and 10 it clears the castle rectangle (1036,1672)-(1144,1764) of all players except the owning guild, teleports stragglers at eight specific coordinates to (1057,1742), sets the door to state 3, spawns the three live towers (`TORRE_NOATUM1..3`) and sets `CastleState = 2` (`:6247`-`:6267`); after minute 50 it announces `_DN_Castle_will_be_closed` and moves to `CastleState = 3`; after minute 55 it announces `_DN_Castle_closed` and calls `FinishCastleWar`.

`FinishCastleWar` (`:4324`) is the reset rule: door back to state 1, purge the castle rectangle of the *owning* guild, set `CastleState = 0`, broadcast `_MSG_SendCastleState` to players twice (once as a signal and once as an `ESCENE_FIELD` parameter message, `:4332`-`:4346`), delete every mob in the world wearing face 219 (the castle door), and finally purge the rectangle of all players.

The capture rule lives in `RegenMob`, which is called per tick per playing user. It first normalises `CastleStatus`: unless the player is at the exact capture tile (1046,1690), the castle is in state 1 or 2, the guild is valid, the player is guild master, and the guild is not the incumbent, the counter is zeroed (`:4366`-`:4368`). Otherwise it counts how many *other* zones the guild owns (`:4374`-`:4378`); only a guild holding at least one other city can charge. Each successful tick increments `CastleStatus`, multicasts `_MSG_SendCastleState2` with `Parm = 1` to the surrounding grid, and announces the charge once at zero. When the counter passes 22 the guild takes zone 4 (guild and clan recorded), `FinishCastleWar` runs, and `ClearCrown` is applied to every user. With the 500 ms tick this is roughly 11 seconds of uninterrupted standing, but because the counter is only advanced inside `RegenMob` for `USER_PLAY` sessions in the castle rectangle, the real requirement is 22 processed ticks.

**Rule workflow:**
```
GuildProcess castle branch (CastleServer == 1 and hour == 21):
   state 0/1, minute < 5  -> notice will-open ; CastleState = 1 ; broadcast
   state 1, 5 <= min <= 10 -> purge rect except owner ; teleport stragglers ; door 3 ;
                              spawn towers ; CastleState = 2
   state 2, min > 50       -> notice will-close ; CastleState = 3
   state 3, min > 55       -> notice closed ; FinishCastleWar()

RegenMob(conn) castle branch:
   eligible = CastleState > 1 && at (1046,1690) && guild valid && GuildLevel == 9
              && guild != owner
   otherCities = count of zones (excluding 4) owned by guild
   if !eligible or otherCities == 0 -> CastleStatus = 0
   else: multicast charge packet ; CastleStatus++
         CastleStatus > 22 -> notice ; zone4 owner = guild/clan ; FinishCastleWar ;
                              ClearCrown for all users
```
*Confidence: HIGH.*

---

### Business Rule: BR-44/45/46/47/48 - Scheduled events (Battle Royale, Colosseum, newbie rotation, castle-server rotation)

**Overview:**
Four calendar rules share `GuildProcess` and `ProcessMinTimer`: a 20-minute-bucketed Battle Royale (`BrState`), a minute-stepped Colosseum (`ColoState`), a daily newbie-channel rotation, and a weekly castle-host rotation.

**Detailed description:**
**Battle Royale** is active only when `BRItem` is valid and the current server is the designated newbie server (or a test/local server). It uses two derived values: `tmmin2 = tm_min / 20` (which 20-minute block we are in) and `tmmin = tm_min % 20` (position inside the block). The states are: `BrState = 1` on the hour with notices and a level-band purge of the arena rectangle (2604,1708)-(2648,1744) - band 100..400 in the first block, 200..400 in the second (`:6329`-`:6366`); `BrState = 2` at minute offset 5 with the door set to 3, participants teleported/re-gridded and any party leader removed from their party (`:6314`-`:6346`); `BrState = 3` at offset 13, when the configured `BRItem` is spawned at (2621,1726) with a random rotation and a notice is sent to the arena area (`:6297`-`:6312`); and a reset to 0 at offset >=14 with the door restored to state 1 (`:6379`-`:6383`). The "remove party" rule is a deliberate anti-collusion measure: entering the arena dissolves group membership.

**Colosseum** is the densest rule in the file: a nine-state machine where each state is a (hour, minute-threshold, previous state) triple. From `ColoState = 8` at minute < 3 it resets taxes and the level-150 limit; at minutes >=3 it sets `ColoState = 1`, opens the door, and - during the newbie hour - sets `Colo150Limit` and purges levels 150..400; at minute 4 it generates the first wave; at 5 it generates a second wave and opens the inner door; at 7, 9 and 11 it generates further waves with state transitions; at 13 and 15 it generates the final pairs and then closes both doors, deletes the colosseum and resets to state 8 (`:6386`-`:6502`). Wave identity depends on which hour is active: during `GuildHour` the ids 0,1,2 are used, during `NewbieHour` the ids 5,6,7 - i.e. the same arena hosts two different monster sets depending on the hour, and the two hour variables can coincide.

**Newbie rotation** (`ProcessSecMinTimer.cpp:2036`-`:2057`) computes `NewbieServerID = (tm_mday - 1) % NumServerInGroup`; on a test/local server or when the local `ServerIndex` matches, `NewbieEventServer` is set; when a server stops being the newbie host, every trade session is cancelled (`RemoveTrade`) - a rule that protects players from trading across a channel change. **Castle rotation** (`:2060`-`:2071`) uses `BASE_GetWeekNumber()`, computes `wMod = week % 7` and `wDay = week / 7`, and flags `CastleServer` when `wMod == 0` and the parity of `wDay` matches the parity of `ServerIndex`. The literal `if (1 != 0)` around it is a permanent `true` carrying a source comment that calls it a probable debug flag.

**Rule workflow:**
```
Battle Royale (per 12 s tick, guild/newbie server only, BRItem > 0):
    block = tm_min / 20 ; off = tm_min % 20
    hour != BRHour or BrState != 0:
        off == 5  -> door 3 ; BrState = 2 ; notices ; purge band 100/200..400 ; dissolve parties
        off == 13 -> spawn BRItem at (2621,1726) ; BrState = 3 ; area notice
    hour == BRHour and BrState == 0 and off == 0:
        BrState = 1 ; ready notices ; purge band
    hour == BRHour and BrState == 3 and off >= 14:
        door 1 ; BrState = 0

Colosseum (per 12 s tick):
    minute < 3 -> ColoState = 0, taxes reset, Colo150Limit = 0
    minute >= 3 -> ColoState = 1, door 3, newbie hour -> Colo150Limit = 1 + purge 150..400
    minute >= 4 -> ColoState = 2, generate wave 0/5
    minute >= 5 -> ColoState = 3, inner door open
    minute >= 7 -> ColoState = 4, generate 1/6
    minute >= 9 -> ColoState = 5, generate 0,1 / 5,6
    minute >= 11 -> ColoState = 6, generate 1,2 / 6,7
    minute >= 13 -> ColoState = 7, generate 1,2 / 6,7
    minute >= 15 -> ColoState = 8, doors closed, DeleteColosseum, Colo150Limit = 0

Newbie rotation (every 2nd MinCounter tick):
    NewbieServerID = (tm_mday - 1) % NumServerInGroup
    TESTSERVER or LOCALSERVER or ServerIndex == NewbieServerID -> NewbieEventServer = 1
    else if was newbie -> cancel every open trade, NewbieEventServer = 0

Castle rotation:
    week = BASE_GetWeekNumber() ; wMod = week % 7 ; wDay = week / 7
    wMod == 0 && (wDay % 2) == (ServerIndex % 2) -> CastleServer = 1 else 0
```
*Confidence: HIGH.*

---

### Business Rule: BR-49 - RvR kingdom war

**Overview:**
The RvR (kingdom vs kingdom) event is compiled behind `#ifdef RVR_ON` (`:6609`) and is a four-state machine driven from `GuildProcess`.

**Detailed description:**
Activation requires `NewbieEventServer == 1`, a weekday (not Sunday or Saturday), and the configured `RvRHour`. The states advance on minute boundaries: state 0 announces the start with a five-minute warning and moves to 1; state 1 at minute 5 spawns the two kingdom towers (`RVRTORRE_1`, `RVRTORRE_2`), announces the start, resets both kingdom point counters and moves to 2; state 2 at minute 29 silently moves to 3; state 3 at minute 34 purges the kingdom rectangle (1020,1916)-(1286,2178), deletes every mob whose `GenerateIndex` is one of the two towers, announces the end, resets points and returns to 0.

Because the entire block is `#ifdef`-guarded, the feature is absent in the default build configuration: the `Release`/`Debug` preprocessor definitions in `Code/TMSrv/TMSrv.vcxproj:61` and `:79` do not define `RVR_ON`, so `RvRState`, `RvRRedPoint` and `RvRBluePoint` are maintained but never advanced in shipped binaries. This is a build-time business rule that the configuration files and documentation do not mention.

**Rule workflow:**
```
#ifdef RVR_ON
if NewbieEventServer == 1 and weekday and hour == RvRHour:
    state 0, minute < 5  -> notice begin ; RvRState = 1
    state 1, minute >= 5 -> spawn RVRTORRE_1/2 ; notice start ; points = 0 ; RvRState = 2
    state 2, minute >= 29 -> RvRState = 3
    state 3, minute == 34 -> purge rect ; delete tower mobs ; notice end ; points = 0 ; RvRState = 0
#endif
```
*Confidence: HIGH.*

---

### Business Rule: BR-50/51/52 - Duel and ranking arena

**Overview:**
`DoRanking` (`:8052`) starts a duel, `ProcessRanking` (`:7847`) ticks it down, and both share a small set of globals (`RankingProgress`, `Ranking1/2`, `RankingTime`, `nRanking1/2`, `lTitle1/2`).

**Detailed description:**
For `tDuel == 0` (1v1) the rule validates both indices, copies both character names, teleports the challenger to `Ranking2X/Y` and the opponent to `Ranking1X/Y` (so the *first* listed player physically starts on the *second* spot), sends the start message, pushes a `_MSG_StartTime` of `2*121-2` to both clients, and arms `RankingProgress = 1`, `RankingTime = 121`. For team duels the rule is stricter: both participants must belong to a guild, the guilds must differ, `RankingProgress` becomes `tDuel + 1`, the clock is 301, guild names are resolved for display, and each side's "champion" name and level are recorded. For `tDuel == 3` (100v100) both guilds are summoned into the same rectangle with a cap of 100 each; for `tDuel == 1` and `2` the pooling loop walks the arena rectangle `cRanking1X..cRanking2X` by `cRanking1Y..cRanking2Y` and teleports up to 5 or 10 members of each guild into their respective half (`:8113`-`:8167`).

`ProcessRanking` is only active while `RankingTime > 0`. Each tick decrements the clock; every fifth tick it recounts survivors inside the *kingdom* arena rectangle, keyed either by session index (`RankingProgress == 1`) or by guild id, and aborts the match if either side has no survivors (`:7892`-`:7893`). It then emits damage/effect overlays whose footprint depends on remaining time: more than 180 ticks shows nothing, 120..180 shows the two central bands, 60..120 the four bands, and below 60 all six bands (`:7896`-`:7942`). At zero it recounts survivors, prints a draw or a winner notice to both arenas, and - when not a 1v1 - sends a `_MSG_MessageDBRecord` with an `RNK <server> <progress> <name> <title> <guild> ...` record so DBSrv can persist the ranking. Finally it clears every ranking global, blanks the first character of both names, and teleports everyone inside the kingdom rectangle to (2572,1752).

Because `ProcessRanking` is driven by the same 500 ms tick as everything else, `RankingTime` is in half-seconds: a 1v1 match lasts about 60 real seconds and a team match about 150, while the client is told 240 (2x121-2) - a visible mismatch between the advertised and actual duration.

**Rule workflow:**
```
DoRanking(tDuel, conn, enemy):
   tDuel == 0: validate ; record names/levels ; swap teleport ; start message ;
               StartTime 240 ; RankingProgress = 1 ; Ranking1/2 = conn/enemy ; time = 121
   else: require both guilds valid and different
         RankingProgress = tDuel+1 ; Ranking1/2 = guild ids ; time = 301
         record guild names + champion name/level
         tDuel == 3 -> SummonGuild(x2, cap 100)
         tDuel 1/2 -> teleport up to 5/10 members per guild from the arena grid

ProcessRanking (per 500 ms tick while RankingTime > 0):
   RankingTime--
   every 5th tick: count survivors by index (1v1) or guild ; either side empty -> RankingTime = 0
   emit overlays by remaining-time bucket (>=180 none, >=120 two, >=60 four, else six)
   RankingTime <= 0:
        recount ; draw or winner notice to both arenas
        non-1v1 -> send "RNK ..." record to DB
        clear all ranking globals ; ClearAreaTeleport(arena -> 2572,1752)
```
*Confidence: HIGH.*

---

### Business Rule: BR-53/54 - Teleport, great teleport, summon, and recall

**Overview:**
Four movement primitives governed by three different validation strengths: `DoTeleport` (`:8212`), `DoGreatTeleport` (`:8250`), `DoSummon` (`:8232`) and `DoRecall` (`:7558`).

**Detailed description:**
`DoTeleport` and `DoGreatTeleport` both require the grid placement helper to succeed (`GetEmptyMobGrid` and `GetEmptyMobGridGreat` respectively, the latter performing a wider search) and abort silently when it fails; on success they build a standard action packet, set `Effect = 1` (the teleport visual), unicast it to the player when the actor is a player, and multicast it to the surrounding grid. `DoSummon` differs in one important respect: it calls `GetEmptyMobGrid` but **ignores its return value** (`:8238`), so a failed placement still results in an action packet being sent with the requested coordinates, then a multicast - the actor's authoritative position is not updated, but observers see a teleport effect. This is a divergence from the other two primitives rather than a deliberate exception (no comment or guard explains it).

`DoRecall` is the death/penalty return rule. It computes a city id from `(MOB.Merchant & 0xC0) >> 6` (a two-bit field packed inside the merchant flags), takes a random offset of 0..14 from that city's spawn point, then overrides with the guild's castle spawn coordinates when the player's guild currently owns any zone (`:7569`-`:7581`). A second override applies to MORTAL characters below `FREEEXP`: they are sent to the newbie area around (2112,2042) with a +/-2 jitter (`:7583`-`:7587`). The recall then uses `GetEmptyMobGrid` and, unlike `DoTeleport`, does **not** check the return value before sending the packet. It also does not clear affects, cancel trade, or interrupt combat, so a recalled player keeps all states.

**Rule workflow:**
```
DoTeleport(mob, x, y): GetEmptyMobGrid ok -> action(Effect=1) -> unicast if player -> multicast
DoGreatTeleport(mob, x, y): GetEmptyMobGridGreat ok -> same
DoSummon(target, x, y): GetEmptyMobGrid (result unused) -> action -> unicast/multicast
DoRecall(conn):
   city = (Merchant & 0xC0) >> 6
   x,y = CitySpawn + rand()%15
   if Guild > 0 and Guild owns zone i -> x,y = GuildSpawn[i]
   if ClassMaster == MORTAL and Level < FREEEXP -> x,y = 2112+/-3, 2042+/-3
   GetEmptyMobGrid -> action(Effect=1) -> unicast/multicast
```
*Confidence: HIGH.*

---

### Business Rule: BR-55/56/57/58 - Combat engagement, mob skill rolls, parry and mount damage

**Overview:**
`SetBattle` (`:7218`) is the aggro-assignment rule and `MobAttack` (`:8927`) is the mob-side damage-declaration rule that fabricates the same `MSG_AttackOne` packets clients send.

**Detailed description:**
`SetBattle` refuses invalid indices, dead/empty slots, and self-targeting, then applies a range gate of 23 tiles per axis (versus the 16-tile `HALFGRIDX` used by affect processing) plus a role gate: the attacker must be an NPC, or the target must be an NPC or a playing user (`mob >= MAX_USER && (target >= MAX_USER || pUser[target].Mode == USER_PLAY)`) (`:7232`). On success it puts the mob into `MOB_COMBAT`, adds the target to the 13-entry `EnemyList`, and - for NPCs with a valid generator index - randomly speaks one of the generator's four `FightAction` lines, but only when the mob has no leader (i.e. is not a pet or a summoned unit).

`MobAttack` first applies a skill-remap rule for skill 109: `rand()%100 < 40` becomes 110, `< 80` becomes 111, otherwise stays 109 (`:8931`-`:8938`). It then iterates up to 13 targets, skipping dead or out-of-range ones, resetting `FlagLocal` and, for every non-109 skill, overwriting the incoming damage with `500 + rand()%200` (`:8956`) - meaning mob damage for ordinary skills is dictated by the server, not the packet. For each target it derives `kind = (skill % MAX_SKILL / 8) + 1`, and when `kind` is 1..3 it applies the attacker's corresponding `Special[kind]` as both an affect and a tick with a 100 % duration factor, sending a score update when either succeeded. Against player targets it computes an accuracy from `Dex/5`, plus 100 if the `0x1000000` learned-skill bit is set, plus 500 if `Rsv & 0x40`, feeds that to the shared `GetParryRate`, scales parry by 30 % for skills 79 and 22, rolls `rand()%1000+1`, and on a successful parry sets damage to -3 (or -4 when `Rsv & 0x200` and the roll is below 100). Finally, if the attacker's clan is 4 (mounts), damage is reduced to 40 %.

**Rule workflow:**
```
SetBattle(mob, target):
   reject invalid/dead/self
   require (mob >= MAX_USER) and (target >= MAX_USER or target is USER_PLAY)
   require |dx| <= 23 and |dy| <= 23
   mob.Mode = MOB_COMBAT ; AddEnemyList(target)
   if generator valid and no leader: random FightAction line

MobAttack(attacker, sm):
   if skill == 109: rand%100 -> <40 : 110 ; <80 : 111
   for each of MAX_TARGET entries:
       if skill != 109: damage = 500 + rand()%200
       if 0 <= skill < MAX_SKILLINDEX and SkillParm == 0:
            kind = skill/MAX_SKILL/8 + 1 ; if 1..3: SetAffect/SetTick(target, skill, 100, Special[kind])
       if target is player and damage > 0:
            accuracy = Dex/5 (+100 if learned 0x1000000) (+500 if Rsv & 0x40)
            parry = GetParryRate(...) ; skills 79/22 -> 30% of parry
            rand%1000+1 < parry -> damage = -3 (or -4 if Rsv & 0x200 and roll < 100)
            else if attacker clan == 4 -> damage = 40%
```
*Confidence: HIGH.*

---

### Business Rule: BR-59/60/61/62 - Affect and tick (buff/debuff) contract

**Overview:**
Two mirror functions, `SetAffect` (`:8270`) and `SetTick` (`:8317`), plus two targeted removals (`DoRemoveHide` `:8168`, `DoRemoveSamaritano` `:8190`) and the per-tick processor `ProcessAffect` (`:4513`), form the buff system's server half.

**Detailed description:**
`SetAffect` starts by refusing merchants and - with an off-by-one bound - any `conn > MAX_USER` (so `conn == MAX_USER` is accepted and writes into a mob slot). Aggressive effects (`g_pSpell[].Aggressive`) are refused when the target carries `RSV_BLOCK`. It then obtains a slot through `GetEmptyAffect(conn, sAffectType)` and refuses when the spell has no affect type or when no slot is available. Duration is computed as `(g_pSpell[skillnum].AffectTime + 1) * time / 100`, but when the *previously occupying* effect in that slot was type 1, 3 or 10 the duration is forced to 4 (`:8293`-`:8294`); the comment-free constant makes this a "short-lived refresh window" for stun/freeze-class effects. Duration and level are only raised when the same effect type is already present (`:8296`-`:8306`), which is the "no downgrade" rule; a caller-supplied `time` of 2139062143 (0x7F7F7F7F) is treated as "infinite" and pinned (`:8308`-`:8309`). The score is recomputed unconditionally at the end.

`SetTick` mirrors the structure with different arithmetic and guards: the merchant check is `Merchant == 1 && mob > MAX_USER` (so a merchant *player* is not protected), duration is `Delay * (AffectTime + 1) / 100` capped at 500,000,000, and tick types 1/3/10 are clamped to 2 whenever the computed duration reaches 3. Unlike `SetAffect` there is no "raise only" rule - the value, time and level are always overwritten.

`ProcessAffect` is the per-tick evaluator. Its first block implements the "fada" rule: if the actor wears item 753, 769 or 1726 in the accessory slot (13) and has a current target, it validates map attributes (`0x40` = PvP zone), refuses to attack same-leader/same-guild targets, refuses PvP-flagged targets in non-PvP zones, refuses targets with `Rsv & 0x10`, refuses max-level merchants with the `Merchant & 1` flag, and only then fabricates a `MSG_AttackOne` with skill 32 (34 for item 769, 36 for 1726) and damage -1, feeding it back into `ProcessClientMessage(..., TRUE)` - i.e. the AI re-enters the same attack pipeline as a client. The second block iterates the 32 affect slots applying type-specific math: type 17 (life aura) regenerates `Level/2 + Value` HP clamped into `[1..MaxHp]`, type 20 (poison/drain) subtracts a value that is reduced for NPCs by `LearnedSkill / MAX_USER`, and so on for the remaining types.

**Rule workflow:**
```
SetAffect(conn, skill, time, level):
   if Merchant == 1 or conn > MAX_USER -> FALSE
   if (Rsv & RSV_BLOCK) and spell aggressive -> FALSE
   slot = GetEmptyAffect(conn, AffectType) ; invalid -> FALSE
   Type = AffectType ; Value = AffectValue
   duration = (AffectTime + 1) * time / 100 ; if old type in {1,3,10} -> duration = 4
   keep max(duration), keep max(level) when same type
   if time >= 2139062143 -> duration = 2139062143
   GetCurrentScore(conn) -> TRUE

SetTick(mob, skill, delay, level):
   if Merchant == 1 and mob > MAX_USER -> 0
   if (Rsv & RSV_BLOCK) and aggressive -> 0
   slot = GetEmptyAffect(mob, TickType) ; require TickType > 0 and slot valid
   Time = Delay * (AffectTime + 1) / 100 ; cap 500000000
   if Time >= 3 and TickType in {1,3,10} -> Time = 2
   GetCurrentScore(mob) -> TRUE
```
*Confidence: HIGH.*

---

### Business Rule: BR-63/64/65/66 - Mount lifecycle, PK points and guilt

**Overview:**
Four rules keep mounts and PvP standing consistent, all located in `MountProcess`/`LinkMountHp`/`ProcessAdultMount` (`:4178`-`:4298`) and the 450-tick block of `RegenMob` (`:4415`-`:4509`).

**Detailed description:**
`MountProcess` requires the equipped item (slot 14) to be byte-identical to the caller-supplied mount when one is given (`memcmp`, `:4184`-`:4188`) - an anti-spoof check so a client cannot request a summon for a mount it no longer wears. It resolves the party leader (self when none), deletes every existing summon of that leader whose face is in `[315..344)` (`:4195`-`:4209`), then derives a "baby id" as `sIndex - 2320`; only ids in `[10..40)` with a positive `EF_MOUNTHP` produce a summon (`:4211`-`:4220`). `LinkMountHp` is the reverse mapping: it applies only to NPC slots (`mobConn >= MAX_USER`), clan 4, face 315..344, a valid summoner index whose session is playing, and - critically - a face/summoner item match (`(face - 315) != (mountItemIndex - 2330)` -> return). When the item's stored HP differs from the mob's current HP the item is rewritten and pushed to the client, making the mount item the persistent HP container.

`ProcessAdultMount` handles the 2360..2389 range: it clamps the stored HP into `[.., MaxHp]` after subtracting a loss, zeroes the feed effect when the resulting HP is <= 0, forces a `SendEquip` when HP crosses zero in either direction (mount death and revival are visually distinct), and only sends the item packet when the value actually changed. The feeding rule inside `RegenMob` runs every 450 ticks: `MountDiv` is intended to be `(sIndex - 2330) % 30` but is written `Mount->sIndex - 2330 % 30`, which C++ evaluates as `sIndex - 20` - so the "adult mounts feed faster" branch (`MountDiv > 15`) is effectively decided by `sIndex > 35`, i.e. always true for real mount ids (`:4443`-`:4448`). Feed is decremented by 4 (never by 2 in practice) and at feed <= 1 the mount dies.

The PvP rule in the same block increments the PK point every 450 ticks while it is below 75 and messages the player with `PKP - 75`, i.e. a negative "points until redemption" display. The guilty counter decrements once per tick regardless of the 450-tick gate; reaching zero restores the player's appearance through a `_MSG_CreateMob` broadcast and resets hostile mobs: every mob in `MOB_COMBAT` (mode 5) with clan 4 that targeted the player is retargeted to 0 and returned to peace (`:4483`-`:4508`).

**Rule workflow:**
```
MountProcess(conn, item):
   if item given and memcmp(item, Equip[14]) != 0 -> return
   leader = Leader or conn
   delete leader's summons with face in [315..344)
   id = Equip[14].sIndex - 2320
   if id in [10..40) and EF_MOUNTHP > 0 -> GenerateSummon(conn, id, item, 1)

LinkMountHp(mob):
   require NPC slot, clan 4, face in [315..344), summoner playing, face-315 == mountItem-2330
   if item HP != mob HP -> item HP = mob HP ; SendItem(slot 14)

ProcessAdultMount(conn, loss):
   require sIndex in [2360..2390)
   hp = stEffect[0].sValue - loss ; clamp to MaxHp ; if <= 0 -> feed = 0
   crossing zero -> SendEquip ; changed -> SendItem

RegenMob (every 450 ticks):
   PKP < 75 -> PKP++ ; message PKP-75
   mount feed: feed -= (MountDiv > 15 ? 4 : 2) ; feed <= 1 -> mount dies
   guilty > 0 -> guilty-- ; at 0 -> broadcast appearance + reset clan-4 aggressors
```
*Confidence: HIGH for mechanics; the mount-divisor behaviour is a verified precedence defect (HIGH).*

---

### Business Rule: BR-67/68/69/70/71/72 - NPC creation, spawn scatter, event scaling and generator cadence

**Overview:**
`CreateMob` (`:2865`), `GenerateMob` (`:2995`), `DeleteMob` (`:7020`), `DeleteGenerateMob` (`:9378`) and the generator loop in `ProcessMinTimer` (`ProcessSecMinTimer.cpp:2212`) implement the NPC population rules.

**Detailed description:**
`CreateMob` acquires a free NPC slot, pre-writes the requested name, and loads the record from `./<folder>/<name>` through `ReadMob` (`:6974`), which reports `EINVAL`/`EMFILE`/`ENOENT`/unknown errors distinctly and then forces `BaseScore = CurrentScore`. The name is then sanitised: byte 95 (`_`) becomes a space and byte 64 (`@`) becomes the string terminator (`:2890`-`:2897`), and `BaseScore.Merchant` is forced to 0 - mobs can never be merchants. The spawn scatter fills five route segments with `PosX/PosY +/- 2` random offsets **unless** the mob's face is 220, 219 or 358, in which case every segment is pinned to the exact coordinate (`:2901`-`:2914`) - the rule that keeps doors, ships and static objects from drifting. A face of 219 (castle door) additionally inherits `g_pGuildZone[4].ChargeGuild` as its guild so it is attackable by the correct side.

`WaitSec` is 10 for `Type == 0` and 40 otherwise, and `SegmentProgress` is 0 or 4 correspondingly (`:2936`, `:2945`), which sets how far along its route a freshly spawned NPC believes it is. HP starts at MaxHp, except during the newbie event for mobs below level 120, which start at 75 % (`:2942`-`:2943`) - a global difficulty reduction tied to the rotating newbie server. If the mob carries a `MaxMp` value it is interpreted as a skill index and both an affect and a tick are applied with 30,000 delay and level 200 (`:2973`-`:2979`), which is how aura/poison mobs are declared in data.

The generator cadence rule converts a per-generator `MinuteGenerate` value into a modulo test against `MinCounter` (`ProcessSecMinTimer.cpp:2216`-`:2224`), with indices 0, 1, 2, 5, 6, 7 excluded from the cadence (they are managed by other rules). After each spawn the value is re-randomised within the band that contains it: 500-999 -> `rand%500+500`, 1000-1999 -> `rand%1000+1000`, 2000-3799 -> `rand%1800+2000`, >=3800 -> `rand%1000+3800`. The >=3800 band additionally triggers the dungeon prize rule when `DUNGEONEVENT` is set: 5-9 items drawn from a 10-entry `DungeonItem` table are placed at one of 30 `DungeonPos` coordinates, each passed through `SetItemBonus(&item, 0, 0, 0)` so dungeon loot is enchanted (BR-27 with level 0). `DeleteMob` completes the rule set by decrementing the generator's `CurrentNumMob` with a floor of 0 for NPC slots and clearing HP, mode, grid cell and party (`:7032`-`:7052`).

**Rule workflow:**
```
CreateMob(name, x, y, folder, type):
   slot = GetEmptyNPCMob() ; 0 -> log + return -1
   ReadMob(record) ; failure -> 0
   sanitise name (_ -> space, @ -> terminator) ; Merchant = 0 ; affects cleared
   face in {220,219,358} ? segments pinned : segments = (x,y) + rand()%5-2
   face 219 -> Guild = castle owner, GuildLevel = 0
   GenerateIndex = -1 ; Formation 0 ; RouteType = type ; Mode = MOB_PEACE ; Leader = 0
   WaitSec = (type == 0) ? 10 : 40 ; SegmentProgress = (type == 0) ? 0 : 4
   GetCurrentScore(MAX_USER) ; HP = MaxHp
   newbie event and level < 120 -> HP = 3*HP/4
   GetEmptyMobGrid failed -> release slot, return -2
   MaxMp != 0 -> SetAffect(slot, MaxMp, 30000, 200) + SetTick(...)
   multicast _MSG_CreateMob

generator tick (per 12 s):
   skip indices 0,1,2,5,6,7
   MinuteGenerate <= 0 -> skip
   MinCounter % MinuteGenerate == index % MinuteGenerate -> GenerateMob(index)
        re-randomise MinuteGenerate inside its band
        if band >= 3800 and DUNGEONEVENT -> spawn 5..9 bonus items at a random DungeonPos
```
*Confidence: HIGH.*

---

### Business Rule: BR-73/74 - Area purge and summon generation

**Overview:**
Five `ClearArea*` variants (`:5730`-`:5866`) plus `GenerateSummon` (`:2572`) define how the server mass-moves or mass-creates actors.

**Detailed description:**
Every purge variant shares the same traversal and the same filters: only `USER_PLAY` sessions whose mob is not `USER_EMPTY`, and only positions inside the inclusive rectangle. Inside that set, dead players (`Hp <= 0`) are revived to exactly 2 HP and a score update is sent before the movement - a deliberate rule so that a purge never creates a corpse that cannot be moved. `ClearArea` recalls players to their own city/guild spawn; `ClearAreaLevel` adds a level window (`minLv..maxLv`) so events can eject only a particular band (used to expel level 150+ from the newbie colosseum and level 100+/200+ from BR); `ClearAreaGuild` acts as the *inverse* rule - it recalls every player **not** in the given guild (used to evict non-owners from the castle); `ClearAreaTeleport` sends everyone in the rectangle to fixed coordinates; `ClearAreaQuest` additionally resets `QuestFlag` before recalling.

`GenerateSummon` is the pet/merc creation rule. It refuses when no NPC slot is available (`_NN_Cant_Create_More_Summons`), validates the summon id against `MAX_SUMMONLIST`, and then enforces a **mutual-exclusivity** rule by scanning every member of the leader's party against every summon definition: if any party member already has a summon whose face differs from the requested one, the whole call is refused (`:2607`-`:2613`) - one visual family of summons per party. Existing matching summons are accounted for first (with a visibility check that produces an action packet instead of a new mob), and only the remainder up to `Num` is created. Each created mob copies the summon template, is capped at the owner's base level (`<= MAX_LEVEL`), gets `^` appended to its name with underscores turned back into spaces, has its affects cleared, and receives stat scaling from `pSummonBonus[SummonID]`: `Int * Unk / 100` and `Special[2] * Unk2 / 100` added to damage, `Unk3/Unk4` to AC and `Unk5/Unk6` to MaxHp (`:2687`-`:2705`). Mount-specific scaling then applies `EF_MOUNTSANC` (capped at 100) to Con and adds `6 * EF_MOUNTSANC` to damage (`:2729`-`:2753`), and helmet faces 543/545 and 544/546 are replaced by 734/735 so the summon's appearance matches the rider's tier.

**Rule workflow:**
```
ClearArea{Level,Guild,Teleport,Quest}(rect[, params]):
   for each USER_PLAY inside rect:
        if Hp <= 0 -> Hp = 2 ; SendScore
        Guild variant: skip if Guild == owner
        Quest variant: QuestFlag = 0
        DoRecall / DoTeleport to target coordinates

GenerateSummon(conn, id, item, num):
   leader = Leader or conn ; slot = GetEmptyNPCMob() (0 -> message, abort)
   id in [0..MAX_SUMMONLIST) ; face = summon template face
   for each party member: conflicting face already summoned -> abort
   count existing matching summons (visibility packet when out of view)
   for remaining: allocate slot, copy template
        level = min(owner level, MAX_LEVEL)
        name: append '^', '_' -> ' '
        damage += Int*Unk/100 + Special[2]*Unk2/100
        AC     += Int*Unk3/100 + Special[2]*Unk4/100
        MaxHp  += Int*Unk5/100 + Special[2]*Unk6/100
        Leader = leader ; Formation 5 ; RouteType 5 ; Mode = MOB_PEACE ; LastTime = now
        if item: mountsanc = min(EF_MOUNTSANC, 100) -> Con/Damage scaling ;
                 helmet 543/545 -> 734, 544/546 -> 735
```
*Confidence: HIGH.*

---

### Business Rule: BR-75/76/77 - Treasure map and combination economy

**Overview:**
`CombineTreasureMap` (`:8398`) and `Combine` (`:8509`) are the two item-consumption rules in this component.

**Detailed description:**
`CombineTreasureMap` implements a **layout-sensitive** rule: the six map fragments (788, 789, 790, 791, 792, 793) must occupy carry slots `i`, `i+1`, `i+9`, `i+10`, `i+18`, `i+19` - that is, a 2x3 block in the 9-column inventory grid. For each candidate base index it re-validates the bounds before every access (`i+19 >= MAX_CARRY -> continue`), so a partially matching layout cannot read out of bounds. On a full match it writes an audit record `ETC treasure_map <server> <account> <char>` to DBSrv, clears the six slots and re-sends each, and messages `_NN_Treasure_Map`. If no layout matches, the fallback rule converts **every** item in `788..793` into item 485 and counts them; if the count is positive it congratulates the player, otherwise it returns 0. The fallback is therefore destructive for a partial set: holding three fragments yields three 485s, not three fragments retained for a later attempt.

`Combine` requires ten units of a given item index. The count rule honours stacking: an item whose effect 0 is 61 contributes its `cValue` rather than 1 (`:8521`). If fewer than ten are found it returns without consuming anything; otherwise it clears ten units (tracked through a `clearitem` counter that is not visible in the excerpted head, but the loop breaks once ten are removed) and builds a prize whose index is the caller's `PrizeItem`, with effects 0 and 1 set to effect 59 and `rand()%100`, and effect 2 set to effect 59 with `rand()%50 + 50` - a guaranteed 50..99 on the third effect. The prize is placed in the first empty carry slot by writing two 32-bit words through pointer casts (`*(int*)&Carry[i] = *(int*)&Prize`, `:8583`-`:8584`) - an aliasing/alignment assumption about the packed `STRUCT_ITEM`. If no carry slot is free the prize is dropped on the ground at the player's position.

**Rule workflow:**
```
CombineTreasureMap(conn):
   for i in [0..MAX_CARRY-20]:
        require Carry[i]=788, [i+1]=789, [i+9]=790, [i+10]=791, [i+18]=792, [i+19]=793
        -> DB record "ETC treasure_map ..." ; clear + resend six slots ; message ; TRUE
   fallback: every Carry in 788..793 -> 485, resend, count
        count > 0 -> message _NN_Congratulations, TRUE
        else      -> 0

Combine(conn, itemIndex, prizeIndex):
   sum = 0
   for carry slots: if index == itemIndex -> sum += (effect0 == 61 ? value : 1)
   if sum < 10 -> return (nothing consumed)
   consume 10 units
   prize: index = prizeIndex, eff0 = (59, rand%100), eff1 = (59, rand%100), eff2 = (59, 50+rand%50)
   place in first empty carry slot, else CreateItem on ground
```
*Confidence: HIGH for the map rule; HIGH for `Combine`'s count and prize rules (consumption detail inferred from the counter idiom, MEDIUM).*

---

### Business Rule: BR-78 - Vital application bounds

**Overview:**
`ApplyHp`/`ApplyMp` (`:8591`, `:8625`) and `SetReqHp`/`SetReqMp` (`:8659`, `:8674`) are the only sanctioned writers of current HP/MP in this component; regen and item use go through them.

**Detailed description:**
The apply pair is a rate limiter. `ReqHp` is first clamped down to `MaxHp`, then the rule only acts when the request exceeds the current value; the delta is capped at **2,000 per call** (`:8604`-`:8605`), the resulting value is saturated at 2,000,000,000, and finally clamped to the request - producing the invariant `current <= ReqHp <= MaxHp`. The return value reports whether anything changed, which lets callers avoid redundant score packets. `ApplyMp` is an exact structural copy of the same rule for mana.

The normalisation pair is the guard used after external writes (items, DB loads). `SetReqHp` clamps a negative `ReqHp` to 0, clamps `CurrentScore.Hp` down to `MaxHp`, and then raises `ReqHp` up to at least the current HP - i.e. the request can never fall below the actual value, which prevents a subsequent `ApplyHp` from spuriously healing. Both pairs refuse connections outside `[1..MAX_USER)`; the apply pair does **not** perform that check at all (`:8591` begins directly with `pUser[conn].ReqHp`), relying on caller discipline.

**Rule workflow:**
```
ApplyHp(conn):
   if ReqHp > MaxHp -> ReqHp = MaxHp
   if ReqHp <= Hp -> return FALSE
   delta = min(ReqHp - Hp, 2000) ; Hp += delta ; saturate 2e9 ; Hp = min(Hp, ReqHp)
   return TRUE
SetReqHp(conn):
   validate conn ; ReqHp < 0 -> 0 ; Hp > MaxHp -> MaxHp ; ReqHp < Hp -> ReqHp = Hp
```
*Confidence: HIGH.*

---

### Business Rule: BR-80 - Name and account lookup resolution

**Overview:**
`GetUserByName` (`:6793`) is the server's single name resolver: whisper targets, guild invites, trade requests and moderator commands all funnel through it. It implements a two-namespace lookup with a syntactic switch.

**Detailed description:**
A leading `'+'` in the query selects **account-name** lookup: the call uppercases the substring after the marker with `_strupr` and compares it against `pUser[i].AccountName` (`:6795`-`:6813`). Without the marker the query is matched case-sensitively against `pMob[i].MOB.MobName` (`:6815`-`:6826`). In both modes the scan runs `i` from 1 to `MAX_USER-1`, skipping slots whose mob is `MOB_EMPTY` and whose session is not `USER_PLAY`, and returns 0 when nothing matches - the same "0 means nothing" convention as the slot allocators.

Two asymmetries are worth recording. First, `_strupr` mutates the caller's buffer in place (the pointer arithmetic is `name + 1`, so the marker itself survives), meaning the caller's string is altered as a side effect of a lookup. Second, account names are compared case-insensitively *by normalisation* while character names are compared case-sensitively with no normalisation, so two characters differing only in case are distinguishable while two account names differing only in case are not - an inconsistency that is invisible at the call sites. The `+` syntax is not documented in any header and appears only in the handlers that build the query.

**Rule workflow:**
```
if name[0] == '+':
    strupr(name + 1)
    for i in [1..MAX_USER): mob alive and USER_PLAY -> if strcmp(name+1, AccountName) == 0 -> return i
    return 0
for i in [1..MAX_USER): mob alive and USER_PLAY -> if strcmp(name, MobName) == 0 -> return i
return 0
```
*Confidence: HIGH.*

---

### Business Rule: BR-81/82/83/84 - Administrative control surface, shutdown and logging

**Overview:**
The window menu (`WM_CREATE` `:4055`, `WM_COMMAND` `:4081`), the shutdown path (`WM_CLOSE` `:4116`, `WM_DESTROY` `:4145`), the graceful-drain state machine (`ProcessSecMinTimer.cpp:42`-`:119`) and the log writers (`StartLog` `:6654`, `Log` `:6701`, `CrackLog` `:6671`) form the operational control surface.

**Detailed description:**
The menu exposes six commands: `Mob > ReloadMob` re-reads the NPC generator file (`mNPCGen.ReadNPCGenerator()`), `Sistema > Reboot` merely sets `ServerDown = 1` (it does *not* restart anything immediately), `SaveAll` calls `SaveAll()` (defined in `Code/TMSrv/imple.cpp:33`, outside this component), `ReadGuild` re-reads guild data from disk, `ReadGuildName` re-initialises the shared guild-name cache, and `ReadGameConfig` re-runs `ReadConfig` + `DrawConfig` against the live world. There is no confirmation for any of these except `Exit`.

The shutdown rule is a two-stage drain. `WM_CLOSE` asks for confirmation; on acceptance it emits a billing logout, writes guild files, closes the main log and destroys the font, then defers to `DefWindowProc`, which triggers `WM_DESTROY` -> `WSACleanup()` + `PostQuitMessage(0)`. The `Reboot` path instead sets `ServerDown = 1`, which makes `ProcessSecTimer` increment the value each tick, broadcast `g_pMessageStringTable[ServerDown/20 + 17]` every twentieth tick (i.e. the "server closing in N minutes" countdown strings), and at 120 switch the tick to 200 ms. From then on each tick closes exactly one non-empty user, advancing `UserCount`, until `UserCount >= MAX_USER`, at which point it logs `"sys,server down complete"`, destroys the font and posts the quit message (`:55`-`:94`). Because `UserCount` is a persistent global starting at 1 and only advances inside this loop, the drain is resumable but also sticky: any re-entry re-processes from the last index.

Logging is file-per-boot with a second-resolution name (`TM_%02d_%02d_%04d_%02d_%02d_%02d.txt` in `./Log`), rotated daily by `ProcessMinTimer` comparing `tm_mday` against `LastLogDay`/`LastChatLogDay`/`LastItemLogDay`. `Log(str1, str2, ip)` prepends a timestamp and, when the IP is non-zero, the dotted-quad form of the raw integer, then concatenates `<str2> <str1>` and writes both to the file and to the window title - the window title is therefore the live tail. `CrackLog(conn, str)` is the sanitised variant that resolves connections to account names (masking the last two characters) or mob names, appends the caller's message, and logs it as `-system`.

**Rule workflow:**
```
WM_CREATE  -> build File/Mob/Sistema menus
WM_COMMAND -> ReloadMob | Reboot(ServerDown=1) | SaveAll | ReadGuild | ReadGuildName |
              ReadGameConfig | Exit
WM_CLOSE   -> confirm ; billing logout ; WriteGuild ; fclose(log) ; DeleteObject(font)
WM_DESTROY -> WSACleanup ; PostQuitMessage(0)

server down drain (per tick while ServerDown in (0..120)):
   ServerDown % 20 == 1 -> SendNotice(countdown string)
   ServerDown++ ; at 120 -> SetTimer(TIMER_SEC, 200)
   ServerDown == 120 entering: close one user per tick until UserCount >= MAX_USER
   -> font delete ; PostQuitMessage
```
*Confidence: HIGH.*

---

### Business Rule: BR-85 - World reboot / reset

**Overview:**
`Reboot()` (`:7127`) is the only function that rebuilds world state in place; it is called once from `WinMain` (`:3505`) and is not reachable from the menu (the menu's "Reboot" only begins a shutdown).

**Detailed description:**
The reset walks the entire 4096x4096 mob, item and height grids zeroing them (`:7129`-`:7137`) - 50 million byte/word writes plus 16.7 million height bytes, which is why the boot takes a visible pause. It then zeroes the three live-tower flags, loads `MobName.txt` through `BASE_InitializeMobname`, loads the NPC generator list, and reads `./heightmap.dat` (16,777,216 bytes) into `pHeightGrid`; a missing heightmap aborts the remainder of `Reboot` with a log entry (`:7146`-`:7153`), leaving the world without BaseDef, language strings and summons - a fail-open configuration again.

On success it calls `BASE_InitializeBaseDef()` (item/spell tables), `BASE_ApplyAttribute(pHeightGrid, MAX_GRIDX)` (terrain attributes), `BASE_InitializeMessage("Language.txt")` (the message table used by every `g_pMessageStringTable[...]` access in the component), and `mSummon.Initialize()`. It then re-places every initial item from `g_pInitItem[0..g_dwInitItem)` at its configured coordinates and rotation, and for each placed item that carries an `EF_KEYID` in `[1..14)` it calls `UpdateItem(ID, STATE_LOCKED, ...)` - the rule that gate/door items start locked. Finally it recomputes `WeekMode` from the current weekday and hour: 5 on the pre-battle day, 0 before the battle hour on battle day, else 4 (`:7195`-`:7204`), so a server restarted mid-cycle resumes the correct war phase. It ends by locking the colosseum door.

**Rule workflow:**
```
Reboot():
   zero pMobGrid/pItemGrid/pHeightGrid (4096x4096)
   LiveTower[0..2] = 0
   BASE_InitializeMobname("MobName.txt") ; mNPCGen.ReadNPCGenerator()
   open heightmap.dat ; missing -> log + return
   read pHeightGrid ; close
   BASE_InitializeBaseDef() ; BASE_ApplyAttribute(pHeightGrid) ; BASE_InitializeMessage(...)
   mSummon.Initialize()
   for each g_pInitItem[i]:
        CreateItem(pos, item, rotate, 1)
        if EF_KEYID in [1..15) -> UpdateItem(ID, STATE_LOCKED)
   WeekMode = 4 ; if tm_wday == GuildDay-1 -> 5 ; if tm_wday == GuildDay and hour < GuildHour -> 0
   SetColoseumDoor(1)
```
*Confidence: HIGH.*

---

### Business Rule: BR-86/87/88/89 - Small contracts (zone reporting, geometry, challenge reset, client gate)

**Overview:**
Four small but load-bearing rules complete the inventory.

**Detailed description:**
`GuildZoneReport` (`:739`) serialises the five `g_pGuildZone[i].ChargeGuild` values into `_MSG_GuildZoneReport` with `ID = ServerIndex` and pushes it to DBSrv; it is called at boot (`:3636`) and after each war resolution (`:6564`), making DBSrv the cross-server record of zone ownership. `GetAngle` (`:6936`) converts a delta into a 3,600-unit circle (`atan2 * 3600 / 6.283184`) with normalisation into `[0..3600]`, returning 0 for degenerate axes; `GetLength` (`:6954`) squares the deltas, returns 100 for anything above 100, 0 for non-positive, and truncates the square root otherwise - so distance is silently capped at 100 units for all callers. `ClearChallanger` (`:7209`) zeroes both `ChallangeGuild` and `pChallangerMoney[i]` for zones `[0..ValidGuild)`, which is the reset half of BR-34's auction. `ProcessClientMessage` (`Code/TMSrv/ProcessClientMessage.cpp:106`) treats any client-originated `_MSG_UpdateScore` as tampering: it logs `"cra client send update score"` and adds 2 crack points, the only type-91 anomaly in the code base.

**Rule workflow:**
```
GuildZoneReport: build MSG_GuildZoneReport{Type, Size, ID = ServerIndex, Guild[0..4]} -> DB
GetAngle(dx,dy): 0 if either delta is 0; else round(atan2(dy,dx)*3600/2pi) normalised to [0..3600]
GetLength(dx,dy): s = dx^2+dy^2 ; s > 100 -> 100 ; s <= 0 -> 0 ; else trunc(sqrt(s))
ClearChallanger: for i < ValidGuild: ChallangeGuild = 0 ; pChallangerMoney[i] = 0
Client _MSG_UpdateScore -> Log + AddCrackError(conn, 2, 91)
```
*Confidence: HIGH.*

### 3.3 Business rule coverage index

Every rule identified in 3.1 is analysed in exactly one detailed card in 3.2. This index makes the mapping auditable.

| Rule IDs covered | Detailed card (3.2) | Confidence |
|------------------|---------------------|------------|
| BR-01, BR-02, BR-03 | Configuration contract, fail-open default and self-generation | HIGH |
| BR-04 | Drop-bonus matrix validation | HIGH |
| BR-06 | Treasure conversion configuration | HIGH (parsing) / MEDIUM (runtime use) |
| BR-05, BR-79 | Free-experience threshold and party-bonus clamp | HIGH |
| BR-07, BR-08, BR-09 | Server identity resolution and DB connectivity | HIGH |
| BR-10 | Optional billing connectivity | HIGH |
| BR-11, BR-12, BR-14 | Environment special-casing, group counting and boot spawn | HIGH |
| BR-13 | Fixed-resolution dual-clock scheduling | HIGH |
| BR-15, BR-16 | Connection admission control | HIGH |
| BR-17 | Crack-point accumulation and forced logout | HIGH |
| BR-18, BR-19, BR-20 | Session timeout, failure table, and periodic persistence | HIGH |
| BR-21, BR-22 | Session close and character logout semantics | HIGH |
| BR-23, BR-24, BR-25 | Item granting, world-drop creation and treasure boxes | HIGH |
| BR-26 | Ground item decay | HIGH (mechanics) / LOW (intent) |
| BR-27 | Random item bonus generation (`SetItemBonus`) | HIGH (flow) / MEDIUM (per-slot meaning) |
| BR-28 | Secondary per-slot bonus (`SetItemBonus2`) | HIGH |
| BR-29, BR-30 | Level-up item reward contract | HIGH |
| BR-31 | Circlet sub-god progression | HIGH |
| BR-32 | Crown and dragon-insignia reconciliation | HIGH |
| BR-33, BR-34 | Guild challenge eligibility and fame auction | HIGH |
| BR-35, BR-36, BR-37, BR-38 | Guild war lifecycle and winner determination | HIGH |
| BR-39, BR-40, BR-41 | Alliance, war declaration, and guild teleport caps | HIGH |
| BR-42, BR-43 | Castle siege schedule and capture counter | HIGH |
| BR-44, BR-45, BR-46, BR-47, BR-48 | Scheduled events (Battle Royale, Colosseum, newbie rotation, castle-server rotation) | HIGH |
| BR-49 | RvR kingdom war | HIGH |
| BR-50, BR-51, BR-52 | Duel and ranking arena | HIGH |
| BR-53, BR-54 | Teleport, great teleport, summon, and recall | HIGH |
| BR-55, BR-56, BR-57, BR-58 | Combat engagement, mob skill rolls, parry and mount damage | HIGH |
| BR-59, BR-60, BR-61, BR-62 | Affect and tick (buff/debuff) contract | HIGH |
| BR-63, BR-64, BR-65, BR-66 | Mount lifecycle, PK points and guilt | HIGH (defect at `:4443` confirmed) |
| BR-67, BR-68, BR-69, BR-70, BR-71, BR-72 | NPC creation, spawn scatter, event scaling and generator cadence | HIGH |
| BR-73, BR-74 | Area purge and summon generation | HIGH |
| BR-75, BR-76, BR-77 | Treasure map and combination economy | HIGH / MEDIUM (consumption detail) |
| BR-78 | Vital application bounds | HIGH |
| BR-80 | Name and account lookup resolution | HIGH |
| BR-81, BR-82, BR-83, BR-84 | Administrative control surface, shutdown and logging | HIGH |
| BR-85 | World reboot / reset | HIGH |
| BR-86, BR-87, BR-88, BR-89 | Small contracts (zone reporting, geometry, challenge reset, client gate) | HIGH |

---

## 4. Component Structure

### 4.1 File-level map of the component boundary

```
Code/TMSrv/
|-- Server.cpp                     9,449 lines  - this component (process host, globals, ~110 services)
|-- Server.h                         418 lines  - this component's public surface (~120 decls, ~150 externs)
|
|   -- files that define types owned/used by Server Core ----------------------------
|-- CUser.h / CUser.cpp             user session object (socket, cargo, trade, billing flags)
|-- CMob.h  / CMob.cpp              mob object (scores, affects, AI pathing, party/enemy lists)
|-- CItem.h / CItem.cpp             ground item object
|-- CNPCGene.h / CNPCGene.cpp       NPC generator table (mNPCGen.pList, MinuteGenerate)
|-- CReadFiles.h / CReadFiles.cpp   guild / challenger / mac / admin persistence
|-- CWarTower.cpp / .h              live tower (draws from GuildProcess -> CWarTower::GuildProcess)
|-- CCastleZakum.cpp / .h           castle quest extension (ProcessMinTimer, ReadCastleQuest)
|
|   -- files that consume Server Core heavily (afferent) ----------------------------
|-- ProcessClientMessage.cpp/.h     packet dispatch (switch over _MSG_*)  -> 2 grid refs
|-- ProcessDBMessage.cpp/.h         DBSrv message dispatch                -> 220 refs
|-- ProcessSecMinTimer.cpp          tick body (both clocks)               -> 260 refs
|-- SendFunc.cpp / SendFunc.h       outbound packet builders             -> 252 refs
|-- GetFunc.cpp  / GetFunc.h        lookup/grid helpers                  -> 163 refs
|-- imple.cpp                       SaveAll + misc                       -> 143 refs
|-- MobKilled.cpp                   death/loot/exp pipeline               -> 294 refs
|-- _MSG_*.cpp (58 files)           per-message handlers (largest: _MSG_UseItem 160 KB,
|                                   _MSG_Quest 74 KB, _MSG_Attack 44 KB)
|
|   -- shared, outside the component directory --------------------------------------
Code/Basedef.h  (2,500+ lines)       STRUCT_MOB/ITEM/AFFECT, MAX_* constants, g_pItemList,
                                     g_pSpell, g_pBonusValue, g_pDropBonus, g_pInitItem
Code/Basedef.cpp                     definitions of the above + serialisers
Code/CPSock.h / CPSock.cpp           CPSock socket class, WSA_* message ids, MSG_* structs
Code/ItemEffect.h                    EF_* effect codes (EF_GRADE0..5, EF_MOUNTHP, EF_KEYID...)
```

### 4.2 Internal organisation of `Server.cpp` (ordered by line)

| Line range | Group | Contents |
|-----------|-------|----------|
| 1-40 | includes | `Windows.h`, `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`, local headers |
| 43-490 | global definitions | ~150 tuning scalars, hardcoded coordinate tables (`WaterScrollPosition`, `Pesa*`, `Pista*`, `PistaRune`), world arrays, guild tables, ranking state |
| 494-760 | small domain helpers | `ClearCrown`, `AddCrackError`, `PutItem`, `GuildZoneReport`, `GetAFont` (stub) |
| 761-1392 | configuration | `ReadConfig`, `DrawConfig`, `AddFailAccount`, `CheckFailAccount`, `IsFree`, `SendBilling`, `SendBilling2` |
| 1393-2478 | item generation | `SetItemBonus` (941 lines), `SetItemBonus2` |
| 2479-2864 | guild + summon | `Challange`, `GenerateSummon` |
| 2865-3391 | NPC population | `CreateMob`, `GenerateMob`, `RebuildGenerator` |
| 3392-3716 | host | `InitApplication`, `InitInstance`, `WriteHeader`, `WriteArmor` (stub), `WinMain`, `WriteWeapon`, `BuildList` (stub) |
| 3718-4155 | window procedure | `MainWndProc` (timers, four socket message ids, paint, menu, close, destroy) |
| 4157-4512 | player services | `GetUserInArea`, mount trio, `ProcessBILLMessage` (stub), `CheckIdle`, `FinishCastleWar`, `RegenMob` |
| 4513-5640 | affect engine | `ProcessAffect` (924 lines), damage broadcasters |
| 5641-6202 | area control | `SummonGuild*`, `ClearArea*`, door controls, `GenerateColosseum`, `DecideWinner` |
| 6203-6653 | calendar | `GuildProcess` (castle, BR, colosseum, guild war, RvR) |
| 6654-7126 | platform services | logging trio, slot allocators, `GetUserByName`, `CloseUser`, `ReadMob`, `DeleteMob`, `SaveUser`, `CharLogOut`, `Reboot` |
| 7127-7557 | world items | `ClearChallanger`, `SetBattle`, `CreateItem`, `RemoveTrade`, `RemoveParty`, `UpdateItem` |
| 7558-8167 | guild/diplomacy/duel | `DoRecall`, `DoWar`, `DoAlly`, `DoDeprivate`, `ProcessRanking`, `DoRanking` |
| 8168-8730 | affects + decay | removal helpers, teleport trio, `SetAffect`, `SetTick`, `ProcessDecayItem`, `CombineTreasureMap`, `Combine` |
| 8730-9449 | rewards + spawn utilities | `ApplyHp/Mp`, `SetReqHp/Mp`, `ClearMapa`, `ReadLevelItemConfig`, `DoItemLevel`, `SetCircletSubGod`, `SummonServer`, `MobAttack`, chat/item logs, `ParseMobMercString`, `DeleteGenerateMob`, `CreateTreasureBox` |

### 4.3 Structural observations

- **One translation unit owns 110 function definitions and ~200 globals.** There is no internal module boundary, no anonymous namespace, no static linkage of helpers - every helper is exported through `Server.h` and therefore part of the public ABI of the component.
- **`Server.h` is a de-facto global header**: 418 lines containing 4 includes of sibling headers, ~120 function prototypes and ~150 `extern` declarations. Any change to a global's type ripples to all 65 dependent TUs.
- **Three files (`Server.cpp`, `ProcessSecMinTimer.cpp`, `MobKilled.cpp`) contain the majority of the world-mutating logic**, but `Server.cpp` alone performs 1,049 direct accesses to the core arrays (the highest of any file), making it the densest mutation site in the project.
- **Data-heavy zoning tables are hardcoded in source** rather than configuration: water scroll destinations, Pesa (arena) standard positions, the seven pista rooms, the Balrog portal graph, ranking spawn rectangles and the treasure-map rune sequences (`PistaRune[7][10]`) are compile-time constants (`:56`-`:487`).

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (this component's efferent edges)

```
Server.cpp
 |- Basedef.h/.cpp        STRUCT_*, MAX_*, g_pItemList, g_pSpell, g_pBonusValue,
 |                        g_pDropBonus, g_pInitItem, g_dwInitItem, BASE_InitModuleDir,
 |                        BASE_InitializeHitRate, BASE_InitializeBaseDef,
 |                        BASE_ApplyAttribute, BASE_InitializeMessage, BASE_InitializeMobname,
 |                        BASE_InitializeGuildName, BASE_GetGuildName, BASE_GetItemAbility,
 |                        BASE_GetBonusItemAbility, BASE_ClearItem, BASE_GetVillage,
 |                        BASE_GetWeekNumber, BASE_GetItemCode, BASE_GetEnglish,
 |                        BASE_CheckPacket, BASE_InitializeServerList, g_pServerList,
 |                        g_pGuildName, ChargedGuildList, g_pMessageStringTable
 |- CPSock.h/.cpp          CPSock (ListenSocket, DBServerSocket, BillServerSocket, per-user cSock),
 |                        WSA_ACCEPT/WSA_READ/WSA_READDB/WSA_READBILL, MSG_* payload structs
 |- ItemEffect.h          EF_GRADE0..5, EF_MOUNTHP, EF_MOUNTSANC, EF_INCUDELAY, EF_GROUND,
 |                        EF_KEYID, EF_SANC, EF_AC, EF_HP
 |- Language.h            g_pMessageStringTable indices (_NN_*, _SN_*, _DN_*, _SS_*, _DD_*)
 |- CUser.h/.cpp          pUser[], CUser::AcceptUser, CUser::CloseUser
 |- CMob.h/.cpp           pMob[], CMob::GetCurrentScore, ProcessorSecTimer, AddEnemyList,
 |                        SetSegment, CheckGetLevel
 |- CItem.h/.cpp          pItem[]
 |- CNPCGene.h/.cpp       mNPCGen.pList[], NumList, MinuteGenerate, CurrentNumMob, FightAction
 |- CReadFiles.h/.cpp     ReadMacblock, ReadGuild, WriteGuild, WriteChallanger
 |- CWarTower.cpp         CWarTower::GuildProcess(timeinfo)
 |- CCastleZakum.cpp       CCastleZakum::ReadCastleQuest, ProcessMinTimer
 |- ProcessClientMessage  ProcessClientMessage (re-entered from ProcessAffect for fada attacks)
 |- ProcessDBMessage      (inverse direction: called by MainWndProc)
 |- SendFunc.cpp          SendItem, SendScore, SendEquip, SendClientMessage, SendNotice,
 |                        SendNoticeArea, SendGuildNotice, SendWarInfo, SendChat, SendSay,
 |                        SendClientSignal, SendClientSignalParm, SendDamage, SendWeather,
 |                        SendGridMob, SendEnvEffect, GridMulticast, GetCreateMob, GetAction,
 |                        GetCreateItem, GetPKPoint, SetPKPoint, GetGuilty, SetGuilty, GetGuild
 |- GetFunc.cpp           GetEmptyMobGrid, GetEmptyMobGridGreat, GetEmptyItemGrid, GetInView,
 |                        GetAttribute, GetParryRate, GetEmptyAffect
 |- imple.cpp             SaveAll (only external definition of a Server.h prototype)
 `- Win32 API             CreateWindowEx/RegisterClass/SetTimer/WSAStartup/MessageBox/
                          TextOutA/CreateFont/timeGetTime/localtime/PostQuitMessage
```

### 5.2 External dependencies

| Dependency | Version / detail | Nature |
|-----------|------------------|--------|
| Windows SDK (Win32, `WSAAsyncSelect` model) | `WIN32`, `_WINDOWS`, `MultiByte` code page | Platform (no CRT-only alternative) |
| MSVC toolchain | `PlatformToolset v143` (`Code/TMSrv/TMSrv.vcxproj:28`, `:35`), `WholeProgramOptimization` in Release | Build |
| C runtime (`fopen`/`sscanf`/`sprintf`/`strncpy`/`_open`/`_read`) | `_CRT_SECURE_NO_WARNINGS` defined in both configurations (`:61`, `:79`) | Runtime |
| DBSrv (separate project in the same solution) | TCP 7514, hand-written `MSG_*` binary protocol | Service |
| Billing/BISrv (external, described as deprecated in README) | address+port from `biserver.txt` | Service |
| W2 client (build v7662 per README) | TCP `GAME_PORT` 8281, `MSG_STANDARD` binary protocol | Service |
| Data files | `gameconfig.txt`, `LevelItem.txt`, `localip.txt`, `biserver.txt`, `serverlist.txt` (read by `BASE_InitializeServerList`, `Code/Basedef.cpp:6218`, with fallback `../../Common/serverlist.txt`), `MobName.txt`, `Language.txt`, `heightmap.dat` (16 MB), `./npc/*`, `./BaseMob/*` (DBSrv), `./Log/*` | File |
| Third-party libraries | **none** (no package manager, no vendored deps, no `#include` of non-Windows headers) | - |

### 5.3 Runtime dependency graph (call-time)

```
WinMain --> ReadConfig --> (globals) --> DrawConfig
        --> ReadLevelItemConfig --> LevelItem[][][]
        --> Reboot --> BASE_* , mNPCGen , mSummon , CreateItem , UpdateItem
        --> StartListen --> WSA_ACCEPT (window message)
MainWndProc
   |- WM_TIMER/SEC  --> ProcessSecTimer --> CMob::ProcessorSecTimer
   |                                    --> SaveUser --> DBServerSocket
   |                                    --> CloseUser --> DeleteMob / DBServerSocket
   |- WM_TIMER/MIN  --> ProcessMinTimer --> GenerateMob --> CreateMob --> ReadMob (disk)
   |                                    --> GuildProcess --> DecideWinner / SummonGuild /
   |                                                        GenerateColosseum / SetCastleDoor /
   |                                                        CWarTower::GuildProcess
   |                                    --> UpdateItem (gate items)
   |- WSA_READ      --> cSock.ReadMessage --> ProcessClientMessage --> Exec_MSG_* --> this component's services
   |- WSA_READDB    --> DBServerSocket.ReadMessage --> ProcessDBMessage --> this component's services
   |- WSA_READBILL  --> ReadBillMessage --> ProcessBILLMessage (empty)
   `- WM_COMMAND    --> ReadConfig / SaveAll / CReadFiles::ReadGuild / mNPCGen.ReadNPCGenerator
```

---

## 6. Afferent and Efferent Coupling

Coupling counted by distinct translation units (80 `.cpp` files in `Code/`, excluding build output) referencing a symbol. "Afferent" = TUs depending on this unit; "Efferent" = distinct external units this unit depends on.

| Component (unit) | Afferent | Efferent | Criticality | Notes |
|------------------|----------|----------|-------------|-------|
| `pUser` array (`Server.cpp:269`) | 65 | 0 | Critical | Shared mutable session registry; any layout change breaks 65 TUs |
| `pMob` array (`Server.cpp:270`) | 64 | 0 | Critical | 25,000 x `CMob`; same exposure as `pUser` |
| `Log()` (`Server.cpp:6701`) | 53 | 3 (CRT, Win32, log FILE*) | High | De-facto logging facade; also used by DBSrv-side helpers |
| `SendClientMessage` (`SendFunc.cpp`, consumes this component's state) | 46 | - | High | 46 TUs send through it; indirectly coupled to `pUser` |
| `AddCrackError` (`Server.cpp:684`) | 22 | `Log`, `CharLogOut` | High | Anti-cheat entry point used by every handler |
| `DBServerSocket` (`Server.cpp:262`) | 17 | `CPSock` | Critical | Single shared DB channel; no queueing |
| `g_pGuildZone` (`Basedef.cpp`, declared `Server.h:417`) | 15 | - | High | Zone ownership drives war/castle/challenge |
| `DoTeleport` (`Server.cpp:8212`) | 12 | `GetEmptyMobGrid`, `SendFunc` | Medium | Movement primitive reused by events |
| `pMobGrid` (`Server.cpp:363`) | 11 | 0 | Critical | Spatial index; written by ~10 TUs |
| `pItem` (`Server.cpp:279`) | 10 | 0 | High | 5,000 ground items |
| `GuildInfo[65536]` (`Server.cpp:361`) | 10 | 0 | High | Fame/persistence data |
| `PutItem` (`Server.cpp:709`) | 10 | `SendItem`, `SendClientMessage` | High | Single arbiter of inventory grants |
| `DoRecall` (`Server.cpp:7558`) | 8 | `GetEmptyMobGrid`, `SendFunc` | Medium | Death/penalty/area-exit path |
| `DeleteMob` (`Server.cpp:7020`) | 8 | `GridMulticast`, `mNPCGen` | High | Must stay consistent with grid + generator counters |
| `CharLogOut` (`Server.cpp:7085`) | 8 | `SaveUser`, `DeleteMob`, `CloseUser` | High | 8 TUs force logouts (e.g. billing, crack) |
| `CloseUser` (`Server.cpp:6834`) | 7 | `CUser`, `CPSock`, DB channel | Critical | Torn-down paths, three return points |
| `GenerateMob` (`Server.cpp:2995`) | 7 | `CreateMob`, `mNPCGen`, `pMobMerc` | High | Spawn driver |
| `SecCounter` (`Server.cpp:384`) | 7 | 0 | High | Half-second tick leaked as "seconds" |
| `pItemGrid` (`Server.cpp:364`) | 7 | 0 | High | Item spatial index |
| `g_pGuildWar` / `g_pGuildAlly` (`:359`-`:360`) | 6 / 6 | 0 | Medium | 65,536-entry relation tables |
| `mNPCGen` (`Server.cpp:266`) | 6 | `CNPCGene` | High | Generator table read by timer + spawn code |
| `SetItemBonus` (`Server.cpp:1393`) | 5 | `g_pItemList`, `g_pBonusValue`, `g_pDropBonus`, `rand` | Critical | 941-line stochastic generator |
| `CreateItem` (`Server.cpp:7254`) | 5 | `GetEmptyItem`, `GetEmptyItemGrid`, `SendFunc` | High | Ground-item factory |
| `SetAffect` (`Server.cpp:8270`) | 5 | `g_pSpell`, `GetEmptyAffect`, `CMob::GetCurrentScore` | High | Buff entry point |
| `SaveUser` (`Server.cpp:7056`) | 5 | DB channel, `CUser` | Critical | Persistence |
| `ClearArea*` family (`:5754`-`:5866`) | 5 | `DoRecall`, `DoTeleport`, `SendScore` | Medium | Event hygiene |
| `GenerateSummon` (`Server.cpp:2572`) | 3 | `mSummon`, `pSummonBonus`, `GetEmptyNPCMob` | Medium | Pet system |
| `g_pTreasure` (`Server.cpp:273`) | 3 | - | Medium | 8 configured treasure conversions |
| `SetTick` (`Server.cpp:8317`) | 3 | `g_pSpell`, `GetEmptyAffect` | Medium | DoT entry point |
| `WeekMode` (`Server.cpp:332`) | 3 | - | Medium | Guild-war state machine variable |
| `GetEmptyUser` / `GetEmptyItem` / `GetEmptyNPCMob` | 2 / 3 / 1 | - | High | Slot allocators (asymmetric usage) |
| `ListenSocket` (`Server.cpp:371`) | 4 | `CPSock` | Critical | Single listener (port 8281) |
| `WinMain` + `MainWndProc` | 0 | ~15 subsystems | Critical | Entry point / reactor |
| `Server.h` (whole header) | 65 (indirect) | 6 headers | Critical | Widest interface in TMSrv |

**Interpretation.** The component is a textbook *unstable hub*: it is depended upon by ~81 % of the server's translation units while depending on a comparatively small set of shared primitives (Basedef, CPSock, SendFunc, GetFunc). In Robert Martin's terms its instability `I = Ce/(Ca+Ce)` is very low (~0.15), which is appropriate for a foundational module - but its *abstractness* is also near zero (no interfaces, no virtual dispatch, only concrete globals), so it sits in the "zone of pain". Cohesion is thematic rather than functional: the file groups by *lifecycle stage* (boot -> config -> items -> NPC -> host -> services), not by domain, so `Server.cpp` simultaneously contains the process entry point, a stochastic item generator, calendar scheduling and inventory rules.

---

## 7. Endpoints

TMSrv Server Core exposes **no HTTP/REST, GraphQL or gRPC surface**. Its endpoints are socket events posted by `WSAAsyncSelect` and window messages. The tables below document them, plus the protocol they carry.

### 7.1 Network endpoints owned by this component

| Endpoint | Direction | Trigger / Message id | Handler | Description |
|----------|-----------|----------------------|---------|-------------|
| TCP 8281 (`GAME_PORT`, `Code/Basedef.h:44`) | Inbound, listen | `WSA_ACCEPT` (`WM_USER+3`) | `MainWndProc:4006` | Accepts W2 game clients; allocates a `pUser` slot; enforces admin-reservation and shutdown gates |
| per-client socket | Inbound, read | `WSA_READ` (`WM_USER+100`) | `MainWndProc:3919` | Reads framed `MSG_STANDARD` packets, dispatches each to `ProcessClientMessage` |
| DBSrv `DBServerAddress:7514` | Outbound + inbound | `WSA_READDB` (`WM_USER+2`) | `MainWndProc:3812` | Bidirectional control/data channel to DBSrv (`ProcessDBMessage`) |
| Billing server (`biserver.txt`) | Outbound + inbound | `WSA_READBILL` (`WM_USER+4`) | `MainWndProc:3731` | Billing authorisation channel; inbound messages are discarded (`ProcessBILLMessage` is empty) |
| `WSA_ACCEPTADMIN` (`WM_USER+5`), `WSA_READADMIN` (`WM_USER+6`), `WSA_READADMINCLIENT` (`WM_USER+7`) | declared only | none | none | Constants exist in `Code/CPSock.h:29`-`:31` but **no handler is registered** in this component (admin socket path is not implemented in TMSrv) |

### 7.2 Window-message endpoints (administrative)

| Endpoint | Method (message) | Description |
|----------|------------------|-------------|
| `WM_TIMER` / `wParam == TIMER_SEC` (0) | 500 ms tick (200 ms during shutdown) | Gameplay tick: save, flush, per-mob processing |
| `WM_TIMER` / `wParam == TIMER_MIN` (1) | 12,000 ms tick | Calendar tick: spawns, log rotation, `GuildProcess` |
| `WM_COMMAND` / `IDC_MOBRELOAD` | menu | `mNPCGen.ReadNPCGenerator()` |
| `WM_COMMAND` / `IDC_REBOOT` | menu | `ServerDown = 1` (begins graceful drain) |
| `WM_COMMAND` / `IDC_SAVEALL` | menu | `SaveAll()` (implemented in `imple.cpp`) |
| `WM_COMMAND` / `IDC_READGUILD` | menu | `CReadFiles::ReadGuild()` |
| `WM_COMMAND` / `IDC_READGUILDNAME` | menu | `BASE_InitializeGuildName()` |
| `WM_COMMAND` / `IDC_READGAMECONFIG` | menu | `ReadConfig()` + `DrawConfig(0)` against a live world |
| `WM_COMMAND` / `IDC_EXIT` | menu | posts `WM_CLOSE` |
| `WM_CLOSE` | confirm + flush | billing logout, `WriteGuild`, log close, font delete |
| `WM_PAINT` | repaint | `DrawConfig(0)` re-renders configuration text |
| `WM_DESTROY` | teardown | `WSACleanup()` + `PostQuitMessage(0)` |

### 7.3 Client protocol scope

The client-facing endpoint carries a single binary envelope (`MSG_STANDARD`: Type, Size, ID, KeyWord, ClientTick) with **57 distinct message types** dispatched by `Code/TMSrv/ProcessClientMessage.cpp:66`-`:311` (login, character management, chat/whisper, movement/action, attack x3, item use, buy/sell/trade, 12 combination shops, party, guild, quest, ranking, teleport, PK mode, etc.). Detailed payload contracts belong to the per-message handlers (`_MSG_*.cpp`), which are outside this component but call into it.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv (same solution, `Code/DBSrv`) | Internal service | Character save/load, guild info, guild zones, ranking records | TCP, `WSAAsyncSelect`, hand-rolled framing (`CPSock::ReadMessage`) | Binary `MSG_*` structs (`MSG_DBSaveMob`, `MSG_SavingQuit`, `MSG_DBNoNeedSave`, `MSG_GuildInfo`, `MSG_GuildZoneReport`, `MSG_MessageDBRecord`) | Fixed address `:7514`; 2 reconnect attempts with `Sleep(200)`; failure -> `PostQuitMessage` (process exit) |
| Billing / BISrv | External service | Account authorisation, play-time billing | TCP, `WSAAsyncSelect`, `ReadBillMessage`/`SendBillMessage` | `_AUTH_GAME` (196-byte payload) + billing frames | Connect failure -> `BILLING = 0`; runtime disconnect -> close + `BillCounter = 360`; inbound messages are dropped (empty handler) |
| W2 game clients | External clients | All gameplay | TCP `:8281`, `WSAAsyncSelect` | `MSG_STANDARD` + typed payloads | Per-socket read retry (`Receive` called twice), then `CloseUser`; `NumError` accumulation; idle timeout; `_PACKET_DEBUG` `BASE_CheckPacket` validation when compiled with `_PACKET_DEBUG` |
| `gameconfig.txt` | File (read + write) | All tuning | Text, positional lines and tokens | `name value` pairs, 16-integer rows, treasure records | `MessageBox` + early return per section (fail-open); missing file -> defaults + regeneration |
| `LevelItem.txt` | File (read) | Level reward table | Text, 10 whitespace-separated integers per line | `cls type level item eff0 v0 eff1 v1 eff2 v2` | Missing file -> `MessageBox`, table zeroed; **no validation** of indices |
| `localip.txt` / `serverlist.txt` (read by `BASE_InitializeServerList`, `Code/Basedef.cpp:6218`, with fallback `../../Common/serverlist.txt`) | File (read) | Server identity resolution | Text / in-memory `g_pServerList` | dotted quad, group layout | Missing `localip.txt` -> message box, uninitialised buffer; no match -> message box + terminate |
| `biserver.txt` | File (read) | Billing endpoint | Text | `address port` | Missing -> empty address, connect will fail softly |
| `MobName.txt`, `Language.txt`, `heightmap.dat`, `./npc/*` | Files (read) | World data | Text / raw binary | - | Missing heightmap -> log + aborted `Reboot`; missing language -> empty message table (all `g_pMessageStringTable[...]` lookups degrade) |
| `./Log/TM_*.txt`, chat/item logs | Files (write) | Audit | Text | timestamped lines | `fopen` result not checked; `fprintf` guarded by `if (fLogFile)` |
| CReadFiles (`guild.txt`, challenger files, mac block, admin list) | File (read/write) | Guild/fame/challenger persistence, MAC filtering | Text | - | Delegated; called from menu, boot and `Challange` |
| Win32 timers (`SetTimer`) | Platform | Scheduling | WM_TIMER | ids 0/1 | No drift compensation; `SecCounter`/`MinCounter` are tick counts |
| Win32 UI (`CreateWindow`, `TextOutA`, `SetWindowText`) | Platform | Operator console | GDI | - | `GetDC`/`CreateFont` failures return silently |
| `Basedef` globals (`g_pItemList`, `g_pSpell`, `g_pBonusValue`, `g_pInitItem`) | In-process shared memory | Item/skill/bonus definitions | Direct array access | `STRUCT_ITEMLIST` etc. | No bounds checks on many accesses |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / effect |
|---------|----------------|----------|------------------|
| Event-driven reactor (WSAAsyncSelect + window messages) | `wndproc` switch on `WSA_ACCEPT`/`WSA_READ`/`WSA_READDB`/`WSA_READBILL`/`WM_TIMER` | `Server.cpp:3718`-`:4155` | Single-threaded, non-blocking I/O multiplexing without threads; all state mutation is serialised by the message pump |
| Composition root / manual DI | `WinMain` wires config, DB socket, billing socket, listener, generator, timers | `:3459`-`:3657` | All lifetimes are process-wide; no containers, no factories with lifetimes |
| God-object / global registry | `pUser`, `pMob`, `pItem`, grids, ~150 scalars defined at file scope and re-exported by `Server.h` | `:43`-`:449` | Zero-cost sharing across 65 TUs at the price of total coupling |
| Facade | `Server.h` exposes 120 functions and 150 externs as one header | `Server.h` | Any module can call any service without layering |
| Table-driven configuration | `g_pDropBonus[64]`, `LevelItem[4][4][400]`, `g_pTreasure[8]`, `weapon[32][9]`, `PistaRune[7][10]`, `WaterScrollPosition[3][10][2]` | `:56`-`:487`; `ReadConfig`, `ReadLevelItemConfig` | Gameplay tuning lives in data rather than branches |
| State machine (explicit enum-as-int) | `WeekMode` 0-5, `ColoState` 0-8, `BrState` 0-3, `RvRState` 0-3, `CastleState` 0-3, `ServerDown` counter | `:6203`-`:6650`, `:4362`, `ProcessSecMinTimer.cpp:42` | Calendar/event progression without a scheduler library |
| Object pool with free-list cursor | `GetEmptyUser`/`GetEmptyItem`/`GetEmptyNPCMob` (cursor `g_EmptyMob`), `GetUserFromSocket` linear scan | `:6729`-`:6791` | O(n) slot reuse; `g_EmptyMob` avoids rescanning the NPC tail |
| Factory (static creators) | `CreateMob`, `CreateItem`, `CreateTreasureBox`, `GenerateSummon`, `GenerateMob`, `GenerateColosseum` | `:2865`, `:7254`, `:9392`, `:2572`, `:2995`, `:5983` | All world-object instantiation funnels through named creators |
| DTO / packet structs | `MSG_STANDARD`, `MSG_Action`, `MSG_Attack`, `MSG_CreateItem`, `MSG_CreateMob`, `MSG_SavingQuit`, ... | `Code/CPSock.h`, `Code/Basedef.h` | Wire contract shared with client and DBSrv; no serialisation layer |
| Publish/subscribe (broadcast) | `GridMulticast`, `SendNotice`, `SendNoticeArea`, `SendGuildNotice`, `SendWarInfo`, `SendDamage*` | `SendFunc.cpp`, `:5437`-`:5640` | Spatial and social fan-out of state changes |
| Null object / no-op stub | `GetAFont` returns 0, `BuildList`, `WriteArmor`, `ProcessBILLMessage`, `IsImple`, `SendBilling` | `:756`, `:3713`, `:3455`, `:4300`, `:6829`, `:1377` | Decompilation leftovers; callers remain linked |
| Command dispatcher | `switch (LOWORD(wParam))` in `WM_COMMAND`; `switch (std->Type)` in `ProcessClientMessage` | `:4083`; `ProcessClientMessage.cpp:66` | No dispatch table - each case is a direct call |
| Two-phase destructive update | `Kingdom1Clear`/`Kingdom2Clear`, `pUser.Mode = USER_SAVING4QUIT`, `RankingTime` countdown | `ProcessSecMinTimer.cpp:2112`; `:6917`; `:8041` | Defers irreversible work by one tick to keep the current frame consistent |
| Idempotent reconciler | `ClearCrown`, `RebuildGenerator`, `SetCircletSubGod` | `:494`, `:3363`, `:8855` | Repairs persistent state from authoritative sources; safe to call repeatedly |

**Architectural decisions evidenced by the code**

1. **Single-threaded world simulation.** There are no synchronisation primitives, no worker threads, no `CreateThread` calls in the component; every mutation happens inside the window procedure. This eliminates data races and also eliminates any possibility of parallel scaling.
2. **Tick-quantised time.** All temporal behaviour is expressed as tick counts rather than wall-clock comparisons (except the calendar rules, which mix `localtime` with tick counters). This makes timing deterministic per tick but ties it to the 500 ms/12 s resolutions.
3. **Client-agnostic server authority by re-entry.** Mobs attack by constructing the same `MSG_AttackOne` a client would send and calling `ProcessClientMessage(..., TRUE)` (`:4622`); the `isServer` flag is what bypasses the `SKIPCHECKTICK`/tick validation. The AI therefore shares one damage pipeline with players.
4. **Global, process-lifetime memory.** Roughly 25,000 `CMob` + 1,000 `CUser` + 5,000 `CItem` + 3 x 16.7 MB grids + 65,536-entry guild tables are statically allocated; the process commits hundreds of megabytes at startup and never releases.
5. **Configuration as a first-class runtime artefact.** The server both consumes and emits `gameconfig.txt`, and it can reload it live from the menu - the configuration file is treated as a two-way interface with the operator rather than as immutable deployment input.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|-----------|----------------|-------|--------|
| High | `ProcessDecayItem` (`:8353`-`:8396`) | Loop bound `ItemCount` is incremented inside the body and wrapped at 5000; the examined slot is `ItemCount`, not `i` | Item decay is driven by a side-effecting global cursor; predictable decay requires understanding a self-modifying loop; concurrent callers cannot exist, but restarting the pass mid-way is not possible |
| High | `Reboot` (`:7127`-`:7157`) | Clears only the spatial grids, never `pUser`, `pMob`, `pItem` contents; aborts on missing `heightmap.dat` | World arrays can retain stale entries if `Reboot` is ever called on a live process; failure leaves BaseDef/Language/summons uninitialised while boot continues |
| High | `ReadConfig` (`:761`-`:1115`) | Fail-open partial application; no rollback; `WinMain` sets `ConfigReady = 1` unconditionally (`:3486`); runtime re-read from menu (`:4109`) | A typo in one section silently mixes new and default values in a live server; no log record of the partial apply |
| High | `ReadLevelItemConfig` (`:8762`-`:8799`) | `cls`/`type`/`level` from file are used as array indices with no range checks (`LevelItem[4][4][400]`) | Malformed or edited `LevelItem.txt` writes out of bounds of a static array -> memory corruption at boot |
| High | `SetItemBonus` (`:1393`-`:2333`) | 941-line function, nesting >=6, ~20 magic constants per branch, RNG-dependent, no unit-level documentation | Cannot be reasoned about or verified; balance changes are guesswork; the only functions with comparable size in the project are decompiled handlers |
| High | Timing model (`:3631`-`:3632`, `:384`-`:385`) | `SecCounter` is a 500 ms tick and `MinCounter` a 12 s tick, but names, comments and downstream arithmetic assume seconds/minutes | Timeouts, save cadence, PK accrual, mount feeding and NPC respawn are all a factor of 2 (or 5) off the nominal value |
| High | `CheckIdle` (`:4304`-`:4322`) | The `> ser - 1440` reset runs before the `< ser - 720` test, inverting the intended timeout band | Players idle 12+ minutes are never disconnected; players idle 6-12 minutes are |
| High | `WSA_READDB` recovery (`:3815`-`:3847`) | The reconnect success branch still calls `PostQuitMessage` after `Sleep(200)` | The documented "reconnect" behaviour cannot succeed; any DB blip terminates the process |
| High | `AddCrackError` (`:684`-`:707`) | Signed `int` accumulator with a 2x109 threshold and no validation of `val` or `conn` | Signed overflow (UB) before the threshold; negative `val` disables the anti-cheat; `pUser[conn]` dereferenced unvalidated |
| High | `ProcessAffect` fada branch (`:4518`-`:4622`) | Server-side AI re-enters `ProcessClientMessage(..., TRUE)` with a fabricated packet; boundary conditions combine `&&`/`||` in one expression (`:4518`) | AI attacks share the client validation path with only the `isServer` flag separating them; a change to handler validation can silently break or empower AI skills |
| Medium | `MountProcess`/`RegenMob` (`:4443`) | `Mount->sIndex - 2330 % 30` binds `%` before `-`; intended `(sIndex - 2330) % 30` | The "adult mounts consume 4 feed" branch is effectively always taken |
| Medium | `GenerateSummon` (`:2757`) | `pMob[MobEmpty].MOB.Guild = pMob[MobEmpty].MOB.Guild;` - self-assignment | Summoned pets never inherit guild affiliation as evidently intended; guild-war visibility of pets is wrong |
| Medium | `DrawConfig` (`:1279`-`:1281`) | `sprintf(String, "%s  ...", String, ...)` - source aliases destination | Undefined behaviour in the config writer; can truncate or garble the treasure lines that `ReadConfig` later re-parses |
| Medium | `ProcessSecTimer` (`ProcessSecMinTimer.cpp:127`) | `if (SecCounter % 120) CReadFiles::ReadMacblock();` | Reads the MAC block file 119 of every 120 ticks (~every 0.5 s) instead of every 60 s - continuous disk I/O |
| Medium | `SetAffect` (`:8272`, `:8319`) | `conn > MAX_USER` bounds instead of `>=`; `SetTick`'s merchant guard uses `&&` between `Merchant` and `mob > MAX_USER` | `conn == MAX_USER` writes into a mob slot; merchant players are not protected from ticks |
| Medium | `DoSummon` (`:8232`-`:8248`) | `GetEmptyMobGrid` result discarded before sending the action packet | Observers can see a teleport that the authoritative position never accepted - desync vector |
| Medium | `DoRecall` (`:7589`-`:7601`) | `GetEmptyMobGrid` result discarded; no affect/trade/combat cleanup | Recall can place the character on an occupied cell; buffs and trades survive death recall |
| Medium | `Reboot` week logic (`:7195`-`:7204`) | `WeekMode` recomputed from wall clock regardless of whether a war is in progress | Restarting during a war window can jump the state machine forward/backward |
| Medium | Hardcoded magic identifiers | Item ids 747, 3993/3994, 2330, 2360, 1727, 788-793, 485, 219/220/358, 543-549, 734/735, 753/769/1726, coordinates, `7514`, `192.168.0.153`, `192.168.0.50` | Behaviour changes require source edits; environments cannot be re-targeted without rebuild |
| Medium | `GuildInfo[65536]`, `ChargedGuildList` indexing | `GuildInfo` indexed by guild id with no upper bound check in several paths; `ClearCrown` scans only `MAX_SERVER` entries | Large static footprint; cross-server crowns beyond server 10 are invisible |
| Medium | Resource handling | `fopen` returns unchecked in `StartLog`/`WriteHeader`; `GetDC`/`CreateFont` failures silently ignored; `fclose(fLogFile)` without null check in `WM_CLOSE` | Missing `./Log` directory means no file logging with no operator feedback |
| Medium | `sprintf`-based formatting everywhere | Fixed 4096-byte global `temp` (`:277`) shared by all modules; many `sprintf` into 16/32-byte buffers (`FailAccount[16][16]`, `nRanking1[32]`) | Buffer overrun risk from long account/character names; `temp` is a cross-module clobber hazard |
| Medium | Localisation / operator messages | Portuguese and English strings hardcoded in `MessageBox` and inline log lines (`"erro, Nao foi possivel ..."`) | Cannot localise or machine-parse boot diagnostics |
| Medium | Dead declarations | `ParseCastleString` (`Server.h:182`), `ParseCampString` (`Server.h:184`) declared, undefined anywhere | Misleading API surface for integrators |
| Medium | Stubs returning success | `SendBilling` returns `TRUE` without doing anything (`:1377`); `ProcessBILLMessage` empty (`:4300`) | Billing disconnect is reported as successful while nothing is sent; all inbound billing state is lost |
| Low | `GetMobFromIndex` (`:6928`) | `if (index < 0 ...)` on an `unsigned int` parameter - dead condition | Slightly misleading validation |
| Low | `InitApplication` (`:3408`) | `if (!RegisterClass(&wc)) return RegisterClass(&wc);` - retries the same call in the failure path | Failure path performs a duplicate registration attempt |
| Low | `WM_CLOSE` (`:4116`) | `DefWindowProc` not called on the `IDNO` path (`break` before) - correct, but the default branch also lacks `return` | Handler returns 0 for unhandled messages instead of the default result |
| Low | `WriteHeader`/`WriteWeapon` (`:3431`, `:3659`) | Emit an HTML item-picker page referencing `opener.document.frmRegist` | Dead legacy web-tool code retained in the server binary |
| Low | `g_dLevel`/`g_dLevel1`/`g_dLevel2` (`:397`-`:399`) | Global scratch variables written by `SetItemBonus` as a side channel | Hidden coupling: callers can observe item-generation internals |
| Low | `ProcessSecTimer`/`ProcessMinTimer` naming | Names imply seconds/minutes but implement 500 ms/12 s | Perpetuates the timing misconception in every downstream reader |

**Positive characteristics worth recording** (not recommendations, just findings): the four socket paths all bound-check the derived user index; `PutItem`, `CreateItem`, `DoTeleport`, `SummonGuild`, `ClearArea*` and `DecideWinner` validate indices before array access; `CombineTreasureMap` re-validates bounds before every slot access; `_PACKET_DEBUG` builds compile in `BASE_CheckPacket` validation on both DB and client paths (`:3895`-`:3905`, `:3980`-`:3988`); and the shutdown drain closes sessions one per tick rather than aborting, which avoids partial saves.

---

## 11. Test Coverage Analysis

### 11.1 Evidence of what was searched

| Search | Scope | Result |
|--------|-------|--------|
| Test source files (`*test*`, `*Test*`, `*spec*`) | Entire repository, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` | **0 files** |
| Test-framework identifiers (`gtest`, `catch2`, `CppUnit`, `BOOST_TEST`, `TEST_CASE`, `ASSERT_EQ`, `unittest`) | `Code/*.cpp`, `Code/*/*.cpp` | **0 matches** |
| Test projects | `W2PP Code Project.sln` | Only 2 buildable projects: `DBSrv`, `TMSrv` (plus a "Solution Items" folder). **No test project** |
| CI configuration | Repository root | No `.github/`, no `.gitlab-ci.yml`, no `azure-pipelines.yml`, no `Jenkinsfile` |
| Build scripts | Repository root and `Code/` | No `CMakeLists.txt`, `Makefile`, `*.bat`, `*.ps1`, `*.json` (only `.vcxproj` / `.filters` / `.user`) |
| Assertions in production code | `Code/` | No `assert()` usage in the component; error detection is `if`-based and log-based |
| Test fixtures / mocks / stubs | `Code/` | None; the only "fakes" are the empty stubs listed in Section 9 |
| Documentation of expected behaviour | `docs/` | **Empty directory** (no test plans, no specs, no ADRs) |

### 11.2 Coverage table

| Unit / rule cluster | Unit Tests | Integration Tests | Coverage | Test Quality |
|---------------------|-----------|-------------------|----------|--------------|
| `WinMain` / `MainWndProc` (host, reactor) | 0 | 0 | 0 % | Not testable without a live Win32 message loop and real sockets; no harness present |
| `ReadConfig` / `DrawConfig` (BR-01..06) | 0 | 0 | 0 % | 355 lines of parsing with 12 modal-message failure paths; no sample configs or golden files in the repo |
| `SetItemBonus` / `SetItemBonus2` (BR-27/28) | 0 | 0 | 0 % | 1,085 lines of RNG logic; no seeded-RNG tests, no expected-value fixtures |
| `ReadLevelItemConfig` / `DoItemLevel` (BR-29/30) | 0 | 0 | 0 % | No `LevelItem.txt` sample committed; parser's wildcard matrix unverified |
| Guild rules (BR-32..41) | 0 | 0 | 0 % | Requires DB + client; no simulation harness |
| Castle / BR / Colosseum / RvR calendars (BR-42..49) | 0 | 0 | 0 % | Time-dependent; no clock injection point, so even manual verification needs wall-clock alignment |
| Combat & affects (BR-55..62) | 0 | 0 | 0 % | Damage/parry math and 32-slot affect engine entirely unchecked |
| Mounts / PK / guilt (BR-63..66) | 0 | 0 | 0 % | Contains the verified `%`-precedence defect; nothing would have caught it |
| NPC creation & generator (BR-67..72) | 0 | 0 | 0 % | Depends on `./npc` binaries and `MobName.txt` not present in the repo |
| Item lifecycle (BR-23..26, 75..77) | 0 | 0 | 0 % | Inventory/grid boundary conditions (MaxCarry vs MAX_CARRY, 2x3 map layout) unverified |
| Persistence (BR-20..22) | 0 | 0 | 0 % | Requires DBSrv; no protocol-level contract tests |
| Logging / admin surface (BR-81..84) | 0 | 0 | 0 % | Manual-only |
| Geometry helpers `GetAngle`/`GetLength` (BR-87) | 0 | 0 | 0 % | Pure functions - the cheapest units in the file to test - still uncovered |

**Aggregate:** 0 unit tests, 0 integration tests, 0 % automated coverage, no test infrastructure of any kind. Manual verification appears to have been the only quality gate (the `_PACKET_DEBUG` compile flag in the Debug configuration at `Code/TMSrv/TMSrv.vcxproj:61` is the sole debug-only aid, and it validates packet *size/type*, not business rules).

### 11.3 Consequence analysis

Because nothing in the component is covered, every rule in Section 3 is verified only by the fact that a live server has not yet been observed to fail. The four defect classes found by static inspection (precedence error at `:4443`, self-assignment at `:2757`, overlapping `sprintf` at `:1279`, unvalidated table indices at `:8774`) are all of the kind that a single unit test per function would have surfaced immediately, and all of them are in code paths with gameplay or persistence consequences. The absence of tests also means the two most fragile contracts - the `gameconfig.txt` positional grammar and the `MSG_DBSaveMob` payload - have no executable specification other than the code itself.

---

*End of report. This analysis is descriptive only; no project files were modified.*
