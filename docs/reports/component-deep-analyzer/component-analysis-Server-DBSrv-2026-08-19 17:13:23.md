# Component Deep Analysis Report

**Component:** Server (DBSrv)
**Project:** W2PP (legacy C/C++ MMO game server)
**Component location:** `legacy/Code/DBSrv/` (primary: `Server.cpp` / `Server.h`)
**Scope analyzed:** `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy`
**Date:** 2026-08-19 17:13
**Folders ignored:** `.git`, `.opencode`
**Analysis mode:** read-only, no project files modified

---

## 1. Executive Summary

The **Server (DBSrv)** component is the central **database/accounts server** of the W2PP
legacy MMO server stack. It is the persistence and coordination hub that (a) **accepts
connections from one or more game servers** (TMSrv instances) over a raw TCP socket on
port `DB_PORT` (7514), (b) **accepts connections from an administrative tool** (NPTool)
over a second raw TCP listener on port `ADMIN_PORT` (8895), and (c) optionally **connects
outbound to a redirect/transfer server** as a client to support character transfer.

The component is implemented in `legacy/Code/DBSrv/`, with the entry point and main loop
in `Server.cpp` (2,342 lines) and `Server.h`. It is a **Win32 GUI message-pump application**,
single-threaded, driven by `WSAAsyncSelect` asynchronous sockets that deliver network
events as window messages to `MainWndProc`. All processing is serialized on the UI thread.

The component's responsibilities are:
- **Main loop / event dispatch** (`WinMain`, `MainWndProc`, `ProcessSecTimer`, `ProcessMinTimer`).
- **Game-server connection management** (accept, IP validation, session slot allocation).
- **Admin-tool connection management** (accept, IP allow-list, challenge/response handshake).
- **Account/character persistence** through the `CFileDB` file-backed store (account files
  under `./account/<first-letter>/<name>` and character index files under `./char/...`).
- **Data import/export** through `CReadFiles` (items, users, donations, guild info, ranking).
- **Ranking subsystem** through `CRanking`.
- **Coordination state** broadcast back to every connected game server (guild war/ally,
  guild info, sapphire rate, notices).

**Key findings:**
- The component is the **single point of persistence** for all TMSrv instances; a crash or
  downtime affects account login/character operations for the whole server cluster.
- It relies on a **custom, header-defined binary wire protocol** (`Basedef.h`), with packet
  flags (`FLAG_GAME2DB`, `FLAG_NP2DB`, etc.) used to validate the origin/direction of each
  message before dispatch.
- **File-based persistence with no transactionality**: account files are written directly
  with `_write`, with no locking, journaling, or atomic rename; a crash mid-write can
  corrupt an account file.
- **Account passwords are stored and compared in plaintext** (`STRUCT_ACCOUNTINFO.AccountPass`,
  compared with `strcmp`/`strncmp`). Block/disabled states are encoded as reserved
  first-character prefixes (`@`, `_`, `#`).
- **Admin access control** is IP-allow-list based (`pAdminIP` from `Admin.txt`) plus a
  lightweight random `Encode1`/`Encode2` challenge; admin privilege level is derived from the
  in-game character level (`maxlevel - 1000`).
- **No automated tests** exist anywhere in the analyzed scope (CodeGraph reports no covering
  tests for any handler in this component).

---

## 2. Data Flow Analysis

The following traces the primary flows through the component.

### 2.1 Game-server (TMSrv) connect & session setup
```
1. TMSrv connects to ListenSocket on DB_PORT (7514)
2. MainWndProc receives WSA_ACCEPT
3. CUser::AcceptUser() accepts socket and registers WSAAsyncSelect(wParam=WSA_READ)
4. Server.cpp validates TempUser.IP against g_pServerList[ServerIndex][1..MAX_SERVERNUMBER-1]
5. Slot allocated in global pUser[User]; if local subnet, session state is copied
6. DBSrv sends _MSG_DBSetIndex (ServerIndex, Sapphire, User) to the TMSrv
7. DBSrv sends guild info / war / ally state for all 65536 guild slots
8. If TransperCharacter enabled, sends _MSG_TransperCharacter
```

### 2.2 Game-server packet processing (login / char / save)
```
1. TMSrv sends a packet; MainWndProc receives WSA_READ
2. GetUserFromSocket() maps socket -> pUser[User]
3. CPSock::Receive() buffers bytes; ReadMessage() extracts a framed message
4. Server.cpp::ProcessClientMessage(conn, msg) validates FLAG_GAME2DB + ID bounds
5. cFileDB.ProcessMessage(msg, conn) dispatches on packet Type
6. Handler performs file I/O via DBReadAccount / DBWriteAccount / DBExportAccount
7. Response is built (MSG_DBCNFAccountLogin / MSG_DBCNFCharacterLogin / etc.)
8. pUser[svr].cSock.SendOneMessage() sends the response back to the originating TMSrv
```

### 2.3 Admin-tool (NPTool) connect & command
```
1. NPTool connects to AdminSocket on ADMIN_PORT (8895)
2. MainWndProc receives WSA_ACCEPTADMIN
3. TempUser.AcceptUser() accepts; IP validated against pAdminIP allow-list (Admin.txt)
4. Slot allocated in pAdmin[User]; Encode1/Encode2 random values generated
5. MSG_NPIDPASS challenge sent to the admin tool
6. WSA_READADMIN -> GetAdminFromSocket() -> ProcessAdminMessage(conn, msg)
7. Admin authenticates by echoing Encode1/Encode2 + account/password; Level derived
8. Admin commands (account query, disable/enable, notice, donate) are handled
9. Responses sent via pAdmin[conn].cSock.SendOneMessage
```

### 2.4 Redirect/transfer server (outbound client)
```
1. On boot, DBSrv reads redirect.txt -> sip, port, adminclientid, adminclientpass
2. AdminClient.ConnectServer() connects outbound; TransperCharacter = 1
3. WSA_READADMINCLIENT -> ProcessAdminClientMessage() handles create-character replies
4. Used to move characters between servers (character transfer / transper)
```

### 2.5 Periodic maintenance (timers)
```
1. SetTimer(TIMER_SEC, 1000ms) -> ProcessSecTimer() every second
2. ImportUser() / UpdateUser() run every second
3. Every 30s: ImportItem(), ImportDonate(), UpdateConnection(), MinCounter++
4. Every 30 min (MinCounter%30==0): UpdateConnectionData(), HourCounter++
5. Every 600s: WriteRanking()
6. Daily midnight (Sunday, hour==0): reset guild Fame, WriteGuildInfo()
7. New day: StartLog("A") and DayLog_ItemLog() / DayLog_ExpLog()
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Game-server socket accepted only if IP is in `g_pServerList[ServerIndex]` and on local subnet | `Server.cpp:1086-1188` |
| Validation | Admin socket accepted only if IP matches `pAdminIP[]` allow-list (from `Admin.txt`) | `Server.cpp:968-1028`, `Server.cpp:353-391` |
| Security | Admin auth requires matching `Encode1`/`Encode2` challenge plus valid account/password | `Server.cpp:1680-1736` |
| Security | Admin privilege level = highest character level minus 1000; login requires level >= 1000 | `Server.cpp:1713-1727` |
| Security | Admin account rejected if password begins with `_` or `@` (reserved block/defect states) | `Server.cpp:1701-1704` |
| Validation | Account name must not be a `COMx`/`LPTx` device name (x = 0-9) | `CFileDB.cpp:86-89, 520-522, 619-625, 2400-2403, 2531-2534` |
| Validation | Character name must not be `COMx`/`LPTx` or reserved command names (`KING`,`KINGDOM`,`GRITAR`,`RELO`) | `CFileDB.cpp:897-914, 1461-1478, 2249-2256, 2311-2314` |
| Validation | Character name must not contain the double `í` (Korean) character sequence | `CFileDB.cpp:940-950, 1504-1514, 1864-1874` |
| Business Logic | Account login compares password with `strcmp` (plaintext); rejection signals `_MSG_DBAccountLoginFail_Pass` | `CFileDB.cpp:677-682` |
| Business Logic | Account block (ban) via `Year`/`YearDay`; rejected with `_MSG_DBAccountLoginFail_Block` if not expired | `CFileDB.cpp:648-660` |
| Business Logic | Dual-login prevented: already-logged account sends `_MSG_DBAlreadyPlaying` or `_MSG_DBStillPlaying` + `SendDBSavingQuit` | `CFileDB.cpp:685-703` |
| Business Logic | Server-change via `TempKey`; matching key bypasses password check and auto-logs in | `CFileDB.cpp:662-675, 802-853` |
| Business Logic | Character creation requires a free slot and a `SecurePass` already set | `CFileDB.cpp:856-1026, 882-889` |
| Business Logic | Character deletion requires matching account password, `ClassMaster` MORTAL/ARCH, and level < 219 | `CFileDB.cpp:1276-1355, 1293-1328` |
| Business Logic | Account disable/enable via admin (sets/clears `Year`/`YearDay`) | `Server.cpp:1359-1461`, `Server.cpp:1927-1980` |
| Business Logic | Sapphire (exp rate) doubles/halves, clamped to [1, 64] | `CFileDB.cpp:483-508` |
| Business Logic | Capsule system stores/retrieves characters in `./capsule/<id>` files | `CFileDB.cpp:1624-1927` |
| Business Logic | Guild war/ally state kept in `g_pGuildWar[65536]`/`g_pGuildAlly[65536]` and broadcast | `CFileDB.cpp:388-452` |
| Business Logic | Ranking (top 500 by exp) excludes characters with level >= 1000 (admin) | `CRanking.cpp:187-188`, `CReadFiles.cpp:1057` |
| Business Logic | Primary-account detection by MAC; broadcasts `_MSG_DBCheckPrimaryAccount` | `CFileDB.cpp:782-801, 2068-2101, 2351-2367` |
| Business Logic | Item import validates item index and effect ranges before crediting | `CReadFiles.cpp:209-221` |
| Business Logic | Donation import rejects negative amounts | `CReadFiles.cpp:796-808` |
| Business Logic | Name swap fix: swaps two characters if they hold `Equip[13]` indexes 774/775 | `CFileDB.cpp:707-743` |

### Detailed breakdown of the business rules

---

### Business Rule: Game-Server Connection Acceptance

**Overview**
The component accepts inbound TCP connections from game servers (TMSrv instances) on the
`DB_PORT` (7514) listener. Acceptance is gated by a two-stage IP validation: the source IP
must (1) match one of the configured game-server IPs in the shared server list
(`g_pServerList[ServerIndex][1..MAX_SERVERNUMBER-1]`), and (2) be on the same local subnet
as the DBSrv's own `LocalIP[0..2]`. This is the primary trust boundary between DBSrv and its
consumers; only validated game servers may reach the persistence layer.

**Detailed description**
When the `WSA_ACCEPT` window message fires (Server.cpp:1068), the component first checks
`WSAGETSELECTERROR` and aborts on a socket-level accept error. It then calls
`TempUser.AcceptUser(ListenSocket.Sock, WSA_READ)` (Server.cpp:1076), which performs the
actual `accept()`, registers `WSAAsyncSelect` for `FD_READ | FD_CLOSE`, and records the peer
IP in `TempUser.IP` with `Mode = USER_ACCEPT` (CUser.cpp:39-64). Next the handler searches
for an existing slot with the same IP in `pUser[]` (Server.cpp:1086-1093); if none is found,
it scans the shared `g_pServerList[ServerIndex]` entries (indices 1..MAX_SERVERNUMBER-1) for
a match against the source IP string (Server.cpp:1095-1118). A game server that does not
appear in the server list has its socket closed immediately and a log entry is written
(`err wsa_accept request from ...`).

If the IP is recognized, a slot `User` is derived (either the pre-existing slot or the
list position minus one). The handler then enforces that the target slot is empty
(`Mode == USER_EMPTY`); a duplicate/occupied slot closes both the new socket and the stale
one (Server.cpp:1119-1136). Finally, the handler enforces the local-subnet rule
(Server.cpp:1150): only connections whose first three IP octets match `LocalIP[0..2]` are
accepted; otherwise the socket is closed with `err wsa_accept outer ethernet IP`. On success,
the session state is copied from `TempUser` into `pUser[User]`, and DBSrv sends
`_MSG_DBSetIndex` with `(ServerIndex, Sapphire, User)` to that game server, followed by the
full guild/war/ally state for all 65536 guild slots, and `_MSG_TransperCharacter` if the
redirect server is active (Server.cpp:1159-1176).

**Rule workflow**
```
WSA_ACCEPT
  -> WSAGETSELECTERROR? abort
  -> AcceptUser()
  -> IP already in pUser[]?  User = that slot
     else scan g_pServerList[ServerIndex][1..N-1] for IP; not found -> close socket
  -> slot occupied? close both, abort
  -> local subnet (cIP[0..2] == LocalIP[0..2])? no -> close, abort
  -> copy session into pUser[User]
  -> send _MSG_DBSetIndex + guild/war/ally + (transper)
