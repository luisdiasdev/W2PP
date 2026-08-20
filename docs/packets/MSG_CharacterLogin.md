# MSG_CharacterLogin

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_CharacterLogin` = `(19 \| FLAG_CLIENT2GAME)` = `0x0213` = `531` |
| Sequence ID | 19 |
| Direction(s) | Client → TMSrv (`FLAG_CLIENT2GAME`); TMSrv forwards to DBSrv as `_MSG_DBCharacterLogin`; DBSrv replies `_MSG_DBCNFCharacterLogin` → TMSrv → client `_MSG_CNFCharacterLogin` |
| Wire struct | `MSG_CharacterLogin` (dedicated struct, `Basedef.h:1336-1342`) |
| Total size | 20 bytes (fixed; `sizeof(MSG_CharacterLogin)`), no variable-length tail |
| Packing | **Default MSVC `/Zp8`** — NOT in any `#pragma pack(push,1)` region (pack(1) regions: `Basedef.h:808-835`, `1212-1246`, `1465-1492`, `2063-2097`; this struct at 1336-1342 falls outside all of them) |
| Handler | `Exec_MSG_CharacterLogin` @ `TMSrv/_MSG_CharacterLogin.cpp:21` |
| Aliases | Low-byte 19 is shared only by `_MSG_CharacterLogin` itself (no other constant uses low byte 19). The struct `MSG_CharacterLogin` is shared verbatim by its DB-mirror `_MSG_DBCharacterLogin` (same wire layout, different `Type`). |
| Related | `_MSG_DBCharacterLogin` `(4\|FLAG_GAME2DB)`=`0x0804`; `_MSG_DBCNFCharacterLogin` `(23\|FLAG_DB2GAME)`=`0x0417` (struct `MSG_CNFCharacterLogin`); `_MSG_CNFCharacterLogin` `(20\|FLAG_GAME2CLIENT)`=`0x0114` (struct `MSG_CNFClientCharacterLogin`); `_MSG_CharacterLoginFail` `(25\|FLAG_GAME2CLIENT)`=`0x0119`; `_MSG_DBCharacterLoginFail` `(28\|FLAG_DB2GAME)`=`0x041C` |

## 2. Wire Framing

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with 4-byte `INITCODE = 0x1F11F311` magic before any framed message.
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating XOR transform keyed by `KeyWord` (index into shared `pKeyWord[512]`).
- `CheckSum` = `Sum2 - Sum1` (raw vs. transformed payload sums); validated on receive.
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` else the buffer is reset.
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is disabled (body commented out, returns `FALSE`) — no central size validation in release.

Per-packet notes:
- No deviation from standard framing. Plain client→game packet carried on the client socket to TMSrv.
- `Type = 0x0213` on the wire from the client; **rewritten to `_MSG_DBCharacterLogin` (0x0804)** by the handler before DB forwarding, and `ID` is rewritten to the client `conn` (`_MSG_CharacterLogin.cpp:73-81`).
- The client-side `Size` is expected to be `sizeof(MSG_CharacterLogin)` = 20. **The handler performs NO size validation** — it casts to `MSG_CharacterLogin` and only reads `Slot`/`Force`; an oversized or undersized frame is still interpreted as 20 bytes and re-sent as exactly 20 bytes to DBSrv.
- The DB mirror `_MSG_DBCharacterLogin` (0x0804) carries the identical 20-byte payload (same struct, `Type` rewritten).
- The DBSrv success reply `_MSG_DBCNFCharacterLogin` (0x0417) uses the much larger `MSG_CNFCharacterLogin` struct (2616 bytes); TMSrv then re-targets a trimmed `MSG_CNFClientCharacterLogin` (2608 bytes) to the client with `Type = _MSG_CNFCharacterLogin` (0x0114).

## 3. Binary Layout

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 20) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_CharacterLogin` = 0x0213 (0x0804 after TMSrv rewrite) |
| 6 | 2 | `ID` | `short` | Client connection slot (sender-stamped; TMSrv overwrites with `conn` before DB forward) |
| 8 | 4 | `ClientTick` | `unsigned int` | Client tick; must not equal `SKIPCHECKTICK` (0x0E0A_… = 235543242) |

