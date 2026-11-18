# Component Deep Analysis Report

**Component:** Client Message Dispatcher
**Primary location:** `Code/TMSrv/ProcessClientMessage.cpp`
**Contract header:** `Code/TMSrv/ProcessClientMessage.h`
**Analyzed artifact:** TMSrv (W2PP game/world server, Win32 x86, MSVC v143, MultiByte charset, C/C++ decompiled codebase)
**Project scope:** entire project root (`E:\wyd\test-setup\w2pp`)
**Excluded from analysis:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis date:** 2026-11-15 22:07:28
**Analysis mode:** static, read-only (no files modified, no code executed, no tests run)

---

## 1. Executive Summary

### 1.1 Purpose

The Client Message Dispatcher is a single free function, `ProcessClientMessage(int conn, char *pMsg, BOOL isServer)`, defined at `Code/TMSrv/ProcessClientMessage.cpp:38` and declared at `Code/TMSrv/ProcessClientMessage.h:70`. It is the **only inbound entry point from the game client into the TMSrv gameplay layer**. Every byte that arrives over the client TCP port (`GAME_PORT` = 8281, `Code/Basedef.h:44`) is decoded by the socket layer, framed, de-obfuscated and checksum-verified, and then handed to this function, which decides whether the packet is legitimate and which of 58 `Exec_MSG_*` command handlers receives it.

The entire component body is 313 lines (`Code/TMSrv/ProcessClientMessage.cpp`) containing exactly one function: a linear guard chain (lines 40-64) followed by a 64-label `switch` on the packet `Type` field (lines 66-311) and an unconditional `return` (lines 311-312). There are no classes, no data structures, no state owned by the component, and no configuration read by it.

### 1.2 Role in the system

Architecturally the component is an **Inbound Adapter / Front Controller / Message Router** sitting between the transport layer (`Code/CPSock.cpp`) and the command-handler family (`Code/TMSrv/_MSG_*.cpp`, 58 files). Its role is deliberately narrow:

- It is the boundary where the *transport identity* (`conn`, the socket owner index resolved by `GetUserFromSocket`, `Code/TMSrv/Server.cpp:6729-6741`) is bound to a *gameplay identity* (`pUser[conn]`, `pMob[conn]`). All 58 handlers receive `conn`, never a packet-provided identity.
- It enforces the two dispatcher-level admission rules: `ID` range and global shutdown state.
- It performs the only dispatcher-level anti-tamper check in the whole inbound path, the `SKIPCHECKTICK` sentinel rejection (line 63).
- It performs the only dispatcher-level anti-cheat action in the whole inbound path, the hard block and crack-point penalty for client-sent `_MSG_UpdateScore` (lines 106-110).
- It suppresses keep-alive traffic (`_MSG_Ping`, line 59) so no handler has to know about it.

### 1.3 Key findings

1. **The dispatcher is a pure, stateless router with fail-closed-on-unlisted-type but fail-open-on-payload semantics.** It validates the 12-byte header only (`MSG_STANDARD`, `Code/Basedef.h:943-946`). It never validates `Size` against the expected `MSG_*` struct of the routed type, and it casts the raw `char*` to `MSG_STANDARD*` without any length check (`Code/TMSrv/ProcessClientMessage.cpp:40`). The only structural size-check helper in the codebase, `BASE_CheckPacket` (`Code/Basedef.cpp:6475`), is **entirely commented out and returns `FALSE` unconditionally** (`Code/Basedef.cpp:6476` opens the comment block, `Code/Basedef.cpp:6594-6595` closes it and returns `FALSE`), so even the `_PACKET_DEBUG` diagnostic path cannot detect malformed sizes.

2. **The `ID` field is validated but never used for dispatch.** The gate at lines 42-51 rejects `std->ID < 0 || std->ID >= MAX_USER`, yet the dispatch identity is `conn` (line 38, passed unchanged to every handler). This makes the `ID` check a *pure wire-hygiene filter* (it rejects mob-range and out-of-range spoofed IDs), not an authorization mechanism. Handlers that do use the in-packet identity read it from their own payload structs and re-validate it (for example `_MSG_Trade.cpp:32-40` validates `OpponentID`, `_MSG_Buy.cpp:49-53` validates `TargetID`).

3. **A deliberate, undocumented "server-originated packet" channel exists.** The `isServer` parameter lets TMSrv feed synthetic packets back through the same dispatcher. There are 4 call sites in `Code/TMSrv/Server.cpp` (lines 4622, 5062, 5371, 7464), all passing `isServer = TRUE`, used to inject mob attack results and automatic party acceptance. The sentinel `ClientTick = 0xE0A1ACA` used at `Code/TMSrv/Server.cpp:4600` is numerically identical to `SKIPCHECKTICK` (`235543242`, `Code/Basedef.h:172`), which is exactly the value that rule (line 63) rejects for client-originated packets and that `Exec_MSG_Attack` rewrites to `CurrentTime` at `Code/TMSrv/_MSG_Attack.cpp:1730-1731`. The sentinel therefore functions as a shared trust token between the dispatcher, the AI code and the combat handler.

4. **Routing surface: 64 distinct type constants, 59 case groups, 58 handler functions, 1 intentionally blocked type, 0 default case.** The dispatcher recognises 64 `_MSG_*` constants (measured by label count: 64 `case _MSG_*` labels), maps them onto 59 distinct `case` groups (three groups are multi-label), invokes 58 unique `Exec_MSG_*` functions, blocks `_MSG_UpdateScore` without invoking a handler, and provides **no `default:` label** - a `grep` for `default:` in the file returns zero matches. Any unrecognised or wrong-flag `Type` is silently discarded without logging (the `switch` falls to `return` at line 312).

5. **Guard ordering is significant and observable.** The sequence is: `ID` range (log on failure) -> `ServerDown` (silent) -> liveness refresh (silent) -> `_MSG_Ping` drop (silent) -> `ClientTick` sentinel reject (silent) -> dispatch. Two consequences follow. First, an out-of-range `ID` produces a log line even while the server is shutting down, because the `ID` check precedes the shutdown gate. Second, the idle-timer refresh happens **after** the shutdown gate but **before** all type filtering, so keep-alive and unknown types keep a session alive (they refresh `LastReceiveTime`), whereas during a shutdown countdown they do not, because the gate returns first.

6. **The component is highly afferent and moderately efferent, and it is the load-bearing hub of the server.** 66 translation units in `Code/` include `ProcessClientMessage.h` (measured: 66 files), and every one of the 58 handlers depends on the dispatcher contract. The component itself depends on 9 project headers plus 2 TMSrv functions (`Log`, `AddCrackError`), matching the architecture report's ~69 afferent / ~14 efferent estimate.

7. **There is no test of any kind for this component, and none anywhere in the repository.** See section 11 for the exhaustive evidence. The component is the most security-sensitive branch in the server (it is the only gate before 58 handlers that trust raw pointers) and it is verified only by manual play and by production log inspection.

### 1.4 Confidence statement

All statements in this report are derived from directly read source. Two areas carry marked uncertainty: (a) the exact runtime semantics of the shutdown countdown timing, because `ProcessSecTimer` is bound to a 500 ms Win32 timer (`Code/TMSrv/Server.cpp:3631`) while its counter is named and reasoned about in seconds; and (b) the wire-level meaning of the `KeyWord`/`CheckSum` pair, which is handled by `CPSock::ReadMessage` (`Code/CPSock.cpp:353-467`) via a lookup table (`pKeyWord`) whose generation is outside this component's boundary.

---

## 2. Data Flow Analysis

### 2.1 End-to-end inbound path (client-originated)

```
1. Winsock async notification WSA_READ arrives at MainWndProc
   (Code/TMSrv/Server.cpp:3919, WSA_READ = WM_USER-based constant, Code/CPSock.h)
2. Sender socket mapped to a session slot:
   int User = GetUserFromSocket(wParam)            Code/TMSrv/Server.cpp:3921
   (returns 0 if socket unknown -> closesocket + break, lines 3922-3927)
3. Event filter: WSAGETSELECTEVENT(lParam) must be FD_READ
   otherwise: log "clo,fd %d-%d", CloseUser(User)      Code/TMSrv/Server.cpp:3929-3936
4. Transport read: pUser[User].cSock.Receive()      Code/TMSrv/Server.cpp:3938
   (recv into pRecvBuffer at nRecvPosition; failure retried once, then CloseUser)
5. Framing loop: while (1) Msg = cSock.ReadMessage(&Error, &ErrorCode)
   Code/TMSrv/Server.cpp:3973-4002  ->  Code/CPSock.cpp:353
   5a. Init handshake: first 4 bytes must equal INITCODE 0x1F11F311
       else ErrorCode = 2                                Code/CPSock.cpp:366-383
   5b. Header completeness: nRecvPosition - nProcPosition < sizeof(HEADER) -> wait
                                                          Code/CPSock.cpp:386-387
   5c. Size bounds: Size > 8192 (MAX_MESSAGE_SIZE) or Size < 12 -> reset buffers,
       ErrorCode = 2                                     Code/CPSock.cpp:397-406
   5d. Frame completeness: Size > bytes available -> wait  Code/CPSock.cpp:408-411
   5e. De-obfuscation of bytes 4..Size-1 with pKeyWord[] and running checksum
       accumulation (Sum1/Sum2)                          Code/CPSock.cpp:424-455
   5f. Checksum mismatch -> ErrorCode = 1 but the packet is STILL returned
                                                          Code/CPSock.cpp:457-464
6. Error gate: if (Error == 1 || Error == 2) { log "err,read (%d),%d"; break; }
                                                          Code/TMSrv/Server.cpp:3990-3999
7. Dispatch: ProcessClientMessage(User, Msg, FALSE)      Code/TMSrv/Server.cpp:4001
8. Dispatcher guard 1 - ID range:
   std->ID < 0 || std->ID >= MAX_USER (1000) -> sprintf + Log("err,packet ...") + return
                                                          Code/TMSrv/ProcessClientMessage.cpp:42-51
9. Dispatcher guard 2 - shutdown gate:
   ServerDown >= 120 -> return (silent)                   ProcessClientMessage.cpp:53-54
10. Dispatcher side effect - liveness:
    if (conn > 0 && conn < MAX_USER) pUser[conn].LastReceiveTime = SecCounter
                                                          ProcessClientMessage.cpp:56-57
11. Dispatcher guard 3 - keep-alive:
    Type == _MSG_Ping (0x03A0) -> return (silent)         ProcessClientMessage.cpp:59-60
12. Dispatcher guard 4 - client tick sentinel:
    isServer == FALSE && ClientTick == SKIPCHECKTICK (235543242) -> return (silent)
                                                          ProcessClientMessage.cpp:62-64
13. Dispatcher routing: switch (std->Type) over 64 constants / 59 case groups
                                                          ProcessClientMessage.cpp:66-311
    13a. _MSG_UpdateScore (0x0336) -> special branch: Log + AddCrackError(conn, 2, 91)
                                                          ProcessClientMessage.cpp:106-110
    13b. all other matched types -> Exec_MSG_<Family>(conn, pMsg)
14. Unmatched types: switch falls out -> return           ProcessClientMessage.cpp:311-312
15. Handler execution: the handler re-casts pMsg to its own MSG_* struct,
    re-checks session state (pUser[conn].Mode / pMob[conn].MOB.CurrentScore.Hp),
    mutates world state, and emits responses via SendFunc / CPSock
    (e.g. Code/TMSrv/_MSG_Attack.cpp:21-96, _MSG_Buy.cpp:21-58)
```

### 2.2 Server-originated (self-injection) path

```
1. Mob AI decides to attack (CMob tick) inside Server.cpp combat loop
2. A MSG_AttackOne is synthesised on the stack:
   Type = _MSG_AttackOne, ID = idx, Size = sizeof(MSG_AttackOne),
   ClientTick = 0xE0A1ACA (== SKIPCHECKTICK), Motion = 254,
   SkillIndex 32/34/36, Dam[0].TargetID/Damage                    Server.cpp:4591-4620
3. ProcessClientMessage(idx, (char*)&sm, TRUE)                    Server.cpp:4622
4. Same guard 1 (ID range), same guard 2 (ServerDown), same liveness write
5. Guard 4 is BYPASSED because isServer == TRUE                    ProcessClientMessage.cpp:63
6. Same routing -> Exec_MSG_Attack(conn, pMsg)
7. Inside the handler, ClientTick == SKIPCHECKTICK triggers the "server generated"
   branches: rate limits skipped, m->ClientTick rewritten to CurrentTime
                                                    _MSG_Attack.cpp:59, 79, 88, 1730-1731
```

Additional injection sites: area/aura attack fan-out guarded by `idx < MAX_USER` (`Code/TMSrv/Server.cpp:5061-5062`, `5369-5371`) and automatic party acceptance with `sm.ID = 0` (`Code/TMSrv/Server.cpp:7451-7464`).

### 2.3 Data transformations applied to the packet

| Stage | Owner | Transformation | Location |
| --- | --- | --- | --- |
| Wire bytes -> frame | `CPSock::ReadMessage` | 4-byte `INITCODE` consumed once per connection (`Init` flag), then 12-byte header parsed | `Code/CPSock.cpp:366-395` |
| Obfuscated -> plaintext | `CPSock::ReadMessage` | Per-byte arithmetic keyed by `pKeyWord[iKeyWord*2 + 1]` with a 4-phase modulus on `i & 0x3` | `Code/CPSock.cpp:428-453` |
| Integrity | `CPSock::ReadMessage` | `CheckSum` == `Sum2 - Sum1`; mismatch sets `ErrorCode = 1` and only *logs*, packet is still returned | `Code/CPSock.cpp:455-464` |
| `char*` -> `MSG_STANDARD*` | Dispatcher | Unchecked reinterpret cast, no size assertion | `ProcessClientMessage.cpp:40` |
| `MSG_STANDARD*` -> `MSG_X*` | Handler | Unchecked cast to the type-specific struct, no size assertion | e.g. `_MSG_Attack.cpp:23` |
| Session slot -> world entity | Handler / `pUser`, `pMob` globals | `conn` indexes `pUser[conn]` (session) and `pMob[conn]` (player mob record); entity IDs `>= MAX_USER` are NPC/mob range | `Code/Basedef.h:56,107` |
| Response | `SendFunc.cpp` | `MSG_*` built, `cSock.AddMessage` / `SendOneMessage` / multicast | e.g. `Code/TMSrv/SendFunc.cpp:494-508` |

