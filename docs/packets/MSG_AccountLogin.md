# MSG_AccountLogin

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_AccountLogin` = `(13 \| FLAG_CLIENT2GAME)` = `0x020D` = `525` |
| Sequence ID | 13 |
| Direction(s) | Client → TMSrv (`FLAG_CLIENT2GAME`) |
| Wire struct | `MSG_AccountLogin` (dedicated struct, pack(1)) |
| Total size | 116 bytes (fixed) |
| Packing | Explicit `#pragma pack(push, 1)` (`Basedef.h:1212-1246`) |
| Handler | `Exec_MSG_AccountLogin` @ `TMSrv/_MSG_AccountLogin.cpp:21` |
| Aliases | Low-byte 13 shared with `_MSG_GuildZoneReport` `(13 \| FLAG_GAME2DB)` = `0x080D` — different direction, unrelated. Struct-share: `MSG_AccountLogin_HWID` (defined, never used). |
| Related | `_MSG_CNFAccountLogin` `(10 \| FLAG_GAME2CLIENT)` = `0x010A`; `_MSG_DBAccountLogin` `(3 \| FLAG_GAME2DB)` = `0x0803`; `_MSG_DBCNFAccountLogin` `(22 \| FLAG_DB2GAME)` = `0x0416`; `_MSG_DBAccountLoginFail_Account` `0x0421`; `_MSG_DBAccountLoginFail_Pass` `0x0422`; `_MSG_DBAccountLoginFail_Block` `0x0424`; `_MSG_DBAccountLoginFail_Disable` `0x0425`; `_MSG_DBAlreadyPlaying` `0x041F`; `_MSG_DBStillPlaying` `0x0420`; `_MSG_DBCheckPrimaryAccount` `0x0414` |

## 2. Wire Framing (protocol preamble)

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with 4-byte `INITCODE = 0x1F11F311` magic before any framed message.
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating XOR transform keyed by `KeyWord` (index into shared `pKeyWord[512]`).
- `CheckSum` = `Sum2 - Sum1` (raw vs. transformed payload sums); validated on receive.
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` else buffer is reset.
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is disabled (body commented out, returns `FALSE`) — no central size validation in release.

Per-packet notes:
- No deviation from standard framing. This is a plain client→game packet, carried over the client socket to TMSrv.
- `Type = 0x020D` on the wire from the client; **rewritten to `_MSG_DBAccountLogin` (0x0803)** by the handler before DB forwarding (`_MSG_AccountLogin.cpp:73`).
- The client-side `Size` is expected to be `sizeof(MSG_AccountLogin)` = 116. The handler accepts any `Size >= 116` (see §5, guard #2) and always re-sends exactly `sizeof(MSG_AccountLogin)` = 116 bytes to DBSrv (`_MSG_AccountLogin.cpp:92`).

## 3. Binary Layout

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 116) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_AccountLogin` = 0x020D |
| 6 | 2 | `ID` | `short` | Client connection slot (sender-stamped; TMSrv overwrites with `conn` before DB forward) |
| 8 | 4 | `ClientTick` | `unsigned int` | Client tick; must not equal `SKIPCHECKTICK` (0x0E0A_… = 235543242) |

### 3.2 Payload

Packing context: **pack(1)** active (region `Basedef.h:1212-1246`). No alignment padding anywhere. All fields contiguous.

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 12 | `AccountPassword` | `char[12]` | 1 | 0 | Plaintext account password (`ACCOUNTPASS_LENGTH` = 12). Compared with `strcmp` on DBSrv. |
| 24 | 16 | `AccountName` | `char[16]` | 1 | 0 | Account name (`ACCOUNTNAME_LENGTH` = 16). Uppercased on TMSrv; must not match `COMn`/`LPTn` device names. |
| 40 | 52 | `Zero` | `char[52]` | 1 | 0 | See note below — server-transfer TempKey transport. Treated as opaque 52-byte buffer. |
| 92 | 4 | `ClientVersion` | `int` | 1 | 0 | Client build version; must equal `APP_VERSION` = 6975 (`Basedef.h:42`). |
| 96 | 4 | `DBNeedSave` | `int` | 1 | 0 | Flags how DBSrv handles an already-online account (0 = reject as already playing; != 0 = force save+quit old session). |
| 100 | 16 | `AdapterName` | `int[4]` | 1 | 0 | Client MAC/network-adapter identifier (4 DWORDs). Stored per-account; used for MAC blocking and primary-account detection. |