### 3.2 Payload

Packing context: **default `/Zp8`** (not a pack(1) region — see §1). Each member aligned to `min(sizeof(member), 8)`. The two payload members are `int` (align 4), and the header ends at offset 12 which is already 4-aligned, so **no padding** is inserted.

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 4 | `Slot` | `int` | 4 | 0 | 0-based character slot to log into. Must be `>= 0 && < MOB_PER_ACCOUNT` (4) (`_MSG_CharacterLogin.cpp:25`). |
| 16 | 4 | `Force` | `int` | 4 | 0 | **UNKNOWN semantics** — declared but never read anywhere in the TMSrv/DBSrv handlers. Treated as an unused/ignored field. |

**Total payload: 8 bytes → total struct 20 bytes.**

### 3.3 Nested struct expansions

`MSG_CharacterLogin` embeds only the `_MSG` header macro (primitive fields) plus two `int`s. It contains **no `STRUCT_*` members**, so there are no nested expansions.

For reference, the response structs used by the DBSrv reply (`_MSG_DBCNFCharacterLogin`) and the client re-target (`_MSG_CNFCharacterLogin`) DO embed nested structs; their sizes (computed under MSVC `/Zp8`, LP32 where `long`/`time_t` = 4 bytes) are:

| Struct | Size | Align | Location |
|---|---|---|---|
| `STRUCT_ITEM` | 8 | 2 | `Basedef.h:398-412` |
| `STRUCT_SCORE` | 48 | 4 | `Basedef.h:414-436` |
| `STRUCT_MOB` | 816 | 8 | `Basedef.h:438-481` |
| `STRUCT_AFFECT` | 8 | 4 | `Basedef.h:593-599` |
| `STRUCT_MOBEXTRA` | 520 | 8 | `Basedef.h:483-591` |
| `MSG_CNFCharacterLogin` | 2616 | 8 | `Basedef.h:1345-1367` |
| `MSG_CNFClientCharacterLogin` | 2608 | 8 | `Basedef.h:1368-1386` |

### 3.4 Size verification

Offset math (`/Zp8`, no padding in this struct):
```
Header (_MSG):                  0 + 12              = 12
Slot (int, align 4)            12 + 4               = 16
Force (int, align 4)           16 + 4               = 20
sizeof(MSG_CharacterLogin)                              = 20
```
- Expected `Size` header value: **20**.
- Cross-checks: the handler forwards exactly `sizeof(MSG_CharacterLogin)` = 20 bytes to DBSrv (`_MSG_CharacterLogin.cpp:81`). `Basedef.cpp:6497` (`BASE_CheckPacket`, disabled) checks `_MSG_CharacterLogin` against `sizeof(MSG_CharacterLogin)`; `Basedef.cpp:6564` checks `_MSG_DBCharacterLogin` against the same — both 20. **No mismatch.**
- DBSrv reply size: `sm.Size = sizeof(MSG_CNFCharacterLogin)` = 2616 and sent as 2616 (`CFileDB.cpp:1058,1080`). TMSrv copies `sizeof(MSG_CNFClientCharacterLogin)` = 2608 bytes from it (`ProcessDBMessage.cpp:641`) and sends 2608 to the client (`ProcessDBMessage.cpp:984`). The 8-byte difference is the trailing `int Donate` (+ trailing padding) in `MSG_CNFCharacterLogin`; `m->Donate` is read before the copy (`ProcessDBMessage.cpp:728`).
- No variable-length tail. No unknown/reserved members in the request (only `Force`, which is unused).

## 4. Lifecycle & Flow

Direction: **Client → TMSrv → DBSrv → TMSrv → Client**.

