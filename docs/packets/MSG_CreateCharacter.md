# MSG_CreateCharacter

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_CreateCharacter` = `(15 \| FLAG_CLIENT2GAME)` = `0x020F` = `527` |
| Sequence ID | 15 |
| Direction(s) | Client → TMSrv (`FLAG_CLIENT2GAME`); TMSrv forwards to DBSrv as `_MSG_DBCreateCharacter`; DBSrv replies `_MSG_DBCNFNewCharacter` (success) or `_MSG_DBNewCharacterFail` (failure) → TMSrv → client as `_MSG_CNFNewCharacter` (success) or `_MSG_NewCharacterFail` (failure) |
| Wire struct | `MSG_CreateCharacter` (dedicated struct, `Basedef.h:1260-1267`) |
| Total size | 36 bytes (fixed; `sizeof(MSG_CreateCharacter)`), no variable-length tail |
| Packing | **Default MSVC `/Zp8`** — NOT in any `#pragma pack(push,1)` region (pack(1) regions: `Basedef.h:808-835`, `1212-1246`, `1465-1492`, `2063-2097`; this struct at 1260-1267 falls outside all of them) |
| Handler | `Exec_MSG_CreateCharacter` @ `TMSrv/_MSG_CreateCharacter.cpp:20` |
| Aliases | Low byte 15 is shared by `_MSG_CreateCharacter` (0x020F), `_MSG_DBCNFArchCharacterFail` (`15\|FLAG_DB2GAME`=`0x040F`, `Basedef.h:1027`), and `_MSG_DBSendItem` (`15\|GAME2DB\|DB2GAME`=`0x0C0F`, `Basedef.h:1282`). The `MSG_CreateCharacter` struct is shared verbatim by its DB-mirror `_MSG_DBCreateCharacter` (same wire layout, `Type` rewritten — `_MSG_DBCreateCharacter` declares **no** dedicated struct, `Basedef.h:1151`). |
| Related | `_MSG_DBCreateCharacter` `(2\|FLAG_GAME2DB)`=`0x0802` (reuses `MSG_CreateCharacter`); `_MSG_DBCNFNewCharacter` `(24\|FLAG_DB2GAME)`=`0x0418` (struct `MSG_CNFNewCharacter`); `_MSG_DBNewCharacterFail` `(29\|FLAG_DB2GAME)`=`0x041D`; `_MSG_CNFNewCharacter` `(16\|FLAG_GAME2CLIENT)`=`0x0110` (struct `MSG_CNFNewCharacter`); `_MSG_NewCharacterFail` `(26\|FLAG_GAME2CLIENT)`=`0x011A` (`Basedef.h:1393`) |

> **Note (prompt vs. source):** the command brief stated `_MSG_DBNewCharacterFail = (26 \| FLAG_DB2GAME) = 0x41A`. The **source wins**: `Basedef.h:1117` defines `_MSG_DBNewCharacterFail = (29 \| FLAG_DB2GAME) = 0x041D`. Low byte 26 is actually `_MSG_DBNewAccountFail` (`Basedef.h:1114`). See §9.