**Total payload: 104 bytes → total struct 116 bytes.**

`Zero[52]` note: during a normal first login this field is zero-filled. During a **server-change (character transfer)** re-login, the destination client echoes back the `TempKey` string previously stored by the source DB (see §6, Rule 3). On the destination DBSrv, `memcmp(file.TempKey, m->Zero, 52)` decides whether to bypass the password and auto-login into a specific character slot. `sscanf(m->Zero, "*%d %d … %d %d", …)` parses it as `*<Server> <Enc0..Enc7> <Slot>` (`CFileDB.cpp:812`).

### 3.3 Nested struct expansions

None. `MSG_AccountLogin` embeds only primitive arrays; no `STRUCT_*` members.

### 3.4 Size verification

Offset math (pack(1), no padding):
```
Header (_MSG):                      0 + 12              = 12
AccountPassword[12]                12 + 12              = 24
AccountName[16]                    24 + 16              = 40
Zero[52]                           40 + 52              = 92
ClientVersion (int,4)              92 + 4               = 96
DBNeedSave (int,4)                 96 + 4               = 100
AdapterName (int[4] = 16)          100 + 16             = 116
sizeof(MSG_AccountLogin)                                   = 116
```
- Expected `Size` header value: **116**.
- Cross-checks: handler validates `Size >= sizeof(MSG_AccountLogin)` and sends `sizeof(MSG_AccountLogin)` bytes to DBSrv (`_MSG_AccountLogin.cpp:42,44,67,92`) — consistent with 116. `Basedef.cpp:6485` (`BASE_CheckPacket`, disabled) checks `_MSG_AccountLogin` against `sizeof(MSG_AccountLogin)`; `Basedef.cpp:6575` checks `_MSG_DBAccountLogin` against the same — all 116. No mismatch.

Variant `MSG_AccountLogin_HWID` (`Basedef.h:1229-1245`): identical first 116 bytes plus `char HwId[50]` at offset 116 → total 166. **Never referenced anywhere in source** — the server always casts to `MSG_AccountLogin` (116). Treat 166-byte frames as carrying a trailing HWID the server ignores.

## 4. Lifecycle & Flow

Direction: **Client → TMSrv → DBSrv → TMSrv → Client**.

```
[Client]                                  [TMSrv]                                   [DBSrv]
   |  MSG_AccountLogin (0x020D, 116B)        |                                            |
   |---------------------------------------->|                                            |
   |   ClientTick != SKIPCHECKTICK,          |                                            |
   |   ID in [0,MAX_USER), !ServerDown       |                                            |
   |                                         | ProcessClientMessage:68 → Exec_MSG_AccountLogin:21
   |                                         |  guards: conn range, Size, ClientVersion,
   |                                         |  Mode==USER_ACCEPT, CheckFailAccount<3
   |                                         |  rewrite Type=0x0803, ID=conn
   |                                         |  DBServerSocket.SendOneMessage(:92)
   |                                         |  Mode=USER_LOGIN, pMob=MOB_EMPTY (:94-95)
   |                                         |---------------------------------------->|
   |                                         |   CFileDB.cpp:611 _MSG_DBAccountLogin
   |                                         |   validate account/pass/ban; build reply
   |                                         |<----------------------------------------|
   |                                         |  _MSG_DBCNFAccountLogin (0x0416) to TMSrv
   |                                         | ProcessDBMessage:354 →
   |                                         |  MAC-block check, reset session flags,
   |                                         |  Type=_MSG_CNFAccountLogin, ID=ESCENE_FIELD+2
   |   MSG_CNFAccountLogin (0x010A)          |  Mode=USER_SELCHAR
   |<----------------------------------------|
```