```
[Client]                                  [TMSrv]                                    [DBSrv]
   |  MSG_CharacterLogin (0x0213, 20B)        |                                             |
   |  Slot, Force; Tick != SKIPCHECKTICK      |                                             |
   |----------------------------------------->|                                             |
   |   Dispatcher guards (ID in [0,MAX_USER), |                                             |
   |   ServerDown<120, Tick!=SKIPCHECKTICK)   |                                             |
   |                                          | ProcessClientMessage:72 -> Exec_MSG_CharacterLogin:21
   |                                          |  Slot range guard, billing-mode logic (see §5)
   |                                          |  rewrite Type=0x0804, ID=conn
   |                                          |  DBServerSocket.SendOneMessage(:81) = 20B
   |                                          |  Mode=USER_CHARWAIT, pMob.Mode=MOB_USER (:76-77)
   |                                          |-------------------------------------------->|
   |                                          |   DBSrv Server.cpp:1339 ProcessClientMessage
   |                                          |   (guard: Type&FLAG_GAME2DB, ID in [0,MAX_USER])
   |                                          |   -> cFileDB.ProcessMessage -> CFileDB.cpp:1028
   |                                          |   case _MSG_DBCharacterLogin
   |                                          |   validate Slot, SecurePass, MobName
   |                                          |   build MSG_CNFCharacterLogin (2616B) from
   |                                          |   account file Char[Slot]/ShortSkill/affect/
   |                                          |   mobExtra/Donate; Type=0x0417, ID=client conn
   |                                          |<--------------------------------------------|
   |                                          |  _MSG_DBCNFCharacterLogin (0x0417)
   |                                          | ProcessDBMessage:635 (conn = std->ID = client slot)
   |                                          |  item-effect sanitization, class-master fixups,
   |                                          |  build MSG_CNFClientCharacterLogin (2608B),
   |                                          |  Type=0x0114, spawn position, guild/city logic,
   |                                          |  Mode=USER_PLAY
   |   MSG_CNFCharacterLogin (0x0114)          |  pUser[conn].cSock.SendOneMessage(:984)
   |<-----------------------------------------|
```

- **Client → TMSrv**: `CPSock.ReadMessage` de-frames/de-obfuscates → `Server.cpp (WSA_READ)` → `ProcessClientMessage(conn, pMsg, FALSE)` → `switch(std->Type)` case `_MSG_CharacterLogin` → `Exec_MSG_CharacterLogin(conn, pMsg)` (`ProcessClientMessage.cpp:72-74`).
- **Dispatcher guards** (`ProcessClientMessage.cpp:42-64`): `ID ∈ [0, MAX_USER)` else log+drop; `ServerDown >= 120` drop; `ClientTick == SKIPCHECKTICK` anti-spoof drop. `_MSG_Ping` also returns early (`:59-60`). No `default:` case — unknown types dropped silently.
- **TMSrv handler** (`_MSG_CharacterLogin.cpp:21-109`): billing-gate logic (see §5); on pass, rewrites `m->Type = _MSG_DBCharacterLogin`, `m->ID = conn`, sets `pUser[conn].Mode = USER_CHARWAIT` and `pMob[conn].Mode = MOB_USER`, clears `pMob[conn].MOB.Merchant`, then `DBServerSocket.SendOneMessage(m, sizeof(MSG_CharacterLogin))` (`:73-81`).
- **TMSrv → DBSrv**: `_MSG_DBCharacterLogin` (0x0804), 20 bytes, on the `DBServerSocket`.
- **DBSrv processing** (`DBSrv/Server.cpp:1339-1357` → `CFileDB.cpp:1028-1094`): guard `Type & FLAG_GAME2DB` and `ID ∈ [0, MAX_USER]`; `Idx = GetIndex(conn, m->ID)` where `conn` is the DBSrv's game-server slot (`CFileDB.cpp:2328-2333` → `server*MAX_USER + id`); validates Slot, `SecurePass != -1`, non-empty `MobName`; on success builds `MSG_CNFCharacterLogin` with `Type=0x0417`, `ID=m->ID` (client conn), character data from the in-memory `pAccountList[Idx].File`; sends via `pUser[conn].cSock.SendOneMessage` (`CFileDB.cpp:1054-1080`); also updates grind-ranking connection + individual rank (`CFileDB.cpp:1083-1092`).
- **DBSrv → TMSrv → Client**: `ProcessDBMessage.cpp:635` case `_MSG_DBCNFCharacterLogin` (note `conn = std->ID` at `:54`; `conn==0` is spoofed → `CrackLog`+`CloseUser` at `:643-650`). Applies item-effect sanitization, class-master effects, mob stats recompute, spawn placement (city/guild/level gates), then re-targets: `sm.Type = _MSG_CNFCharacterLogin`, `sm.ID = ESCENE_FIELD` (or +1 if `NewbieEventServer`), `sm.ClientID = conn`, sets `Mode = USER_PLAY`, sends 2608 bytes to client (`:787-984`).
- **Failure paths**: `_MSG_DBCharacterLoginFail` (0x041C) is handled at `ProcessDBMessage.cpp:1084-1096` → sends client signal `_MSG_CharacterLoginFail` (0x0119), `Mode = USER_SELCHAR`, `CrackLog`. **No DBSrv producer exists** (see §9).

