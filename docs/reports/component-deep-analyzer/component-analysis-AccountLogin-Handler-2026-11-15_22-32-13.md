# Component Deep Analysis Report

**Component:** AccountLogin Handler
**Primary location:** `Code/TMSrv/_MSG_AccountLogin.cpp` (96 lines, 2,526 bytes)
**Related location:** `Code/TMSrv/_MSG_AccountSecure.cpp` (31 lines, 1,032 bytes)
**Project:** W2PP — decompiled C/C++ (Win32, MSVC, Visual Studio 2015) MMORPG server emulator for W2
**Analyzed host process:** TMSrv (game/world server)
**Collaborating process:** DBSrv (account database server)
**Analysis date:** 2026-11-15 22:32:13
**Excluded from analysis:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Mode:** read-only analysis. No project file was modified.

---

## 1. Executive Summary

### Purpose

`Exec_MSG_AccountLogin` is the TMSrv entry point for the very first application-level message a game client sends after the socket handshake: `_MSG_AccountLogin` (message type `13 | FLAG_CLIENT2GAME`, `Code/Basedef.h:1210`). Its responsibility is narrow and strictly gatekeeping: it decides whether an incoming credential packet is *admissible* on this server at this moment, captures the client adapter (MAC) fingerprint that travels inside the packet, normalizes the account name, and then forwards the still-encrypted-payload packet to DBSrv with a re-addressed header so that DBSrv can perform the authoritative credential check.

The function does **not** authenticate anything itself. No password comparison, no account-file access, and no database lookup occurs in TMSrv. Password verification, account existence, account blocking, and duplicate-session resolution all happen in DBSrv (`Code/DBSrv/CFileDB.cpp:611-854`). The login handler therefore constitutes a *pre-authentication filter plus transport relay* and is the single point in the game server where the client's claimed client-version and its connection state are trusted for the first time.

`Exec_MSG_AccountSecure` (`Code/TMSrv/_MSG_AccountSecure.cpp:21-31`) is the companion relay for the numeric second-factor token exchange (`_MSG_AccountSecure`, type `222`). It performs no validation beyond an index bounds check and forwards the packet verbatim to DBSrv.

### Role in the system

```
Client --MSG 13/222--> TMSrv (this component) --MSG 3--> DBSrv (CFileDB)
                             |                                |
                             <- MSG 22/33/34/36/37/38 ---------
                                      |
                     ProcessDBMessage handlers -> client
```

The component is the outermost admission layer of an eleven-state user-session machine (`USER_EMPTY`, `USER_ACCEPT`, `USER_LOGIN`, `USER_SELCHAR`, `USER_CHARWAIT`, `USER_WAITDB`, `USER_PLAY`, `USER_SAVING4QUIT` — `Code/TMSrv/CUser.h:26-37`). It is the only legal transition out of `USER_ACCEPT` into `USER_LOGIN`.

### Key findings

1. **The handler is a filter, not an authenticator.** All credential truth lives in DBSrv; TMSrv forwards the client's password bytes unmodified (`_MSG_AccountLogin.cpp:92`).
2. **Version gating is compile-time dependent and asymmetric.** The `m->ClientVersion != APP_VERSION` check at `_MSG_AccountLogin.cpp:44` is excluded when `_PACKET_DEBUG` is defined. The Debug configurations of both `TMSrv.vcxproj` (line 61) and `DBSrv.vcxproj` (line 61) define `_PACKET_DEBUG`; the Release configurations (lines 79 and 83) do not. A Debug build therefore accepts any client build, while a Release build enforces `APP_VERSION = 6975` (`Code/Basedef.h:42`).
3. **The "three wrong passwords" rule is implemented but unreachable.** `CheckFailAccount` (`Code/TMSrv/Server.cpp:1347-1361`) counts matches in `FailAccount[16][16]`, and the handler refuses the attempt when the count reaches 3 (`_MSG_AccountLogin.cpp:82-90`). However `AddFailAccount` (`Code/TMSrv/Server.cpp:1334-1345`) has **zero callers anywhere in the repository** (verified across `Code/`), and the only writes to `FailAccount` are two `memset` zeroing operations (`Code/TMSrv/Server.cpp:3623`, `Code/TMSrv/ProcessSecMinTimer.cpp:2136-2137`). The counter can never exceed 0, so the brute-force gate is dead code.
4. **Two tamper-sensitive client fields are relayed untouched into decision logic in DBSrv.** `MSG_AccountLogin.DBNeedSave` (`Code/Basedef.h:1225`) is never written by TMSrv yet drives DBSrv's duplicate-session branch (`Code/DBSrv/CFileDB.cpp:692-702`); `MSG_AccountLogin.Zero[52]` (`Code/Basedef.h:1220`) is used as a server-change hand-off key compared against `file.TempKey` (`Code/DBSrv/CFileDB.cpp:664-675`) and allows skipping the password comparison.
5. **Client-version mismatch and slot exhaustion share the same destructive response**: a version/size failure and an out-of-range slot both terminate the socket (`CloseUser`), so an operator tuning `APP_VERSION` incorrectly produces what looks like a network fault.
6. **No automated tests exist anywhere in the project.** The solution contains only the `DBSrv` and `TMSrv` projects; no test project, fixture, or assertion file exists outside the excluded `openspec` folder.

---

## 2. Data Flow Analysis

### Primary flow — account login (`_MSG_AccountLogin.cpp`)

```
 1. Client sends raw TCP frame: 4-byte INITCODE (0x1F11F311) + encrypted message.
    CPSock::ReadMessage validates INITCODE, header size bounds (MAX_MESSAGE_SIZE = 8192,
    >= sizeof(HEADER)), performs per-byte de-obfuscation with pKeyWord, and recomputes
    the checksum. A checksum mismatch does NOT discard the packet (Code/CPSock.cpp:353-466).

 2. TMSrv WSA_READ handler receives FD_READ, resolves socket -> slot via GetUserFromSocket,
    calls Receive(), then loops ReadMessage + ProcessClientMessage (Code/TMSrv/Server.cpp:3919-4004).

 3. ProcessClientMessage pre-validates: std->ID in [0, MAX_USER), ServerDown < 120,
    refreshes pUser[conn].LastReceiveTime, ignores _MSG_Ping and any client packet
    whose ClientTick equals SKIPCHECKTICK (Code/TMSrv/ProcessClientMessage.cpp:42-64).

 4. switch(std->Type) case _MSG_AccountLogin -> Exec_MSG_AccountLogin(conn, pMsg)
    (Code/TMSrv/ProcessClientMessage.cpp:68-70).

 5. Handler: AccountName[NAME_LENGTH-1] = 0 defensive NUL (_MSG_AccountLogin.cpp:28).

 6. Handler: slot eligibility gate  conn <= 0 || conn >= MAX_USER - ADMIN_RESERV
    -> "Reconnect" message, flush, CloseUser, return (_MSG_AccountLogin.cpp:30-39).

 7. Handler: size gate  Size < sizeof(MSG_AccountLogin)  [_PACKET_DEBUG builds]
             size gate + version gate  m->ClientVersion != APP_VERSION  [non-debug builds]
    -> "Version not match, rerun (%d)" message, flush, CloseUser, return (:41-54).

 8. Handler: state gate  pUser[conn].Mode != USER_ACCEPT
    -> "Login now, wait a moment." message, CrackLog(conn," accountlogin"), flush, return
    (socket kept open, no CloseUser) (:56-63).

 9. Handler: MAC capture. Short packet -> memset(Mac, 0xFF, 16); full packet ->
    memcpy(Mac, m->AdapterName, 16) (:65-70).

10. Handler: re-address for DBSrv: m->Type = _MSG_DBAccountLogin; m->ID = conn (:73-74).

11. Handler: account normalization: sscanf(m->AccountName,"%s", pUser[conn].AccountName);
    _strupr(...); strncpy(m->AccountName, pUser[conn].AccountName, NAME_LENGTH) (:76-80).

12. Handler: brute-force gate: CheckFailAccount(m->AccountName) >= 3
    -> "3 times wrong password" message, flush, return (no CloseUser) (:82-90).

13. Handler: DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_AccountLogin)) (:92)
    -> CPSock::AddMessage re-stamps Size/KeyWord/CheckSum/ClientTick and encrypts the
       payload into the DB socket send buffer (Code/CPSock.cpp:513-591).

14. Handler: pUser[conn].Mode = USER_LOGIN; pMob[conn].Mode = MOB_EMPTY (:94-95).

15. DBSrv ProcessMessage case _MSG_DBAccountLogin (Code/DBSrv/CFileDB.cpp:611):
    uppercase, reserved-name rejection (COM0-9/LPT0-9), file read, negative-coin clamp,
    date block, TempKey server-change bypass, plaintext password compare, duplicate
    session resolution, character-name swap fix, account-list registration, MAC storage,
    MSG_DBCNFAccountLogin emission, optional MSG_DBCheckPrimaryAccount broadcast.

16. TMSrv ProcessDBMessage case _MSG_DBCNFAccountLogin (Code/TMSrv/ProcessDBMessage.cpp:354-472):
    name echo check, blocked-MAC list check (pMac[200]), per-session flag reset,
    Mode = USER_SELCHAR, cargo/coin/SelChar copy, billing hook, MAC log line.
    Failure paths: _MSG_DBAccountLoginFail_Account (:487), _Fail_Block (:501),
    _Fail_Disable (:527), _Fail_Pass (:565) -> message + flush + CloseUser.
```

### Secondary flow — numeric token (`_MSG_AccountSecure.cpp`)

```
 1. ProcessClientMessage case _MSG_AccountSecure -> Exec_MSG_AccountSecure (ProcessClientMessage.cpp:88-90).
 2. Handler: conn <= 0 || conn >= MAX_USER -> silent return (_MSG_AccountSecure.cpp:25-26).
 3. Handler: m->ID = conn (:28).
 4. Handler: DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_AccountSecure)) (:30).
 5. DBSrv CFileDB case _MSG_AccountSecure (Code/DBSrv/CFileDB.cpp:1357-1421):
    SecurePass state machine (-1 unverified / 1 verified), numeric token create/compare/change.
 6. DBSrv replies _MSG_AccountSecure or _MSG_AccountSecureFail.
 7. TMSrv ProcessDBMessage relays both signals to the client (ProcessDBMessage.cpp:541-551).
```

### Data elements carried