```

---

### Business Rule: Admin-Tool Connection Acceptance and Handshake

**Overview**
The component exposes a second listener, `AdminSocket`, on `ADMIN_PORT` (8895) for the
administrative tool (NPTool). Acceptance is restricted to an **IP allow-list** read from the
`Admin.txt` file into `pAdminIP[MAX_ADMIN]`. Each accepted admin session is issued a
**challenge/response handshake** using two random values (`Encode1`, `Encode2`) that the
admin tool must echo back along with a valid account credential before the session is
promoted from `Level = -1`.

**Detailed description**
At boot, `ReadAdmin()` (Server.cpp:353-391) parses `Admin.txt`, which contains lines of the
form `<index> <a> <b> <c> <d>`; it converts the dotted IP (little-endian) into a 32-bit value
and stores it in `pAdminIP[idx]` (skipping invalid indices). When `WSA_ACCEPTADMIN` fires
(Server.cpp:951), `ReadAdmin()` is re-read to refresh the allow-list, then
`TempUser.AcceptUser(AdminSocket.Sock, WSA_READADMIN)` accepts the socket. The handler first
searches existing `pAdmin[]` entries by matching IP; if not found, it searches the
`pAdminIP[]` allow-list (Server.cpp:968-1003). If the IP is not in the allow-list, the socket
is closed with `err, wsa_acceptadmin request accept from ...`. If the matching slot is already
occupied, both the new and previous sockets are closed (Server.cpp:1007-1019).

For an accepted admin, the component initializes `Level = -1` (unauthenticated), copies the
session state, and generates `Encode1 = rand() % 10000` and `Encode2 = rand() % 10000`
(Server.cpp:1052-1053). It then sends `MSG_NPIDPASS` (`_MSG_NPReqIDPASS`) containing these
two encode values (Server.cpp:1055-1065). The admin tool must later respond via
`_MSG_NPIDPASS` (processed in Server.cpp:1680) by echoing both values, providing an account
name and password, and the DBSrv verifies them before elevating the session.

**Rule workflow**
```
ReadAdmin() loads pAdminIP[] from Admin.txt
WSA_ACCEPTADMIN
  -> AcceptUser()
  -> IP in pAdmin[]? else IP in pAdminIP[]? no -> close, abort
  -> slot occupied? close both, abort
  -> Level=-1, Encode1/Encode2 = rand()%10000
  -> send _MSG_NPReqIDPASS(Encode1, Encode2)
  -> await _MSG_NPIDPASS response
```

---

### Business Rule: Admin Authentication and Privilege Level

**Overview**
Admin sessions authenticate by sending `_MSG_NPIDPASS` containing the echoed `Encode1`/
`Encode2` challenge, an account name, and a password. The DBSrv validates the challenge
values, rejects reserved block/defect accounts, verifies the plaintext password, and derives
the admin privilege `Level` from the **highest character level on the account minus 1000**.
An account must have at least one character at level 1000+ to be eligible.

**Detailed description**
In the `_MSG_NPIDPASS` handler (Server.cpp:1680-1736), the account name is uppercased. The
handler first requires `pAdmin[conn].Encode1 == m->Encode1 && pAdmin[conn].Encode2 ==
m->Encode2`; a mismatch returns `FALSE`, which causes the caller to close the socket
(Server.cpp:863-872). It also requires the session to still be in the unauthenticated state
(`Level == -1`); otherwise it returns `TRUE` (idempotent). The account file is read via
`DBReadAccount`; if it does not exist, the handler returns `TRUE` (no error response).
Accounts whose password begins with `_` (defective) or `@` (blocked) are rejected with
`FALSE` (Server.cpp:1701-1704). The password is compared with `strncmp(m->Pass, p,
ACCOUNTPASS_LENGTH)`; a mismatch returns `FALSE` (Server.cpp:1708-1711).

The handler then scans all four character slots for the highest `BaseScore.Level`
(Server.cpp:1715-1719). If `maxlevel < 1000`, the login is rejected (`FALSE`); otherwise
`admin = maxlevel - 1000` becomes the session's `Level` (Server.cpp:1721-1726). This `Level`
is the effective authorization token used by subsequent admin commands: level > 0 is needed
for account queries (`_MSG_NPReqAccount`, Server.cpp:1742), level >= 2 for global notice
broadcast (Server.cpp:1611), and level > 2 (i.e. >= 3) for account save (`_MSG_NPReqSaveAccount`,
Server.cpp:1852), disable (`_MSG_NPDisable`, Server.cpp:1929), and enable (`_MSG_NPEnable`,
Server.cpp:1959). On success the account name is stored in `pAdmin[conn].Name`, a log line is
written, and `_MSG_NPAccountInfo`-style feedback is sent via `SendAdminMessage`.

**Rule workflow**
```
_MSG_NPIDPASS
  -> Encode1/Encode2 match? no -> FALSE (close)
  -> Level == -1? no -> TRUE
  -> DBReadAccount; missing -> TRUE
  -> pass[0]=='_' or '@'? -> FALSE
  -> strncmp(Pass, AccountPass)? mismatch -> FALSE
  -> maxlevel = max(char levels); maxlevel<1000 -> FALSE
  -> Level = maxlevel - 1000
  -> authorize subsequent admin commands by Level
