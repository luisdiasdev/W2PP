# Component Deep Analysis Report

**Component:** Server (TMSrv)
**Project:** W2PP (legacy C/C++ game server)
**Scope analyzed:** `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy/Code/TMSrv`
**Date:** 2026-08-19 17:13
**Folders ignored:** `.git`, `.opencode`
**Analysis type:** Read-only (no project files modified)

---

## 1. Executive Summary

The **Server (TMSrv)** component is the game/connection server of the W2PP MMO
server release. It is the process that game clients connect to on TCP port 8281
(`GAME_PORT`), and it owns the server's main loop, all socket I/O (accept,
receive, and outbound), the connection lifecycle, and a large portion of the
core in-world game logic. The component is implemented almost entirely in a
single translation unit, `legacy/Code/TMSrv/Server.cpp` (9,449 lines), whose
public contract and global state are declared in `legacy/Code/TMSrv/Server.h`
(418 lines).

The component's runtime is a **Win32 GUI message-pump application**: `WinMain`
(Server.cpp:3459) registers a window class, creates the main window, initializes
Winsock, connects to the database server (DBSrv) and optionally the billing
server, then enters a `GetMessage`/`DispatchMessage` loop. All asynchronous
socket events are delivered to `MainWndProc` (Server.cpp:3718) as Win32 window
messages via `WSAAsyncSelect`. There is **no threading** in the gameplay path;
every connection, every packet, and all world simulation run serially on the UI
thread. Timers (`SetTimer`) drive `ProcessSecTimer` (500 ms) and
`ProcessMinTimer` (12 s), implemented in `ProcessSecMinTimer.cpp` and declared
in `Server.h`.

**Key findings:**

- The component is the architectural hub of TMSrv: `Server.h` declares dozens of
  `extern` globals (the `pUser[MAX_USER]`, `pMob[MAX_MOB]`, `pItem[MAX_ITEM]`
  world arrays, the `pMobGrid`/`pItemGrid`/`pHeightGrid` maps, guild/war state,
  event and ranking config) that every `_MSG_*` handler, `GetFunc`, `SendFunc`,
  and timer function in the whole TMSrv process reads and mutates.
- Socket handling is entirely asynchronous and event-driven; `MainWndProc`
  handles four socket message classes: `WSA_ACCEPT` (new client),
  `WSA_READ` (game client data), `WSA_READDB` (DBSrv responses), and
  `WSA_READBILL` (billing server responses).
- Persistence is delegated to DBSrv over a custom binary protocol; this
  component never touches the filesystem for account data, only for config/data
  loading (`gameconfig.txt`, `localip.txt`, `biserver.txt`, `heightmap.dat`,
  guild/region files) and log output.
- Security is minimal: packet validation via `BASE_CheckPacket` is compiled only
  under `_PACKET_DEBUG` (Debug builds); Release builds trust client packets with
  only an `ID` bounds check and a `ClientTick` sentinel check in the dispatcher.
- There are **no automated tests** anywhere in the analyzed scope (0 test files
  found), so all correctness guarantees are by-inspection only.

---

## 2. Data Flow Analysis

The following traces the primary inbound data path (client packet) through the
component, plus the secondary paths (DBSrv response and periodic world
simulation).

```
1. Client connects  → WSAAsyncSelect FD_ACCEPT → MainWndProc WSA_ACCEPT
                       → GetEmptyUser() → pUser[User].AcceptUser(ListenSocket.Sock)
                       → reservation policy (MAX_USER ceiling, admin slots,
                         ServerDown rejection)
2. Client sends data → FD_READ → MainWndProc WSA_READ
                       → GetUserFromSocket(wParam) → pUser[User].cSock.Receive()
                       → ReadMessage(&Error,&ErrorCode) (framing)
3. Packet framing     → per-user read loop; errors → CloseUser(User)
4. Dispatch           → ProcessClientMessage(User, Msg, FALSE)
                       → ID bounds check; ServerDown>=120 reject; ClientTick
                         sentinel check; switch(std->Type) → Exec_MSG_* handler
5. Business logic     → _MSG_* handlers mutate pUser/pMob/pItem world arrays,
                         call GetFunc helpers, and build responses via SendFunc
6. Outbound           → SendFunc → pUser[n].cSock.SendMessageA / SendOneMessage
                       → client
7. Cross-user effects → GridMulticast / SyncMulticast / MapaMulticast to
                         nearby players in pMobGrid neighborhood
8. Persistence path   → login/save requests → DBServerSocket.SendOneMessage
                         (e.g. MSG_DBSaveMob, MSG_SavingQuit, MSG_DBNoNeedSave)
                       → DBSrv → FD_READ → MainWndProc WSA_READDB
                       → ProcessDBMessage(Msg) → per-result handlers
9. Periodic world     → SetTimer(TIMER_SEC/TIMER_MIN) → ProcessSecTimer /
                         ProcessMinTimer → RegenMob, ProcessAffect,
                         ProcessDecayItem, ProcessRanking, GuildProcess,
                         weather, castle/war state
```

Entry points into this component from the OS are: `WinMain` (boot), `MainWndProc`
(every window/socket message), and `WM_TIMER` (world simulation). The component's
principal outbound sinks are the game client sockets, the DBSrv socket, and the
optional BillServer socket.

---

## 3. Business Rules & Logic