| Field | Type/Size | Origin | Mutated by component | Consumed by |
|---|---|---|---|---|
| `Size` | `short` (header) | client frame | rewritten by `AddMessage` | handler size gate; DB size validation |
| `Type` | `short` | client (`13`) | **rewritten to `_MSG_DBAccountLogin`** | DBSrv dispatch |
| `ID` | `short` | client | **rewritten to `conn`** | DBSrv session mapping |
| `ClientTick` | `unsigned int` | client | rewritten to `CurrentTime` | pre-dispatch SKIPCHECKTICK filter |
| `AccountPassword[12]` | `char[12]` | client | untouched | DBSrv `strcmp` vs account file |
| `AccountName[16]` | `char[16]` | client | uppercased, NUL-capped, `sscanf`-tokenized | DBSrv file lookup, TMSrv `pUser[conn].AccountName` |
| `Zero[52]` | `char[52]` | client | untouched | DBSrv server-change key (`TempKey` compare) |
| `ClientVersion` | `int` | client | untouched | TMSrv version gate only |
| `DBNeedSave` | `int` | client | untouched | DBSrv duplicate-session branch |
| `AdapterName[4]` | `int[4]` | client | copied to `pUser[conn].Mac` | TMSrv MAC block list; DBSrv `pAccountList[].Mac`; log line |
| `NumericToken[6]` / `ChangeNumeric` | `char[6]` / `int` | client | untouched | DBSrv `SecurePass` state machine |

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---|---|---|---|
| BR-01 | Validation / Session | Reject login on slot index 0 or on slots reserved for administration (`conn <= 0 \|\| conn >= MAX_USER - ADMIN_RESERV`), send "Reconnect", close socket | `Code/TMSrv/_MSG_AccountLogin.cpp:30-39` |
| BR-02 | Validation / Framing | Reject packets shorter than `sizeof(MSG_AccountLogin)` (116 bytes) | `Code/TMSrv/_MSG_AccountLogin.cpp:42,44` |
| BR-03 | Validation / Compatibility | Reject login when `ClientVersion != APP_VERSION` (6975); **only compiled when `_PACKET_DEBUG` is undefined** | `Code/TMSrv/_MSG_AccountLogin.cpp:26,41-45`; `Code/TMSrv/TMSrv.vcxproj:61,79` |
| BR-04 | Validation / State | Login accepted only from `USER_ACCEPT`; any other state yields "Login now, wait a moment." plus a crack log, socket retained | `Code/TMSrv/_MSG_AccountLogin.cpp:56-63` |
| BR-05 | Data capture | Capture 16 bytes of `AdapterName` into `pUser[conn].Mac`; when the packet is shorter than the full struct, fill the MAC with `0xFF` | `Code/TMSrv/_MSG_AccountLogin.cpp:65-70` |
| BR-06 | Protocol translation | Re-stamp the client packet as a DB packet: `Type = _MSG_DBAccountLogin`, `ID = conn` | `Code/TMSrv/_MSG_AccountLogin.cpp:73-74` |
| BR-07 | Data normalization | Account name is tokenized with `sscanf("%s")`, uppercased with `_strupr`, and copied back into the packet capped at `NAME_LENGTH` (16) with a forced NUL at index 15 | `Code/TMSrv/_MSG_AccountLogin.cpp:28,76-80` |
| BR-08 | Anti-brute-force | Refuse login when the account has 3 or more entries in the `FailAccount` ring buffer (16 slots x 16 chars) | `Code/TMSrv/_MSG_AccountLogin.cpp:82-90`; `Code/TMSrv/Server.cpp:1334-1361` |
| BR-09 | Transport | Forward the credential packet to DBSrv as a fixed 116-byte message | `Code/TMSrv/_MSG_AccountLogin.cpp:92` |
| BR-10 | State transition | On forward, set `pUser[conn].Mode = USER_LOGIN` and `pMob[conn].Mode = MOB_EMPTY` | `Code/TMSrv/_MSG_AccountLogin.cpp:94-95` |
| BR-11 | Validation (downstream) | DBSrv rejects accounts whose name is a reserved DOS device alias `COM0`-`COM9` or `LPT0`-`LPT9` | `Code/DBSrv/CFileDB.cpp:619-625`; `Code/DBSrv/CFileDB.cpp:2481-2484, 2531-2534` |
| BR-12 | Validation (downstream) | DBSrv rejects login when the account file cannot be read/does not exist | `Code/DBSrv/CFileDB.cpp:636-643` |
| BR-13 | Business (downstream) | DBSrv blocks the account while `Info.Year`/`Info.YearDay` indicate a block date at or after "now" | `Code/DBSrv/CFileDB.cpp:648-660` |
| BR-14 | Business (downstream) | A non-empty `TempKey` matching the client's `Zero[52]` field bypasses password verification and switches to server-change mode | `Code/DBSrv/CFileDB.cpp:664-675` |
| BR-15 | Authentication (downstream) | DBSrv compares the client password against `Info.AccountPass` with `strcmp` (plaintext, case-sensitive) | `Code/DBSrv/CFileDB.cpp:677-682` |
| BR-16 | Concurrency (downstream) | If the account is already logged in on another TMSrv session, DBSrv answers `_MSG_DBAlreadyPlaying` when `DBNeedSave == 0`, otherwise `_MSG_DBStillPlaying` and issues a saving-quit to the previous session | `Code/DBSrv/CFileDB.cpp:685-703` |
| BR-17 | Lifecycle | The `FailAccount` buffer is zeroed at server start-up and every 10 minutes of `MinCounter` | `Code/TMSrv/Server.cpp:3623`; `Code/TMSrv/ProcessSecMinTimer.cpp:2136-2137` |
| BR-18 | Related relay | `_MSG_AccountSecure` is forwarded with `ID = conn` after a bounds check only; no state or size validation | `Code/TMSrv/_MSG_AccountSecure.cpp:21-31` |
| BR-19 | Downstream relay | TMSrv validates that a returning `_MSG_DBCNFAccountLogin` echoes the account name it forwarded, otherwise it closes the connection with "Try to reconnect" | `Code/TMSrv/ProcessDBMessage.cpp:358-367` |
| BR-20 | Downstream relay | TMSrv rejects login when the captured MAC matches an entry in the operator-maintained `pMac[200]` block list (`../../DBSRV/Run/Mac.txt`) | `Code/TMSrv/ProcessDBMessage.cpp:369-383`; `Code/TMSrv/CReadFiles.cpp:40,678-715` |

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Session slot eligibility for login

**Overview**:
Login is accepted only on a user slot whose index is strictly greater than zero and strictly below `MAX_USER - ADMIN_RESERV` (`1000 - 10 = 990`). Violations terminate the socket with the localized "Reconnect" text (`g_pMessageStringTable[_NN_Reconnect]`, index 191).

**Detailed description**:

The rule reflects the layout of the `pUser[MAX_USER]` array, where slot 0 is deliberately excluded from allocation by `GetEmptyUser`, which iterates from index 1 (`Code/TMSrv/Server.cpp:6743-6752`). Index 0 is effectively a sentinel: many functions treat `conn <= 0` as "no connection" (for example `CPSock::SendClientMessage` returns immediately, `CrackLog` falls back to a mob-name branch, and `ProcessClientMessage` logs and drops any packet whose `ID` is out of `[0, MAX_USER)`). Allowing a login on slot 0 would therefore create a session that cannot send messages and cannot be logged. The upper bound encodes a capacity reservation: the last ten slots (`990`-`999`) are kept for administration, so a full server still has room for operational intervention. The same constant pair is enforced twice — once during accept (`Code/TMSrv/Server.cpp:4022-4030`) and once again here — which means the reservation is not a single point of configuration failure.

Because the check is duplicated, an accepted connection on a reserved slot normally never reaches this handler; if it does, the reserved-slot rejection path exists as a defence-in-depth measure. Both paths share the same observable behaviour: a `_MSG_MessagePanel` text is queued, the send buffer is flushed with `SendMessageA`, and `CloseUser` is invoked, which closes the socket, clears `IsBillConnect`, sets `Mode = USER_EMPTY` and blanks the account name (`Code/TMSrv/CUser.cpp:81-92`). The rule therefore guarantees the invariant that any slot entering `USER_LOGIN` is a normal, addressable, non-reserved session.

Note the ordering defect with no exploitable consequence: the defensive write `pUser[conn].AccountName[NAME_LENGTH - 1] = 0` occurs at line 28, *before* the index validation at line 30. Safety depends entirely on the caller, `ProcessClientMessage`, which rejects `std->ID` outside `[0, MAX_USER)` (`Code/TMSrv/ProcessClientMessage.cpp:42-51`). The check is nevertheless meaningful when the handler is entered from a non-standard caller, which does not exist in this codebase.

**Rule workflow**:
```
receive conn
  -> if conn <= 0 OR conn >= (1000 - 10):
        format g_pMessageStringTable[191] ("reconnect")
        SendClientMessage(conn, ...)          [queues MSG_MessagePanel]
        cSock.SendMessageA()                  [flushes]
        CloseUser(conn)                       [socket close, Mode = USER_EMPTY, AccountName = ""]
        return
  -> continue to size/version gate
```

---

### Business Rule: BR-02 — Minimum credential packet size

**Overview**:
The handler rejects any login packet whose declared `Size` is smaller than `sizeof(MSG_AccountLogin)` = 116 bytes.

**Detailed description**:

`MSG_AccountLogin` is declared with `#pragma pack(push,1)` and contains a 12-byte header (`Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`), `AccountPassword[12]`, `AccountName[16]`, `Zero[52]`, `ClientVersion`, `DBNeedSave`, and `AdapterName[4]` — 12 + 104 = 116 bytes (`Code/Basedef.h:1213-1228`). The size gate protects two distinct things at once: it prevents the handler from reading `ClientVersion`, `DBNeedSave` and `AdapterName` out of a truncated buffer, and it prevents an under-sized structure from being forwarded to DBSrv where the same 116-byte layout is assumed. The lower bound is deliberately a *minimum*, not an equality test, which is what allows the historical `MSG_AccountLogin_HWID` extension (`Code/Basedef.h:1229-1245`, +50 bytes HWID) to remain wire-compatible: a longer packet passes the gate and its extra tail is simply not read by this handler.

The gate is expressed twice. Lines 41-45 wrap it in `#ifdef _PACKET_DEBUG`: the debug build compiles only the size comparison, whereas the non-debug build compiles size **or** version comparison. Line 67 then re-evaluates `m->Size < sizeof(MSG_AccountLogin)` in order to choose the MAC-fill strategy (see BR-05). Consequently, an under-sized packet can only reach the MAC branch if the earlier gate has been removed by a code change; in the current source the second test at line 67 is statically false when reached, which makes the `memset(..., 0xFF, ...)` branch unreachable through this code path. This is a preserved residue of an older protocol revision in which shorter login packets without an adapter field were legal, and it documents that the MAC field was appended to the protocol after the account name block.

On failure the response text is the version-mismatch string with `APP_VERSION` substituted (`_NN_Version_Not_Match_Rerun`, index 36, formatted with `%d`), followed by a flush and `CloseUser`. A malformed-size client therefore receives a misleading message that mentions a version number rather than a framing problem — an observability weakness rather than a logic error.

**Rule workflow**:
```
Size = m->Size
if defined(_PACKET_DEBUG):
    if Size < 116: reject("version not match 6975") -> flush -> CloseUser -> return
else:
    if Size < 116 OR m->ClientVersion != 6975: reject(...) -> flush -> CloseUser -> return
```

---

### Business Rule: BR-03 — Client build/version gate

**Overview**:
A client may only authenticate if its embedded build number equals `APP_VERSION`, which is `6975` (`Code/Basedef.h:42`). This rule is compiled into Release builds and compiled out of Debug builds.

**Detailed description**:

The gate is a strict, symmetric equality between the 32-bit `ClientVersion` integer supplied by the client and the compile-time constant `APP_VERSION`. There is no range tolerance, no minor-version window, and no allow-list. Operationally this means one server binary serves exactly one client build, and both sides must be rebuilt in lock-step: TMSrv's `APP_VERSION` and the client's build stamp. The failure message is formatted with the same constant (`sprintf(temp, g_pMessageStringTable[_36], APP_VERSION)`), which conveniently discloses to the operator — and to any client — the exact build number the server expects. Because the rejection also calls `CloseUser`, a stale client cannot linger in `USER_ACCEPT` to retry with a different build number; it must reconnect and re-handshake.

The compile-time dependence is the crucial subtlety. `_MSG_AccountLogin.cpp:41` opens an `#ifdef _PACKET_DEBUG` block. In Debug builds, `_PACKET_DEBUG` is defined by the project file (`Code/TMSrv/TMSrv.vcxproj:61`) and the version comparison is not compiled, so the condition reduces to the size test only. In Release (`Code/TMSrv/TMSrv.vcxproj:79`) the macro is absent and the version comparison is enforced. Debug builds are therefore permissive by construction — intentional for client-development convenience, but it means "version enforcement" cannot be demonstrated in the configuration a developer is most likely to run. The same macro also disables `BASE_CheckPacket` logging and enables extra send/recv diagnostics (`Code/CPSock.cpp:544-552,636-656`).

A second-order observation: `APP_VERSION` is `6975`, while the repository ships a client patch directory named `ClientPatch_v7662`. These are different numbering schemes (server protocol revision versus client-patch release), so no contradiction can be asserted from the source alone — the pairing is not documented anywhere in the code, and is recorded here as an ambiguity rather than a defect.

**Rule workflow**:
```
(Release only)
if m->ClientVersion != APP_VERSION (6975):
    temp = "version not match, rerun <6975>"
    SendClientMessage(conn, temp); cSock.SendMessageA(); CloseUser(conn)
    return
(Debug builds skip this comparison entirely)
```

---

### Business Rule: BR-04 — Login only from the USER_ACCEPT state

**Overview**:
The handler requires `pUser[conn].Mode == USER_ACCEPT` (value 1). Any other state produces the message "Login now, wait a moment.", a crack-log entry, and a buffer flush — but the socket stays open.

**Detailed description**:

