# MSG_DBCNFAccountLogin

## 1. Summary

| Attribute | Value |
|---|---|
| Type constant | `_MSG_DBCNFAccountLogin` = `(22 \| FLAG_DB2GAME)` = `(0x16 \| 0x0400)` = **`0x416`** (`Basedef.h:1094`) |
| Struct on the wire (DBSrv→TMSrv) | `MSG_DBCNFAccountLogin` (`Basedef.h:1095-1109`) |
| Struct on the wire (TMSrv→client) | same `MSG_DBCNFAccountLogin` struct, re-tagged `_MSG_CNFAccountLogin` |
| Client-facing mirror type | `_MSG_CNFAccountLogin` = `(10 \| FLAG_GAME2CLIENT)` = `(0x0A \| 0x0100)` = **`0x10A`** (`Basedef.h:1211`) |
| Direction (DBSrv→TMSrv) | `FLAG_DB2GAME` (0x0400) — DB-to-Game |
| Direction (TMSrv→client) | `FLAG_GAME2CLIENT` (0x0100) — Game-to-Client |
| sizeof / expected Size | **1928 bytes** (see §3.4) |
| Primary producer | `DBSrv/CFileDB.cpp` — `case _MSG_DBAccountLogin` (lines ~764-801) and `case _MSG_DBNewAccount` (lines ~548-561) |
| Primary consumer | `TMSrv/ProcessDBMessage.cpp` — `case _MSG_DBCNFAccountLogin` (`ProcessDBMessage.cpp:354-472`) |
| Role | DBSrv → TMSrv **success** response to `MSG_AccountLogin`; TMSrv then forwards the same struct to the client as `_MSG_CNFAccountLogin` to unlock character-select |
| Session mode transition | `USER_LOGIN` → `USER_SELCHAR` (`ProcessDBMessage.cpp:400`) |

### Aliases / related constants
- `_MSG_CNFAccountLogin` (`0x10A`) shares the **exact same** `MSG_DBCNFAccountLogin` struct — the TODO comment at `Basedef.h:1095` explicitly notes: *"TODO: Check and rename if it's used for MSG_CNFAccountLogin, rename to MSG_CNFAccountLogin and change references in DB source"*. So the same struct is used for both legs; only the `Type` field differs.
- No other constant shares the low byte `22` with different flags; low byte 22 is used only here (`_MSG_DBCNFAccountLogin`). `_MSG_DBPrimaryAccount` = `(23 | FLAG_GAME2DB)` (`Basedef.h:1085`) is the next value and is unrelated.
- Same-struct but different-type constants that reuse the identical binary layout: none — `MSG_DBCNFAccountLogin`'s 1928-byte layout is unique to this message.

---

## 2. Wire Framing

Standard CPSock preamble (no per-packet deviation):

1. **Connection handshake:** on connect, the sender transmits a single 4-byte magic `INITCODE = 0x1F11F311` (`CPSock.h:40`, sent at `CPSock.cpp:249-250`). All following bytes are messages.
2. **Message framing (`_MSG` header, 12 bytes):** `Size(short)@0`, `KeyWord(char)@2`, `CheckSum(char)@3`, `Type(short)@4`, `ID(short)@6`, `ClientTick(uint)@8` (`Basedef.h:925-930`, mirrored by `HEADER` at `CPSock.h:42-50`). `Size` = total message size **including** the 12-byte header.
3. **Send path (`CPSock::AddMessage`, `CPSock.cpp:513-591`):**
   - `Size` set to the passed size; `KeyWord` = random index `iKeyWord = rand()%256` mapped via `pKeyWord[iKeyWord*2]`; `CheckSum=0` initially (`CPSock.cpp:535-540`).
   - Payload from byte **4** onward is obfuscated position-rotating XOR: for `i in [4,Size)`, `pos=KeyWord`, `Trans = pKeyWord[rst*2+1]` (`rst=pos%256`), and per `mod = i&0x3`: `0:+Trans<<1`, `1:-Trans>>3`, `2:+Trans<<2`, `3:-Trans>>5` (`CPSock.cpp:558-581`).
   - `CheckSum = Sum2 - Sum1` where `Sum1=Σ plain[i]`, `Sum2=Σ obfuscated[i]` (`CPSock.cpp:583-584`).