### Overview of the business rules:

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Startup | Boot requires `localip.txt`, a matching ServerList entry, and a DBSrv connection on port 7514 | Server.cpp:3509-3570 |
| Startup | `NumServerInGroup` derived from server-list; clamped to 1..10 | Server.cpp:3572-3582 |
| Startup | Optional billing connect from `biserver.txt`; failure disables BILLING | Server.cpp:3590-3618 |
| Connection | Max concurrent users `MAX_USER` (1000); last 10 slots reserved for admins (`ADMIN_RESERV`) | Server.cpp:4022-4030 |
| Connection | New connections rejected with `_NN_ServerReboot_Cant_Connect` when `ServerDown != -1000` | Server.cpp:4032-4039 |
| Shutdown | Graceful shutdown countdown lasts 120 s; notices broadcast every 20 s; all users force-saved | ProcessSecMinTimer.cpp:42-119 |
| Validation | Incoming packet `ID` must be in `[0, MAX_USER)`; else rejected and logged | ProcessClientMessage.cpp:42-51 |
| Validation | Packets ignored while `ServerDown >= 120` | ProcessClientMessage.cpp:53-54 |
| Validation | Client packets with `ClientTick == SKIPCHECKTICK` are discarded (anti-spoof sentinel) | ProcessClientMessage.cpp:63-64 |
| Resilience | DBSrv connection loss triggers 2 retries (200 ms apart); failure → `PostQuitMessage` | Server.cpp:3812-3881 |
| Resilience | Billing connection loss sets a 360 s reconnect counter; failure disables billing | ProcessSecMinTimer.cpp:130-167 |
| Idle Policy | Users idle > 720 s (12 min) are disconnected; `LastReceiveTime` also normalized if > `SecCounter` or < `SecCounter-1440` | Server.cpp:4304-4322 |
| Anti-Abuse | Failed-account tracker holds up to 16 accounts; cleared every 10 min | Server.cpp:1334-1361; ProcessSecMinTimer.cpp:2136-2137 |
| Persistence | One user saved every 8 s, round-robin, via `_MSG_DBSaveMob` | ProcessSecMinTimer.cpp:169-198 |
| Billing | In `BILLING==2` child-pay mode, characters at/beyond `FREEEXP` are logged out in defined hour windows | ProcessSecMinTimer.cpp:178-185 |
| World | Item decay every 2 s; ranking every 4 s; per-user regen/affect every 16 s; idle check every 32 s | ProcessSecMinTimer.cpp:1101-1188 |
| World | Weather changes randomly each minute; `ForceWeather` overrides | ProcessSecMinTimer.cpp:2284-2311 |
| Combat | Mob AI driven per-tick (`MobAttack`, `RegenMob`, `ProcessAffect`) | ProcessSecMinTimer.cpp:1186-1187 |
| Config | `gameconfig.txt` is strict-format; header lines and parameter names are validated with fatal message boxes | Server.cpp:761-1115 |
| Config | Drop-bonus values must be in `[0,9999]`; `PARTYBONUS` clamped to `[50,200]` | Server.cpp:924-1013 |
| Teleport | Water/Carta/secret-room clear-and-teleport timers with fixed map coordinates | ProcessSecMinTimer.cpp:1108-1170 |

---

## Detailed breakdown of the business rules:

---

### Business Rule: Server Startup and Boot Sequence

**Overview:**
`WinMain` (Server.cpp:3459) is the process entry point. It performs one-time
initialization: Winsock init, log setup, config loading, world-array zeroing,
DBSrv and billing connections, timer installation, and game-port listening,
before entering the Win32 message loop that the rest of the component's lifetime
is spent servicing.

**Detailed description:**
The boot path is highly order-dependent and has several hard failure
checkpoints. First, `ReadConfig()` parses `gameconfig.txt` (if missing, a default
config is generated with `ConfigReady=1`). `ListenSocket.WSAInitialize()` must
succeed or the process returns `FALSE`. `Reboot()` then zeroes the three world
grids, loads `heightmap.dat`, initializes base definitions, message table, mob
names, NPC generators, and spawns the initial locked items listed in
`g_pInitItem` (locking any with an `EF_KEYID` ability value in `(0,15)`).

The server identity is resolved by reading `localip.txt` and matching it against
the compiled-in `g_pServerList[][MAX_SERVERNUMBER]` table. If no match is found
(`DBServerAddress` stays empty), a modal error box is shown and the process
exits (`return TRUE`). On success it sets `ServerGroup` and `ServerIndex`
(= `j-1`), and connects to DBSrv at the matched group's address on port 7514.
`TESTSERVER`/`LOCALSERVER` flags are set by comparing the DBSrv address to two
hard-coded IPs. The billing server is then optionally connected from
`biserver.txt`; a connect failure logs and disables `BILLING`.

Timers `TIMER_SEC` (500 ms) and `TIMER_MIN` (12 s) are installed, the listener
starts on `GAME_PORT` (8281) with `WSA_ACCEPT`, and the initial Kefra mob
population is generated if `KefraLive == 0`. Only then does the message pump run.

**Rule workflow:**
1. Read `gameconfig.txt` (or generate defaults).
2. `WSAInitialize()` the listen socket; abort on failure.
3. `Reboot()`: load heightmap, base defs, messages, NPC generators, spawn locked init items.
4. Read `localip.txt`; resolve `ServerGroup`/`ServerIndex` against `g_pServerList`.
5. Connect `DBServerSocket` to `g_pServerList[ServerGroup][0]:7514`; abort on failure.
6. Optionally connect billing from `biserver.txt`; disable `BILLING` on failure.
7. Install timers, start listening on 8281, spawn Kefra mobs.
8. Enter `GetMessage`/`DispatchMessage` loop.

---

### Business Rule: Connection Acceptance and Slot Reservation

**Overview:**
On an `FD_ACCEPT` event, `MainWndProc` (case `WSA_ACCEPT`, Server.cpp:4006)
assigns the new socket to a free `CUser` slot and applies capacity and server
state policies before the socket is allowed to proceed to login.