## 2. Wire Framing

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with 4-byte `INITCODE = 0x1F11F311` magic before any framed message (`CPSock.cpp:366-383`).
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating XOR transform keyed by `KeyWord` (index into shared `pKeyWord[512]`) — modulo-4 transform per byte offset (`CPSock.cpp:558-581` send, `430-453` receive).
- `CheckSum` = `Sum2 - Sum1` (raw vs. transformed payload sums); validated on receive (`CPSock.cpp:583-584`, `455-464`).
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` (8192) else the receive buffer is reset (`CPSock.cpp:397-406`).
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is disabled (body commented out, returns `FALSE`) — no central size validation in release.

Per-packet notes:
- No deviation from standard framing. Plain client→game packet carried on the client socket to TMSrv.
- `Type = 0x020F` on the wire from the client; **rewritten to `_MSG_DBCreateCharacter` (0x0802)** by the handler before DB forwarding, and `ID` is rewritten to the client `conn` (`_MSG_CreateCharacter.cpp:38-39`).
- The client-side `Size` is expected to be `sizeof(MSG_CreateCharacter)` = 36. **The handler performs NO size validation** — it casts to `MSG_CreateCharacter` and only reads `Slot`/`MobName`/`MobClass`; the frame is re-sent as exactly 36 bytes to DBSrv (`_MSG_CreateCharacter.cpp:43`).
- The DB mirror `_MSG_DBCreateCharacter` (0x0802) carries the identical 36-byte payload (same struct, `Type` rewritten).
- The DBSrv success reply `_MSG_DBCNFNewCharacter` (0x0418) uses the much larger `MSG_CNFNewCharacter` struct (856 bytes) carrying a full `STRUCT_SELCHAR`; TMSrv re-targets it to the client unchanged with `Type = _MSG_CNFNewCharacter` (0x0110).
- Failure replies (`_MSG_DBNewCharacterFail` 0x041D from DBSrv; `_MSG_NewCharacterFail` 0x011A from TMSrv) are `MSG_STANDARD` (12 bytes) signals.

## 3. Binary Layout

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 36) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_CreateCharacter` = 0x020F (0x0802 after TMSrv rewrite) |
| 6 | 2 | `ID` | `short` | Client connection slot (sender-stamped; TMSrv overwrites with `conn` before DB forward) |
| 8 | 4 | `ClientTick` | `unsigned int` | Client tick; must not equal `SKIPCHECKTICK` (235543242, `Basedef.h:172`) |

### 3.2 Payload

Packing context: **default `/Zp8`** (not a pack(1) region — see §1). Each member aligned to `min(sizeof(member), 8)`. Header ends at offset 12 (already 4-aligned), so no leading padding. The only embedded struct is the `_MSG` header macro (primitives); the payload members are `int`/`char[]` with no `STRUCT_*` types, so no nested alignment is introduced.

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 4 | `Slot` | `int` | 4 | 0 | 0-based character slot to fill. Must be `>= 0 && < MOB_PER_ACCOUNT` (4) (`CFileDB.cpp:865`) and the slot must currently be **empty** (`mob->MobName[0] == 0`, `CFileDB.cpp:927`) |
| 16 | 16 | `MobName` | `char[NAME_LENGTH]` | 1 | 0 | Desired character name, NUL-terminated, `NAME_LENGTH` = 16 (`Basedef.h:132`). Handler force-NULs `[14]` and `[15]` (`_MSG_CreateCharacter.cpp:24-25`). Validated by `BASE_CheckValidString` (length 4..15) on the TMSrv side |
| 32 | 4 | `MobClass` | `int` | 4 | 0 | Starting class index. Must be `>= 0 && < 4` (`CFileDB.cpp:873`). 0=TK, 1=FM, 2=BM, 3=HT (see §6) |

**Total payload: 24 bytes → total struct 36 bytes.**

### 3.3 Nested struct expansions

`MSG_CreateCharacter` embeds only the `_MSG` header macro (primitive fields) plus `int Slot`, `char MobName[16]`, `int MobClass`. It contains **no `STRUCT_*` members**, so there are no nested expansions in the request struct itself.

For reference, the success response struct `MSG_CNFNewCharacter` (`Basedef.h:1301-1306`) embeds `STRUCT_SELCHAR`; sizes computed under MSVC `/Zp8`, LP32 (`int`=4, `long long`=8):

**`STRUCT_ITEM`** (`Basedef.h:398-412`) — align 2, size 8:
| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 2 | `sIndex` | `short` |
| 2 | 6 | `stEffect[3]` | `union` (`short sValue`, or `uchar cEffect`+`uchar cValue`) |