`USER_ACCEPT` is the state installed by `CUser::AcceptUser` together with the peer IP (`Code/TMSrv/CUser.cpp:73-75`). It is the only state from which `USER_LOGIN` may be entered, and it is restored after a character logout (`Code/TMSrv/ProcessDBMessage.cpp:1180`). The rule is therefore a session-state-machine guard: it prevents a second `_MSG_AccountLogin` from being processed while the first is still in flight at DBSrv (`USER_LOGIN`), and it prevents re-login from any post-login state (`USER_SELCHAR`, `USER_PLAY`, `USER_SAVING4QUIT`). This matters because the DBSrv duplicate-session logic keys off account-name occupancy: replaying a login mid-flight would either desynchronize the two servers' views or be interpreted as a takeover attempt.

Unlike the size, version and slot gates, this failure path deliberately does **not** call `CloseUser`. The connection is kept alive and the client is told to wait, which is the correct behaviour for a transient overlap (for example a client that pushed two login packets in one TCP segment). The side effect is that `Mode` remains `USER_LOGIN`; retries will keep hitting the same branch until DBSrv answers. Each such attempt also writes `"conn:%d mode:%d name:%s accountlogin"` through `CrackLog` (`Code/TMSrv/Server.cpp:6671-6699`), which is an operational signal an operator can use to detect a client stuck in retry. Note that `CrackLog` truncates the account name at indices 14 and 15 before logging, and it also calls `SetWindowTextA` on the main window — so a burst of these events disturbs the server UI title, which is a pre-existing side effect of the shared logging helper.

Because the branch is unthrottled and does not increment any counter (`AddCrackError` is not called here), a client in `USER_LOGIN` can generate unlimited "wait" responses. The only consumption is socket send-buffer space, bounded by `SEND_BUFFER_SIZE` (128 KB) and the `AddMessage` guard at `Code/CPSock.cpp:518-524`.

**Rule workflow**:
```
if pUser[conn].Mode != USER_ACCEPT:
    SendClientMessage(conn, "Login now, wait a moment.")
    CrackLog(conn, " accountlogin")     [writes -system log + window title]
    cSock.SendMessageA()
    return                               [socket retained, Mode unchanged]
```

---

### Business Rule: BR-05 — Adapter (MAC) capture with 0xFF sentinel

**Overview**:
The 16 bytes at `m->AdapterName` are copied into `pUser[conn].Mac` (an `int[4]`). If the packet is shorter than the full struct, the MAC is instead filled with `0xFF` bytes.

**Detailed description**:

The MAC is captured at the earliest possible moment — before the account name is normalized and before the packet is forwarded — so that it exists in the TMSrv session regardless of whether DBSrv accepts the credentials. It is used immediately after the DB round-trip: the `_MSG_DBCNFAccountLogin` handler compares all four integers against the operator-supplied block list `pMac[200]` loaded from `../../DBSRV/Run/Mac.txt` (`Code/TMSrv/ProcessDBMessage.cpp:369-383`, `Code/TMSrv/CReadFiles.cpp:40,678-715`), and it logs `"CNFAccountLogin Mac: %d.%d.%d.%d"` alongside the account name and IP (`Code/TMSrv/ProcessDBMessage.cpp:456-457`). On the DBSrv side the same four integers are stored in the in-memory account record `pAccountList[Idx].Mac[]` (`Code/DBSrv/CFileDB.cpp:751-754`) and are used by `GetAccountsByMac` to decide whether the login is a "primary account" worthy of a `_MSG_DBCheckPrimaryAccount` broadcast (`Code/DBSrv/CFileDB.cpp:782-801`).

The `0xFF` fill is a sentinel, not a real address. It makes a short packet's MAC distinguishable from a legitimate all-zero or random-looking address, and it will never match a value parsed from `Mac.txt` because the loader accepts decimal octets in `0..255` in four separate integers (`sscanf(ttt, "%d %d %d %d %d", ...)`) — although a determined operator could write `255 255 255 255` and thereby block all legacy clients. As established under BR-02, the branch that produces this sentinel is unreachable through the current size gate, so in practice `pUser[conn].Mac` always receives client-supplied `AdapterName` values verbatim.

Two properties of this capture deserve emphasis. First, the value is entirely client-controlled and unauthenticated; it is a soft identifier used for policy and correlation, never for authentication (the DBSrv code that stores it does not compare it against anything). Second, `sizeof(pUser[conn].Mac)` is 16 bytes and `AdapterName` is exactly `int[4]`, so the copy is correctly sized — but the `memcpy` is executed only after the size gate has proved the field is present, which is the reason the gate must precede the copy.

**Rule workflow**:
```
if m->Size < sizeof(MSG_AccountLogin):        [only reachable if BR-02 removed]
    memset(pUser[conn].Mac, 0xFF, 16)
else:
    memcpy(pUser[conn].Mac, m->AdapterName, 16)
```

---

### Business Rule: BR-06 — Re-addressing the client packet for the DB channel

**Overview**:
The same buffer that arrived from the client is retyped to `_MSG_DBAccountLogin` (`3 | FLAG_GAME2DB`) and its `ID` is overwritten with the TMSrv slot index `conn`.

**Detailed description**:

This in-place mutation is the protocol bridge between the client-facing namespace (`FLAG_CLIENT2GAME`, type 13) and the internal server-to-server namespace (`FLAG_GAME2DB`, type 3), both declared adjacently at `Code/Basedef.h:978` and `Code/Basedef.h:1210`. Reusing the inbound buffer avoids an allocation and a copy, and it is only safe because the `MSG_AccountLogin` layout is identical in both directions — DBSrv casts the received buffer back to `MSG_AccountLogin` immediately (`Code/DBSrv/CFileDB.cpp:613`). `ProcessDBMessage` on the return path enforces the mirror-image convention by rejecting anything whose high byte lacks `FLAG_DB2GAME` (`Code/TMSrv/ProcessDBMessage.cpp:43-52`).

The `ID` overwrite is the load-bearing part of the rule. TMSrv trusts its own slot index, not the client's claimed ID: whatever the client put in `MSG_STANDARD.ID` is discarded and replaced. DBSrv then uses `ID` both to route the reply back to the correct TMSrv session and to address the per-session account slot through `GetIndex(server, id) = server * MAX_USER + id` (`Code/DBSrv/CFileDB.cpp:2328-2333`). A client that forged `ID = 500` while connected on slot 7 cannot redirect another player's login as a result of this overwrite — this is the single most important security property of the handler.

One consequence is that header fields are re-stamped twice: first here (`Type`, `ID`), then by `CPSock::AddMessage`, which overwrites `Size`, `KeyWord`, `CheckSum` and `ClientTick` before encryption (`Code/CPSock.cpp:535-542`). No client-supplied header value survives into the DB channel.

**Rule workflow**:
```
m->Type = _MSG_DBAccountLogin     (3 | 0x0800)
m->ID   = conn
m->Size, m->KeyWord, m->CheckSum, m->ClientTick  <- overwritten by AddMessage
```

---

### Business Rule: BR-07 — Account-name normalization and bounding

**Overview**:
The account name is tokenized out of the packet with `sscanf("%s")`, uppercased in place with `_strupr`, written into `pUser[conn].AccountName`, and copied back into the packet capped at 16 bytes, with a defensive NUL pre-written at index 15.

**Detailed description**:

Four operations compose into one normalization contract. `sscanf(m->AccountName, "%s", pUser[conn].AccountName)` stops at the first whitespace character, which strips trailing padding that clients commonly emit and also neutralizes an embedded space as an injection vector for `%s`-based formatting downstream. `_strupr` folds the name to upper case, making the subsequent file lookup and the DBSrv password record effectively case-insensitive with respect to the account identifier (DBSrv re-applies `_strupr` at `Code/DBSrv/CFileDB.cpp:615` and again on the file copy at line 705, so the canonical form is enforced on both sides). `strncpy(m->AccountName, pUser[conn].AccountName, NAME_LENGTH)` writes the canonical form back so that the forwarded packet matches what TMSrv stored; `strncpy` does not NUL-terminate when the source is exactly 16 bytes, which is why line 28 pre-clears index 15.

`NAME_LENGTH` is 16 and `ACCOUNTNAME_LENGTH` is also 16 (`Code/Basedef.h:132,65`), so no truncation mismatch exists between the session field and the wire field. The visible effect of the cap is that a 20-character account name submitted by a client is silently truncated to 16 characters before any lookup; two distinct over-long names sharing their first 16 characters therefore collapse to one identifier at this layer. This is a silent-truncation rule, not a rejection rule, and it is consistent with the DBSrv account-file key, which is also a 16-byte name.

The normalized name is also the value used by BR-08's counter lookup (`CheckFailAccount(m->AccountName)`) and by the return-path echo check (`strcmp(m->AccountName, pUser[conn].AccountName)` at `Code/TMSrv/ProcessDBMessage.cpp:358`). Because both sides of that comparison derive from the same normalization, the echo test can only fail if DBSrv returns a different account than requested — which is exactly the anomaly it is designed to catch.

**Rule workflow**:
```
pUser[conn].AccountName[15] = 0                    [defensive pre-clear, line 28]
sscanf(m->AccountName, "%s", pUser[conn].AccountName)   [tokenize at whitespace]
_strupr(pUser[conn].AccountName)                        [canonical upper case]
strncpy(m->AccountName, pUser[conn].AccountName, 16)    [write back, bounded]
```

---

### Business Rule: BR-08 — Brute-force gate: three failed attempts per account

**Overview**:
If `CheckFailAccount(m->AccountName) >= 3`, the login is refused with `g_pMessageStringTable[_NN_3_Tims_Wrong_Pass]` (index 37). The counter is backed by the global `FailAccount[16][16]` ring buffer. In the current codebase no code path ever inserts an entry, so the gate is unreachable.

**Detailed description**:

The intended behaviour is a per-account-name attempt budget: `AddFailAccount` scans `FailAccount[16][16]` for the first row whose first character is `0` and copies the account name into it (`Code/TMSrv/Server.cpp:1334-1345`); `CheckFailAccount` scans the same 16 rows and counts exact `strcmp` matches (`Code/TMSrv/Server.cpp:1347-1361`). The handler consumes only the count and refuses at 3 or more. The natural insertion point would be the `_MSG_DBAccountLoginFail_Pass` case in `ProcessDBMessage`, immediately before the "wrong password" message and `CloseUser` (`Code/TMSrv/ProcessDBMessage.cpp:564-576`) — that case knows both the account name (via `pUser[conn].AccountName`) and that DBSrv rejected the password. It does not call `AddFailAccount`.

A repository-wide search for `AddFailAccount` returns exactly two hits: the definition and the declaration in `Code/TMSrv/Server.h:76`. There are no call sites. Consequently `FailAccount` is only ever written by `memset` (start-up at `Code/TMSrv/Server.cpp:3623`, and every tenth `MinCounter` tick at `Code/TMSrv/ProcessSecMinTimer.cpp:2136-2137`), so every row's first byte remains `0`, `CheckFailAccount` always returns `0`, and the `check >= 3` branch is unreachable. Confidence: **high** (verified by exhaustive text search over `Code/` for the symbol, plus inspection of every `FailAccount` reference).

Even under the intended wiring, three structural limits would bound the rule. First, the capacity is 16 names server-wide, not 16 per account: once 16 distinct names have failed, `AddFailAccount` silently drops further insertions (its loop falls through without a return). Second, `_strupr`-normalized names of 16 bytes fit exactly in the 16-byte rows only because the copy is `strcpy` of a bounded source — an over-long source would overflow; the current normalization (BR-07) bounds it to 15 characters plus NUL, so the copy is safe. Third, the window is time-based (10-minute decay, BR-17), not attempt-window based, so a slow attacker sees the count reset. Because the insertion point is missing, none of this affects current behaviour, but it explains why the rule as written could not have been a strong brute-force defence even when active.

**Rule workflow**:
```
int check = CheckFailAccount(m->AccountName)     [counts rows 0..15 matching name]
if check >= 3:
    SendClientMessage(conn, g_pMessageStringTable[37])   ["3 times wrong password"]
    cSock.SendMessageA()
    return                                        [socket retained, no MODE change]
-- intended but absent: AddFailAccount(name) on _MSG_DBAccountLoginFail_Pass
```

---