4. **Receive path (`CPSock::ReadMessage`, `CPSock.cpp:371-467`):** validates `INITCODE`, requires `Size ∈ [sizeof(HEADER)=12, MAX_MESSAGE_SIZE=8192]` (`CPSock.cpp:397`, `MAX_MESSAGE_SIZE` at `CPSock.h:38`), deobfuscates with the inverse rotation, and verifies `CheckSum == Sum2-Sum1` (`CPSock.cpp:455-458`). Even on checksum mismatch the packet is still returned (`CPSock.cpp:457-464`).
5. **No size-sanity check:** `BASE_CheckPacket` (`Basedef.cpp:6475`) is **disabled** — its entire body is inside a `/* ... */` block comment. The per-type size comparisons, including `_MSG_DBCNFAccountLogin && m->Size != sizeof(MSG_DBCNFAccountLogin)` (`Basedef.cpp:6557`) and the client-leg `_MSG_CNFAccountLogin && m->Size != sizeof(MSG_DBCNFAccountLogin)` (`Basedef.cpp:6486`), are dead code.
6. **Billing channel** is a separate 196-byte plain protocol (`CPSock.cpp:482`, `g_cGame`); irrelevant to this packet except that `SendBilling` (below) is a no-op stub in this build.

---

## 3. Binary Layout

**Packing context:** `MSG_DBCNFAccountLogin` (`Basedef.h:1095-1109`) is **NOT** inside a `#pragma pack(push,1)` region. The only pack(1) regions in `Basedef.h` are lines 808-835 (`STRUCT_RANKING`), 1212-1246 (`MSG_AccountLogin`/`MSG_AccountLogin_HWID`), 1465-1492 (`MSG_UpdateScore`), 2063-2097 (`MSG_AttackOne`). Therefore `MSG_DBCNFAccountLogin` and every struct it embeds (`STRUCT_SELCHAR`, `STRUCT_SCORE`, `STRUCT_ITEM`) use the **MSVC default /Zp8** layout: each member aligned to `min(sizeof(member), 8)`, struct size rounded up to the largest member alignment. Little-endian x86, LP32 (int=4, short=2, char=1, long long=8, no pointers on wire).

### 3.1 Header (12 bytes)

The `_MSG` macro (`Basedef.h:925-930`):

| Offset | Size | Type | Member | Alignment | Padding | Semantics |
|---|---|---|---|---|---|---|
| 0 | 2 | short | `Size` | 2 | — | Total message size incl. header = 1928 |
| 2 | 1 | char | `KeyWord` | 1 | — | Obfuscation key index (on wire; real key = `pKeyWord[KeyWord*2]`) |
| 3 | 1 | char | `CheckSum` | 1 | — | `Sum2 - Sum1` checksum |
| 4 | 2 | short | `Type` | 2 | — | `0x416` on DBSrv→TMSrv; `0x10A` after TMSrv rewrite |
| 6 | 2 | short | `ID` | 2 | — | TMSrv connection index (DBSrv copies from `m->ID`); rewritten to `ESCENE_FIELD+2=30002` on client leg |
| 8 | 4 | unsigned int | `ClientTick` | 4 | — | Timestamp (`CurrentTime`) |
| **12** | | | **header size** | | | largest align = 4 → header = 12 (already 4-aligned) |

### 3.2 Payload (offsets relative to start of message, i.e. after the 12-byte header's base of 0)

`MSG_DBCNFAccountLogin` (`Basedef.h:1095-1109`):