**`STRUCT_SCORE`** (`Basedef.h:414-436`) — align 4, size 48:
| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 4 | `Level` | `int` |
| 4 | 4 | `Ac` | `int` |
| 8 | 4 | `Damage` | `int` |
| 12 | 1 | `Merchant` | `uchar` |
| 13 | 1 | `AttackRun` | `uchar` |
| 14 | 1 | `Direction` | `uchar` |
| 15 | 1 | `ChaosRate` | `uchar` |
| 16 | 4 | `MaxHp` | `int` |
| 20 | 4 | `MaxMp` | `int` |
| 24 | 4 | `Hp` | `int` |
| 28 | 4 | `Mp` | `int` |
| 32 | 2 | `Str` | `short` |
| 34 | 2 | `Int` | `short` |
| 36 | 2 | `Dex` | `short` |
| 38 | 2 | `Con` | `short` |
| 40 | 8 | `Special[4]` | `short[4]` |

**`STRUCT_SELCHAR`** (`Basedef.h:765-778`) — align 8 (`long long`), size 840:
| Offset | Size | Field | Type | Align | Pad |
|---|---|---|---|---|---|
| 0 | 8 | `SPX[4]` | `short[4]` | 2 | 0 |
| 8 | 8 | `SPY[4]` | `short[4]` | 2 | 0 |
| 16 | 64 | `Name[4][16]` | `char[4][16]` | 1 | 0 |
| 80 | 192 | `Score[4]` | `STRUCT_SCORE[4]` | 4 | 0 |
| 272 | 512 | `Equip[4][16]` | `STRUCT_ITEM[4][16]` | 2 | 0 |
| 784 | 8 | `Guild[4]` | `unsigned short[4]` | 2 | 0 |
| 792 | 16 | `Coin[4]` | `int[4]` | 4 | 0 |
| 808 | 32 | `Exp[4]` | `long long[4]` | 8 | 0 |

**`MSG_CNFNewCharacter`** (`Basedef.h:1301-1306`) — align 8, size **856**:
| Offset | Size | Field | Type | Align | Pad |
|---|---|---|---|---|---|
| 0 | 12 | `_MSG` header | macro | 4 | 0 |
| 12 | 4 | *(padding)* | — | — | 4 (to 8-align `sel`) |
| 16 | 840 | `sel` | `STRUCT_SELCHAR` | 8 | 0 |

### 3.4 Size verification

Offset math for `MSG_CreateCharacter` (`/Zp8`, no padding in this struct):
```
Header (_MSG):                   0 + 12               = 12
Slot (int, align 4)             12 +  4               = 16
MobName (char[16], align 1)     16 + 16               = 32
MobClass (int, align 4)         32 +  4               = 36
sizeof(MSG_CreateCharacter)                             = 36
```
- Expected `Size` header value: **36**.
- Cross-checks: the TMSrv handler forwards exactly `sizeof(MSG_CreateCharacter)` = 36 bytes to DBSrv (`_MSG_CreateCharacter.cpp:43`). The DBSrv reads `Slot`/`MobClass`/`MobName` within that window (`CFileDB.cpp:860-861,893`) and re-sends the account `STRUCT_ACCOUNTFILE` via `DBWriteAccount` which `_write`s `sizeof(STRUCT_ACCOUNTFILE)` bytes (`CFileDB.cpp:2444`). **No mismatch.**
- DBSrv success reply size: `pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFNewCharacter))` = 856 (`CFileDB.cpp:1024`). TMSrv re-targets it to the client as the same 856 bytes (`ProcessDBMessage.cpp:586`). **No mismatch.**
- Failure replies are `MSG_STANDARD` (12 bytes): `SendDBSignal` builds `sizeof(sm)` = 12 (`CFileDB.cpp:2114-2117`); TMSrv `SendClientSignal` builds `sizeof(MSG_STANDARD)` = 12 (`SendFunc.cpp:199-206`).
- No variable-length tail. No unknown/reserved members in the request.

## 4. Lifecycle & Flow

Direction: **Client → TMSrv → DBSrv → TMSrv → Client**.

