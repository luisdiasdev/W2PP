# Component Deep Analysis Report

**Component:** CUser (DBSrv)
**Project:** W2PP Legacy C/C++ codebase (`legacy/` folder)
**Analyzed on:** 2026-08-19
**Scope:** Per-game-server session state in the DBSrv database server

---

## 1. Executive Summary

**Component purpose.** The `CUser` class is the per-connection session-state container used by the DBSrv database server. Each `CUser` instance represents one live TCP connection slot from a peer to the DB server. The DB server maintains three kinds of `CUser` instances:

- `pUser[MAX_SERVER]` — up to 10 slots (one per connected **game server / TMSrv**), defined in `legacy/Code/DBSrv/Server.cpp:82`.
- `pAdmin[MAX_ADMIN]` — up to 10 slots (one per connected **NP admin tool**), defined in `legacy/Code/DBSrv/Server.cpp:83`.
- `TempUser` — a transient scratch slot used during the `accept()` handshake before the connection is committed to a persistent slot (`Server.cpp:80`).

**Role in the system.** DBSrv is the central database/persistence server of the W2PP game-server emulator. It accepts TCP connections from game servers (port 7514, `DB_PORT`) and from the NP admin tool (port 8895, `ADMIN_PORT`). `CUser` is the socket-level session record that DBSrv uses to (a) associate each connected peer with its network socket and IP, (b) track the connection lifecycle state (`USER_EMPTY`/`USER_ACCEPT`), (c) dispatch incoming messages to the correct peer, and (d) broadcast messages to all connected peers. It is the thin transport/session layer on top of which `CFileDB` (business/persistence logic) and `CReadFiles`/`CRanking` (file import and ranking) operate.

**Key findings.**

1. `CUser` is a minimal, plain-old-data style class: it bundles an `IP`, a `Mode` state, a `CPSock` network socket, a `Count`, a `Level`, two challenge codes `Encode1`/`Encode2`, an account `Name`, a `DisableID`, and two date fields (`Year`, `YearDay`).
2. The entire session lifecycle is managed from `Server.cpp::MainWndProc`, which handles WinSock async messages (`WSA_ACCEPT`, `WSA_READ`, `WSA_ACCEPTADMIN`, `WSA_READADMIN`) and drives `CUser::AcceptUser`.
3. The state machine is binary and coarse: a slot is either `USER_EMPTY` (0, free) or `USER_ACCEPT` (1, connected). There is no fine-grained per-game-server authentication state beyond `Mode`; the admin path uses `Level` to encode privilege after a challenge/response login.
4. `CUser::Level` is dual-purpose: `-1` means "not yet authenticated" for admins, and after admin login it is set to `maxlevel - 1000` (where `maxlevel` is the highest character level on the account), forming a crude admin privilege tier.
5. There is **no test infrastructure** in the project. No unit, integration, or regression tests exist for `CUser` or any other component in the repository.
6. The class is tightly coupled to global instances (`pUser`, `pAdmin`, `TempUser`) shared across `Server.cpp`, `CFileDB.cpp`, `CReadFiles.cpp`, and `CRanking.cpp` via `extern` declarations, which makes the component a shared global state with high blast radius.

---

## 2. Data Flow Analysis

The following traces the flow of a game-server connection through the `CUser` session machinery.

```
1. TCP SYN from a game server (TMSrv) arrives on DB_PORT 7514
2. WinSock posts WSA_ACCEPT async message to MainWndProc (Server.cpp:1068)
3. TempUser.AcceptUser(ListenSocket.Sock, WSA_READ) accepts the socket:
     - accept() -> tSock
     - WSAAsyncSelect(tSock, hWndMain, WSA_READ, FD_READ|FD_CLOSE)
     - fills cSock.Sock and buffer positions (CUser.cpp:39-64)
     - records acc_sin.sin_addr.S_un.S_addr into IP
     - sets Mode = USER_ACCEPT
4. TempUser.IP validated:
     - if already bound to a pUser[i].IP -> reuse slot i (Server.cpp:1088)
     - else must appear in g_pServerList[ServerIndex][...] -> slot i-1 (Server.cpp:1099)
5. LocalIP 3-octet match required (Server.cpp:1150) - external LAN peers rejected
6. TempUser fields copied into persistent pUser[User] slot (Server.cpp:1152-1157)
7. cFileDB.SendDBSignalParm3(User, 0, _MSG_DBSetIndex, ServerIndex, Sapphire, User)
   sent to the game server to confirm registration (Server.cpp:1159)
8. Guild/War/Ally state pushed to the new session (Server.cpp:1161-1176)
9. Game server now sends business messages:
     - WSA_READ posted -> GetUserFromSocket(wParam) resolves slot (Server.cpp:879, 2169)
     - ProcessClientMessage(User, Msg) validates FLAG_GAME2DB + ID range (Server.cpp:1339)
     - cFileDB.ProcessMessage(Msg, conn) executes business logic (CFileDB.cpp:236)
     - replies routed back via pUser[conn].cSock.SendOneMessage(...)
10. Peer disconnects (FD_CLOSE):
     - pUser[User].cSock.CloseSocket(); pUser[User].Mode = USER_EMPTY (Server.cpp:896-897)
```