| Offset | Size | Type | Member | Alignment | Padding | Semantics |
|---|---|---|---|---|---|---|
| 0-11 | 12 | `_MSG` | header | 4 | — | §3.1 |
| 12 | 16 | char[16] | `HashKeyTable[16]` | 1 | — | **UNKNOWN** — only ever filled by `memset(&sm,0,…)` in the DBAccountLogin path (`CFileDB.cpp:766`); left uninitialized in the DBNewAccount path (§9) |
| 28 | 4 | int | `Unknow_28` | 4 | — | Named after its offset. Set to `0xCCCCCCCC` only in the DBAccountLogin path (`CFileDB.cpp:771`). **UNKNOWN** purpose |
| 32 | 840 | STRUCT_SELCHAR | `sel` | 8 | — | Character-select summary (4 chars). §3.3.1 |
| 872 | 1024 | STRUCT_ITEM[128] | `Cargo[MAX_CARGO]` | 2 | — | Shared bank cargo, 128 × 8 bytes. §3.3.3 |
| 1896 | 4 | int | `Coin` | 4 | — | Shared bank coin (`CFileDB.cpp:776`) |
| 1900 | 16 | char[16] | `AccountName[ACCOUNTNAME_LENGTH]` | 1 | — | Uppercase account name (verified against `pUser[conn].AccountName` at `ProcessDBMessage.cpp:358`) |
| 1916 | 12 | char[12] | `Keys[12]` | 1 | — | **UNKNOWN** — never set/read anywhere on either leg (dead field) |
| 1928 | | | **total** | | | largest align = 8 → 1928 is a multiple of 8, no tail padding |

### 3.3 Nested struct expansions

#### 3.3.1 `STRUCT_SELCHAR` (`Basedef.h:765-778`) — 840 bytes, align 8 (due to `long long Exp[4]`)

| Offset | Size | Type | Member | Align | Padding | Semantics |
|---|---|---|---|---|---|---|
| 0 | 8 | short[4] | `SPX[4]` | 2 | — | Saved-position X per char (warp scroll) |
| 8 | 8 | short[4] | `SPY[4]` | 2 | — | Saved-position Y per char |
| 16 | 64 | char[4][16] | `Name[4][16]` | 1 | — | Character names (4 × 16) |
| 80 | 192 | STRUCT_SCORE[4] | `Score[4]` | 4 | 0 pad before (16→80, already 4-aligned) | Per-char scores. §3.3.2 |
| 272 | 512 | STRUCT_ITEM[4][16] | `Equip[4][16]` | 2 | 0 pad before (272 already 2-aligned) | Equipped items, 64 × 8 bytes. §3.3.3 |
| 784 | 8 | unsigned short[4] | `Guild[4]` | 2 | — | Guild IDs |
| 792 | 16 | int[4] | `Coin[4]` | 4 | — | Per-char coin |
| 808 | 32 | long long[4] | `Exp[4]` | **8** | 0 pad before (808 already 8-aligned) | Per-char EXP (drives the 8-byte alignment of the whole struct) |
| **840** | | | **total** | 8 | | 840 % 8 = 0 → no tail pad |

#### 3.3.2 `STRUCT_SCORE` (`Basedef.h:414-436`) — 48 bytes, align 4

| Offset | Size | Type | Member | Align | Padding | Semantics |
|---|---|---|---|---|---|---|
| 0 | 4 | int | `Level` | 4 | — | Level |
| 4 | 4 | int | `Ac` | 4 | — | Defense |
| 8 | 4 | int | `Damage` | 4 | — | Damage |
| 12 | 1 | uchar | `Merchant` | 1 | — | Merchant flag |
| 13 | 1 | uchar | `AttackRun` | 1 | — | Speed |
| 14 | 1 | uchar | `Direction` | 1 | — | Facing |
| 15 | 1 | uchar | `ChaosRate` | 1 | — | Chaos rate |
| 16 | 4 | int | `MaxHp` | 4 | 0 pad (15→16) | Max HP |
| 20 | 4 | int | `MaxMp` | 4 | — | Max MP |
| 24 | 4 | int | `Hp` | 4 | — | Current HP |
| 28 | 4 | int | `Mp` | 4 | — | Current MP |
| 32 | 2 | short | `Str` | 2 | — | Strength |
| 34 | 2 | short | `Int` | 2 | — | Intelligence |
| 36 | 2 | short | `Dex` | 2 | — | Dexterity |
| 38 | 2 | short | `Con` | 2 | — | Constitution |
| 40 | 8 | short[4] | `Special[4]` | 2 | — | Special points |
| **48** | | | **total** | 4 | | 48 % 4 = 0 |

#### 3.3.3 `STRUCT_ITEM` (`Basedef.h:398-412`) — 8 bytes, align 2

