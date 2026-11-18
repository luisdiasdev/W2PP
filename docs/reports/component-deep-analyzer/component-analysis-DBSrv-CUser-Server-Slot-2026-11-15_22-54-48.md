# Component Deep Analysis Report — DBSrv CUser Server Slot

| Attribute | Value |
|---|---|
| Component name | DBSrv CUser Server Slot (`CUser`) |
| Primary location | `Code/DBSrv/CUser.h`, `Code/DBSrv/CUser.cpp` |
| Declared in | `Code/DBSrv/Server.h:104-107` (globals `TempUser`, `pUser[MAX_SERVER]`, `pAdmin[MAX_ADMIN]`) |
| Platform | Win32, MSVC (`PlatformToolset v143`), WSAAsyncSelect-based single-threaded server |
| Analysis date | 2026-11-15 22:54:48 |
| Analysis scope | `Code/DBSrv/**` (all files), plus the shared primitives the component composes (`Code/CPSock.*`, `Code/Basedef.h`) |
| Excluded from analysis | `.agents`, `.codegraph`, `.opencode`, `openspec` (per `ignore-folders`) |
| Out of scope by instruction | `Code/TMSrv/CUser.cpp` (separate component report) |

---

## 1. Executive Summary

`CUser` is the **connection-slot entity** of DBSrv. DBSrv is the account/character persistence server of W2PP: it listens on TCP `7514` for game servers (`TMSrv`) and TCP `8895` for the NP administrative tool (`Basedef.h:45-46`). Every accepted peer — whether a game server or an admin console — is represented by exactly one `CUser` instance placed in one of two fixed arrays: `pUser[MAX_SERVER]` (10 game-server slots) or `pAdmin[MAX_ADMIN]` (10 admin slots) (`Server.cpp:82-83`).

The class itself is extremely small: 10 data members and one method (`AcceptUser`) in 52/65 lines respectively. It carries **slot identity (IP), a two-value lifecycle state (`Mode`), the transport object (`cSock`), the per-server session counter (`Count`), an admin authorization level (`Level`), a per-connection encode challenge (`Encode1`, `Encode2`), the authenticated admin account name (`Name`), and a deferred account-disable handle (`DisableID`, `Year`, `YearDay`)**. All behavioural logic — admission policy, duplicate-slot eviction, encode challenge issuance and verification, privilege derivation, deferred ban execution — lives in the *consumers* of the entity (`Server.cpp`, `CFileDB.cpp`, `CReadFiles.cpp`, `CRanking.cpp`), which reach into the public fields of the objects directly. This split is the single most important architectural fact about the component: **`CUser` is an anemic record, and its invariants are enforced by convention in five other translation units, not by the class.**

Key findings:

1. **Two-state lifecycle.** `Mode` has exactly two values (`USER_EMPTY 0`, `USER_ACCEPT 1` — `CUser.h:26-27`). There is no "authenticating", "bound", or "saving" state, unlike the multi-stage session machine of the TMSrv counterpart. `Mode` is referenced 26 times across `Code/DBSrv` — 7 writes and 19 occupancy reads (13 in `CFileDB.cpp`, 5 in `Server.cpp`, 1 in `CReadFiles.cpp`) — making `Mode != USER_EMPTY` the sole "slot is occupied" predicate in the subsystem.
2. **Slot identity is the peer IP, not the connection.** Admission resolves a slot by scanning `pUser[i].IP == TempUser.IP` (`Server.cpp:1086-1093`) and `pAdmin[i].IP == TempUser.IP` (`Server.cpp:968-975`). `IP` is deliberately **never cleared** on disconnect (`Server.cpp:890-900`, `810-821`), which makes the ip→slot mapping sticky and provides the entire reconnection mechanism. Because the array is only 10 entries, address churn permanently consumes slot namespaces (`Server.cpp:1086-1118`).
3. **Two-factor-ish admin admission.** Admin slots require (a) IP allow-list membership from `Admin.txt` — re-read from disk on *every* admin accept (`Server.cpp:953`, `353-391`) — and (b) an account-level login against `Encode1/Encode2` plus password (`Server.cpp:1680-1736`). The challenge is `rand() % 10000` twice, sent in clear on accept (`Server.cpp:1052-1065`).
4. **Privilege is derived from game data.** Admin level is `max character level across the account's 4 slots − 1000` (`Server.cpp:1713-1726`). There is no separate role store. Levels ≤ 0 authenticate successfully but cannot use privileged commands; thresholds are `> 0`, `>= 2`, and `> 2` depending on command (see BR-13).
5. **Asymmetric cleanup.** FD_CLOSE and protocol-error paths clear `Mode` for admin slots (including `Name[0]`) but the receive-failure path clears nothing; for game-server slots the receive-failure path resets buffer cursors but not `Mode`. A slot can therefore survive in a half-occupied, unusable state (`Server.cpp:825-832`, `904-916`).
6. **No tests exist anywhere in the project** (evidence in Section 14). All behaviour is unverified by automation.

Architectural role confirmed against `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:169`: "One slot per connected game server or admin tool (IP, mode, encode challenge, admin level)", afferent ≈ 6, efferent 2. This analysis confirms efferent = 2 (`CPSock` composition, `Basedef.h` constants/types) and measures afferent = 5 translation units.

---

## 2. Data Flow Analysis

### 2.1 Game-server connection (TCP 7514)

```
1. TCP SYN accepted by kernel; Winsock posts WSA_ACCEPT (WM_USER+3) to hWndMain
2. MainWndProc: dispatch case WSA_ACCEPT                                  Server.cpp:1068
3. WSAGETSELECTERROR(lParam) guard -> log + break on error                Server.cpp:1070
4. CUser::AcceptUser(ListenSocket.Sock, WSA_READ)    [staging slot = TempUser]
   4a. accept()                                                          CUser.cpp:44
   4b. WSAAsyncSelect(tSock, hWndMain, WSA_READ, FD_READ|FD_CLOSE)       CUser.cpp:49
   4c. cSock.Sock / nRecvPosition / nProcPosition / nSendPosition reset  CUser.cpp:55-58
   4d. IP = peer address (network order), Mode = USER_ACCEPT             CUser.cpp:60-62
5. Slot resolution phase 1: linear scan pUser[i].IP == TempUser.IP        Server.cpp:1086
6. Slot resolution phase 2 (only if phase 1 missed):
   scan g_pServerList[ServerIndex][1..10] for the peer IP string;
   User = (whitelist index - 1)                                          Server.cpp:1095-1118
7. Duplicate check: if pUser[User].Mode != USER_EMPTY
   -> close new socket, clear TempUser.Mode, close old slot, Mode=0     Server.cpp:1119-1136
8. Subnet admission: first 3 octets of peer IP must equal LocalIP[0..2]
   -> otherwise close socket, log "outer ethernet IP"                    Server.cpp:1150, 1178-1188
9. Slot commit: IP, Mode, cSock.Sock, nRecvPosition, nProcPosition,
   nSendPosition copied from TempUser (INITCODE flag is *not* copied)     Server.cpp:1152-1157
10. Post-admission priming: _MSG_DBSetIndex (server index, Sapphire,
    slot id) via CFileDB::SendDBSignalParm3                              Server.cpp:1159
11. Priming loop over 65536 guild ids: GuildInfo citizen -> SendGuildInfo;
    g_pGuildWar -> _MSG_War; g_pGuildAlly -> _MSG_GuildAlly              Server.cpp:1161-1171
12. If TransperCharacter: _MSG_TransperCharacter                         Server.cpp:1173-1174
13. Steady state: WSA_READ events -> GetUserFromSocket(wParam) -> slot   Server.cpp:877-949
14. First inbound read requires 4-byte INITCODE 0x1F11F311 (CPSock::Init) CPSock.cpp:366-383
15. Each parsed HEADER validated: Type & FLAG_GAME2DB, 0 <= ID <= MAX_USER
    -> CFileDB::ProcessMessage(msg, conn)                                Server.cpp:1339-1357
16. Player session accounting: AddAccountList -> pUser[conn].Count++,
    RemoveAccountList -> Count--                                         CFileDB.cpp:2213, 2228
17. Disconnect: FD_CLOSE -> CloseSocket(), Mode = USER_EMPTY             Server.cpp:890-900
```

### 2.2 Admin-tool connection (TCP 8895)

```
1. WSA_ACCEPTADMIN (WM_USER+5) dispatched in MainWndProc                  Server.cpp:951
2. ReadAdmin() re-reads Admin.txt into pAdminIP[0..9] (hot reload)        Server.cpp:953, 353-391
3. WSAGETSELECTERROR guard                                               Server.cpp:955
4. TempUser.AcceptUser(AdminSocket.Sock, WSA_READADMIN)                  Server.cpp:962
5. Slot resolution phase 1: pAdmin[i].IP == TempUser.IP, i = 0..MAX_ADMIN
   (i <= 10 -> one out-of-bounds read past pAdmin[9])                     Server.cpp:968-975
6. Slot resolution phase 2: pAdminIP[i] == TempUser.IP (Admin.txt)        Server.cpp:977-1003
7. If no slot: close socket + log                                         Server.cpp:995-1002, 1021-1028
8. If slot occupied: close new socket, close live slot, Mode = EMPTY,
   break (last-connection-wins)                                           Server.cpp:1005-1019
9. Slot commit: Level = -1 (unauthenticated sentinel), DisableID = 0,
   IP, Mode, Name[0] = 0, cSock.Sock + 3 cursors                          Server.cpp:1036-1044
10. Challenge: Encode1 = rand()%10000, Encode2 = rand()%10000             Server.cpp:1052-1053
11. Send MSG_NPIDPASS{Type=_MSG_NPReqIDPASS, ID=0, Encode1, Encode2}      Server.cpp:1055-1065
12. NP client replies _MSG_NPIDPASS{Account, Encode1, Pass, Encode2}
    -> WSA_READADMIN -> GetAdminFromSocket -> ProcessAdminMessage         Server.cpp:797-875, 863
13. Envelope check: Type & FLAG_NP2DB, 0 <= ID < MAX_DBACCOUNT            Server.cpp:1467-1477
14. Challenge echo check; Level != -1 duplicate check                     Server.cpp:1686-1690
15. Account file read; blocked/defective prefixes rejected; password
    strncmp over ACCOUNTPASS_LENGTH                                       Server.cpp:1692-1711
16. maxlevel = max(BaseScore.Level of 4 char slots); require >= 1000      Server.cpp:1713-1722
17. Level = maxlevel - 1000; DisableID = 0; Name = account               Server.cpp:1724-1729
18. Privileged commands gated by Level thresholds                        Server.cpp:1611,1640,1742,1852,1929,1959
19. Deferred ban: _MSG_NPDisable sets DisableID + Year/YearDay and asks
    the game server to save+quit; CFileDB calls DisableAccount() later    Server.cpp:1949-1952; CFileDB.cpp:1108-1120, 1261-1272
20. Disconnect: FD_CLOSE -> CloseSocket(), Mode = EMPTY, Name[0] = 0      Server.cpp:810-821
```

### 2.3 Data ownership notes

