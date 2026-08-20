# MSG_AccountSecure

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_AccountSecure` = `(222 \| FLAG_DB2GAME \| FLAG_GAME2DB \| FLAG_CLIENT2GAME \| FLAG_GAME2CLIENT)` = `0xFDE` |
| Sequence ID | 222 (0xDE) |
| Direction(s) | Client → TMSrv → DBSrv → TMSrv → Client (4-flag round trip: `FLAG_CLIENT2GAME` \| `FLAG_GAME2DB` \| `FLAG_DB2GAME` \| `FLAG_GAME2CLIENT`) |
| Wire struct | `MSG_AccountSecure` (dedicated struct) |
| Total size | 32 bytes (fixed) |
| Packing | Compiler default (/Zp8) — outside all pack(1) regions |
| Handler | `Exec_MSG_AccountSecure` @ `TMSrv/_MSG_AccountSecure.cpp:21` (client leg); DBSrv case @ `DBSrv/CFileDB.cpp:1357` |
| Aliases | `_MSG_AccountSecureFail` = `(223 \| same flags)` = `0xFDF` — sibling success/fail pair, same struct semantics |
| Related | `_MSG_AccountSecureFail` `0xFDF`; `SendDBSignal` (`MSG_STANDARD`) responses; gated by `SecurePass` on DBSrv |

## 2. Wire Framing (protocol preamble)

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with 4-byte `INITCODE = 0x1F11F311` magic before any framed message.
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating XOR transform keyed by `KeyWord` (index into `pKeyWord[512]`).
- `CheckSum` = `Sum2 - Sum1` (raw vs. transformed payload sums); validated on receive.
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` else buffer reset.
- `BASE_CheckPacket` (`Basedef.cpp:6475`) disabled — no central size validation.

Per-packet notes:
- **Same `Type` value travels every leg.** The client→game, game→DB, DB→game, and game→client legs all use `_MSG_AccountSecure` (0xFDE). Legs are distinguished by socket, not by a rewritten Type. The DBSrv reply is a **bare `MSG_STANDARD`** (Type=0xFDE) via `SendDBSignal`, while the request carries the full `MSG_AccountSecure` payload.
- The TMSrv forwards the request unmodified except `m->ID = conn` (`_MSG_AccountSecure.cpp:28`); the `Size` field is preserved from the client.

## 3. Binary Layout

Packing context: **/Zp8 (compiler default)**. `MSG_AccountSecure` (`Basedef.h:1250-1257`) sits immediately after the pack(1) region that ends at line 1246, so no pack pragma applies. No nested `STRUCT_*` members.

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 32) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_AccountSecure` = 0xFDE |
| 6 | 2 | `ID` | `short` | Connection slot (sender-stamped; TMSrv overwrites with `conn` before DB forward) |
| 8 | 4 | `ClientTick` | `unsigned int` | Client tick; must not equal `SKIPCHECKTICK` (dispatcher) |

### 3.2 Payload

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 6 | `NumericToken` | `char[6]` | 1 | 0 | 6-char numeric security token submitted by the client. Compared against the stored token on DBSrv (`strncmp(...,6)`). |
| 18 | 10 | `Unknown` | `char[10]` | 1 | 0 | **UNKNOWN** — never read anywhere in the codebase. |
| 28 | 4 | `ChangeNumeric` | `int` | 4 | 0 | Flag: 0 = verify existing token; 1 = change/overwrite token (only valid when already secure). |

### 3.3 Nested struct expansions

None — only primitive arrays and an `int`.

### 3.4 Size verification

Offset math (little-endian, /Zp8; char arrays align to 1, `int` aligns to 4):
```
Header (_MSG):                      0 + 12          = 12
NumericToken[6] (char, align 1)     12 + 6          = 18
Unknown[10]    (char, align 1)      18 + 10         = 28   (no pad: 18 % 1 == 0)
ChangeNumeric  (int, align 4)       28 + 4          = 32   (no pad: 28 % 4 == 0)
sizeof(MSG_AccountSecure)                                  = 32
```
- Expected `Size` header value: **32**.
- Cross-check: TMSrv forwards `sizeof(MSG_AccountSecure)` = 32 bytes to DBSrv (`_MSG_AccountSecure.cpp:30`). No other hardcoded size; no mismatch. Largest member alignment = 4, so struct size = 32 exactly.

## 4. Lifecycle & Flow

This is a **4-leg round-trip** account security-token packet.

```
[Client]                          [TMSrv]                                    [DBSrv]
   | MSG_AccountSecure (0xFDE, 32B) |                                            |
   |-------------------------------->|  ProcessClientMessage:88 ->                |
   |  ClientTick!=SKIPCHECKTICK      |  Exec_MSG_AccountSecure:21                 |
   |  conn in [0,MAX_USER)           |  set m->ID=conn (:28)                      |
   |                                 |  DBServerSocket.SendOneMessage(m,32) (:30) |
   |                                 |------------------------------------------->|
   |                                 |  CFileDB.cpp:1357 case _MSG_AccountSecure  |
   |                                 |  validate/change NumericToken, SecurePass  |
   |                                 |  reply via SendDBSignal (MSG_STANDARD)      |
   |                                 |<-------------------------------------------|
   |                                 |  0xFDE (success) or 0xFDF (fail)           |
   |                                 | ProcessDBMessage.cpp:541 / :547             |
   |                                 |  SendClientSignal(conn,ESCENE_FIELD,type)  |
   |  MSG_STANDARD (Type=0xFDE/0xFDF) |                                            |
   |<--------------------------------|