- **Client → TMSrv**: `CPSock.ReadMessage` de-frames/de-obfuscates → `Server.cpp (WSA_READ)` → `ProcessClientMessage(conn, pMsg, FALSE)` → `switch(std->Type)` case `_MSG_AccountLogin` → `Exec_MSG_AccountLogin(conn, pMsg)` (`ProcessClientMessage.cpp:68-69`).
- **Dispatcher guards** (`ProcessClientMessage.cpp:42-64`): `ID ∈ [0, MAX_USER)`, `ServerDown >= 120` returns, `ClientTick == SKIPCHECKTICK` anti-spoof drop.
- **TMSrv handler** (`_MSG_AccountLogin.cpp:21-96`): guards → rewrites `m->Type = _MSG_DBAccountLogin`, `m->ID = conn`, uppercases account name, forwards `sizeof(MSG_AccountLogin)` bytes via `DBServerSocket.SendOneMessage` (`:92`), sets `pUser[conn].Mode = USER_LOGIN`, `pMob[conn].Mode = MOB_EMPTY` (`:94-95`).
- **TMSrv → DBSrv**: `_MSG_DBAccountLogin` (0x0803) on the DBServerSocket.
- **DBSrv processing** (`CFileDB.cpp:611-854`): full auth; replies with `_MSG_DBCNFAccountLogin` (0x0416) or a `_MSG_DBAccountLoginFail_*`/`_MSG_DBAlreadyPlaying`/`_MSG_DBStillPlaying` signal via `SendDBSignal`/`SendDBSignalParm` (`CFileDB.cpp:2108-2135`). `m->ID` (client conn slot) is copied into the reply `ID`.
- **DBSrv → TMSrv → Client**: `ProcessDBMessage.cpp:354` case `_MSG_DBCNFAccountLogin` → MAC-block re-check, flag reset, then re-targets the message: `m->ID = ESCENE_FIELD + 2`, `m->Type = _MSG_CNFAccountLogin`, `Mode = USER_SELCHAR`, sends to client (`:397-434`). Fail signals map to localized messages + `CloseUser` (`:487-576`).

## 5. Validation & Guards

| # | Check | Condition | On failure | Location |
|---|---|---|---|---|
| 1 | Connection slot range | `conn <= 0 \|\| conn >= MAX_USER - ADMIN_RESERV` | Send `_NN_Reconnect` msg, flush, `CloseUser`, return | `_MSG_AccountLogin.cpp:30-39` |
| 2 | Size + client version | `Size < sizeof(MSG_AccountLogin)` **OR** `ClientVersion != APP_VERSION(6975)` (unless `_PACKET_DEBUG`) | Send `_NN_Version_Not_Match_Rerun` msg, flush, `CloseUser` | `_MSG_AccountLogin.cpp:41-54` |
| 3 | Session mode gate | `pUser[conn].Mode != USER_ACCEPT` | Send "Login now, wait a moment.", `CrackLog(conn," accountlogin")`, flush; **no close** | `_MSG_AccountLogin.cpp:56-63` |
| 4 | MAC capture | `m->Size < sizeof(MSG_AccountLogin)` → `Mac=0xFF…`; else `memcpy(Mac, AdapterName, 16)` | (corrective, not a fail) | `_MSG_AccountLogin.cpp:65-70` |
| 5 | Account-name normalization | `sscanf` name, `_strupr`, re-copy into `m->AccountName` | (transform) | `_MSG_AccountLogin.cpp:76-80` |
| 6 | Repeated-failure gate | `CheckFailAccount(name) >= 3` (count in `FailAccount[16][16]`) | Send `_NN_3_Tims_Wrong_Pass`, flush; **no close** | `_MSG_AccountLogin.cpp:82-90` |
| 7 | ClientTick anti-spoof | `ClientTick == SKIPCHECKTICK` (dispatcher) | Drop silently | `ProcessClientMessage.cpp:63-64` |

Notes:
- Guard #2's `Size` check is a minimum (`>= 116`), not an exact equality; oversized frames (incl. a `MSG_AccountLogin_HWID` 166-byte frame) pass it and are handled as 116 bytes.
- Guard #3/#6 return without closing — the client keeps the connection and may retry; only guard #1/#2 close.
- `CheckFailAccount` counts exact matches of the uppercased account name against the in-memory `FailAccount` table (populated via `AddFailAccount`, `Server.cpp:1334`, cleared on server start / periodically `ProcessSecMinTimer.cpp:2137`).

## 6. Game Mechanics & Business Logic

### Rule 1: Account authentication (DBSrv)
**Overview.** The DB validates the supplied account/password against the on-disk account file and enforces ban status.
**Workflow:**
1. `_strupr(m->AccountName)`; reject device names `COMn`/`LPTn` → `_MSG_DBAccountLoginFail_Account` (`CFileDB.cpp:615-625`).
2. `Idx = GetIndex(conn, m->ID)` (session slot), `IdxName = GetIndex(m->AccountName)` (existing logged-in session) (`:627-628`).
3. Read account file via `DBReadAccount` (path `./account/<firstkey>/<ACCTNAME>`, `CFileDB.cpp:2518`); missing/read-fail → `_MSG_DBAccountLoginFail_Account` (`:636-643`).
4. Clamp negative `Coin` to 0 (`:645-646`).
5. Ban check: if `Year != 0 && YearDay != 0` and (`Year >= now.tm_year` OR `Year == now.tm_year && YearDay >= now.tm_yday`) → `_MSG_DBAccountLoginFail_Block` (Parm 0) (`:648-660`).
6. Password: `strcmp(file.Info.AccountPass, m->AccountPassword)`; mismatch → `_MSG_DBAccountLoginFail_Pass` (`:677-682`).