* The staging slot `TempUser` (`Server.cpp:80`) is a **single shared object** reused for every accept on both listeners. Because it is a `CUser` with an embedded `CPSock`, it owns 256 KB of heap buffers (`CPSock.cpp:62-73`, 128 KB send + 128 KB receive) allocated at static-initialization time.
* Slot commit copies `Sock` and the three buffer cursors but **not** `cSock.Init` and not the buffer contents (`Server.cpp:1041-1044`, `1154-1157`). `Init` therefore remains `0` on the destination slot, which is what makes the 4-byte `INITCODE` handshake mandatory on every connection.
* `TempUser.cSock.Sock` is never zeroed after a successful handoff, so the staging slot and the committed slot briefly alias the same `SOCKET` handle (see the slot-handoff risk in Section 13).

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---|---|---|---|
| BR-01 | State model | A slot is either `USER_EMPTY` (0) or `USER_ACCEPT` (1); `Mode != 0` means occupied | `Code/DBSrv/CUser.h:26-27` |
| BR-02 | Initialization | Constructor defaults: `Mode=0`, `IP=0`, `Count=0`, `DisableID=0`, `Level=-1`; `Name`, `Encode1/2`, `Year`, `YearDay` are left uninitialized | `Code/DBSrv/CUser.cpp:26-33` |
| BR-03 | Transport binding | Accept configures socket + async events (`FD_READ|FD_CLOSE`), zeroes the 3 cursors, records peer IP, sets `Mode=USER_ACCEPT` | `Code/DBSrv/CUser.cpp:39-65` |
| BR-04 | Admission (game) | A game server is admitted only if its IP is in the local subnet AND appears in `serverlist.txt` for the configured server group | `Code/DBSrv/Server.cpp:1086-1118`, `1150`, `1178-1188` |
| BR-05 | Slot identity | Slot lookup is keyed on peer IPv4 address; the IP is retained after disconnect, making slots sticky per address | `Code/DBSrv/Server.cpp:1086-1093`, `890-900` |
| BR-06 | Duplicate/eviction | A second connection for an already-occupied slot terminates both the live session and the new connection (no takeover) | `Code/DBSrv/Server.cpp:1119-1136`, `1005-1019` |
| BR-07 | Capacity | At most 10 concurrent game-server slots and 10 concurrent admin slots, statically sized | `Code/Basedef.h:48`, `58`; `Server.cpp:82-83` |
| BR-08 | Priming (game) | On admission the slot is sent server index/Sapphire, all guild data, all war/ally data, and the transfer flag | `Code/DBSrv/Server.cpp:1159-1174` |
| BR-09 | Disconnect semantics | `FD_CLOSE` frees the slot (close socket + `Mode=0`); a receive failure only resets cursors (game) or only logs (admin) | `Code/DBSrv/Server.cpp:890-900`, `904-916`, `825-832` |
| BR-10 | Socket→slot resolution | Inbound events are mapped to slots by linear search on `cSock.Sock`; unknown handles are closed and logged | `Code/DBSrv/Server.cpp:2169-2189`, `799-808`, `879-888` |
| BR-11 | Session accounting | `Count` = number of player accounts currently logged in through that game server; maintained on add/remove of the account cache | `Code/DBSrv/CFileDB.cpp:2213`, `2228` |
| BR-12 | Admission (admin) | Admin slot requires IP allow-list membership (`Admin.txt`, hot-reloaded) or a retained IP; then an account challenge login | `Code/DBSrv/Server.cpp:953`, `968-1003`, `353-391` |
| BR-13 | Encode challenge | `Encode1`/`Encode2` are independently random `0..9999` values issued on accept and must be echoed exactly; mismatch destroys the slot | `Code/DBSrv/Server.cpp:1052-1065`, `1686-1687` |
| BR-14 | Auth state machine | `Level == -1` means unauthenticated; a non-negative `Level` means authenticated and blocks re-authentication on the same slot | `Code/DBSrv/Server.cpp:1036`, `1689-1690` |
| BR-15 | Credential policy | Account file must exist; password must match 16 bytes; `AccountPass[0]` of `_` or `@` blocks authentication; requires a character of level ≥ 1000 | `Code/DBSrv/Server.cpp:1692-1722` |
| BR-16 | Privilege derivation | Admin level = (highest character level on the account) − 1000; per-command thresholds `>0`, `>=2`, `>2`; two commands have no threshold | `Code/DBSrv/Server.cpp:1724-1726`, `1481`, `1982` |
| BR-17 | Deferred ban | A ban of a logged-in account is deferred: request save+quit, remember `DisableID`/`Year`/`YearDay`, apply ban at logout | `Code/DBSrv/Server.cpp:1939-1952`; `CFileDB.cpp:1108-1120`, `1261-1272`; `Server.cpp:1359-1417` |
| BR-18 | Idempotent ban/enable | Banning an already-banned account returns state `"1"`; enabling an unbanned account returns the stored password | `Code/DBSrv/Server.cpp:1382-1388`, `1442-1448` |
| BR-19 | Fan-out targeting | Broadcast traffic iterates slots and skips `Mode == USER_EMPTY` or `Sock == 0`; single-target traffic indexes directly | `Code/DBSrv/Server.cpp:1614-1635`; `CFileDB.cpp:1595-1620` |
| BR-20 | Packet admission (game) | Every game→DB packet must carry `FLAG_GAME2DB` and a slot id in `0..MAX_USER` before dispatch | `Code/DBSrv/Server.cpp:1343-1354` |
| BR-21 | Packet admission (admin) | Every NP→DB packet must carry `FLAG_NP2DB` and an id in `0..MAX_DBACCOUNT`, checked before any handler (including pre-auth) | `Code/DBSrv/Server.cpp:1467-1477` |

---

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Binary slot state model

**Overview**

`CUser` models the lifecycle of a peer connection with a single integer `Mode` that has exactly two legal values, `USER_EMPTY` (0) and `USER_ACCEPT` (1), defined in `CUser.h:26-27`. Every consumer treats `Mode != USER_EMPTY` as the authoritative "this slot is occupied" test.

**Detailed description**

The DBSrv slot has no intermediate states because DBSrv performs no per-connection handshake that would need one: a game server is trusted as soon as its IP passes the allow-list and subnet checks, and an admin tool is trusted at the *slot* level as soon as it connects (its *authorization* is tracked separately in `Level`). Consequently the class exposes no `Bind`, `Login`, `Wait`, or `Quit` transition methods — only `AcceptUser`, which unconditionally writes `Mode = USER_ACCEPT` after a successful `accept()` and `WSAAsyncSelect` (`CUser.cpp:62`).

The occupied test appears at 19 read sites. The most behaviourally significant are: the duplicate-slot guards in both accept handlers (`Server.cpp:1007`, `1121`), the fan-out guards that prevent writing to dead slots during guild/war/guild-ally replay and admin broadcasts (`CFileDB.cpp:379`, `411`, `445`, `470`, `499`, `583`, `603`, `796`, `1595`, `1604`, `1616`, `2086`, `2384`), the population-status generator (`CReadFiles.cpp:73`), the weekly guild fame reset (`Server.cpp:2074`), the generic free-slot helper (`Server.cpp:2195`), and the notice fan-out (`Server.cpp:1616`). Because the same field is also compared to the literal `0` in several places (`Server.cpp:1132`, `2074`), the symbolic constants and raw `0` are used interchangeably — so any future third state would silently break those comparisons.

The transition out of `USER_ACCEPT` is a direct assignment to `USER_EMPTY` performed by the *caller*, never by the class: three sites for admin slots (`Server.cpp:817`, `868`, `1016`) and one for game slots (`Server.cpp:897`). There is no `Logout`/`Reset` method, which is why each of those sites also has to remember to clear `Name[0]` (admin only) and why `IP` is intentionally *not* cleared — creating the sticky-address behaviour of BR-05. The absence of an encapsulated transition method is the root cause of the asymmetric cleanup documented in BR-09.

**Rule workflow**

```
accept() succeeds
   -> CUser::AcceptUser: Mode = USER_ACCEPT
   -> consumer commits fields into pUser[i] / pAdmin[i] (Mode copied)
   -> [occupied while Mode == 1]
   -> FD_CLOSE          : cSock.CloseSocket(); Mode = USER_EMPTY   (IP retained)
   -> receive failure   : (game) cursors reset only, Mode stays 1
                          (admin) log only, Mode stays 1
   -> handler returns FALSE: cSock.CloseSocket(); Mode = USER_EMPTY (admin)
```

---

### Business Rule: BR-02 — Constructor defaults and uninitialized members

**Overview**

`CUser::CUser` (`CUser.cpp:26-33`) initializes exactly five members: `Mode = USER_EMPTY`, `IP = 0`, `Count = 0`, `DisableID = 0`, `Level = -1`. The remaining members — `Name[16]`, `Encode1`, `Encode2`, `Year`, `YearDay` — are left indeterminate.

**Detailed description**

The initialized subset encodes the entity's three independent meanings: occupancy (`Mode`), identity (`IP`), and two nullable-domain sentinels (`Level = -1` meaning "not authenticated as an administrator", `DisableID = 0` meaning "no pending account-disable"). Initializing `Level` to `-1` is load-bearing: `ProcessAdminMessage` distinguishes "never authenticated" from "authenticated at level 0" purely by that value (`Server.cpp:1689`), so a default of `0` would let the very first packet be treated as a repeat login and be silently ignored.

The uninitialized members are safe only because every read path is preceded by a full write on the same slot: `Name` is written only in the successful `_MSG_NPIDPASS` branch (`Server.cpp:1729`) and read only in logs and in the save-escalation comparison (`Server.cpp:1891`, `1923`, `1733`); `Encode1/Encode2` are written on every admin accept before any read (`Server.cpp:1052-1053`) and read only by the challenge check (`Server.cpp:1686`); `Year`/`YearDay` are written when a ban is deferred (`Server.cpp:1951-1952`) and read in `DisableAccount(i, acc, pAdmin[i].Year, pAdmin[i].YearDay)` (`CFileDB.cpp:1119`, `1268`). For **game-server** slots, `Name`, `Encode1/2`, `Year`, `YearDay` are *never* written and never read, so the fields simply hold process-heap garbage for those 10 instances.

The empty destructor (`CUser.cpp:35-37`) means no slot ever closes its socket on destruction. Because `pUser`/`pAdmin`/`TempUser` are file-scope statics (`Server.cpp:80-83`), destruction happens at CRT teardown after `WSAStartup` resources are still live; the OS reclaims the handles. This is tolerable in a process that exits, but it means the class cannot be used as a scoped connection object.

**Rule workflow**

```
static init: 20 pUser slots + 10 pAdmin slots + TempUser constructed
   each: CPSock() allocates 128KB send + 128KB receive  (CPSock.cpp:66-67)
   each: Mode=0, IP=0, Count=0, DisableID=0, Level=-1
   each: Name/Encode1/Encode2/Year/YearDay = indeterminate
process exit: ~CUser() {} -> no closesocket(), OS reclaims
```

---

### Business Rule: BR-03 — Transport binding performed by `AcceptUser`

**Overview**

`CUser::AcceptUser(int ListenSocket, int wsa)` (`CUser.cpp:39-65`) is the component's only method. It performs the `accept()`, registers async socket events, resets the three buffer cursors, stores the peer address, and marks the slot occupied.

**Detailed description**

The function is the boundary between Winsock and the slot model. It calls `accept()` on the listening socket, and on failure returns `FALSE` without touching any member (`CUser.cpp:44-47`) — but no caller checks the return value (see Section 13, slot admit risk), so a failed accept leaves the staging slot holding the *previous* connection's `IP` and `Mode`, and the accept handler then proceeds to resolve a slot from stale data. On success it registers `WSAAsyncSelect(tSock, hWndMain, wsa, FD_READ | FD_CLOSE)`, i.e. the caller chooses which window message it will receive (the accept handlers pass `WSA_READ` for game servers and `WSA_READADMIN` for admin tools), and the slot only ever receives read and close notifications — writes are synchronous.

The failure test `if(WSAAsyncSelect(...) > 0)` is inverted relative to the Winsock contract: `WSAAsyncSelect` returns `0` on success and `SOCKET_ERROR` (`-1`) on failure, so the guard can never fire. A failure to register events is therefore undetected and the socket is committed to a slot that will never be signalled. After registration the method resets `Sock`, `nRecvPosition`, `nProcPosition`, `nSendPosition` — importantly it does **not** reset `nSentPosition` or `Init`; `Init` is reset only by `CPSock::CloseSocket` (`CPSock.cpp:90`) or by construction, which is why per-connection handshake state happens to work for freshly accepted sockets but is not explicitly enforced here.

The last two statements define the slot's identity: `IP = acc_sin.sin_addr.S_un.S_addr` (raw network-order 32-bit value, compared only for equality and decoded byte-wise for logging) and `Mode = USER_ACCEPT`. Note the local `char *tmp = inet_ntoa(...)` at `CUser.cpp:60` is computed and discarded, an artifact of decompilation. The recorded `IP` is what every downstream admission decision (BR-04, BR-05, BR-12) keys on.

**Rule workflow**

```
accept(listen) -> fail: return FALSE (members untouched: stale-slot hazard)
              -> ok  : WSAAsyncSelect(FD_READ|FD_CLOSE, wsa)  (guard is dead code)
                       cSock.Sock = tSock
                       nRecvPosition = nProcPosition = nSendPosition = 0
                       IP = peer S_addr
                       Mode = USER_ACCEPT
                       return TRUE
```

---

### Business Rule: BR-04 — Game-server admission: allow-list plus subnet

**Overview**

A game server may only occupy a `pUser` slot if (a) its address appears in `g_pServerList[ServerIndex][1..10]` (loaded from `serverlist.txt`) **or** its IP already owns a slot, and (b) the first three octets of its address equal `LocalIP[0..2]`.

**Detailed description**