## 5. Validation & Guards

### 5.1 TMSrv — dispatcher (`ProcessClientMessage.cpp`)

| # | Check | Condition | On failure | Location |
|---|---|---|---|---|
| 1 | ID range | `std->ID < 0 \|\| std->ID >= MAX_USER` | Log `"err,packet Type:%d ID:%d Size:%d KeyWord:%d"`, drop | `ProcessClientMessage.cpp:42-51` |
| 2 | Server-down gate | `ServerDown >= 120` | Drop silently | `ProcessClientMessage.cpp:53-54` |
| 3 | Anti-spoof tick | `isServer==FALSE && std->ClientTick == SKIPCHECKTICK` | Drop silently | `ProcessClientMessage.cpp:63-64` |

### 5.2 TMSrv — `Exec_MSG_CharacterLogin` (`_MSG_CharacterLogin.cpp`)

| # | Check | Condition | On failure | Location |
|---|---|---|---|---|
| 1 | Slot range | `m->Slot < 0 \|\| m->Slot >= MOB_PER_ACCOUNT` (4) | `SendClientMessage(_NN_SelectCharacter)` then return | `:25-29` |
| 2 | Free/billing pre-gate | `SelChar.Score[Slot].Level < FREEEXP \|\| >= 999 \|\| BILLING != 2` | `goto Label1` (skip child/billing branch) | `:31-32` |
| 3 | Billing-state machine (`Unk_1816`) | `Unk_1816 > 1` | enter nested branch (see below) | `:34` |
| 3a | `Unk_1816 == 3` (child-pay block) | — | `SendClientMessage(_DN_Not_Allowed_Account)`, `SendClientSignalParm(conn,0,404,0)`, flush `SendMessageA` | `:36-43` |
| 3b | `Unk_1816 == 4` (other server group) | — | `SendClientMessage(_NN_Using_Other_Server_Group)` | `:46-48` |
| 3c | `Label1` — billing-3 level gate | `Level >= FREEEXP && < 999 && BILLING == 3 && Level >= 1000` | (dead condition, see §9) conditional `_NN_Wait_Checking_Billing` / `_DN_Not_Allowed_Account` / `_NN_Using_Other_Server_Group` | `:53-67` |
| 3d | Forward gate | `BILLING != 2 \|\| Unk_2728 != 1 \|\| Level < FREEEXP \|\| (g_Hour > 7 && g_Hour < 23)` | else `SendClientMessage(_NN_Child_Pay)` | `:69-91` |
| 3e | Mode gate (inside forward) | `pUser[conn].Mode == USER_SELCHAR` | else `SendClientMessage("Wait a moment.")` + log `"err,charlogin not user_selchar %d %d"` | `:71-88` |
| 4 | `Unk_1816 <= 1` branch | `Unk_2732 && Unk_2732 < SecCounter - 10` | clear `Unk_2732`, set `Unk_1816=5` | `:97-101` |
| 5 | Billing poll (else) | (no condition) | `SendClientMessage(_NN_Wait_Checking_Billing)`, `SendBilling(conn, AccountName, 1, 1)` (no-op, `Server.cpp:1377-1380`) | `:104-105` |