### Rule 2: Single-session enforcement
**Workflow:**
1. If `IdxName == Idx` → no-op (already this session) (`:685-686`).
2. If `IdxName != 0` (account online elsewhere): if `m->DBNeedSave == 0` → `_MSG_DBAlreadyPlaying`; else → `_MSG_DBStillPlaying` + `SendDBSavingQuit(IdxName, 0)` (force old session to save/quit), then continue login (`:688-703`).

### Rule 3: Server-change (character transfer) fast path — the `Zero[52]` field
**Overview.** Transfer re-login bypasses the password when the client presents the TempKey string the source DB stored.
**Workflow:**
1. If `file.TempKey[0] != 0 && m->Zero[0] != 0` (`:664`):
   - `memcmp(file.TempKey, m->Zero, 52) == 0` → clear `TempKey`, `ChangeServer = 1`, `goto lb_sucess` (skip password) (`:666-671`).
   - mismatch → clear `TempKey`, `DBWriteAccount`, return (login fails) (`:672-674`).
2. On success with `ChangeServer == 1`: `sscanf(m->Zero, "*%d %d %d %d %d %d %d %d %d %d", &Server, &Enc[0..7], &Slot)` (`:812`), persist (`DBWriteAccount`), `SecurePass = 1`, and send `_MSG_DBCNFCharacterLogin` (0x0417) with the parsed `Slot`'s character, `ShortSkill`, `affect`, `mobExtra`, `Donate` — a direct character auto-login (`:802-853`).
3. The `TempKey` is produced by the DBSrv's server-change handler: `sprintf(sm.Enc, "*%d %d … %d %d", NewServerID, Enc[0..7], Slot)` then `memcpy(TempKey, sm.Enc, 52)` (`CFileDB.cpp:1981-1984`).

### Rule 4: Post-auth success handling
**Workflow:**
1. Name-swap fix: if two char slots carry `Equip[13].sIndex` 774 and 775, swap their `MobName`s, clear both, log `"etc,name swap %s %s"`, `DBWriteAccount` (`:707-743`).
2. Copy account file into session `pAccountList[Idx].File`, record `Mac[0..3] = m->AdapterName[0..3]`, `AddAccountList(Idx)` (`:745-756`).
3. Build select-character summary via `DBGetSelChar` (`:758-760`, `:2611`) and send `MSG_DBCNFAccountLogin` (Type 0x0416, ID = client conn) with `Unknow_28 = 0xCCCCCCCC`, account name, full cargo, coin, sel (`:764-780`).
4. Primary-account broadcast: if `GetAccountsByMac(m->AdapterName) <= 1`, send `_MSG_DBCheckPrimaryAccount` (0x0414) to every game server (`:782-801`).

### Rule 5: TMSrv-side confirmation post-processing
When `ProcessDBMessage.cpp:354` receives `_MSG_DBCNFAccountLogin`:
1. Require `strcmp(m->AccountName, pUser[conn].AccountName) == 0` else `_NN_Try_Reconnect` + `CloseUser` (`:358-367`).
2. MAC block table scan `pMac[MAX_MAC]`; match → `_NN_MAC_Block` + `CloseUser` (`:369-383`).
3. Reset session flags (`IsBillConnect`, `Whisper`, `Guildchat`, `PartyChat`, `KingChat`, `Admin`, `Chatting`, `Unk_2732/2728`, `OnlyTrade=1`) (`:386-395`).
4. Sanitize cargo: weapon-position items (`nPos` 64/192) with `EF_DAMAGEADD`/`EF_DAMAGE2` → `EF_DAMAGE`; if `evDelete`, null `sIndex` in [470,500] (`:402-431`).
5. Re-target + send to client: `ID = ESCENE_FIELD + 2`, `Type = _MSG_CNFAccountLogin`, `Mode = USER_SELCHAR` (`:397-400,434`); copy `Cargo`, `Coin`, `SelChar` into `pUser` (`:436-440`).
6. Optional billing (`BILLING > 0 && IsFree(&m->sel)`) → `SendBilling`; optional `TransperCharacter` → extra `_MSG_TransperCharacter` (`:443-470`).