### Business Rule: BR-09 — Credential forwarding to DBSrv

**Overview**:
The retyped packet is transmitted with `DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_AccountLogin))`, i.e. always 116 bytes regardless of the size the client declared.

**Detailed description**:

`SendOneMessage` is `AddMessage` followed by an immediate flush (`Code/CPSock.cpp:686-693`). `AddMessage` bounds-checks the 128 KB send buffer, writes the payload into it while applying the per-byte obfuscation table `pKeyWord` keyed by a random `iKeyWord`, and recomputes `CheckSum` as the difference of the encrypted-byte sum and the plaintext-byte sum (`Code/CPSock.cpp:513-591`). The DB link is the same framing as the client link, so the login packet is encrypted on one untrusted socket (client) and re-encrypted on a second, trusted-but-plaintext-TCP socket (DB). The `sizeof` argument — rather than `m->Size` — is the effective enforcement of the 116-byte contract: a client that declared a larger size (for instance a `MSG_AccountLogin_HWID`) has its extra bytes dropped here, and a client that declared exactly 116 has its full structure forwarded.

The forward is unconditional with respect to content: the password, the `Zero[52]` block, `ClientVersion`, `DBNeedSave` and `AdapterName` all travel byte-for-byte. This is why the two client-controlled fields noted in the executive summary are escalation-relevant: DBSrv's duplicate-session branch and server-change branch both read values that TMSrv validated in no way whatsoever. The only content mutation performed before forwarding is the account-name normalization of BR-07.

Reliability characteristics are inherited from `CPSock`: if `DBServerSocket` is disconnected, `Sock <= 0` makes `AddMessage` log `"err,add buffer invalid"` and return `FALSE`, and `SendMessageA` resets the send positions and returns `FALSE`; the handler ignores both return values and proceeds to set `USER_LOGIN` anyway. A client can therefore be left in `USER_LOGIN` with no DB request in flight, waiting for a reply that will never come, until the idle checker closes the session after 720 seconds without a received packet (`CheckIdle`, `Code/TMSrv/Server.cpp:4304-4322`) or the client disconnects. Note that `LastReceiveTime` is refreshed by any incoming packet including `_MSG_Ping` (`Code/TMSrv/ProcessClientMessage.cpp:56-60`), so a client that keeps pinging a stalled session is never reaped by the idle checker.

**Rule workflow**:
```
DBServerSocket.SendOneMessage((char*)m, 116)
   -> AddMessage(m, 116): size bound -> socket validity -> stamp header -> encrypt + checksum
   -> SendMessageA(): flush, reset positions
return values ignored
```

---

### Business Rule: BR-10 — Session state transition after forwarding

**Overview**:
After a successful forward the session becomes `USER_LOGIN` (2) and the paired mob slot becomes `MOB_EMPTY`.

**Detailed description**:

`USER_LOGIN` means "credentials have been forwarded, awaiting DBSrv's verdict". It is the state checked by BR-04 on re-entry, and it is left behind by the `_MSG_DBCNFAccountLogin` handler which installs `USER_SELCHAR` (`Code/TMSrv/ProcessDBMessage.cpp:400`) or, on failure, terminates the socket. The second assignment, `pMob[conn].Mode = MOB_EMPTY`, severs the user slot from any leftover mob state: the `pMob` array shares indices with `pUser` for the first `MAX_USER` entries and holds the player's live mob when `USER_PLAY` is active. Clearing it at login prevents a stale mob (from a previous session on the same slot, or from an interrupted character login) from being visible or targetable during the character-selection phase.

Both assignments occur after the DB send, so a failed DB write leaves the session in `USER_LOGIN` rather than `USER_ACCEPT` (see BR-09). No `AccountName` length fixups, no MAC clearing and no `LastReceiveTime` update occur here — the latter is maintained by the dispatcher (`Code/TMSrv/ProcessClientMessage.cpp:56-57`).

**Rule workflow**:
```
pUser[conn].Mode = USER_LOGIN   (2)
pMob[conn].Mode  = MOB_EMPTY
```

---

### Business Rule: BR-11 — Reserved DOS device names cannot log in (DBSrv)

**Overview**:
DBSrv rejects any account whose upper-cased name is `COM0`-`COM9` or `LPT0`-`LPT9` (four characters plus NUL) with `_MSG_DBAccountLoginFail_Account`.

**Detailed description**:

This is a filesystem-safety rule, not a gameplay rule. Accounts are stored as individual files under `./account/<FirstKey>/<NAME>` (`Code/DBSrv/CFileDB.cpp:2536-2544`), and on Win32 the names `COM1`..`COM9` and `LPT1`..`LPT9` are device aliases that bypass normal file semantics regardless of directory. The check is repeated in three places — the login handler (`Code/DBSrv/CFileDB.cpp:619-625`), `DBReadAccount` (`:2531-2534`) and `DBExportAccount` (`:2481-2484`) — so a pre-existing file or an export batch cannot reintroduce the hazard. Only single-digit forms are blocked (`ac[4] == 0`), so `COM10` is legal.

Because it fires before the account file is opened, the rule is reached for names that never existed. TMSrv's corresponding handler then emits "no account with that name" (`Code/TMSrv/ProcessDBMessage.cpp:487-498`) and closes the socket, making the rejection indistinguishable from a genuine non-existent account — a deliberate non-disclosure choice.

**Rule workflow**:
```
_strupr(name)
if name matches COM[0-9]\0 or LPT[0-9]\0 -> send _MSG_DBAccountLoginFail_Account -> return
```

---

### Business Rule: BR-12 — Account existence check (DBSrv)

**Overview**:
If `DBReadAccount` fails, DBSrv answers `_MSG_DBAccountLoginFail_Account`; TMSrv shows "no account with that name" and closes the connection.

**Detailed description**:

`DBReadAccount` first forces the last two bytes of both the name and password to NUL, then re-applies the device-name guard, derives the shard directory via `BASE_GetFirstKey`, opens `./account/<key>/<NAME>` in binary mode and returns `FALSE` on failure (`Code/DBSrv/CFileDB.cpp:2518-2556`). The handler treats `ret == 0` as non-existence and answers with the "account" failure signal (`Code/DBSrv/CFileDB.cpp:636-643`). The return value convention is inconsistent between the branches of this function (`return FALSE` here, `return TRUE` for the password failure, bare `return` for the success path), which is a readability hazard but not a behaviour change, since the caller only tests for `0`.

The account-existence answer is the earliest point at which an attacker learns whether a name is registered. The message text differs from the wrong-password text, so account enumeration is possible by design — a pre-existing product decision, recorded here as an observation. TMSrv's handling closes the socket in both cases and does not increment any counter.

**Rule workflow**:
```
if DBReadAccount(&file) == 0:
    send _MSG_DBAccountLoginFail_Account
    return
-> TMSrv: "no account with that name" -> flush -> CloseUser
```

---

### Business Rule: BR-13 — Date-based account block (DBSrv)

**Overview**:
If the account file carries a non-zero `Info.Year` and `Info.YearDay` whose date is at or after "now", the account is blocked with `_MSG_DBAccountLoginFail_Block` and parameter `0`.

**Detailed description**:

`Info.Year` and `Info.YearDay` (`Code/Basedef.h:793-794`) are an absolute block expiry encoded as two integers in `tm` conventions (`tm_year` is years since 1900, `tm_yday` is 0-based day-of-year). The comparison `file.Info.Year >= when.tm_year || file.Info.Year >= when.tm_year && file.Info.YearDay >= when.tm_yday` (`Code/DBSrv/CFileDB.cpp:648-660`) is redundant in form — the left disjunct subsumes the right when the years are equal, and the right disjunct can never add a true case the left did not already cover — but its effective semantics are "block while the recorded year is the current year or later". Because the day component is effectively ignored, an account blocked for a future year stays blocked for the whole of that year regardless of the recorded day.

The reply carries `Parm = 0`, and TMSrv formats `g_pMessageStringTable[_SN_Blocked_Account]` with the literal `"X"` (`Code/TMSrv/ProcessDBMessage.cpp:507-508`). The handler also implements parameters 1, 2 and 3, mapping to `"A"`, `"B"`, `"C"` (`:510-517`), but DBSrv never emits them in this build — only `0` is sent (`Code/DBSrv/CFileDB.cpp:657`). Note the size mismatch this creates with the (dead) validation table, which expects `sizeof(MSG_STANDARD)` for `_MSG_DBAccountLoginFail_Block` (`Code/Basedef.cpp:6571`) while DBSrv sends `MSG_STANDARDPARM` — harmless only because `BASE_CheckPacket` returns `FALSE` unconditionally.

The block is per-account-file state and survives restarts; it is unrelated to the in-memory `FailAccount` mechanism of BR-08 and to the MAC list of BR-20.

**Rule workflow**:
```
if Info.Year != 0 and Info.YearDay != 0:
    now = localtime(time())
    if Info.Year >= now.tm_year (or same year and YearDay >= now.tm_yday):
        send _MSG_DBAccountLoginFail_Block(parm=0)
        return
-> TMSrv: "blocked account (X)" -> flush -> CloseUser
```

---

### Business Rule: BR-14 — Server-change bypass via TempKey (DBSrv)

**Overview**:
When the account record holds a non-empty `TempKey` and the login packet's `Zero[52]` field is non-empty and byte-identical to it, DBSrv clears the key, marks `ChangeServer = 1`, and jumps directly to the success label, skipping password verification.

**Detailed description**:

This is the cross-server hand-off mechanism. `TempKey` is a per-account ticket written into the account record by the source shard before a character transfer; the destination shard receives it in the `Zero` field of the login packet and compares it with `memcmp` over `sizeof(file.TempKey)` (`Code/DBSrv/CFileDB.cpp:664-675`). On match the key is wiped in memory, `ChangeServer = 1` is set, and control jumps to `lb_sucess` (`:683`), so the password comparison at `:677` is never executed. On mismatch — but only when both sides are non-empty — the stored key is wiped, the file is written back, and the function returns without any signal to TMSrv, leaving the client waiting.

The ticket is one-shot and single-use by construction: the key is cleared on every path, including the failure path. The bypass therefore cannot be replayed. Its security rests entirely on the secrecy of `TempKey`, which is generated and distributed by the transfer tooling outside this component; the login handler forwards `Zero[52]` verbatim (BR-09) and cannot distinguish a legitimate transfer packet from a crafted one. Because the field is 52 bytes and the comparison length is `sizeof(file.TempKey)`, the effective secret length is whatever the writer chose — not documented in the analyzed code, and recorded here as an ambiguity.

When `ChangeServer == 1` the success path additionally re-saves the account, sets `SecurePass = 1` (thereby marking the session as having satisfied the numeric-token step), parses server/slot/encryption data out of the same `Zero` string with `sscanf("*%d %d ...")`, and pushes a `_MSG_DBCNFCharacterLogin` for the target slot (`Code/DBSrv/CFileDB.cpp:802-853`).

**Rule workflow**:
```
if file.TempKey[0] != 0 and m->Zero[0] != 0:
    if memcmp(file.TempKey, m->Zero, sizeof(TempKey)) == 0:
        clear TempKey; ChangeServer = 1; goto success   [password check skipped]
    else:
        clear TempKey; DBWriteAccount(file); return     [silent, no signal to TMSrv]
```

---

### Business Rule: BR-15 — Plaintext, case-sensitive password comparison (DBSrv)

**Overview**:
`strcmp(file.Info.AccountPass, m->AccountPassword)` decides authentication. On mismatch DBSrv signals `_MSG_DBAccountLoginFail_Pass`; TMSrv shows "wrong password" and closes the connection.

**Detailed description**:

`AccountPass` is `char[12]` (`Code/Basedef.h:783`, `ACCOUNTPASS_LENGTH = 12` at `Code/Basedef.h:66`) and the wire field `AccountPassword` is the same width (`Code/Basedef.h:1217`). Both `DBReadAccount` and `DBWriteAccount` force the last two bytes to NUL (`Code/DBSrv/CFileDB.cpp:2522-2523`), which caps the usable secret at 10 characters and guarantees a terminator for `strcmp`. There is no hashing, salting, stretching or encryption anywhere on this path: the client transmits cleartext (obfuscated per-byte by the `pKeyWord` stream cipher, which is a transport encoding rather than a cryptographic protection), TMSrv relays it untouched, and DBSrv compares it with `strcmp`.