Notes:
- Guard 1 is the only hard validation in the handler; **no `Size` check, no `Force` use, no Mode pre-check at entry**.
- The handler never closes the connection on any guard; failures just emit a message and return (client stays connected in `USER_SELCHAR`).
- `FREEEXP = 35` default (`Server.cpp:43`), `BILLING = 3` default (`Server.cpp:320`), both runtime-configurable via game config (`Server.cpp:917-919`).
- `MOB_PER_ACCOUNT = 4` (`Basedef.h:71`).

### 5.3 DBSrv (`Server.cpp:1339` + `CFileDB.cpp:1028`)

| # | Check | Condition | On failure | Location |
|---|---|---|---|---|
| 1 | Direction flag | `!(std->Type & FLAG_GAME2DB)` | Log `"err,packet …"`, drop | `Server.cpp:1343-1353` |
| 2 | ID range | `std->ID < 0 \|\| std->ID > MAX_USER` | Log, drop | `Server.cpp:1343` |
| 3 | Slot range | `Slot < 0 \|\| Slot >= MOB_PER_ACCOUNT` | Log `"err,charlogin slot illegal"`, break (no reply) | `CFileDB.cpp:1036-1041` |
| 4 | Secure pass | `pAccountList[Idx].SecurePass == -1` | Log `"err,charlogin secure illegal"`, break (no reply) | `CFileDB.cpp:1043-1050` |
| 5 | Mob name | `MobName[0] == 0` | Log `"err,charlogin mobname empty"`, return TRUE (no reply) | `CFileDB.cpp:1073-1078` |

## 6. Game Mechanics & Business Logic

### Rule 1: Billing gating before character selection
The handler implements a state machine driven by `pUser[conn].Unk_1816` (billing/account state, `CUser.h:76`) and `BILLING`/`FREEEXP` config (`Server.cpp:320,43,917-919`).
- If `BILLING != 2` the whole billing/child branch is skipped (`goto Label1`) — forward proceeds directly.
- `Unk_1816 == 3` → account not allowed to pay (child), signal 404 + localized block message.
- `Unk_1816 == 4` → using another server group.
- Forwarding to DBSrv only happens when: `BILLING != 2` **OR** `Unk_2728 != 1` **OR** `Level < FREEEXP` **OR** hour outside `7 < g_Hour < 23` — i.e. **during the "child-pay window" (08:00–22:00) with an eligible paid account, login is refused with `_NN_Child_Pay`** (`:69-91`). `Unk_2728` (`CUser.h:99`) is "related to BILLING (Child!?!)".
- `Unk_1816 <= 1` and a stale `Unk_2732` (`CUser.h:100`, "ReqBillSec") older than 10 s re-arms billing with `Unk_1816 = 5`; otherwise a billing poll is issued via `SendBilling(conn, AccountName, 1, 1)` (`:97-105`; `SendBilling` is a **no-op stub** at `Server.cpp:1377-1380`).

### Rule 2: Character selection slot
`Slot` (0–3) selects which of the up to 4 characters in the account to load. The handler validates it against `SelChar.Score[m->Slot].Level` (`STRUCT_SELCHAR`, `Basedef.h:765-778`) for billing decisions; DBSrv re-validates against `MOB_PER_ACCOUNT` and loads `File.Char[Slot]` (`CFileDB.cpp:1052,1062`). `pAccountList[Idx].Slot = Slot` is recorded for subsequent saves (`CFileDB.cpp:1052`).

### Rule 3: DBSrv character materialization
On success DBSrv copies from the in-memory account file into `MSG_CNFCharacterLogin`:
- `mob = File.Char[Slot]` (816-byte `STRUCT_MOB`), `Donate`, `ShortSkill[Slot]` (16 bytes), `affect[Slot]` (32×`STRUCT_AFFECT`), `mobExtra[Slot]` (`STRUCT_MOBEXTRA`) (`CFileDB.cpp:1062-1067`).
- Sets `Type=0x0417`, `ID = m->ID` (client conn), `Size = 2616` (`CFileDB.cpp:1056-1060`).
- Post-send ranking: registers the connection for grind ranking and sends an individual rank update keyed by mob name (`CFileDB.cpp:1083-1092`).