Admission proceeds in two phases. Phase 1 is address stickiness: a linear scan of all 10 slots for `pUser[i].IP == TempUser.IP` (`Server.cpp:1086-1093`). Because `IP` survives disconnects, a reconnecting game server reclaims its previous slot without consulting the file. Phase 2, reached only when phase 1 misses, string-compares the dotted-quad address against `g_pServerList[ServerIndex][i]` for `i = 1..10` and, on a match, sets `User = i - 1` (`Server.cpp:1095-1118`). Index 0 of that table is the server-group/local host identity — `WinMain` compares it to the machine hostname to derive `ServerIndex` and `LocalIP` (`Server.cpp:555-569`) — so column 0 is not a game-server address. If neither phase produces an index, the socket is closed and the attempt logged (`Server.cpp:1105-1115`).

The subnet gate then requires `cIP[0]==LocalIP[0] && cIP[1]==LocalIP[1] && cIP[2]==LocalIP[2]`, i.e. a /24 match; the host octet is free, which is what allows 10 game servers on one LAN segment (`Server.cpp:1150`). The check uses only the first three octets deliberately, so a peer on the same /24 that is *not* in `serverlist.txt` still cannot obtain a slot (phase 2 already rejected it), but a peer whose listed IP differs only in the last octet passes — the two controls are not equivalent. A failure here logs `"err,wsa_accept outer ethernet IP"` and closes the fresh socket without touching any slot (`Server.cpp:1178-1188`).

Two configuration preconditions bound this rule. First, `ServerIndex` must be resolved at boot or the process exits (`Server.cpp:575-581`), so the allow-list is always anchored to a resolved group. Second, the table is populated by `BASE_InitializeServerList()` from `serverlist.txt` (`Server.cpp:529`); if the file is missing or the row is short, the entire game-server side of the component can never admit a peer. There is no runtime reload of `serverlist.txt` — unlike `Admin.txt`, which is reloaded per admin accept.

**Rule workflow**

```
peer accepted on 7514
  scan pUser[0..9].IP == peer.IP
     hit -> candidate slot = i
  miss -> for i in 1..10: g_pServerList[ServerIndex][i] == "a.b.c.d"
             hit  -> slot = i-1
             miss -> close socket, log, END
  slot occupied (Mode != 0)?
     yes -> close new + close existing, Mode=0, END
  peer[0..2] == LocalIP[0..2]?
     no  -> close socket, log "outer ethernet IP", END
     yes -> commit fields, prime slot (BR-08), END
```

---

### Business Rule: BR-05 — Slot identity is the peer IP (reconnection semantics)

**Overview**

The IP address, not the socket, is the slot key, and it is never reset by any cleanup path. Reconnection therefore resolves to the same array index, and a slot's `Count`/`Level` history is bound to an address rather than to a session.

**Detailed description**

All three cleanup paths that free a slot clear `Mode` (and, for admins, `Name[0]`) but leave `IP` intact: `Server.cpp:896-897`, `816-818`, `867-869`, and `1015-1016`. This is not an oversight — it is the mechanism by which an operator's admin console keeps index 0 across reconnects, keeping stable slot numbering in the UI and in `Admin.txt` (`idx` in `Admin.txt` maps directly to `pAdminIP[idx]`, `Server.cpp:380-387`). For game servers the same stickiness means `pUser[i]` keeps serving the same machine across `TMSrv` restarts, and the priming messages of BR-08 are re-sent on each reconnect because the commit path re-runs.

The cost is permanent namespace consumption: since the 10-slot array is also searched by IP before the allow-list, the array gradually fills with historical addresses. If a peer churns through addresses (DHCP, NAT, multi-homed host), each new address either takes a free slot or fails. When all 10 slots are attributed to addresses that are not currently connecting, a *whitelisted* peer can still be admitted through phase 2 only if its exact allow-list index resolves to an index whose `IP` matches — which is precisely the stale address stored there — so phase 2 finds `User = i-1`, then the duplicate check compares `Mode` (empty) and proceeds to overwrite. In other words, a whitelisted peer can always reclaim its own allow-list index, and the array is a cache rather than a hard reservation.

A second consequence concerns identity of the admin `Level`: on accept the level is reset to `-1` unconditionally (`Server.cpp:1036`), so reconnection always re-authenticates; but between the FD_CLOSE and the next accept, the slot retains its old non-negative `Level` while `Mode == USER_EMPTY`. During that window, the deferred-ban scanners in `CFileDB` (`CFileDB.cpp:1108-1120`, `1261-1272`) can still observe the disconnected admin and execute `DisableAccount` on its behalf — the pending ban is applied with the authority of a session that has already ended.

**Rule workflow**

```
connection A (ip X) -> slot 3, Mode=1
FD_CLOSE            -> close socket, Mode=0, IP still X
connection B (ip X) -> phase 1 hit slot 3; Mode==0 -> reuse slot 3
connection C (ip Y) -> phase 1 miss -> allow-list lookup -> slot k
admin reconnect     -> Level reset to -1 -> challenge re-issued -> re-auth
```

---

### Business Rule: BR-06 — One live session per slot; last connection wins by mutual kill

**Overview**

If a connection arrives for a slot that is already occupied, DBSrv does not perform a takeover: it closes the *new* socket and also closes the *existing* live session, leaving the slot empty.

**Detailed description**

The game-server handler requires `pUser[User].Mode == USER_EMPTY`; when the slot is busy it logs `"err wsa_accept no previous slot %d"`, closes `TempUser.cSock`, explicitly sets `TempUser.Mode = 0`, closes `pUser[User].cSock`, sets `pUser[User].Mode = 0`, and breaks out of the switch (`Server.cpp:1119-1136`). The admin handler is the same rule with fewer resets: it closes the new socket, closes the slot socket, sets `Mode = USER_EMPTY`, and breaks — notably it does **not** clear the slot's `Name[0]`, `Level`, or the pending `DisableID` (`Server.cpp:1005-1019`).

The consequence is that a duplicate connection is an availability event, not a harmless retry: a second `TMSrv` instance launched from the same host (or a stale half-open socket that the OS has not yet signalled as closed) will disconnect the legitimately running server, which then reconnects and kills the attacker's socket in turn — an oscillation that repeats for as long as both processes retry. The design implicitly assumes exactly one process per allow-listed IP and no network-level duplicate.

Because the eviction path frees the slot by setting `Mode = 0` but leaves `IP` set, the immediately following reconnect from the same address re-enters phase 1 and reuses the same index (BR-05), so the oscillation is index-stable and visible in the status window as a single flapping row. The admin variant additionally preserves the pre-eviction `Level` until the next accept resets it, so a kicked admin session's credentials remain "valid" in memory for the duration of the gap.

**Rule workflow**

```
accept for slot k
  if pUser[k].Mode != USER_EMPTY:
      close(new socket)
      TempUser.Mode = 0
      pUser[k].cSock.CloseSocket()
      pUser[k].Mode = 0            (IP, Count retained)
      break                        (no priming, no slot commit)
  else: proceed with BR-04 / BR-08
```

---

### Business Rule: BR-07 — Static capacity of 10 + 10 slots

**Overview**

`pUser` has `MAX_SERVER` = 10 elements and `pAdmin` has `MAX_ADMIN` = 10 elements, both compile-time constants (`Basedef.h:48`, `58`), and both are file-scope arrays (`Server.cpp:82-83`).

**Detailed description**

Capacity is not managed: there is no free-list, no allocator, no reclamation policy, and no configuration knob. The only capacity-related error string is `"err,accept_admin no empty"` (`Server.cpp:1032`), which is in fact unreachable — the preceding searches either produce a valid `User` or already close the socket and `break`, so `User == -1` cannot survive to that line. Consequently the true "server group is full" condition for game servers is expressed as a *whitelist miss* (`Server.cpp:1105`), not as a capacity error, and for admin tools as the allow-list miss at `Server.cpp:995`.

Each slot permanently owns a `CPSock` with two 128 KB heap buffers (`CPSock.cpp:66-67`), so the 21 slot objects (10 + 10 + staging) reserve roughly 5.4 MB at process start regardless of how many peers connect. Combined with `#define MAX_SERVERNUMBER (MAX_SERVER+1)` = 11 used both as the allow-list column count and, incorrectly, as the bound for the slot index check `User < MAX_SERVERNUMBER` (`Server.cpp:1119`), the capacity constant has three meanings that must stay numerically aligned by hand.

Related helper `GetEmptyUser()` (`Server.cpp:2191-2200`) returns the first slot with `Mode == USER_EMPTY`, but is **never called** in DBSrv (only TMSrv calls its own namesake), so "first free slot" allocation is dead code; allocation is strictly by IP/allow-list index.

**Rule workflow**

```
capacity checks:
  game  : slot index = allow-list position - 1  (0..9)
  admin : slot index = IP match, else Admin.txt idx (0..9)
  no free-list; IP stickiness consumes slots (BR-05)
  buffers: 21 slots x 256KB reserved at static init
```

---

### Business Rule: BR-08 — Post-admission priming of a game-server slot

**Overview**

Immediately after a game server is committed to a slot, DBSrv pushes the authoritative shared state to it: server index and Sapphire value, every guild record, every war entry, every alliance entry, and the character-transfer flag.

**Detailed description**

The priming block (`Server.cpp:1159-1174`) runs inside the accept handler, on the UI thread, before the handler breaks. It first sends `_MSG_DBSetIndex` with three parameters (`ServerIndex`, `Sapphire`, `User`) through `CFileDB::SendDBSignalParm3`, telling the game server which group it belongs to, what Sapphire rate it must apply, and which slot id it owns — the game server's own id is thus dictated by DBSrv rather than chosen by the peer. `Sapphire` is loaded from `Config.txt` at boot (`Server.cpp:404`).

It then iterates all 65536 possible guild ids and, for each `GuildInfo[i].Citizen`, sends a full `MSG_GuildInfo`; for each non-zero `g_pGuildWar[i]` and `g_pGuildAlly[i]` it sends `_MSG_War`/`_MSG_GuildAlly` (`Server.cpp:1161-1171`). This replays the entire guild/war/ally domain into the newly attached world server so that a restart of `TMSrv` reconverges without a manual sync. Finally, if `TransperCharacter` is set (which happens only when `redirect.txt` exists and the redirect socket connected, `Server.cpp:589-610`), it sends `_MSG_TransperCharacter`.

The block is O(65536) per connection and writes through `SendOneMessage`, which means up to ~65k × 2 potential sends on the UI thread during an accept; in practice the population of guild/war arrays is small, but the loop itself is unconditional. Because the sends go through `AddMessage`, they are silently dropped and logged individually when the slot's socket is already invalid (`CPSock.cpp:518-533`), so a peer that disappears mid-priming produces a burst of log lines rather than an error path.

**Rule workflow**

```
slot committed for User
  SendDBSignalParm3(User, 0, _MSG_DBSetIndex, ServerIndex, Sapphire, User)
  for i in 0..65535:
      if GuildInfo[i].Citizen : SendGuildInfo(User, i)
      if g_pGuildWar[i]       : SendDBSignalParm2(User, 0, _MSG_War, i, value)
      if g_pGuildAlly[i]      : SendDBSignalParm2(User, 0, _MSG_GuildAlly, i, value)
  if TransperCharacter        : SendDBSignalParm2(User, 0, _MSG_TransperCharacter, 0, 0)
```

---

### Business Rule: BR-09 — Asymmetric disconnect semantics

**Overview**

Three different failure detections exist for a slot, and they clean up different amounts of state: `FD_CLOSE` frees the slot; a failed `Receive()` on a game slot resets cursors only; a failed `Receive()` on an admin slot only logs.

**Detailed description**

`WSAGETSELECTEVENT(lParam) != FD_READ` is how the handlers detect `FD_CLOSE`. For game slots this closes the socket and sets `Mode = USER_EMPTY` (`Server.cpp:890-900`); for admin slots it additionally clears `Name[0]` (`Server.cpp:810-821`). Neither resets `Level`, `Count`, `Encode1/2`, `DisableID`, or `IP` — which is the design of BR-05 but also means a slot's authorization residue outlives its session.

A `Receive()` returning anything but `1` takes a different path. `CPSock::Receive` returns `FALSE` on `SOCKET_ERROR` **or** when the socket delivered exactly the remaining buffer space (`CPSock.cpp:341-342`) — the latter is a buffer-full condition, not necessarily a disconnect. For game slots the handler resets `nRecvPosition`/`nProcPosition` and logs three identical `"clo, server receive %d"` lines (`Server.cpp:904-916`), leaving `Mode` set and the socket open. For admin slots it logs one line and returns immediately without resetting anything (`Server.cpp:825-832`), so the next `FD_READ` continues from a receive buffer whose cursors may be inconsistent with the data. Only the `ProcessAdminMessage` return value (`FALSE`) and `FD_CLOSE` actually free an admin slot.