```
[Client]                                 [TMSrv]                                        [DBSrv]
   |  MSG_CreateCharacter (0x020F, 36B)      |                                                  |
   |  Slot, MobName, MobClass; Tick!=SKIP    |                                                  |
   |---------------------------------------->|                                                  |
   |   Dispatcher guards (ID in [0,MAX_USER),|                                                  |
   |   ServerDown<120, Tick!=SKIPCHECKTICK)  |                                                  |
   |   ProcessClientMessage:84 -> Exec_MSG_CreateCharacter:20                                   |
   |    Mode==USER_SELCHAR? (else fail)      |                                                  |
   |    BASE_CheckValidString(MobName)?      |                                                  |
   |    rewrite Type=0x0802, ID=conn         |                                                  |
   |    Mode=USER_WAITDB (:41)               |                                                  |
   |    DBServerSocket.SendOneMessage(:43) = 36B                                                 |
   |                                         |------------------------------------------------->|
   |                                         |   DBSrv Server.cpp:1339 ProcessClientMessage       |
   |                                         |   (guard: Type&FLAG_GAME2DB, ID in [0,MAX_USER])  |
   |                                         |   -> cFileDB.ProcessMessage(msg, conn) -> CFileDB.cpp:856
   |                                         |   case _MSG_DBCreateCharacter                     |
   |                                         |   validations (see §5)                             |
   |                                         |   build STRUCT_MOB from g_pBaseSet[cls],          |
   |                                         |   name, mobExtra, persist DBWriteAccount           |
   |                                         |   success: MSG_CNFNewCharacter sel via            |
   |                                         |     DBGetSelChar, Type=0x0418, ID=client conn,    |
   |                                         |     SendOneMessage 856B (:1016-1024)              |
   |                                         |   failure: SendDBSignal _MSG_DBNewCharacterFail   |
   |                                         |     (0x041D, 12B)                                 |
   |                                         |<-------------------------------------------------|
   |                                         |  ProcessDBMessage (conn = std->ID = client slot) |
   |                                         |  case _MSG_DBCNFNewCharacter :579                 |
   |                                         |   rewrite Type=0x0110, ID=ESCENE_FIELD+1          |
   |                                         |   SendOneMessage 856B (:586), Mode=USER_SELCHAR   |
   |                                         |  case _MSG_DBNewCharacterFail :622                |
   |                                         |   SendClientSignal _MSG_NewCharacterFail          |
   |                                         |   (0x011A, 12B), Mode=USER_SELCHAR                |
   |  MSG_CNFNewCharacter (0x0110, 856B)     |                                                  |
   |<----------------------------------------|                                                  |
   |  or _MSG_NewCharacterFail (0x011A,12B)  |                                                  |
```

Detailed notes:
- **Dispatch chain:** client socket → `CPSock.ReadMessage` (deobfuscate, validate checksum/size) → `Server.cpp` WSA_READ → `ProcessClientMessage(conn, pMsg, FALSE)` (`ProcessClientMessage.cpp:38`) → `case _MSG_CreateCharacter` (`ProcessClientMessage.cpp:84-86`) → `Exec_MSG_CreateCharacter` (`_MSG_CreateCharacter.cpp:20`).
- **DB-side dispatch:** DBSrv receives the GAME2DB frame, guards `Type & FLAG_GAME2DB` and `ID in [0,MAX_USER]` (`DBSrv/Server.cpp:1343-1354`), then `cFileDB.ProcessMessage(msg, conn)` (`Server.cpp:1356`) → `case _MSG_DBCreateCharacter` (`CFileDB.cpp:856`).
- **Success:** DBSrv builds `MSG_CNFNewCharacter sm`, sets `sm.Type = _MSG_DBCNFNewCharacter`, `sm.ID = m->ID` (client conn), fills `sm.sel` via `DBGetSelChar`, and sends 856B back to the game server (`CFileDB.cpp:1016-1024`). TMSrv `ProcessDBMessage` derives `conn = std->ID` (the client conn, `ProcessDBMessage.cpp:54`), rewrites `Type = _MSG_CNFNewCharacter`, `ID = ESCENE_FIELD + 1`, sends 856B to the client, and returns the session to `USER_SELCHAR` (`ProcessDBMessage.cpp:579-592`).
- **Failure:** DBSrv sends `_MSG_DBNewCharacterFail` (0x041D) via `SendDBSignal(conn, m->ID, ...)` (12B). TMSrv maps it to client `_MSG_NewCharacterFail` (0x011A) and restores `USER_SELCHAR` (`ProcessDBMessage.cpp:622-632`).