**Detailed description:**
`GetEmptyUser()` scans `pUser[1..MAX_USER)` for the first slot with
`Mode == USER_EMPTY`. Because users and mobs share the same numeric index space
(`pMob[i]` is the in-world entity for `pUser[i]`), slot 0 is never used and the
top `ADMIN_RESERV` (10) slots are reserved for administrators. If the accepted
user lands in a reserved slot, the client is immediately sent
`_NN_Reconnect` and closed. Separately, if `ServerDown != -1000` (a shutdown or
reboot is in progress), the connection is refused with
`_NN_ServerReboot_Cant_Connect` and closed. If no empty slot exists at all, an
"accept fail - no empty" message is logged and the connection is dropped.
`pUser[User].AcceptUser()` performs the actual `accept()` and initializes the
connection's session state (including `Mode` transitions that gate later login).

**Rule workflow:**
1. `WSAGETSELECTERROR(lParam)` non-zero → log, reject.
2. `GetEmptyUser()` returns 0 → log "accept fail - no empty", reject.
3. `pUser[User].AcceptUser(ListenSocket.Sock)`.
4. If `User >= MAX_USER - ADMIN_RESERV` → send `_NN_Reconnect`, close.
5. If `ServerDown != -1000` → send `_NN_ServerReboot_Cant_Connect`, close.

---

### Business Rule: Graceful Shutdown Countdown and Forced Save

**Overview:**
When an administrator triggers a reboot/shutdown, the server enters a 120-second
countdown (`ServerDown`) during which it broadcasts periodic notices, then
serializes a forced save of every connected user before terminating.

**Detailed description:**
`ProcessSecTimer` (ProcessSecMinTimer.cpp:40) drives the countdown. While
`ServerDown > -1000`, it increments each second; when `ServerDown % 20 == 1` it
broadcasts a notice message (indexed `ServerDown/20 + 17` into the message
string table) so players see the remaining time in ~20 s intervals. At
`ServerDown == 120` the timer is re-set to a 200 ms cadence for the final drain.
During the drain phase the server walks `UserCount` from 1 to `MAX_USER`,
closing each non-empty user via `CloseUser` (which triggers a `MSG_SavingQuit`
save to DBSrv). When all users are empty, billing is disconnected (if enabled),
the font object is freed, and `PostQuitMessage` ends the process. New connections
are refused throughout the countdown by the `ServerDown != -1000` check in the
accept handler.

**Rule workflow:**
1. `ServerDown` incremented every `ProcessSecTimer` tick.
2. Every 20 s (`%20==1`) broadcast a shutdown-notice message.
3. At `ServerDown==120`, speed timer to 200 ms and start the user drain.
4. For each `UserCount`, `CloseUser` → `MSG_SavingQuit` save to DBSrv.
5. When all users drained, disconnect billing, free font, `PostQuitMessage`.

---

### Business Rule: Inbound Packet Validation and Dispatch

**Overview:**
`ProcessClientMessage` (ProcessClientMessage.cpp:38) is the gate through which
every game packet passes before reaching its `Exec_MSG_*` handler. It enforces a
small set of structural and anti-spoof checks and then dispatches by `std->Type`
over 63 `case` branches to the 58 per-message handler files.

**Detailed description:**
The function casts the raw buffer to `MSG_STANDARD*` and first validates the
packet's `ID`: if `ID < 0 || ID >= MAX_USER`, the packet is logged
("err,packet Type:%d ID:%d...") and dropped. Next, while `ServerDown >= 120`
packets are ignored (shutdown drain). For a valid connection it refreshes
`pUser[conn].LastReceiveTime` (used by the idle policy). `_MSG_Ping` is consumed
and returns immediately without further processing (it serves as a keepalive).
The anti-spoof sentinel check discards client-originated packets whose
`ClientTick == SKIPCHECKTICK` (only server-originated frames, `isServer==TRUE`,
bypass it). Only then does the `switch(std->Type)` route to the matching
`Exec_MSG_*` handler. There is no default-handler fallback listed, so unknown
types are silently ignored.

**Rule workflow:**
1. Cast buffer to `MSG_STANDARD*`.
2. Reject if `ID` out of `[0, MAX_USER)`; log and return.
3. Return if `ServerDown >= 120`.
4. Update `LastReceiveTime` for valid conns.
5. Consume `_MSG_Ping` (keepalive) and return.
6. Reject client frames with `ClientTick == SKIPCHECKTICK`.
7. `switch(Type)` → dispatch to `Exec_MSG_*` handler.

---

### Business Rule: Database Connection Resilience

**Overview:**
The TMSrv↔DBSrv link is the persistence lifeline. `MainWndProc` (case
`WSA_READDB`, Server.cpp:3812) handles read events on `DBServerSocket` and
contains explicit reconnection logic for when the link drops.

**Detailed description:**
When a non-`FD_READ` event or a failed `Receive()` occurs on the DBSrv socket,
the server closes it and attempts to reconnect up to 2 times, sleeping 200 ms
between attempts, using the same `DBServerAddress`/`DBServerPort` and the local
IP as the source address. If reconnection fails (`ret == 0`), the server logs
"reconnect DB fail." and calls `PostQuitMessage`, shutting the whole game server
down rather than running without persistence. Notably, even on a successful
reconnect the code still calls `PostQuitMessage` after a 200 ms sleep
(Server.cpp:3845-3847 and 3879-3880), so the current implementation effectively
terminates the process after a DBSrv disconnect rather than resuming service.
While connected, the handler drains queued messages with `ReadMessage`, runs
`BASE_CheckPacket` under `_PACKET_DEBUG`, and feeds each valid frame to
`ProcessDBMessage`.