The practical effect is a set of "zombie" states that no other code can distinguish from healthy ones, because every other module only tests `Mode`: a game slot can be `Mode == 1` with a socket the OS already reset; an admin slot can be `Mode == 1` with a partially-filled receive buffer. Broadcast code (BR-19) will keep attempting to write to such slots, and each attempt logs `"err,add buffer invalid"` (`CPSock.cpp:527-532`) once the handle becomes invalid.

**Rule workflow**

```
event on slot
  FD_CLOSE              -> admin: close, Mode=0, Name[0]=0
                           game : close, Mode=0
  Receive() != 1        -> admin: log only (slot stays occupied)
                           game : reset recv cursors, log, slot stays occupied
  Parse error (code 1/2)-> admin: break read loop
                           game : break read loop
  handler returns FALSE -> admin: close, Mode=0, Name[0]=0
```

---

### Business Rule: BR-10 — Socket-handle to slot resolution

**Overview**

Inbound WSAMessages carry only the `SOCKET` handle in `wParam`. It is translated to a slot index by a linear scan of the corresponding array, and an unrecognised handle is forcibly closed.

**Detailed description**

`GetUserFromSocket(int Sock)` scans `pUser[0..MAX_SERVER-1]` for `cSock.Sock == Sock` and returns the index or `-1` (`Server.cpp:2169-2178`); `GetAdminFromSocket` does the same over `pAdmin[0..MAX_ADMIN-1]` (`Server.cpp:2180-2189`). Both compare a caller-supplied `int` against an `unsigned int` member via an explicit cast on the member side, so a negative `wParam` would not match any slot. The scan is O(10) and is executed on every read event for every slot — negligible in absolute terms, but it makes the slot array the de-facto connection registry, which is why slot reuse (BR-05) must not leave two slots holding the same handle.

If the scan fails, the handler calls raw `closesocket(wParam)` — not `CPSock::CloseSocket` — and logs `"err, wsa_read unregistered game server socket"` or the admin equivalent (`Server.cpp:881-888`, `801-808`). Closing the raw handle directly bypasses `CPSock` state, which is correct here (there is no slot to reset) but means the corresponding `CPSock` — if one existed from a reaped slot — keeps a stale `Sock` value. This is one of the mechanisms by which a slot can hold a handle that has been closed elsewhere: `TempUser.cSock.Sock` retains the committed handle after handoff and is never cleared, though no code path resolves `TempUser` through these lookups.

**Rule workflow**

```
WSA_READ(wParam=socket)      -> GetUserFromSocket  -> -1: closesocket, log, drop
                                                   -> i : pUser[i].cSock.Receive()
WSA_READADMIN(wParam=socket) -> GetAdminFromSocket -> -1: closesocket, log, drop
                                                   -> i : pAdmin[i].cSock.Receive()
```

---

### Business Rule: BR-11 — Per-server player session counter

**Overview**

`CUser::Count` counts how many player accounts are currently hosted by that game server, incremented when an account enters the DBSrv in-memory account cache and decremented when it leaves.

**Detailed description**

The counter is not maintained by `CUser`; it is maintained by `CFileDB::AddAccountList` and `CFileDB::RemoveAccountList`, which derive the slot from the global account index: `int conn = Idx / MAX_USER; pUser[conn].Count++` (`CFileDB.cpp:2211-2213`) and the symmetric decrement (`CFileDB.cpp:2226-2228`). Since `MAX_USER` is 1000 and `MAX_DBACCOUNT` is `MAX_USER * MAX_SERVER` = 10000 (`Basedef.h:56`, `60`), the account index space is partitioned into 10 blocks of 1000, one per game-server slot — the same partitioning used by `_MSG_NPNotice` to translate a session index back into `svr = IdxName / MAX_USER`, `id = IdxName % MAX_USER` (`Server.cpp:1652-1653`).

The value is consumed as a population metric, not as a limit: `CReadFiles::UpdateConnection` writes `-1` for empty slots and otherwise `(4 * Count / 3) + rand() % 4` to the status CSV, and tracks the session peak in `UserConnection[i]` (`CReadFiles.cpp:71-83`). `DrawConfig` prints it in the server window (`Server.cpp:148`). No admission decision anywhere compares `Count` against a maximum, so `MAX_USER` is a namespace size rather than a per-server population cap enforced by this component.

The counter has no reset on disconnect: when a game server drops, its accounts are removed through the normal logout/save path, but if the peer vanishes without saving, `Count` retains whatever it had, and the slot's status line keeps reporting a non-zero population while `Mode` may already be `USER_EMPTY` (the status writer suppresses that case by emitting `-1` for empty slots). Because `AddAccountList` guards against double-add via `pAccountList[Idx].Login`, and `RemoveAccountList` guards against double-remove, the counter is monotone-safe under repeated events.

**Rule workflow**

```
account logs in  -> AddAccountList(Idx)
                      if already Login: log, return
                      pUser[Idx/MAX_USER].Count++
                      pAccountList[Idx].Login = TRUE
account leaves   -> RemoveAccountList(Idx)
                      if not Login: return
                      pUser[Idx/MAX_USER].Count--
                      Login = FALSE, Slot = -1, File zeroed
publisher        -> UpdateConnection: Mode==0 ? "-1" : f(Count)
```

---

### Business Rule: BR-12 — Admin admission: allow-list (hot reload) plus challenge login

**Overview**

An admin slot is granted when the peer IP matches either an existing `pAdmin[i].IP` or an entry in `Admin.txt` indexed by slot number; the slot is then in the unauthenticated state until `_MSG_NPIDPASS` succeeds.

**Detailed description**

`ReadAdmin()` is called at the top of every `WSA_ACCEPTADMIN` (`Server.cpp:953`). It opens `Admin.txt`, replaces every `.` with a space, parses five integers `idx a b c d`, assembles `pAdminIP[idx] = (d<<24)|(c<<16)|(b<<8)|a`, and ignores indices outside `0..MAX_ADMIN-1` (`Server.cpp:353-391`). Because the little-endian in-memory image of `a.b.c.d` equals that arithmetic result, the comparison `pAdminIP[i] == TempUser.IP` at `Server.cpp:988` is consistent with the raw `sin_addr.S_un.S_addr` stored by `AcceptUser`. Two properties matter: the file is hot-reloaded (an operator can revoke an IP without restarting DBSrv), and entries are only ever *written*, never cleared — a line removed from `Admin.txt` leaves the previous value in `pAdminIP` until the process restarts, so revocation is not actually effective for the lifetime of the process.

Slot resolution mirrors the game path but with an off-by-one: the first loop runs `for(i = 0; i <= MAX_ADMIN; i++)` and therefore reads `pAdmin[10].IP`, one element past the array (`Server.cpp:968`). That read touches adjacent globals; if it happened to equal the peer address, `User` would become 10 and be rejected by the subsequent `User < MAX_ADMIN` test, so the defect is an out-of-bounds read rather than a writable overflow. The second loop scans `pAdminIP` and skips zero entries, and its "not found" detection is `if(i == MAX_ADMIN)`.

After the slot is resolved and confirmed empty, all identity fields are overwritten: `Level = -1`, `DisableID = 0`, `IP`, `Mode`, `Name[0] = 0`, and the socket plus three cursors (`Server.cpp:1036-1044`). Then the challenge is generated and sent (BR-13). Nothing here verifies that the peer is the NP tool rather than any other program on an allow-listed host; the protocol check happens only when a well-formed `_MSG_NPIDPASS` is parsed.

**Rule workflow**

```
WSA_ACCEPTADMIN
  ReadAdmin()  (reload Admin.txt -> pAdminIP, hot; no revocation)
  AcceptUser(AdminSocket, WSA_READADMIN)
  scan pAdmin[0..10].IP == peer.IP        (i<=10: OOB read at 10)
     else scan pAdminIP[0..9] == peer.IP
        else close new socket, log, break
  if slot occupied -> close new + close existing, Mode=EMPTY, break
  Level=-1, DisableID=0, IP, Mode, Name[0]=0, Sock+cursors copied
  Encode1/2 = rand()%10000 ; send _MSG_NPReqIDPASS
```

---

### Business Rule: BR-13 — Encode challenge issuance and verification

**Overview**

Each admin connection receives a fresh two-part challenge, `Encode1` and `Encode2`, each an independent `rand() % 10000`; the NP client must echo both values in its login packet or the slot is destroyed.

**Detailed description**

The challenge is generated in the accept handler and immediately transmitted:

```
pAdmin[User].Encode1 = rand() % 10000;                 // Server.cpp:1052
pAdmin[User].Encode2 = rand() % 10000;                 // Server.cpp:1053
MSG_NPIDPASS sm; memset(&sm,0,sizeof(sm));
sm.Type = _MSG_NPReqIDPASS;                            // (1 | FLAG_DB2NP)
sm.Size = sizeof(MSG_NPIDPASS); sm.ID = 0;
sm.Encode1 = pAdmin[User].Encode1; sm.Encode2 = pAdmin[User].Encode2;
pAdmin[User].cSock.SendOneMessage((char*)&sm, sizeof(MSG_NPIDPASS));
```

(`Server.cpp:1052-1065`; message constants at `Basedef.h:2243-2253`.) Note the struct is `MSG_NPIDPASS` while the type is `_MSG_NPReqIDPASS`, and `ID` is forced to 0 — the challenge is slot-scoped, not account-scoped.

Verification is the *first* semantic check in the `_MSG_NPIDPASS` handler and precedes all credential work: `if(pAdmin[conn].Encode1 != m->Encode1 || pAdmin[conn].Encode2 != m->Encode2) return FALSE;` (`Server.cpp:1686-1687`). A `FALSE` return propagates to the `WSA_READADMIN` loop, which closes the socket, sets `Mode = USER_EMPTY`, and clears `Name[0]` (`Server.cpp:865-872`). So a client that guesses wrong once is disconnected and must reconnect to receive a new challenge — there is no attempt counter and no delay, which means the challenge is a *session binding* token rather than a rate-limited secret: with `rand()` unseeded (`srand` is never called in DBSrv), the sequence is deterministic from process start, and the two halves are drawn consecutively from that sequence. An observer of one challenge can therefore predict subsequent ones for the process lifetime.

The check also gives the slot a useful property: a replayed login packet from a previous session fails because the challenge was regenerated on accept, and a login packet delivered to the wrong slot fails because challenges are per-slot. Both halves are compared with `||`, so a single-field collision is sufficient to pass — the effective entropy is that of one 14-bit draw, not two.

**Rule workflow**

```
on accept  : Encode1 = rand()%10000 ; Encode2 = rand()%10000
             send _MSG_NPReqIDPASS{Encode1, Encode2}
on login   : if either value differs -> return FALSE
                                       -> close socket, Mode=0, Name[0]=0, END
             else continue to BR-14 / BR-15
```

---

### Business Rule: BR-14 — Admin authentication state machine (`Level == -1`)

**Overview**

`Level` doubles as the authorization level and as the "has this slot authenticated" flag: `-1` means unauthenticated, any non-negative value means authenticated.

**Detailed description**

The sentinel is established at accept (`Server.cpp:1036`), defaulted by the constructor (`CUser.cpp:32`), and tested in exactly one place for state purposes: `if(pAdmin[conn].Level != -1) return TRUE;` (`Server.cpp:1689-1690`). A second `_MSG_NPIDPASS` on an already-authenticated slot is therefore accepted as a no-op — the slot keeps its level, its `Name`, and its identity, and no error is reported to the client. Because `Level` is `int`, there is no distinction between "authenticated as level 0" and "authenticated as level 0 with a pending ban" — the pending-ban handle lives in `DisableID` instead.

The same field then serves as the authorization gate for every privileged command, with three different threshold styles: `Level <= 0` rejects (`_MSG_NPReqAccount`, `_MSG_NPNotice` targeted, `Server.cpp:1742`, `1640`), `Level < 2` rejects (`_MSG_NPNotice` broadcast, `Server.cpp:1611`), and `Level <= 2` rejects (`_MSG_NPReqSaveAccount`, `_MSG_NPDisable`, `_MSG_NPEnable`, `Server.cpp:1852`, `1929`, `1959`). Two handlers have no gate at all: `_MSG_NPCreateCharacter` (`Server.cpp:1481`) and `_MSG_NPDonate` (`Server.cpp:1982`). Since both are reachable with `Level == -1`, they are callable by an unauthenticated peer that merely passed the IP allow-list and echoed the challenge.

State is not reset on plain disconnect: `FD_CLOSE` clears `Mode` and `Name[0]` but leaves `Level` at its authenticated value (`Server.cpp:810-821`), and the reset to `-1` only happens on the *next* accept. Between those two moments the slot is a non-empty authorization record with no socket, which is what allows the deferred-ban scanners (BR-17) to keep acting for a departed operator.

**Rule workflow**