```

---

### Business Rule: Reserved Account and Character Name Validation

**Overview**
The component enforces a set of reserved-name rules to prevent collisions with operating
system device names and to prevent account/character names that could be abused for command
injection or aliasing. Two categories exist: (1) **device names** `COM0`-`COM9` and
`LPT0`-`LPT9` (matched in uppercase against the first four characters plus a numeric digit
and a null terminator), and (2) **reserved command names** for characters (`KING`, `KINGDOM`,
`GRITAR`, `RELO`). Additionally, character names containing the double `í` (Korean Hangul)
byte sequence are rejected.

**Detailed description**
The device-name check appears in every persistence entry point: `AddAccount` (CFileDB.cpp:86-89),
`_MSG_DBNewAccount` (CFileDB.cpp:520-522), `_MSG_DBAccountLogin` (CFileDB.cpp:619-625),
`DBWriteAccount` (CFileDB.cpp:2400-2403), `DBReadAccount` (CFileDB.cpp:2531-2534),
`DBExportAccount` (CFileDB.cpp:2481-2484), `CreateCharacter` (CFileDB.cpp:2244-2256),
`DeleteCharacter` (CFileDB.cpp:2311-2314), and `GetAccountByChar` (CFileDB.cpp:2641-2644).
The check is `check[0]=='C' && check[1]=='O' && check[2]=='M' && check[3]>='0' &&
check[3]<='9' && check[4]==0` (and the `LPT` variant). If matched, the operation is aborted
with `FALSE` — the account cannot be created, read, written, or exported, and the character
cannot be created or deleted.

The character command-name check appears in `_MSG_DBCreateCharacter` (CFileDB.cpp:897-904)
and `_MSG_DBCreateArchCharacter` (CFileDB.cpp:1461-1468): after uppercasing the name, any of
`KING`, `KINGDOM`, `GRITAR`, `RELO` triggers `_MSG_DBNewCharacterFail`. The double-`í` check
(CFileDB.cpp:940-950, 1504-1514, 1864-1874) scans the name and rejects it if the two-byte
sequence `íí` occurs (a data-integrity guard against a malformed Korean encoding). These rules
apply both to regular and arch (celestial) character creation, and to capsule extraction
(`_MSG_DBOutCapsule`).

**Rule workflow**
```
(account/char name)
  -> uppercase
  -> COMx/LPTx device name? -> FALSE (abort)
  -> (char only) KING/KINGDOM/GRITAR/RELO? -> _MSG_DBNewCharacterFail
  -> (char only) contains 'í''í'? -> _MSG_DBNewCharacterFail
```

---

### Business Rule: Account Login (Password, Block, Dual-Login, Server Change)

**Overview**
`_MSG_DBAccountLogin` is the core authentication flow initiated by a TMSrv on behalf of a
connecting client. It validates the device-name rule, reads the account file, rejects blocked
(banned) accounts, handles the server-change (TempKey) fast path, verifies the plaintext
password, prevents a single account from being logged in twice, records the client MAC, and
returns a confirmation packet containing the account cargo and the selectable-character
summary.

**Detailed description**
The handler (CFileDB.cpp:611-854) first rejects `COMx`/`LPTx` names with
`_MSG_DBAccountLoginFail_Account`. It resolves the session index `Idx` (from server/id) and
searches for an existing logged-in index `IdxName = GetIndex(account)`. The account file is
read; a missing file produces `_MSG_DBAccountLoginFail_Account`. Negative `Coin` values are
clamped to zero. If `Year != 0 && YearDay != 0` and the ban has not expired
(`Year >= currentYear`, or `Year == currentYear && YearDay >= currentYday`), the login is
rejected with `_MSG_DBAccountLoginFail_Block` (CFileDB.cpp:648-660).

The **server-change fast path** (CFileDB.cpp:662-675): if `file.TempKey[0] != 0` and the
incoming `m->Zero[0] != 0`, and `memcmp(file.TempKey, m->Zero)` matches, the TempKey is
cleared and `ChangeServer = 1`, jumping to success (bypassing the password check). If the keys
mismatch, the TempKey is cleared and saved and the login returns without success. Otherwise
the password is compared with `strcmp(file.Info.AccountPass, m->AccountPassword)`; a mismatch
yields `_MSG_DBAccountLoginFail_Pass` (CFileDB.cpp:677-682).

Dual-login prevention (CFileDB.cpp:685-703): if `IdxName == Idx`, the login is a no-op
(already this session). If `IdxName != 0` (account already online elsewhere), then with
`DBNeedSave == 0` it returns `_MSG_DBAlreadyPlaying`; otherwise it returns
`_MSG_DBStillPlaying` and sends `SendDBSavingQuit(IdxName, 0)` to force the old session to
save and quit, then falls through to re-login.

The **name-swap fix** (CFileDB.cpp:707-743): if two character slots hold `Equip[13].sIndex`
values 774 and 775, their `MobName`s are swapped and both `Equip[13].sIndex` values cleared,
with a log entry. Then the account file is copied into the session slot, the client MAC
(`AdapterName[4]`) is recorded, `AddAccountList(Idx)` marks the session logged-in, the
select-character summary is built via `DBGetSelChar`, and `MSG_DBCNFAccountLogin` is sent to
the TMSrv. If this is the only account on that MAC (`GetAccountsByMac(...) <= 1`), the
`_MSG_DBCheckPrimaryAccount` broadcast is issued to all game servers to flag it as the
primary account (CFileDB.cpp:782-801). When `ChangeServer == 1`, the TempKey is persisted,
`SecurePass` is set, and the `_MSG_DBCNFCharacterLogin` fast-path is sent with the selected
character, affect, short-skills, and mob-extra (CFileDB.cpp:802-853).

**Rule workflow**
```
_MSG_DBAccountLogin
  -> COMx/LPTx -> Fail_Account
  -> read account; missing -> Fail_Account
  -> clamp Coin
  -> ban not expired -> Fail_Block
  -> TempKey fast-path match -> ChangeServer=1 (skip password)
     mismatch -> clear TempKey, save, return
  -> strcmp(pass)? mismatch -> Fail_Pass
  -> IdxName==Idx -> no-op
  -> IdxName!=0: DBNeedSave==0 -> AlreadyPlaying; else StillPlaying + DBSavingQuit
  -> name-swap fix (774/775)
  -> copy file to session, record MAC, AddAccountList
  -> send DBCNFAccountLogin (+ primary-account broadcast if sole on MAC)
  -> if ChangeServer: send DBCNFCharacterLogin
```

---

### Business Rule: Character Creation (Mortal and Arch)

**Overview**
Character creation (`_MSG_DBCreateCharacter` and `_MSG_DBCreateArchCharacter`) creates a new
in-game character in a free account slot. It enforces slot bounds, class bounds, a prior
`SecurePass` state, reserved-name rules, name-length rules, and free-slot availability, then
initializes the character from the class base-mob (`g_pBaseSet[cls]`) and persists the account
file.

**Detailed description**
For mortal creation (CFileDB.cpp:856-1026), the handler validates `Slot` in `[0,
MOB_PER_ACCOUNT)` and class `cls` in `[0,4)`, both producing `_MSG_DBNewCharacterFail` on
failure. It requires `pAccountList[Idx].SecurePass != -1` (i.e., the account's numeric-token
secure pass must already be set) — otherwise the request is dropped (CFileDB.cpp:882-889). It
then applies the reserved-name checks (command names, device names, double-`í`) as described
in the name-validation rule. It rejects creation if the target slot already has a name
(CFileDB.cpp:927-935). `CreateCharacter()` (CFileDB.cpp:2236-2301) verifies the character
name does not collide with an existing `./char/<first>/<name>` index file, creating that
index file with the owning account name as content. The handler then clears the mob and
mob-extra, copies the base class template from `g_pBaseSet[cls]` (TK/FM/BM/HT), sets
`MortalFace`, copies the chosen `MobName`, and calls `DBWriteAccount`. On success it sends
`_MSG_DBCNFNewCharacter` with the updated select-character summary.

Arch (celestial) creation (CFileDB.cpp:1423-1589) is analogous but finds the first free slot
automatically, sets `ClassMaster = ARCH`, stores the `MortalSlot`, sets `MortalFace`,
overrides `Equip[0].sIndex = MortalFace + 5 + cls`, sets `BaseScore.Ac = 230`, clears all but
the head equip and 4 carry items, and on success sends both `_MSG_DBCNFArchCharacterSucess`
(with the slot) and `_MSG_DBCNFNewCharacter`.

**Rule workflow**
```
_MSG_DBCreateCharacter / _MSG_DBCreateArchCharacter
  -> slot in range? class in range? -> else Fail
  -> SecurePass set? no -> drop
  -> reserved names? -> Fail
  -> slot empty? no -> Fail
  -> CreateCharacter() uniqueness (./char index) -> else Fail
  -> copy g_pBaseSet[cls]; set face/name
  -> DBWriteAccount
  -> send DBCNFNewCharacter (+ ArchSucess for arch)
```

---

### Business Rule: Character Deletion

**Overview**
`_MSG_DBDeleteCharacter` removes a character from an account slot. It is gated by slot
bounds, a prior `SecurePass`, a plaintext account-password match, a class-master restriction
(MORTAL or ARCH only), and a level ceiling of 218. It also removes the character's
`./char/<first>/<name>` index file via `DeleteCharacter()`.

**Detailed description**
The handler (CFileDB.cpp:1276-1355) validates `Slot` in `[0, MOB_PER_ACCOUNT)` and requires
`SecurePass != -1`. It then requires the supplied `m->Password` to match the account password
with `strncmp(..., ACCOUNTPASS_LENGTH)`; a mismatch returns `_MSG_DBDeleteCharacterFail`.
Next it enforces that `mobExtra[Slot].ClassMaster` is either `MORTAL` or `ARCH` — characters
of other class-master states cannot be deleted (CFileDB.cpp:1315-1319). It enforces
`mob->BaseScore.Level < 219` — characters at level 219 or above cannot be deleted
(CFileDB.cpp:1321-1328). On success, `ShortSkill[Slot]` is cleared, `DeleteCharacter()`
removes the character index file, the mob and mob-extra are cleared via `BASE_ClearMob` /
`BASE_ClearMobExtra`, the account file is rewritten, and `_MSG_DBCNFDeleteCharacter` is sent
with the updated select-character summary.

**Rule workflow**
```
_MSG_DBDeleteCharacter
  -> slot in range? SecurePass set? -> else drop/Fail
  -> password matches? no -> DeleteCharacterFail
  -> ClassMaster in {MORTAL, ARCH}? no -> Fail
  -> BaseScore.Level < 219? no -> Fail
  -> DeleteCharacter() removes ./char index
  -> clear mob/mob-extra; DBWriteAccount
  -> send DBCNFDeleteCharacter