**Rule workflow:**
1. Non-`FD_READ` or `Receive()==FALSE` → close `DBServerSocket`.
2. Retry `ConnectServer` up to 2 times, 200 ms apart.
3. On final failure → log, `PostQuitMessage` (shut down).
4. Otherwise drain `ReadMessage` frames → `ProcessDBMessage`.

---

### Business Rule: Idle Disconnection Policy

**Overview:**
`CheckIdle` (Server.cpp:4304) disconnects users who have not sent a packet for a
configurable threshold, preventing zombie connections from consuming slots.

**Detailed description:**
Each user's `LastReceiveTime` is set to the current `SecCounter` on every
inbound packet (ProcessClientMessage.cpp:57). `CheckIdle` normalizes the value
first: if `LastReceiveTime` is ahead of `SecCounter`, or more than 1440 s behind,
it resets it to the current counter (clamping out-of-band values). Then, if the
last-receive time is more than **720 seconds** (12 minutes) behind the current
counter, the user is logged and disconnected via `CloseUser`. `CheckIdle` is
called on a rotating basis: each second, the users for which `i % 32 == SecCounter % 32`
are checked (ProcessSecMinTimer.cpp:1181-1182), so every user is checked at least
once per 32 seconds.

**Rule workflow:**
1. Normalize `LastReceiveTime` if out of band (`> SecCounter` or `< SecCounter-1440`).
2. If `LastReceiveTime < SecCounter - 720` → log and `CloseUser`.
3. Rotation: check user `i` when `i % 32 == SecCounter % 32`.

---

### Business Rule: Periodic Character Auto-Save

**Overview:**
The server periodically persists player state to DBSrv to bound the amount of
progress lost on a crash, walking the user table in a round-robin fashion.

**Detailed description:**
Every 8 seconds (`SecCounter % 8 == 0`), `ProcessSecTimer` advances a persistent
`SaveCount` cursor. It scans forward from `SaveCount` and, on finding the next
user in `USER_PLAY` whose mob is alive, calls `SaveUser(SaveCount, 0)` and
advances the cursor by one. `SaveUser` (Server.cpp:7056) builds a
`MSG_DBSaveMob` packet containing the full `STRUCT_MOB`, the player's
`Cargo[128]`, `Coin`, `Slot`, `Donate`, `ShortSkill[16]`, `Affect`, and `extra`,
plus the account name and connection `ID`, and sends it to DBSrv. When
`SaveCount` reaches `MAX_USER` it wraps to 1. If billing is in "child pay" mode
(`BILLING == 2` and `Unk_2728 == 1`), a character at or above the `FREEEXP`
level who is playing during the restricted hours (`g_Hour <= 12 || g_Hour >= 19`)
is instead logged out with a `_NN_Child_Pay` notice rather than merely saved.

**Rule workflow:**
1. Every 8 s, advance `SaveCount` (wrap at `MAX_USER`).
2. Find next `USER_PLAY`/alive user; else advance and continue.
3. If child-pay billing condition matches → notice + `CharLogOut`.
4. Else `SaveUser(conn,0)` → send `MSG_DBSaveMob` to DBSrv.

---

### Business Rule: User Save on Logout and Session Teardown

**Overview:**
`CloseUser` (Server.cpp:6834), `CharLogOut` (Server.cpp:7085), and `SaveUser`
(Server.cpp:7056) define how player state is persisted and cleaned up when a
session ends, distinguishing between a mid-login abort (no save) and a full
character logout (saved).

**Detailed description:**
`CloseUser` first clears the user's grid occupancy, resets `Admin`, and, if the
user was billing-connected, notifies billing (`SendBilling(...,2,...)`). It then
closes the socket. If the session mode is `USER_ACCEPT` or empty, no save is
needed and the slot is reset. For users not in `USER_PLAY`/`USER_SAVING4QUIT`
(e.g., mid-login), a `_MSG_DBNoNeedSave` is sent to DBSrv and the mob is marked
empty. For users in `USER_PLAY`/`USER_SAVING4QUIT`, it removes the user from any
party (`RemoveParty`) and trade (`RemoveTrade` if the counterparty points back),
then builds a `MSG_SavingQuit` carrying the full mob, cargo, short-skill,
affects, extra, account name, coin, donate, and slot, sends it to DBSrv, sets the
slot to `USER_SAVING4QUIT`, and deletes the mob. It also guards against
out-of-range `conn` and `Slot` with `CrackLog` calls.
`CharLogOut` is the graceful "back to character select" path: it validates the
connection and `USER_PLAY` state, resolves the trade, sets `Mode = USER_SELCHAR`,
stores the current level into `SelChar.Score[Slot].Level`, calls `SaveUser(conn,1)`
(export flag set), `DeleteMob(conn,2)`, resets spawn coordinates, and confirms
with `_MSG_CNFCharacterLogout`.

**Rule workflow:**
1. `CloseUser`: clear grid, reset admin, notify billing, close socket.
2. Determine mode; no-save (`USER_ACCEPT`/empty) → reset slot.
3. Mid-login modes → `_MSG_DBNoNeedSave` to DBSrv, mark mob empty.
4. `USER_PLAY`/`USER_SAVING4QUIT` → `RemoveParty`, `RemoveTrade`, send `MSG_SavingQuit`, set `USER_SAVING4QUIT`, `DeleteMob(conn,2)`.
5. `CharLogOut`: validate, resolve trade, save via `SaveUser(conn,1)`, `DeleteMob(conn,2)`, confirm.

---

### Business Rule: Failed-Account Tracker

**Overview:**
`AddFailAccount`/`CheckFailAccount` (Server.cpp:1334-1361) implement a small
in-memory abuse counter used to detect repeated failed logins for an account,
with the tally periodically cleared.