### 2.4 State touched by the dispatcher itself

| State | Type | Direction | Location |
| --- | --- | --- | --- |
| `pUser[conn].LastReceiveTime` | `int` (`CUser.h:97`) | write (`SecCounter`) | `ProcessClientMessage.cpp:57` |
| `ServerDown` | `int`, initial `-1000` (`Server.cpp:46`) | read | `ProcessClientMessage.cpp:53` |
| `SecCounter` | `unsigned int` global | read | `ProcessClientMessage.cpp:57` |
| `pUser[conn].NumError` | `int` (`CUser.h:75`) | indirect write via `AddCrackError` | `ProcessClientMessage.cpp:109` -> `Server.cpp:692` |
| `temp` | `char[4096]` global (`Server.cpp:277`) | write via `sprintf` | `ProcessClientMessage.cpp:46` |
| Log file handle `fLogFile`, window title | static / HWND | indirect write via `Log` | `ProcessClientMessage.cpp:48` -> `Server.cpp:6722-6725` |

The dispatcher owns **no** state of its own: no counters, no table, no per-connection flag. Everything it touches belongs to `CUser`, the global world arrays, or the logger. This is the cleanest cohesion property of the component.

---

## 3. Business Rules & Logic

## Overview of the business rules:

| ID | Rule Type | Rule Description | Location |
| --- | --- | --- | --- |
| BR-01 | Validation | Inbound packet `ID` must satisfy `0 <= ID < MAX_USER` (1000); violation is logged and the packet dropped | `Code/TMSrv/ProcessClientMessage.cpp:42-51` |
| BR-02 | Validation / Lifecycle | When the shutdown countdown reaches `ServerDown >= 120`, all inbound packets (client and server-originated) are dropped | `Code/TMSrv/ProcessClientMessage.cpp:53-54` |
| BR-03 | Session management | Every admitted packet refreshes `LastReceiveTime` for the owning connection slot, but only for `0 < conn < MAX_USER` | `Code/TMSrv/ProcessClientMessage.cpp:56-57` |
| BR-04 | Filtering | `_MSG_Ping` is consumed by the dispatcher and never routed to a handler (liveness already refreshed) | `Code/TMSrv/ProcessClientMessage.cpp:59-60` |
| BR-05 | Anti-tamper | A client-originated packet whose `ClientTick` equals the reserved sentinel `SKIPCHECKTICK` (235543242) is dropped | `Code/TMSrv/ProcessClientMessage.cpp:62-64` |
| BR-06 | Routing | `Type` is dispatched to exactly one of 58 `Exec_MSG_*` handlers across 64 recognised type constants and 59 case groups | `Code/TMSrv/ProcessClientMessage.cpp:66-311` |
| BR-07 | Routing | Three aliased families (`Action/Action2/Action3`, `Attack/AttackOne/AttackTwo`, `CombineItemOdin/Odin2`) share one handler each and are discriminated inside the handler | `ProcessClientMessage.cpp:96-100, 192-196, 266-269` |
| BR-08 | Anti-cheat | A client-sent `_MSG_UpdateScore` is never processed; it is logged and charged 2 crack points (type 91) | `Code/TMSrv/ProcessClientMessage.cpp:106-110` |
| BR-09 | Trust boundary | `isServer == TRUE` marks a TMSrv-generated packet: it bypasses BR-05 and is accepted for 4 internal injection sites | `ProcessClientMessage.cpp:38,63`; `Server.cpp:4622,5062,5371,7464` |
| BR-10 | Fail-closed routing | Any `Type` that matches no `case` is silently discarded; there is no `default:` label, no log, no crack accounting | `Code/TMSrv/ProcessClientMessage.cpp:66,311-312` |
| BR-11 | Delegation contract | The dispatcher performs zero payload validation (no `Size` check, no type-specific schema check, no session-state check); every handler must validate its own payload and state | `Code/TMSrv/ProcessClientMessage.cpp:40,66` |
| BR-12 | Identity | Dispatch identity is `conn` (the authenticated socket owner resolved at `Server.cpp:3921`), never the packet's `ID` field | `Code/TMSrv/ProcessClientMessage.cpp:38,69` |
| BR-13 | Namespacing | `Type` equality includes flag bits, so a message code without its expected flag (e.g. `13` instead of `13 | FLAG_CLIENT2GAME` = `0x020D`) matches no case and is dropped by BR-10 | `Code/Basedef.h:925-941` |

## Detailed breakdown of the business rules:

---

### Business Rule: BR-01 - Packet ID range gate

**Overview**:
Every inbound packet is rejected unless its header `ID` field lies in the player-session range `[0, MAX_USER)` where `MAX_USER` is 1000 (`Code/Basedef.h:56`). The check is at `Code/TMSrv/ProcessClientMessage.cpp:42`, the rejection logging at lines 46-48, and the drop at line 50.

**Detailed description**:
The `MSG_STANDARD` header (`Code/Basedef.h:925-930, 943-946`) declares `ID` as a signed `short`, immediately after `Type`. Because `ID` is a 16-bit signed integer, the `std->ID < 0` half of the condition rejects negative values and the `std->ID >= MAX_USER` half rejects 1000 and above. The upper bound is not arbitrary: `MAX_USER` is simultaneously the count of usable player sessions and the **first index of the mob/NPC range** (`Code/Basedef.h:56` comment: "Max users on CUser pUser and starting index of npcs and mobs"; `MAX_MOB` is 25000 at `Code/Basedef.h:107`). This means the gate rejects any packet that claims an identity belonging to the NPC/mob half of the shared `pMob[]` entity array, closing the most obvious identity-confusion vector in a design where player connections and world entities share one array (see architecture report note on "slot identity aliasing").

The check is a *wire-hygiene* filter, not an authorization control, because the dispatcher never uses `ID` for anything else: it passes `conn` to the handler and ignores `std->ID` completely (BR-12). The real protective value is the log line at line 46-48, which records `Type`, `ID`, `Size` and `KeyWord` of the offending packet into the server log and into the window title (`Log` at `Code/TMSrv/Server.cpp:6701-6727` writes to `fLogFile` when open and always calls `SetWindowText`). This is the only place in the inbound path where a malformed header's identifier fields are captured for forensics, which makes it the primary diagnostic instrument for client/protocol mismatch.

Two edge properties matter operationally. First, the check is performed *before* the shutdown gate (BR-02), so during a shutdown countdown the server still writes an `err,packet ...` line for every malformed packet, i.e. log noise continues while the drain runs. Second, the check does not consult the connection's own slot, so a legitimate client can send `ID = 0` while connected on slot 437 and still pass; the semantic meaning of `ID` is entirely delegated to each handler, several of which do enforce it themselves when the payload carries a real target identity (for example `_MSG_Trade.cpp:32-40` validates `OpponentID <= 0 || OpponentID >= MAX_USER`, `_MSG_Buy.cpp:49-53` validates `TargetID < MAX_USER`).

**Rule workflow**:
```
std = (MSG_STANDARD *)pMsg
IF std->ID < 0 OR std->ID >= 1000 THEN
    sprintf(temp, "err,packet Type:%d ID:%d Size:%d KeyWord:%d", m->Type, m->ID, m->Size, m->KeyWord)
    Log(temp, "-system", 0)          // log file + window title
    RETURN                            // packet dropped, no crack points, no client notice
END IF
CONTINUE to BR-02
```

---

### Business Rule: BR-02 - Global shutdown gate

**Overview**:
If `ServerDown >= 120` the dispatcher returns immediately for every packet, before any other action. Located at `Code/TMSrv/ProcessClientMessage.cpp:53-54`, reading the global `int ServerDown` declared `extern` at `Code/TMSrv/Server.h:198` and initialised to `-1000` at `Code/TMSrv/Server.cpp:46`.

**Detailed description**:
`ServerDown` is TMSrv's global shutdown state machine. It is not a boolean: its value space encodes four distinct phases. `-1000` means "running normally". Values `> -1000` and `< 0` mean "scheduled shutdown pending" (used for staggered multi-shard shutdowns, `Code/TMSrv/imple.cpp:655`). Values `1..119` are the countdown in progress, and `120` is the terminal phase. The transition into the terminal phase is orchestrated entirely outside this component by `ProcessSecTimer` (`Code/TMSrv/ProcessSecMinTimer.cpp:96-119`): it increments `ServerDown`, emits a notice every 20th step (`ServerDown % 20 == 1` -> `SendNotice(g_pMessageStringTable[messageId + 17])`), and when it reaches 120 it re-arms the Win32 timer to 200 ms (`SetTimer(hWndMain, TIMER_SEC, 200, 0)`, line 115) so the drain loop runs faster. From that moment the dispatcher's gate is permanently closed for the remainder of the process lifetime, and `ProcessSecTimer` starts closing one user per tick until `UserCount >= MAX_USER` and then calls `PostQuitMessage` (`Code/TMSrv/ProcessSecMinTimer.cpp:42-93`).

For the dispatcher this produces a very specific behaviour: **all gameplay input stops abruptly and silently**. There is no "server is closing" message sent from this gate; the notice text is emitted by the timer path. There is also no exception for the shutdown drain: even `_MSG_CharacterLogout` or `_MSG_Ping` will be dropped, because the gate precedes the `_MSG_Ping` filter. The drain therefore relies on socket closure (`CloseUser`) rather than on graceful per-session logout through the dispatcher. A second consequence is that the gate is checked **before** the liveness refresh (BR-03), so during the terminal phase `LastReceiveTime` stops advancing and the idle-timeout machinery (`CheckIdle`, `Code/TMSrv/Server.cpp:4304-4322`, threshold 720 ticks) becomes irrelevant.

The gate is also asymmetric with connect-time behaviour: `WSA_ACCEPT` refuses new connections whenever `ServerDown != -1000` (`Code/TMSrv/Server.cpp:4032-4039`, responding with `_NN_ServerReboot_Cant_Connect`), i.e. from the very first countdown step, long before `120`. So the operator-visible shutdown window is `ServerDown != -1000` for connection admission, but `ServerDown >= 120` for packet admission. During `1..119` players keep playing while new logins are refused; at `120` play stops entirely.

**Rule workflow**:
```
IF ServerDown >= 120 THEN
    RETURN                     // silent drop, applies to isServer == TRUE too
END IF
CONTINUE to BR-03
```

---

### Business Rule: BR-03 - Session liveness refresh

**Overview**:
For each admitted packet, the dispatcher stamps the owning session's `LastReceiveTime` with the current second counter, provided `0 < conn < MAX_USER`. Located at `Code/TMSrv/ProcessClientMessage.cpp:56-57`; the field is `int LastReceiveTime` at `Code/TMSrv/CUser.h:97`, documented in-file as offset `2720`.

**Detailed description**:
Idle detection in TMSrv is a purely receive-driven heartbeat: `CheckIdle(int conn)` (`Code/TMSrv/Server.cpp:4304-4322`) compares `pUser[conn].LastReceiveTime` with `SecCounter`, defends against clock wraparound by re-stamping when `lst > ser`, and disconnects when `lst < ser - 720` after logging `sys,disconnect last:%d server:%d mode:%d conn:%d`. The dispatcher is the **only** writer of `LastReceiveTime` in the receive path, so this one line (57) is the sole reason a connected-but-idle client stays connected, and the reason a client that stops sending anything is eventually disconnected. `SecCounter` itself is advanced by `ProcessSecTimer` (`Code/TMSrv/ProcessSecMinTimer.cpp:123`), i.e. on the 500 ms Win32 timer (`Code/TMSrv/Server.cpp:3631`), while the timeout thresholds (720, 1440) are expressed as if seconds - a nominal/actual ratio worth noting but irrelevant to the dispatcher's logic.

The guard `conn > 0` is not defensive padding; it reflects a hard invariant of session allocation. Slot **0 is reserved and never allocated**: `GetEmptyUser()` scans `for (int i = 1; i < MAX_USER; i++)` and returns `0` to signal "no free slot" (`Code/TMSrv/Server.cpp:6743-6752`), and `WSA_ACCEPT` aborts accept handling when it receives `0` (`Code/TMSrv/Server.cpp:4011-4018`). `GetUserFromSocket()` likewise returns `0` for "unknown socket" (`Code/TMSrv/Server.cpp:6729-6741`). So a `conn` of 0 at the dispatcher cannot be a live session, and the write is skipped. The second half of the guard (`conn < MAX_USER`) protects the array bound, although in practice `conn` always originates from `GetUserFromSocket`, which only yields `1..999`.

Placement is semantically important. The refresh occurs **after** the shutdown gate (so it stops during terminal shutdown) and **before** the `_MSG_Ping` and `SKIPCHECKTICK` filters (so discarded keep-alives and malicious sentinel packets still count as liveness). It also occurs before type dispatch, so an unknown or unhandled type still keeps the connection alive - a client that spams an unrouted type is never idle-disconnected, which is an intentional-but-undocumented leniency that interacts with BR-10.

**Rule workflow**:
```
IF conn > 0 AND conn < MAX_USER THEN
    pUser[conn].LastReceiveTime = SecCounter
END IF
LATER (ProcessSecTimer -> CheckIdle):
    IF pUser[conn].LastReceiveTime < SecCounter - 720 THEN
        Log("sys,disconnect ..."); CloseUser(conn)
    END IF
CONTINUE to BR-04
```