### Rule 4: TMSrv re-target & world entry (`ProcessDBMessage.cpp:635-1081`)
- `conn = std->ID` is the client slot; `conn == 0` is treated as spoof → `CrackLog`+`CloseUser` (`:54,643-650`).
- Item-effect sanitization: weapon-position items (`nPos` 64/192) with `EF_DAMAGE2`/`EF_DAMAGEADD` are normalized to `EF_DAMAGE` (equip + carry) (`:654-696`); if `evDelete`, carry `sIndex` in [470,500] are nulled (`:698-705`).
- Class-master equipment fixups on `Equip[0]` (98/106 effects) for `MORTAL`/`ARCH`/`CELESTIAL` family (`:706-726`).
- Populates `pMob[conn]` (mob, extra, affect, ShortSkill, MaxCarry 30 +15 per `Carry[60/61].sIndex==3467`), `pUser[conn].Donate`, resets session timers/counters, computes HP/MP/score bonuses via `BASE_GetHpMp`/`GetCurrentScore`/`BASE_GetBonus*` (`:728-780`).
- Spawn placement: city spawn from `Merchant>>6` zone, guild charge-zone spawn, newbie (Level<FREEEXP, MORTAL) fixed ~(2112,2042), then `GetEmptyMobGrid` (`:848-887`); failure → log + `CloseUser` (`:889-896`).
- Sets `Mode = USER_PLAY`, mounts (`MountProcess`), broadcasts `MSG_CreateMob` (CreateType 2) via `GridMulticast`, `SendPKInfo`, `SendGridMob`, `SendWarInfo`, castle state, `SendEtc`/`SendScore` (`:906-1076`).
- Experience-segment initialization vs `g_pNextLevel`/`g_pNextLevel_2`, and optional `SendBilling(…,1,1)` when `Level >= FREEEXP && CHARSELBILL == 0` (`:1015-1054`).
- Log `"sta,Login char:%s exp:%llu level:%d conn:%d money:%d, store:%d"` (`:1078-1080`).

## 7. Side Effects

| Effect | Target | Mechanism | Location |
|---|---|---|---|
| Mode transition `USER_SELCHAR → USER_CHARWAIT` | `pUser[conn].Mode` | set directly | `_MSG_CharacterLogin.cpp:76` |
| Mob mode `MOB_USER` | `pMob[conn].Mode` | set directly | `_MSG_CharacterLogin.cpp:77` |
| Clear merchant flag | `pMob[conn].MOB.Merchant = 0` | set directly | `_MSG_CharacterLogin.cpp:79` |
| DB forward `_MSG_DBCharacterLogin` (0x0804, 20B) | DBSrv | `DBServerSocket.SendOneMessage` (Type/ID rewritten) | `_MSG_CharacterLogin.cpp:73-81` |
| Billing-state mutation | `pUser[conn].Unk_2732/Unk_1816` | set directly | `_MSG_CharacterLogin.cpp:99-100` |
| Outgoing localized messages | client | `SendClientMessage` (`_NN_SelectCharacter`, `_DN_Not_Allowed_Account`, `_NN_Using_Other_Server_Group`, `_NN_Wait_Checking_Billing`, `_NN_Child_Pay`, `"Wait a moment."`) | `_MSG_CharacterLogin.cpp:27,38,47,57,62,66,85,91,104` |
| Client signal 404 | client | `SendClientSignalParm(conn, 0, 404, 0)` + flush | `_MSG_CharacterLogin.cpp:41-42` |
| Billing poll | billing | `SendBilling(conn, AccountName, 1, 1)` (no-op stub) | `_MSG_CharacterLogin.cpp:105` |
| Log `"err,charlogin not user_selchar %d %d"` | log | `Log` | `_MSG_CharacterLogin.cpp:86-87` |
| Account slot recorded | `pAccountList[Idx].Slot` | set directly | `CFileDB.cpp:1052` |
| DBSrv reply `_MSG_DBCNFCharacterLogin` (0x0417, 2616B) | TMSrv | `pUser[conn].cSock.SendOneMessage` | `CFileDB.cpp:1054-1080` |
| Logs `"err,charlogin slot illegal"`, `"err,charlogin secure illegal"`, `"err,charlogin mobname empty"` | log | `Log` | `CFileDB.cpp:1038,1047,1075` |
| Grind-ranking conn registration + individual rank send | ranking | `rankingSystem.grindRanking…` | `CFileDB.cpp:1083-1092` |
| Client re-target `_MSG_CNFCharacterLogin` (0x0114, 2608B) | client | `pUser[conn].cSock.SendOneMessage` | `ProcessDBMessage.cpp:787-984` |
| Mode transition → `USER_PLAY` | `pUser[conn].Mode` | set directly | `ProcessDBMessage.cpp:906` |
| World entry broadcasts | peers | `GridMulticast(MSG_CreateMob)`, `SendPKInfo`, `SendGridMob`, `SendWarInfo`, `SendEtc`, `SendScore`, `MountProcess`, castle state | `ProcessDBMessage.cpp:990-1076` |
| Log `"sta,Login char:…"` | log | `Log` | `ProcessDBMessage.cpp:1078-1080` |
| Fail handling `_MSG_DBCharacterLoginFail` | client | `SendClientSignal(_MSG_CharacterLoginFail)`; `Mode = USER_SELCHAR`; `CrackLog` | `ProcessDBMessage.cpp:1084-1096` |