| Offset | Size | Type | Member | Align | Padding | Semantics |
|---|---|---|---|---|---|---|
| 0 | 2 | short | `sIndex` | 2 | — | Item index (0 = empty; 470-500 = evDelete item range) |
| 2 | 2 | union | `stEffect[0]` | 2 | — | `short sValue` OR `{uchar cEffect; uchar cValue}` |
| 4 | 2 | union | `stEffect[1]` | 2 | — | ditto |
| 6 | 2 | union | `stEffect[2]` | 2 | — | ditto |
| **8** | | | **total** | 2 | | 8 % 2 = 0 |

`Cargo[MAX_CARGO=128]` → 128 × 8 = **1024 bytes** at payload offset 872.
`Equip[4][16]` → 64 × 8 = **512 bytes** at `STRUCT_SELCHAR` offset 272.

### 3.4 Size verification

```
header (12)
+ HashKeyTable[16]          (16)  -> 28
+ Unknow_28  (int, 4-al)    ( 4)  -> 32
+ sel (STRUCT_SELCHAR, 8-al)(840)  -> 872
+ Cargo[128] (2-al)         (1024) -> 1896
+ Coin (int, 4-al)          ( 4)  -> 1900
+ AccountName[16]           (16)  -> 1916
+ Keys[12]                  (12)  -> 1928
= sizeof(MSG_DBCNFAccountLogin) = 1928 bytes
```

**Expected `Size` field = 1928.** Cross-check of every `sizeof()` use:

| Use site | `sizeof(...)` | Value | Consistent |
|---|---|---|---|
| `CFileDB.cpp:552` | `sizeof(MSG_DBCNFAccountLogin)` | 1928 | ✅ |
| `CFileDB.cpp:559` | `sizeof(MSG_DBCNFAccountLogin)` | 1928 | ✅ |
| `CFileDB.cpp:766` | `sizeof(MSG_DBCNFAccountLogin)` | 1928 | ✅ |
| `CFileDB.cpp:780` | `sizeof(MSG_DBCNFAccountLogin)` | 1928 | ✅ |
| `ProcessDBMessage.cpp:434` | `sizeof(MSG_DBCNFAccountLogin)` | 1928 | ✅ |
| `Basedef.cpp:6486` (commented, `_MSG_CNFAccountLogin`) | `sizeof(MSG_DBCNFAccountLogin)` | 1928 | ✅ (dead) |
| `Basedef.cpp:6557` (commented, `_MSG_DBCNFAccountLogin`) | `sizeof(MSG_DBCNFAccountLogin)` | 1928 | ✅ (dead) |

**No mismatches.** 1928 < MAX_MESSAGE_SIZE (8192) so framing accepts it.

---

## 4. Lifecycle & Flow

### Producer — DBSrv

Two DBSrv code paths emit `MSG_DBCNFAccountLogin`:

**Path A — `case _MSG_DBAccountLogin` (`CFileDB.cpp:611`), success emission at 764-801:**
1. `MSG_AccountLogin *m` parsed (`CFileDB.cpp:613`); reserved `COM`/`LP` names rejected (`:619-625`).
2. Account read via `DBReadAccount` (`:636`); fail → `_MSG_DBAccountLoginFail_Account` (`:640`).
3. Coin floor, year/day block check → `_MSG_DBAccountLoginFail_Block` (`:645-659`).
4. TempKey (server-change) / password verification → `_MSG_DBAccountLoginFail_Pass` (`:664-682`); duplicate-login handling → `_MSG_DBAlreadyPlaying`/`_MSG_DBStillPlaying` (`:688-703`).
5. Name-swap cleanup for left/right hand items 774/775 (`:710-743`).
6. Copy file into account list, record MAC (`:745-754`).
7. Build `MSG_DBCNFAccountLogin sm`: `memset(&sm,0,…)` (**`:766`**), `Type = _MSG_DBCNFAccountLogin` (`:768`), `ID = m->ID` (`:769`), **`Unknow_28 = 0xCCCCCCCC` (`:771`)**, `AccountName` (`:773`), `Cargo` from account file (`:774`), `Coin` (`:776`), `sel` via `DBGetSelChar` (`:778`).
8. Send to TMSrv: `pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(...))` (**`:780`**).
9. Primary-account broadcast (`:782-801`) and server-change handling (`:802+`).