```

---

### Business Rule: Account Disable / Enable (Admin Ban System)

**Overview**
Admin sessions at level > 2 can disable (ban) or enable (unban) an account. Disabling sets a
`Year`/`YearDay` expiration on the account file, which the account-login rule enforces as a
block. Enabling clears those fields. For an account currently online, disable is deferred
until the session saves and quits (`SendDBSavingQuit`).

**Detailed description**
`_MSG_NPDisable` (Server.cpp:1927-1955) requires `Level > 2`. It uppercases the account name
and computes `IdxName = cFileDB.GetIndex(account)`. If the account is **not** currently
online (`IdxName == 0`), it calls `DisableAccount(conn, name, Year, YearDay)`
(Server.cpp:1359-1417). `DisableAccount` reads the account file; a missing account sends
`_MSG_NPNotFound` and a message. If the account is already banned
(`Year != 0 && YearDay != 0`), it sends `_MSG_NPState` with a "1" (blocked) state and returns.
Otherwise it clears the password's trailing bytes, sets `Year` and `YearDay`, writes the
account, and if a `DisableID` is set it issues a `_MSG_NPReqAccount` to re-query the account.
If the account **is** online (`IdxName != 0`), the disable is deferred: `SendDBSavingQuit(IdxName,
1)` is sent, and `pAdmin[conn].DisableID = IdxName`, `Year`, `YearDay` are stored
(Server.cpp:1949-1953). When the game server later reports the account has been saved
(`_MSG_DBNoNeedSave`, CFileDB.cpp:1096-1122, or `_MSG_SavingQuit`, CFileDB.cpp:1261-1272),
the DBSrv iterates admin sessions whose `DisableID` matches and finally performs the disable.

`_MSG_NPEnable` (Server.cpp:1957-1980) requires `Level > 2`. If the account is currently
online (`IdxName != 0`), it sends "Check session. already playing." and returns. Otherwise it
calls `EnableAccount` (Server.cpp:1419-1461), which reads the account, sends
`_MSG_NPState` with the account password if it is not currently banned, otherwise clears
`Year`/`YearDay`, writes the account, and sends `_MSG_NPState` with the password. The
`SendAdminState` helper (Server.cpp:297-312) maps the password's first character to a numeric
state: `@`->1 (blocked), `_`->2 (defective), `#`->3 (disabled).

**Rule workflow**
```
_MSG_NPDisable (Level>2)
  -> online? yes: SendDBSavingQuit(Idx,1); store DisableID/Year/YearDay (defer)
     no:  DisableAccount -> read; missing->NPNotFound; already banned->NPState(1)
          else set Year/YearDay, write, NPReqAccount
  [deferred] on save completion -> DisableAccount now executes

_MSG_NPEnable (Level>2)
  -> online? yes: "already playing"; return
     no: EnableAccount -> read; missing->NPNotFound; not banned->NPState(pass)
          else clear Year/YearDay, write, NPState(pass)
```

---

### Business Rule: Account / Character Information Query

**Overview**
Admin sessions at level > 0 can query account information via `_MSG_NPReqAccount`, which
resolves an account by name (or by character name via `GetAccountByChar`), reads the account
file, and returns `_MSG_NPAccountInfo` containing the full account file plus a session index
and a textual account-state summary.

**Detailed description**
`_MSG_NPReqAccount` (Server.cpp:1738-1848) first requires `Level > 0`. It uppercases both the
account and character names. If a character name is supplied, `GetAccountByChar` (CFileDB.cpp:2633-2664)
reads the `./char/<first>/<name>` index file, which contains the owning account name, into
`m->Account` — this is how a query by character name resolves to its account. If the account
name is empty, `_MSG_NPNotFound` plus a message is sent. The account file is then read; a
missing file sends `_MSG_NPNotFound`. On success, the handler builds `MSG_NPAccountInfo` with
`account = file`, `Session = cFileDB.GetIndex(account)` (0 if not online), and truncates the
character/account name buffers to be null-terminated. It computes the admin-equivalent level
(`maxlevel - 1000`) for informational purposes, and derives a textual state string from the
password prefix: `@` -> "Conta bloqueada." (state 1), `_` -> "Conta Defeituosa." (state 2),
`#` -> "Conta Desativada." (state 3). A human-readable summary line is sent via
`SendAdminMessage`, and the full `MSG_NPAccountInfo` is returned to the admin tool.

**Rule workflow**
```
_MSG_NPReqAccount (Level>0)
  -> uppercase account/char
  -> char given? GetAccountByChar resolves account from ./char index
  -> account empty -> NPNotFound + msg
  -> DBReadAccount; missing -> NPNotFound + msg
  -> build NPAccountInfo (account, Session=GetIndex, State from pass prefix)
  -> send summary message + NPAccountInfo
```

---

### Business Rule: Account Save by Admin

**Overview**
Admin sessions at level > 2 can write an edited account back to disk via
`_MSG_NPReqSaveAccount`. The rule guards against overwriting an online account, prevents
privilege escalation beyond the admin's own level, preserves the account password, and
rewrites the account file.

**Detailed description**
`_MSG_NPReqSaveAccount` (Server.cpp:1850-1925) first requires `Level > 2`; otherwise it sends
"Not allowed". It computes `IdxName = GetIndex(account)`; if the account is currently online
(`IdxName != 0`), it refuses with "For saving, account should be disabled." — an online
account must be offline before an admin overwrite. It then computes the max character level
of the incoming (edited) account and derives `admin = maxlevel - 1000`. If `maxlevel >= 2000`
(i.e., the edited account would have an admin-equivalent level), it enforces two escalation
guards (CFileDB.cpp:1882-1898): (1) `admin` must not exceed the saving admin's own `Level`,
and (2) if `admin == pAdmin[conn].Level`, the account name must equal the saving admin's own
name — preventing one admin from promoting another account to the same level. If either check
fails, it sends "Set admin level error." Next, it re-reads the on-disk account
(`DBReadAccount`); if missing, it sends "There's no account with that account name". It then
**preserves the existing password** by copying `tmpact.Info.AccountPass` into the incoming
`m->account.Info.AccountPass` (CFileDB.cpp:1913) and writes the account via `DBWriteAccount`,
logging success.

**Rule workflow**
```
_MSG_NPReqSaveAccount (Level>2)
  -> online (GetIndex!=0)? -> "account should be disabled"
  -> maxlevel>=2000 && (admin > myLevel || (admin==myLevel && name != mine))
       -> "Set admin level error"
  -> DBReadAccount; missing -> "no account"
  -> preserve AccountPass from disk
  -> DBWriteAccount
```

---

### Business Rule: Sapphire (Experience Rate) Update

**Overview**
`_MSG_DBUpdateSapphire` adjusts the server-wide experience multiplier (`Sapphire` global,
initialized to 8) by doubling or halving it, clamped to the closed interval `[1, 64]`, and
broadcasts the new rate to every connected game server.

**Detailed description**
In `ProcessMessage` (CFileDB.cpp:483-508), the handler reads `m->Parm` from a
`MSG_STANDARDPARM`. If `Parm == 1`, `Sapphire *= 2`; otherwise `Sapphire /= 2`. It then clamps
the value: if `Sapphire < 1`, it is set to 1; if `Sapphire > 64`, it is set to 64. For each
connected game server (`pUser[i].Mode != USER_EMPTY` and socket valid), it sends
`SendDBSignalParm3(i, 0, _MSG_DBSetIndex, -1, Sapphire, i)` to propagate the new rate, and it
persists the value to `Config.txt` via `WriteConfig()`.

**Rule workflow**
```
_MSG_DBUpdateSapphire
  -> Parm==1 ? Sapphire*=2 : Sapphire/=2
  -> clamp to [1,64]
  -> broadcast _MSG_DBSetIndex(Sapphire) to all game servers
  -> WriteConfig()
```

---

### Business Rule: Guild War and Alliance State Distribution

**Overview**
The component maintains global guild-war and guild-alliance state in two 65536-entry arrays
(`g_pGuildWar[]`, `g_pGuildAlly[]`) and redistributes every change to all connected game
servers so the whole cluster shares a consistent inter-guild conflict/ally view.

**Detailed description**
`_MSG_War` (CFileDB.cpp:388-418) reads `m->Parm1` (the guild) and validates it is in
`[1, 65536)`, then records `g_pGuildWar[myguild] = m->Parm2` (the enemy guild / day marker;
the day-based assignment is commented out). For every connected game server it calls
`SendDBSignalParm2(i, 0, _MSG_War, m->Parm1, m->Parm2)`. `_MSG_GuildAlly` (CFileDB.cpp:420-452)
behaves identically for the ally relationship. These arrays are initialized to zero at boot
(`memset` in `WinMain`, Server.cpp:515-516). On game-server connect, the full war/ally state
for all 65536 slots is replayed to the new server (Server.cpp:1166-1170). Guild info is
distributed separately via `_MSG_GuildInfo` (CFileDB.cpp:454-481), which stores
`GuildInfo[myguild]`, broadcasts it, and persists via `CReadFiles::WriteGuildInfo()`.

**Rule workflow**
```
_MSG_War / _MSG_GuildAlly
  -> validate guild in [1,65536)
  -> store g_pGuildWar[guild] / g_pGuildAlly[guild]
  -> broadcast to all connected game servers
```