```

- **Client → TMSrv**: de-framed by `CPSock.ReadMessage` → `Server.cpp (WSA_READ)` → `ProcessClientMessage(conn, pMsg, FALSE)` → case `_MSG_AccountSecure` → `Exec_MSG_AccountSecure` (`ProcessClientMessage.cpp:88-89`).
- **TMSrv handler** (`_MSG_AccountSecure.cpp:21-31`): guards `conn <= 0 || conn >= MAX_USER` → return (silent drop). Otherwise sets `m->ID = conn` and forwards the full 32-byte buffer to DBSrv via `DBServerSocket.SendOneMessage` (`:30`). **No mode transition.**
- **TMSrv → DBSrv**: `_MSG_AccountSecure` on the DBServerSocket.
- **DBSrv processing** (`CFileDB.cpp:1357-1421`): resolves session `Idx = GetIndex(conn, m->ID)`, reads `Secure = pAccountList[Idx].SecurePass` and `Change = m->ChangeNumeric`, then applies the token-verify/change rules (§6). Replies via `SendDBSignal(conn, m->ID, _MSG_AccountSecure)` or `_MSG_AccountSecureFail` — a bare `MSG_STANDARD` with `Type` set and `ID = m->ID` (`CFileDB.cpp:2108-2120`).
- **DBSrv → TMSrv → Client**: `ProcessDBMessage.cpp:541-544` case `_MSG_AccountSecure` → `SendClientSignal(std->ID, ESCENE_FIELD, _MSG_AccountSecure)`; case `_MSG_AccountSecureFail` (`:547-550`) → `SendClientSignal(std->ID, ESCENE_FIELD, _MSG_AccountSecureFail)`. `SendClientSignal` builds a `MSG_STANDARD` (Type set, ID=`ESCENE_FIELD`=30000) and `AddMessage`s it to the client send buffer (`SendFunc.cpp:197-206`).

## 5. Validation & Guards

| # | Check | Condition | On failure | Location |
|---|---|---|---|---|
| 1 | Connection range (TMSrv) | `conn <= 0 \|\| conn >= MAX_USER` | Silent drop (return) | `_MSG_AccountSecure.cpp:25-27` |
| 2 | Dispatcher ID range | `std->ID < 0 \|\| std->ID >= MAX_USER` | Log + drop | `ProcessClientMessage.cpp:42-51` |
| 3 | Dispatcher anti-spoof | `ClientTick == SKIPCHECKTICK` | Drop silently | `ProcessClientMessage.cpp:63-64` |
| 4 | Change-without-secure (DBSrv) | `Change != 0 && SecurePass == -1` | Log `"err,secureverify change error 1"`, **no response** (`break`) | `CFileDB.cpp:1366-1371` |
| 5 | Token-not-set creation | `NumericToken[0] == -1` | Write token, `SecurePass=1`, reply success | `CFileDB.cpp:1373-1390` |
| 6 | Token verify | `Change == 0 && SecurePass == -1 && strncmp(stored, sent, 6) == 0` | `SecurePass=1`, reply success | `CFileDB.cpp:1392-1398` |
| 7 | Token change | `Change == 1 && SecurePass == 1` | Overwrite token, save, `SecurePass=1`, reply success | `CFileDB.cpp:1400-1417` |
| 8 | Fallthrough (any other combo) | reached | `SecurePass = -1`, reply **fail** (`_MSG_AccountSecureFail`) | `CFileDB.cpp:1419-1420` |

Notes:
- The "not set" sentinel is `NumericToken[0] == -1`, established at account creation (`CFileDB.cpp:119`) — note `-1` stored into a `char` array element.
- Token comparison is a 6-byte `strncmp` (`CFileDB.cpp:1392`).
- Failure mode #4 silently drops (no reply to the client), unlike #8 which explicitly sends `_MSG_AccountSecureFail`.
- `SecurePass` gates the whole flow: it is set to `-1` at login (`CFileDB.cpp:762`) and to `1` on the server-change fast path (`:806`); character login refuses while `SecurePass == -1` (`CFileDB.cpp:1043-1050`).

## 6. Game Mechanics & Business Logic

### Rule 1: Numeric security-token (account 2FA-style token) setup/verification/change
**Overview.** `MSG_AccountSecure` lets the client establish, verify, or change the 6-char `NumericToken` attached to the account. The per-session `SecurePass` flag on DBSrv records whether the current login has passed token verification; a failed/absent token blocks character login.
**Detailed description.** On the first ever login the token is unset (`NumericToken[0] == -1`), so any presented token is saved as the new token (Rule 1a). Thereafter the client must present the matching token to set `SecurePass = 1` (Rule 1b). With `ChangeNumeric = 1`, an already-secure session may replace the stored token (Rule 1c). Any other state combination fails (Rule 1d).
**Workflow:**
1. `Idx = GetIndex(conn, m->ID)`; `Secure = pAccountList[Idx].SecurePass`; `Change = m->ChangeNumeric` (`CFileDB.cpp:1361-1364`).
2. If `Change && Secure == -1`: invalid state → log `"err,secureverify change error 1"`, drop (`:1366-1371`).
3. If `NumericToken[0] == -1`: save `m->NumericToken` (6 bytes) into the account file, `DBWriteAccount`; on success `SecurePass = 1` and reply `_MSG_AccountSecure`; on write failure log `"err,save secure - create file"` and return (`:1373-1390`).
4. Else if `Change == 0 && Secure == -1 && strncmp(stored, m->NumericToken, 6) == 0`: `SecurePass = 1`, reply `_MSG_AccountSecure` (`:1392-1398`).
5. Else if `Change == 1 && Secure == 1`: overwrite stored token with `m->NumericToken`, `DBWriteAccount`, `SecurePass = 1`, reply `_MSG_AccountSecure` (`:1400-1417`).
6. Else: `SecurePass = -1`, reply `_MSG_AccountSecureFail` (`:1419-1420`).

### Rule 2: SecurePass gate on character login
**Overview.** The result of token verification is a precondition for entering a character.
**Workflow.** In `_MSG_DBCharacterLogin` (`CFileDB.cpp:1043-1050`), if `pAccountList[Idx].SecurePass == -1`, log `"err,charlogin secure illegal"` and drop the character-login request. A successful `MSG_AccountSecure` sets `SecurePass = 1` (`:1386,1394,1413`), unblocking it.

## 7. Side Effects

| Effect | Target | Mechanism | Location |
|---|---|---|---|
| DB forward `_MSG_AccountSecure` | DBSrv | `DBServerSocket.SendOneMessage(m, sizeof(MSG_AccountSecure))`, `m->ID = conn` | `_MSG_AccountSecure.cpp:28-30` |
| Drop (invalid conn) | — | return, no response | `_MSG_AccountSecure.cpp:26` |
| Token persisted | account file (`./account/…`) | `DBWriteAccount` after `strncpy(NumericToken, …)` | `CFileDB.cpp:1375-1377, 1402-1404` |
| `SecurePass` flag set to 1 | `pAccountList[Idx].SecurePass` | direct assignment | `CFileDB.cpp:1386, 1394, 1413` |
| `SecurePass` flag reset to -1 | `pAccountList[Idx].SecurePass` | direct assignment (fail) | `CFileDB.cpp:1419` |
| Success reply `_MSG_AccountSecure` | TMSrv → client | `SendDBSignal` (`MSG_STANDARD`, Type=0xFDE, ID=m->ID) → `SendClientSignal` | `CFileDB.cpp:1387,1395,1414`; `ProcessDBMessage.cpp:541-544` |
| Fail reply `_MSG_AccountSecureFail` | TMSrv → client | `SendDBSignal` (`MSG_STANDARD`, Type=0xFDF) → `SendClientSignal` | `CFileDB.cpp:1420`; `ProcessDBMessage.cpp:547-550` |
| Log `"err,secureverify change error 1"` | log | `Log(..., AccountName, 0)` | `CFileDB.cpp:1368` |
| Log `"err,save secure - create file"` | log | `Log(..., AccountName, 0)` | `CFileDB.cpp:1381, 1408` |

## 8. Related Packets

| Packet | Constant | Value | Direction | Role |
|---|---|---|---|---|
| `_MSG_AccountSecure` | `(222 \| 0xF00)` | `0xFDE` | all 4 legs | Request + success reply (round-trip) |
| `_MSG_AccountSecureFail` | `(223 \| 0xF00)` | `0xFDF` | all 4 legs | Failure reply |
| `_MSG_AccountLogin` | `(13 \| FLAG_CLIENT2GAME)` | `0x020D` | C→G | Precedes token flow; login establishes the session (`SecurePass=-1`) |
| `_MSG_CharacterLogin` | `(19 \| FLAG_CLIENT2GAME)` | `0x0213` | C→G | Gated by `SecurePass` on DBSrv (`CFileDB.cpp:1043`) |

## 9. Discrepancies & Open Questions

- **`Unknown[10]` never used.** Declared (`Basedef.h:1255`) but no code path reads or writes it — always `UNKNOWN`. Possibly reserved for a future token variant.
- **Same-type multi-leg reuse.** Both 0xFDE and 0xFDF carry all four direction flags; the receiver must rely on the socket and payload shape (full struct vs. `MSG_STANDARD`) to disambiguate leg. `BASE_CheckPacket` (disabled) would have enforced `sizeof` per type, but cannot distinguish the 32-byte request from the 12-byte `MSG_STANDARD` reply since both use Type 0xFDE.
- **`NumericToken[0] == -1` sentinel** stores `-1` (0xFF) into a `char`; meaningful only because a real token is ASCII digits (0x30–0x39). A token whose first byte is 0xFF is impossible from the client.
- **Token is plaintext and 6 chars fixed.** Compared with `strncmp` (`CFileDB.cpp:1392`); no hashing.
- **Silent-drop asymmetry.** The "change while not secure" path (`CFileDB.cpp:1366-1371`) sends no reply at all, whereas the general fallthrough (`:1419-1420`) sends a fail — a client cannot distinguish "invalid change request" from "nothing happened".
- No report-vs-code conflicts found.

## 10. Source References

**Constants / structs**
- `Basedef.h:1248-1257` (`_MSG_AccountSecure`, `_MSG_AccountSecureFail`, `MSG_AccountSecure`), `925-930` (`_MSG` macro), `932-941` (FLAG_*), `170` (`ESCENE_FIELD`), `791` (`STRUCT_ACCOUNTINFO.NumericToken`).

**TMSrv**
- `_MSG_AccountSecure.cpp:21-31` (handler), `ProcessClientMessage.cpp:38-66` (dispatcher guards), `88-89` (case), `ProcessClientMessage.h:82` (proto).
- `ProcessDBMessage.cpp:541-544` (success relay), `547-550` (fail relay).
- `SendFunc.cpp:197-206` (`SendClientSignal`).

**DBSrv**
- `CFileDB.cpp:119` (token init `-1`), `762/806` (`SecurePass` at login), `1043-1050` (character-login gate), `1357-1421` (`_MSG_AccountSecure` handler), `2108-2120` (`SendDBSignal`).