## 7. Side Effects

| Effect | Target | Mechanism | Location |
|---|---|---|---|
| Mode transition `USER_ACCEPT → USER_LOGIN` | `pUser[conn].Mode` | set directly | `_MSG_AccountLogin.cpp:94` |
| Mob mode `MOB_EMPTY` | `pMob[conn].Mode` | set directly | `_MSG_AccountLogin.cpp:95` |
| Account name stored/uppercased | `pUser[conn].AccountName` | `sscanf`+`_strupr` | `_MSG_AccountLogin.cpp:76-78` |
| MAC stored | `pUser[conn].Mac[4]` | `memcpy` from `AdapterName` (or `0xFF`) | `_MSG_AccountLogin.cpp:67-70` |
| DB forward `_MSG_DBAccountLogin` (0x0803) | DBSrv | `DBServerSocket.SendOneMessage` (Type/ID rewritten) | `_MSG_AccountLogin.cpp:73-92` |
| Outgoing fail messages | client | `SendClientMessage` + `SendMessageA` flush | `_MSG_AccountLogin.cpp:33,48,58,86` |
| Crack log | log | `CrackLog(conn, " accountlogin")` on mode gate | `_MSG_AccountLogin.cpp:60` |
| Account file read | disk `./account/…` | `DBReadAccount` | `CFileDB.cpp:636` |
| Account file write (TempKey clear / name swap / transfer persist) | disk | `DBWriteAccount` | `CFileDB.cpp:673,742,804` |
| Session state | `pAccountList[Idx]` | file copy, MAC, `AddAccountList`, `SecurePass` | `CFileDB.cpp:745-756` |
| Reply `_MSG_DBCNFAccountLogin` (0x0416) | TMSrv | `SendOneMessage` | `CFileDB.cpp:780` |
| Primary-account broadcast `_MSG_DBCheckPrimaryAccount` | all game servers | `SendOneMessage` loop | `CFileDB.cpp:782-801` |
| Fail signals (Account/Pass/Block/Already/Still) | TMSrv | `SendDBSignal`/`SendDBSignalParm` | `CFileDB.cpp:622,640,657,679,694,699` |
| Force old-session quit `_MSG_DBSavingQuit` | old session's TMSrv | `SendDBSavingQuit(IdxName,0)` | `CFileDB.cpp:700` |
| Log `"etc,name swap %s %s"` | log | `Log` | `CFileDB.cpp:738` |
| Log `"CNFAccountLogin Mac: %d.%d.%d.%d"` | log | `Log` | `ProcessDBMessage.cpp:456-457` |
| Session flag reset + `Mode = USER_SELCHAR` | `pUser[conn]` | set directly | `ProcessDBMessage.cpp:386-400` |

## 8. Related Packets

| Packet | Constant | Value | Direction | Role |
|---|---|---|---|---|
| `_MSG_AccountLogin` | `(13 \| FLAG_CLIENT2GAME)` | `0x020D` | C→G | This packet |
| `_MSG_DBAccountLogin` | `(3 \| FLAG_GAME2DB)` | `0x0803` | G→DB | TMSrv forwarding of this packet |
| `_MSG_DBCNFAccountLogin` | `(22 \| FLAG_DB2GAME)` | `0x0416` | DB→G | Success reply (`MSG_DBCNFAccountLogin` struct, `Basedef.h:1095`) |
| `_MSG_CNFAccountLogin` | `(10 \| FLAG_GAME2CLIENT)` | `0x010A` | G→C | Same struct re-targeted to client (`ProcessDBMessage.cpp:397-398`) |
| `_MSG_DBAccountLoginFail_Account` | `(33 \| FLAG_DB2GAME)` | `0x0421` | DB→G | Bad/missing account (`MSG_STANDARD`) |
| `_MSG_DBAccountLoginFail_Pass` | `(34 \| FLAG_DB2GAME)` | `0x0422` | DB→G | Wrong password (`MSG_STANDARD`) |
| `_MSG_DBAccountLoginFail_Block` | `(36 \| FLAG_DB2GAME)` | `0x0424` | DB→G | Banned (`MSG_STANDARDPARM`, Parm 0..3→X/A/B/C) |
| `_MSG_DBAccountLoginFail_Disable` | `(37 \| FLAG_DB2GAME)` | `0x0425` | DB→G | Disabled account (`MSG_STANDARD`); **handler present, no producer found** |
| `_MSG_DBAlreadyPlaying` | `(31 \| FLAG_DB2GAME)` | `0x041F` | DB→G | Account online, `DBNeedSave==0` |
| `_MSG_DBStillPlaying` | `(32 \| FLAG_DB2GAME)` | `0x0420` | DB→G | Account online, force old session out |
| `_MSG_DBSavingQuit` | `(10 \| FLAG_DB2GAME)` | `0x040A` | DB→G | Force old session save+quit |
| `_MSG_DBCheckPrimaryAccount` | `(20 \| FLAG_DB2GAME)` | `0x0414` | DB→G | Sole-account-on-MAC broadcast |
| `_MSG_GuildZoneReport` | `(13 \| FLAG_GAME2DB)` | `0x080D` | G→DB | Alias (same low byte 13, different path) |
| `MSG_AccountLogin_HWID` | (shares struct value) | — | — | 166-byte variant; defined, never used |