**Path B — `case _MSG_DBNewAccount` (`CFileDB.cpp:510`), success emission at 548-561:**
1. New account created in file (`:524-535`); fail → `_MSG_DBNewAccountFail` (`:539`).
2. Build `MSG_DBCNFAccountLogin sm`: `Type` (`:550`), `ID = m->ID` (`:551`), `Size = sizeof(...)` (`:552`), `AccountName` (`:554`), `Cargo` zeroed (`:555`), `sel` via `DBGetSelChar` (`:557`).
3. Send: `SendOneMessage((char*)&sm, sizeof(...))` (**`:559`**).
   ⚠️ This path does **not** `memset` the struct first — `HashKeyTable`, `Unknow_28`, `Coin`, `Keys`, `ClientTick` are left as stack garbage (§9).

`DBGetSelChar` (`CFileDB.cpp:2611-2631`) fills `sel`: per char, copies `Name`, `Equip` (with class-master face normalization at `:2618-2619`), `Guild`, `SPX/SPY`, `Score = CurrentScore`, `Coin`, `Exp`.

### Consumer — TMSrv (`ProcessDBMessage.cpp:354-472`) → client

Dispatch: `ProcessDBMessage` (`:39`) validates `Type & FLAG_DB2GAME` and `0 ≤ ID < MAX_USER` (`:43`), then `switch(std->Type)` → `case _MSG_DBCNFAccountLogin` (`:354`). The client-side initiating message that started all this was dispatched from `ProcessClientMessage.cpp:68-69` → `Exec_MSG_AccountLogin` (`_MSG_AccountLogin.cpp:21`), which forwards it to DBSrv as `_MSG_DBAccountLogin` (`_MSG_AccountLogin.cpp:73-92`) and sets `Mode = USER_LOGIN` (`:94`).

```text
[Client]  MSG_AccountLogin (0x20D, CLIENT2GAME)
   │  ProcessClientMessage.cpp:68 → Exec_MSG_AccountLogin (_MSG_AccountLogin.cpp:21)
   │  Type→_MSG_DBAccountLogin, Mode=USER_LOGIN (:73-94)
   ▼
[TMSrv] ──DBServerSocket.SendOneMessage──▶ [DBSrv]
                                            case _MSG_DBAccountLogin (CFileDB.cpp:611)
                                            build MSG_DBCNFAccountLogin (:764-780 / :548-559)
   ▲                                            │ Unknow_28=0xCCCCCCCC (:771)
   │              SendOneMessage(:780/:559)     ▼
   └────────────── obfuscated 1928B frame ──▶ [TMSrv] ProcessDBMessage (ProcessDBMessage.cpp:39)
                                                case _MSG_DBCNFAccountLogin (:354)
                                                │ validate AccountName (:358) / MAC block (:369-383)
                                                │ reset session flags (:386-395)
                                                │ rewrite Type=_MSG_CNFAccountLogin, ID=ESCENE_FIELD+2 (:397-398)
                                                │ Mode=USER_SELCHAR (:400)
                                                │ normalize weapon cargo effects (:402-422)
                                                │ evDelete cleanup (:424-431)
                                                │ SendOneMessage to client (:434)
                                                │ copy Cargo/Coin/sel to pUser (:436-440)
                                                │ billing IsFree/SendBilling (:443-451)
                                                │ log (:456-457); TransperCharacter signal (:459-470)
   ▼
[Client]  MSG_CNFAccountLogin (0x10A, GAME2CLIENT) — same 1928-byte struct, Type/ID rewritten
```

---

## 5. Validation & Guards

Numbered guards, in execution order, inside `case _MSG_DBCNFAccountLogin` (`ProcessDBMessage.cpp:354-472`):

| # | Line | Guard / check | On failure → action |
|---|---|---|---|
| G1 | `:358` | `strcmp(m->AccountName, pUser[conn].AccountName) != 0` — received account must match the connection's recorded account | Send `_NN_Try_Reconnect`, `SendMessageA`, `CloseUser`, `return` (`:360-366`) |
| G2 | `:369-383` | Scan `pMac[0..MAX_MAC)` for a fully-zero or matching-MAC entry vs `pUser[conn].Mac[0..3]` | On match: send `_NN_MAC_Block`, `SendMessageA`, `CloseUser`, `break` (`:376-381`) — **note:** only `break`s the loop; execution continues at `:386` even though the user was closed (§9) |
| G3 | `:443` | `BILLING > 0 && IsFree(&m->sel) != 0` | Gate for `SendBilling` (see §6) |
| — | `:43` (dispatch) | `Type & FLAG_DB2GAME` and `0 ≤ ID < MAX_USER` | log `err,packet ...` and return (`ProcessDBMessage.cpp:43-52`) |