An analogous flow exists for the admin tool over `ADMIN_PORT`:
```
1. WSA_ACCEPTADMIN posted (Server.cpp:951)
2. ReadAdmin() reloads the allowed admin IP table from Admin.txt (Server.cpp:353)
3. TempUser.AcceptUser(AdminSocket.Sock, WSA_READADMIN)
4. IP must match pAdminIP[i] (from Admin.txt) or an already-bound slot (Server.cpp:968-993)
5. Slot checked for Mode == USER_EMPTY; copy TempUser -> pAdmin[User] (Server.cpp:1036-1044)
6. Random challenge Encode1/Encode2 = rand()%10000 sent via _MSG_NPReqIDPASS (Server.cpp:1052-1065)
7. Admin replies _MSG_NPIDPASS; on success pAdmin[conn].Level = maxlevel - 1000 (Server.cpp:1680-1736)
8. Admin business messages handled by ProcessAdminMessage (Server.cpp:1463)
```

---

## 3. Business Rules & Logic

## Overview of the business rules:

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| State Machine | Slot is either USER_EMPTY (0) or USER_ACCEPT (1) | legacy/Code/DBSrv/CUser.h:26-27 |
| Validation | Game-server accept rejects unregistered IPs | legacy/Code/DBSrv/Server.cpp:1095-1115 |
| Validation | Game-server registration requires LocalIP 3-octet match | legacy/Code/DBSrv/Server.cpp:1150 |
| Validation | Admin accept rejects IPs not in Admin.txt allowlist | legacy/Code/DBSrv/Server.cpp:977-1003 |
| Concurrency | Re-accept into a non-empty slot closes both sockets | legacy/Code/DBSrv/Server.cpp:1007-1018, 1121-1135 |
| Capacity | Max 10 game-server slots (MAX_SERVER) | legacy/Code/Basedef.h:48 |
| Capacity | Max 10 admin slots (MAX_ADMIN) | legacy/Code/Basedef.h:58 |
| Session Lookup | Socket-to-slot resolution is a linear scan | legacy/Code/DBSrv/Server.cpp:2169-2189 |
| Slot Allocation | First free USER_EMPTY slot returned by GetEmptyUser | legacy/Code/DBSrv/Server.cpp:2191-2200 |
| Auth (Admin) | Random Encode1/Encode2 challenge before login | legacy/Code/DBSrv/Server.cpp:1052-1065, 1686 |
| Auth (Admin) | Level derived from max character level minus 1000; min 1000 required | legacy/Code/DBSrv/Server.cpp:1713-1726 |
| Accounting | pUser[conn].Count tracks logged-in account count | legacy/Code/DBSrv/CFileDB.cpp:2213, 2228 |
| Reporting | Connection count reported as 4*Count/3 + rand()%4 | legacy/Code/DBSrv/CReadFiles.cpp:82 |
| Naming | Reserved account prefixes COM* and LPT* rejected | legacy/Code/DBSrv/CFileDB.cpp:86-89 |
| Broadcast | Iteration only over non-empty slots with non-zero socket | legacy/Code/DBSrv/CFileDB.cpp:377-384 |

## Detailed breakdown of the business rules:

---

### Business Rule: Connection Slot State Machine

**Overview:**
Every `CUser` slot has a single `Mode` integer that encodes the connection state. The only two states defined are `USER_EMPTY` (0) and `USER_ACCEPT` (1). This state is the canonical signal used across the entire DBSrv codebase to determine whether a slot is available for a new connection or whether it currently holds a live peer.

**Detailed description:**
The constructor `CUser::CUser()` initializes `Mode = USER_EMPTY`, so all freshly declared slots start as free. When `AcceptUser` successfully completes the TCP accept and WinSock registration, it sets `Mode = USER_ACCEPT`, marking the slot as occupied by a connected peer. Every consumer of the `pUser`/`pAdmin` arrays gates its behavior on this field: broadcast loops skip slots whose `Mode == USER_EMPTY`, the accept handler refuses to bind into a slot that is not empty, and the disconnect handler resets a slot back to `USER_EMPTY` when the peer closes. This binary state is the backbone of the entire session model and is the primary invariant the component enforces. Because the state has only two values, there is no intermediate "connecting" or "authenticating" state stored in `Mode`; any finer-grained condition (such as admin authentication progress) is tracked separately through fields like `Level` and the challenge codes.

**Rule workflow:**
```
1. Slot constructed -> Mode = USER_EMPTY (free)
2. AcceptUser succeeds -> Mode = USER_ACCEPT (connected)
3. Peer disconnects (FD_CLOSE / receive failure)
   -> pUser[User].cSock.CloseSocket(); Mode = USER_EMPTY
4. Slot is free again for a future connection
```

---

### Business Rule: Game-Server Accept IP Validation

**Overview:**
When a game server attempts to connect, DBSrv validates the source IP before committing the connection to a persistent slot. This is the first gate that prevents unauthorized peers from registering.