## 8. Related Packets

| Packet | Constant | Value | Direction | Role |
|---|---|---|---|---|
| `_MSG_CharacterLogin` | `(19 \| FLAG_CLIENT2GAME)` | `0x0213` | C→G | This packet (`MSG_CharacterLogin`, 20B) |
| `_MSG_DBCharacterLogin` | `(4 \| FLAG_GAME2DB)` | `0x0804` | G→DB | TMSrv forwarding of this packet (same struct) |
| `_MSG_DBCNFCharacterLogin` | `(23 \| FLAG_DB2GAME)` | `0x0417` | DB→G | Success reply (`MSG_CNFCharacterLogin`, 2616B, `Basedef.h:1345`) |
| `_MSG_CNFCharacterLogin` | `(20 \| FLAG_GAME2CLIENT)` | `0x0114` | G→C | Trimmed reply re-targeted to client (`MSG_CNFClientCharacterLogin`, 2608B, `Basedef.h:1368`) |
| `_MSG_CharacterLoginFail` | `(25 \| FLAG_GAME2CLIENT)` | `0x0119` | G→C | Client-side fail signal (`MSG_STANDARD`) |
| `_MSG_DBCharacterLoginFail` | `(28 \| FLAG_DB2GAME)` | `0x041C` | DB→G | Fail signal; **handler exists, no DBSrv producer** (`MSG_STANDARD`) |
| `_MSG_AccountLogin` | `(13 \| FLAG_CLIENT2GAME)` | `0x020D` | C→G | Prior step; must have completed to reach `USER_SELCHAR` |
| `_MSG_DBCNFAccountLogin` | `(22 \| FLAG_DB2GAME)` | `0x0416` | DB→G | Account-login success that puts client into `USER_SELCHAR` |

## 9. Discrepancies & Open Questions

- **`Force` is never read.** The `MSG_CharacterLogin.Force` field (`Basedef.h:1341`) is declared and transmitted (offset 16) but no handler (TMSrv or DBSrv) references it. Purpose unknown.
- **No size validation on receive.** `Exec_MSG_CharacterLogin` never checks `m->Size`; it casts and reads `Slot` regardless of frame length and re-sends a fixed 20 bytes. A malformed (oversized/undersized) frame is silently reinterpreted. `BASE_CheckPacket` (the only size gate) is disabled.
- **`_MSG_DBCharacterLoginFail` has no producer.** Its TMSrv handler (`ProcessDBMessage.cpp:1084-1096`) exists, but the DBSrv `_MSG_DBCharacterLogin` case never sends a fail reply — on slot/secure/name failure it only logs and breaks, leaving the client stuck in `USER_CHARWAIT` with no confirmation. Possible dead path, or a client-side timeout is expected.
- **Dead condition in the billing gate.** `_MSG_CharacterLogin.cpp:53-54`: `Level >= FREEEXP && Level < 999 && BILLING == 3 && Level >= 1000` — `Level < 999` and `Level >= 1000` are mutually exclusive, so the body is unreachable.
- **Inconsistent inline layout comments in `Basedef.h`.** The `STRUCT_ACCOUNTFILE` comments (`Basedef.h:842-851`, e.g. "3480 - 4504") are consistent with `STRUCT_ITEM=8`/`MAX_CARGO=128`/`STRUCT_MOB=816` (verified by compiler) — but such offset comments should be re-checked if structs change. No impact on `MSG_CharacterLogin` (20B, primitive-only).
- **`SendBilling` is a stub** (`Server.cpp:1377-1380` returns `TRUE`), so the `SendBilling(conn, AccountName, 1, 1)` calls at `_MSG_CharacterLogin.cpp:105` and `ProcessDBMessage.cpp:1051` perform no network I/O in this build.
- **`STRUCT_MOB`/`STRUCT_MOBEXTRA` sizes assume MSVC LP32** (`long`=4, `time_t`=4). The response-struct sizes (2616/2608) were computed under that assumption and match the code's `sizeof` usage; they are presented for reference only — the request packet itself is unaffected.