There is **no** length/size validation for this message — `BASE_CheckPacket` is disabled (dead code, §2 item 5). The handler trusts `sizeof(MSG_DBCNFAccountLogin)` for the client forward (`:434`).

---

## 6. Game Mechanics & Business Logic

1. **Weapon-position cargo effect normalization** (`ProcessDBMessage.cpp:402-422`): for each `Cargo[i]`, if `0 < sIndex < MAX_ITEMLIST` and `g_pItemList[sIndex].nPos == 64 || 192` (weapon positions), rewrite any `EF_DAMAGEADD` (67, `ItemEffect.h:107`) or `EF_DAMAGE2` (73, `ItemEffect.h:115`) effect in slots `[0..2]` to `EF_DAMAGE` (2, `ItemEffect.h:4`). Rationale: server-side legacy damage effect normalization so cargo weapons use the standard damage effect.
2. **`evDelete` event-item cleanup** (`:424-431`): if global `evDelete != 0` (`Server.cpp:307`), zero the `sIndex` of any `Cargo[i]` with `470 ≤ sIndex ≤ 500` — strips event items from cargo during the delete event.
3. **Billing gating** (`:443-451`): when `BILLING > 0` and `IsFree(&m->sel) != 0` (`IsFree`, `Server.cpp:1363-1375`: true if `FREEEXP ≤ 0` or any non-empty char has `FREEEXP ≤ Level < 999`), call `SendBilling(conn, m->AccountName, 8|1, 1)` depending on `CHARSELBILL` and set `pUser[conn].Unk_2732 = SecCounter`. `SendBilling` is a stub returning `TRUE` (`Server.cpp:1377-1380`) — the actual billing I/O is stubbed out.
4. **TransperCharacter signal** (`:459-470`): if `TransperCharacter != 0` (`Server.cpp:382`), send a `MSG_STANDARDPARM2` (`_MSG_TransperCharacter`, `ID = ESCENE_FIELD+1`) to the client.
5. **Character-select data provisioning:** the `sel`, `Cargo`, `Coin` carried by the packet are the entire basis of the client's character-select screen and of the TMSrv-side session state (copied at `:436-440`).

---

## 7. Side Effects

On successful handling in `ProcessDBMessage.cpp:354-472`:

1. **Session flag resets** (`:386-395`): `IsBillConnect=0`, `Whisper=0`, `Guildchat=0`, `PartyChat=0`, `KingChat=0`, `Admin=0`, `Chatting=0`, `Unk_2732=0`, `Unk_2728=0`, `OnlyTrade=1`.
2. **Mode transition** `USER_LOGIN → USER_SELCHAR` (`:400`).
3. **Client forward:** `m->ID = ESCENE_FIELD + 2 = 30002`, `m->Type = _MSG_CNFAccountLogin (0x10A)` (`:397-398`), then `cSock.SendOneMessage((char*)m, sizeof(MSG_DBCNFAccountLogin))` (`:434`). The client receives the **same 1928-byte struct** as `_MSG_CNFAccountLogin`.
4. **State copy to server session:** `memcpy(pUser[conn].Cargo, m->Cargo, sizeof(STRUCT_ITEM)*MAX_CARGO)` (`:436`), `pUser[conn].Coin = m->Coin` (`:438`), `pUser[conn].Unk_1816 = 0` (`:439`), `pUser[conn].SelChar = m->sel` (`:440`).
5. **Billing state:** possibly `pUser[conn].Unk_2732 = SecCounter` (`:450`) after `SendBilling` (`:446-448`).
6. **Misc reset:** `pUser[conn].Unk5[0]=0`, `LastClientTick=0` (`:453-454`).
7. **Log:** `"CNFAccountLogin Mac: %d.%d.%d.%d"` with `pUser[conn].Mac[0..3]` (`:456-457`).
8. **In-flight cargo mutation:** weapon effects normalized (§6.1) and evDelete items zeroed (§6.2) **in the packet buffer before** the client forward and before the `pUser[conn].Cargo` copy — so the client sees normalized cargo and the server session stores it.