## 9. Discrepancies & Open Questions

- **`MSG_AccountLogin_HWID` never used.** Defined (`Basedef.h:1229-1245`) but no reference exists anywhere in the codebase. The server always treats the frame as `MSG_AccountLogin` (116 bytes). This may be a client-variant in the original protocol that W2PP's server never implemented/needed.
- **`_MSG_DBAccountLoginFail_Disable` producer missing.** Its TMSrv handler exists (`ProcessDBMessage.cpp:527-538`) but no DBSrv code path sends it (the fail path is `Fail_Block`). Possible dead path or an unimplemented account-disable flag.
- **`Unknow_28` = `0xCCCCCCCC`** in `MSG_DBCNFAccountLogin` (`CFileDB.cpp:771`) — sentinel value, semantics unknown.
- **`Zero[52]` dual use.** It is both a "must be zero" padding field on normal login and a 52-byte TempKey transport on server-change login. The `memcmp` (`CFileDB.cpp:666`) compares the full 52 bytes against `file.TempKey`.
- **Password is plaintext.** `strcmp(file.Info.AccountPass, m->AccountPassword)` (`CFileDB.cpp:677`) — no hashing. `AdapterName` is likewise stored/compared raw.
- `FailAccount` is only populated via `AddFailAccount` (called by the password-fail DB response elsewhere); the `_NN_3_Tims_Wrong_Pass` gate at `_MSG_AccountLogin.cpp:84` depends on that counter being maintained elsewhere.
- Report-vs-code: the component report's `_MSG_DBAccountLogin` workflow matches the code exactly; no conflicts found.

## 10. Source References

**Constants / structs**
- `Basedef.h:42` (`APP_VERSION`), `65-66` (`ACCOUNTNAME/ACCOUNTPASS_LENGTH`), `170-172` (`ESCENE_FIELD`, `SKIPCHECKTICK`), `925-941` (`_MSG` macro, FLAG_*), `1094-1109` (`_MSG_DBCNFAccountLogin`, `MSG_DBCNFAccountLogin`), `1119-1126` (`_MSG_DBAlreadyPlaying`…`_MSG_DBAccountLoginFail_*`), `1210-1211` (`_MSG_AccountLogin`, `_MSG_CNFAccountLogin`), `1212-1246` (pack(1) `MSG_AccountLogin`, `MSG_AccountLogin_HWID`), `978` (`_MSG_DBAccountLogin`), `855` (`TempKey`), `765-778` (`STRUCT_SELCHAR`).

**TMSrv**
- `ProcessClientMessage.cpp:38-69` (dispatcher guards + case), `ProcessClientMessage.h:77` (proto).
- `_MSG_AccountLogin.cpp:21-96` (handler).
- `ProcessDBMessage.cpp:354-472` (`_MSG_DBCNFAccountLogin`), `487-576` (fail signals).
- `Server.cpp:1334-1361` (`AddFailAccount`, `CheckFailAccount`), `369` (`FailAccount`), `6671` (`CrackLog`).
- `Language.h:56,57,130-133,150-153,161,174,191,522` (`_NN_*`/`_SN_*` message ids).

**DBSrv**
- `CFileDB.cpp:611-854` (`_MSG_DBAccountLogin`), `2108-2135` (`SendDBSignal`/`SendDBSignalParm`), `2369-2388` (`SendDBSavingQuit`), `2390-2517` (`DBWriteAccount`), `2518-2577` (`DBReadAccount`), `2611-2631` (`DBGetSelChar`), `1981-1984` (TempKey set).