---

### Business Rule: BR-04 - Keep-alive (`_MSG_Ping`) suppression

**Overview**:
`_MSG_Ping` (`160 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME` = `0x03A0`, `Code/Basedef.h:2136`) is terminated at the dispatcher and never reaches a handler. Located at `Code/TMSrv/ProcessClientMessage.cpp:59-60`.

**Detailed description**:
W2 clients emit periodic ping packets to keep the TCP session and the server-side idle timer alive. Because BR-03 has already refreshed `LastReceiveTime` before this check, the ping achieves its entire purpose before being discarded; the `return` at line 60 therefore costs nothing functionally. The rule exists so that none of the 58 handlers needs a ping branch, and so that the dispatcher has a cheap high-frequency fast path: ping traffic is the largest packet volume on an idle shard and it stops after 5 comparisons (`ID` bound, `ServerDown`, `conn` bounds, `Type`).

The rule is deliberately placed after the liveness write but before the `ClientTick` sentinel check (BR-05), which means a ping carrying `SKIPCHECKTICK` is still accepted as a keep-alive. It is also placed before routing, so `_MSG_Ping` has no `case` at all in the switch - grep of the routing table in BR-06 confirms it is absent. Two contrasting properties follow: no ping is ever crack-penalised whatever its contents, and no ping is ever logged, which makes "is the client alive" invisible except through the idle-disconnect log line.

There is a performance observation here rather than a functional one: a ping has already been paid for in full by the transport layer - `recv`, `INITCODE` check, header parse, 12-byte-plus de-obfuscation loop with per-byte table lookups, and checksum accumulation (`Code/CPSock.cpp:428-453`) - before this `return` drops it. The dispatcher-level filter cannot short-circuit that cost; it only prevents handler cost.

**Rule workflow**:
```
IF std->Type == _MSG_Ping (0x03A0) THEN
    RETURN                       // keep-alive consumed; LastReceiveTime already refreshed
END IF
CONTINUE to BR-05
```

---

### Business Rule: BR-05 - Client tick sentinel rejection

**Overview**:
A packet arriving from a client (`isServer == FALSE`) whose `ClientTick` field equals the reserved sentinel `SKIPCHECKTICK` (decimal 235543242, `Code/Basedef.h:172`) is dropped without log or penalty. Located at `Code/TMSrv/ProcessClientMessage.cpp:62-64`.

**Detailed description**:
`SKIPCHECKTICK` is documented in the header as "base number for the tick checking so people don't get kicked out before doing something" (`Code/Basedef.h:172`). Functionally it is a **trust token**: it means "this packet was generated by the server, do not apply client-side timing checks". Using it is a privilege, so the dispatcher's rule is that clients may never present it. The guard is a single boolean expression combining two independent inputs - the transport-derived `isServer` flag and the wire-derived `ClientTick` value - which is why it is the only dispatcher rule that reasons about trust rather than range.

The consequences of the sentinel downstream are substantial and observable in `Exec_MSG_Attack`. At `Code/TMSrv/_MSG_Attack.cpp:59` the 800 ms attack rate limit is skipped when `ClientTick == SKIPCHECKTICK`; at line 68 the monotonic-tick check is skipped; at line 79 the 15-second drift window against `CurrentTime` is skipped; at line 104 the target-position consistency check is skipped; at lines 113/124/200/205 the skill-learned checks and skill-delay accounting are skipped; at line 494 the per-skill target-count cap is skipped; and at line 533 the sentinel plus `Motion == 254` plus `SkillIndex` in {32, 34, 36} is the exact signature used for server-generated area attacks. At line 1730-1731 the handler finally rewrites `m->ClientTick = CurrentTime` so that any downstream consumer sees a sane value. In other words, a client that could smuggle `SKIPCHECKTICK` past this guard would bypass the entire combat timing model. BR-05 is therefore the single most security-relevant line in the component.

The rule is implemented as a `return`, not as a crack penalty, so sentinel abuse is invisible in the `NumError` accounting (contrast BR-08). Logging is also absent. That combination means a determined attacker learns nothing from rejection (fail-silent), but operators also learn nothing from it - a trade-off documented here as observed behaviour, not as a recommendation. Note also that the check is `isServer == FALSE && ...`, so a client packet with any *other* value - including `0`, arbitrary garbage, or a value in the future - passes this gate and is instead filtered by the per-handler drift checks named above.

**Rule workflow**:
```
IF isServer == FALSE AND std->ClientTick == SKIPCHECKTICK (235543242) THEN
    RETURN                       // silent; no crack points, no log
END IF
CONTINUE to BR-06 (routing)
```

---

### Business Rule: BR-06 - Message type routing table

**Overview**:
The `switch (std->Type)` at `Code/TMSrv/ProcessClientMessage.cpp:66-311` maps 64 recognised type constants (59 case groups) onto 58 distinct `Exec_MSG_*` handler functions. One recognised type (`_MSG_UpdateScore`) has no handler and is handled by BR-08. Unrecognised types are dropped by BR-10.

**Detailed description**:
The routing table is expressed as a flat `switch` on a 16-bit signed field whose values are composite: a small base code OR-ed with namespace flags (`FLAG_GAME2CLIENT` = 0x0100, `FLAG_CLIENT2GAME` = 0x0200, `FLAG_DB2GAME` = 0x0400, `FLAG_GAME2DB` = 0x0800, `Code/Basedef.h:932-941`). Because the flags are part of the constant, the same numeric base code can appear in several namespaces; for example `_MSG_Attack` is `103 | 0x0100 | 0x0200` = `0x0367` while other components use `103` with DB flags. The dispatcher's `case` labels are the fully-qualified composite values, so routing is effectively "route by exact wire tag", and the compiler is free to emit a jump table for what is otherwise a 59-way comparison chain (MSVC release build, `Optimization=MaxSpeed`, `Code/TMSrv/TMSrv.vcxproj`).

The dispatch unit is a *message family*, not a message: 3 groups are multi-label. `_MSG_Action` (`0x036C`), `_MSG_Action2` (`0x0366`) and `_MSG_Action3` (`0x0368`) all funnel into `Exec_MSG_Action` (lines 96-100); `_MSG_Attack` (`0x0367`), `_MSG_AttackOne` (`0x039D`) and `_MSG_AttackTwo` (`0x039E`) all funnel into `Exec_MSG_Attack` (lines 192-196); and `_MSG_CombineItemOdin` (`0x02D2`) and `_MSG_CombineItemOdin2` (`0x02E2`) funnel into `Exec_MSG_CombineItemOdin` (lines 266-269). This is BR-07. Every other label is a 1:1 mapping. Coverage spans the full gameplay surface of the server: authentication and character lifecycle (login, create, delete, logout, account secure), social (chat, whisper, party request/accept/remove), movement (action family, motion, no-view), combat (attack family, PK mode, challenge/duel), economy (buy, sell, trade, trading-item, request-buy, quit-trade, deposit, withdraw, auto-trade shop, shop list, trade list), inventory (use, get, drop, update, split, delete item, short skill), crafting (six combine families plus tiny/shany/ailyn/agatha/odin/lindy/alquimia/extracao/ehre), progression (apply bonus, quest, ranking), guild (invite, ally, war), teleport, restart, seals, capsule info and shop.

The routing table is the de-facto **server protocol manifest**: `Code/TMSrv/ProcessClientMessage.h:74-131` declares exactly 58 handlers (numbered `Exec_MSG_Attack` .. `Exec_MSG_Quest`) and the switch calls exactly those 58 (measured: 58 unique `Exec_MSG_` identifiers in the file, 58 declarations, 66 files including the header). However, nothing enforces the correspondence at compile time: adding a new `_MSG_*` constant does not create a case, and a `case` without a handler declaration will fail to link rather than fail to compile. This is a maintainability property, recorded here as observed structure.

| Constant | Base code | Full wire value | Handler | Handler location |
| --- | --- | --- | --- | --- |
| `_MSG_AccountLogin` | 13 | `0x020D` | `Exec_MSG_AccountLogin` | `Code/TMSrv/_MSG_AccountLogin.cpp:21` |
| `_MSG_CharacterLogin` | 19 | `0x0213` | `Exec_MSG_CharacterLogin` | `Code/TMSrv/_MSG_CharacterLogin.cpp:21` |
| `_MSG_CharacterLogout` | 21 | `0x0215` | `Exec_MSG_CharacterLogout` | `Code/TMSrv/_MSG_CharacterLogout.cpp:21` |
| `_MSG_DeleteCharacter` | 17 | `0x0211` | `Exec_MSG_DeleteCharacter` | `Code/TMSrv/_MSG_DeleteCharacter.cpp:20` |
| `_MSG_CreateCharacter` | 15 | `0x020F` | `Exec_MSG_CreateCharacter` | `Code/TMSrv/_MSG_CreateCharacter.cpp:20` |
| `_MSG_AccountSecure` | 222 | `0x0FDE` | `Exec_MSG_AccountSecure` | `Code/TMSrv/_MSG_AccountSecure.cpp:21` |
| `_MSG_MessageChat` | 51 | `0x0333` | `Exec_MSG_MessageChat` | `Code/TMSrv/_MSG_MessageChat.cpp:20` |
| `_MSG_Action` | 108 | `0x036C` | `Exec_MSG_Action` | `Code/TMSrv/_MSG_Action.cpp:21` |
| `_MSG_Action2` | 102 | `0x0366` | `Exec_MSG_Action` | `Code/TMSrv/_MSG_Action.cpp:21` |
| `_MSG_Action3` | 104 | `0x0368` | `Exec_MSG_Action` | `Code/TMSrv/_MSG_Action.cpp:21` |
| `_MSG_Motion` | 106 | `0x036A` | `Exec_MSG_Motion` | `Code/TMSrv/_MSG_Motion.cpp:20` |
| `_MSG_UpdateScore` | 54 | `0x0336` | (none - blocked by BR-08) | `Code/TMSrv/ProcessClientMessage.cpp:106` |
| `_MSG_NoViewMob` | 105 | `0x0369` | `Exec_MSG_NoViewMob` | `Code/TMSrv/_MSG_NoViewMob.cpp:20` |
| `_MSG_Restart` | 137 | `0x0289` | `Exec_MSG_Restart` | `Code/TMSrv/_MSG_Restart.cpp:20` |
| `_MSG_Deprivate` | 140 | `0x028C` | `Exec_MSG_Deprivate` | `Code/TMSrv/_MSG_Deprivate.cpp:20` |
| `_MSG_Challange` | 142 | `0x028E` | `Exec_MSG_Challange` | `Code/TMSrv/_MSG_Challange.cpp:21` |
| `_MSG_ChallangeConfirm` | 143 | `0x028F` | `Exec_MSG_ChallangeConfirm` | `Code/TMSrv/_MSG_ChallangeConfirm.cpp:21` |
| `_MSG_ReqTeleport` | 144 | `0x0290` | `Exec_MSG_ReqTeleport` | `Code/TMSrv/_MSG_ReqTeleport.cpp:20` |
| `_MSG_REQShopList` | 123 | `0x027B` | `Exec_MSG_REQShopList` | `Code/TMSrv/_MSG_REQShopList.cpp:20` |
| `_MSG_Deposit` | 136 | `0x0388` | `Exec_MSG_Deposit` | `Code/TMSrv/_MSG_Deposit.cpp:20` |
| `_MSG_Withdraw` | 135 | `0x0387` | `Exec_MSG_Withdraw` | `Code/TMSrv/_MSG_Withdraw.cpp:20` |
| `_MSG_RemoveParty` | 126 | `0x037E` | `Exec_MSG_RemoveParty` | `Code/TMSrv/_MSG_RemoveParty.cpp:20` |
| `_MSG_SendReqParty` | 127 | `0x037F` | `Exec_MSG_SendReqParty` | `Code/TMSrv/_MSG_SendReqParty.cpp:20` |
| `_MSG_AcceptParty` | 171 | `0x03AB` | `Exec_MSG_AcceptParty` | `Code/TMSrv/_MSG_AcceptParty.cpp:21` |
| `_MSG_TradingItem` | 118 | `0x0376` | `Exec_MSG_TradingItem` | `Code/TMSrv/_MSG_TradingItem.cpp:20` |
| `_MSG_MessageWhisper` | 52 | `0x0334` | `Exec_MSG_MessageWhisper` | `Code/TMSrv/_MSG_MessageWhisper.cpp:20` |
| `_MSG_ChangeCity` | 145 | `0x0291` | `Exec_MSG_ChangeCity` | `Code/TMSrv/_MSG_ChangeCity.cpp:21` |
| `_MSG_PKMode` | 153 | `0x0399` | `Exec_MSG_PKMode` | `Code/TMSrv/_MSG_PKMode.cpp:20` |
| `_MSG_ReqTradeList` | 154 | `0x039A` | `Exec_MSG_ReqTradeList` | `Code/TMSrv/_MSG_ReqTradeList.cpp:20` |
| `_MSG_UpdateItem` | 116 | `0x0374` | `Exec_MSG_UpdateItem` | `Code/TMSrv/_MSG_UpdateItem.cpp:20` |
| `_MSG_Quest` | 139 | `0x028B` | `Exec_MSG_Quest` | `Code/TMSrv/_MSG_Quest.cpp:20` |
| `_MSG_SetShortSkill` | 120 | `0x0378` | `Exec_MSG_SetShortSkill` | `Code/TMSrv/_MSG_SetShortSkill.cpp:20` |
| `_MSG_Attack` | 103 | `0x0367` | `Exec_MSG_Attack` | `Code/TMSrv/_MSG_Attack.cpp:21` |
| `_MSG_AttackOne` | 157 | `0x039D` | `Exec_MSG_Attack` | `Code/TMSrv/_MSG_Attack.cpp:21` |
| `_MSG_AttackTwo` | 158 | `0x039E` | `Exec_MSG_Attack` | `Code/TMSrv/_MSG_Attack.cpp:21` |
| `_MSG_DropItem` | 114 | `0x0272` | `Exec_MSG_DropItem` | `Code/TMSrv/_MSG_DropItem.cpp:20` |
| `_MSG_GetItem` | 112 | `0x0270` | `Exec_MSG_GetItem` | `Code/TMSrv/_MSG_GetItem.cpp:20` |
| `_MSG_QuitTrade` | 132 | `0x0384` | `Exec_MSG_QuitTrade` | `Code/TMSrv/_MSG_QuitTrade.cpp:20` |
| `_MSG_UseItem` | 115 | `0x0373` | `Exec_MSG_UseItem` | `Code/TMSrv/_MSG_UseItem.cpp:41` |
| `_MSG_ApplyBonus` | 119 | `0x0277` | `Exec_MSG_ApplyBonus` | `Code/TMSrv/_MSG_ApplyBonus.cpp:21` |
| `_MSG_SendAutoTrade` | 151 | `0x0397` | `Exec_MSG_SendAutoTrade` | `Code/TMSrv/_MSG_SendAutoTrade.cpp:20` |
| `_MSG_ReqBuy` | 152 | `0x0398` | `Exec_MSG_ReqBuy` | `Code/TMSrv/_MSG_ReqBuy.cpp:20` |
| `_MSG_Buy` | 121 | `0x0379` | `Exec_MSG_Buy` | `Code/TMSrv/_MSG_Buy.cpp:21` |
| `_MSG_Sell` | 122 | `0x037A` | `Exec_MSG_Sell` | `Code/TMSrv/_MSG_Sell.cpp:20` |
| `_MSG_Trade` | 131 | `0x0383` | `Exec_MSG_Trade` | `Code/TMSrv/_MSG_Trade.cpp:20` |
| `_MSG_CombineItem` | 166 | `0x03A6` | `Exec_MSG_CombineItem` | `Code/TMSrv/_MSG_CombineItem.cpp:21` |
| `_MSG_ReqRanking` | 159 | `0x039F` | `Exec_MSG_ReqRanking` | `Code/TMSrv/_MSG_ReqRanking.cpp:20` |
| `_MSG_CombineItemEhre` | 211 | `0x02D3` | `Exec_MSG_CombineItemEhre` | `Code/TMSrv/_MSG_CombineItemEhre.cpp:20` |
| `_MSG_CombineItemTiny` | 192 | `0x03C0` | `Exec_MSG_CombineItemTiny` | `Code/TMSrv/_MSG_CombineItemTiny.cpp:20` |
| `_MSG_CombineItemShany` | 196 | `0x02C4` | `Exec_MSG_CombineItemShany` | `Code/TMSrv/_MSG_CombineItemShany.cpp:20` |
| `_MSG_CombineItemAilyn` | 181 | `0x03B5` | `Exec_MSG_CombineItemAilyn` | `Code/TMSrv/_MSG_CombineItemAilyn.cpp:20` |
| `_MSG_CombineItemAgatha` | 186 | `0x03BA` | `Exec_MSG_CombineItemAgatha` | `Code/TMSrv/_MSG_CombineItemAgatha.cpp:21` |
| `_MSG_CombineItemOdin` | 210 | `0x02D2` | `Exec_MSG_CombineItemOdin` | `Code/TMSrv/_MSG_CombineItemOdin.cpp:20` |
| `_MSG_CombineItemOdin2` | 226 | `0x02E2` | `Exec_MSG_CombineItemOdin` | `Code/TMSrv/_MSG_CombineItemOdin.cpp:20` |
| `_MSG_DeleteItem` | 228 | `0x02E4` | `Exec_MSG_DeleteItem` | `Code/TMSrv/_MSG_DeleteItem.cpp:20` |
| `_MSG_InviteGuild` | 213 | `0x03D5` | `Exec_MSG_InviteGuild` | `Code/TMSrv/_MSG_InviteGuild.cpp:20` |
| `_MSG_SplitItem` | 229 | `0x02E5` | `Exec_MSG_SplitItem` | `Code/TMSrv/_MSG_SplitItem.cpp:20` |
| `_MSG_CombineItemLindy` | 195 | `0x02C3` | `Exec_MSG_CombineItemLindy` | `Code/TMSrv/_MSG_CombineItemLindy.cpp:20` |
| `_MSG_CombineItemAlquimia` | 225 | `0x02E1` | `Exec_MSG_CombineItemAlquimia` | `Code/TMSrv/_MSG_CombineItemAlquimia.cpp:20` |
| `_MSG_CombineItemExtracao` | 212 | `0x02D4` | `Exec_MSG_CombineItemExtracao` | `Code/TMSrv/_MSG_CombineItemExtracao.cpp:20` |
| `_MSG_GuildAlly` | 18 | `0x0E12` | `Exec_MSG_GuildAlly` | `Code/TMSrv/_MSG_GuildAlly.cpp:20` |
| `_MSG_War` | 14 | `0x0E0E` | `Exec_MSG_War` | `Code/TMSrv/_MSG_War.cpp:20` |
| `_MSG_CapsuleInfo` | 205 | `0x02CD` | `Exec_MSG_CapsuleInfo` | `Code/TMSrv/_MSG_CapsuleInfo.cpp:21` |
| `_MSG_PutoutSeal` | 204 | `0x03CC` | `Exec_MSG_PutoutSeal` | `Code/TMSrv/_MSG_PutoutSeal.cpp:20` |
| `_MSG_Ping` | 160 | `0x03A0` | (none - dropped by BR-04) | `Code/TMSrv/ProcessClientMessage.cpp:59` |