```
accept            -> Level = -1
_MSG_NPIDPASS     -> Encode mismatch            : FALSE (destroy slot)
                     Level != -1                : TRUE  (no-op)
                     account read fails         : TRUE  (silent, Level stays -1)
                     blocked/defective/dup pass : FALSE (destroy slot)
                     success                    : Level = maxlevel - 1000, Name = Account
privileged cmd    -> Level <= 0 / < 2 / <= 2 : reject (message or silent TRUE)
FD_CLOSE          -> Mode = 0, Name[0] = 0, Level unchanged
next accept       -> Level = -1
```

---

### Business Rule: BR-15 — Credential and account-status policy

**Overview**

Authentication requires: an existing account file; a password matching all 16 bytes of `AccountPass`; an `AccountPass[0]` that is neither `_` nor `@`; and at least one character whose `BaseScore.Level >= 1000`.

**Detailed description**

After the challenge check and the duplicate-auth check, the handler reads the account through `cFileDB.DBReadAccount(&file)` using the uppercased `m->Account` (`Server.cpp:1684`, `1692-1699`). A read failure returns `TRUE`, which is silently converted into "ignore the packet" by the caller's `if(ret == FALSE)` logic — the peer stays connected with `Level == -1` and can try again, indefinitely, with no lockout, delay, or log entry. This is the only branch of the authentication path that neither authenticates nor disconnects.

Status prefixes are then checked before the password: `'_'` (documented elsewhere as "Conta Defeituosa" — defective account, `Server.cpp:1821-1825`) and `'@'` ("Conta bloqueada" — blocked, `Server.cpp:1816-1820`) both return `FALSE`, destroying the slot. `'#'` ("Conta Desativada" — deactivated) is *not* rejected here, an asymmetry with the status report path in `_MSG_NPReqAccount` (`Server.cpp:1826-1830`). The password comparison is `strncmp(m->Pass, p, ACCOUNTPASS_LENGTH)` over the full 16 bytes with no early exit — a timing-safe style comparison, though the surrounding protocol is plaintext so the property has no practical value.

The final gate is game-state-derived: the handler scans the 4 character slots and keeps the maximum `BaseScore.Level` of any character with a non-empty `MobName` (`Server.cpp:1713-1719`), then requires `maxlevel >= 1000` or returns `FALSE` (`Server.cpp:1721-1722`). A legitimate operator account with no level-1000+ character cannot log in at all. Conversely, any account whose character reaches level 1000 through normal play or through a defect becomes an admin — the credential policy and the authorization policy are the same check, evaluated against the *stored* character file rather than a role store.

**Rule workflow**

```
_strupr(Account)
if Encode1/2 mismatch            -> FALSE (disconnect)
if Level != -1                   -> TRUE  (no-op)
DBReadAccount fail               -> TRUE  (stay unauthenticated, no log)
if Pass[0] == '_' or '@'         -> FALSE (disconnect)
if strncmp(Pass, file, 12) != 0  -> FALSE (disconnect)
maxlevel = max(BaseScore.Level of non-empty char slots)
if maxlevel < 1000               -> FALSE (disconnect)
Level = maxlevel - 1000 ; DisableID = 0 ; Name = Account ; log + notify
```

---

### Business Rule: BR-16 — Privilege derivation and per-command thresholds

**Overview**

Admin authority is `Level = maxCharacterLevel − 1000`, and each command declares its own minimum level; there is no central permission table.

**Detailed description**

The derivation is a single subtraction applied in two places: the login path (`Server.cpp:1724-1726`) and the save path, which recomputes the same quantity from a *client-supplied* packet (`Server.cpp:1870-1880`). At login, `Level` becomes the account's maximum character level minus 1000, so a level-1000 character yields `Level 0`, level 1002 yields `2`, and so on. Because the maximum is taken over all four character slots, an operator only needs one high-level character on the account, and the effective rank is the highest ever stored — not a value an administrator sets explicitly.

The thresholds are inconsistent by design or by accident. `_MSG_NPNotice` to a single account requires `Level > 0`; the broadcast variant requires `Level >= 2`; account save, disable, and enable require `Level > 2` (strictly greater, so `Level == 2` cannot ban). Account query requires `Level > 0`. The character-creation handler `_MSG_NPCreateCharacter` and the donation handler `_MSG_NPDonate` have **no** level gate at all and are reachable while `Level == -1`. The save path additionally contains an escalation guard: if the submitted account's derived admin level is greater than the operator's own, the save is refused; and if equal, the save is refused unless the account name matches the operator's own name (`Server.cpp:1882-1898`), which prevents lateral takeover and self-demotion but permits self-modification.

Because `Level` is derived from data the game server itself persists, any defect that lets a player's stored `BaseScore.Level` reach 1000 grants DBSrv console authority — the trust boundary between "game state" and "administration" does not exist in this component.

**Rule workflow**

```
login      : Level = max(char[].BaseScore.Level) - 1000
commands   : _MSG_NPReqAccount      requires Level > 0
             _MSG_NPNotice (single) requires Level > 0
             _MSG_NPNotice (all)    requires Level >= 2
             _MSG_NPReqSaveAccount  requires Level > 2, plus escalation guard
             _MSG_NPDisable         requires Level > 2
             _MSG_NPEnable          requires Level > 2
             _MSG_NPCreateCharacter no gate
             _MSG_NPDonate          no gate
```

---

### Business Rule: BR-17 — Deferred account ban (DisableID / Year / YearDay)

**Overview**

Banning an account that is currently logged in cannot be done immediately (the game server owns the character file), so DBSrv records the intent in the admin slot and completes the ban when the game server reports the logout.

**Detailed description**

`_MSG_NPDisable` first checks `Level > 2`, then uppercases the account name and asks `cFileDB.GetIndex(...)` whether the account is currently in the in-memory cache (`Server.cpp:1929-1946`). If it is *not* logged in, `DisableAccount(conn, name, Year, YearDay)` runs immediately: it reads the account file, refuses if already banned (BR-18), truncates the password field's last two bytes, writes `Year`/`YearDay`, persists the file, and then re-enters `ProcessAdminMessage` with a synthetic `_MSG_NPReqAccount` to refresh the operator's view (`Server.cpp:1359-1416`). If the account *is* logged in, the handler issues `cFileDB.SendDBSavingQuit(IdxName, 1)` to force a save-and-quit, stores `pAdmin[conn].DisableID = IdxName`, `Year`, and `YearDay`, and returns — nothing is written to disk at this point (`Server.cpp:1949-1952`).

Completion happens in the persistence layer. When the game server reports `_MSG_DBNoNeedSave` (`CFileDB.cpp:1096-1122`) or finishes `_MSG_DBSaveMob` (`CFileDB.cpp:1259-1272`), `CFileDB` iterates `pAdmin[0..9]`, skips slots with `Level <= 2` or `DisableID == 0`, and calls `DisableAccount(i, acc, pAdmin[i].Year, pAdmin[i].YearDay)` for the slot whose `DisableID` equals the account index. `DisableAccount` then clears `pAdmin[conn].DisableID = 0` (`Server.cpp:1415`), completing the two-phase protocol. `DisableID` is also cleared on successful login (`Server.cpp:1727`) and on every admin accept (`Server.cpp:1037`).

The protocol has sharp edges. The stored handle is a global account index (`svr * MAX_USER + id`), so it is only meaningful while the account stays in the same slot; if the player disconnects and reconnects to a different game server before the ban completes, the pending handle points at an index that no longer describes the intended account. Because the completion scan requires `Level > 2`, an operator who disconnects between request and completion cannot complete the ban — and, per BR-14, the slot keeps its old `Level` after disconnect, so the scan may still fire for a departed operator. If the game server never reports the logout (host crash), the ban is never applied and the `DisableID` residue persists until that slot authenticates again.

**Rule workflow**

```
_MSG_NPDisable(account, Year, YearDay)
  Level <= 2                      -> return TRUE
  IdxName = GetIndex(account)
  IdxName == 0  -> DisableAccount(conn, account, Year, YearDay)  [immediate]
  IdxName != 0  -> SendDBSavingQuit(IdxName, 1)
                   DisableID = IdxName ; Year ; YearDay          [deferred]
game server completes save/logout (_MSG_DBSaveMob | _MSG_DBNoNeedSave)
  for each admin slot with Level > 2 and DisableID != 0:
      if DisableID == account index -> DisableAccount(slot, acc, Year, YearDay)
                                        DisableID = 0
```

---

### Business Rule: BR-18 — Idempotent ban / enable

**Overview**

Banning an already-banned account and enabling an already-enabled account are both no-ops that report current state instead of failing.

**Detailed description**

`DisableAccount` returns early when both `Year` and `YearDay` are non-zero, responding with `SendAdminState(conn, 0, _MSG_NPState, "1")` — the string `"1"` signals "already blocked" to the NP client (`Server.cpp:1382-1388`). `EnableAccount` returns early when both are zero, echoing the stored `AccountPass` back in `_MSG_NPState` (`Server.cpp:1442-1448`), which lets the tool display the password that the ban had masked. In the non-early-exit paths the two functions are exact mirrors: truncate the last two bytes of the password, set or clear both date fields, persist, and report.

The guard is a conjunction, so a half-written record (`Year != 0`, `YearDay == 0`) is treated as "not banned" and re-banned, overwriting the date. Since `Year`/`YearDay` are the only ban representation in the account file, this also means a ban expires only by explicit enable — there is no time comparison anywhere in the component, so the date fields are informational.

Both functions are also called outside the NP command path: `EnableAccount(-1, account)` is invoked during character-transfer completion (`Server.cpp:662`), where `conn == -1` suppresses all client responses via the `conn >= 0` guards. That guards-as-parameter pattern is the only way the component distinguishes "operator-initiated" from "system-initiated" work.

**Rule workflow**

```
DisableAccount(conn, acc, Y, D):
   DBReadAccount fail              -> _MSG_NPNotFound + text (if conn>=0)
   Year != 0 && YearDay != 0       -> _MSG_NPState "1" , return
   Pass[10..11]=0 ; Year=Y ; YearDay=D ; DBWriteAccount
   if conn>=0 -> replay _MSG_NPReqAccount ; DisableID = 0
EnableAccount(conn, acc):
   DBReadAccount fail              -> _MSG_NPNotFound + text
   Year == 0 && YearDay == 0       -> _MSG_NPState <password>, return
   Pass[10..11]=0 ; Year=0 ; YearDay=0 ; DBWriteAccount
   if conn>=0 -> _MSG_NPState <password>
```

---

### Business Rule: BR-19 — Fan-out targeting rules

**Overview**

Broadcast operations iterate the slot arrays and must skip empty or socket-less slots; single-target operations index a slot directly and do not re-validate it.

**Detailed description**

The recurring pattern is `if(pUser[i].Mode == USER_EMPTY) continue; if(pUser[i].cSock.Sock == 0) continue;` before `SendOneMessage` (`CFileDB.cpp:379-383`, `411-414`, `445-448`, `470-476`, `499-502`, `583-587`, `603-607`, `1595-1601`, `1604-1610`, `1616-1620`, `2086-2089`; `Server.cpp:1614-1621`, `2071-2075`). The two conditions are not redundant: `Mode` can be `USER_ACCEPT` with an already-closed socket (BR-09), and `CPSock::Sock` is the only reliable "usable" indicator. `AddMessage` itself re-checks `Sock <= 0` and logs `"err,add buffer invalid"` (`CPSock.cpp:527-533`), so a missed guard degrades to a log line rather than a write to a closed handle.

Single-target sends do not re-validate. `SendAdminSignal`, `SendAdminMessage`, `SendAdminParm`, `SendAdminState` (`Server.cpp:255-312`) all write to `pAdmin[svr].cSock` unconditionally — the `svr` comes from the inbound message's slot index, so it is always the current slot and is therefore occupied by construction. The same holds for `pAdmin[conn].cSock.SendOneMessage` inside command handlers. The riskier variant is `_MSG_NPNotice`'s single-account path, which derives `svr = IdxName / MAX_USER` from the account cache and then sends to `pUser[svr]` without checking `Mode`/`Sock` (`Server.cpp:1652-1674`); the bounds check `svr < 0 || svr >= MAX_SERVER || id <= 0 || id >= MAX_USER` guards the index but not slot liveness.

`CRanking` uses the third variant — a bare `if(pUser[tmsrvId].cSock.Sock)` guard before sending an individual ranking update (`CRanking.cpp:312-313`) — which is sufficient for the socket but ignores `Mode`.

**Rule workflow**

```
broadcast: for each slot: if Mode == USER_EMPTY skip
                          if Sock == 0        skip
                          SendOneMessage(...)
targeted : index derived from message/cache -> SendOneMessage (no liveness re-check)
           CPSock::AddMessage drops + logs if Sock <= 0
```

