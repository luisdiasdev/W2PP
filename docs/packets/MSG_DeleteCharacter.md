# MSG_DeleteCharacter

## 1. Summary

| Attribute | Value |
|---|---|
| Type constant | `_MSG_DeleteCharacter` |
| Base sequence | `17` (`0x11`) |
| Flags (OR'd) | `FLAG_CLIENT2GAME` (0x0200) |
| **Type value (authoritative)** | **`0x211` (529)** |
| Alias / dedicated struct | `MSG_DeleteCharacter` (Basedef.h:1270-1278) |
| Wire struct | `MSG_DeleteCharacter` |
| Wire struct size | **44 bytes** (`sizeof(MSG_DeleteCharacter)`), no padding (see §3.4) |
| Direction(s) | **CLIENT2GAME** (client → TMSrv) |
| Producer | Client (character-select screen) |
| Consumer | TMSrv `ProcessClientMessage` → `Exec_MSG_DeleteCharacter` (ProcessClientMessage.cpp:80-82) |
| DB mirror | `_MSG_DBDeleteCharacter` = `9 \| FLAG_GAME2DB` = **0x809** (Basedef.h:999), forwarded TMSrv→DBSrv |
| DB responses | `_MSG_DBCNFDeleteCharacter` = `25 \| FLAG_DB2GAME` = **0x419** (Basedef.h:1113); `_MSG_DBDeleteCharacterFail` = `30 \| FLAG_DB2GAME` = **0x41E** (Basedef.h:1118) |
| Client response | `_MSG_CNFDeleteCharacter` = `18 \| FLAG_GAME2CLIENT` = **0x112** (Basedef.h:1308) / `_MSG_DeleteCharacterFail` = `27 \| FLAG_GAME2CLIENT` = **0x11B** (Basedef.h:1394) |
| Related packets | `_MSG_CreateCharacter`, `_MSG_CNFNewCharacter`, `_MSG_DBCNFNewCharacter`, `_MSG_CharacterLogin`, `_MSG_AccountSecure` |

**Source:** `const short _MSG_DeleteCharacter = (17 | FLAG_CLIENT2GAME);` — Basedef.h:1269.
**Struct:** Basedef.h:1270-1278.

**Flag decode math:** `0x11 | 0x0200 = 0x211` (529). The DB mirror `0x9 | 0x0800 = 0x809` (2057); `0x19 | 0x0400 = 0x419` (1049); `0x1E | 0x0400 = 0x41E` (1054). All four constants **verified** against the embedded protocol reference.

## 2. Wire Framing (protocol preamble)

Standard CPSock framing applies — **no per-packet deviations**. Facts verified in `CPSock.cpp` / `CPSock.h` (authoritative reference, not re-derived here):

- **Magic:** `INITCODE = 0x1F11F311` (CPSock.h:40; compared at CPSock.cpp:373).
- **Obfuscation:** payload from offset 4 is XOR-obfuscated by a per-byte position-rotating key derived from `KeyWord` (CPSock.cpp:249, ReadMessage path; 535-540 send path). The `_MSG` header fields (`Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`) are not obfuscated.
- **CheckSum:** `CheckSum = Sum2 - Sum1` over the body (CPSock.cpp:583-584).
- **Size validation:** `Size` must satisfy `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE` (CPSock.cpp:397).
- **Billing:** the separate 196-byte plain billing protocol does not apply here.
- **BASE_CheckPacket** (Basedef.cpp:6475) is **disabled**, so no further checksum/replay gate at dispatch.

Total on-the-wire message: 4-byte `INITCODE` + 44-byte obfuscated `MSG_DeleteCharacter` payload = **48 bytes** (transport framing; the logical packet `Size` field = 44).

## 3. Binary Layout

Packing context: Basedef.h is **not** uniformly packed. Explicit `#pragma pack(push,1)` regions cover only lines **808-835**, **1212-1246**, **1465-1492**, **2063-2097**. `MSG_DeleteCharacter` lives at lines **1270-1278**, i.e. **outside** every packed region, so MSVC **default `/Zp8`** applies: each member aligned to `min(sizeof(member), 8)`; struct size rounded up to the largest member alignment (4, driven by the `int Slot` member). Little-endian x86, LP32 (4-byte pointers; none on the wire here).

There is **no nested `STRUCT_*`** inside `MSG_DeleteCharacter` (only the `_MSG` header macro and two flat character arrays), so §3.3 lists the nested `STRUCT_SELCHAR` of the **response** packet `MSG_CNFDeleteCharacter` for completeness.

### 3.1 Header (`_MSG`, Basedef.h:925-930) — 12 bytes

| Field | Type | Offset | Size | Notes |
|---|---|---|---|---|
| `Size` | `short` | 0 | 2 | Total struct size incl. header = 44 |
| `KeyWord` | `char` | 2 | 1 | Obfuscation key byte |
| `CheckSum` | `char` | 3 | 1 | `Sum2 - Sum1` |
| `Type` | `short` | 4 | 2 | `_MSG_DeleteCharacter` (0x211) |
| `ID` | `short` | 6 | 2 | Client connection id; on forward becomes DBSrv index selector (see §4) |
| `ClientTick` | `unsigned int` | 8 | 4 | Client tick; must **not** equal `SKIPCHECKTICK` (235543242) or the packet is dropped (ProcessClientMessage.cpp:63-64) |

Header offset math under `/Zp8`: `short`@0 (2) → `char`@2 (1) → `char`@3 (1) → `short`@4 (2, aligned) → `short`@6 (2) → `unsigned int`@8 (4, aligned). No padding within the header; header size 12.

### 3.2 Payload — `MSG_DeleteCharacter` (Basedef.h:1270-1278), 32 bytes

| Field | Type | Offset | Size | Align | Pad | Notes |
|---|---|---|---|---|---|---|
| `Slot` | `int` | 12 | 4 | 4 | — | Target character slot in `[0, MOB_PER_ACCOUNT)` (validated on DBSrv) |
| `MobName` | `char[NAME_LENGTH]` | 16 | 16 | 1 | — | `NAME_LENGTH = 16` (Basedef.h:132); truncated at `[15]=0` by TMSrv (handler line 24) |
| `Password` | `char[12]` | 32 | 12 | 1 | — | Account password, 12 bytes = `ACCOUNTPASS_LENGTH` (Basedef.h:66); compared on DBSrv |
| *(tail pad)* | — | 44 | — | — | 0 | Struct ends at 44; already aligned to 4, no trailing pad |

Payload offset math under `/Zp8` (continuing from header end at 12): `int Slot`@12 (12 % 4 = 0, aligned, 4) → `MobName[16]`@16 (char align 1, 16) → `Password[12]`@32 (char align 1, 12) → end at **44**.

### 3.3 Nested struct expansions

`MSG_DeleteCharacter` itself has no nested structs. The **response** struct `MSG_CNFDeleteCharacter` (Basedef.h:1309-1314) embeds `STRUCT_SELCHAR` (Basedef.h:765-778), also outside all packed regions → `/Zp8`. Its layout:

| Member | Type | Offset | Size | Align | Notes |
|---|---|---|---|---|---|
| `SPX[4]` | `short` | 0 | 8 | 2 | Saved x positions |
| `SPY[4]` | `short` | 8 | 8 | 2 | Saved y positions |
| `Name[4][16]` | `char` | 16 | 64 | 1 | 4 char names × 16 |
| `Score[4]` | `STRUCT_SCORE` | 80 | 192 | 4 | 4 × 48 |
| `Equip[4][16]` | `STRUCT_ITEM` | 272 | 512 | 2 | 4×16 items × 8 |
| `Guild[4]` | `unsigned short` | 784 | 8 | 2 | 4 guild ids |
| `Coin[4]` | `int` | 792 | 16 | 4 | 4 coin values |
| `Exp[4]` | `long long` | 808 | 32 | 8 | 4 exp values |
| *(tail)* | — | 840 | — | — | end = 840, aligned to 8 |

Supporting sizes: `STRUCT_ITEM` (Basedef.h:398-412) = `short sIndex`(2) + `stEffect[3]`(3×2 union = 6) = **8** bytes, align 2. `STRUCT_SCORE` (Basedef.h:414-436) = `int`×4 (Level/Ac/Damage @0-12) + `uchar`×4 (Merchant/AttackRun/Direction/ChaosRate @12-16) + `int`×4 (MaxHp/MaxMp/Hp/Mp @16-32) + `short`×4 (Str/Int/Dex/Con @32-40) + `short Special[4]` (@40-48) = **48** bytes, align 4. → `sizeof(STRUCT_SELCHAR)` = **840**.

### 3.4 Size verification

```
sizeof(MSG_DeleteCharacter):
  _MSG header  = 12
  int Slot     =  4   (@12)
  char[16]     = 16   (@16)
  char[12]     = 12   (@32)
  total        = 44
  largest align = 4; 44 % 4 = 0 → sizeof = 44  → Size field = 44
```

Cross-checks in send/recv code:
- **TMSrv forward:** `DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_DeleteCharacter))` — _MSG_DeleteCharacter.cpp:33.
- **DBSrv recv:** `MSG_DeleteCharacter *m = (MSG_DeleteCharacter*)Msg;` then reads `m->Slot`, `m->Password`, `m->MobName` — CFileDB.cpp:1278-1340. Consistent with 44 bytes.
- **Response `MSG_CNFDeleteCharacter`:** `sizeof(MSG_CNFDeleteCharacter)` = 12 (header) + STRUCT_SELCHAR(840, align 8 → starts @16, so 4 pad after header) = **856**. Used at CFileDB.cpp:1353 and ProcessDBMessage.cpp:602. No mismatch.
- **No mismatch** found between the 44-byte layout, the `Size` field expectation, and `sizeof()` usages.

## 4. Lifecycle & Flow

### 4.1 Client → TMSrv (dispatch & guard)

1. Client sends `MSG_DeleteCharacter` (`Type = _MSG_DeleteCharacter`, `Size = 44`).
2. `CPSock.ReadMessage` reassembles + de-obfuscates; `Server.cpp(WSA_READ)` → `ProcessClientMessage(conn, pMsg, FALSE)` (ProcessClientMessage.cpp:38).
3. Dispatcher guards (ProcessClientMessage.cpp:42-64): `ID in [0, MAX_USER)`; `ServerDown < 120`; `ClientTick != SKIPCHECKTICK` (line 63-64).
4. `switch(std->Type)` → `case _MSG_DeleteCharacter:` → `Exec_MSG_DeleteCharacter(conn, pMsg)` (ProcessClientMessage.cpp:80-82).

### 4.2 TMSrv handler (Exec_MSG_DeleteCharacter, _MSG_DeleteCharacter.cpp:20-45)

- Truncates `MobName[NAME_LENGTH-1] = 0` (line 24).
- If `pUser[conn].Mode == USER_SELCHAR` (CUser.h:30): rewrites `m->Type = _MSG_DBDeleteCharacter`, `m->ID = conn`, sets `pUser[conn].Mode = USER_WAITDB` (CUser.h:32), forwards the **same 44-byte buffer** to DBSrv via `DBServerSocket.SendOneMessage` (line 33), logs `etc,delchar name:%s %d %d`.
- Else: `SendClientMessage(conn, "Deleting Character. wait a moment.")` + logs `err,delchar not user_selchar %d %d` (lines 40-43).

### 4.3 TMSrv → DBSrv (forward)

- `_MSG_DBDeleteCharacter` = 0x809 carries `FLAG_GAME2DB`. On DBSrv, `Server.cpp` dispatcher guard (Server.cpp:1343): requires `(std->Type & FLAG_GAME2DB)` and `ID in [0, MAX_USER]`, then calls `cFileDB.ProcessMessage(msg, conn)` (Server.cpp:1356).

### 4.4 DBSrv processing (CFileDB.cpp:1276-1355, `case _MSG_DBDeleteCharacter`)

- `Idx = GetIndex(conn, m->ID)` (server*MAX_USER + id, CFileDB.cpp:2328-2333); `Slot = m->Slot`.
- Validations (see §5). On success:
  1. `memset(ShortSkill[Slot], 0, 16)` (1331).
  2. `DeleteCharacter(mob->MobName, AccountName)` — deletes the on-disk character file (see §6).
  3. `BASE_ClearMob(mob)` + `BASE_ClearMobExtra(&mobExtra[Slot])` (1340-1341).
  4. `DBWriteAccount(&File)` — persists the account file (1343).
  5. Builds `MSG_CNFDeleteCharacter sm` with `Type = _MSG_DBCNFDeleteCharacter`, `sm.sel` filled by `DBGetSelChar` (1349), `ID = m->ID`, sends back to TMSrv (1353). **Note: `sm.Size` is not assigned** (see §9).

### 4.5 DBSrv → TMSrv response

- **Success:** `_MSG_DBCNFDeleteCharacter` (0x419) received in TMSrv `ProcessDBMessage` `case _MSG_DBCNFDeleteCharacter` (ProcessDBMessage.cpp:595-606): casts to `MSG_CNFDeleteCharacter`, rewrites `Type = _MSG_CNFDeleteCharacter` (0x112), `ID = ESCENE_FIELD + 1` (30001), sends to client via `pUser[conn].cSock.SendOneMessage(..., sizeof(MSG_CNFDeleteCharacter))`, then `Mode = USER_SELCHAR`.
- **Failure:** `_MSG_DBDeleteCharacterFail` (0x41E) in `case _MSG_DBDeleteCharacterFail` (ProcessDBMessage.cpp:609-619): sets `ID = ESCENE_FIELD + 1`, sends client signal `SendClientSignal(conn, 0, _MSG_DeleteCharacterFail)` (0x11B), then `Mode = USER_SELCHAR`.

### 4.6 ASCII sequence diagram

```
 Client                 TMSrv                    DBSrv (CFileDB)
   |  MSG_DeleteCharacter |                         |
   |  (Type 0x211, Size44)|                         |
   |--------------------->|                         |
   |                      | Exec_MSG_DeleteCharacter|
   |                      |  check Mode==USER_SELCHAR
   |                      |  Type=_MSG_DBDeleteCharacter (0x809)
   |                      |  ID=conn; Mode=USER_WAITDB
   |                      |------------------------>|
   |                      |                         | case _MSG_DBDeleteCharacter
   |                      |                         |  validate Slot/Pass/Class/Level
   |                      |                         |  DeleteCharacter(file)
   |                      |                         |  ClearMob/ClearMobExtra
   |                      |                         |  DBWriteAccount
   |                      |<------------------------|  MSG_CNFDeleteCharacter (Type 0x419, sel, ID=conn)
   |                      | case _MSG_DBCNFDeleteCharacter
   |                      |  Type=_MSG_CNFDeleteCharacter (0x112)
   |                      |  ID=ESCENE_FIELD+1
   |<---------------------|  Mode=USER_SELCHAR
   |   (fail path: DBSrv sends Type 0x41E →
   |    TMSrv SendClientSignal(0x11B) → Mode=USER_SELCHAR)
```

## 5. Validation & Guards

Execution order (TMSrv then DBSrv):

| # | Layer | Guard | Source | On failure |
|---|---|---|---|---|
| 1 | TMSrv dispatch | `std->ID in [0, MAX_USER)` | ProcessClientMessage.cpp:42 | log + drop |
| 2 | TMSrv dispatch | `ServerDown < 120` | ProcessClientMessage.cpp:53 | silent return |
| 3 | TMSrv dispatch | `ClientTick != SKIPCHECKTICK` | ProcessClientMessage.cpp:63 | silent return |
| 4 | TMSrv handler | `pUser[conn].Mode == USER_SELCHAR` | _MSG_DeleteCharacter.cpp:26 | `SendClientMessage` "Deleting Character. wait a moment." |
| 5 | DBSrv dispatch | `Type & FLAG_GAME2DB`; `ID in [0, MAX_USER]` | Server.cpp:1343 | log + drop |
| 6 | DBSrv | `Slot in [0, MOB_PER_ACCOUNT)` (MOB_PER_ACCOUNT=4) | CFileDB.cpp:1284 | `_MSG_DBDeleteCharacterFail` + log `err,deletechar slot` |
| 7 | DBSrv | `SecurePass != -1` (secure not registered) | CFileDB.cpp:1295 | log `err,deletechar secure illegal` (break, **no** fail sent) |
| 8 | DBSrv | `strncmp(m->Password, File.Info.AccountPass, 12) == 0` | CFileDB.cpp:1302 | `_MSG_DBDeleteCharacterFail` + log `err,deletechar password` |
| 9 | DBSrv | `mobExtra[Slot].ClassMaster ∈ {MORTAL(2), ARCH(1)}` | CFileDB.cpp:1315 | `_MSG_DBDeleteCharacterFail` (no log) |
| 10 | DBSrv | `mob->BaseScore.Level < 219` | CFileDB.cpp:1321 | `_MSG_DBDeleteCharacterFail` + log `err,deletechar level 219` |

Note: guards 8-10 also effectively enforce that the slot is **occupied** — a cleared/empty slot (`MORTAL`, `Level 0`) passes guards 9-10, but `DeleteCharacter` on an empty name / `BASE_ClearMob` on an already-empty mob is a no-op wipe of that slot. Guard 9 blocks deleting a character that has advanced past `ARCH` (i.e. a reborn/second-tier class), and guard 10 blocks deleting a level ≥ 219 character.

## 6. Game Mechanics & Business Logic

**Deletion rules (what is wiped / restricted):**
- The **on-disk character file** is deleted by `CFileDB::DeleteCharacter` (CFileDB.cpp:2303-2326): builds `./char/<FirstKey>/<UPPERCASE_NAME>` and calls `DeleteFileA`. It refuses to delete reserved system names `COM[0-9]` and `LPT[0-9]` (returns FALSE, no file delete) — CFileDB.cpp:2311-2314. The returned value is **ignored** at the call site (1333), so a failed file delete does not block the in-memory wipe.
- The **account file slot is cleared in place**:
  - `ShortSkill[Slot]` zeroed (CFileDB.cpp:1331).
  - `BASE_ClearMob(mob)` (Basedef.cpp:2943-2963): zeroes `STRUCT_MOB`, sets `SPX/SPY = 2112`, clears `BaseScore`/`CurrentScore`, clears all `Equip[MAX_EQUIP]` and `Carry[MAX_CARRY]` items, zeroes `SkillBar[4]`. **`MobName` becomes all zeros** (the slot is empty).
  - `BASE_ClearMobExtra(&mobExtra[Slot])` (Basedef.cpp:2965-2971): zeroes `STRUCT_MOBEXTRA`, resets `ClassMaster = MORTAL`, clears `QuestInfo`.
- **Persistence:** `DBWriteAccount(&File)` (CFileDB.cpp:2390) writes the account record to disk. (`DBExportAccount` is **not** called in this path, unlike the account-logout case at CFileDB.cpp:1256.)
- **Slot re-usability:** the account keeps all 4 slots (MOB_PER_ACCOUNT=4); the deleted slot is simply emptied, so a new character can later be created in it.

**Account capacity / coins / equipment:** The handler does **not** decrement any account-level coin/exp counter, and does **not** reclaim the character's coins/exp into the account — coins (`mob->Coin`) and `Exp` are destroyed with the mob (they live on `STRUCT_MOB`, cleared by `BASE_ClearMob`). There is no per-account coin wallet transfer on delete.

**Response content:** On success, DBSrv rebuilds the 4-slot select screen via `DBGetSelChar` (CFileDB.cpp:2611-2631): copies each slot's `Name`, `Equip`, `Guild`, `SPX/SPY`, `CurrentScore`, `Coin`, `Exp`. Note a pre-existing quirk: `sel->SPX[i] = Char[i].SPX; sel->SPX[i] = Char[i].SPY;` — the second assignment overwrites `SPX` with `SPY` (CFileDB.cpp:2623-2624), so the y-coordinate is never set. Equip face slot `sIndex` 22/23/24/25/32 is remapped to `ClassMaster`-derived value (2618-2619).

## 7. Side Effects

- **pAccountList mutation:** the account file slot `Char[Slot]` and `mobExtra[Slot]` are cleared in place (CFileDB.cpp:1340-1341); `ShortSkill[Slot]` zeroed (1331). No `RemoveAccountList` call; the account stays logged-in at DBSrv.
- **DB persistence:** `DBWriteAccount(&pAccountList[Idx].File)` (CFileDB.cpp:1343).
- **File deletion:** `./char/<FirstKey>/<NAME>` removed by `DeleteFileA` (CFileDB.cpp:2323); refused for `COM[0-9]`/`LPT[0-9]` names (2311-2314); return ignored.
- **Logs (format strings):**
  - TMSrv: `"etc,delchar name:%s %d %d"` with `pMob[conn].MOB.MobName, conn, Mode` (_MSG_DeleteCharacter.cpp:35); `"err,delchar not user_selchar %d %d"` (line 42).
  - DBSrv: `"err,deletechar slot"`, `"err,deletechar secure illegal"`, `"err,deletechar password"`, `"err,deletechar level 219"` (CFileDB.cpp:1288, 1297, 1306, 1325); `"delete char [%s]"` with mob name (CFileDB.cpp:1336-1338).
- **Outgoing packets:** on success TMSrv→client `_MSG_CNFDeleteCharacter` (0x112, `MSG_CNFDeleteCharacter` w/ `sel`); on failure TMSrv→client `_MSG_DeleteCharacterFail` (0x11B, `MSG_STANDARD` signal via `SendClientSignal`, SendFunc.cpp:197).
- **Mode transitions:** TMSrv `USER_SELCHAR` → `USER_WAITDB` on forward (_MSG_DeleteCharacter.cpp:31); back to `USER_SELCHAR` on both success and failure (ProcessDBMessage.cpp:604, 617).
- **DBSrv conn/index:** `m->ID` (the TMSrv conn) is used by DBSrv `GetIndex(conn, m->ID)` and echoed back in the response `sm.ID = m->ID` (CFileDB.cpp:1351).

## 8. Related Packets

- `_MSG_CreateCharacter` — create a char in a slot; mirror `_MSG_DBCNFNewCharacter` (0x418), client confirm `_MSG_CNFNewCharacter` (0x110).
- `_MSG_CharacterLogin` / `_MSG_CNFCharacterLogin` — entering/exiting the select screen.
- `_MSG_AccountSecure` — account security pass (`SecurePass`); deleting requires a non-`-1` `SecurePass` (CFileDB.cpp:1295).
- `_MSG_DBCNFAccountLogOut` (0x40B) — the analogous DBSrv confirm for account logout (CFileDB.cpp:1259).
- `_MSG_DBSaveMob`, `_MSG_DBSavingQuit` — persistence-related GAME2DB/DB2GAME mirrors.
- Fail path uses `SendClientSignal(conn, 0, _MSG_DeleteCharacterFail)` (0x11B) — ProcessDBMessage.cpp:615.

## 9. Discrepancies & Open Questions

1. **`sm.Size` not set in the DBSrv success response** (CFileDB.cpp:1345-1353) nor in the TMSrv rewrite (ProcessDBMessage.cpp:597-602). `MSG_CNFDeleteCharacter` is stack-allocated uninitialized and sent with `sizeof(...)`. Whether `Size` is garbage or `0` on the wire is undefined — the receiver's CPSock size-validation (`CPSock.cpp:397`) could reject it if `Size` reads as < `sizeof(HEADER)`. This mirrors an observed pattern in other packets; flagged as a latent bug.
2. **`DeleteCharacter` return value ignored** (CFileDB.cpp:1333): a failed file deletion (e.g. missing file) still proceeds with in-memory wipe and success confirm. Intentional (idempotent delete) or bug — UNKNOWN.
3. **`sel->SPX` overwritten by `SPY`** in `DBGetSelChar` (CFileDB.cpp:2623-2624): `SPY` is never copied, `SPX` gets the y value. Pre-existing upstream bug; affects the refreshed select screen.
4. **Guard 7 (`SecurePass == -1`) `break`s instead of sending a fail signal** (CFileDB.cpp:1295-1300): the client receives **no** `_MSG_DeleteCharacterFail` and stays stuck in `USER_WAITDB` (TMSrv never returns to `USER_SELCHAR`). Asymmetric with the other guards.
5. **TMSrv does not validate `Slot`/`Password`** — all validation is deferred to DBSrv. The TMSrv handler only checks `Mode`. A malformed `Slot` reaches DBSrv guard 6.
6. **Level threshold 219** is a hard-coded magic number (CFileDB.cpp:1321) with no named constant; semantics (max deletable level) inferred from the guard, not documented in code.
7. **Coins/exp of the deleted character are destroyed**, not credited to the account — confirmed by `BASE_ClearMob` zeroing `Coin`/`Exp` on `STRUCT_MOB` (Basedef.cpp:2943-2963). No account-level wallet exists for reclaim.

## 10. Source References

| File | Lines | Content |
|---|---|---|
| legacy/Code/Basedef.h | 925-930 | `_MSG` header macro |
| legacy/Code/Basedef.h | 932-941 | Direction flags |
| legacy/Code/Basedef.h | 65, 66, 71, 132 | `ACCOUNTNAME_LENGTH`=16, `ACCOUNTPASS_LENGTH`=12, `MOB_PER_ACCOUNT`=4, `NAME_LENGTH`=16 |
| legacy/Code/Basedef.h | 172 | `SKIPCHECKTICK` = 235543242 |
| legacy/Code/Basedef.h | 398-412 | `STRUCT_ITEM` (8 B) |
| legacy/Code/Basedef.h | 414-436 | `STRUCT_SCORE` (48 B) |
| legacy/Code/Basedef.h | 765-778 | `STRUCT_SELCHAR` (840 B) |
| legacy/Code/Basedef.h | 999 | `_MSG_DBDeleteCharacter` = 0x809 |
| legacy/Code/Basedef.h | 1113, 1118 | `_MSG_DBCNFDeleteCharacter` = 0x419, `_MSG_DBDeleteCharacterFail` = 0x41E |
| legacy/Code/Basedef.h | 1269-1278 | `_MSG_DeleteCharacter` = 0x211 + `MSG_DeleteCharacter` struct |
| legacy/Code/Basedef.h | 1308-1314 | `_MSG_CNFDeleteCharacter` = 0x112 + struct |
| legacy/Code/Basedef.h | 1394 | `_MSG_DeleteCharacterFail` = 0x11B |
| legacy/Code/TMSrv/_MSG_DeleteCharacter.cpp | 20-45 | `Exec_MSG_DeleteCharacter` handler |
| legacy/Code/TMSrv/ProcessClientMessage.cpp | 38-66, 80-82 | Dispatcher guards + case dispatch |
| legacy/Code/TMSrv/ProcessDBMessage.cpp | 595-606, 609-619 | DB confirm / fail responses |
| legacy/Code/TMSrv/CUser.h | 30, 32 | `USER_SELCHAR`, `USER_WAITDB` |
| legacy/Code/TMSrv/SendFunc.cpp | 197-206 | `SendClientSignal` |
| legacy/Code/DBSrv/CFileDB.cpp | 1276-1355 | `case _MSG_DBDeleteCharacter` |
| legacy/Code/DBSrv/CFileDB.cpp | 2303-2326 | `CFileDB::DeleteCharacter` (file delete) |
| legacy/Code/DBSrv/CFileDB.cpp | 2328-2333 | `GetIndex(server,id)` |
| legacy/Code/DBSrv/CFileDB.cpp | 2611-2631 | `DBGetSelChar` |
| legacy/Code/DBSrv/CFileDB.cpp | 2108-2120 | `SendDBSignal` |
| legacy/Code/DBSrv/Server.cpp | 1340-1357 | GAME2DB dispatch guard → `cFileDB.ProcessMessage` |
| legacy/Code/Basedef.cpp | 2943-2971 | `BASE_ClearMob`, `BASE_ClearMobExtra` |
| legacy/Code/CPSock.cpp / CPSock.h | 249, 373, 397, 535-584, 686 | Wire framing (magic, obfuscation, checksum, size) |