**Rule workflow**:
```
switch (std->Type)                    // 64 constants, 59 case groups
    case _MSG_AccountLogin:      Exec_MSG_AccountLogin(conn, pMsg);      break;
    case _MSG_Action:
    case _MSG_Action2:
    case _MSG_Action3:           Exec_MSG_Action(conn, pMsg);            break;
    ...
    case _MSG_UpdateScore:       Log("cra client send update score", ...);
                                 AddCrackError(conn, 2, 91);             break;
    ...
    case _MSG_PutoutSeal:        Exec_MSG_PutoutSeal(conn, pMsg);        break;
    // NO default label
END switch
RETURN
```

---

### Business Rule: BR-07 - Aliased message families share a handler

**Overview**:
Three message families deliver several distinct wire tags into a single handler: `Action/Action2/Action3` -> `Exec_MSG_Action` (`Code/TMSrv/ProcessClientMessage.cpp:96-100`), `Attack/AttackOne/AttackTwo` -> `Exec_MSG_Attack` (`:192-196`), `CombineItemOdin/CombineItemOdin2` -> `Exec_MSG_CombineItemOdin` (`:266-269`). In each group the packet's own `Type` field survives into the handler so the handler can branch.

**Detailed description**:
This rule exists because the W2 protocol evolved by splitting message codes without splitting semantics. `_MSG_Attack` (`0x0367`, base 103) is the original attack packet, while `_MSG_AttackOne` (`0x039D`, base 157) and `_MSG_AttackTwo` (`0x039E`, base 158) are later variants; the client chooses based on the number of affected targets or the skill shape. The server intentionally resolves all three into one combat authority (`Code/TMSrv/_MSG_Attack.cpp:21`), which re-derives semantics from payload fields (`SkillIndex`, `Motion`, `Dam[]`, `TargetX/TargetY`, `AttackerID`, `CurrentMp`) rather than from `Type`. Evidence for that convention: the internal attack generator sets `Type = _MSG_AttackOne` but also sets `Motion = 254` and `SkillIndex` to 32/34/36 precisely because the *handler* keys off those fields (`Code/TMSrv/Server.cpp:4595-4617`, with the handler special-casing at `_MSG_Attack.cpp:533`). Similarly the action group carries movement variants (`0x036C`, `0x0366`, `0x0368`) into `Exec_MSG_Action`, whose first business checks are session-state and trade-state, not type (`_MSG_Action.cpp:25-50`), and the Odin combine pair (`0x02D2`, `0x02E2`) is unified so both recipe generations share validation.