---

### Business Rule: Capsule (Character Storage) System

**Overview**
The capsule system lets players store a character into a capsule file and later extract it
into a free slot. `_MSG_DBPutInCapsule` writes the character to `./capsule/<id>`, clears the
slot, and creates an import item; `_MSG_DBOutCapsule` reads the capsule, places the character
into a free slot, and consumes the capsule item.

**Detailed description**
`_MSG_DBPutInCapsule` (CFileDB.cpp:1684-1766) validates `Slot`, increments the global
`LastCapsule`, and writes a `STRUCT_CAPSULE` (the character mob + mob-extra, with
`MortalSlot = -1`) to `./capsule/<LastCapsule>`. It clears `ShortSkill[Slot]`, clears the mob
and mob-extra in the session, rewrites the account file, sends `_MSG_DBCNFCapsuleSucess` and a
`_MSG_DBCNFDeleteCharacter` (to refresh the select-char view), and writes an import file to
`../../Common/ImportItem/<id>` containing the account and a capsule item (index 3443) whose
value encodes the capsule id (CFileDB.cpp:1761-1765).

`_MSG_DBOutCapsule` (CFileDB.cpp:1768-1927) validates the source `Slot` and finds a free
`NovoSlot`. It calls `CreateCharacter()` to reserve the new character name, then uses
`GetItemPointer` to find the capsule item in the source slot and computes `index =
item->stEffect[0].cValue * 256 + item->stEffect[1].cValue`, reading `./capsule/<index>`. It
applies the same name validation (empty slot, double-`í`), clears the capsule item, initializes
the new slot from the capsule mob/extra (resetting coin/guild/guild-level), writes the account,
deletes the capsule file, and sends `_MSG_DBCNFNewCharacter`.

**Rule workflow**
```
PutInCapsule: validate slot -> LastCapsule++ -> write ./capsule/<id> ->
              clear slot/shortskill -> DBWriteAccount -> send success+refresh ->
              write ImportItem capsule-item file
OutCapsule:  validate slot -> find free slot -> CreateCharacter ->
             read ./capsule/<index> from item -> validate name ->
             clear capsule item -> init new slot -> DBWriteAccount ->
             delete capsule file -> send DBCNFNewCharacter
```

---

### Business Rule: Server Change (TempKey Transfer)

**Overview**
`_MSG_DBServerChange` implements cross-server character transfer by generating a one-time
`TempKey` embedded in a confirmation packet. The client later logs in on the destination
server using this key, which the account-login rule accepts as a password-bypass fast path.

**Detailed description**
`_MSG_DBServerChange` (CFileDB.cpp:1929-1988) validates `ID` in `[1, MAX_USER)` and
`NewServer` in `[1, MAX_SERVER+1]`. It calls `GetEncPassword` (CFileDB.cpp:2671-2683), which
fills an 8-element `Enc[]` array with random values in `[100, 1000)` but always returns
`FALSE` (the "encryption" is effectively a stub that never signals an error). Because the
return is `FALSE`, the else branch always executes: `SetEncPassword` (a no-op stub) is called,
and a `_MSG_DBCNFServerChange` packet is built with `AccountName` and an `Enc` string formatted
as `"*%d %d %d %d %d %d %d %d %d %d"` (NewServerID, the 8 encode values, and Slot). This string
is stored into `pAccountList[Idx].File.TempKey` (52 bytes) and sent to the client. The client
re-sends this as the `Zero[52]` field of `MSG_AccountLogin` on the destination server; the
account-login rule (see that rule) matches it against `TempKey` to bypass the password check.

**Rule workflow**
```
_MSG_DBServerChange
  -> validate ID, NewServer
  -> GetEncPassword() fills Enc[] (returns FALSE)
  -> SetEncPassword() (no-op)
  -> format "*server enc0..enc7 slot" -> store in File.TempKey
  -> send _MSG_DBCNFServerChange
[destination] _MSG_DBAccountLogin with Zero == TempKey -> bypass password
```

---

### Business Rule: Primary Account Detection (MAC)

**Overview**
The component tracks which account is the "primary" account per machine by recording the
client MAC (`AdapterName[4]`) at login and counting how many logged-in accounts share the
same MAC. When only one account on a MAC is online, the DBSrv broadcasts a primary-account
signal to all game servers.

**Detailed description**
At login (`_MSG_DBAccountLogin`), the four `AdapterName` values are copied into
`pAccountList[Idx].Mac[0..3]` (CFileDB.cpp:751-754). After a successful login, the handler
calls `GetAccountsByMac(m->AdapterName)` (CFileDB.cpp:2351-2367), which iterates all
`MAX_DBACCOUNT` session slots counting those with `Login != 0` and matching MACs. If the count
is `<= 1`, the account is the sole online account on that machine, so the component broadcasts
`_MSG_DBCheckPrimaryAccount` (carrying the MAC and account name) to every connected game server
(CFileDB.cpp:782-801). The client can explicitly promote an account to primary via
`_MSG_DBPrimaryAccount` (CFileDB.cpp:2068-2101), which re-broadcasts the check and sends a
localized confirmation message ("Sua conta agora é a primária.").

**Rule workflow**
```
login -> record Mac[] -> GetAccountsByMac()>1? no -> broadcast DBCheckPrimaryAccount
_MSG_DBPrimaryAccount -> broadcast DBCheckPrimaryAccount + confirmation message
```

---

### Business Rule: Item and Donation Import

**Overview**
`CReadFiles::ImportItem` and `CReadFiles::ImportDonate` process flat files dropped into
`../../Common/ImportItem/` and `../../Common/ImportDonate/`. Each file grants an item or a
donation balance to an account. Files are validated, applied to the account file (or sent
live to an online session), and then deleted or moved to an error folder.

**Detailed description**
`ImportItem` (CReadFiles.cpp:130-378) scans the import folder, up to 10 files per sweep. For
each file, it parses a line `"<account> <index> <eff1> <val1> <eff2> <val2> <eff3> <val3>"`. It
validates `Index` in `[0, MAX_ITEMLIST)` and each effect in `[0, 255]` (CReadFiles.cpp:209-221);
invalid files are moved to the error folder. If the target account is online
(`GetIndex` valid and `Slot` in `[0,3]`), the item is sent live as `_MSG_DBSendItem`. The
account file is then read; if the account does not exist, the file is moved to the error
folder (unless already sent live). A cargo position is found — first via `BASE_CanCargo` over
126 positions, then by a linear scan for the first empty cargo slot (CReadFiles.cpp:284-308).
If no space exists, the file is moved to the error folder. Otherwise the item bytes are copied
into `file.Cargo[Pos]` and the account is saved; the import file is deleted (or moved to the
error folder on failure).

`ImportDonate` (CReadFiles.cpp:726-908) parses `"<account> <donate>"`. It rejects negative
donate amounts (moved to error). If the account is online, it sends `_MSG_DBSendDonate` live.
It then reads the account, adds `Donate` to `file.Donate`, writes the account, and deletes the
file (or moves it to `ImportDonateError` on failure).

**Rule workflow**
```
ImportItem:  scan folder (max 10) -> parse+validate item -> online? send live ->
             read account (missing -> error) -> find cargo pos (no space -> error) ->
             write item to cargo -> DBWriteAccount -> delete file
ImportDonate: scan folder -> parse+validate (negative -> error) -> online? send live ->
             read account -> file.Donate += Donate -> DBWriteAccount -> delete file
```

---

### Business Rule: Ranking System

**Overview**
`CRanking` maintains an in-memory top-500 "grind ranking" by experience. It is loaded at
startup by scanning all account files, updated live from game servers (`_MSG_UpdateExpRanking`),
and periodically written to `../../Common/Ranking.txt`. Admin accounts (level >= 1000) are
excluded from ranking.

**Detailed description**
The `RankingSystem` constructor calls `loadRanking()`, which recursively scans `./account/*`
(CRanking.cpp:99-111, 118-215). Each account file is read if its size is either `7500-7600`
bytes or exactly `sizeof(STRUCT_ACCOUNTFILE)`. For each non-empty character, characters with
`BaseScore.Level >= 1000` are skipped (admin exclusion, CRanking.cpp:187-188), and the rest
are inserted via `tryInsertInRanking`. Class-master normalization (CRanking.cpp:224-229,
367-372) maps `MORTAL` down one and `ARCH` up one so mortals rank below celestial classes.
`tryInsertInRanking` (CRanking.cpp:222-274) replaces the last-ranked player if the candidate
has higher value/class and bubbles the new entry up via `riseRankingElement` swaps.
`increaseRankingElementValue` (CRanking.cpp:363-392) updates an existing player's value and
bubbles upward. Rank position changes are propagated to the affected online players via
`sendUpdateIndividualRank` / `sendUpdateRangeRank` using stored `GrindRankingConnId`
(TMSrv/player id) tuples (CRanking.cpp:279-337).

`_MSG_UpdateExpRanking` (CFileDB.cpp:1990-2042) is the live update entry point: if the player
is not in the ranking, `tryInsertInRanking` is called and the kicked last player plus all
affected range players are notified; if the player is already ranked and their value/class
improves, `increaseRankingElementValue` is called and the affected range is notified. Every
600 seconds, `ProcessSecTimer` calls `CReadFiles::WriteRanking()` (CReadFiles.cpp:1043-1066),
which writes the ranked players (excluding level >= 1000) with class-master and class labels to
`Ranking.txt`.

**Rule workflow**
```
loadRanking(): scan ./account -> for each char (level<1000) tryInsertInRanking
_MSG_UpdateExpRanking: not ranked? tryInsert + notify range; ranked? improve -> rise + notify
WriteRanking(): every 600s write top ranks (level<1000) to Ranking.txt
```