The comparison is case-sensitive because neither side is case-folded — only the account *name* passes through `_strupr`. It is also the only authentication test in the whole login path unless a numeric token has been configured, in which case the token exchange of BR-18 supplements it. Because the failure is answered with a distinct message and a socket close, and because BR-08's counter is inert, an attacker faces no server-side attempt limit on this comparison other than the cost of a reconnect.

The comparison is the last step before the success block that registers the account in `pAccountList`, records the MAC, emits `_MSG_DBCNFAccountLogin`, and optionally broadcasts `_MSG_DBCheckPrimaryAccount` when the MAC is shared by more than one account (`Code/DBSrv/CFileDB.cpp:745-801`).

**Rule workflow**:
```
if strcmp(AccountPass, AccountPassword) != 0:
    send _MSG_DBAccountLoginFail_Pass
    return
-> TMSrv: "wrong password" -> flush -> CloseUser
(no attempt counter is incremented anywhere on this path)
```

---

### Business Rule: BR-16 — Duplicate-session resolution driven by client DBNeedSave (DBSrv)

**Overview**:
If the account is already registered in DBSrv's account list on a different TMSrv session, the outcome depends on the client-supplied `DBNeedSave`: `0` yields `_MSG_DBAlreadyPlaying`, any other value yields `_MSG_DBStillPlaying` plus a saving-quit to the previous session.

**Detailed description**:

Session identity on DBSrv is `GetIndex(conn, m->ID) = conn * MAX_USER + m->ID` (`Code/DBSrv/CFileDB.cpp:627,2328-2333`), and account identity is the linear scan in `GetIndex(char*)` over `pAccountList[MAX_DBACCOUNT]` where `MAX_DBACCOUNT = MAX_USER * MAX_SERVER` (`Code/DBSrv/CFileDB.cpp:2335-2349`, `Code/Basedef.h:60`). When the same account resolves to a different index, DBSrv logs `"err, desconectado. conexão anterior finalizada."` and branches on `m->DBNeedSave` (`Code/DBSrv/CFileDB.cpp:685-703`). With `DBNeedSave == 0` the newcomer is refused with `_MSG_DBAlreadyPlaying`; with any non-zero value DBSrv answers `_MSG_DBStillPlaying` and calls `SendDBSavingQuit(IdxName, 0)` to force the incumbent session to save and quit, allowing the newcomer to take over.

The important property is that `DBNeedSave` is never written by TMSrv — it arrives from the client and is forwarded untouched (BR-09). The handler has no way to distinguish "I am resuming my own session after a crash" from "I want to evict the current player". Whatever value the client sends decides whether the incumbent is evicted. This is documented as an observation of the implemented logic; the rule is faithfully implemented with respect to its own contract, and the trust boundary is simply placed in the client.

TMSrv's side of both signals is a client signal plus `CloseUser`: `_MSG_DBAlreadyPlaying` emits `_MSG_AlreadyPlaying` with `ID = ESCENE_FIELD + 2`, flushes, and closes the session (`Code/TMSrv/ProcessDBMessage.cpp:1111-1121`); `_MSG_DBStillPlaying` does the same with `_MSG_StillPlaying` (`:1124-1134`). Note that both use `m->ID` for the signal target but `conn` for the close — values that are equal under the dispatcher's assignment (`conn = std->ID`, `:54`). When the incumbent is evicted, its own `_MSG_DBCNFAccountLogOut` path restores `USER_ACCEPT` before closing (`Code/TMSrv/ProcessDBMessage.cpp:1172-1183`).

**Rule workflow**:
```
Idx = GetIndex(conn, m->ID); IdxName = GetIndex(m->AccountName)
[after password/ticket success]
if IdxName == Idx: proceed
if IdxName != 0:
    if m->DBNeedSave == 0: send _MSG_DBAlreadyPlaying; return
    send _MSG_DBStillPlaying; SendDBSavingQuit(IdxName, 0); break
```

---

### Business Rule: BR-17 — Fail-counter decay cycle

**Overview**:
`FailAccount[16][16]` is zeroed at start-up and every time `MinCounter % 10 == 0`, i.e. every ten minutes.

**Detailed description**:

The reset is part of the minute timer, not a dedicated janitor (`Code/TMSrv/ProcessSecMinTimer.cpp:2136-2137`), and it is unconditional — it does not consult whether any entry is in use. Its intent is to give the BR-08 budget a sliding window: three failures inside ten minutes would block, three spread over an hour would not. Since the insert side is missing, the reset currently has no observable effect beyond keeping the array zeroed.

A second, one-shot initialisation exists at `Code/TMSrv/Server.cpp:3623`, executed in `InitInstance` after the billing connection is established and before the listen socket starts. That ordering guarantees the array is defined before the first client can be accepted. No persistence exists: a server restart always clears the (theoretical) counts, so an attacker who can induce a restart resets the budget — an inherent property of an in-memory counter.

**Rule workflow**:
```
start-up:      memset(FailAccount, 0, sizeof(FailAccount))
each minute:   if (MinCounter % 10) == 0: memset(FailAccount, 0, sizeof(FailAccount))
```

---

### Business Rule: BR-18 — Numeric-token relay (`_MSG_AccountSecure`)

**Overview**:
`Exec_MSG_AccountSecure` rejects `conn` outside `[1, MAX_USER)`, overwrites the packet `ID` with `conn`, and forwards the packet to DBSrv with a fixed 116-byte-independent size (`sizeof(MSG_AccountSecure)`).

**Detailed description**:

The related handler is deliberately minimal: three effective statements plus a guard (`Code/TMSrv/_MSG_AccountSecure.cpp:21-31`). It performs no size validation, no state check, and no rate limiting, unlike the login handler. `MSG_AccountSecure` is 32 bytes (`_MSG` 12 + `NumericToken[6]` + `Unknown[10]` + `int ChangeNumeric` = 12 + 6 + 10 + 4, with default 8-byte alignment after `#pragma pack(pop)` at `Code/Basedef.h:1246`, `:1250-1257`), and the fixed-size send means a client that sent a shorter frame is still forwarded as 32 bytes — the tail being whatever remained in the receive buffer. Because `ReadMessage` compacts the buffer on each read, that residue is previous packet content rather than uninitialized heap, but the packet is not zero-filled before forwarding.

`_MSG_AccountSecure` is flagged for all four directions simultaneously (`Code/Basedef.h:1248`), so the same numeric type identifies the client→server request, the server→DB request, and both replies. Direction is distinguished only by which socket carries it and by `FLAG` membership checks at the dispatcher. The handler's `MAX_USER` guard uses `<` rather than the login handler's `< MAX_USER - ADMIN_RESERV`, so an administrative slot can relay a token packet even though it cannot log in — an inconsistency between the two handlers.

DBSrv's state machine (`Code/DBSrv/CFileDB.cpp:1357-1421`) treats `SecurePass == -1` as "not yet verified": `ChangeNumeric` is refused unless the session was already verified; an unset token (`NumericToken[0] == -1`) is created from the client's value; a matching token sets `SecurePass = 1`; a matching change request overwrites the token; anything else sets `SecurePass = -1` and returns `_MSG_AccountSecureFail`. TMSrv relays both outcomes to the client as signals (`Code/TMSrv/ProcessDBMessage.cpp:540-551`).

**Rule workflow**:
```
if conn <= 0 or conn >= MAX_USER: return silently
m->ID = conn
DBServerSocket.SendOneMessage(m, sizeof(MSG_AccountSecure))
```

---

### Business Rule: BR-19 — Account-name echo validation on the DB reply

**Overview**:
When `_MSG_DBCNFAccountLogin` arrives, TMSrv requires `strcmp(m->AccountName, pUser[conn].AccountName) == 0`; otherwise it shows "try to reconnect" and closes the socket.

**Detailed description**:

The reply carries the account name back (`Code/Basedef.h:1107`, populated at `Code/DBSrv/CFileDB.cpp:773`). TMSrv compares it against the name it stored during normalization (BR-07). The purpose is correlation: it proves the reply corresponds to the request currently outstanding on that session slot, which matters because DBSrv multiplexes ten game servers onto one account list and because a slot can be recycled quickly after a disconnect. A mismatch means the session has been re-used or the reply mis-routed, and the correct response is to destroy the session rather than adopt whatever account the reply describes.

The failure path sends `_NN_Try_Reconnect` (index 130) and calls `CloseUser`, so the slot immediately returns to `USER_EMPTY` (`Code/TMSrv/ProcessDBMessage.cpp:358-367`). On success the handler proceeds through the MAC block check (BR-20), resets per-session flags (`IsBillConnect`, `Whisper`, `Guildchat`, `PartyChat`, `KingChat`, `Admin`, `Chatting`, billing timers), writes `USER_SELCHAR`, and forwards `MSG_DBCNFAccountLogin` to the client with the cargo, coin and selection data (`Code/TMSrv/ProcessDBMessage.cpp:386-440`).

**Rule workflow**:
```
if strcmp(m->AccountName, pUser[conn].AccountName) != 0:
    send "try reconnect" -> flush -> CloseUser -> return
```

---

### Business Rule: BR-20 — MAC block list enforcement on the DB reply

**Overview**:
After a successful DB reply, the captured MAC (`pUser[conn].Mac[0..3]`) is compared against `pMac[MAX_MAC]` (200 entries) loaded from `../../DBSRV/Run/Mac.txt`; a match rejects the login with `_NN_MAC_Block` and closes the socket.

**Detailed description**:

The list is read once at start-up by `CReadFiles::ReadMacblock` (`Code/TMSrv/CReadFiles.cpp:678-715`), which parses lines of the form `<index> <a> <b> <c> <d>` with dots replaced by spaces, discards indices outside `[0, 200)`, and stores the four octets into `pMac[idx].Mac[]`. Empty entries are those whose four integers are all zero, and the comparison loop skips them explicitly (`Code/TMSrv/ProcessDBMessage.cpp:371-372`) before comparing all four integers against the session's MAC (`:374-382`). The block therefore only applies to clients that supplied a non-zero `AdapterName`; a client sending `0 0 0 0` matches nothing and cannot be blocked by this mechanism.

Enforcement happens *after* DBSrv has already authenticated the password and registered the account in `pAccountList` (`Code/DBSrv/CFileDB.cpp:745-756`). TMSrv then closes the client socket without sending any DB signal to release that registration, so the account remains marked as logged-in on DBSrv until some other path (idle timeout, saving-quit broadcast, reconnect negotiation per BR-16) clears it. The observable consequence of a MAC-blocked login is therefore a client that is disconnected while the account stays "occupied".

The check runs once per successful login and iterates at most 200 entries with early skip on empty rows, so its cost is negligible. Note the parallel `NULL`-check discrepancy: entries are considered empty when all four integers are zero, so a real adapter whose four dwords are all zero is indistinguishable from an unused row and can never be blocked.

**Rule workflow**:
```
for x in 0..199:
    if pMac[x] is all-zero: continue
    if pMac[x].Mac[0..3] == pUser[conn].Mac[0..3]:
        send "MAC block" -> flush -> CloseUser -> break
```

---

## 4. Component Structure