**Detailed description:**
`AddFailAccount` stores up to 16 account names in the fixed
`FailAccount[16][16]` array, writing into the first empty slot. `CheckFailAccount`
counts how many times a given account name appears across the 16 slots. The
array is reset to zero every 10 minutes via `ProcessMinTimer`
(`MinCounter % 10 == 0`, ProcessSecMinTimer.cpp:2136-2137). The actual login
handler (`_MSG_AccountLogin`) uses this counter to gate/stall repeated failed
login attempts, and the windowed reset bounds how long a lockout lasts. Because
the table is small (16) and circular-in-time, it is a lightweight brute-force
deterrent rather than a durable ban mechanism.

**Rule workflow:**
1. On failed login → `AddFailAccount(account)` into first free slot (max 16).
2. `CheckFailAccount(account)` returns occurrence count.
3. Every 10 minutes → zero the whole `FailAccount` table.

---

### Business Rule: World Simulation Cadence

**Overview:**
`ProcessSecTimer` and `ProcessMinTimer` (ProcessSecMinTimer.cpp) drive the
cooperative world simulation on the single UI thread, scheduling each subsystem
on a fixed modulo of a global `SecCounter`/`MinCounter`.

**Detailed description:**
The per-second loop increments `SecCounter` and stamps `CurrentTime`. Item decay
runs every 2 s (`%2==0`). Ranking (`ProcessRanking`), the water-scroll clear/teleport
timers, and the secret-room ("Carta") timer run every 4 s. Every 16 s
(`i % 16 == Sec16`) each live playing user gets `RegenMob(i)` (HP/MP regeneration)
and `ProcessAffect(i)` (status-affect tick); every 32 s idle checks run as
described above. Mob AI combat is driven through `MobAttack` in the per-mob
processing sections. The per-minute loop rolls the daily logs when the day
changes, rotates the newbie-event server designation, computes the castle-server
designation from week number/parity, persists the guild "imposto" (tax) NPC
state to `./npc/` files, drives `CCastleZakum::ProcessMinTimer`, advances the
kingdom-clear state machine, clears the failed-account table every 10 minutes,
randomly changes the weather, and calls `GuildProcess`. Because everything runs
on one thread, a long subsystem blocks the whole world.

**Rule workflow (per second):**
1. Handle shutdown countdown / billing reconnect.
2. Auto-save one user every 8 s.
3. Item decay every 2 s; ranking + water/carta timers every 4 s.
4. Idle check every 32 s; regen + affect every 16 s per user.
5. Mob AI (`MobAttack`) per tick.

**Rule workflow (per minute):**
1. Roll daily logs on day change.
2. Rotate newbie/castle server designations.
3. Persist guild tax NPCs; castle/war timers.
4. Clear failed-account table every 10 min.
5. Weather change; `GuildProcess`.

---

### Business Rule: Weather System

**Overview:**
The server changes the world weather probabilistically each minute, or honors a
forced weather override, and notifies all clients via `SendWeather`.

**Detailed description:**
In `ProcessMinTimer`, a pseudo-random value `rand() % 1200` is drawn. If
`ForceWeather == -1` (no override), weather transitions to state 0 (clear) with
probability 260/1200 (~21.7%), state 1 with probability 20/1200 (~1.7%, the
`30..50` band), and state 2 with probability 5/1200 (~0.4%, the `55..60` band),
each only if the current weather differs; otherwise nothing changes. If
`ForceWeather != -1`, the weather is forced to that value and `SendWeather()` is
called whenever it differs from `CurrentWeather`. The state machine is
stateless between minutes and there is no weather-dependent game mechanic
branching here beyond the client notification.

**Rule workflow:**
1. If `ForceWeather == -1`: draw `rndWeather = rand() % 1200`.
2. Map ranges to weather 0/1/2; if changed → `SendWeather()`.
3. Else if `ForceWeather != CurrentWeather` → set and `SendWeather()`.

---

### Business Rule: Configuration File Validation (gameconfig.txt)

**Overview:**
`ReadConfig` (Server.cpp:761) parses the server's primary configuration file with
strict, position-sensitive validation; any deviation from the expected format is
treated as fatal (a modal error box and an aborted load).