## 5. Validation & Guards

Execution order (TMSrv then DBSrv):

| # | Check | Location | On failure |
|---|---|---|---|
| 1 | Dispatcher: `std->ID in [0, MAX_USER)` | `ProcessClientMessage.cpp:42-51` | log + drop |
| 2 | Dispatcher: `ServerDown < 120` | `ProcessClientMessage.cpp:53-54` | drop |
| 3 | Dispatcher: `isServer==FALSE && ClientTick != SKIPCHECKTICK` | `ProcessClientMessage.cpp:63-64` | drop |
| 4 | Session mode `== USER_SELCHAR` | `_MSG_CreateCharacter.cpp:27-34` | log `err,createchar not user_selchar`, `SendClientSignal _MSG_NewCharacterFail` |
| 5 | Name valid per `BASE_CheckValidString` (length 4..15; chars `[A-Za-z0-9-]` or multibyte; not reserved words Reino/subcreate/create/gritar/king/kingdom/getout/gfame/expulsar/summonguild/summon/time/relo/stopally/stopwar) | `_MSG_CreateCharacter.cpp:36`; `Basedef.cpp:2522-2551` | `SendClientSignal _MSG_NewCharacterFail` |
| 6 | DB: slot `in [0, MOB_PER_ACCOUNT)` | `CFileDB.cpp:865` | log + `_MSG_DBNewCharacterFail` |
| 7 | DB: class `in [0, 4)` | `CFileDB.cpp:873` | log + `_MSG_DBNewCharacterFail` |
| 8 | DB: account `SecurePass != -1` (account authenticated/established) | `CFileDB.cpp:882-889` | log + `break` (no reply) |
| 9 | DB: name not reserved **KING/KINGDOM/GRITAR/RELO** (case-insensitive) | `CFileDB.cpp:891-904` | log + `_MSG_DBNewCharacterFail` |
| 10 | DB: name not of form **COM# / LPT#** (reserved Windows device prefixes) | `CFileDB.cpp:906-914` | log + `_MSG_DBNewCharacterFail` |
| 11 | DB: target slot currently empty (`Char[Slot].MobName[0] == 0`) — enforces MOB_PER_ACCOUNT=4 capacity | `CFileDB.cpp:927-935` | log + `_MSG_DBNewCharacterFail` |
| 12 | DB: no `íí` byte sequence in name | `CFileDB.cpp:940-950` | `_MSG_DBNewCharacterFail` |
| 13 | DB: `CreateCharacter(acct, name)` — reserves name via `./char/<First>/<name>` file; fails if name already exists (EEXIST) | `CFileDB.cpp:952-959`, `2236-2301` | log + `_MSG_DBNewCharacterFail` |
| 14 | DB: `DBWriteAccount` succeeds (persist full account file) | `CFileDB.cpp:999-1008`, `2390-2469` | log + `_MSG_DBNewCharacterFail` |

Notes:
- The **name-length/format** check (`BASE_CheckValidString`, length 4..15) runs **only** on the TMSrv side; the DBSrv trusts it and does not re-validate length. A client that bypasses TMSrv could still be gated by the DB's reserved-word / COM/LPT / file-existence checks.
- There is **no creation fee / coin deduction** anywhere in this flow (checked both `_MSG_CreateCharacter.cpp` and `CFileDB.cpp` create case).
- `BASE_CheckValidString` treats a negative (high-bit, multibyte) lead byte by skipping the next byte and continuing — allowing multibyte names.

## 6. Game Mechanics & Business Logic