**Detailed description:**
On `WSA_ACCEPT`, `TempUser.AcceptUser` records the peer IP from `acc_sin.sin_addr`. The accept handler then searches the `pUser[]` array for an existing slot already bound to that IP. If found, that slot index is reused. If not found, the handler scans the configured server list `g_pServerList[ServerIndex][1..MAX_SERVERNUMBER-1]` looking for a textual match against the dotted-quad string form of the peer IP. If the IP appears in neither the already-bound slots nor the configured server list, the connection is rejected: `TempUser.cSock.CloseSocket()` is called and an error is logged ("err wsa_accept request from %s"). This rule ensures only game servers that are pre-declared in the server-list configuration can establish a session, effectively forming an IP-based allowlist at the transport layer. The check is enforced entirely in `Server.cpp:1095-1115` before any slot is committed.

**Rule workflow:**
```
1. accept() records peer IP in TempUser.IP
2. Scan pUser[0..MAX_SERVER) for matching IP -> reuse that slot
3. If not bound, scan g_pServerList[ServerIndex] for dotted-quad match -> slot = i-1
4. If neither matches -> close socket, log error, drop connection
5. Otherwise proceed to registration
```

---

### Business Rule: Local Network Subnet Registration Check

**Overview:**
In addition to the server-list allowlist, the accept handler requires the first three octets of the peer IP to match the local host IP (`LocalIP[0..2]`). Only connections from the same /24-style subnet are permitted to complete registration.