**Detailed description:**
The file must begin with a series of exact header lines ("Drop Item Event
Settings:", "Etc Event Settings:", "Billing Settings:", "Item Drop Bonus
Settings:", "Treasure Settings:", "Etc Settings:"). Each block's parameter names
(e.g., `evindex evdelete evon evitem evrate evstart`, `double deadpoint
dungeonevent statsapphire`, `billmode freeexp`) are verified with `strcmp`; a
mismatch or a sentinel `-1` value left after `sscanf` triggers a modal
"not game-server generated gameconfig.txt" error and the file is closed without
applying the settings. It loads event config (drop event index/rate/items),
etc-event flags (`DOUBLEMODE`, `DUNGEONEVENT`, `DEADPOINT`, `StatSapphire`,
`BRItem`), billing settings (`BILLING`, `FREEEXP`, `CHARSELBILL`,
`POTIONCOUNT`, `PARTYBONUS`, `GUILDBOARD`), the 64-entry `g_pDropBonus` table
(two 16-value lines at lines 936-1014, each value clamped to `[0,9999]`), the 8
treasure recipes (`g_pTreasure`), and misc settings (`PARTY_DIF`, `KefraLive`,
`GTorreHour`, `RvRHour`, `isDropItem`, `BRHour`, `maxNightmare`,
`PotionDelay`). `PARTYBONUS` is clamped to `[50,200]` (default 100) if out of
range.

**Rule workflow:**
1. Open `gameconfig.txt`; if missing → generate defaults, `ConfigReady=1`.
2. Validate each header line exactly; mismatch → modal error, abort.
3. Validate parameter name tokens per block; mismatch or `-1` value → abort.
4. Clamp `PARTYBONUS` to `[50,200]`; clamp drop-bonus values to `[0,9999]`.
5. Populate globals and the `g_pTreasure`/`g_pDropBonus` tables.

---

### Business Rule: World Object Spawning and Regeneration

**Overview:**
The component owns the spawn primitives `CreateMob`/`GenerateMob`/`CreateItem`/
`GenerateSummon` and the periodic `RegenMob`, which instantiate the world's mobs,
items, summons, and boss populations and replenish their HP/MP over time.

**Detailed description:**
`CreateMob` (Server.cpp:2865) instantiates a mob by name into a grid position
from a data folder; `GenerateMob` (Server.cpp:2995) binds a mob generator to its
region. `GenerateSummon` (Server.cpp:2572) materializes a summon from a `SummonID`
with an item and count. `CreateItem` (Server.cpp:7254) drops/creates an in-world
`CItem`. `RegenMob` (Server.cpp:4362) is invoked every 16 s for each live playing
user and restores HP/MP per the regeneration rules, subject to the mob being
alive. Initial boss population is generated at boot (Kefra mobs) when
`KefraLive == 0`. Mob AI is advanced by `MobAttack` (Server.cpp:8927), which is
driven from `ProcessSecTimer`'s per-mob loop.

**Rule workflow:**
1. Boot: if `KefraLive==0`, `GenerateMob` each Kefra mob + boss.
2. Per 16 s: for live playing users, `RegenMob(i)` + `ProcessAffect(i)`.
3. Per tick: `MobAttack` advances mob AI toward targets.
4. Handlers use `CreateMob`/`GenerateSummon`/`CreateItem` to spawn on demand.

---

### Business Rule: Guild, War, and Castle Event State

**Overview:**
`GuildProcess` (Server.cpp:6203), `DoWar`/`DoAlly`/`DoDeprivate`, the coloseum/
arena/castle door helpers (`SetColoseumDoor`, `SetArenaDoor`, `SetCastleDoor`),
`DecideWinner` (Server.cpp:6108), and `CCastleZakum::Process*Timer` govern the
periodic guild-war, alliance, castle-siege, and arena-event state transitions
driven by wall-clock time.

**Detailed description:**
The per-minute `GuildProcess` reconciles guild-war (`g_pGuildWar`), alliance
(`g_pGuildAlly`), and guild-zone ownership/tax state against the configured
`GuildDay`/`GuildHour` and `WeekMode`. `DecideWinner` resolves a concluded
arena/castle battle. Door helpers toggle collision/teleport gates for the
coloseum, arena, and castle. `Reboot` computes the initial `WeekMode` from the
current day-of-week vs. the configured `GuildDay`/`GuildHour`, and `ProcessMinTimer`
computes which server hosts the castle event (`CastleServer`) using week-number
parity and `ServerIndex`. The guild tax NPCs are persisted to `./npc/<MobName>`
files each minute when alive. `FinishCastleWar` reopens the castle door and
clears the defending guild's area.

**Rule workflow:**
1. `Reboot`: derive `WeekMode` from `GuildDay`/`GuildHour`/`tm_wday`.
2. `ProcessMinTimer`: compute `CastleServer`; persist alive guild-tax NPCs to `./npc/`.
3. `GuildProcess` (per min): reconcile war/ally/zone/tax state.
4. `DecideWinner`/`FinishCastleWar` on battle conclusion.

---

## 4. Component Structure

The Server (TMSrv) component's primary boundary is `Server.cpp`/`Server.h`. It
directly owns and invokes the dispatchers and the timer-driven world simulation
that are declared in `Server.h` and compiled into the same TMSrv process.

```
legacy/Code/TMSrv/
├── Server.cpp                    # Component core (9,449 lines): WinMain,
│                                 #   MainWndProc, socket I/O, connection
│                                 #   lifecycle, core game logic, billing
├── Server.h                      # Component contract (418 lines): function
│                                 #   prototypes + all extern world globals
├── ProcessSecMinTimer.cpp        # ProcessSecTimer/ProcessMinTimer world
│                                 #   simulation drivers (declared in Server.h)
├── ProcessClientMessage.cpp/.h   # Inbound client packet dispatcher (called
│                                 #   from Server.cpp WSA_READ handler)
├── ProcessDBMessage.cpp/.h       # DBSrv response dispatcher (called from
│                                 #   Server.cpp WSA_READDB handler)
├── SendFunc.cpp/.h               # Outbound packet builders (called by handlers)
├── GetFunc.cpp/.h                # Shared game-logic helpers (called by handlers)
├── CUser.cpp/.h                  # Per-player session state (pUser[] elements)
├── CMob.cpp/.h                   # In-world mob/player entity (pMob[] elements)
├── CItem.cpp/.h                  # In-world dropped item (pItem[] elements)
├── CNPCGene.cpp/.h               # NPC/mob generators & summons
├── MobKilled.cpp                 # Combat/kill resolution
├── imple.cpp                     # In-game command/administrator handler
├── CReadFiles.cpp/.h             # Config/data file loading
├── CCastleZakum.cpp/.h           # Castle (Zakum) siege event logic
├── CWarTower.cpp/.h              # Guild war tower logic
├── Language.h                    # Localized message-string table
└── _MSG_*.cpp (58 files)         # Per-message-type handlers (dispatched via
                                  #   ProcessClientMessage)
```

Note: `ProcessSecMinTimer.cpp`, `ProcessClientMessage.cpp`, and
`ProcessDBMessage.cpp` are separate translation units but are functionally part
of the Server component's execution path (their entry points are declared in
`Server.h` and called from `Server.cpp`). The 58 `_MSG_*` handler files and the
`SendFunc`/`GetFunc` helpers are consumers of the component's exported globals
and functions.