Character creation is a **name-slot-class** assignment backed by the pre-baked base mob set; there is no stat/score computation in the handler. The mechanics follow `CFileDB.cpp:856-1026`:

1. **Class mapping:** `cls` 0..3 selects `g_pBaseSet[0..3]` (`CFileDB.cpp:973-983`). These are loaded at DBSrv boot from `./BaseMob/TK`, `./BaseMob/FM`, `./BaseMob/BM`, `./BaseMob/HT` (`DBSrv/Server.cpp:450-503`), i.e. **TK (TransKnight), FM (Foema), BM (BeastMaster), HT (Huntress)**. Each `STRUCT_MOB` is memcpy'd wholesale into `pAccountList[Idx].File.Char[Slot]` (`CFileDB.cpp:974-983`) — so starting `STRUCT_SCORE` (stats/HP/MP), `Equip`, `Carry`, `Coin`, `Exp`, and `SPX/SPY` all come verbatim from the base file. `g_pBaseSet[i].BaseScore = g_pBaseSet[i].CurrentScore` is set at load (`DBSrv/Server.cpp:505-508`).
2. **Pre-clearing:** the slot mob and `mobExtra[Slot]` are cleared via `BASE_ClearMob` / `BASE_ClearMobExtra` (`CFileDB.cpp:964-965`; `Basedef.cpp:2943-2971`), the slot's `affect` array is zeroed and `ShortSkill[Slot]` filled with `-1` (`CFileDB.cpp:967-969`). (`BASE_ClearMob` sets `SPX=SPY=2112` but is then overwritten by the base-mob memcpy, so actual start position comes from the base file.)
3. **Class master:** `extra->ClassMaster = MORTAL` (2, `Basedef.h:178`) regardless of chosen class (`CFileDB.cpp:971`); `BASE_ClearMobExtra` also defaults to `MORTAL` (`Basedef.cpp:2969`). New characters are always Mortal-tier.
4. **Face:** `extra->MortalFace = mob->Equip[0].sIndex` (the base mob's equipment-slot-0 item index) (`CFileDB.cpp:995`). `DBGetSelChar` later maps face indices 22/23/24/25/32 back to a derived face for the select screen (`CFileDB.cpp:2618-2619`).
5. **Name assignment:** `memcpy(mob->MobName, m->MobName, NAME_LENGTH)` (`CFileDB.cpp:997`), with `[14]`/`[15]` force-NUL'd.
6. **Persistence:** `DBWriteAccount(&pAccountList[Idx].File)` writes the full `STRUCT_ACCOUNTFILE` to `./account/<First>/<ACCOUNTNAME>` (`CFileDB.cpp:999`, `2390-2469`).
7. **Name reservation:** `CreateCharacter` first probes `./char/<First>/<name>`; if it exists the name is taken (EEXIST → fail); otherwise it creates that file and writes the account name into it (`CFileDB.cpp:2236-2301`), so character names are unique server-wide across all accounts.
8. **Select-screen reply:** on success the DB replies with a full `STRUCT_SELCHAR` snapshot of the 4 slots via `DBGetSelChar` (`CFileDB.cpp:2611-2631`), so the client immediately shows the new character.

**Capacity rule:** an account holds up to `MOB_PER_ACCOUNT` = 4 characters (`Basedef.h:71`); a slot can only be created if `Char[Slot].MobName[0] == 0` (`CFileDB.cpp:927`).

**No fees/coins deducted, no starting item/score math** — everything is inherited from the base mob file.

## 7. Side Effects

- **`pAccountList[Idx].File` mutation** (`CFileDB.cpp:918-998`): `Char[Slot]` overwritten with `g_pBaseSet[cls]` + name; `mobExtra[Slot]` set (ClassMaster=MORTAL, MortalFace, cleared QuestInfo); `affect[Slot]` zeroed; `ShortSkill[Slot]` filled with 0xFF; `Coin`/`Exp`/`Equip`/`Carry`/`BaseScore`/`CurrentScore`/`SPX`/`SPY` inherited from base mob.
- **Disk persistence:** `DBWriteAccount` rewrites `./account/<First>/<ACCOUNTNAME>` (`CFileDB.cpp:999`, `2411-2444`); `CreateCharacter` creates `./char/<First>/<name>` name-reservation file (`CFileDB.cpp:2263-2298`).
- **Logs** (all via `Log(msg, who, 0)`):
  - `_MSG_CreateCharacter.cpp:45` — `etc,createchar name:%s %d %d` (name, conn, Mode) — success path, TMSrv.
  - `_MSG_CreateCharacter.cpp:29` — `err,createchar not user_selchar %d %d` — wrong mode.
  - `CFileDB.cpp:867,875,886,899,909,931,989,1005` — `err,newchar slot/class/secure/cmd/com/already charged/undefined class/create file`.
  - `CFileDB.cpp:1012-1014` — `create character [%s]` (account context).
  - `CreateCharacter` errno logs `err createchar EEXIST/EACCES/EINVAL/EMFILE/ENOENT/UNKNOWN` (`CFileDB.cpp:2272-2294`).
- **Outgoing packets:**
  - Success: `MSG_CNFNewCharacter` (0x0418 → client 0x0110), 856B.
  - Failure: `_MSG_DBNewCharacterFail` (0x041D) → client `_MSG_NewCharacterFail` (0x011A), 12B.
  - Immediate TMSrv-side failure: `_MSG_NewCharacterFail` (0x011A) via `SendClientSignal`.
- **Mode transitions:** TMSrv `USER_SELCHAR` → `USER_WAITDB` on forward (`_MSG_CreateCharacter.cpp:41`); back to `USER_SELCHAR` on either DB reply (`ProcessDBMessage.cpp:588,630`).
- **Server-wide side effect:** the character name becomes globally reserved (the `./char/` file), blocking future creation of the same name from any account.

## 8. Related Packets

| Packet | Value | Struct | Direction | Role |
|---|---|---|---|---|
| `_MSG_DBCreateCharacter` | `0x0802` | `MSG_CreateCharacter` (reused) | TMSrv→DBSrv | DB mirror of the request |
| `_MSG_DBCNFNewCharacter` | `0x0418` | `MSG_CNFNewCharacter` | DBSrv→TMSrv | DB success ack |
| `_MSG_DBNewCharacterFail` | `0x041D` | `MSG_STANDARD` | DBSrv→TMSrv | DB failure ack |
| `_MSG_CNFNewCharacter` | `0x0110` | `MSG_CNFNewCharacter` | TMSrv→client | client success ack |
| `_MSG_NewCharacterFail` | `0x011A` | `MSG_STANDARD` | TMSrv→client | client failure ack |
| `_MSG_DeleteCharacter` | `0x0211` (`17\|CLIENT2GAME`) | `MSG_DeleteCharacter` | client→TMSrv→DBSrv | counterpart (delete) flow |
| `_MSG_DBCNFDeleteCharacter` / `_MSG_DBDeleteCharacterFail` | `0x0419` / `0x041E` | `MSG_CNFDeleteCharacter` / `MSG_STANDARD` | DBSrv→TMSrv | delete acks (parallel pattern) |
| `_MSG_CharacterLogin` | `0x0213` | `MSG_CharacterLogin` | client→TMSrv→DBSrv | subsequent login into a created slot |

## 9. Discrepancies & Open Questions

1. **`_MSG_DBNewCharacterFail` value:** the command brief asserted `(26 \| FLAG_DB2GAME) = 0x41A`, but `Basedef.h:1117` defines it as `(29 \| FLAG_DB2GAME) = 0x041D`. Low byte 26 is `_MSG_DBNewAccountFail` (`Basedef.h:1114`). All DB fail sends in the create case use `_MSG_DBNewCharacterFail` (`CFileDB.cpp:869,877,901,911,929,946,956,987,1003`), so 0x041D is the operative value. **Source wins over the brief.**
2. **Missing size validation:** neither `Exec_MSG_CreateCharacter` nor the DBSrv case validates the frame size — both cast blindly to `MSG_CreateCharacter` and re-emit a fixed 36 bytes. An undersized client frame could cause an over-read of the receive buffer up to 36 bytes on forward.
3. **`SecurePass == -1` path (`CFileDB.cpp:882-889`)** does a bare `break` with **no reply** — the client would hang until timeout rather than get a failure signal. Possibly a latent bug.
4. **`BASE_CheckValidString` only on TMSrv:** name-length/format validation lives entirely client-side-server; DBSrv does not re-check length, trusting TMSrv. A compromised/bespoke client reaching DBSrv directly (bypassing TMSrv) could create an oddly-named character (subject only to the DB reserved-word / COM-LPT / file-existence checks).
5. **Starting position:** not explicitly set in the create handler; it comes from the base mob file's `SPX/SPY`. If a base file has zero `SPX/SPY`, the character would spawn at (0,0) — the `BASE_ClearMob` default of 2112 is overwritten by the base-mob memcpy.
6. **No creation fee** — confirmed absent in both handler and DB case. If a fee was ever intended for creation, it is not implemented here.

## 10. Source References

- `legacy/Code/Basedef.h` — `_MSG` macro `:925-930`; flags `:932-941`; `_MSG_DBCNFNewCharacter` `:1112`; `_MSG_DBNewCharacterFail` `:1117`; `_MSG_DBCreateCharacter` `:1151`; `_MSG_CreateCharacter` + `MSG_CreateCharacter` `:1259-1267`; `_MSG_CNFNewCharacter` + `MSG_CNFNewCharacter` `:1300-1306`; `_MSG_NewCharacterFail` `:1393`; `STRUCT_ITEM` `:398-412`; `STRUCT_SCORE` `:414-436`; `STRUCT_SELCHAR` `:765-778`; `NAME_LENGTH` `:132`; `MOB_PER_ACCOUNT` `:71`; `MAX_CLASS` `:115`; `MORTAL` `:178`; `SKIPCHECKTICK` `:172`; `ESCENE_FIELD` `:170`; `MAX_USER` `:56`; `MAX_EQUIP/MAX_CARRY/MAX_CARGO` `:75-77`
- `legacy/Code/Basedef.cpp` — `BASE_CheckValidString` `:2522-2551`; `BASE_ClearMob` `:2943-2963`; `BASE_ClearMobExtra` `:2965-2971`; `BASE_GetFirstKey` `:4114+`
- `legacy/Code/TMSrv/_MSG_CreateCharacter.cpp` — handler `:20-50`
- `legacy/Code/TMSrv/ProcessClientMessage.cpp` — dispatcher + guards `:38-66`; `case _MSG_CreateCharacter` `:84-86`
- `legacy/Code/TMSrv/ProcessDBMessage.cpp` — dispatch `:39-54`; `case _MSG_DBCNFNewCharacter` `:579-592`; `case _MSG_DBNewCharacterFail` `:622-632`
- `legacy/Code/TMSrv/SendFunc.cpp` — `SendClientSignal` `:197-206`
- `legacy/Code/TMSrv/CUser.h` — `USER_SELCHAR` `:30`; `USER_WAITDB` `:32`
- `legacy/Code/DBSrv/CFileDB.cpp` — `case _MSG_DBCreateCharacter` `:856-1026`; `CreateCharacter` `:2236-2301`; `DBWriteAccount` `:2390-2469`; `DBGetSelChar` `:2611-2631`; `GetIndex` `:2328-2349`; `SendDBSignal` `:2108-2120`
- `legacy/Code/DBSrv/Server.cpp` — `ProcessClientMessage` `:1339-1357`; base-mob load `:450-508`
- `legacy/Code/CPSock.cpp` — framing `:353-467` (read), `:513-591` (AddMessage), `:686-688` (SendOneMessage)
- `legacy/Code/CPSock.h` — `INITCODE` `:40`; `MAX_MESSAGE_SIZE` `:38`