---

## 8. Related Packets

| Type constant | Value | Role |
|---|---|---|
| `_MSG_AccountLogin` | `13 \| CLIENT2GAME` = `0x20D` (`Basedef.h:1210`) | Initiating client→TMSrv login request |
| `_MSG_DBAccountLogin` | `13 \| GAME2DB` = `0x80D` | TMSrv→DBSrv relay of the login (formed in `_MSG_AccountLogin.cpp:73`) |
| `_MSG_CNFAccountLogin` | `10 \| GAME2CLIENT` = `0x10A` (`Basedef.h:1211`) | Client-facing mirror of this same struct |
| `_MSG_DBAccountLoginFail_Account` | `33 \| DB2GAME` (`Basedef.h:1121`) | Fail: no such account (`CFileDB.cpp:640`) |
| `_MSG_DBAccountLoginFail_Pass` | `34 \| DB2GAME` (`Basedef.h:1122`) | Fail: bad password (`CFileDB.cpp:679`) |
| `_MSG_DBAccountLoginFail_Block` | `36 \| DB2GAME` (`Basedef.h:1124`) | Fail: account blocked (year/day, `CFileDB.cpp:657`) |
| `_MSG_DBAccountLoginFail_Disable` | `37 \| DB2GAME` (`Basedef.h:1125`) | Fail: disabled account |
| `_MSG_DBAlreadyPlaying` | `31 \| DB2GAME` (`Basedef.h:1119`) | Fail: same account already logged in (`CFileDB.cpp:694`) |
| `_MSG_DBStillPlaying` | `32 \| DB2GAME` (`Basedef.h:1120`) | Fail: still playing / force-quit (`CFileDB.cpp:699`) |
| `_MSG_DBNewAccountFail` | `26 \| DB2GAME` (`Basedef.h:1114`) | Fail: new-account creation failed (`CFileDB.cpp:539`; TMSrv handler `ProcessDBMessage.cpp:475-484`) |
| `_MSG_DBCheckPrimaryAccount` | (see `CFileDB.cpp:784-788`) | Primary-account broadcast sent alongside the success response (`CFileDB.cpp:782-801`) |
| `_MSG_TransperCharacter` | (see `ProcessDBMessage.cpp:459-469`) | Optional follow-up signal when server-change mode is active |

---

## 9. Discrepancies & Open Questions

1. **Two producers, inconsistent initialization.** `case _MSG_DBAccountLogin` `memset`s the whole struct (`CFileDB.cpp:766`) and sets `Unknow_28=0xCCCCCCCC` (`:771`) and `Coin` (`:776`); `case _MSG_DBNewAccount` (`:548-561`) does **not** `memset` and never sets `HashKeyTable`, `Unknow_28`, `Coin`, `Keys`, or `ClientTick` — those are stack garbage on the wire for the new-account path. Open question: is this intentional or a latent bug? (`HashKeyTable`/`Keys` are unused on the receiving side regardless.)
2. **`Unknow_28` semantics unknown.** Named after its offset (28), value `0xCCCCCCCC` (`CFileDB.cpp:771`) is never read on the TMSrv side. Its purpose is UNKNOWN.
3. **`HashKeyTable[16]` and `Keys[12]` are dead fields** — never written meaningfully and never read on either leg. Purpose UNKNOWN.
4. **MAC-block `break` vs `return`.** The MAC scan (`ProcessDBMessage.cpp:369-383`) `break`s out of the loop after `CloseUser(conn)` but does not `return`; execution continues through the session-reset/forward logic (`:386-470`) on a closed socket. Likely intended to `return` (compare the account-name guard at `:358-367` which does). Potential bug.
5. **Struct name/TODO.** The `_MSG_DBCNFAccountLogin`/`MSG_DBCNFAccountLogin` name is reused for the client leg (`_MSG_CNFAccountLogin`); the TODO at `Basedef.h:1095` flags renaming to `MSG_CNFAccountLogin`.
6. **`send` vs `AddMessage`:** `Exec_MSG_AccountLogin` sends via `DBServerSocket.SendOneMessage` (`_MSG_AccountLogin.cpp:92`) which wraps `AddMessage` → full obfuscation (§2). No deviation.
7. **Disabled validation:** `BASE_CheckPacket` dead code (`Basedef.cpp:6475` body commented out) means malformed `Size` is never rejected; the handler blindly forwards `sizeof(MSG_DBCNFAccountLogin)` bytes.