---

### Business Rule: Periodic Timers and Day/Monthly Maintenance

**Overview**
`ProcessSecTimer` (Server.cpp:2013-2084) is the component's heartbeat, invoked every second
by the `TIMER_SEC` window timer. It drives all periodic file imports, connection reporting,
ranking writes, log rotation, and the weekly guild-fame reset.

**Detailed description**
Each tick calls `CReadFiles::ImportUser()` and `CReadFiles::UpdateUser()` (user registration
and password-update imports). Every 30 seconds (`SecCounter % 30 == 0`) it calls
`ImportItem()`, `ImportDonate()`, and `UpdateConnection()`, and increments `MinCounter`. Every
30 minutes (`MinCounter % 30 == 0`) it calls `UpdateConnectionData()` and increments
`HourCounter`. Every 600 seconds it writes the ranking (`CReadFiles::WriteRanking()`). It
checks the local day; on a new day it rotates the main log via `StartLog("A")` and flushes
`DayLog_ItemLog()` / `DayLog_ExpLog()`. On the weekly boundary (Sunday at 00:00:00,
`when.tm_hour == 0 && when.tm_wday == 0 && when.tm_min == 0 && when.tm_sec == 0`), it resets
the `Fame` of every guild to zero, re-sends the updated guild info to all game servers, and
persists via `CReadFiles::WriteGuildInfo()`. `ProcessMinTimer` (Server.cpp:2086-2089) is an
empty stub.

**Rule workflow**
```
ProcessSecTimer (1s):
  ImportUser, UpdateUser
  30s: ImportItem, ImportDonate, UpdateConnection, MinCounter++
  30min: UpdateConnectionData, HourCounter++
  600s: WriteRanking
  new day: StartLog("A"), DayLog_ItemLog, DayLog_ExpLog
  Sunday 00:00:00: reset guild Fame, broadcast GuildInfo, WriteGuildInfo
```

---

### Business Rule: Account Registration Import and Password Update

**Overview**
`CReadFiles::ImportUser` registers new accounts from files in `../../Common/ImportUser/`, and
`CReadFiles::UpdateUser` changes account passwords from files in
`../../Common/serv<index>/update/`. Both parse fixed multi-line text formats, validate field
lengths, and delegate persistence to `CFileDB::AddAccount` / `CFileDB::UpdateAccount`.

**Detailed description**
`ImportUser` (CReadFiles.cpp:380-690) reads each import file line by line: account id, password,
real name, `SSN1`, `SSN2`, email, telephone, address, and an optional bonus flag. Each field is
length-validated against its bound (e.g., `len >= ACCOUNTNAME_LENGTH` rejected) and the id is
uppercased. `AddAccount` (CFileDB.cpp:78-143) re-checks the device-name rule, ensures the
account file does not already exist (existing file -> `FALSE`), populates the account info,
sets `NumericToken[0] = -1` (unset secure token) and `file.ShortSkill` bytes to `-1`, and if a
`bonus` is given fills the cargo with 50x item 401 and 50x item 406. On success the import file
is deleted.

`UpdateUser` (CReadFiles.cpp:910-1041) scans the per-server update folder, parses an account id
and a new password, validates lengths, and calls `UpdateAccount` (CFileDB.cpp:145-208), which
reads the existing account, writes the new password, and persists — updating the in-memory
session password if the account is online. In both importers, malformed/empty files are skipped
and processed files are deleted.

**Rule workflow**
```
ImportUser:  scan folder -> parse id/pass/name/ssn/email/tel/addr/bonus ->
             validate lengths -> AddAccount (device-name, no-exist, bonus cargo) ->
             delete file
UpdateUser:  scan per-server update folder -> parse id + new pass ->
             UpdateAccount (read, set pass, write) -> delete file
```

---

### Business Rule: Transper (Cross-Server Character Transfer via Redirect Server)

**Overview**
`_MSG_ReqTransper` handles character transfer requests routed through the outbound
`AdminClient` connection to the redirect/transfer server. It validates the request, verifies
the character's current name matches the expected old name, disables the source account, and
forwards a create-character request to the redirect server.

**Detailed description**
The handler (CFileDB.cpp:243-355) requires `TransperCharacter != 0` and `AdminClient.Sock != 0`
(i.e., the redirect server must be connected), otherwise it sends a `_MSG_ReqTransper` result
of 4 (failure). It validates `m->ID` in `[1, MAX_USER)` and `slot` in `[0, MOB_PER_ACCOUNT)`.
It resolves the session index and compares the slot's current character name (uppercased)
against `m->OldName` (uppercased); a mismatch sends result 4. On success it disables the source
account (`DisableAccount(-1, account, 0, 0)`) and builds a `MSG_NPCreateCharacter` with the
character's mob data, sending it to `AdminClient` (the redirect server). The redirect server's
reply is handled in `ProcessAdminClientMessage` (Server.cpp:626-741), which on success
(`Result == 0`) reads the account, zeroes the character slot, rewrites the account, and sends
the originating game server a `_MSG_ReqTransper` result; on transfer failures (read/write) it
logs and sends result 4.

**Rule workflow**
```
_MSG_ReqTransper
  -> TransperCharacter && AdminClient connected? no -> result 4
  -> validate ID, slot
  -> old name matches slot? no -> result 4
  -> DisableAccount(source)
  -> send MSG_NPCreateCharacter to redirect server
[reply] ProcessAdminClientMessage
  -> result 0: read account, clear slot, write, reply result 0 to source TMSrv
  -> fail: log, reply result 4
```

---

### Business Rule: Broadcasting and Notice Distribution

**Overview**
Several message types are rebroadcast by the DBSrv to all connected game servers or admin
sessions: system notices (`_MSG_DBNotice`, `_MSG_MessageDBImple`), global announcements
(`_MSG_MagicTrumpet`), appeals (`_MSG_NPAppeal`), and admin-generated notices
(`_MSG_NPNotice`). Broadcasts iterate the `pUser[]` / `pAdmin[]` session tables and forward
the packet to every non-empty session with a valid socket.

**Detailed description**
`_MSG_DBNotice` (CFileDB.cpp:1612-1622) and `_MSG_MagicTrumpet` (CFileDB.cpp:1591-1610) forward
the incoming packet to all connected game servers (and, for the trumpet, to all admin sessions
as well). `_MSG_MessageDBImple` (CFileDB.cpp:592-609) is an "implementer" broadcast sent to all
game servers. `_MSG_NPAppeal` (CFileDB.cpp:579-590) forwards a client appeal to all connected
admin sessions. The admin-generated `_MSG_NPNotice` (Server.cpp:1600-1678) has two modes: if
`AccountName` is empty and `Parm1 == 1`, it is a global broadcast gated on `Level >= 2`; the
notice is sent to all game servers. Otherwise, it requires `Level > 0`, resolves the target
account via `GetIndex`, validates the derived server/id, and sends the notice only to that
account's game server.

**Rule workflow**
```
_MSG_DBNotice / _MSG_MagicTrumpet / _MSG_MessageDBImple / _MSG_NPAppeal
  -> iterate pUser[]/pAdmin[] -> forward packet to valid sessions
_MSG_NPNotice
  -> global (empty account, Parm1==1, Level>=2): broadcast to all game servers
  -> direct (Level>0): resolve account -> send to that server
```

---

## 4. Component Structure

The Server (DBSrv) component is a single Visual Studio project producing one Win32 executable.
The primary file implementing the main loop and connection handling is `Server.cpp`; the rest
of the project provides the persistence, import, ranking, and session support layers it calls
into.

```
legacy/Code/DBSrv/
├── Server.cpp                 # Main loop (WinMain), MainWndProc event dispatcher,
│                              #   game-server & admin accept, ProcessClientMessage /
│                              #   ProcessAdminMessage / ProcessAdminClientMessage,
│                              #   ProcessSecTimer/ProcessMinTimer, logging, day logs,
│                              #   config read/write, base-mob loading, guild transfer
├── Server.h                   # Declarations + extern globals (pUser, pAdmin, CPSock
│                              #   sockets, cFileDB, ServerIndex, Sapphire, etc.)
├── CFileDB.cpp                # File-backed account/character persistence: ProcessMessage
│                              #   dispatcher (login/char/save/delete/capsule/guild/ranking),
│                              #   DBReadAccount / DBWriteAccount / DBExportAccount,
│                              #   AddAccount/UpdateAccount/CreateCharacter/DeleteCharacter
├── CFileDB.h                  # CFileDB class + STRUCT_ACCOUNTLIST + extern GuildInfo
├── CReadFiles.cpp             # Periodic file import/export: ImportItem/ImportUser/
│                              #   ImportDonate/UpdateUser/UpdateConnection/Read-WriteGuildInfo
├── CReadFiles.h               # CReadFiles static-method class + file path constants
├── CRanking.cpp               # RankingSystem / GrindRanking: load, insert, rise, broadcast
├── CRanking.h                 # RankingSystem / GrindRanking classes + STRUCT_RANKING types
├── CUser.cpp                  # Per-connection session state (CUser) + AcceptUser()
├── CUser.h                    # CUser class (IP, Mode, CPSock, Level, Encode1/2, DisableID...)
├── DBSrv.vcxproj              # Visual Studio project (v143 toolset, Win32, ws2_32+winmm)
├── DBSrv.vcxproj.filters      # Solution filter layout
├── DBSrv.rc                   # Win32 resource script (UTF-16)
├── resource.h                 # Resource identifiers (IDI_ICON1, dialog ids)
├── stdafx.h                   # Precompiled header (afxwin.h)
└── icon1.ico                  # Application icon
```