## 10. Source References

**Constants / structs**
- `Basedef.h:71` (`MOB_PER_ACCOUNT`), `170-172` (`ESCENE_FIELD`, `SKIPCHECKTICK`), `925-941` (`_MSG` macro, `FLAG_*`), `979` (`_MSG_DBCharacterLogin`), `1111` (`_MSG_DBCNFCharacterLogin`), `1116` (`_MSG_DBCharacterLoginFail`), `1335-1342` (`_MSG_CharacterLogin`, `MSG_CharacterLogin`), `1344-1386` (`_MSG_CNFCharacterLogin`, `MSG_CNFCharacterLogin`, `MSG_CNFClientCharacterLogin`), `1392` (`_MSG_CharacterLoginFail`), `398-412` (`STRUCT_ITEM`), `414-436` (`STRUCT_SCORE`), `438-481` (`STRUCT_MOB`), `483-591` (`STRUCT_MOBEXTRA`), `593-599` (`STRUCT_AFFECT`), `765-778` (`STRUCT_SELCHAR`).

**TMSrv**
- `ProcessClientMessage.cpp:42-74` (dispatcher guards + case).
- `_MSG_CharacterLogin.cpp:21-109` (handler).
- `ProcessDBMessage.cpp:39-64` (DB guard + `conn=std->ID`), `635-1081` (`_MSG_DBCNFCharacterLogin`), `1084-1096` (`_MSG_DBCharacterLoginFail`).
- `SendFunc.cpp:27-45` (`SendClientMessage`), `197-218` (`SendClientSignal`, `SendClientSignalParm`), `560` (`SendGridMob`), `1136` (`SendScore`), `1195` (`SendEtc`), `1416` (`SendWarInfo`), `1737` (`SendPKInfo`).
- `Server.cpp:43` (`FREEEXP`), `320` (`BILLING`), `917-919` (config), `1377-1380` (`SendBilling` stub).
- `CUser.h:76,99,100` (`Unk_1816`, `Unk_2728`, `Unk_2732`), `27-37` (USER_* modes).
- `Language.h:54,58-60,226` (`_NN_SelectCharacter`, `_NN_Wait_Checking_Billing`, `_DN_Not_Allowed_Account`, `_NN_Using_Other_Server_Group`, `_NN_Child_Pay`).
- `Basedef.cpp:6497,6564` (`BASE_CheckPacket`, disabled).

**DBSrv**
- `Server.cpp:1339-1357` (`ProcessClientMessage` guard + dispatch), `946` (WSA_READ → `ProcessClientMessage`).
- `CFileDB.cpp:1028-1094` (`_MSG_DBCharacterLogin`), `2328-2333` (`GetIndex(server,id)`), `1054-1080` (reply build/send), `1083-1092` (ranking).

**Size verification helper**
- Sizes computed via a standalone C model replicating MSVC `/Zp8` + LP32 (`long`/`time_t` = 4); `MSG_CharacterLogin` = 20B confirmed against `sizeof` usage in handler and `BASE_CheckPacket`.