```
Code/
├── Basedef.h                              # Global protocol catalogue and layout definitions
│   ├── APP_VERSION 6975                   # :42   build gate value
│   ├── MAX_USER 1000                      # :56
│   ├── ADMIN_RESERV 10                    # :57
│   ├── ACCOUNTNAME_LENGTH 16              # :65
│   ├── ACCOUNTPASS_LENGTH 12              # :66
│   ├── NAME_LENGTH 16                     # :132
│   ├── MAX_STRING 2000                    # :150
│   ├── MAX_MAC 200                        # :152
│   ├── SKIPCHECKTICK                      # :172  dispatcher tick sentinel
│   ├── _MSG macro (12-byte header)        # :925-930
│   ├── _MSG_DBAccountLogin = 3|GAME2DB    # :978
│   ├── MSG_DBCNFAccountLogin = 22|DB2GAME # :1094-1109
│   ├── MSG_AccountLogin = 13|CLIENT2GAME  # :1210-1228 (pack(1), 116 bytes)
│   ├── MSG_AccountLogin_HWID              # :1229-1245 (declared, never used)
│   ├── MSG_AccountSecure = 222            # :1248-1257
│   └── g_pMessageStringTable[MAX_STRING][128]  # :2461
├── Basedef.cpp
│   ├── BASE_InitializeMessage("Language.txt")  # :4637-4710  message table loader
│   └── BASE_CheckPacket                        # :6475-6596  entire body commented out; returns FALSE
├── CPSock.h                                # CPSock class, HEADER, INITCODE, buffer sizes
├── CPSock.cpp
│   ├── ReadMessage                             # :353-466  de-obfuscation + checksum
│   ├── AddMessage                              # :513-591  framing + encryption
│   └── SendOneMessage / SendMessageA           # :617-693
├── TMSrv/
│   ├── _MSG_AccountLogin.cpp                   # THIS COMPONENT (96 lines)
│   ├── _MSG_AccountSecure.cpp                  # related relay (31 lines)
│   ├── ProcessClientMessage.cpp                # :38-313  client dispatcher (caller)
│   ├── ProcessClientMessage.h                  # :77,82  handlers declared
│   ├── ProcessDBMessage.cpp                    # :354-576 login reply handling
│   ├── CUser.h / CUser.cpp                     # session object, modes, AcceptUser/CloseUser
│   ├── Server.cpp                              # FailAccount, AddFailAccount, CheckFailAccount,
│   │                                           # CrackLog, GetEmptyUser, CloseUser, socket loop
│   ├── Server.h                                # :65 pMac, :76-77 fail-account API, :224 DBServerSocket
│   ├── ProcessSecMinTimer.cpp                  # :2136-2137  FailAccount decay
│   ├── CReadFiles.cpp/.h                       # :40 MAC_BLOCK_PATH, :678-715 ReadMacblock
│   ├── Language.h                              # message index constants (_NN_*)
│   └── TMSrv.vcxproj                           # :61 Debug _PACKET_DEBUG, :79 Release NDEBUG
└── DBSrv/
    ├── CFileDB.cpp                             # :611-854 account login authority
    │                                           # :1357-1421 numeric token state machine
    │                                           # :2328-2349 index resolution
    │                                           # :2518+     DBReadAccount file I/O
    └── DBSrv.vcxproj                           # :61 Debug _PACKET_DEBUG, :83 Release NDEBUG
```

### Internal organization of the analyzed file

| Lines | Concern | Notes |
|---|---|---|
| 1-18 | GPL header | Decompilation provenance comment |
| 19 | Include | Single include: `ProcessClientMessage.h`, which transitively provides `Basedef.h`, `CPSock.h`, `CUser.h`, `Server.h`, `CMob.h`, `SendFunc.h` |
| 21-23 | Signature / cast | `void Exec_MSG_AccountLogin(int conn, char *pMsg)`; unsafe C cast to `MSG_AccountLogin*` |
| 25-26 | Locals | `Size` copy; `ClientVersion` local always equals `APP_VERSION` — a constant compared to itself when used |
| 28 | Pre-clear | Defensive NUL before validation |
| 30-39 | Gate 1 (slot) | Reject + close |
| 41-54 | Gates 2-3 (size, version) | Compile-time split via `_PACKET_DEBUG` |
| 56-63 | Gate 4 (state) | Reject without close |
| 65 | Dead local | `int tMac = 0;` never read |
| 67-70 | MAC capture | Short-packet sentinel vs full copy |
| 73-74 | Protocol translation | Type/ID rewrite |
| 76-80 | Normalization | Tokenize, upper, bounded write-back |
| 82-90 | Gate 5 (counter) | Effective only if `AddFailAccount` is ever wired |
| 92 | Egress | Fixed-size DB forward |
| 94-95 | State mutation | `USER_LOGIN` + `MOB_EMPTY` |

### Files that participate in the same business transaction but are outside this component

| File | Role in the login transaction |
|---|---|
| `Code/TMSrv/ProcessClientMessage.cpp` | Dispatch and pre-dispatch validation; supplies `conn` |
| `Code/TMSrv/ProcessDBMessage.cpp` | All five outcome handlers for the login reply |
| `Code/TMSrv/Server.cpp` | `FailAccount` storage, `CrackLog`, `CloseUser`, listening socket, slot allocation |
| `Code/DBSrv/CFileDB.cpp` | The authoritative decision engine for BR-11..BR-16 |
| `Code/CPSock.cpp` | Framing, obfuscation, buffers, checksum |
| `Code/Basedef.h/.cpp` | Wire layouts, limits, message text table |

---

## 5. Dependency Analysis

### Internal dependency chains

```
ProcessClientMessage (Code/TMSrv/ProcessClientMessage.cpp:68)
  └── Exec_MSG_AccountLogin (_MSG_AccountLogin.cpp:21)
        ├── pUser[conn]            (CUser, Code/TMSrv/CUser.h:39)
        │     ├── AccountName[16]  (:42)
        │     ├── Mode             (:45)
        │     ├── Mac[4]           (:93)
        │     └── cSock            (:47 -> CPSock)
        ├── pMob[conn].Mode        (CMob, Code/TMSrv/CMob.h)
        ├── g_pMessageStringTable  (Code/Basedef.cpp:742)
        ├── temp[4096]             (Code/TMSrv/Server.cpp:277)
        ├── SendClientMessage      (Code/TMSrv/SendFunc.cpp:27)
        │     └── CPSock::AddMessage (Code/CPSock.cpp:513)
        ├── CloseUser              (Code/TMSrv/Server.cpp:6834)
        │     └── CUser::CloseUser (Code/TMSrv/CUser.cpp:81)
        ├── CrackLog               (Code/TMSrv/Server.cpp:6671)
        │     └── Log               (Code/TMSrv/Server.cpp:6701)
        ├── CheckFailAccount       (Code/TMSrv/Server.cpp:1347)
        │     └── FailAccount[16][16] (Code/TMSrv/Server.cpp:369)
        └── DBServerSocket         (Code/TMSrv/Server.cpp:262, declared Server.h:224)
              └── CPSock::SendOneMessage -> AddMessage -> SendMessageA

Exec_MSG_AccountSecure (_MSG_AccountSecure.cpp:21)
  ├── DBServerSocket.SendOneMessage
  └── MAX_USER bound

Return path (not part of this component, but completing the transaction)
ProcessDBMessage (Code/TMSrv/ProcessDBMessage.cpp:39)
  ├── _MSG_DBCNFAccountLogin -> pMac[] (Server.cpp:271), pUser, SendOneMessage
  ├── _MSG_DBAccountLoginFail_Account / _Fail_Block / _Fail_Disable / _Fail_Pass -> SendClientMessage + CloseUser
  └── _MSG_AccountSecure / _MSG_AccountSecureFail -> SendClientSignal
```

### External dependencies

| Dependency | Version / Form | Purpose in this component | Notes |
|---|---|---|---|
| Windows Sockets 2 (`ws2_32`) | Win32 API | Socket accept/recv/send for both client and DB links | Blocking-free async via `WSAAsyncSelect` |
| MSVC CRT | VS2015 toolset | `sscanf`, `strncpy`, `_strupr`, `memcpy`, `memset`, `strcmp`, `sprintf` | `_CRT_SECURE_NO_WARNINGS` set in both projects |
| Windows USER32 | Win32 API | `SetWindowTextA` via `CrackLog` | UI coupling from a protocol path |
| DBSrv process | TCP, port 7514 (`Code/TMSrv/Server.cpp:3558`) | Credential authority, account storage | Address from configuration; port also hardcoded at `Server.cpp:3562` |
| `Language.txt` | Runtime text file | Supplies indices 36, 37, 130, 131, 133, 161, 191, 522 used by this flow | Loaded by `BASE_InitializeMessage` (`Code/TMSrv/Server.cpp:7160`); if absent, `g_pMessageStringTable` is only `memset` to byte value 128 (`Basedef.cpp:4639`) and message text is garbage |
| `../../DBSRV/Run/Mac.txt` | Runtime text file | MAC block list consumed on the reply path | Path is compile-time constant `CReadFiles::MAC_BLOCK_PATH` |
| Account files `./account/<key>/<NAME>` | Binary `STRUCT_ACCOUNTFILE` | Credential + character storage (DBSrv side) | 16-byte name; password stored as cleartext bytes |

### Compile-time configuration dependencies

| Macro | Where defined | Effect on this component |
|---|---|---|
| `_PACKET_DEBUG` | `TMSrv.vcxproj:61` (Debug), absent at `:79` (Release) | Debug: version gate compiled out, extra packet diagnostics; Release: version gate enforced |
| `_CRT_SECURE_NO_WARNINGS` | both projects | Permits `strcpy`/`sprintf`/`sscanf` usage |
| `_DEBUG` / `NDEBUG` | both projects | Selects the above |

---

## 6. Afferent and Efferent Coupling

Metrics are given for the C functions examined (the natural "component" unit in this procedural C++ codebase) plus the shared data structures they touch.

| Component | Afferent (Ca) | Efferent (Ce) | Instability Ce/(Ca+Ce) | Criticality | Notes |
|---|---|---|---|---|---|
| `Exec_MSG_AccountLogin` | 1 | 14 | 0.93 | Medium-High | Sole caller: `ProcessClientMessage.cpp:69`. 14 distinct outbound dependencies (see below) |
| `Exec_MSG_AccountSecure` | 1 | 5 | 0.83 | Low-Medium | Sole caller: `ProcessClientMessage.cpp:89`; near-pure relay |
| `CheckFailAccount` / `AddFailAccount` | 1 / 0 | 1 / 1 | n.a. | Low (inert) | `CheckFailAccount` called only from the login handler; `AddFailAccount` is uncalled |
| `CUser` | > 40 call sites (not exhaustively enumerated) | 5 (`CPSock`, `Basedef` types, modes) | low | High | Shared session state; every handler reads/writes it |
| `CPSock` | > 80 call sites (not exhaustively enumerated) | 3 (winsock, `pKeyWord`, `Log`) | low | High | Transport chokepoint for client, DB and billing links |
| `ProcessDBMessage` login cases | 0 (DB socket is external trigger) | 8 (`pUser`, `pMac`, `pMob`, `SendClientMessage`, `CloseUser`, `SendClientSignal`, `Log`, `temp`) | 1.0 | High | Only path that can move a session out of `USER_LOGIN` |
| `CFileDB::ProcessMessage` login case (DBSrv) | 1 (DBSrv socket dispatcher) | > 12 (`DBReadAccount`, `DBWriteAccount`, `GetIndex`, `GetAccountsByMac`, `AddAccountList`, `DBGetSelChar`, `Log`, `rankingSystem`, ...) | 0.92 | Critical | Single authentication authority for all shards |

### Efferent dependency inventory of `Exec_MSG_AccountLogin` (14)

| # | Dependency | Kind | Line |
|---|---|---|---|
| 1 | `MSG_AccountLogin` layout | type | 23 |
| 2 | `_MSG_DBAccountLogin` constant | constant | 73 |
| 3 | `APP_VERSION` | constant | 26, 47 |
| 4 | `MAX_USER`, `ADMIN_RESERV`, `NAME_LENGTH` | constants | 28, 30, 80 |
| 5 | `pUser` (`CUser`) | global object array | 28-95 |
| 6 | `pMob` (`CMob::Mode`) | global object array | 95 |
| 7 | `CPSock::SendMessageA` | method | 35, 50, 61, 88 |
| 8 | `SendClientMessage` | function | 33, 48, 58, 86 |
| 9 | `CloseUser` | function | 37, 52 |
| 10 | `CrackLog` | function | 60 |
| 11 | `CheckFailAccount` | function | 82 |
| 12 | `DBServerSocket` (`CPSock::SendOneMessage`) | global object | 92 |
| 13 | `g_pMessageStringTable` | global table | 32, 47, 86 |
| 14 | `temp` | global buffer | 32, 47 |

This matches the parent-supplied afferent/efferent profile (Ca 1, Ce 14).

### Cohesion and boundary assessment