---

### Business Rule: BR-20 / BR-21 — Packet admission envelopes

**Overview**

Before any handler runs, DBSrv validates that the packet's `Type` carries the correct direction flag and that its `ID` lies inside the direction's index space.

**Detailed description**

For the game link, `ProcessClientMessage` rejects any packet where `(std->Type & FLAG_GAME2DB) == 0` or `std->ID < 0` or `std->ID > MAX_USER`, logging type/id/size/keyword and returning without dispatch (`Server.cpp:1341-1354`; `FLAG_GAME2DB = 0x0800` at `Basedef.h:936`). The upper bound is `>` rather than `>=`, so `ID == MAX_USER` (1000) passes, even though `MAX_USER` is documented as the first NPC/mob index (`Basedef.h:56`) and the account cache is indexed by `svr * MAX_USER + id`. Every accepted packet is then handed to `cFileDB.ProcessMessage(msg, conn)`, which is where per-type `GetIndex(conn, m->ID)` bounds checks live.

For the admin link, `ProcessAdminMessage` rejects packets where `(std->Type & FLAG_NP2DB) == 0` or `std->ID < 0` or `std->ID >= MAX_DBACCOUNT` (`Server.cpp:1465-1477`; `FLAG_NP2DB = 0x2000`). The identical envelope check is duplicated in `ProcessAdminClientMessage` for the outbound redirect server (`Server.cpp:626-638`), so the redirect feed and the NP tool share one validation contract.

Crucially, the envelope check is the *only* check applied to every packet; authorization is per-handler (BR-16) and is therefore absent wherever a handler forgets it. Because the check runs before the `switch`, an unauthenticated peer must still produce well-formed NP packets — but `_MSG_NPCreateCharacter` and `_MSG_NPDonate` remain reachable at `Level == -1`.

**Rule workflow**

```
game  packet -> Type & 0x0800 ? no -> log + drop
                ID in 0..MAX_USER ? no -> log + drop
                -> CFileDB::ProcessMessage(msg, conn)
admin packet -> Type & 0x2000 ? no -> log + return FALSE (=> slot destroyed)
                ID in 0..MAX_DBACCOUNT-1 ? no -> log + return FALSE
                -> switch(Type): per-handler Level gate -> handler body
```

---

## 4. Component Structure

```
Code/DBSrv/
├── CUser.h                      # Slot entity: 10 public fields, 2 mode constants, AcceptUser decl
│                                #   CUser.h:26-27   USER_EMPTY / USER_ACCEPT
│                                #   CUser.h:32-43   IP, Mode, cSock, Count, Level, Encode1, Encode2,
│                                #                  Name[16], DisableID, Year, YearDay
│                                #   CUser.h:46-49   ctor / dtor / AcceptUser
├── CUser.cpp    (65 lines)      # The entire component implementation
│                                #   :26-33   constructor (partial initialization)
│                                #   :35-37   empty destructor
│                                #   :39-65   AcceptUser (accept + WSAAsyncSelect + IP + Mode)
├── Server.h                     # Declares the slot arrays and all consumers
│                                #   :56-58   GetUserFromSocket / GetAdminFromSocket / GetEmptyUser
│                                #   :104-107 TempUser, pUser[MAX_SERVER], pAdmin[MAX_ADMIN]
├── Server.cpp   (2330 lines)    # Slot lifecycle owner: accept handlers, read/close handlers,
│                                #   ProcessAdminMessage (Level gates), ProcessClientMessage,
│                                #   ReadAdmin (Admin.txt), DisableAccount/EnableAccount,
│                                #   GetUserFromSocket/GetAdminFromSocket/GetEmptyUser
├── CFileDB.h / .cpp             # Consumes pUser/pAdmin for fan-out, cache accounting (Count),
│                                #   and deferred-ban completion (DisableID)
├── CReadFiles.h / .cpp          # Consumes Mode/Count for population publishing
├── CRanking.h / .cpp            # Consumes pUser[tmsrvId].cSock for per-server rank updates
├── DBSrv.vcxproj                # Compiles CUser.cpp (:99), includes CUser.h (:110)
└── Debug/, Release/, DBSrv/Release/   # MSBuild intermediates (out of scope)
```

**External primitives the entity composes (not part of the component, but its affordances):**

```
Code/CPSock.h                    # HEADER layout, WSA message ids, INITCODE, buffer sizes
Code/CPSock.cpp                  # ctor allocates 2 x 128KB per instance; CloseSocket resets
                                 #   Init/Sock; Receive; ReadMessage (INITCODE + checksum);
                                 #   AddMessage/SendMessageA/SendOneMessage
Code/Basedef.h                   # MAX_SERVER 10, MAX_ADMIN 10, MAX_USER 1000,
                                 #   MAX_DBACCOUNT 10000, ACCOUNTNAME_LENGTH 16,
                                 #   ACCOUNTPASS_LENGTH 16, DB_PORT 7514, ADMIN_PORT 8895,
                                 #   FLAG_GAME2DB / FLAG_NP2DB, MSG_NPIDPASS
```

**Boundary observation.** The component's *file* boundary (`CUser.h/.cpp`) contains 2% of the behaviour that its *semantic* boundary spans. All 21 business rules above are enforced in `Server.cpp`, `CFileDB.cpp`, `CReadFiles.cpp`, and `CRanking.cpp`; `CUser` itself enforces only BR-02 and part of BR-03. Any change to a `CUser` field is therefore a cross-cutting change, and any change to the slot lifecycle must be made in `Server.cpp`, not in the component's own file.

---

## 5. Dependency Analysis

### Internal Dependencies

```
CUser
 ├── CPSock                    (composition: cSock member, by value)
 │     └── winsock2 / WSAAsyncSelect / recv / send / closesocket
 └── Basedef.h                 (ACCOUNTNAME_LENGTH = 16 for Name[]; MAX_SERVER/MAX_ADMIN at the
                                array declarations in Server.h)

Consumers (afferent):
 Server.cpp
   ├── CUser::AcceptUser                :962, 1076
   ├── pUser[]  field access            : 99 references (IP, Mode, cSock, Count, Level, Name, ...)
   ├── pAdmin[] field access            (same file)
   └── TempUser                       :80, 962-1065, 1076-1188
 CFileDB.cpp
   ├── extern CUser pUser/pAdmin        :41-42
   ├── Mode fan-out guards              :379, 411, 445, 470, 499, 583, 603, 1595, 1604, 1616, 2086
   ├── pUser[conn].Count++/--           :2213, 2228
   ├── pAdmin[].Level/DisableID/Year    :1108-1120, 1261-1272
   └── pUser[svr].cSock.SendOneMessage  :274, 294, 333, 559, 681, 708, 716, 724, 729, 734, 780, 841,
                                           1024, 1080, 1353, 1587, 1681, 1759, 1926, 1965, 1986,
                                           2100, 2116-2117, 2131-2132, 2147-2148, 2164-2165, 2180,
                                           2198-2199, 2384-2385
 CReadFiles.cpp
   ├── pUser[i].Mode (empty slots -> "-1") :73
   ├── pUser[i].Count (population)        :79-82
   └── pUser[svr].cSock.SendOneMessage    :253, 829
 CRanking.cpp
   └── pUser[tmsrvId].cSock.SendOneMessage:312-313
```

### External Dependencies

| Dependency | Version / Form | Purpose | Coupling |
|---|---|---|---|
| Winsock 2 (`ws2_32`) | Win32 system library, `MAKEWORD(1,1)` at `CPSock.cpp:113` | `accept`, `WSAAsyncSelect`, `recv`, `send`, `closesocket` | Hard, compile + link |
| Win32 USER32 | system library | `hWndMain`, window messages `WM_USER+2..7` as the async socket event channel | Hard, structural |
| C runtime | static CRT (Debug `MultiThreadedDebug`) | `rand`, `srand` (never called), `malloc`, `memcpy`, `strncpy`, `sprintf` | Hard |
| `Admin.txt` | operator-managed text, working directory | Admin IP allow-list, reloaded per accept | Soft, runtime file |
| `serverlist.txt` → `g_pServerList` | loaded by `BASE_InitializeServerList()` (`Server.cpp:529`) | Game-server IP allow-list + local group identity | Soft, runtime file, boot-only |
| `Config.txt` | `ReadConfig` (`Server.cpp:393-410`) | `Sapphire` sent in slot priming | Soft, runtime file |
| `account/*` via `CFileDB` | raw binary `STRUCT_ACCOUNTFILE` | Admin credential + level source | Indirect (through CFileDB) |
| Environment (hostname) | `gethostname` + `getaddrinfo` (`Server.cpp:542-569`) | Derives `ServerIndex` and `LocalIP`; exit if unresolved | Hard precondition |

No third-party libraries, no MFC usage at runtime (`stdafx.h` includes `<afxwin.h>` but no PCH is configured and no MFC symbol is used), no COM, no database driver.

---

## 6. Afferent and Efferent Coupling

### Component level

| Component | Afferent (Ca) | Efferent (Ce) | Critical | Basis |
|---|---|---|---|---|
| `CUser` (whole class) | 5 | 2 | Medium | Ca = `Server.cpp`, `CFileDB.cpp`, `CReadFiles.cpp`, `CRanking.cpp`, `Server.h` (extern decls). Ce = `CPSock` (composition), `Basedef.h` (constant/type). Matches the architecture report's "~6 / 2" within the stated approximation method. |
| `CUser::AcceptUser` | 2 | 4 | Low | Call sites `Server.cpp:962`, `1076`; efferent = `accept`, `WSAAsyncSelect`, `closesocket`, `inet_ntoa` + member writes |

### Member level (afferent fan-in on each field)

| Member | Read sites | Write sites | Afferent | Efferent | Critical | Note |
|---|---|---|---|---|---|---|
| `cSock` | ~40 | ~8 (plus 3 cursor copies per handoff) | Very high | 4 (CPSock methods used: `Receive`, `ReadMessage`, `SendOneMessage`, `CloseSocket`) | High | The only member that performs I/O; 114 textual references across the 4 consumers (`Server.cpp` 60, `CFileDB.cpp` 50, `CReadFiles.cpp` 2, `CRanking.cpp` 2) |
| `Mode` | 19 | 7 | High | 0 | High | Occupancy oracle for 4 modules; two-state assumption is implicit everywhere |
| `Level` | ~24 | 3 | High | 0 | High | Simultaneously auth flag and privilege value (30 textual references across `Server.cpp`, `CFileDB.cpp`, `CRanking.cpp`) |
| `IP` | 8 | 4 | Medium | 0 | Medium | Slot key; compared raw and decoded byte-wise for logs/UI |
| `DisableID` | 6 | 5 | Medium | 0 | High | Cross-module deferred-work handle into `CFileDB` |
| `Encode1` / `Encode2` | 2 | 2 | Low | 1 (`rand`) | High (security) | Only used by the challenge handshake |
| `Name` | 4 | 2 | Low | 0 | Medium | Authenticated admin identity, used in audit logs |
| `Count` | 3 | 2 | Low | 0 | Low | Population metric only |
| `Year` / `YearDay` | 2 | 4 | Low | 0 | Medium | Carried from NP packet to `DisableAccount` |
| `TempUser` (instance) | 2 handlers | 2 handlers | Medium | 1 (`AcceptUser`) | High | Single shared staging object for both listeners |

**Cohesion assessment.** `CUser` is *coincidentally cohesive*: `IP`, `Mode`, `cSock` form a coherent connection identity; `Count` is a game-server concern; `Level`, `Encode1/2`, `Name`, `DisableID`, `Year`, `YearDay` are admin concerns. Two distinct roles (world-server slot, operator session) share one class, distinguishable only by which array the instance lives in — and nothing in the type system prevents `pAdmin`-only fields from being used on a `pUser` instance (they are, in fact, permanently uninitialized there).

**Instability.** `I = Ce / (Ca + Ce)` for the class = 2/7 ≈ 0.29. The component is relatively stable, which is appropriate — but the effective instability is masked by the fact that its *semantics* live in the unstable `Server.cpp` (`Ca` ≈ 6, `Ce` ≈ 8 per the architecture report), so the coupling burden has been shifted to the consumer.

---

## 7. Endpoints

**Not applicable in the REST/GraphQL/gRPC sense.** `CUser` does not expose any network endpoint, route, RPC method, or serialized interface. It is a passive entity populated by Winsock accept events.

For completeness, the socket entry points that *drive* the slot lifecycle (owned by `Server.cpp`, registered by `CPSock::StartListen`/`ConnectServer`) are:

| Entry point | Transport | Message ID | Handler | Purpose |
|---|---|---|---|---|
| `0.0.0.0:7514` (`DB_PORT`) | TCP, Winsock async | `WSA_ACCEPT` = `WM_USER+3` | `MainWndProc` case `WSA_ACCEPT` (`Server.cpp:1068`) | Admit a game server into a `pUser` slot |
| `0.0.0.0:8895` (`ADMIN_PORT`) | TCP, Winsock async | `WSA_ACCEPTADMIN` = `WM_USER+5` | `MainWndProc` case `WSA_ACCEPTADMIN` (`Server.cpp:951`) | Admit an NP admin tool into a `pAdmin` slot |
| Per-slot game link | TCP, Winsock async | `WSA_READ` = `WM_USER+100` | `MainWndProc` case `WSA_READ` (`Server.cpp:877`) | Read/dispatch game-server packets |
| Per-slot admin link | TCP, Winsock async | `WSA_READADMIN` = `WM_USER+6` | `MainWndProc` case `WSA_READADMIN` (`Server.cpp:797`) | Read/dispatch NP packets (incl. `_MSG_NPIDPASS`) |
| Outbound redirect link | TCP client | `WSA_READADMINCLIENT` = `WM_USER+7` | `MainWndProc` case `WSA_READADMINCLIENT` (`Server.cpp:756`) | Character-transfer feed (does not use `CUser`) |

Wire format for all of the above: 4-byte `INITCODE` (`0x1F11F311`) then `HEADER{short Size; char KeyWord; char CheckSum; short Type; short ID; uint ClientTick}` followed by the typed body (`CPSock.h:42-50`), body obfuscated with the fixed 512-byte key table (`CPSock.cpp:29-46`).

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| Game servers (`TMSrv`) | Internal service, inbound | Account/character persistence for 10 world servers | TCP 7514, Winsock async, `FLAG_GAME2DB` packets | Binary, key-table obfuscated, `HEADER` + typed struct | IP allow-list + /24 subnet check at admit; unknown socket closed; `Mode` guards on fan-out; **no** retry/backoff/circuit breaker |
| NP admin tool | Privileged operator client, inbound | Account admin, notices, bans, character create/transfer | TCP 8895, `FLAG_NP2DB` packets | Binary, same envelope; `MSG_NPIDPASS` challenge | Two-step admit (IP list, then account challenge); challenge/password/level failures destroy the slot; no lockout or rate limit |
| Redirect / transfer server | Outbound client | Relay for cross-server character transfer | TCP, `WSA_READADMINCLIENT` | Binary, same envelope | Enabled only if `redirect.txt` exists; connect failure shows a modal box and aborts boot (`Server.cpp:601-605`) |
| `Admin.txt` | Operator file, hot-read | Admin IP allow-list, slot-indexed | Text, `%.` → space, `idx a b c d` | ASCII lines | Missing file → silently no allow-list; malformed index skipped; **stale entries never cleared** |
| `serverlist.txt` → `g_pServerList` | Boot-time config | Game-server IP allow-list + group identity | Text via `BASE_InitializeServerList` | `[group][11][64]` strings | Unresolved group → modal box + exit (`Server.cpp:575-581`); boot-only, no reload |
| `Config.txt` | Boot-time config | `Sapphire`, `LastCapsule` | Text `Sapphire %d` | ASCII | Missing → modal box, `ReadConfig` returns FALSE but boot continues (`Server.cpp:518`) |
| `account/*` (via `CFileDB`) | Persistence | Admin credential, character levels for privilege derivation | Raw binary `STRUCT_ACCOUNTFILE` | Fixed-size records | Read failure during auth → **silently ignored**, slot stays unauthenticated |
| Status publishers (`CReadFiles`) | Outbound files | Population per slot; peak tracking | `C:/wamp/www/servNN.htm`, `../../Common/dataNN.csv` | HTML/CSV text | Write failure → silent return (`CReadFiles.cpp:68-69`) |
| `Log` sink | Diagnostics | Every accept, reject, auth, ban, socket error | `.\Log\DB_*.txt` + window caption | Text | `fprintf` only when `fLogFile` non-null (`Server.cpp:2135-2136`) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Anemic Domain Entity | `CUser` = public fields + one transport method | `CUser.h:29-50`, `CUser.cpp:26-65` | Pure data carrier; all behaviour external |
| Fixed-capacity Object Pool (static) | `pUser[MAX_SERVER]`, `pAdmin[MAX_ADMIN]` file-scope arrays | `Server.cpp:82-83` | Pre-allocated slots, zero runtime allocation for slots |
| Staging buffer / Flyweight handoff | Single `TempUser` instance reused for every accept; fields copied into the destination slot | `Server.cpp:80`, `962`, `1076`, `1036-1044`, `1152-1157` | Avoids allocating a slot before admission decisions complete |
| Proactor via window messages | `WSAAsyncSelect` posts `WM_USER+n` to `hWndMain`; `MainWndProc` switches on message id | `CPSock.h:25-31`, `Server.cpp:743-949` | Single-threaded event loop, no locking anywhere |
| Repository / Gateway | `CFileDB` owns the account cache and file persistence; slots never touch files directly (except `ReadAdmin`) | `CFileDB.cpp`, `Server.cpp:353-391` | Isolate binary-file storage |
| Two-phase commit by intent record | `DisableID` + `Year`/`YearDay` recorded on the admin slot, consumed later by `CFileDB` at logout | `Server.cpp:1949-1952`; `CFileDB.cpp:1108-1120`, `1261-1272` | Defer an operation that cannot be performed while the resource is owned by another server |
| Challenge/Response (session binding) | `Encode1`/`Encode2` per accept, echoed in `_MSG_NPIDPASS` | `Server.cpp:1052-1065`, `1686-1687` | Bind login to a specific connection and slot |
| Guard-clause authorization | Per-handler `Level` threshold checks at the top of each `case` | `Server.cpp:1611`, `1640`, `1742`, `1852`, `1929`, `1959` | Command-level privilege gating |
| Null-object sentinel | `Level = -1`, `DisableID = 0`, `Mode = USER_EMPTY` | `CUser.cpp:28-32`, `Server.cpp:1036-1037` | Encode absent state without optionals |
| Header/Type tag dispatch | `HEADER.Type` with direction flags `FLAG_GAME2DB`/`FLAG_NP2DB` then `switch(Type)` | `Server.cpp:1343`, `1467`, `1479` | Route by protocol+command in one integer |

**Architectural decisions observed.** (1) No threads, no locks: the UI thread is the only executor, so slot fields are never concurrently accessed — a deliberate simplicity that caps throughput. (2) Trust is address-based at the transport layer and data-based at the authorization layer; there is no session token, no TLS, and no per-message authentication. (3) The slot arrays are the connection registry — no map, no handle table — so every event pays an O(10) scan. (4) All state that must survive a disconnect is deliberately left in the slot (IP, Level) or in a global (`pAccountList`), which keeps reconnection cheap but makes cleanup non-uniform.

---

## 10. Security Measures and Resilience

**Present controls**

* Game link: IP allow-list from `serverlist.txt` **and** /24 subnet match (`Server.cpp:1095-1118`, `1150`).
* Admin link: IP allow-list from `Admin.txt`, hot-reloaded per accept (`Server.cpp:953`, `353-391`).
* Admin credentials: per-connection random challenge, 16-byte password comparison, blocked/defective account prefixes rejected, level-derived authorization (`Server.cpp:1686-1722`).
* Packet envelope validation with direction flags and index bounds before dispatch (`Server.cpp:1341-1354`, `1465-1477`).
* Fixed-size message cap of 8192 bytes enforced in the parser, with buffer reset on violation (`CPSock.cpp:397-406`).
* Two-phase ban that refuses to overwrite a live session's file (`Server.cpp:1949-1952`).
* Audit logging of every accept, reject, login success/failure, ban, and save (`Log` at `Server.cpp:2115-2139`).

**Absences and weaknesses (see Section 13 for the risk table)**

* No transport encryption; the admin challenge and password cross the wire obfuscated only by a fixed, public key table.
* `srand` is never called, so the challenge sequence is deterministic from process start; `Encode1 || Encode2` collapses to roughly one 14-bit draw.
* Two admin commands (`_MSG_NPCreateCharacter`, `_MSG_NPDonate`) execute without any `Level` check, i.e. before authentication.
* No login attempt limit, delay, or lockout; a failed account read is not even logged.
* No rate limiting on accepts; the duplicate-slot rule turns a second connection into a denial of service against the live session (BR-06).
* A peer that fails the `INITCODE` handshake keeps its slot and never gets disconnected; each read appends to the receive buffer until it fills (`CPSock.cpp:366-383`, `341-342`).
* Admin revocation is not effective: `pAdminIP` is never cleared, so a removed `Admin.txt` line stays honoured until restart.

---

## 11. Configuration Management

| File | Read at | Contents | Effect on this component | Failure mode |
|---|---|---|---|---|
| `serverlist.txt` | Boot, `BASE_InitializeServerList()` (`Server.cpp:529`) | `[group][11]` IP strings; column 0 = group host identity | Defines the game-server allow-list and index→slot mapping | Missing/short → no game server can be admitted; unresolved group → process exits |
| `Admin.txt` | Every `WSA_ACCEPTADMIN` (`Server.cpp:953`) | `idx a.b.c.d` per admin slot | Admin IP allow-list, slot-indexed | Missing → no new admin IPs (retained `pAdmin[i].IP` still work); removed lines are not revoked |
| `Config.txt` | Boot (`Server.cpp:518`, `404`) | `Sapphire %d`, `LastCapsule %hu` | `Sapphire` sent to each game server on admission | Missing → modal box; boot continues with default `Sapphire = 8` (`Server.cpp:50`) |
| `redirect.txt` | Boot (`Server.cpp:589-611`) | `host port adminclientid adminclientpass` | Enables `TransperCharacter` priming message | Present but unreachable → modal box + boot abort |
| Hostname / DNS | Boot (`Server.cpp:542-569`) | Machine hostname | Derives `ServerIndex` and `LocalIP[4]`, the subnet key for BR-04 | Unresolved → modal box + exit |
| Environment/identity | `Config.txt` + hostname together | — | `ServerIndex` selects which `serverlist.txt` row group is authoritative | Mismatch → exit at `Server.cpp:575-581` |

No environment variables, no registry keys, no feature flags, and no per-environment build variants affecting this component. All capacities (`MAX_SERVER`, `MAX_ADMIN`, `MAX_USER`, `ACCOUNTNAME_LENGTH`, ports) are compile-time constants in `Code/Basedef.h`.

---

## 12. Performance Characteristics

* **Slot lookup is O(MAX_SERVER)/O(MAX_ADMIN) per event** (`Server.cpp:2169-2189`); with 10 slots this is 10 integer comparisons per read event, executed on every `FD_READ`. Effectively free, but it scales with slot count if constants grow.
* **Accept-time priming is O(65536)** with two potential sends per guild id (`Server.cpp:1161-1171`). It runs synchronously on the UI thread, so a busy guild table delays all other socket events during a game-server (re)connect.
* **Memory is fixed and front-loaded**: 21 `CUser` instances × 256 KB of `CPSock` buffers ≈ 5.4 MB reserved at static init (`CPSock.cpp:66-67`), independent of traffic. No per-connection allocation, so no fragmentation or leak from the slot path itself.
* **Send is synchronous but non-blocking-tolerant**: `SendMessageA` loops once and returns, leaving unsent bytes in `nSentPosition` for the next call (`CPSock.cpp:658-683`); `AddMessage` rejects the packet when the 128 KB buffer is nearly full (`CPSock.cpp:518-524`). Broadcast bursts (guild replay, admin notice) can therefore be partially dropped under back-pressure, with only a log line.
* **Single-threaded executor**: every accept, read, parse, dispatch, file write, and log write happens on the window thread. A slow `DBWriteAccount` in `DisableAccount` (triggered from inside the read loop via `ProcessAdminMessage`) blocks all other slots.
* **`inet_ntoa` result discarded** at `CUser.cpp:60` — one needless (thread-unsafe, static-buffer) call per accept.
* **No timeout/keepalive** on slot sockets: `WSAAsyncSelect` requests only `FD_READ | FD_CLOSE`, so a silently dropped peer (no FIN) occupies its slot until TCP eventually resets or the process restarts.

---

## 13. Technical Debt & Risks