---

## 5. Dependency Analysis

**Internal Dependencies (within the TMSrv process):**

```
Server.cpp (WinMain/MainWndProc)
  ├─► ProcessClientMessage ─► Exec_MSG_* (58 handlers) ─► SendFunc / GetFunc
  ├─► ProcessDBMessage ─► DBSrv response handlers
  ├─► ProcessSecTimer / ProcessMinTimer (ProcessSecMinTimer.cpp)
  ├─► CUser / CMob / CItem / CNPCGene (world arrays & entities)
  ├─► CReadFiles (config/data loading)
  ├─► CCastleZakum / CWarTower (event systems)
  └─► Basedef (packet structs, domain models, constants)
```

**External Dependencies:**

| Dependency | Type | Purpose | Notes |
|------------|------|---------|-------|
| DBSrv (TCP :7514) | Inter-process service | Account/character persistence, login flow | Custom binary protocol from `Basedef.h` |
| BillServer (TCP) | External service (optional) | Billing/authentication | Enabled by `BILLING`; graceful fallback to 0 |
| Game Client (TCP :8281) | External client | All inbound gameplay traffic | Custom binary protocol |
| Winsock 2 (`ws2_32.lib`) | OS library | Async socket I/O (`WSAAsyncSelect`) | Linked via `TMSrv.vcxproj` |
| Win32 / WinMM (`winmm.lib`) | OS libraries | GUI message pump, timers | `kernel32`, `user32`, `gdi32`, etc. |
| ODBC (`odbc32.lib`/`odbccp32.lib`) | OS libraries | Linked but not used in this component's game path | Present in link deps |
| Filesystem (local files) | Local storage | `gameconfig.txt`, `localip.txt`, `biserver.txt`, `heightmap.dat`, `ServerList.txt`, `MobName.txt`, `Language.txt`, `./npc/*`, log files | Read/write at boot and periodically |

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the file/component level for this C++ codebase
(`Server.cpp`/`Server.h` are the component). Afferent coupling (Ca) counts
distinct consumers that depend on the component's exported functions/globals;
efferent coupling (Ce) counts distinct components the component calls/uses.
These are relative estimates derived from call-site and declaration analysis.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|-------------------|
| Server (Server.h globals) | Very High (every handler, SendFunc, GetFunc, timer reads `pUser`/`pMob`/`pItem`/grid globals) | Low (globals only) | High |
| Server.cpp (functions) | High (`_MSG_*` handlers, timers call `SaveUser`, `CloseUser`, `CreateMob`, `CreateItem`, `DoRecall`, etc.) | Very High (calls ProcessClientMessage, ProcessDBMessage, timers, SendFunc, GetFunc, CReadFiles, CCastleZakum, CWarTower, CUser/CMob/CItem, Basedef) | Critical |
| ProcessSecMinTimer | Low (from Server `WM_TIMER`) | High (RegenMob, ProcessAffect, MobAttack, SaveUser, CheckIdle, CCastleZakum, GuildProcess, SendFunc) | High |
| ProcessClientMessage | Medium (from Server `WSA_READ`) | High (58 handlers, SendFunc, GetFunc, Log) | High |
| ProcessDBMessage | Medium (from Server `WSA_READDB`) | High (per-response handlers, SendFunc) | High |

The component is both the most-consumed provider of shared mutable state and the
most-connected caller in the system, making it the central hub (and a
single-point-of-failure) of the TMSrv process.

---

## 7. Endpoints

The Server (TMSrv) component does **not** expose REST/GraphQL/gRPC endpoints. It
is a Win32 application exposing a **custom binary socket protocol** on a single
TCP listening port, plus outbound TCP client connections to DBSrv and the billing
server. The endpoint surface is therefore summarized in table form:

| Endpoint / Socket | Direction | Description |
|-------------------|-----------|-------------|
| TCP `GAME_PORT` (8281) | Listen (inbound from game client) | Accepts game clients; all gameplay packets arrive here |
| `DBServerSocket` → DBSrv `DB_PORT` (7514) | Outbound connect | Persistence/login requests and responses to/from DBSrv |
| `BillServerSocket` → BillServer (port from `biserver.txt`) | Outbound connect (optional) | Billing/authentication traffic when `BILLING > 0` |