* **Functional cohesion is high within the handler**: every statement serves the single purpose "admit or relay a login attempt". The one exception is the unused `tMac` local (`:65`), a decompilation residue.
* **Temporal cohesion with the reply path**: the component sets `USER_LOGIN` (BR-10) but does not own the transition out of it; the state machine is split across two files in two different message directions. This is a boundary that must be respected when reasoning about session invariants.
* **Control coupling with DBSrv via three client-controlled fields** (`ClientVersion` for the gate, `DBNeedSave` and `Zero[52]` for downstream policy). Only the first is validated locally.
* **Shared mutable state without synchronisation**: `pUser`, `pMob`, `temp`, `FailAccount`, `pMac` are process-wide globals mutated from the Win32 message loop. The server is single-threaded by design (`WSAAsyncSelect` on one `HWND`), so there are no data races, but there is no protection against re-entrancy or against one slot's handler overwriting `temp` used by another (`temp` is used for message formatting and re-filled immediately before each send, so the window is small but non-zero if a send were ever deferred).
* **Data coupling into the DB channel is structural, not interface-based**: TMSrv and DBSrv communicate by casting a shared byte buffer. There is no IDL, no schema version, and no runtime layout check — the only guard is a compile-time shared header and the (disabled) size table.

---

## 7. Endpoints

The component is not an HTTP/gRPC service; it exposes message-protocol endpoints on the TMSrv client socket and consumes/produces message-protocol endpoints on the TMSrv↔DBSrv socket.

### Inbound — client socket (TMSrv, `WSA_READ`)

| Message endpoint | Code | Direction flags | Size | Description |
|---|---|---|---|---|
| `_MSG_AccountLogin` | 13 | `FLAG_CLIENT2GAME` | 116 bytes (`pack(1)`) | First credential submission; triggers this handler |
| `_MSG_AccountSecure` | 222 | `CLIENT2GAME \| GAME2CLIENT \| GAME2DB \| DB2GAME` | 32 bytes | Numeric-token create/verify/change; relayed by the related handler |

### Outbound — DB socket (TMSrv → DBSrv, port 7514)

| Message endpoint | Code | Direction flags | Size | Description |
|---|---|---|---|---|
| `_MSG_DBAccountLogin` | 3 | `FLAG_GAME2DB` | 116 bytes | Credentials + MAC for authoritative verification |
| `_MSG_AccountSecure` | 222 | `FLAG_GAME2DB` | 32 bytes | Token verification request |

### Inbound — DB socket (DBSrv → TMSrv), consumed by the reply handlers that complete this transaction

| Message endpoint | Code | Direction flags | Description |
|---|---|---|---|
| `_MSG_DBCNFAccountLogin` | 22 | `FLAG_DB2GAME` | Login accepted; carries cargo, coin, `STRUCT_SELCHAR`, account name |
| `_MSG_DBAccountLoginFail_Account` | 33 | `FLAG_DB2GAME` | No such account (or reserved device name) |
| `_MSG_DBAccountLoginFail_Pass` | 34 | `FLAG_DB2GAME` | Wrong password |
| `_MSG_DBAccountLoginFail_Block` | 36 | `FLAG_DB2GAME` | Account blocked by date |
| `_MSG_DBAccountLoginFail_Disable` | 37 | `FLAG_DB2GAME` | Disabled account (handler exists at `ProcessDBMessage.cpp:527`; **never emitted by DBSrv in this build**) |
| `_MSG_DBOnlyOncePerDay` | 38 | `FLAG_DB2GAME` | Restricted account (handler exists at `ProcessDBMessage.cpp:553`; **never emitted**) |
| `_MSG_DBAlreadyPlaying` | 31 | `FLAG_DB2GAME` | Account in use, `DBNeedSave == 0` |
| `_MSG_DBStillPlaying` | 32 | `FLAG_DB2GAME` | Account in use, incumbent being saved and evicted |
| `_MSG_AccountSecure` / `_MSG_AccountSecureFail` | 222 / 223 | `FLAG_DB2GAME` | Token accepted / rejected |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| Game client → TMSrv | External client | Submit credentials, adapter fingerprint, build number | TCP, custom binary framing (`INITCODE` + per-byte obfuscation + checksum) | `MSG_AccountLogin` (116 B, `pack(1)`) | Size gate + `CloseUser`; a declared frame `Size` below `sizeof(HEADER)` or above `MAX_MESSAGE_SIZE` (8192) resets the receive buffer (`CPSock.cpp:397-406`), while an incomplete frame simply waits (`:386-411`); checksum mismatch does **not** drop the packet (`CPSock.cpp:455-466`) |
| TMSrv → DBSrv | Internal service | Authoritative credential verification; account/MAC registration | TCP, same framing, port 7514, auto-reconnect loop | `MSG_AccountLogin` retyped to `_MSG_DBAccountLogin` (116 B) | Send failure is ignored by the handler; `CPSock` logs `"err,add buffer invalid/full"`; `Server.cpp:3819-3865` re-connects the DB socket on failure |
| DBSrv → TMSrv | Internal service | Login outcome, session data | TCP, same framing | `MSG_DBCNFAccountLogin` (22) or one of the fail signals (31-38) | Each case formats a client message and calls `CloseUser` (success path proceeds to `USER_SELCHAR`) |
| DBSrv account store | Filesystem | Read/persist `STRUCT_ACCOUNTFILE` (credentials, chars, block date, numeric token, TempKey) | File I/O on `./account/<key>/<NAME>` | Binary struct, no encryption | `DBReadAccount` returns 0 on open failure; NUL-forced name/password tails; device-name guard repeated |
| `Language.txt` | Configuration file | All client-visible strings used by this flow | Text, `index<tab>text` | `char[MAX_STRING][128]` | Missing file leaves table uninitialised (only a bad `memset` of 2000 bytes to value 128 at `Basedef.cpp:4639`), so messages degrade to garbage rather than failing loudly |
| `../../DBSRV/Run/Mac.txt` | Configuration file | Operator MAC block list | Text, `index a b c d` | `STRUCT_BLOCKMAC pMac[200]` | Missing file → empty list, no blocking; malformed lines skipped by range check |
| `localip.txt` | Configuration file | Local bind address | Text, single token | string | Missing → fallback path in `Server.cpp:3511-3520` |
| Win32 window procedure | Platform | Drives all socket I/O and timers | `WSAAsyncSelect` messages on `hWndMain` | n.a. | `CloseUser` on `FD_CLOSE` and on receive failures (`Server.cpp:3929-3968`) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / assessment |
|---|---|---|---|
| Distributed gatekeeper / authority separation | TMSrv validates admissibility; DBSrv owns credentials | `_MSG_AccountLogin.cpp` vs `DBSrv/CFileDB.cpp:611-854` | Clean trust split: the public-facing server never touches account files |
| Message dispatcher (command switch) | `switch(std->Type)` over ~60 cases | `ProcessClientMessage.cpp:66-311` | One function per message; the login handler is one arm |
| Handler/Executor naming convention | `Exec_MSG_*` free functions, one file each | `Code/TMSrv/_MSG_*.cpp` (60+ files) | Uniform structure, no class hierarchy, no interfaces |
| In-place packet mutation / re-addressing | Client buffer retyped and forwarded | `_MSG_AccountLogin.cpp:73-74` | Zero-copy relay; couples both servers to one binary layout |
| Defensive sentinel state | `0xFF` MAC fill; `USER_EMPTY`/`MOB_EMPTY`; all-zero MAC entries treated as empty | `:68`, `ProcessDBMessage.cpp:371-372` | Simple and dependency-free; sentinel collides with theoretically valid data |
| State machine (implicit) | Integer `Mode` field with `#define` constants | `CUser.h:26-37` | No transition table, no guard object; validity enforced ad hoc by each handler |
| Ring-buffer attempt counter | Fixed 16x16 array scanned linearly | `Server.cpp:1334-1361` | O(16) lookup; silent overflow when full; currently never populated |
| Layered transport with obfuscation | `AddMessage`/`ReadMessage` symmetric per-byte transform + weak checksum | `CPSock.cpp:353-591` | Anti-tamper for casual packet editing, not cryptographic protection; the same transform is used for the trusted DB link |
| Configuration-table indirection for user text | `g_pMessageStringTable[index]` | `Basedef.cpp:4637-4710`, `Language.h` | Localisation without recompilation; index constants shared with DBSrv |
| Shared-header binary protocol | One header compiled into all three binaries | `Basedef.h` | Zero-cost, zero-validation; version skew produces silent misinterpretation |
| Fail-fast (partial) | Slot/size/version failures close the socket immediately | `:37, :52` | Prevents resource retention; contrasts with the soft-fail state gate and counter gate |

### Architectural decisions evidenced in code