The behavioural consequence for the dispatcher is that `Type` is used exclusively as a *routing key*, never as a *semantic discriminator*, and that the mapping is many-to-one in three places only. Any auditing assumption that "one wire tag = one code path" is false for combat, movement and one crafting family - which is why combat rate-limiting (BR-05's downstream effects) must live inside the handler, where it can see all three variants. The rule also means that a client switching between `_MSG_Attack` and `_MSG_AttackOne` for the same action hits the same `Exec_MSG_Attack` state (`pUser[conn].LastAttackTick`), so no rate-limit bypass is available by tag-switching - a beneficial property that arises from the aliasing rather than from an explicit control.

From a maintenance standpoint these are the only three multi-label groups out of 59, so the alias set is stable and small. Nothing in the code names the alias groups; the grouping is implicit in switch label adjacency. This is recorded as observed structure.

**Rule workflow**:
```
std->Type ∈ {0x036C, 0x0366, 0x0368}  -> Exec_MSG_Action(conn, pMsg)
std->Type ∈ {0x0367, 0x039D, 0x039E}  -> Exec_MSG_Attack(conn, pMsg)
std->Type ∈ {0x02D2, 0x02E2}          -> Exec_MSG_CombineItemOdin(conn, pMsg)
Inside handler: discriminate on payload fields, not on Type
```

---

### Business Rule: BR-08 - Client-sent `_MSG_UpdateScore` is blocked and penalised

**Overview**:
`_MSG_UpdateScore` (`54 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME` = `0x0336`, `Code/Basedef.h:1466`) is a legitimate server-to-client score notification. If a client sends it, the dispatcher logs `cra client send update score` and calls `AddCrackError(conn, 2, 91)`. Located at `Code/TMSrv/ProcessClientMessage.cpp:106-110`. This is the only `case` in the entire switch that has no handler call.

**Detailed description**:
This is the dispatcher's only active anti-cheat rule. It encodes a directional constraint that the flag system cannot express: `_MSG_UpdateScore` is flagged both `FLAG_GAME2CLIENT` and `FLAG_CLIENT2GAME`, so from the type tag alone it looks bidirectional, and the switch would happily route it. Instead the dispatcher treats client origination as an anomaly. The reasoning is visible in the payload: `MSG_UpdateScore` carries the authoritative `STRUCT_SCORE` block, so a client that could push it would be able to declare its own stats - the classic "stat injection" attack in a client-authoritative protocol.

The penalty path is `AddCrackError(int conn, int val, int Type)` (`Code/TMSrv/Server.cpp:684-707`). It suppresses the log line only for `Type` values 3, 8 and 15; type 91 is therefore logged twice (once by the dispatcher's own `Log` call at line 108, once inside `AddCrackError` at line 688-689 as `cra point: 2  type: 91`). It then accumulates into `pUser[conn].NumError` (`int`, `Code/TMSrv/CUser.h:75`) and, when the accumulator reaches `>= 2000000000`, sends `g_pMessageStringTable[_NN_Bad_Network_Packets]`, calls `CharLogOut(conn)`, logs `cra char logout type: %d` and returns `TRUE`. Because the increment is only 2 per violation, the terminal threshold is unreachable in practice by this rule alone (2 billion points requires ~1e9 packets); its operational value is the *accumulating counter* shared with the other 44 `AddCrackError` call sites across 22 files, which together implement a soft cheat-score. Two details are worth recording: the threshold comparison uses a signed `int` accumulated additively, so it is overflow-prone by construction; and `pUser[conn]` is indexed by `conn` without a fresh range check inside `AddCrackError` (the dispatcher's guarantee that `conn` came from `GetUserFromSocket` is the only protection).

The rule demonstrates the component's design philosophy in miniature: known-cheat message types are enumerated as explicit `case` blocks rather than filtered by a direction bit, because the legacy protocol has no reliable direction field. It also shows the asymmetry between BR-05 (silent drop) and BR-08 (logged and charged) - the former protects a timing bypass, the latter protects the stat authority, and only the latter leaves an audit trail.

**Rule workflow**:
```
case _MSG_UpdateScore (0x0336):
    Log("cra client send update score", pUser[conn].AccountName, pUser[conn].IP)   // -> fLogFile + window
    AddCrackError(conn, 2, 91):
        Log("cra point: 2  type: 91", AccountName, IP)
        pUser[conn].NumError += 2
        IF NumError >= 2000000000 THEN
            SendClientMessage(conn, _NN_Bad_Network_Packets)
            CharLogOut(conn)
            Log("cra char logout type: 91", ...)
            RETURN TRUE
        END IF
    break                                          // no handler invoked
```

---

### Business Rule: BR-09 - Server-originated packet injection (`isServer == TRUE`)

**Overview**:
`isServer` is the third parameter of the dispatcher (`Code/TMSrv/ProcessClientMessage.cpp:38`). When `TRUE`, BR-05 (client tick sentinel rejection) is bypassed, allowing TMSrv's own subsystems to synthesise packets and push them through the identical validation, routing and handler pipeline. There are 4 such call sites, all in `Code/TMSrv/Server.cpp`: lines 4622, 5062, 5371, 7464.

**Detailed description**:
TMSrv reuses the client entry point for server-authored actions, which is a significant architectural decision: instead of exposing `Exec_MSG_Attack` as an internal API, the mob AI builds a real `MSG_AttackOne` on the stack and calls the dispatcher. The primary site (`Code/TMSrv/Server.cpp:4591-4622`) is the mob melee attack: it zeroes the struct, sets `Type = _MSG_AttackOne`, `ID = idx`, `Size = sizeof(MSG_AttackOne)`, `ClientTick = 0xE0A1ACA`, `Motion = 254`, `SkillIndex` 32/34/36 depending on `FadaIndex`, `CurrentMp = -2`, `AttackerID = idx`, and `Dam[0].TargetID/Damage`. The three remaining sites inject area/aura attacks (`calc > 350/400/450/500` cascades allocating `Dam[]` slots) and party auto-acceptance (`MSG_AcceptParty` with `LeaderID = novolider`, `MobName` copied from the leader, and `pMob[partyconn].LastReqParty` pre-set before dispatch at `Server.cpp:7451-7464`).

The numeric detail that ties this rule to BR-05 deserves emphasis: `sm.ClientTick = 0xE0A1ACA` at `Server.cpp:4600` equals decimal **235,543,242**, which is exactly `SKIPCHECKTICK` (`Code/Basedef.h:172`). So the AI is not merely setting a magic number; it is deliberately presenting the sentinel. The dispatcher's line 63 would then reject this packet if it had arrived with `isServer == FALSE`; with `isServer == TRUE` it passes, and inside the handler `ClientTick == SKIPCHECKTICK` disables the timing checks and marks the packet as server-authoritative (`_MSG_Attack.cpp:59,68,79,104,113,124,200,205,494,533`), with `_MSG_Attack.cpp:1730-1731` rewriting the tick to `CurrentTime` afterwards. This is a complete, end-to-end trust channel built from one boolean parameter and one sentinel constant.

Three behavioural consequences belong to this rule. First, **internal packets are still subject to BR-01 and BR-02**: mob attacks are dropped if `ServerDown >= 120`, and would be dropped by BR-01 if `idx` were outside `[0, 1000)` - in practice the AI guards this itself (`if (idx < MAX_USER)` at lines 5061 and 5369), and site 4622 does not re-check. Second, internal packets still refresh `LastReceiveTime` for the target slot (BR-03), so an AFK player being attacked by a mob is kept alive by the mob's own traffic - a subtle interaction. Third, the internal path has no error return: `ProcessClientMessage` is `void`, so an injection that a handler rejects is invisible to the caller.

**Rule workflow**:
```
CALLER (mob AI / party logic):
    build MSG_* on stack; set Type, ID = idx, Size = sizeof(MSG_*)
    set ClientTick = 0xE0A1ACA (== SKIPCHECKTICK)
    ProcessClientMessage(idx, (char*)&sm, TRUE)

DISPATCHER:
    BR-01 ID range   -> enforced
    BR-02 ServerDown -> enforced
    BR-03 liveness   -> enforced
    BR-04 Ping       -> enforced
    BR-05 tick gate  -> SKIPPED because isServer != FALSE
    BR-06 routing    -> normal

HANDLER (_MSG_Attack):
    ClientTick == SKIPCHECKTICK -> skip rate/drift/learned-skill checks
    at end: m->ClientTick = CurrentTime
```

---

### Business Rule: BR-10 - Unmatched message types are silently discarded

**Overview**:
The switch at `Code/TMSrv/ProcessClientMessage.cpp:66-311` has **no `default:` label** (verified: zero `default:` matches in the file). Any `Type` value that does not exactly equal one of the 64 routed constants falls out of the switch to the function's trailing `return` at line 312. No log line, no crack points, no client response.

**Detailed description**:
This is the dispatcher's fail-closed routing policy at the message level, and it is entirely implicit - there is no code that expresses it, only the absence of a `default`. The packet has already been fully paid for by the transport layer, and it has already refreshed the session's idle timer (BR-03), so the drop is cheap and leaves no trace. That has three practical consequences. First, **protocol probing is silent**: a client sending `Type = 0x9999` receives nothing back, and the server logs nothing, which makes the reachable message surface unobservable to an attacker (good) and to an operator (bad). Second, **protocol desync is hard to diagnose**: a client built against a newer protocol version whose new message types are unknown to this server will appear merely "unresponsive", with no `err,packet` line to correlate - unlike BR-01, which does log. Third, **unrouted traffic is not crack-penalised**: only `_MSG_UpdateScore` (BR-08) accrues points.

The drop is also the mechanism by which BR-13's flag namespacing enforces itself. Because values are compared with full flag bits, a packet built with a bare base code (e.g. `Type = 13` instead of `0x020D`) matches nothing and is dropped silently - the server cannot distinguish "wrong namespace" from "unknown message", and both land here. During the two-minute shutdown window, this rule combines with BR-02 so that everything below the gate is moot: BR-02 returns first.

The absence of a `default` also means the 191 declared `_MSG_*` constants in `Code/Basedef.h` (the architecture report counts 191 message constants across all namespaces) are mostly *not* accepted from clients; the dispatcher's 64 constants are the complete allow-list, i.e. roughly a third of the declared protocol. Everything else - DB-namespace, NP-namespace, and most game-to-client-only messages - is dropped here by construction.

**Rule workflow**:
```
switch (std->Type)
    ... 59 case groups ...
    // no default
END switch
return                              // unreachable labels fall through to here
```

---

### Business Rule: BR-11 - No payload validation: the handler-delegation contract

**Overview**:
The dispatcher validates exactly one 16-bit field (`ID`) and one 32-bit field (`ClientTick`) of the 12-byte header. It never validates `Size`, never validates `KeyWord`/`CheckSum` semantics, never validates session state (`pUser[conn].Mode`), never validates world state (dead/alive, trade state, map position), and performs an unchecked reinterpret cast of the raw `char*` to `MSG_STANDARD*`. Located at `Code/TMSrv/ProcessClientMessage.cpp:40,42-64` (all validation) versus `:66-311` (all dispatch).

**Detailed description**:
The delegation contract is the component's most consequential implicit business rule: *every* structural and semantic guarantee about a packet's payload is the receiving handler's responsibility. The dispatcher's total validation budget is 6 comparisons and 1 `sprintf`; the transport layer above it (`CPSock::ReadMessage`, `Code/CPSock.cpp:353-467`) guarantees only frame boundaries (`Size <= MAX_MESSAGE_SIZE` = 8192 and `Size >= sizeof(HEADER)` = 12, lines 397-406) and frame completeness (line 410), plus a checksum that is *not enforced* - on mismatch the packet is still returned with `ErrorCode = 1` and dropped by the caller's error gate at `Code/TMSrv/Server.cpp:3990`. Crucially, nothing anywhere verifies that `Size` matches `sizeof(MSG_Attack)` for an `_MSG_Attack`, and every handler immediately casts (`MSG_Attack *m = (MSG_Attack*)pMsg;` at `_MSG_Attack.cpp:23`; `MSG_Buy *m = (MSG_Buy*)pMsg;` at `_MSG_Buy.cpp:23`; and so on for all 58).

The intended safeguard is `BASE_CheckPacket(MSG_STANDARD *m)` (`Code/Basedef.cpp:6475-6596`), a table of ~100 `if (m->Type == X && m->Size != sizeof(MSG_X)) code = 1;` assertions. It is **dead code**: the entire body is inside a comment block opened at line 6476 and closed at line 6594, and the function ends with `return FALSE;` at line 6595. Its two client-path call sites (`Code/TMSrv/Server.cpp:3983` inside the `_PACKET_DEBUG` block, and `Code/CPSock.cpp:547`) therefore always evaluate false and are no-ops. The Release build does not even define `_PACKET_DEBUG` (`PreprocessorDefinitions` in `Code/TMSrv/TMSrv.vcxproj`: `_CRT_SECURE_NO_WARNINGS;WIN32;NDEBUG;_WINDOWS`), so the diagnostic block is compiled out entirely. The practical result is that a client can send a 12-byte `_MSG_Attack` header and the handler will read `Dam[0..12]`, `SkillIndex`, and coordinate fields far beyond the received bytes from within the 128 KB receive buffer (`RECV_BUFFER_SIZE`, `Code/CPSock.h:35`) - a read past the logical packet boundary that stays inside the allocated buffer, producing handler-visible garbage rather than a crash.

The same delegation covers world state. Each handler re-implements the standard preconditions, with slight variations that are visible across files: `pMob[conn].MOB.CurrentScore.Hp == 0 || pUser[conn].Mode != USER_PLAY` -> `SendHpMode` + `AddCrackError` (e.g. `_MSG_Buy.cpp:25-30`, `_MSG_Sell.cpp:24-29`, `_MSG_Trade.cpp:24-30`), trade-state teardown via `RemoveTrade` (`_MSG_Trade.cpp:28`, `_MSG_Buy.cpp:32-43`), `pUser[conn].TradeMode` rejection with `_NN_CantWhenAutoTrade` (`_MSG_Attack.cpp:27-31`, `_MSG_Quest.cpp:42-46`). Because the dispatcher does none of this, a single missing check in any of 58 handlers is a direct gameplay exploit with no second line of defence - which is exactly the risk class this component's design concentrates.

**Rule workflow**:
```
DISPATCHER: validate header only (ID, ClientTick, Type) -> cast char* to MSG_STANDARD*
HANDLER:    cast char* to MSG_X*  (no size check)
            validate Size?          -> not done anywhere (BASE_CheckPacket is a stub)
            validate session mode   -> per handler
            validate world state    -> per handler
            validate payload ranges -> per handler
```

---

### Business Rule: BR-12 - Dispatch identity is `conn`, not the packet `ID`

**Overview**:
Every routed handler is invoked as `Exec_MSG_<Family>(conn, pMsg)`, where `conn` is the socket-owner session index supplied by `MainWndProc`/`GetUserFromSocket` (`Code/TMSrv/Server.cpp:3921,4001`). The packet's `ID` field is validated (BR-01) and then discarded. Handlers that need a second identity read it from their own payload and re-validate it.

**Detailed description**:
This is the component's core trust decision and the reason the server survives a client-authoritative protocol. `conn` cannot be forged by a packet: it is derived from the socket handle that delivered the bytes (`GetUserFromSocket(wParam)`, `Code/TMSrv/Server.cpp:6729-6741`, scanning `pUser[i].cSock.Sock == Sock` for `i` in `1..MAX_USER-1`). The `ID` field, by contrast, is client-controlled. Because the dispatcher passes only `conn`, all implicit authority in the handler family is anchored to the socket: `pUser[conn]` is the session, `pMob[conn]` is the player's entity record, and `pMob[conn].MOB.CurrentScore` is the character's authoritative state. Every handler starts from these (e.g. `_MSG_Attack.cpp:33` checks `pUser[conn].Mode != USER_PLAY`; `_MSG_Buy.cpp:25` reads `pMob[conn].MOB.CurrentScore.Hp`), which is why "who am I" is never negotiable.

The second identity - "who is the other party" - is unavoidably in the payload, and each handler that needs it validates the range itself: `_MSG_Trade.cpp:34-40` rejects `OpponentID <= 0 || OpponentID >= MAX_USER` with `_NN_Wrong_Trade_Packet1` and `RemoveTrade`; `_MSG_Buy.cpp:49-53` rejects out-of-range `TargetID`/`TargetInvenPos`/`MyInvenPos` with a logged `err,MSG_BUY, target of carrypos outofrange`; `_MSG_Quest.cpp:27-28` rejects `npcIndex < MAX_USER || npcIndex >= MAX_MOB`; `_MSG_War.cpp:27-31` rejects guild/enemy outside `1..65535`. The recurring shape is `MAX_USER` as the player/mob boundary and `MAX_MOB` (25000) as the upper entity bound, matching the architecture report's "slot identity aliasing" observation.

For the injected path (BR-09) the same field is reused as a self-identification: internal attack packets set `ID = idx` (`Server.cpp:4596`) and party injection sets `ID = 0` (`Server.cpp:7456`). Both satisfy BR-01's range, and both are ignored by routing, which is consistent: the field is metadata, and `conn` is authority. One inconsistency is worth noting for completeness - handlers rewrite `m->ID` during processing (for example `_MSG_Attack.cpp:25` sets `m->ID = ESCENE_FIELD` at the very start), so by the time a handler emits multicast packets the `ID` field has been repurposed as "sender scene marker" (30000, `Code/Basedef.h:170`) rather than a session index.

**Rule workflow**:
```
conn   = GetUserFromSocket(wParam)          // transport-derived, unforgeable
std->ID validated for range only            // BR-01; then unused for dispatch
-> Exec_MSG_X(conn, pMsg)                   // self identity always conn
-> handler validates secondary ids from payload
   (OpponentID / TargetID / npcIndex / guild) as its own business rule
```

---

### Business Rule: BR-13 - Implicit flag namespacing of the `Type` field

**Overview**:
Message codes are composite: base code OR namespace flags (`Code/Basedef.h:932-941`) and the dispatcher compares the full value. A packet whose `Type` lacks the expected flag bits therefore matches no `case` and is dropped by BR-10. No code in the dispatcher mentions flags.

**Detailed description**:
The flag system (`FLAG_GAME2CLIENT` 0x0100, `FLAG_CLIENT2GAME` 0x0200, `FLAG_DB2GAME` 0x0400, `FLAG_GAME2DB` 0x0800, plus `FLAG_DB2NP` 0x1000, `FLAG_NP2DB` 0x2000, `FLAG_NEW` 0x4000) is how one 16-bit `Type` space is partitioned among five links: client, game, DB, NP tool and a "new" band. The dispatcher's allow-list is expressed entirely in composite values, so the flags act as an implicit routing filter that the component never has to test. The two four-flag exceptions demonstrate the design's intent: `_MSG_AccountSecure` (`0x0FDE`: `222 | DB2GAME | GAME2DB | CLIENT2GAME | GAME2CLIENT`) and `_MSG_War`/`_MSG_GuildAlly` (`0x0E0E` / `0x0E12`: base `| CLIENT2GAME | GAME2DB | DB2GAME`) are the messages that genuinely travel the client -> game -> DB -> game -> client round trip, and the dispatcher must accept them on the inbound leg even though their flag set includes DB bits.

The enforcement is exact-match, which produces a specific and useful property: **the dispatcher cannot be tricked into processing a DB-namespace or NP-namespace packet.** A packet with `Type = 0x0E0E` is `_MSG_War`, while a packet with `Type = 0x0600 | 14` (a hypothetical DB-only form) is nothing at all. Because the allow-list is exhaustive and closed, the client-visible protocol surface is precisely the 64 constants in BR-06, and namespace confusion - the classic bug in flag-tagged protocols - collapses into a silent drop rather than a mis-route. The corresponding cost is that adding a message requires editing the switch, and that a legitimate client sending a slightly different flag combination produces no diagnostic (BR-10, no `default` log).

One ordering nuance: `Type` is compared as a signed `short`, and several flag combinations make the composite value large - `_MSG_GuildAlly` at `0x0E12` (3602) and `_MSG_AccountSecure` at `0x0FDE` (4062) are the largest in the allow-list, well under `0x7FFF`, so no case label is negative. `FLAG_NEW` (0x4000) combined with any code above 0x3FFF would become negative; no such constant is routed by this component.

**Rule workflow**:
```
IF std->Type == <exact composite constant> THEN route to handler
ELSE                                         -> BR-10 silent drop
Note: base code alone (e.g. 13) never matches; FLAG_CLIENT2GAME (0x0200) is mandatory
      for client-originated routing, except the four/five-flag round-trip messages.
```

---

## 4. Component Structure

```
Code/TMSrv/
├── ProcessClientMessage.cpp        # THE COMPONENT (313 lines, 1 function, 1 switch)
│   ├── :38-40    ProcessClientMessage(conn, pMsg, isServer) + MSG_STANDARD cast
│   ├── :42-51    BR-01  ID range gate + sprintf + Log
│   ├── :53-54    BR-02  ServerDown >= 120 gate
│   ├── :56-57    BR-03  LastReceiveTime refresh
│   ├── :59-60    BR-04  _MSG_Ping drop
│   ├── :62-64    BR-05  SKIPCHECKTICK sentinel rejection
│   ├── :66-311   BR-06  routing switch: 64 constants / 59 case groups / 58 handlers
│   │    ├── :106-110  BR-08 _MSG_UpdateScore -> Log + AddCrackError, no handler
│   │    ├── :96-100   BR-07 Action/Action2/Action3  -> Exec_MSG_Action
│   │    ├── :192-196  BR-07 Attack/AttackOne/AttackTwo -> Exec_MSG_Attack
│   │    └── :266-269  BR-07 CombineItemOdin/Odin2 -> Exec_MSG_CombineItemOdin
│   └── :311-312  BR-10 fall-through return (no default label)
│
├── ProcessClientMessage.h          # CONTRACT (133 lines)
│   ├── :21-43    aggregate includes (Windows/CRT + Basedef, CPSock, ItemEffect,
│   │             Language, CItem, Server, GetFunc, SendFunc, CMob, CUser,
│   │             CNPCGene, CReadFiles, CCastleZakum, CWarTower)
│   ├── :47-68    externs: hWndMain, ServerGroup, CurrentTime, BrState, g_pGuildWar,
│   │             g_pGuildAlly, GuildInfo, pMobGrid, pItemGrid, pHeightGrid,
│   │             CurrentWeather, GuildImpostoID, mNPCGen, pUser[MAX_USER]
│   ├── :70       dispatcher declaration
│   └── :74-131   58 Exec_MSG_* handler declarations (the de-facto handler API)
│
├── _MSG_*.cpp                      # 58 HANDLERS (downstream component family, 1 caller each)
│   ├── _MSG_Attack.cpp            (combat authority; 27 ClientTick references)
│   ├── _MSG_UseItem.cpp           (largest handler, 160 KB)
│   ├── _MSG_Quest.cpp             (73 KB quest engine)
│   ├── _MSG_Sell.cpp / _MSG_Buy.cpp / _MSG_Trade.cpp / _MSG_TradingItem.cpp
│   ├── _MSG_CombineItem*.cpp      (10 crafting families)
│   └── ... 40+ structurally identical handlers (line 20/21 function entry)
│
├── Server.cpp                      # UPSTREAM caller + supporting services
│   ├── :46        int ServerDown = -1000            (BR-02 state)
│   ├── :277       char temp[4096]                   (BR-01 log buffer)
│   ├── :684-707   AddCrackError                     (BR-08 penalty sink)
│   ├── :3631-3632 SetTimer TIMER_SEC 500 ms / TIMER_MIN 12 s
│   ├── :3919-4004 case WSA_READ  -> Receive + ReadMessage loop -> dispatch
│   ├── :4001      ProcessClientMessage(User, Msg, FALSE)      client path
│   ├── :4094      ServerDown = 1 (menu IDC_REBOOT)
│   ├── :4304-4322 CheckIdle (720-tick idle disconnect)
│   ├── :4622      ProcessClientMessage(idx, &sm, TRUE)        mob melee injection
│   ├── :5062      ProcessClientMessage(idx, &sm, TRUE)        area attack injection
│   ├── :5371      ProcessClientMessage(idx, &sm, TRUE)        aura attack injection
│   ├── :6701-6727 Log
│   ├── :6729-6741 GetUserFromSocket                            conn provenance
│   ├── :6743-6752 GetEmptyUser (starts at 1 -> slot 0 reserved)
│   └── :7464      ProcessClientMessage(partyconn, &sm, TRUE)  party auto-accept injection
│
├── ProcessSecMinTimer.cpp          # BR-02 state machine
│   ├── :42-93     ServerDown == 120 -> drain one user per tick -> PostQuitMessage
│   └── :96-119    ServerDown countdown, notice every 20th, timer 200 ms at 120
│
└── ... 92 files in Code/TMSrv total; 66 TUs include ProcessClientMessage.h
```

Supporting definitions outside the component boundary that the dispatcher depends on:

```
Code/Basedef.h
├── :56    MAX_USER 1000            (BR-01 upper bound, player/mob boundary)
├── :107   MAX_MOB 25000            (entity upper bound used by handlers)
├── :170   ESCENE_FIELD 30000       (ID repurposed by handlers on output)
├── :172   SKIPCHECKTICK 235543242  (BR-05 sentinel / BR-09 trust token)
├── :925-930  #define _MSG  Size; KeyWord; CheckSum; Type; ID; ClientTick
├── :932-941  FLAG_GAME2CLIENT / CLIENT2GAME / DB2GAME / GAME2DB / DB2NP / NP2DB / NEW
├── :943-946  struct MSG_STANDARD { _MSG; }        (12 bytes, BR-11 cast target)
└── :1210-2159  the _MSG_* constant space (191 constants declared project-wide)

Code/CPSock.h
├── :35   RECV_BUFFER_SIZE 128 KB
├── :38   MAX_MESSAGE_SIZE 8192
├── :40   INITCODE 0x1F11F311
└── :42-50 struct HEADER { Size; KeyWord; CheckSum; Type; ID; ClientTick; }  (mirrors _MSG)

Code/CPSock.cpp
├── :336-347  Receive()
└── :353-467  ReadMessage(): INITCODE, header, size bounds, de-obfuscation, checksum

Code/Basedef.cpp
└── :6475-6596  BASE_CheckPacket(): entire validation table commented out, returns FALSE
```

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (outbound, what the component needs)

```
ProcessClientMessage (Code/TMSrv/ProcessClientMessage.cpp:38)
│
├── [58 direct call targets - command handlers, 1 call each]
│   Exec_MSG_AccountLogin · CharacterLogin · CharacterLogout · DeleteCharacter ·
│   CreateCharacter · AccountSecure · MessageChat · Action · Motion · NoViewMob ·
│   Restart · Deprivate · Challange · ChallangeConfirm · ReqTeleport · REQShopList ·
│   Deposit · Withdraw · RemoveParty · SendReqParty · AcceptParty · TradingItem ·
│   MessageWhisper · ChangeCity · PKMode · ReqTradeList · UpdateItem · Quest ·
│   SetShortSkill · Attack · DropItem · GetItem · QuitTrade · UseItem · ApplyBonus ·
│   SendAutoTrade · ReqBuy · Buy · Sell · Trade · CombineItem · ReqRanking ·
│   CombineItemEhre · CombineItemTiny · CombineItemShany · CombineItemAilyn ·
│   CombineItemAgatha · CombineItemOdin · DeleteItem · InviteGuild · SplitItem ·
│   CombineItemLindy · CombineItemAlquimia · CombineItemExtracao · GuildAlly ·
│   War · CapsuleInfo · PutoutSeal
│   (declared Code/TMSrv/ProcessClientMessage.h:74-131; defined in 58 _MSG_*.cpp files)
│
├── Log(char*, char*, unsigned int)            Code/TMSrv/Server.cpp:6701  (decl Server.h)
│   └── writes fLogFile + SetWindowText(hWndMain) ; consumes global temp[4096]
│
├── AddCrackError(int conn, int val, int Type) Code/TMSrv/Server.cpp:684   (decl Server.h:70)
│   └── pUser[conn].NumError += val ; >= 2e9 -> SendClientMessage + CharLogOut
│
└── [global state reads/writes]
    pUser[MAX_USER]        Code/TMSrv/ProcessClientMessage.h:68 (def Server.cpp)
    SecCounter             extern in Server.h  (advanced ProcessSecMinTimer.cpp:123)
    ServerDown             Server.h:198 (def Server.cpp:46)
    temp[4096]             Server.h:382 (def Server.cpp:277)
```

Header-level dependency surface (`Code/TMSrv/ProcessClientMessage.cpp:19-36`):

| Include | Kind | Used for | Location |
| --- | --- | --- | --- |
| `<Windows.h>` | CRT/OS | `BOOL`, `TRUE`/`FALSE` | `ProcessClientMessage.cpp:19` |
| `<stdio.h>` | CRT | `sprintf` (BR-01 log line) | `:20` |
| `<fcntl.h>`, `<time.h>`, `<math.h>`, `<io.h>`, `<errno.h>` | CRT | carried by the aggregate contract; not used directly by this TU | `:21-25` |
| `..\Basedef.h` | Project | `MSG_STANDARD`, `MAX_USER`, `_MSG_*` constants, flags | `:27` |
| `..\CPSock.h` | Project | `HEADER`, socket layer types (indirect) | `:28` |
| `..\ItemEffect.h` | Project | item effect enums for handlers (indirect) | `:29` |
| `Language.h` | Project | `g_pMessageStringTable`, `_NN_*` indices (indirect) | `:30` |
| `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h` | Project | `pUser`, `Log`, `AddCrackError`, world declarations | `:32-36` |
| `ProcessClientMessage.h` | Project | handler declarations + 10 transitive subsystem headers | `:34` |

**Transitive dependency amplification.** `ProcessClientMessage.h:27-43` is itself an aggregate: it pulls `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`, `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h`, `CMob.h`, `CUser.h`, `CNPCGene.h`, `CReadFiles.h`, `CCastleZakum.h`, `CWarTower.h`. The dispatcher therefore compiles against the entire server surface even though it calls only `Log`, `AddCrackError` and the 58 handlers. This is why the architecture report counts efferent coupling of ~14 for the component and ~14 for every handler TU: the header is the wiring hub.

### 5.2 Upstream dependencies (inbound callers)

| Caller | Kind | Call | Purpose | Location |
| --- | --- | --- | --- | --- |
| `MainWndProc` / `case WSA_READ` | Client path | `ProcessClientMessage(User, Msg, FALSE)` | Dispatch every framed client packet | `Code/TMSrv/Server.cpp:4001` |
| mob AI combat loop | Self-injection | `ProcessClientMessage(idx, &sm, TRUE)` | Mob melee attack as synthetic `_MSG_AttackOne` | `Code/TMSrv/Server.cpp:4622` |
| mob AI area attack | Self-injection | `ProcessClientMessage(idx, &sm, TRUE)` | Area skill damage fan-out (guarded `idx < MAX_USER`) | `Code/TMSrv/Server.cpp:5062` |
| mob AI aura attack | Self-injection | `ProcessClientMessage(idx, &sm, TRUE)` | Aura/Bestial damage fan-out (guarded `idx < MAX_USER`) | `Code/TMSrv/Server.cpp:5371` |
| party logic (`RemoveParty`-adjacent path) | Self-injection | `ProcessClientMessage(partyconn, &sm, TRUE)` | Automatic party acceptance (`_MSG_AcceptParty`) | `Code/TMSrv/Server.cpp:7464` |

Note: `Code/DBSrv/Server.cpp:1339` defines a **different** function with the same name (`ProcessClientMessage(int, char*)`, two parameters, gated on `FLAG_GAME2DB`) for the DB server. It is a separate build (`DBSrv.vcxproj`) with no linkage to this component and is excluded from this analysis; it is mentioned only to prevent name confusion.

### 5.3 External dependencies

| Dependency | Kind | Version / identity | Used for | Location |
| --- | --- | --- | --- | --- |
| Winsock 2 (ws2_32) | OS library | Win32 | Async socket notifications that ultimately invoke the dispatcher | `Code/TMSrv/Server.cpp:3919`, `Code/CPSock.cpp` |
| MSVC CRT | Runtime | v143 toolset, MultiByte charset | `sprintf`, `strlen`-family via handlers | `Code/TMSrv/TMSrv.vcxproj` |
| Win32 GDI/user32 | OS library | Win32 | `SetWindowText` inside `Log` (dispatcher's log path) | `Code/TMSrv/Server.cpp:6725` |
| Log file sink `fLogFile` | File system | opened by `StartLog` | BR-01 and BR-08 audit trail | `Code/TMSrv/Server.cpp:6722` |

The component has **no** DB, no SQL, no HTTP, no third-party library, and no configuration file dependency. It never reads `gameconfig.txt`, `localip.txt` or any settings file.

### 5.4 Dependency direction and cycles

```
Winsock/MainWndProc ──> CPSock::ReadMessage ──> ProcessClientMessage ──> 58 Exec_MSG_* handlers
                                                      │                        │
                                                      ├──> Log ────────────────┤
                                                      └──> AddCrackError ──────┘
                                                                  │
                                          handlers ──> SendFunc ──┴──> pUser[].cSock.SendMessageA
                                          handlers ──> Basedef (rules), GetFunc (derivations)
```

No cycle passes through the dispatcher itself: it is called *by* `Server.cpp` and calls *into* the handler family, and it never receives a callback. The one structural loop is external to it and created by BR-09: `Server.cpp` (mob AI) calls the dispatcher, which calls `Exec_MSG_Attack`, which computes damage against `pMob[]`, whose results the AI loop later reads - a runtime feedback loop through shared globals, not through the component's interface.

---

## 6. Afferent and Efferent Coupling

### 6.1 Measurement method

For this C/C++ codebase, coupling is measured at translation-unit granularity (the paradigm has no classes; the "components" are the free function, the contract header, and the handler family):

- **Afferent (Ca)** = number of distinct `.cpp`/`.h` files in `Code/` that reference the component's exported names. Measured by include-graph (`#include "ProcessClientMessage.h"`) and, for the dispatch function specifically, by call-site grep `ProcessClientMessage(`.
- **Efferent (Ce)** = number of distinct internal components reached through direct `#include` plus direct call targets (system headers and CRT excluded). Measured from `ProcessClientMessage.cpp:19-36` includes and from the 58 handler identifiers.

### 6.2 Component-level coupling

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Critical | Evidence |
| --- | --- | --- | --- | --- |
| `ProcessClientMessage` (function) | 5 call sites in 1 file (`Server.cpp`), plus 66 TUs inheriting the declaration via the header | 14 | High | `Server.cpp:4001,4622,5062,5371,7464`; 58 handler callees + `Log` + `AddCrackError`; 9 project includes |
| `ProcessClientMessage.h` (contract header) | **66 TUs** include it (58 handler TUs + `CCastleZakum.cpp`, `CWarTower.cpp`, `imple.cpp`, `MobKilled.cpp`, `ProcessClientMessage.cpp`, `ProcessDBMessage.cpp`, `ProcessSecMinTimer.cpp`, `Server.cpp`) | 14 (transitive: Basedef, CPSock, ItemEffect, Language, CItem, Server, GetFunc, SendFunc, CMob, CUser, CNPCGene, CReadFiles, CCastleZakum, CWarTower) | High | measured include graph; header lines `:27-43` |
| `Exec_MSG_*` handler family (58 functions) | 1 each (only the dispatcher calls them) | 14 each (all include the same aggregate header) | Medium-High | `ProcessClientMessage.cpp:66-311`; architecture report rows for handler family |
| `Log` (`Server.cpp:6701`) | 498 call sites across 53 TUs; 2 direct uses here (`:48`, plus inside `AddCrackError`) | 3 (fLogFile, hWndMain, `temp`) | High | measured call-site count |
| `AddCrackError` (`Server.cpp:684`) | 44 call sites across 22 TUs; 1 direct use here (`:109`) | 4 (`pUser`, `Log`, `CharLogOut`, `SendClientMessage`) | High | measured call-site count |
| `MSG_STANDARD` / `_MSG_*` constants (`Basedef.h`) | project-wide (~all 53 TUs) | 0 | High | `Basedef.h:925-946, 1210-2159` |
| `BASE_CheckPacket` (`Basedef.cpp:6475`) | 5 call sites, **none effective** (stub returns FALSE) | 0 | None (dead) | `Basedef.cpp:6476-6595`; call sites `Server.cpp:3983`, `CPSock.cpp:547`, `Server.cpp:3898`, `DBSrv/Server.cpp:848,931` |
| `CPSock::ReadMessage` (upstream producer) | 4 call sites (2 client, 2 DB/bill) | ~5 | High | `CPSock.cpp:353`; callers `TMSrv/Server.cpp:3975`, `DBSrv/Server.cpp` |

### 6.3 Cohesion assessment

| Property | Rating | Justification |
| --- | --- | --- |
| Functional cohesion | High | One function, one job: admit-or-reject then route. Every line serves dispatch. |
| Temporal cohesion | High | The four guards run in a fixed, meaningful order that the code relies on (BR-01 before BR-02 before BR-03). |
| Logical cohesion | Medium | The 59 case groups are grouped only by "is an inbound client message"; there is no domain sub-grouping inside the switch (auth, combat, economy are interleaved by historical numbering). |
| Data cohesion (state ownership) | High | The component owns zero state; it borrows `pUser[conn].LastReceiveTime` and reads two globals. |
| Coupling to volatility | Low | Adding a message requires touching three files (Basedef.h constant, header declaration, switch case) with no compile-time link between them. |

### 6.4 Boundary observations

- The component sits precisely on the **client-trust boundary**: above it (`CPSock`) bytes are unauthenticated network input; below it (`Exec_MSG_*`) data is treated as authoritative gameplay input. What crosses the boundary is a 12-byte header plus an unchecked pointer, which is the narrowest and simultaneously weakest checkpoint in the server.
- It is **structurally central but semantically thin**: Ca is dominated by the *header* (66 TUs), not by the function (5 call sites). Any change to the header's include list perturbs the whole TMSrv build, while a change to the switch affects only the dispatcher TU - an inversion worth recording.
- The 4 self-injection call sites make the component part of the *runtime* call graph of the simulation loop, not just the network path, raising the criticality of any guard change (BR-02 and BR-01 apply to mob AI traffic too).

---

## 7. Integration Points

The dispatcher exposes **no network endpoints of its own** (per the analysis guidelines, an Endpoints section is therefore omitted). It is the terminal consumer of the client TCP endpoint opened elsewhere (`ListenSocket.StartListen(hWndMain, 0, GAME_PORT, WSA_ACCEPT)`, port 8281, `Code/Basedef.h:44`), and its "interfaces" are in-process message types (see BR-06).

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
| --- | --- | --- | --- | --- | --- |
| W2 game client | External client (TCP 8281) | All client gameplay input: auth, movement, combat, economy, crafting, social | Custom binary, `INITCODE` 0x1F11F311 handshake, per-byte obfuscation via `pKeyWord[]`, additive checksum | Fixed 12-byte `HEADER` (`Size`,`KeyWord`,`CheckSum`,`Type`,`ID`,`ClientTick`) + type-specific payload, `Size <= 8192` | Framing errors set `ErrorCode` 1/2 -> caller logs and closes the read loop (`Server.cpp:3990-3999`); dispatcher drops with log (BR-01) or silently (BR-02/04/05/10); crack counter (BR-08) |
| Winsock async notification | OS callback | Delivery of readiness/FD_READ events that trigger dispatch | Win32 message `WSA_READ` | `wParam` = socket handle, `lParam` = FD_* event + error | `GetUserFromSocket == 0` -> `closesocket` (`Server.cpp:3922-3927`); non-FD_READ -> `CloseUser` (`:3929-3936`) |
| Mob AI (combat loop) | Internal producer (self-injection) | Push synthetic `_MSG_AttackOne` for mob attacks and area/aura damage | In-process function call, `isServer = TRUE` | `MSG_Attack` struct on stack, `ClientTick = 0xE0A1ACA` (== `SKIPCHECKTICK`) | No return value; rejected injections are indistinguishable from accepted ones to the caller |
| Party manager | Internal producer (self-injection) | Push synthetic `_MSG_AcceptParty` for automatic party acceptance | In-process function call, `isServer = TRUE` | `MSG_AcceptParty`, `ID = 0`, `LeaderID = novolider` | No return value |
| `CPSock` transport | Internal upstream | Frame extraction, de-obfuscation, checksum | In-process function call | `char*` into the 128 KB `pRecvBuffer` | `ErrorCode 1` (checksum) / `2` (framing) surfaced to caller only |
| Log sink (`fLogFile` + window title) | File / Win32 GDI | Audit trail for BR-01 and BR-08 | `fprintf` + `SetWindowText` | Free text, `"[dd/mm/yyyy][hh:mm:ss] <str2> <str1>"` | `fLogFile` null-checked (`Server.cpp:6722`); no truncation guard on the 1024-byte local buffer |
| `pUser[]` session array | In-process global | Liveness stamp, crack points | Direct memory | `CUser` records (`CUser.h:97`, `:75`) | Bounds guarded only by the caller's `conn` provenance |
| DBSrv (TCP 7514) | Not used by this component | - | - | - | Outbound DB traffic originates in handlers, never in the dispatcher |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
| --- | --- | --- | --- | --- |
| Front Controller / Single Entry Point | One function receives every inbound client packet; no other client entry exists | `Code/TMSrv/ProcessClientMessage.cpp:38` | Centralize admission and routing | Correctly applied; verified by call-site grep (only `Server.cpp:4001` for the client path) |
| Message Router / Type-Tag Dispatch | `switch (std->Type)` over 64 composite constants -> 59 case groups | `:66-311` | Decouple wire tags from behaviour | Textbook static router; O(1) jump table in the Release build |
| Inbound Adapter (Ports & Adapters) | Transport-neutral `char*` message converted into handler invocation with `conn` identity | `:38-66` | Isolate the protocol from gameplay | Adapter boundary exists but leaks: handlers re-cast the same raw pointer (BR-11) |
| Command Handler | 58 `Exec_MSG_*` free functions, uniform `(int conn, char* pMsg)` signature | `ProcessClientMessage.h:74-131` | One handler per message family | Uniform contract is good; no common base/validation hook is the gap |
| Guard Clause / Fail-Fast Chain | Four sequential early returns before any work | `:42,53,59,63` | Cheap rejection of malformed/tampered/shutdown traffic | Ordered deliberately; BR-01-before-BR-02 and BR-03-before-filters are load-bearing |
| Special Case / Sentinel Object | `SKIPCHECKTICK` = 235543242 marks server-authored packets; doubles as rate-limit bypass token | `Basedef.h:172`; `ProcessClientMessage.cpp:63`; `_MSG_Attack.cpp:59-96,1730` | Single value flags "trusted timing" | Effective but untyped; a boolean in a uint field |
| Self-Dispatch / Loopback Injection | Server subsystems call the client entry point with `isServer = TRUE` | `Server.cpp:4622,5062,5371,7464` | Reuse client handlers for AI actions without a second API | Avoids handler duplication; couples the simulation loop to network-entry guards |
| Allow-List Routing (implicit) | Exhaustive case list, no `default`, full flag-bit equality | `:66-311` | Restrict client-reachable protocol to ~1/3 of declared constants | Strong security property, achieved by omission rather than by declaration |
| Header-as-Wiring-Hub (anti-pattern) | `ProcessClientMessage.h` aggregates 14 subsystem headers and declares 58 handlers | `ProcessClientMessage.h:27-43,74-131` | One include for every handler | Recorded in the architecture report as an anti-pattern; directly explains uniform Ce = 14 |
| Global State Access (implicit singleton) | `pUser[]`, `pMob[]`, `ServerDown`, `SecCounter`, `temp[4096]` reached directly | throughout | Avoid passing context | Single-threaded assumption, non-reentrant `temp` (see section 10) |
| Layered architecture (conventional) | `MainWndProc` -> transport -> dispatcher -> handlers -> `GetFunc`/`SendFunc` -> globals | project-wide | Separates I/O from gameplay | Layers are by convention only; the dispatcher respects them, handlers frequently do not |

### 8.1 Architectural decisions embedded in this component

1. **Trust is anchored to the socket, not the message.** `conn` is the only identity passed downstream (BR-12). This is the single most important decision in the component and it is what makes a client-authoritative legacy protocol survivable.
2. **The server reuses the client path for its own events** (BR-09) instead of exposing handlers as an internal API. Consequence: every future guard added to the dispatcher silently constrains mob AI and party logic.
3. **Direction enforcement is per-message, not per-flag.** `_MSG_UpdateScore` proves the flag bits are not trusted as a direction indicator; the dispatcher enumerates known-cheat message types instead (BR-08).
4. **Validation is delegated to handlers** (BR-11), and the intended global validator (`BASE_CheckPacket`) is disabled. The architecture therefore has no single point where message schemas are enforced.
5. **Unlisted message types are dropped without telemetry** (BR-10). Chosen for opacity toward attackers; cost is diagnosability.
6. **Keep-alive is a transport concern handled at the top of the gameplay layer** (BR-03/BR-04): liveness is refreshed for *any* admitted packet, and pings are consumed before routing.

---

## 9. Technical Debt & Risks

| ID | Risk Level | Component Area | Issue | Impact |
| --- | --- | --- | --- | --- |
| TD-01 | High | Payload validation (BR-11) | Dispatcher casts `char*` to `MSG_STANDARD*` with no length check; no handler validates `Size` against `sizeof(MSG_X)` | A 12-byte `_MSG_Attack` yields fabricated `SkillIndex`/`Dam[]`/coordinates read from stale buffer bytes; exploitable packet-shape class |
| TD-02 | High | Dead safeguard | `BASE_CheckPacket` (`Basedef.cpp:6475-6596`) is fully commented out and returns `FALSE`; its call sites (`Server.cpp:3983`, `CPSock.cpp:547`) are unconditional no-ops | The codebase *appears* to validate message sizes but does not; false assurance for reviewers |
| TD-03 | High | Trust token | `SKIPCHECKTICK` is a single magic number that disables combat timing checks (`_MSG_Attack.cpp:59,68,79,104,113,124,494`); protection is one `if` at `ProcessClientMessage.cpp:63` | Any bypass (future refactor, injected packet path) grants unlimited attack rate and skill bypass |
| TD-04 | Medium | Routing completeness | No `default:` label in the 64-way switch; unmatched types dropped with no log, no counter | Protocol desync and probing are invisible; operator cannot distinguish "client bug" from "attack" |
| TD-05 | Medium | Routing maintenance | 3 edit points per message (constant in `Basedef.h`, declaration in `ProcessClientMessage.h`, case in the switch) with no compile-time linkage | Omission is silent (message simply never routes); handler/constant drift is undetectable by the compiler |
| TD-06 | Medium | Threading model | Component and its entire downstream graph read/write globals (`pUser`, `pMob`, `ServerDown`, `temp[4096]`) with no synchronization; correctness depends on the single-threaded Win32 pump | Any future worker thread (e.g. DB I/O) creates data races on liveness and world state |
| TD-07 | Medium | Shared scratch buffer | `sprintf(temp, ...)` writes the process-wide `char temp[4096]` (`Server.cpp:277`) also used by `Log`, `CrackLog` and most handlers | Non-reentrant; log output corruption if any nested call reuses `temp` during the BR-01 path |
| TD-08 | Medium | Anti-cheat accounting | `pUser[conn].NumError` is a signed `int` accumulated additively against a `>= 2000000000` threshold (`Server.cpp:694`); per-violation value is 2 | Signed overflow is reachable in principle; the threshold is effectively unreachable, so the counter is a soft score only |
| TD-09 | Medium | Shutdown coupling | BR-02 gate precedes the injection path, so mob AI packets are dropped during `ServerDown >= 120`; liveness also stops refreshing | Behavioural coupling between the network gate and the simulation loop; drain relies on `CloseUser`, not graceful logout via the dispatcher |
| TD-10 | Low-Medium | Client-controlled timing | Only the exact sentinel is rejected; all other `ClientTick` values pass the dispatcher and are policed per handler (800 ms / 15 s windows, `_MSG_Attack.cpp:59,88`) | Per-handler coverage is uneven across the 58 handlers; the dispatcher offers no shared monotonic-tick policy |
| TD-11 | Low-Medium | Diagnostics | `Log` writes an unbounded formatted string into a 1024-byte local buffer with `sprintf` (`Server.cpp:6720`) | Long `AccountName`/IP-derived strings can overflow; accounts are truncated to 16 bytes upstream, limiting exposure |
| TD-12 | Low | Redundant cast | `std` and `m` are two casts of the same pointer (`ProcessClientMessage.cpp:40,44`) | Cosmetic; obscures intent in the only audit-worthy branch |
| TD-13 | Low | Performance | Ping suppression happens after full transport cost (recv + de-obfuscation + checksum) | Wasted cycles on the highest-frequency packet class; no functional impact |
| TD-14 | Low | Naming collision hazard | `Code/DBSrv/Server.cpp:1339` defines an unrelated `ProcessClientMessage(int, char*)` | Confuses cross-server code reading and IDE navigation; no runtime impact |
| TD-15 | Low | Magic constants | `ServerDown >= 120`, `conn > 0`, `AddCrackError(conn, 2, 91)`, `0xE0A1ACA` at `Server.cpp:4600` are unexplained literals | Intent is only recoverable from the shutdown timer and combat handler; high knowledge-loss risk |
| TD-16 | Informational | Dead debug path | `_PACKET_DEBUG` block (`Server.cpp:3980-3988`) is compiled only in Debug configs; combined with TD-02 it can never report anything | Debug builds provide no extra packet validation |

No recommendations are made; the table records observed conditions only.

---

## 10. Test Coverage Analysis

### 10.1 Result: no tests exist

**There is no automated test of any kind for the Client Message Dispatcher, and no test infrastructure anywhere in the repository.** The following searches were executed over the full project root, excluding `.agents`, `.codegraph`, `.opencode` and `openspec`:

| Search | Command / Tool | Result |
| --- | --- | --- |
| Test-named files (any extension) | recursive file-name scan for `test`, `spec`, `gtest`, `catch2`, `mock` | **0 files** |
| Glob for test artefacts | `**/*{test,TEST,Test}*` | **No files found** |
| Unit-test framework references | grep for `assert(` across all `Code/**/*.cpp` | **0 occurrences** |
| Build targets | `W2PP Code Project.sln` project list | 2 projects only: `DBSrv` (`Code/DBSrv/DBSrv.vcxproj`), `TMSrv` (`Code/TMSrv/TMSrv.vcxproj`). No test project. |
| Build system | glob `**/*.{sln,vcxproj,filters,user}` | `W2PP Code Project.sln`, `Code/TMSrv/TMSrv.vcxproj(+.filters/.user)`, `Code/DBSrv/DBSrv.vcxproj(+.filters/.user)`, `Code/ClientPatch_v7662/ClientPatch_v7662.vcxproj.user`; **no CMakeLists, no Makefile, no test harness project** |
| CI configuration | `Test-Path .github` | **False** (no workflows, no CI test stage) |
| Fixtures / captures / golden data | recursive scan for `test|spec|mock` names | **0 files** |

### 10.2 Coverage table

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
| --- | --- | --- | --- | --- |
| `ProcessClientMessage` (dispatch function) | 0 | 0 | 0% automated | No assertions exist anywhere in the codebase; behaviour is verified only by manual client play and by reading `fLogFile` output |
| BR-01 ID range gate | 0 | 0 | 0% | Only observable through `err,packet ...` log lines in production |
| BR-02 shutdown gate | 0 | 0 | 0% | Exercised only by manual GM `reboot` / `delayreboot` (`imple.cpp:651,655`) or the menu item `IDC_REBOOT` (`Server.cpp:4094`) |
| BR-03 liveness refresh | 0 | 0 | 0% | Exercised only by the 720-tick idle disconnect observed in logs (`Server.cpp:4317`) |
| BR-04 `_MSG_Ping` drop | 0 | 0 | 0% | No assertion that a ping is not routed; verified structurally (no `case _MSG_Ping`) |
| BR-05 `SKIPCHECKTICK` rejection | 0 | 0 | 0% | Highest-risk rule in the component, zero tests |
| BR-06 routing table (64 constants) | 0 | 0 | 0% | No table-driven test; coverage of the 58 handlers is unknown |
| BR-07 alias families | 0 | 0 | 0% | Alias equivalence (`Attack` vs `AttackOne`) unverified |
| BR-08 `_MSG_UpdateScore` penalty | 0 | 0 | 0% | Penalty accumulation and 2e9 threshold untested |
| BR-09 `isServer == TRUE` injection | 0 | 0 | 0% | Exercised implicitly by every mob attack in live play |
| BR-10 unmatched type drop | 0 | 0 | 0% | Cannot be observed at all (no log) |
| BR-11 missing payload validation | 0 | 0 | 0% | No fuzzing, no malformed-packet tests; the one validator is dead code (TD-02) |
| BR-12/BR-13 identity and flag routing | 0 | 0 | 0% | No spoofing tests |

### 10.3 Compensating mechanisms observed (not tests)

| Mechanism | What it provides | Location | Limitation |
| --- | --- | --- | --- |
| `_PACKET_DEBUG` diagnostic block | Logs `Type`/`Size` of packets that fail `BASE_CheckPacket` | `Code/TMSrv/Server.cpp:3980-3988` | Debug-config only; `BASE_CheckPacket` returns `FALSE` so it never fires (TD-02) |
| `Log("err,packet Type:%d ID:%d Size:%d KeyWord:%d", ...)` | Forensic record of rejected header fields | `ProcessClientMessage.cpp:46-48` | Only fires for BR-01 (ID out of range); no other rejection is logged |
| `AddCrackError` accumulator | Soft cheat score with a hard logout threshold | `Server.cpp:684-707` | Only `_MSG_UpdateScore` contributes from this component; threshold effectively unreachable (TD-08) |
| `APIENTRY`-level transport checks | `INITCODE`, `Size` bounds, frame completeness, checksum comparison | `Code/CPSock.cpp:366-411, 455-464` | Checksum mismatch is logged, not enforced; no payload schema |
| Manual GM tooling | `ProcessImple` commands (`reboot`, `delayreboot`, `generate`, `create`, `gift`, ...) | `Code/TMSrv/imple.cpp:647+` | Operational, not verification |

### 10.4 Risk statement

The component is the only admission point for 58 handlers that trust raw pointers (BR-11), it holds the sole check protecting the combat timing model (BR-05), and it has zero automated verification, zero assertions in the entire codebase, and a disabled schema validator. Every business rule in section 3 is currently protected by one `if` statement and manual observation. Test gaps for BR-01, BR-05, BR-06 and BR-11 are the highest-consequence gaps in the analysed component.

---

## 11. Appendix - Analysis Ledger

### 11.1 Files read (relative paths)

| File | Role |
| --- | --- |
| `Code/TMSrv/ProcessClientMessage.cpp` | Primary component (313 lines, read in full) |
| `Code/TMSrv/ProcessClientMessage.h` | Contract header (133 lines, read in full) |
| `Code/TMSrv/Server.cpp` (sections) | Callers 3919-4040, 4080-4114, 4295-4334, 4590-4639, 5040-5079, 5345-5379, 7430-7474; services 684-743, 6695-6782 |
| `Code/TMSrv/ProcessSecMinTimer.cpp:30-160` | `ServerDown` state machine |
| `Code/TMSrv/imple.cpp:630-690` | GM reboot commands |
| `Code/TMSrv/_MSG_Attack.cpp` (grep + excerpts) | `ClientTick`/`SKIPCHECKTICK` downstream contract (27 occurrences) |
| `Code/TMSrv/_MSG_Action.cpp`, `_MSG_Buy.cpp`, `_MSG_Sell.cpp`, `_MSG_Trade.cpp`, `_MSG_Quest.cpp`, `_MSG_War.cpp` (excerpts) | Handler precondition conventions (BR-11/BR-12 evidence) |
| `Code/TMSrv/CUser.h:45-97` | `Mode`, `LastReceiveTime`, `NumError`, `LastAttackTick` field offsets |
| `Code/TMSrv/Server.h` | `Log`/`GetEmptyUser`/`AddCrackError` declarations, `temp[4096]`, `ServerDown` extern |
| `Code/Basedef.h` (sections 30-229, 880-960, constants grep) | `MAX_USER`, `MAX_MOB`, `SKIPCHECKTICK`, `_MSG` macro, flags, `MSG_STANDARD`, 65 message constants |
| `Code/Basedef.cpp:6475-6596` | `BASE_CheckPacket` stub proof |
| `Code/CPSock.h:30-100` | `HEADER`, `INITCODE`, buffer and message size limits |
| `Code/CPSock.cpp:300-467` | `Receive`, `ReadMessage` framing/de-obfuscation/checksum |
| `Code/TMSrv/TMSrv.vcxproj` | Toolset v143, MultiByte, `_PACKET_DEBUG` (Debug only), `Optimization=MaxSpeed` (Release) |
| `W2PP Code Project.sln` | Project inventory (DBSrv, TMSrv only) |
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | Component role context (Ca ~69 / Ce ~14) |
| `README.md` | Project self-description (decompilation of a Polly's Server release, Visual Studio 2015 era targets) |

### 11.2 Measurements (reproducible counts)

| Metric | Value | Method |
| --- | --- | --- |
| Component size | 313 lines, 1 function | file read |
| `case _MSG_*` labels | 64 | regex `^\s*case\s+_MSG_` over the file |
| Distinct case groups | 59 | manual enumeration of the switch |
| Unique `Exec_MSG_*` identifiers referenced | 58 | regex `Exec_MSG_\w+`, deduplicated |
| `Exec_MSG_*` declarations in header | 58 | `ProcessClientMessage.h:74-131` |
| `default:` labels | 0 | regex `default:` over the file |
| TUs including the contract header | 66 | raw-content match for `#include "ProcessClientMessage.h"` |
| `ProcessClientMessage(` call sites (TMSrv) | 5 (1 client, 4 `isServer = TRUE`) | regex over `Code/TMSrv` |
| `Log` call sites / files | 498 / 53 | regex with negative lookbehind over `Code/**/*.cpp` |
| `AddCrackError` call sites / files | 44 / 22 | regex over `Code/**/*.cpp` |
| `ClientTick` references in `_MSG_Attack.cpp` | 27 | regex over file |
| Test files in repository | 0 | recursive name scan + glob |
| `assert(` occurrences in `Code/**/*.cpp` | 0 | regex over all `.cpp` |

### 11.3 Explicit ambiguities recorded

1. **Shutdown timing units.** `ProcessSecTimer` is bound to `SetTimer(hWndMain, TIMER_SEC, 500, NULL)` (`Server.cpp:3631`) yet increments a counter whose thresholds (720, 1440) and name imply seconds. Whether `ServerDown == 120` is ~120 s or ~60 s depends on the intent of the original author; BR-02 states the predicate exactly (`>= 120`) and does not assert a wall-clock duration.
2. **`KeyWord` / `CheckSum` semantics.** `CPSock::ReadMessage` derives `KeyWord = pKeyWord[iKeyWord * 2]` and de-obfuscates bytes using `pKeyWord[rst * 2 + 1]`; the table's generation is outside the read files. BR-01's log line captures both fields but their cryptographic meaning is unverified.
3. **`ServerDown` negative-phase semantics for `delayreboot`.** `imple.cpp:655-658` sets `ServerDown = -1 * ServerIndex * (ival1 * 2)` with a clamp to `[1, -1000]`; the exact stagger arithmetic (whether `ival1` is seconds, minutes or shards) is not documented in the file.
4. **Whether `_MSG_Ping` is client-periodic or client-on-idle.** No client source is present in the repository (`ClientPatch_v7662` is a patch DLL project with only a `.user` file tracked); only the server-side consequence (BR-03/BR-04) is verifiable.
5. **Handler-side completeness of BR-11.** This analysis establishes that the dispatcher validates nothing; the 58 handlers were sampled (6 files read in detail) rather than exhaustively audited for missing preconditions.