**Detailed description:**
After the slot is resolved, `Server.cpp:1150` compares `cIP[0]`, `cIP[1]`, and `cIP[2]` (the high three octets of the peer) against `LocalIP[0]`, `LocalIP[1]`, and `LocalIP[2]` (the high three octets of the DB server's own address, captured at startup via `gethostname`/`getaddrinfo` in `WinMain`). If they match, the connection is committed to the slot, the `_MSG_DBSetIndex` registration message is sent, and guild/war/ally state is pushed to the new session. If they do not match, the connection is closed and the error "err,wsa_accept outer ethernet IP" is logged. This effectively restricts game-server registration to peers on the same LAN subnet, a legacy trust boundary assumption. The consequence is that game servers on a different subnet cannot register even if listed in the server-list configuration.

**Rule workflow:**
```
1. Peer IP octets compared to LocalIP[0..2]
2. Match -> bind slot, send _MSG_DBSetIndex, push guild state
3. No match -> TempUser.cSock.CloseSocket(), log "outer ethernet IP", drop
```

---

### Business Rule: Admin Accept IP Allowlist

**Overview:**
The admin (NP tool) accept path is stricter than the game-server path: the peer IP must match an entry in the `pAdminIP[]` allowlist loaded from `Admin.txt`, unless the peer is already bound to a slot.

**Detailed description:**
On `WSA_ACCEPTADMIN`, `ReadAdmin()` re-reads `Admin.txt` and populates `pAdminIP[idx]` (each line is `idx a.b.c.d`, converted to an integer). `TempUser.AcceptUser(AdminSocket.Sock, WSA_READADMIN)` records the peer IP. The handler first scans the existing `pAdmin[]` slots for a matching IP; if found, that slot is reused. If not found, it scans the `pAdminIP[]` allowlist for the peer IP. Only if the IP is in the allowlist is the connection permitted; otherwise the socket is closed and "err, wsa_acceptadmin request accept from ..." is logged (`Server.cpp:995-1003`). This rule implements a dedicated admin allowlist separate from the game-server server-list, restricting who may connect to the admin port 8895. It is re-read on every accept so that edits to `Admin.txt` take effect without a restart.

**Rule workflow:**
```
1. ReadAdmin() reloads pAdminIP[] from Admin.txt
2. TempUser.AcceptUser records peer IP
3. Scan pAdmin[] slots for matching IP -> reuse slot
4. If not bound, scan pAdminIP[] allowlist
5. Match -> proceed to slot binding; no match -> close socket, log error
```

---

### Business Rule: Duplicate Session Prevention / Re-accept Guard

**Overview:**
When an accepted peer resolves to a slot that is already occupied (i.e., `Mode != USER_EMPTY`), DBSrv treats it as a conflicting/stale session and forcibly closes both the new incoming socket and the existing slot's socket.

**Detailed description:**
For the game-server path, if a peer binds to a slot whose `Mode != USER_EMPTY`, the handler logs "err wsa_accept no previous slot", closes the new `TempUser.cSock`, resets `TempUser.Mode = 0`, then closes the existing `pUser[User].cSock` and resets `pUser[User].Mode = 0` (`Server.cpp:1121-1135`). The admin path behaves similarly at `Server.cpp:1007-1018`. This is a simplistic "last connection wins" strategy that prevents two simultaneous sessions from sharing one slot. The rationale is that a stale/duplicate game-server connection should not be allowed to hold a slot indefinitely; the existing occupant is evicted and the new connection is dropped, leaving the slot free. There is no sophisticated takeover or hand-off; both sockets are closed, so the peer must reconnect.

**Rule workflow:**
```
1. Resolve slot index for the new connection
2. If pUser[User].Mode != USER_EMPTY:
   - close new TempUser socket, reset Mode=0
   - close existing pUser[User] socket, reset Mode=0
   - break (connection dropped)
3. Otherwise proceed to normal slot binding
```

---

### Business Rule: Slot Capacity Limits

**Overview:**
The DBSrv server is hard-capped at `MAX_SERVER = 10` game-server slots and `MAX_ADMIN = 10` admin slots, defined in `legacy/Code/Basedef.h:48` and `:58`. These limits bound the size of the `pUser[]` and `pAdmin[]` arrays.

**Detailed description:**
The arrays are statically declared as `CUser pUser[MAX_SERVER]` and `CUser pAdmin[MAX_ADMIN]` (`Server.cpp:82-83`). Every loop that iterates peers bounds itself by these constants (for example, all broadcast loops use `for(int i = 0; i < MAX_SERVER; i++)`). The `GetEmptyUser()` and lookup functions also bound their scans by `MAX_SERVER`/`MAX_ADMIN`. Because the slot count is a compile-time constant, the server cannot dynamically scale beyond 10 game-server and 10 admin connections without recompilation. This is a fundamental architectural constraint: the maximum number of distinct game servers that can attach to this DB server is 10, and the number of concurrent admin tools is 10. Related constants include `MAX_USER = 1000` (the per-server player id space) and `MAX_DBACCOUNT = MAX_USER * MAX_SERVER = 10000` (total simultaneous player accounts across all servers).

**Rule workflow:**
```
1. Arrays sized at compile time: pUser[MAX_SERVER], pAdmin[MAX_ADMIN]
2. All scans/broadcasts bounded by MAX_SERVER / MAX_ADMIN
3. More than 10 game servers cannot register (no free slot)
```

---

### Business Rule: Socket-to-Slot Resolution (Linear Scan)

**Overview:**
Incoming WinSock events identify peers by socket handle. DBSrv maps a socket handle back to a `CUser` slot index by linearly scanning the arrays and comparing `cSock.Sock`.

**Detailed description:**
`GetUserFromSocket(int Sock)` (`Server.cpp:2169-2178`) iterates `i` from 0 to `MAX_SERVER`, returning the first `i` where `pUser[i].cSock.Sock == (unsigned)Sock`, or `-1` if no slot matches. `GetAdminFromSocket` (`Server.cpp:2180-2189`) does the same for `pAdmin[]`. These functions are invoked at the top of the `WSA_READ` and `WSA_READADMIN` handlers to determine which slot an incoming data/close event belongs to. If the lookup returns `-1`, the socket is treated as unregistered: it is closed and the event is logged as "err wsa_read unregistered game server socket" / "unregistered sever socket". This is an O(n) lookup with n ≤ 10, acceptable for the small slot count. The mapping is purely by socket handle, so the same slot's `Mode` is used to guard further processing.

**Rule workflow:**
```
1. WinSock event delivers a socket handle (wParam)
2. GetUserFromSocket / GetAdminFromSocket scans for matching cSock.Sock
3. Match -> return slot index; no match -> return -1 (socket closed)
```

---

### Business Rule: Admin Authentication Challenge-Response

**Overview:**
Admin sessions are authenticated after the TCP accept via a challenge/response handshake using two random codes and the account's stored password.

**Detailed description:**
Upon committing an admin slot (`Server.cpp:1052-1065`), DBSrv generates `Encode1 = rand() % 10000` and `Encode2 = rand() % 10000` and sends them to the peer in an `_MSG_NPReqIDPASS` message. The admin tool must reply with `_MSG_NPIDPASS` carrying the account name, password, and the received challenge codes. In `ProcessAdminMessage`'s `_MSG_NPIDPASS` case (`Server.cpp:1680-1736`), the handler first verifies that `pAdmin[conn].Encode1 == m->Encode1 && pAdmin[conn].Encode2 == m->Encode2`; a mismatch returns `FALSE`, terminating the session. It then requires `pAdmin[conn].Level == -1` (i.e., not yet authenticated) before proceeding. The account is read from the database; accounts whose password starts with `_` or `@` are rejected. The supplied password is compared against the stored password with `strncmp`. If the password matches, the admin's privilege level is computed as the highest character level on the account minus 1000, but only if that maximum is at least 1000 (`if(maxlevel < 1000) return FALSE;`). This means admin access requires a character of level ≥ 1000, and the resulting `Level` encodes privilege (e.g., level ≥ 2 for disable/enable operations, > 2 for account saves). Finally, `pAdmin[conn].Name` records the account and the login is logged.

**Rule workflow:**
```
1. Slot bound -> Encode1/Encode2 random, sent as _MSG_NPReqIDPASS
2. Peer replies _MSG_NPIDPASS with account, pass, Encode1, Encode2
3. Verify Encode1/Encode2 match the slot's stored codes -> else terminate
4. Require Level == -1 (unauthenticated)
5. Read account; reject if pass starts with '_' or '@'
6. Compare supplied pass to stored pass -> else terminate
7. Compute max character level; reject if < 1000
8. Level = maxlevel - 1000; Name = account; log success
```

---

### Business Rule: Session Account Counting (pUser[conn].Count)

**Overview:**
Each game-server `CUser` slot carries a `Count` field that tracks how many player accounts are currently logged in through that game server. It is incremented and decremented by the account-list management functions.

**Detailed description:**
`CFileDB::AddAccountList(int Idx)` (`CFileDB.cpp:2202-2219`) increments `pUser[conn].Count++` (where `conn = Idx / MAX_USER`) when a player account is registered into the active session list, and sets `Login = TRUE`. `CFileDB::RemoveAccountList(int Idx)` (`CFileDB.cpp:2221-2234`) decrements `pUser[conn].Count--` and clears the account when the player disconnects. This `Count` is the per-server concurrency gauge. It feeds directly into `CReadFiles::UpdateConnection` (`CReadFiles.cpp:79-82`), which tracks a running maximum in `UserConnection[i]` and writes a derived figure to a web-visible status file. The `Count` is also displayed in the server's GUI status panel via `DrawConfig` (`Server.cpp:148`). This makes `Count` a primary observability and reporting metric for how loaded each attached game server is.

**Rule workflow:**
```
1. Account logs in -> AddAccountList(Idx) -> pUser[conn].Count++
2. Account disconnects -> RemoveAccountList(Idx) -> pUser[conn].Count--
3. Reporting: UpdateConnection() -> if(UserConnection[i] < pUser[i].Count) track max;
   write (4*Count/3 + rand()%4) to serv%2.2d.htm
```

---

### Business Rule: Reserved Account-Name Prefixes

**Overview:**
Account names beginning with `COM` followed by a digit, or `LPT` followed by a digit, are reserved and rejected throughout the DB layer.

**Detailed description:**
A repeating defensive check appears in `AddAccount` (`CFileDB.cpp:86-89`), `UpdateAccount` (`:153-156`), `DBWriteAccount` (`:2400-2403`), `DBExportAccount` (`:2481-2484`), `GetAccountByChar` (`:2641-2644`), `CreateCharacter` (`:2244-2247`), and the login/new-account message handlers (`:520-521`, `:619-620`, `:906-907`). The pattern is: after uppercasing, if the first three characters are `C`, `O`, `M` (or `L`, `P`, `T`) and the fourth character is a digit `'0'..'9'` and the fifth character is the null terminator, the operation fails. This mirrors Windows device-name reservation (COM1..COM9, LPT1..LPT9) and prevents the account/character file path from colliding with reserved device names or with the internal `COM#`/`LPT#` account scheme used elsewhere in the system. The check guards both the account-name write path and the character-name creation path.

**Rule workflow:**
```
1. Uppercase the input account/character name
2. Test prefix: "COM"+digit+'\0' OR "LPT"+digit+'\0'
3. Match -> return FALSE / reject the operation
4. No match -> proceed with account/character creation or write
```

---

### Business Rule: Broadcast Only to Live Slots

**Overview:**
Broadcast-type DB messages (guild info, war, ally, notices, magic trumpet, import item/donate, primary-account checks, etc.) are fanned out only to `CUser` slots that are both non-empty and have a live socket.

**Detailed description:**
Across `CFileDB.cpp`, `CReadFiles.cpp`, and `CRanking.cpp`, every multi-peer broadcast first guards each slot with `if(pUser[i].Mode == USER_EMPTY) continue;` and, in many cases, a second guard `if(pUser[i].cSock.Sock == 0) continue;` before calling `pUser[i].cSock.SendOneMessage(...)`. Examples: `_MSG_GuildZoneReport` (`CFileDB.cpp:377-384`), `_MSG_War` (`:409-416`), `_MSG_GuildAlly` (`:443-450`), `_MSG_GuildInfo` (`:468-478`), `_MSG_DBUpdateSapphire` (`:497-504`), `_MSG_MessageDBImple` (`:601-608`), `_MSG_MagicTrumpet` (`:1593-1609`), `_MSG_DBNotice` (`:1614-1621`), primary-account checks (`:794-800`, `:2084-2090`), import-item/donate runtime delivery (`CReadFiles.cpp:253`, `:829`), and ranking updates (`CRanking.cpp:312-313`). This dual guard ensures that no message is sent on a closed or vacant socket, avoiding writes to stale file descriptors. The same pattern is used for admin broadcasts (`_MSG_NPAppeal`, `CFileDB.cpp:581-588`).

**Rule workflow:**
```
1. Iterate i over MAX_SERVER / MAX_ADMIN
2. Skip slot if Mode == USER_EMPTY
3. Skip slot if cSock.Sock == 0
4. Otherwise SendOneMessage(...) to pUser[i]/pAdmin[i].cSock
```

---

### Business Rule: Session Save-Quit Delivery Guard

**Overview:**
`SendDBSavingQuit` delivers a save/quit signal to a game server only when the corresponding slot is live, mirroring the broadcast guard in the targeted (non-broadcast) path.

**Detailed description:**
`CFileDB::SendDBSavingQuit(int Idx, int mode)` (`CFileDB.cpp:2369-2388`) resolves `conn = Idx / MAX_USER` and `id = Idx % MAX_USER`, builds an `_MSG_DBSavingQuit` message, and sends it only `if(pUser[conn].cSock.Sock && pUser[conn].Mode != USER_EMPTY)` (`:2384-2385`). This is the single-slot analog of the broadcast rule: it ensures that a "saving quit" (forced logout / save-and-kick) instruction is only transmitted to a game server that is actually connected and live. The message is used, for example, when an admin disables an account that is currently playing (`Server.cpp:1949`, `CFileDB.cpp:700`) or on logout, to instruct the game server to save and terminate the player session. The guard prevents sending into a dead socket if the game server has already disconnected.

**Rule workflow:**
```
1. Resolve conn = Idx / MAX_USER, id = Idx % MAX_USER
2. Build _MSG_DBSavingQuit with mode
3. If pUser[conn].cSock.Sock && pUser[conn].Mode != USER_EMPTY:
   -> send; otherwise silently skip
```

---

## 4. Component Structure

The `CUser` component consists of the class definition, its implementation, and the global session arrays that instantiate it. It is built as part of the `DBSrv` project (`legacy/Code/DBSrv/DBSrv.vcxproj`).

```
legacy/Code/
├── DBSrv/                          # DBSrv project root
│   ├── CUser.h                     # CUser class declaration (component core)
│   ├── CUser.cpp                   # CUser constructor/destructor + AcceptUser (component core)
│   ├── Server.cpp                  # MainWndProc session lifecycle, accept/lookup/disconnect logic
│   ├── Server.h                    # extern declarations of pUser/pAdmin/TempUser
│   ├── CFileDB.cpp / CFileDB.h     # Business/persistence layer consuming pUser/pAdmin
│   ├── CReadFiles.cpp/.h           # File import + connection reporting using pUser
│   ├── CRanking.cpp/.h             # Ranking broadcast using pUser
│   └── DBSrv.vcxproj / .filters    # MSBuild project definition
├── Basedef.h / Basedef.cpp         # Shared constants (MAX_SERVER, MAX_ADMIN, ACCOUNTNAME_LENGTH)
└── CPSock.h / CPSock.cpp           # Socket abstraction (cSock member of CUser)
```

**Component boundary annotation:**

| File | Role within the component |
|------|---------------------------|
| `legacy/Code/DBSrv/CUser.h` | Declares the `CUser` class, its public data members, and `AcceptUser`; defines `USER_EMPTY`/`USER_ACCEPT`. |
| `legacy/Code/DBSrv/CUser.cpp` | Implements the constructor (defaults `Mode=USER_EMPTY`, `Level=-1`), empty destructor, and `AcceptUser` (accept + async-select registration). |
| `legacy/Code/DBSrv/Server.cpp` | Declares the three `CUser` instances (`TempUser`, `pUser[MAX_SERVER]`, `pAdmin[MAX_ADMIN]`) and implements the accept/disconnect/lookup logic that drives them. |
| `legacy/Code/DBSrv/Server.h` | Exposes `extern CUser pUser/pAdmin/TempUser` and the helper function prototypes. |
| `legacy/Code/DBSrv/CFileDB.cpp` | Uses `pUser`/`pAdmin` for targeted replies, broadcasts, account-count tracking, and save-quit delivery. |
| `legacy/Code/DBSrv/CReadFiles.cpp` | Uses `pUser` for connection reporting and runtime item/donate delivery. |
| `legacy/Code/DBSrv/CRanking.cpp` | Uses `pUser` to route ranking update packets. |

---

## 5. Dependency Analysis

**Internal Dependencies (within the DBSrv component and shared legacy code):**

```
CUser (class)
   ├── CPSock cSock            (composition: socket + buffers)   -> legacy/Code/CPSock.h
   ├── ACCOUNTNAME_LENGTH      (constant)                        -> legacy/Code/Basedef.h:65
   └── hWndMain (extern HWND)  (used by AcceptUser)              -> legacy/Code/DBSrv/Server.cpp:44

Server.cpp (session manager)
   ├── CUser (TempUser, pUser, pAdmin)                           -> DBSrv/CUser.h
   ├── CFileDB (cFileDB)                                         -> DBSrv/CFileDB.h
   ├── CPSock (ListenSocket, AdminSocket, AdminClient)           -> CPSock.h
   └── CReadFiles (ImportUser, UpdateConnection, ...)            -> DBSrv/CReadFiles.h

CFileDB.cpp
   ├── CUser (extern pUser, pAdmin)                              -> DBSrv/CUser.h
   ├── CPSock (AdminClient)                                      -> CPSock.h
   ├── CReadFiles / CRanking / Basedef                           -> shared
   └── CFileDB::SendDBSignal*(...) call pUser[svr].cSock         -> Server.h globals

CReadFiles.cpp
   └── CUser (extern pUser via Server.h), CFileDB, CRanking      -> shared

CRanking.cpp
   └── CUser (extern pUser), CFileDB, Basedef                    -> shared
```

**External Dependencies:**

| Dependency | Kind | Purpose | Notes |
|------------|------|---------|-------|
| Windows Sockets 2 (winsock2.h / ws2_32.lib) | System library | TCP accept, async-select, send/recv | Linked via `ws2_32.lib` in `DBSrv.vcxproj:70` |
| Win32 GUI (user32/gdi32/kernel32) | System library | `hWndMain` window, message pump, timers | Win32 application subsystem |
| CRT / file I/O | System library | File-based account persistence | Account files under `./account/...` |
| Random (rand()) | CRT | Challenge codes + connection-report noise | `Server.cpp:1052-1053`, `CReadFiles.cpp:82` |

**Relationship chains:**

```
GameServer(TMSrv) --TCP/7514--> ListenSocket --WSA_ACCEPT--> CUser::AcceptUser --> pUser[slot]
                                                                                     |
                                   CFileDB::ProcessMessage <-- ProcessClientMessage <--+
                                                     |
                                                     +--> pUser[conn].cSock.SendOneMessage(...)

NPTool --TCP/8895--> AdminSocket --WSA_ACCEPTADMIN--> CUser::AcceptUser --> pAdmin[slot]
                                                         |
                                    ProcessAdminMessage <--+  (challenge/response)
```

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the class level (C++ classes/structs) and at the file level for the procedural consumers, consistent with the object-oriented nature of the code.

| Component (Class/Consumer) | Afferent Coupling | Efferent Coupling | Critical |
|----------------------------|-------------------|-------------------|----------|
| `CUser` (DBSrv) | 5 (Server.cpp, CFileDB.cpp, CReadFiles.cpp, CRanking.cpp, Server.h) | 4 (CPSock, Basedef.h, Windows.h, hWndMain extern) | High |
| `CPSock` | 6 (CUser, Server, CFileDB, CReadFiles, CRanking, AdminClient) | 2 (Windows sockets, internal buffers) | High |
| `CFileDB` | 3 (Server.cpp, CReadFiles.cpp, CRanking.cpp) | 6 (CUser, CPSock, CReadFiles, CRanking, Basedef, file I/O) | High |
| `CReadFiles` | 1 (Server.cpp -> ProcessSecTimer) | 5 (CUser, CFileDB, CRanking, Basedef, file I/O) | Medium |
| `CRanking` | 2 (CFileDB.cpp, CReadFiles.cpp) | 4 (CUser, CFileDB, Basedef, sockets) | Medium |
| Server.cpp procedural session layer | 0 (top-level) | 6 (CUser, CFileDB, CPSock, CReadFiles, Basedef, Win32) | High |

**Coupling analysis.** `CUser` exhibits moderately high afferent coupling because it is a shared global session record touched by nearly every subsystem of the DB server. Its efferent coupling is low (it depends only on `CPSock`, constants, and Windows), but because it embeds a `CPSock` by value and references the global `hWndMain`, it is not a self-contained unit. The `pUser[]`/`pAdmin[]` arrays are global mutable state shared across translation units via `extern`, so the class's afferent coupling is effectively to every module that touches a connection. This is characteristic of a legacy global-state architecture: cohesion is high within each file but the coupling across files is high and implicit.

---

## 7. Endpoints

The `CUser` component does not expose application-level HTTP/REST/gRPC/GraphQL endpoints. It operates at the TCP socket / WinSock async-message layer. The transport-level entry points it participates in are the two listening TCP ports and the async-message events dispatched to `MainWndProc`:

| Endpoint | Protocol | Direction | Handler | Description |
|----------|----------|-----------|---------|-------------|
| `DB_PORT` (7514) | TCP (WinSock async) | Inbound | `WSA_ACCEPT` / `WSA_READ` | Game-server registration and data channel |
| `ADMIN_PORT` (8895) | TCP (WinSock async) | Inbound | `WSA_ACCEPTADMIN` / `WSA_READADMIN` | NP admin tool channel |
| `WSA_READADMINCLIENT` | WinSock async | Outbound (client) | `AdminClient` | Redirection server connection |

These are WinSock `WM_USER+`-based async notification events rather than structured service endpoints; all higher-level "endpoints" are the game/message protocol types (e.g., `_MSG_DBAccountLogin`, `_MSG_NPReqAccount`) dispatched through `CFileDB::ProcessMessage` / `ProcessAdminMessage`.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Game servers (TMSrv) | Internal peer (TCP) | Character/account persistence, session state | TCP + WinSock async select | Binary messages (`MSG_STANDARD` et al.) | Socket closed + `Mode=USER_EMPTY` on receive failure/FD_CLOSE |
| NP Admin tool | Internal peer (TCP) | Account administration (disable/enable/save/notice) | TCP + WinSock async select | Binary messages (`_MSG_NP*`) | Challenge/response; socket close on mismatch; `Level` gating |
| Redirection server | External peer (TCP client) | Cross-server character transfer | TCP (`AdminClient.ConnectServer`) | Binary messages | `redirect.txt` bootstrap; connect failure aborts startup |
| Account file store | Filesystem | Persistent account/character data | File I/O (`_open`/`_read`/`_write`) | Binary `STRUCT_ACCOUNTFILE` | `errno`-based logging on open/write failure |
| Admin allowlist (`Admin.txt`) | Config file | Authorized admin IPs | Text file read (`ReadAdmin`) | Text lines `idx a.b.c.d` | Invalid index skipped |
| Status files (`serv%2.2d.htm`, `data%2.2d.csv`) | Filesystem out | Connection/reporting | Text write | Derived from `pUser[i].Count` | Silent return if file cannot be opened |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Global singleton / shared state | `CUser pUser[]`, `pAdmin[]`, `TempUser` declared once, shared via `extern` | legacy/Code/DBSrv/Server.cpp:80-83; Server.h:104-107 | Provide session state to all subsystems without DI |
| WinSock async select | `WSAAsyncSelect(tSock, hWndMain, wsa, FD_READ\|FD_CLOSE)` in `AcceptUser`; message-driven `MainWndProc` | legacy/Code/DBSrv/CUser.cpp:49; Server.cpp:743 | Drive connection I/O via window messages (single-threaded UI pump) |
| Accept-buffer staging | `TempUser` as a staging slot before committing to `pUser`/`pAdmin` | legacy/Code/DBSrv/Server.cpp:962, 1076 | Validate/attribute a new connection before committing it |
| Repository / persistence facade | `CFileDB` wraps file-based account persistence | legacy/Code/DBSrv/CFileDB.h | Centralize DB reads/writes |
| Gateway / facade for sends | `SendDBSignal*` family centralizes message sending over `pUser[svr].cSock` | legacy/Code/DBSrv/CFileDB.cpp:2108-2168 | Reusable, guarded send helpers |
| Challenge-response handshake | Random `Encode1`/`Encode2` for admin login | legacy/Code/DBSrv/Server.cpp:1052-1065, 1686 | Authenticate admin peers |
| Broadcast fan-out | Iterate live slots for guild/war/notice propagation | legacy/Code/DBSrv/CFileDB.cpp:377-621 | Replicate shared state to all game servers |

**Architectural decisions.** The DB server is a single-threaded, message-pump-driven Win32 application. All network I/O is asynchronous and demultiplexed into the window message loop via `WSAAsyncSelect`, which is why `CUser` embeds an `HWND`-bound socket registration. Session state is deliberately global and mutable, traded off against simplicity: every connected peer is reachable anywhere via `pUser[i]`/`pAdmin[i]`. Persistence is file-based (no SQL), and account files are organized under `./account/<first-key>/<NAME>`. Admin access is delegated to a per-account privilege level derived from in-game character level, a legacy design that ties GM/operator authority to game progression.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Session state | `Mode` is a 2-state value only; no per-connection authentication/phase state | Cannot distinguish "connected but unauthenticated" from "fully authorized" at the session level for game servers; relies on ad-hoc fields |
| High | Auth | Admin privilege derived from in-game character level (`maxlevel - 1000`) | Operator authority is coupled to game progression; an account with a level-1000+ character can become an admin |
| High | Security | Admin password compared in plaintext with `strncmp` against stored password | No hashing; password stored/compared in the clear (`Server.cpp:1708`) |
| Medium | Robustness | Duplicate-session guard closes both sockets ("last wins") with no handoff | In-flight state is lost; peer must reconnect, risking interrupted sessions |
| Medium | Trust boundary | Game-server registration gated on IP allowlist + local subnet octet match | Any host on the LAN subnet that spoofs a listed IP could register |
| Medium | Resource safety | `accept()` result and `WSAAsyncSelect` return are checked, but send paths rely on `Sock != 0` guards | No explicit error propagation on partial/failed sends |
| Medium | Maintainability | `CUser` members exposed as public; shared mutable globals via `extern` across 4 files | High blast radius; hard to reason about concurrency/state transitions |
| Low | Correctness | `GetEmptyUser()` is defined (`Server.cpp:2191-2200`) but its direct callers are limited | Possible dead code / unused helper |
| Low | Consistency | `DrawConfig` reports `pUser[i].Count` while connection report uses `4*Count/3 + rand()%4` | Displayed load vs. reported load differ by a scaling heuristic |
| Low | Error handling | `Log()` writes to a single global `fLogFile`; log-file close failures only logged | Logging is best-effort; no rotation beyond daily restart via `StartLog` |

---

## 11. Test Coverage Analysis

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `CUser` (DBSrv) | 0 | 0 | Not measurable (no test harness) | N/A - no tests exist |

**Findings.**

- A repository-wide search for test files (`*test*`, `*spec*`, and directories named `test`/`spec`) returned **no results** across the entire project (excluding `.git` and `.opencode`).
- There is no unit-test framework, no integration-test suite, and no CI configuration present in the repository.
- The `CUser` accept/lifecycle logic (`AcceptUser`, the accept handlers, the lookup and broadcast helpers) has no automated verification. Its behavior is exercised only by manual runtime testing against live game servers and the NP tool.
- This is a legacy 2012-2015 Win32 emulator codebase whose reliability depends on the original operators' manual testing rather than an automated test strategy.
- **Risk:** the session state machine, IP allowlist logic, admin challenge/response, and account-count accounting are critical, untested paths. Any regression in these areas (e.g., a slot not being reset to `USER_EMPTY`, or a broadcast guard failing) would be silently observable only through runtime behavior or log messages.

---

## 12. Report Metadata

- **Analyzed component:** CUser (DBSrv)
- **Component boundary:** `legacy/Code/DBSrv/CUser.h`, `legacy/Code/DBSrv/CUser.cpp`, and their consumers in `Server.cpp`, `CFileDB.cpp`, `CReadFiles.cpp`, `CRanking.cpp` (with `CPSock`/`Basedef` as shared dependencies).
- **Folders ignored:** `.git`, `.opencode`
- **Files modified:** none (analysis only).