1. **TMSrv is stateless with respect to credentials.** It never persists or caches passwords; the only credential-adjacent data it retains is the normalized account name and the MAC.
2. **The public server performs all "cheap" rejections before touching the DB link.** Slot, framing and build number are checked with no I/O; only after those pass is a DB message produced. This bounds DB load under malformed-client floods.
3. **All replies to the client are text via `MSG_MessagePanel`.** Failures are not signalled with dedicated message types; the client sees a 96-byte text field (`MESSAGE_LENGTH = 96`, `Basedef.h:129`). The one exception is the success path, which sends the full `MSG_DBCNFAccountLogin` structure.
4. **Logging is a side effect of validation.** `CrackLog` writes to the `-system` log and to the main window title, mixing diagnostics with UI.
5. **Layout is frozen by ABI convention.** `#pragma pack(push,1)` around the login structures and fixed-width fields (`char[12]`, `char[16]`, `int`) make the struct a wire format; the struct cannot evolve without a coordinated client and DB release.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|---|---|---|---|
| High | BR-08 brute-force gate | `AddFailAccount` has no callers; `FailAccount` is only ever zeroed, so `check >= 3` is unreachable | No server-side password attempt limit; brute force is bounded only by reconnect cost |
| High | `_MSG_AccountLogin.cpp:44` vs `TMSrv.vcxproj:61` | Client-version gate compiled out whenever `_PACKET_DEBUG` is defined (both projects' Debug configs) | A Debug build silently accepts any client build; divergent behaviour between configurations that is not documented |
| High | BR-16 / `DBSrv/CFileDB.cpp:692-703` | Duplicate-session eviction decided by client-supplied `DBNeedSave`, which TMSrv never writes | A client can choose between "refused" and "evict the incumbent player" |
| High | BR-14 / `DBSrv/CFileDB.cpp:664-675` | Server-change bypass uses client-supplied `Zero[52]` compared to a stored `TempKey`; on match the password check is skipped | Authentication strength of a transfer depends entirely on out-of-band ticket secrecy, which is not enforced or documented here |
| Medium | BR-15 / `DBSrv/CFileDB.cpp:677` | Passwords stored and compared in cleartext with `strcmp` over a 12-byte field | Any account-file disclosure or DB-link capture yields reusable credentials |
| Medium | `_MSG_AccountSecure.cpp` | No size check, no `Mode` check, no rate limit; forwards a fixed 30 bytes from a possibly shorter packet | Unvalidated relay into the token state machine; token packets accepted on any session state |
| Medium | BR-20 / `ProcessDBMessage.cpp:369-383` | MAC block enforced after DBSrv has already registered the account; no `_MSG_DBNoNeedSave` is sent on rejection | Account can remain marked as logged-in on DBSrv while the client is disconnected |
| Medium | `ProcessDBMessage.cpp:507-517` | Block parameters 1/2/3 handled but only `0` is ever emitted; `_Fail_Disable` and `_DBOnlyOncePerDay` handlers have no producer | Dead reply paths; operational block categories cannot be expressed |
| Medium | `Basedef.cpp:6475-6596` | `BASE_CheckPacket` body entirely commented; function returns `FALSE` and is called at `CPSock.cpp:547`, `Server.cpp:3898,3983`, `DBSrv/Server.cpp:848,931` | The size table (which already mismatches `_Fail_Block` between send and expectation) provides no protection; only the handler-local size gate does |
| Medium | `_MSG_AccountLogin.cpp:67` | MAC sentinel branch is unreachable because the earlier size gate already rejected short packets | Divergent intent between two checks of the same field; misleading for maintainers |
| Medium | `_MSG_AccountLogin.cpp:76-80` | Silent truncation of account names longer than 15 characters | Distinct long names collide onto one identifier; no client feedback |
| Medium | `_MSG_AccountLogin.cpp:92` | Return value of `SendOneMessage` ignored; state set to `USER_LOGIN` regardless | Session can stall in `USER_LOGIN` when the DB link is down, until idle/timeout handling clears it |
| Medium | `CPSock.cpp:455-466` | Checksum mismatch returns the packet anyway (documented in-line) | Integrity check is advisory; the obfuscation layer is the only practical tamper barrier |
| Medium | `Basedef.cpp:4639` | `memset(g_pMessageStringTable, MAX_STRING, 128)` empties only 2000 bytes of a 256,000-byte array with the value 128 instead of zero | If `Language.txt` is missing, all login failure texts are garbage, not empty |
| Low | `_MSG_AccountLogin.cpp:65` | `int tMac = 0;` declared and never used | Decompilation residue; compiler warning (silenced by `_CRT_SECURE_NO_WARNINGS`/project settings) |
| Low | `_MSG_AccountLogin.cpp:26` | Local `ClientVersion` is a copy of `APP_VERSION` compared against the packet field | Indirection obscures that the check is a constant comparison |
| Low | `_MSG_AccountLogin.cpp:28` before `:30` | Array write precedes the index validation | Safe only because the dispatcher pre-validates `std->ID` |
| Low | `_MSG_AccountLogin.cpp:23` | Unchecked C-style cast of `char*` to `MSG_AccountLogin*` | Relies on `Size` gate for field validity; no alignment check (harmless: `pack(1)`, x86) |
| Low | `Basedef.h:1229-1245` | `MSG_AccountLogin_HWID` declared but never referenced | Dead protocol variant |
| Low | `_MSG_AccountSecure.cpp:25` | Bound uses `MAX_USER` instead of `MAX_USER - ADMIN_RESERV` | Inconsistent with the login handler's reservation |
| Low | `Server.cpp:1334-1345` | `AddFailAccount` silently drops when all 16 rows are occupied | Undocumented capacity limit; would matter if wired |
| Low | `Server.cpp:6671-6699` | `CrackLog` mutates `pUser[conn].AccountName[14..15]` and calls `SetWindowTextA` | Logging has state side effects and UI coupling |
| Low | `CPSock.cpp:513-591` | Same obfuscation used on the trusted DB link as on the public client link | Internal traffic gains no stronger protection |
| Low | `DBSrv/CFileDB.cpp:648-660` | Block-date comparison has a redundant disjunct and effectively ignores `YearDay` | Whole-year blocking regardless of the day recorded |
| Low | `DBSrv/CFileDB.cpp:611-854` | Mixed `return TRUE/FALSE/bare return` conventions in one case | Value is only tested for `0` by the caller; fragile if reused |

### Security observations (recorded, not remediated)

* **Transport confidentiality.** Both links use a fixed substitution table (`pKeyWord`) with a random one-byte keyword per message, and a checksum that is not enforced. Credentials are recoverable by anyone who can observe the client socket.
* **Account enumeration.** `_MSG_DBAccountLoginFail_Account` ("no account with that name") is distinguishable from `_MSG_DBAccountLoginFail_Pass` ("wrong password").
* **No attempt accounting on the failure path.** `_MSG_DBAccountLoginFail_Pass` (`ProcessDBMessage.cpp:564-576`) does not increment `NumError` (via `AddCrackError`) nor `FailAccount`.
* **Client-controlled policy inputs.** `ClientVersion` (version gate), `DBNeedSave` (session eviction), `Zero[52]` (password bypass ticket), `AdapterName[4]` (MAC policy identifier and MAC block evasion via `0.0.0.0`).
* **Path construction from an unvalidated name (DBSrv).** `sprintf("./account/%s/%s", First, check)` (`CFileDB.cpp:2542`) uses the upper-cased, 16-byte-bounded account name; the device-name guard is the only traversal filter, and `GetAccountsByMac`/`GetIndex` comparisons rely on exact 16-byte matching.
* **Session-state gate is the only barrier against duplicate login on TMSrv**, and it is soft (connection retained, no counter).
* **`_MSG_AccountSecure` forwarding is unauthenticated** — a client can push arbitrary token packets from any mode, and DBSrv's `GetIndex(conn, m->ID)` will resolve to whatever account that session has registered, including slot 0 (`pAccountList[0]`) when no account is registered for the slot.

---

## 11. Test Coverage Analysis

### Result: no automated tests exist for this component — or for any component in this repository

Evidence gathered across the entire project root (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`):

| Evidence | Finding |
|---|---|
| Solution contents | `W2PP Code Project.sln` declares exactly three entries: `DBSrv` (`Code\DBSrv\DBSrv.vcxproj`), `TMSrv` (`Code\TMSrv\TMSrv.vcxproj`) and a `Solution Items` folder. No test project, no test target. |
| Directory names matching `test`/`spec`/`fixture`/`mock` | Exactly one match, `openspec`, which is an excluded folder and is not a test suite. |
| File names matching `test`/`spec`/`_test`/`assert` | Zero matches outside the excluded folders. |
| Assertion / mocking macros | No `assert`, `ASSERT`, `EXPECT_*`, `TEST(`, `catch2`, `gtest`, `CppUnit`, `cpputest` usage found in `Code/`. `#include <assert.h>` is not used by any login-path file. |
| Test doubles for DBSrv | None. `DBServerSocket` is a concrete global `CPSock` (`Server.cpp:262`); there is no seam, interface or injection point in `Exec_MSG_AccountLogin` to substitute it. |
| Fixtures / seed data | None for accounts. Runtime acquisition is manual (`Language.txt`, `Mac.txt`, `./account/...`). |

### Static traceability of the component against its rules

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `Exec_MSG_AccountLogin` | 0 | 0 | 0% (no instrumentation, no harness) | Not applicable — no test code exists |
| `Exec_MSG_AccountSecure` | 0 | 0 | 0% | Not applicable |
| `CheckFailAccount` / `AddFailAccount` | 0 | 0 | 0% | Not applicable; `AddFailAccount` is also uncalled in production code, so nothing exercises it |
| Reply handlers `_MSG_DBAccountLoginFail_*` | 0 | 0 | 0% | Not applicable |
| Downstream `CFileDB` login case | 0 | 0 | 0% | Not applicable |

### Branch-level inventory of untested decision points

Every branch of the handler is unexercised by automation. The following list is the set that a harness would need to cover, derived from the source:

| Branch | Line | Trigger | Expected observable |
|---|---|---|---|
| Slot out of range | `:30-39` | `conn == 0` or `conn >= 990` | `MSG_MessagePanel(191)`, socket closed, `Mode = USER_EMPTY` |
| Short packet (Debug build) | `:42` | `Size < 116` | `MSG_MessagePanel(36, 6975)`, socket closed |
| Wrong build (Release build) | `:44` | `ClientVersion != 6975` | same message, socket closed |
| Wrong state | `:56-63` | `Mode` in {`USER_EMPTY`,`USER_LOGIN`,`USER_SELCHAR`,...} | "Login now, wait a moment.", `CrackLog`, socket open, `Mode` unchanged |
| MAC sentinel | `:67-68` | short packet that passed earlier gates | `Mac[0..3] == 0xFFFFFFFF` |
| MAC copy | `:69-70` | full packet with `AdapterName` | `pUser[conn].Mac` equals sent values |
| Counter hit | `:84-90` | `CheckFailAccount(...) >= 3` | `MSG_MessagePanel(37)`, socket open, no DB send |
| Happy path | `:92-95` | all gates pass | 116-byte `_MSG_DBAccountLogin` on DB socket; `USER_LOGIN`; `MOB_EMPTY` |
| Account-name truncation | `:76-80` | 20-character name | 15 characters + NUL, uppercased, echoed in the DB packet |
| Secure relay guard | `_MSG_AccountSecure.cpp:25` | `conn <= 0` or `>= 1000` | silent return, nothing on DB socket |

Missing coverage is recorded here as a structural risk: the login handler is the only gate protecting the entire session state machine, and no regression signal exists for any of its six rejection paths.

---

## Appendix A — Constants and limits referenced

| Name | Value | Location | Used by this component for |
|---|---|---|---|
| `APP_VERSION` | 6975 | `Code/Basedef.h:42` | Version gate value and failure message parameter |
| `MAX_USER` | 1000 | `Code/Basedef.h:56` | Slot bound, `pUser`/`pMob` sizing |
| `ADMIN_RESERV` | 10 | `Code/Basedef.h:57` | Upper slot reservation (990 usable) |
| `ACCOUNTNAME_LENGTH` | 16 | `Code/Basedef.h:65` | Wire account name width |
| `ACCOUNTPASS_LENGTH` | 12 | `Code/Basedef.h:66` | Wire password width |
| `NAME_LENGTH` | 16 | `Code/Basedef.h:132` | Normalization cap and defensive NUL index |
| `MESSAGE_LENGTH` | 96 | `Code/Basedef.h:129` | Client text panel payload |
| `MAX_STRING` | 2000 | `Code/Basedef.h:150` | Message table size |
| `MAX_MAC` | 200 | `Code/Basedef.h:152` | MAC block list capacity |
| `MAX_MESSAGE_SIZE` | 8192 | `Code/CPSock.h:38` | Framing upper bound |
| `RECV_BUFFER_SIZE` / `SEND_BUFFER_SIZE` | 128 KB each | `Code/CPSock.h:35-36` | Socket buffering |
| `INITCODE` | `0x1F11F311` | `Code/CPSock.h:40` | Post-accept handshake preamble |
| `SKIPCHECKTICK` | 235543242 | `Code/Basedef.h:172` | Dispatcher bypass sentinel |
| `sizeof(MSG_AccountLogin)` | 116 | derived from `Code/Basedef.h:1213-1228` | Size gate and DB send length |
| `sizeof(MSG_AccountSecure)` | 32 | derived from `Code/Basedef.h:1250-1257` (outside the `pack(1)` region) | Secure relay length |
| `USER_ACCEPT` | 1 | `Code/TMSrv/CUser.h:27` | Required source state |
| `USER_LOGIN` | 2 | `Code/TMSrv/CUser.h:28` | Resulting state |
| Message indices | 36, 37, 130, 131, 133, 161, 191, 522 | `Code/TMSrv/Language.h:56-57,150-153,181,211,535` | Client-visible rejection texts |

## Appendix B — Ambiguities and confidence levels

| Statement | Confidence | Basis |
|---|---|---|
| `AddFailAccount` is never called, so BR-08 is unreachable | High | Exhaustive symbol search over `Code/`; only the declaration and definition exist |
| Version gate is active only in non-`_PACKET_DEBUG` builds | High | `#ifdef` in source plus explicit macro in Debug configurations of both project files |
| `DBNeedSave` and `Zero[52]` are client-controlled and unvalidated in TMSrv | High | No assignment to either field anywhere in `Code/TMSrv`; both forwarded verbatim |
| `MSG_AccountLogin_HWID` is unused | High | Declaration only; no references anywhere in `Code/` |
| `BASE_CheckPacket` is a no-op | High | Comment block spans the entire body; function returns `FALSE` |
| `_MSG_DBAccountLoginFail_Disable` and `_MSG_DBOnlyOncePerDay` are never emitted by DBSrv | High | No send site exists in `Code/DBSrv` |
| Password comparison length is effectively 10 characters | Medium | `ACCOUNTPASS_LENGTH = 12` with the final two bytes NUL-forced in `DBReadAccount`; writers elsewhere set arbitrary prefixes, so the exact stored width depends on the account-creation path (`DBSrv/Server.cpp:1840-1844` writes 4 characters plus NUL) |
| `TempKey` secret length | Unknown | The comparison uses `sizeof(file.TempKey)`, whose declared size was not located in the analyzed scope; the write path is outside this component |
| Relationship between `APP_VERSION = 6975` and the `ClientPatch_v7662` directory | Unknown | No source comment links the two numbering schemes |
| MAC sentinel `0xFF` branch reachability | High (unreachable) | Earlier size gate rejects the same condition that this branch tests |
| `pKeyWord` obfuscation strength | Not assessed | Table definition is outside the analyzed component; the transform is a byte-wise substitution, not a block cipher |
| Threading model | High (single-threaded UI loop) | All socket handling is driven by `WSAAsyncSelect` callbacks on one `HWND` (`Code/TMSrv/Server.cpp:3919-4045`) |

---

*End of report. This analysis was produced read-only; no project file, source file or configuration was created, modified or deleted by the analysis process.*