| Risk Level | Component Area | Issue | Evidence | Impact |
|---|---|---|---|---|
| Critical | Admin authorization | `_MSG_NPCreateCharacter` and `_MSG_NPDonate` have no `Level` gate; both are reachable at `Level == -1` after only the IP allow-list and challenge echo | `Server.cpp:1481-1598`, `1982-2006` | Unauthenticated allow-listed peer can create/modify characters and drop donation files |
| Critical | Challenge generation | `srand` never called; `Encode1`/`Encode2` are consecutive `rand()` draws; verification is `A||B` so one 14-bit guess suffices | `Server.cpp:1052-1053`, `1686` | Predictable "challenge"; effectively a fixed token per process lifetime |
| High | Privilege model | Admin level = stored character level − 1000, computed from game-persisted data | `Server.cpp:1713-1726` | Any level inflation (bug, GM item, file edit) becomes DBSrv admin authority |
| High | Slot admit (game) | `CUser::AcceptUser` return value ignored; on `accept()` failure the stale `TempUser.IP`/`Mode` from the previous connection is used for slot resolution | `Server.cpp:962`, `1076`; `CUser.cpp:44-47` | Wrong slot assignment / state corruption under transient accept failures |
| High | Transport binding | `if(WSAAsyncSelect(...) > 0)` is inverted (returns `0` on success, `-1` on error) | `CUser.cpp:49` | Async-registration failure is undetectable; the slot is committed but never signalled |
| High | Admin admit | `for(i = 0; i <= MAX_ADMIN; i++)` reads `pAdmin[10]` — one past a 10-element array | `Server.cpp:968` | Out-of-bounds read on every admin connection; reads adjacent globals |
| High | Parser | A peer that never sends `INITCODE` is never disconnected; positions are not reset on `ErrorCode == 2`, and `ErrorCode == 1` (checksum) still returns the packet | `CPSock.cpp:366-383`, `458-464`; `Server.cpp:938-944` | Slot squatting / slow-loris style DoS against a whitelisted slot |
| High | Slot handoff | `TempUser.cSock.Sock` is not cleared after transfer, aliasing the committed slot's handle; buffer contents and `cSock.Init` are not transferred | `Server.cpp:1041-1044`, `1152-1157` | Any future `TempUser.cSock.CloseSocket()` closes a live slot's socket; handshake state is implicit |
| Medium | Admin revocation | `ReadAdmin` only writes `pAdminIP`; entries removed from `Admin.txt` remain valid until restart | `Server.cpp:353-391` | Cannot revoke a compromised admin host at runtime |
| Medium | Cleanup asymmetry | `Receive() != 1` on admin leaves the slot fully occupied; on game it leaves `Mode = 1` with reset cursors | `Server.cpp:825-832`, `904-916` | Zombie slots that broadcasts keep writing to; inconsistent occupancy |
| Medium | Duplicate connection | Second connection from a slot's IP kills both sessions instead of allowing takeover/handshake | `Server.cpp:1119-1136`, `1005-1019` | Trivial DoS oscillation between two processes on one host; also kills a legit session on RST/retransmit races |
| Medium | Deferred ban | `DisableID` is a global account index recorded on the admin slot; stale if the player reconnects elsewhere; completion requires the admin slot to still have `Level > 2` | `Server.cpp:1949-1952`; `CFileDB.cpp:1108-1120`, `1261-1272` | Bans silently not applied; or applied to the wrong account index |
| Medium | Uninitialized members | `Name`, `Encode1`, `Encode2`, `Year`, `YearDay` are never initialized by the constructor | `CUser.cpp:26-33` | Benign today (write-before-read on admin paths), latent UB if any new read path appears; game slots hold garbage in admin-only fields |
| Medium | Envelope bound | `std->ID > MAX_USER` allows `ID == MAX_USER` (1000), the documented first NPC/mob index | `Server.cpp:1343` | Off-by-one at the account/NPC namespace boundary; relies on downstream checks |
| Medium | Error-code naming | `ReadMessage(int *ErrorCode, int *ErrorType)` is called as `ReadMessage(&Error, &ErrorCode)` and then tested as `Error == 1 \|\| Error == 2` | `CPSock.h:76` vs `Server.cpp:840`, `855`, `924`, `938` | Semantics survive only because both outputs hold small codes; any change to the signature silently breaks error handling |
| Low | Dead code | `GetEmptyUser()` is defined and never called in DBSrv; `"err,accept_admin no empty"` is unreachable | `Server.cpp:2191-2200`, `1030-1034` | Misleading control flow; suggests an allocation strategy that does not exist |
| Low | Capacity constants | `MAX_SERVERNUMBER` (11) used both as allow-list column count and as the `pUser` index bound (`User < MAX_SERVERNUMBER`) | `Server.cpp:1119`; `Basedef.h:50` | Latent OOB if slot count and table width ever diverge |
| Low | Diagnostics | `sprintf(String, str)` with a non-literal format in `TextOutWind`; `inet_ntoa` result discarded; triple repeated log line for one receive failure | `Server.cpp:107`, `907-910`; `CUser.cpp:60` | Format-string hazard if a non-literal message ever reaches it; noise in logs |
| Low | Empty destructor | `~CUser()` closes nothing; static arrays destroyed after CRT teardown | `CUser.cpp:35-37` | Handle leaks by design; prevents reuse as a scoped object |

---

## 14. Test Coverage Analysis

**Result: no tests exist for this component, or anywhere else in the repository.**

Evidence (all commands executed against the project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` per the `ignore-folders` parameter):

1. **Solution contents** — `W2PP Code Project.sln` declares exactly two projects: `DBSrv` and `TMSrv`, both `Application` targets. There is no test project, no test configuration, and no test build target.
2. **Project files** — only `Code/DBSrv/DBSrv.vcxproj` and `Code/TMSrv/TMSrv.vcxproj` exist. Neither references a test framework (`gtest`, `catch2`, `CppUnit`, `doctest`, MSTest, GoogleMock) in includes, additional dependencies, or preprocessor definitions.
3. **Filename search** — recursive search for `*test*`, `*Test*`, `*spec*`, `*Spec*` across all non-ignored paths returned zero files. A targeted search for `gtest*`, `catch*`, `*_test.cpp`, `test_*.cpp` also returned zero.
4. **Directory search** — recursive search for directories named `*test*` (non-ignored) returned zero.
5. **No test harness for the socket layer** — `CPSock`/`WSAAsyncSelect` are instantiated against a real `HWND` (`CUser.cpp:49` uses the global `hWndMain`), so the component cannot be instantiated without a running message pump; no fake/mock transport exists.
6. **No fixtures or golden data** — no `fixtures/`, `testdata/`, `golden/`, or recorded packet captures are present.

| Component / Unit | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `CUser` (constructor) | 0 | 0 | 0% | No test; defaults verified only by reading `CUser.cpp:26-33` |
| `CUser::AcceptUser` | 0 | 0 | 0% | No test; requires a real listening socket + `HWND`; the inverted `WSAAsyncSelect` guard would be caught by a single negative test |
| Slot admission (BR-04) | 0 | 0 | 0% | No test; allow-list + subnet logic only exercised by running two servers |
| Duplicate-slot eviction (BR-06) | 0 | 0 | 0% | No test; the mutual-kill behaviour is undocumented outside this analysis |
| Reconnection / sticky IP (BR-05) | 0 | 0 | 0% | No test |
| Encode challenge (BR-13) | 0 | 0 | 0% | No test; the `||`-collapse and unseeded `rand()` are unverified |
| Admin auth + level derivation (BR-15/BR-16) | 0 | 0 | 0% | No test; thresholds are only expressed as inline `if` statements |
| Deferred ban (BR-17) | 0 | 0 | 0% | No test; a two-phase protocol with no unit coverage of either phase |
| Packet envelope validation (BR-20/21) | 0 | 0 | 0% | No test; the `ID == MAX_USER` boundary is untested |
| Slot arrays / globals (`Server.cpp:80-83`) | 0 | 0 | 0% | No test; global state makes isolated testing impossible without refactoring |

**Consequences.** Every business rule in Section 3 is verified only by production behaviour. The four highest-risk defects identified in Section 13 (missing `Level` gates, predictable challenge, OOB admin slot read, inverted `WSAAsyncSelect` guard) are all statically detectable and would each be caught by a single unit or fuzz test. Two further behaviours (BR-06 eviction, BR-09 asymmetric cleanup) cannot be exercised at all without a live peer, which explains why they persist.

---

## 15. Assumptions, Ambiguities and Confidence

| Item | Confidence | Note |
|---|---|---|
| `Mode` has only two states in DBSrv | High | Only `USER_EMPTY`/`USER_ACCEPT` are defined and assigned; literal `0` used interchangeably (`Server.cpp:1129`, `2074`) |
| `CUser` is one slot per game server *or* admin tool | High | Two separate arrays of the same type; architecture report agrees (`:169`) |
| `Encode1/Encode2` are admin-only | High | Written only at `Server.cpp:1052-1053`; read only at `1686`; never set for `pUser` |
| `Level` semantics (`-1` = unauthenticated) | High | Established at `Server.cpp:1036`, tested at `1689`, derived at `1724` |
| "Slot is free" means `Mode == USER_EMPTY` (not `Sock == 0`) | High | Both predicates appear; fan-out checks use both, admission uses `Mode` only |
| `g_pServerList[...][0]` = local/group identity, `[1..10]` = game servers | Medium-High | `WinMain` compares column 0 to the hostname (`Server.cpp:557`); admission scans from 1 and maps `i-1` |
| Whether index 10 of `serverlist.txt` is a BISrv entry or the 10th game server | Medium | `Basedef.h:50` comment says "DB + TMSrvs + BISrv" but the admission loop consumes indices 1..10 into slots 0..9 — the intent is ambiguous and the comment does not match the arithmetic |
| Intent of `WSAAsyncSelect(...) > 0` | High (defect) | Winsock contract: `0` success, `SOCKET_ERROR` failure; the guard cannot fire |
| Whether `_MSG_NPCreateCharacter`/`_MSG_NPDonate` lacking a `Level` gate is intentional | Low | No comment or spec; treated as an authorization gap, not a documented rule |
| Whether `pAdminIP` is meant to be revoked when an `Admin.txt` line is deleted | Medium | `ReadAdmin` has no clearing logic; the file is re-read per accept, implying hot reload was intended but incompletely implemented |
| Exact meaning of `Year`/`YearDay` | Medium | Written into the account file as a ban marker (`Server.cpp:1393-1394`) and echoed back; no time comparison exists anywhere, so it is informational rather than an expiry |
| `MAX_DBACCOUNT` = `MAX_USER * MAX_SERVER` = 10,000 | High | `Basedef.h:60`; used as the admin envelope bound and as `pAccountList` size |

---

## 16. File Reference Index

| Path (relative) | Role in this component |
|---|---|
| `Code/DBSrv/CUser.h` | Class definition, mode constants, field layout |
| `Code/DBSrv/CUser.cpp` | Constructor, destructor, `AcceptUser` |
| `Code/DBSrv/Server.h` | `extern` declarations of `TempUser`, `pUser`, `pAdmin`, lookup helpers |
| `Code/DBSrv/Server.cpp` | Slot arrays (`:80-83`), `WinMain` boot/config (`:427-624`), `MainWndProc` (`:743-1191`), `ProcessClientMessage` (`:1339`), `DisableAccount`/`EnableAccount` (`:1359`, `1419`), `ProcessAdminMessage` (`:1463-2011`), `ReadAdmin` (`:353`), lookups (`:2169-2200`) |
| `Code/DBSrv/CFileDB.cpp` | `Count` accounting (`:2213`, `2228`), `Mode` fan-out guards, deferred-ban completion (`:1108-1120`, `1261-1272`), `SendDBSavingQuit` (`:2369-2385`) |
| `Code/DBSrv/CReadFiles.cpp` | Population publishing from `Mode`/`Count` (`:71-83`) |
| `Code/DBSrv/CRanking.cpp` | Per-slot ranking push via `pUser[tmsrvId].cSock` (`:312-313`) |
| `Code/CPSock.h` | `HEADER`, `INITCODE`, WSA message ids, buffer limits |
| `Code/CPSock.cpp` | Buffer allocation (`:62-73`), `CloseSocket` (`:84-98`), `StartListen` (`:128-174`), `Receive` (`:336-347`), `ReadMessage` (`:353-467`), `AddMessage` (`:513-541`), `SendMessageA` (`:617-684`) |
| `Code/Basedef.h` | Ports (`:45-46`), capacities (`:48-71`), direction flags (`:936-939`), NP message/struct definitions (`:2243-2325`) |
| `Code/DBSrv/DBSrv.vcxproj` | Build inclusion of `CUser.cpp`/`CUser.h` (`:99`, `:110`) |
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | Architectural context: `:169` (component entry), `:165` (Server Core), `:259-262` (integration points) |
| `Code/TMSrv/CUser.cpp` | **Explicitly out of scope** — separate component report (session entity with a multi-state machine) |

---

*End of report.*