---

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `legacy/Code/Basedef.h` | 56, 65-66, 71, 76-77, 132, 139, 152, 170 | `MAX_USER`, `ACCOUNTNAME_LENGTH=16`, `ACCOUNTPASS_LENGTH=12`, `MOB_PER_ACCOUNT=4`, `MAX_CARRY=64`, `MAX_CARGO=128`, `NAME_LENGTH=16`, `MAX_ITEMLIST=6500`, `MAX_MAC=200`, `ESCENE_FIELD=30000` |
| `legacy/Code/Basedef.h` | 398-412 | `STRUCT_ITEM` (8 bytes) |
| `legacy/Code/Basedef.h` | 414-436 | `STRUCT_SCORE` (48 bytes) |
| `legacy/Code/Basedef.h` | 765-778 | `STRUCT_SELCHAR` (840 bytes, `long long Exp[4]`) |
| `legacy/Code/Basedef.h` | 925-941 | `_MSG` macro, direction flags |
| `legacy/Code/Basedef.h` | 1094-1109 | `_MSG_DBCNFAccountLogin` (0x416), `MSG_DBCNFAccountLogin` struct, TODO |
| `legacy/Code/Basedef.h` | 1210-1211 | `_MSG_AccountLogin` (0x20D), `_MSG_CNFAccountLogin` (0x10A) |
| `legacy/Code/Basedef.h` | 1111-1126 | Related DB fail/status constants |
| `legacy/Code/Basedef.h` | 808/835, 1212/1246, 1465/1492, 2063/2097 | `#pragma pack(push,1)` regions (MSG_DBCNFAccountLogin is NOT in one) |
| `legacy/Code/Basedef.cpp` | 6475-6588 | `BASE_CheckPacket` — body disabled (block comment); size checks incl. `:6486`, `:6557` |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 39-52 | Dispatch + guard |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 354-472 | `case _MSG_DBCNFAccountLogin` handler |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 475-498 | `_MSG_DBNewAccountFail`, `_MSG_DBAccountLoginFail_Account` handlers |
| `legacy/Code/TMSrv/ProcessClientMessage.cpp` | 66-70 | `case _MSG_AccountLogin` dispatch |
| `legacy/Code/TMSrv/_MSG_AccountLogin.cpp` | 21-96 | `Exec_MSG_AccountLogin` (initiating path) |
| `legacy/Code/TMSrv/Server.cpp` | 307, 320-321, 382, 1363-1380 | `evDelete`, `BILLING`, `CHARSELBILL`, `TransperCharacter`, `IsFree`, `SendBilling` |
| `legacy/Code/TMSrv/Server.h` | 78, 196, 264, 276-277, 327 | extern declarations |
| `legacy/Code/DBSrv/CFileDB.cpp` | 510-561 | `case _MSG_DBNewAccount` producer (Path B) |
| `legacy/Code/DBSrv/CFileDB.cpp` | 611-705 | `case _MSG_DBAccountLogin` validation/fail paths |
| `legacy/Code/DBSrv/CFileDB.cpp` | 745-801 | Success emission (Path A), `Unknow_28=0xCCCCCCCC` (`:771`), send (`:780`) |
| `legacy/Code/DBSrv/CFileDB.cpp` | 2611-2631 | `DBGetSelChar` |
| `legacy/Code/CPSock.h` | 38, 40-50 | `MAX_MESSAGE_SIZE=8192`, `INITCODE=0x1F11F311`, `HEADER` |
| `legacy/Code/CPSock.cpp` | 249-250, 371-467, 513-591 | Handshake, receive/obfuscate-verify, send/obfuscate |
| `legacy/Code/ItemEffect.h` | 4, 107, 115 | `EF_DAMAGE=2`, `EF_DAMAGEADD=67`, `EF_DAMAGE2=73` |