The wire protocol is defined in `legacy/Code/Basedef.h`; every message begins
with a `MSG_STANDARD` header (`Type`, `Size`, `ID`, `KeyWord`, `ClientTick`) and
is dispatched by `Type`.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Game Client | External client (TCP) | All gameplay traffic | TCP / custom binary (`Basedef.h`) | Packed `STRUCT_*` frames | Frame errors → `CloseUser`; idle timeout 720 s; `_PACKET_DEBUG` size checks |
| DBSrv | Inter-process (TCP) | Persistence & login | TCP / custom binary (`Basedef.h`) | `MSG_DBSaveMob`, `MSG_SavingQuit`, `MSG_DBNoNeedSave`, etc. | Reconnect retry x2; on failure `PostQuitMessage` (server terminates) |
| BillServer | External service (TCP, optional) | Billing/authentication | TCP / `_AUTH_GAME` frames | `SendBilling2`, `ReadBillMessage` | 360 s reconnect counter; disable `BILLING` on repeated failure |
| Local filesystem | Local storage | Config, heightmap, mob names, messages, guild tax NPCs | Binary/text file I/O | `gameconfig.txt`, `localip.txt`, `biserver.txt`, `heightmap.dat`, `MobName.txt`, `Language.txt`, `./npc/*` | Boot aborts with modal box if required files missing |
| Logging | Local files | Operational/chat/item logs | Text file (`StartLog`, `Log`, `ChatLog`, `ItemLog`) | Formatted text; daily rotation | None (best-effort) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Single-Threaded Event Loop (message pump) | `WinMain` `GetMessage`/`DispatchMessage` + `WSAAsyncSelect` socket events as window messages | Server.cpp:3459-3656, 3718-4155 | Serializes all I/O and simulation on one thread |
| Command/Message Dispatcher | `switch(std->Type)` in `ProcessClientMessage` → `Exec_MSG_*` handlers | ProcessClientMessage.cpp:66-313 | Routes each packet type to its dedicated handler |
| Timer-Driven State Machine | `SetTimer(TIMER_SEC/TIMER_MIN)` → `ProcessSecTimer`/`ProcessMinTimer` | Server.cpp:3631-3632; ProcessSecMinTimer.cpp | Cooperative world simulation on fixed cadence |
| Shared-Header Protocol Contract | `Basedef.h` compiled into TMSrv and DBSrv | Server.cpp:27; `Basedef.h` | Source-level wire-format and domain-model coupling |
| Global State / Singleton-by-convention | `extern` world arrays (`pUser`, `pMob`, `pItem`, grids) in `Server.h` | Server.h:229-311 | Shared mutable world state accessed by all handlers |
| Entity-Component Object Model | `CUser` (session) + `CMob` (in-world) sharing an index | CUser.h, CMob.h | Splits connection state from world entity state |
| Graceful Shutdown | `ServerDown` countdown + forced save drain | ProcessSecMinTimer.cpp:42-119 | Ordered shutdown with data persistence |
| Resilience/Reconnect (DB, Billing) | Reconnect loops and disconnect counters | Server.cpp:3812-3881; ProcessSecMinTimer.cpp:130-167 | Bounded retry on external service loss |

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| Critical | Server.cpp monolith | Single 9,449-line translation unit fuses I/O, lifecycle, and game logic | Maintainability, review burden, large error blast radius |
| Critical | Single-threaded loop | No parallelism; all sockets + world simulation serial on UI thread | A long handler blocks all players and I/O; limited to `MAX_USER` (1000) |
| Critical | DBSrv disconnect → shutdown | `WSA_READDB` calls `PostQuitMessage` even after a successful reconnect | Entire game server terminates on a transient DB blip; availability risk |
| High | Raw packet casting | Buffers cast to `STRUCT_*` with minimal runtime validation in Release (`BASE_CheckPacket` only under `_PACKET_DEBUG`) | Malformed/oversized packets can cause overreads/overwrites |
| High | Plaintext credentials | Passwords/account data carried in cleartext binary frames | Credential exposure over the network |
| High | Global mutable state | Dozens of `extern` globals in `Server.h` mutated everywhere | Concurrency hazards (if ever threaded), hidden coupling |
| Medium | Hard-coded coordinates | Water-scroll, Carta, Pista, coloseum, castle coordinates hard-coded | Rigid event/teleport layout; changes require recompile |
| Medium | Dead/unreachable branches | `IsImple` returns 0; `ProcessBILLMessage` and `WriteArmor` are empty; `return TRUE` from `SendBilling`; duplicated `Log` calls | Confusing dead code and unclear intent |
| Medium | Billing fallback inconsistency | `SendBilling` is a stub while `SendBilling2` performs real I/O | Billing behavior differs depending on which call path is used |
| Medium | Missing config validation depth | `gameconfig.txt` header/name validation aborts silently without defaults when format drifts | Operational fragility; server may run with stale/partial config |
| Medium | No automated tests | 0 test files in the analyzed scope | Regressions undetectable; correctness only by inspection |
| Low | Menu-driven admin | `WM_COMMAND` reload/reboot/save actions are UI-menu only | Limited remote/automated operational control |

---

## 11. Test Coverage Analysis

An exhaustive search of the analyzed scope (excluding `.git`, `.opencode`,
`.codegraph`) found **no test files** — no unit, integration, or fixture files of
any kind exist for the Server (TMSrv) component or anywhere else in the W2PP
codebase. CodeGraph's blast-radius analysis likewise reports "no covering tests"
for the component's core handlers (e.g., `AcceptUser`, `Exec_MSG_AcceptParty`).

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| Server (TMSrv) core (`Server.cpp`, timers) | 0 | 0 | 0% (no automated coverage) | N/A — no tests exist |
| ProcessClientMessage / ProcessDBMessage dispatchers | 0 | 0 | 0% | N/A — no tests exist |
| `_MSG_*` handlers (58) | 0 | 0 | 0% | N/A — no tests exist |

There is no test project, test runner, or CI configuration in the repository.
This is a material risk: the component's 63-way dispatch, 9,449-line core file,
and cooperative timer state machines have zero automated regression protection.

---

## Limitations

- Coupling metrics are **relative estimates** derived from call-site and
  declaration analysis, not an exhaustive static-coupling enumeration.
- Several functions (e.g., the full body of `ProcessSecTimer` between lines
  200-2019, `ProcessAffect` at 4513-5437, `GuildProcess` at 6203-6654) contain
  additional per-second world and event logic that was summarized at the rule
  level rather than line-by-line; the primary business rules governing the
  component's core responsibilities (boot, I/O, connection lifecycle,
  persistence, idle/abuse, cadence, config, world/event state) are fully
  documented above.
- `ProcessBILLMessage`, `IsImple`, `WriteArmor`, and `BuildList` are empty/stub
  implementations; their intended behavior is undocumented in code and was not
  assumed.
- The analysis is read-only and did not modify any project files; no refactoring
  or implementation recommendations are provided per scope.