**Shared files the component compiles against** (outside the component folder but compiled
into the DBSrv executable per `DBSrv.vcxproj`):
```
legacy/Code/Basedef.cpp / Basedef.h   # Core structs, packet protocol, BASE_* helpers
legacy/Code/CPSock.cpp / CPSock.h     # Asynchronous socket + buffering abstraction
legacy/Code/ItemEffect.h              # Item effect constants
```

Note: `DBSrv.vcxproj` references a header `CActivePinCode.h` (line 105) that **does not exist**
in the repository; the `_MSG_DBActivatePinCode` handler (CFileDB.cpp:2060-2066) is commented
out, so this is a stale project reference.

**Runtime data layout (relative to the executable's working directory):**
```
./account/<first-letter>/<ACCOUNTNAME>   # One binary STRUCT_ACCOUNTFILE per account
./char/<first-letter>/<CHARNAME>         # Character index file holding owning account name
./capsule/<id>                           # Character capsule files
./BaseMob/TK|FM|BM|HT                    # Class base-mob templates (loaded at boot)
./Config.txt                             # Sapphire rate + LastCapsule counter
./Admin.txt                              # Admin IP allow-list
./redirect.txt                           # Redirect server address + admin credentials
./Trans.txt                              # Guild transfer info
./Log/DB_*.txt                           # Main log files (rotated per session/day)
./DayLog/EXP_DB_*.txt, ITEM_DB_*.txt     # Daily exp/item logs
../../Common/ImportItem/*                # Item grant import files
../../Common/ImportUser/*                # Account registration import files
../../Common/ImportDonate/*              # Donation import files
../../Common/GuildInfo                   # Guild info binary snapshot
../../Common/Ranking.txt                 # Ranking output
../../Common/record<idx>/*               # Message records
../../Common/serv<idx>/update/*          # Password update import files
```

---

## 5. Dependency Analysis

### Internal Dependencies (within the component / compiled into DBSrv)
```
Server.cpp ──► CFileDB (ProcessMessage, DBReadAccount, DBWriteAccount, GetIndex, SendDBSignal*)
Server.cpp ──► CReadFiles (ImportItem/User/Donate, UpdateUser, WriteRanking, Read/WriteGuildInfo)
Server.cpp ──► CRanking (rankingSystem, via CFileDB)
Server.cpp ──► CUser (pUser[], pAdmin[], TempUser, AcceptUser)
Server.cpp ──► CPSock (ListenSocket, AdminSocket, AdminClient, SendOneMessage, Receive)
Server.cpp ──► Basedef (packet structs, flags, BASE_* helpers, g_pServerList)
Server.cpp ──► ItemEffect.h

CFileDB ──► Basedef (structs, BASE_GetFirstKey, BASE_CanCargo, g_pBaseSet)
CFileDB ──► CPSock (pUser/pAdmin sockets via SendOneMessage)
CFileDB ──► CReadFiles (WriteGuildInfo)  [CFileDB.cpp:480]
CFileDB ──► CRanking (rankingSystem)      [CFileDB.cpp:843, 1998]
CFileDB ──► Server.cpp globals (pUser, pAdmin, ServerIndex, Sapphire, TransperCharacter)

CReadFiles ──► CFileDB (AddAccount, UpdateAccount, GetIndex, DBReadAccount, DBWriteAccount)
CReadFiles ──► CRanking (rankingSystem, via WriteRanking)
CReadFiles ──► Server.cpp globals (pUser, ServerIndex, hWndMain)

CRanking ──► CPSock (pUser sockets), Server.cpp globals (pUser, pAdmin), CReadFiles
CUser ──► CPSock, Basedef
```

### External Dependencies
```
- Winsock 2 (ws2_32.lib)          - TCP sockets, WSAAsyncSelect, getaddrinfo
- Windows multimedia (winmm.lib)  - timeGetTime (Server.cpp:823)
- Win32 API (kernel32, user32, gdi32, advapi32, shell32, ole32, uuid, winspool,
  comdlg32, oleaut32)             - GUI message pump, file I/O, registry
- ODBC (odbc32.lib, odbccp32.lib) - linked (declared in vcxproj) but not used in DBSrv source
- Filesystem                      - account/char/capsule/guild/config/log files
- Redirect/transfer server (TCP)  - optional outbound client (AdminClient from redirect.txt)
- Game servers (TMSrv) (TCP)      - inbound clients on DB_PORT 7514
- Admin tool / NPTool (TCP)       - inbound clients on ADMIN_PORT 8895
```

---

## 6. Afferent and Efferent Coupling

Coupling is computed at the **class / global-module** level within the DBSrv executable
(C++ classes and the `Server.cpp` global state). Afferent = incoming fan-in (who uses it);
Efferent = outgoing fan-out (what it uses). These are estimates from the source-level
call graph across the component.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| `Server.cpp` globals + dispatchers | High (pUser/pAdmin/sockets used by CFileDB, CReadFiles, CRanking; ProcessClientMessage/ProcessAdminMessage call into CFileDB) | High (CFileDB, CReadFiles, CRanking, CPSock, Basedef, ItemEffect) | High |
| `CFileDB` | High (Server::ProcessClientMessage, Server::Disable/EnableAccount, CReadFiles Import*/UpdateUser, ranking via ProcessMessage) | Medium (Basedef, CPSock, CReadFiles, CRanking, Server.cpp globals) | High |
| `CReadFiles` | Medium (Server::ProcessSecTimer, CFileDB::_MSG_GuildInfo, CRanking::loadRanking) | Medium (CFileDB, CRanking, Server.cpp globals, Basedef) | Medium |
| `CRanking` | Low-Medium (CFileDB::_MSG_UpdateExpRanking, CFileDB char-login, CReadFiles::WriteRanking) | Medium (CPSock, Server.cpp globals, CReadFiles) | Low |
| `CUser` | Medium (Server.cpp, CFileDB, CReadFiles, CRanking reference pUser/pAdmin) | Low (CPSock, Basedef) | Low |
| `CPSock` (shared) | Very High (every module uses sockets) | Low (Winsock only) | High |
| `Basedef` (shared) | Very High (included by all) | Low | High |

Note: the tightest coupling is between `Server.cpp` and `CFileDB` — `Server.cpp` performs
connection admission and top-level validation, then delegates virtually all protocol handling
to `CFileDB::ProcessMessage`, while `CFileDB` reaches back into `Server.cpp`'s global session
arrays (`pUser`, `pAdmin`, `ServerIndex`, `Sapphire`) and sockets. This bidirectional
dependency is the component's primary structural seam.

---

## 7. Endpoints

This component does **not** expose REST, GraphQL, or gRPC endpoints. It exposes two raw
**TCP socket listeners** and one optional outbound TCP client connection, all speaking the
custom binary packet protocol defined in `Basedef.h`. Endpoints are listed below.

| Endpoint | Protocol | Port | Direction | Description |
|----------|----------|------|-----------|-------------|
| `DB_PORT` (7514) | Raw TCP, custom binary frames (`_MSG` header + payload) | Inbound listener (`ListenSocket`) | Accepts game servers (TMSrv) | Game-server connection for account/character persistence, guild state, notices, ranking, caps |
| `ADMIN_PORT` (8895) | Raw TCP, custom binary frames (`_MSG` header + payload) | Inbound listener (`AdminSocket`) | Accepts admin tool (NPTool) | Administrative account/character management, notices, donations |
| Redirect server (from `redirect.txt`) | Raw TCP, custom binary frames | Outbound client (`AdminClient`) | Connects to redirect/transfer server | Character transfer (`_MSG_ReqTransper` / `_MSG_NPCreateCharacter`) |

**Message types handled inbound from game servers (`FLAG_GAME2DB`)** (via `CFileDB::ProcessMessage`
and `Server::ProcessClientMessage`):
`_MSG_DBAccountLogin`, `_MSG_DBCharacterLogin`, `_MSG_DBNoNeedSave`, `_MSG_DBSaveMob`,
`_MSG_SavingQuit`, `_MSG_DBDeleteCharacter`, `_MSG_DBUpdateSapphire`, `_MSG_MessageDBRecord`,
`_MSG_GuildZoneReport`, `_MSG_War`, `_MSG_GuildAlly`, `_MSG_GuildInfo`, `_MSG_DBServerChange`,
`_MSG_DBItemDayLog`, `_MSG_DBActivatePinCode` (stub), `_MSG_DBPrimaryAccount`, `_MSG_DBNewAccount`,
`_MSG_DBCreateCharacter`, `_MSG_DBCreateArchCharacter`, `_MSG_MessageDBImple`, `_MSG_NPAppeal`,
`_MSG_MagicTrumpet`, `_MSG_DBNotice`, `_MSG_DBCapsuleInfo`, `_MSG_DBPutInCapsule`,
`_MSG_DBOutCapsule`, `_MSG_ReqTransper`, `_MSG_AccountSecure`, `_MSG_UpdateExpRanking`.

**Message types handled from the admin tool (`FLAG_NP2DB`)** (via `Server::ProcessAdminMessage`):
`_MSG_NPCreateCharacter`, `_MSG_NPNotice`, `_MSG_NPIDPASS`, `_MSG_NPReqAccount`,
`_MSG_NPReqSaveAccount`, `_MSG_NPDisable`, `_MSG_NPEnable`, `_MSG_NPDonate`.

**Message types handled from the redirect/transfer server** (via `Server::ProcessAdminClientMessage`):
`_MSG_NPCreateCharacter_Reply`.

**Outbound message types** (sent to game servers / admin tool / redirect server) include:
`_MSG_DBSetIndex`, `_MSG_DBCNFAccountLogin`, `_MSG_DBCNFCharacterLogin`, `_MSG_DBCNFNewCharacter`,
`_MSG_DBCNFDeleteCharacter`, `_MSG_DBCNFAccountLogOut`, `_MSG_DBNewAccountFail`,
`_MSG_DBNewCharacterFail`, `_MSG_DBDeleteCharacterFail`, `_MSG_DBAlreadyPlaying`,
`_MSG_DBStillPlaying`, `_MSG_DBAccountLoginFail_*`, `_MSG_DBClientMessage`, `_MSG_DBNotice`,
`_MSG_GuildReport`, `_MSG_DBSendItem`, `_MSG_DBSendDonate`, `_MSG_DBCheckPrimaryAccount`,
`_MSG_DBCNFServerChange`, `_MSG_DBServerSend1`, `_MSG_AccountSecure(/_Fail)`, `_MSG_MagicTrumpet`,
`_MSG_MessageDBImple`, `_MSG_CNFDBCapsuleInfo`, `_MSG_DBCNFCapsuleSucess`, `_MSG_SendExpRanking`,
`_MSG_ReqTransper`, `_MSG_NPReqIDPASS`, `_MSG_NPNotFound`, `_MSG_NPAccountInfo`, `_MSG_NPState`,
`_MSG_NPNotice`, `_MSG_NPCreateCharacter_Reply`, `_MSG_TransperCharacter`.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Game servers (TMSrv) | Internal service (inbound) | Account/char persistence, guild state, notices, ranking | Raw TCP (DB_PORT 7514) | Custom binary packets (`_MSG` structs) | IP allow-list; invalid packets logged; `ProcessMessage` returns FALSE on malformed |
| Admin tool (NPTool) | External admin (inbound) | Account management, notices, donations | Raw TCP (ADMIN_PORT 8895) | Custom binary packets (`_MSG_NP*`) | IP allow-list + Encode1/Encode2 challenge; `FALSE` closes socket |
| Redirect/transfer server | External service (outbound) | Character transfer between servers | Raw TCP (`redirect.txt`) | Custom binary packets | Connection required for `TransperCharacter`; failures logged, result 4 returned |
| Filesystem (accounts) | Local storage | Persistent account/character data | Direct file I/O (`_open`/`_write`) | `STRUCT_ACCOUNTFILE` binary blobs | `errno`-based logging; no transactions/atomicity |
| Filesystem (import/export) | Local storage | Item/user/donate grants, ranking, guild snapshot | Flat text files | Newline-delimited / space-separated | Invalid files moved to error folders; missing accounts logged |
| `BaseMob` data files | Local files | Class base-mob templates | Binary read at boot | `STRUCT_MOB` blobs | Boot aborts with message box if missing |
| Winsock 2 | OS API | Async sockets | `WSAAsyncSelect` + window messages | `FD_READ`/`FD_CLOSE` events | Socket-level errors logged; stale sockets closed |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Message-Driven Event Loop | `GetMessage`/`DispatchMessage` in `WinMain`; `MainWndProc` switches on `WM_TIMER`/`WSA_*` events | `Server.cpp:427-624`, `Server.cpp:743-1337` | Single-threaded serialization of all socket + timer work |
| Command Dispatcher (switch) | `ProcessMessage` switches on packet `Type`; `ProcessAdminMessage`/`ProcessAdminClientMessage` likewise | `CFileDB.cpp:236-2106`, `Server.cpp:1463-2011`, `Server.cpp:626-741` | Route each packet type to its handler |
| Asynchronous Socket Abstraction | `CPSock` buffers inbound/outbound; `Receive`/`ReadMessage`/`SendOneMessage` | `CPSock.cpp` / `CPSock.h` | Frame/buffer binary messages without threads |
| Layered Architecture | `Server.cpp` (I/O + admission) → `CFileDB::ProcessMessage` (protocol) → `CFileDB` file I/O | `Server.cpp`, `CFileDB.cpp` | Separation of network handling from persistence |
| Global State / Singleton (extern) | `pUser[]`, `pAdmin[]`, `cFileDB`, `Sapphire`, `GuildInfo[]`, `rankingSystem` | `Server.h`, `CFileDB.h`, `CRanking.cpp:29` | Shared state across handlers without DI |
| Repository (file-backed) | `CFileDB::DBReadAccount`/`DBWriteAccount`/`DBExportAccount` | `CFileDB.cpp:2390-2577` | Abstraction over account file storage |
| File Watcher / Polling Import | `CReadFiles::ImportItem`/`ImportUser`/`ImportDonate` via `_findfirst` | `CReadFiles.cpp` | Batch ingest of external grant files on timers |
| Data Transfer Object (wire structs) | `_MSG` macro + `MSG_*` structs cast over buffers | `Basedef.h:925-930` | Byte-compatible packet layout between processes |
| Builder (outbound packets) | `SendDBSignal`/`SendDBSignalParm2/3`/`SendAdminSignal`/`SendAdminMessage` | `CFileDB.cpp:2108-2200`, `Server.cpp:255-312` | Construct typed response packets |

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| Critical | `CFileDB` persistence | No transactionality, locking, or atomic rename; raw `_write` of account files | A crash mid-write can corrupt an account file; no recovery/backup evident |
| Critical | `Server.cpp` main loop | Single-threaded message pump; all sockets + file I/O serialized | One slow handler blocks all game servers and admin tools; limited scalability |
| High | Credential handling | Account passwords stored and compared in plaintext (`strcmp`/`strncmp`) | Credential exposure on disk and over the network |
| High | Admin authentication | Admin `Level` derived from in-game level minus 1000; weak (1000-value) threshold and no per-command rate limiting | Weak privilege boundary; admin-gated commands gated only by derived level |
| High | `CFileDB`/`Server.cpp` monoliths | `CFileDB.cpp` (2,688 lines) and `Server.cpp` (2,342 lines) concentrate disproportionate logic | Maintainability, error blast radius |
| High | Dual-login / state | `GetIndex(account)` linear scan over 10,000 slots; account file read on every login | Performance and consistency under load |
| Medium | `GetEncPassword`/`SetEncPassword` | Stub functions that always return FALSE / do nothing; TempKey "encryption" is ineffective | The server-change mechanism is effectively a cleartext token with no real cryptography |
| Medium | Capsule files | `./capsule/<id>` written without locking; `LastCapsule` counter persisted to `Config.txt` | Capsule file loss/collision risk |
| Medium | Packet validation | `BASE_CheckPacket` (size validation) is only compiled under `_PACKET_DEBUG` (Debug); Release trusts packet sizes | Malformed/oversized packets can cause over-reads/writes in Release |
| Medium | Stale project reference | `DBSrv.vcxproj` references nonexistent `CActivePinCode.h`; `_MSG_DBActivatePinCode` handler commented out | Build/feature inconsistency; dead code path |
| Medium | `ProcessMinTimer` | Empty stub | Missing periodic minute-level logic (no effect but indicates incomplete feature) |
| Medium | `DBExportAccount` | Hardcoded `S:/export/account<idx>/` absolute path | Environment-specific; fails on non-Windows/different mounts |
| Low | `ReadConfig`/`WriteConfig` | `Config.txt` read/write without robust parsing | Corrupted config silently resets values |
| Low | Resource leaks | `fopen`/`_open`/`FindFirstFile` not always closed on all error paths | Handle leaks under error conditions |

---

## 11. Test Coverage Analysis

**No automated tests exist anywhere in the analyzed scope.** A project-wide search for test
files (names matching `*test*` or `*spec*`) across the repository (excluding `.git`, `.opencode`,
`.codegraph`) returned zero results. The CodeGraph knowledge graph also reports
**"no covering tests found"** for the core handlers of this component (`AcceptUser`,
`AdminSocket`, `AdminClient`, `CFileDB::DBReadAccount`, `CFileDB::DeleteCharacter`, etc.).

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `Server.cpp` (main loop, accept, admin, timers) | 0 | 0 | 0% | None |
| `CFileDB` (persistence, login, char mgmt, capsules) | 0 | 0 | 0% | None |
| `CReadFiles` (imports, exports) | 0 | 0 | 0% | None |
| `CRanking` (ranking logic) | 0 | 0 | 0% | None |
| `CUser` (session state) | 0 | 0 | 0% | None |
| `CPSock` (socket buffering, shared) | 0 | 0 | 0% | None |

**Test file locations:** none found. There are no test directories, unit-test projects, or
test fixtures in the repository. This is a legacy Win32 codebase with no automated testing
infrastructure, and is a significant risk given the amount of low-level file I/O and binary
protocol handling in the component.

---

## 12. Analysis Notes and Limitations

- The component analysis is **read-only**; no project files were modified.
- Coupling metrics in Section 6 are **relative estimates** derived from the source-level call
  graph, not an exhaustive static-coupling enumeration.
- The `DBSrv.rc` resource file is UTF-16 encoded and was not fully parsed; it contains only
  the icon and dialog resource definitions referenced by `resource.h`.
- The `_MSG_DBActivatePinCode` handler is a commented-out stub and `CActivePinCode.h` is
  referenced in the project file but does not exist — noted as a risk above.
- Several functions (`GetEncPassword`, `SetEncPassword`, `ProcessMinTimer`) are stubs/no-ops;
  their intended behavior is inferred from context and documented as such.
- This report covers only the **Server (DBSrv)** component. The shared `Basedef` and `CPSock`
  layers are documented only to the extent they are used by DBSrv; they are separate components.
