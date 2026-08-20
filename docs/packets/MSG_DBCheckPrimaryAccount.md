# MSG_DBCheckPrimaryAccount

## 1. Summary

| Field | Value |
|---|---|
| Type constant | `_MSG_DBCheckPrimaryAccount` = `(20 \| FLAG_DB2GAME)` = `0x414` |
| Base (low byte) | `20` (`0x14`) |
| Direction flag | `FLAG_DB2GAME` = `0x0400` |
| Direction | DBSrv → TMSrv (broadcast to all connected game servers) |
| Struct | `MSG_DBCheckPrimaryAccount` (`Basedef.h:1042-1049`) |
| Transport form | Dedicated struct (NOT `MSG_STANDARD`); header + `AccountName[16]` + `Mac[4]` |
| `sizeof` | **44 bytes** |
| `Size` field value | 44 |
| Primary producer | `CFileDB.cpp:784-800` (case `_MSG_DBAccountLogin`, when `GetAccountsByMac <= 1`) |
| Secondary producer | `CFileDB.cpp:2074-2090` (case `_MSG_DBPrimaryAccount`, gm command `contaprincipal`) |
| TMSrv handler | `ProcessDBMessage.cpp:148-182` (case `_MSG_DBCheckPrimaryAccount`) |
| Purpose | Flag a same-MAC session as the **primary account** and re-evaluate the `OnlyTrade` flag on all other same-MAC sessions. |

`0x414` = `0x0400 | 0x0014` = `FLAG_DB2GAME | 20`. It is a DB2GAME signal-type packet that carries a payload struct (account name + MAC), so it does not travel as a bare `MSG_STANDARD` — it has its own struct.

## 2. Wire Framing

Standard CPSock framing applies (no per-packet deviation beyond normal obfuscation):

- Transport: TCP between DBSrv and each TMSrv; connection bootstrap uses `INITCODE = 0x1F11F311` (`CPSock.h:40`) as the magic to detect/validate the peer.
- `SendOneMessage` (`CPSock.cpp:686-693`) → `AddMessage` (`CPSock.cpp:513-591`) copies the caller buffer into the socket send buffer.
- `AddMessage` overwrites framing fields on the caller's buffer: `Size = Size` (passed in), `KeyWord = random table index`, `CheckSum` computed, `ClientTick = CurrentTime` (`CPSock.cpp:535-541`). The **first 4 bytes** (`Size`, `KeyWord`, `CheckSum`) are copied verbatim (`CPSock.cpp:586`).
- Bytes from **offset 4** to `Size-1` are position-rotating XOR'd per byte using `pKeyWord`, modulo `i&0x3` (`CPSock.cpp:558-581`), and `CheckSum = Sum2 - Sum1` (`CPSock.cpp:583-584`).
- The message is **broadcast**: DBSrv loops over every connected game server socket and calls `pUser[i].cSock.SendOneMessage(...)` for each (`CFileDB.cpp:794-800`). Each socket gets its own randomized `KeyWord`/obfuscation.
- Receiver side validates `Size` within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` (`CPSock.cpp:397`); `HEADER` is 12 bytes (`CPSock.h:42-50`), `MAX_MESSAGE_SIZE = 8192` (`CPSock.h:38`).
- `BASE_CheckPacket` is disabled (`Basedef.cpp:6475`); no extra integrity gate on the receive path.

Per-packet framing deviation: none structurally — same CPSock framing; the packet simply carries a 16-byte MAC and a 16-byte account name in its payload.

## 3. Binary Layout

Packing context: `Basedef.h` is **not** uniformly packed. The explicit `#pragma pack(push,1)` regions cover only lines **808-835**, **1212-1246**, **1465-1492**, **2063-2097**. The `MSG_DBCheckPrimaryAccount` struct (`Basedef.h:1042-1049`) falls in **none** of these regions, so MSVC default **/Zp8** alignment applies: each member is aligned to `min(sizeof(member), 8)`, and the struct size is rounded up to the largest member alignment (4 here, since the widest member is `unsigned int`).

### 3.1 Header (`_MSG`, `Basedef.h:925-930`)

| Offset | Size | Type | Align | Field | Semantics |
|---|---|---|---|---|---|
| 0 | 2 | short | 2 | `Size` | Total message size incl. header (= 44) |
| 2 | 1 | char | 1 | `KeyWord` | Obfuscation table index / key |
| 3 | 1 | char | 1 | `CheckSum` | `Sum2 - Sum1` checksum |
| 4 | 2 | short | 2 | `Type` | `_MSG_DBCheckPrimaryAccount` = `0x414` |
| 6 | 2 | short | 2 | `ID` | Set to 0 by producer (broadcast) → receiver `conn==0` |
| 8 | 4 | unsigned int | 4 | `ClientTick` | Client tick (set by CPSock) |

Header size = **12 bytes**. Offsets: 0,2,3,4,6,8 — no padding within the header (`ClientTick` at 8 is 4-aligned). `HEADER` (`CPSock.h:42-50`) has identical layout.

### 3.2 Payload

| Offset | Size | Type | Align | Field | Semantics |
|---|---|---|---|---|---|
| 12 | 16 | char[16] | 1 | `AccountName` | Account name, `ACCOUNTNAME_LENGTH`=16 (`Basedef.h:65`) |
| 28 | 16 | unsigned int[4] | 4 | `Mac` | 4×32-bit MAC address words (matches `STRUCT_BLOCKMAC`/`AdapterName`) |

Offset math:
- `_MSG` occupies bytes 0–11 (12 bytes).
- `AccountName[16]`: char align 1, placed at offset 12 → bytes 12–27.
- `Mac[4]`: `unsigned int` align 4. Next free offset = 28; 28 % 4 = 0 → **no padding**, placed at 28 → bytes 28–43.

Total = 12 + 16 + 16 = **44 bytes**. Largest member alignment = 4; 44 % 4 = 0 → `sizeof = 44`, no trailing padding.

### 3.3 Nested struct expansions

None. `MSG_DBCheckPrimaryAccount` contains only scalar/array members (`char[16]`, `unsigned int[4]`); there are no nested `STRUCT_*` types to expand. `Mac` maps 1:1 to `STRUCT_BLOCKMAC { int Mac[4]; }` (`Basedef.h:916-919`) but is declared inline as `unsigned int Mac[4]`.

### 3.4 Size verification

- Producer sets `sm_pa.Size = sizeof(MSG_DBCheckPrimaryAccount)` (`CFileDB.cpp:787`, `:2077`) and sends `sizeof(MSG_DBCheckPrimaryAccount)` (`CFileDB.cpp:799`, `:2089`) → Size = 44.
- `memset(&sm_pa, 0, sizeof(...))` before filling (`CFileDB.cpp:785`, `:2075`) → `KeyWord`/`CheckSum`/`ID`/`ClientTick` initially zeroed; CPSock fills framing fields at send time.
- `MSG_DBCheckPrimaryAccount` is **not** inside any `#pragma pack(1)` region; with /Zp8 default it is 44 bytes. If it were (incorrectly) treated as packed, the layout would be identical here (no internal padding exists), so `sizeof` would still be 44 — no mismatch is detectable on the wire either way, but the authoritative layout is the /Zp8 one.
- No size mismatch: all four `sizeof` uses agree (44).

All payload members are set by both producers and consumed by the handler — no UNKNOWN members.

## 4. Lifecycle & Flow

### 4.1 Producer — automatic (account login), `CFileDB.cpp:782-801`

Triggered inside case `_MSG_DBAccountLogin` (GAME2DB, DBSrv receive). After the account is added to the list and `MSG_DBCNFAccountLogin` is sent back to the connecting game server:

1. `GetAccountsByMac(m->AdapterName) <= 1` (`CFileDB.cpp:782`) — counts how many *currently logged-in* accounts share this MAC (`GetAccountsByMac`, `CFileDB.cpp:2351-2367`, iterates `pAccountList[0..MAX_DBACCOUNT)` comparing `Mac` via `memcmp`).
2. If the count is ≤ 1 (this account is the only/primary account on the MAC), build `MSG_DBCheckPrimaryAccount`:
   - `Mac` = `m->AdapterName` (the MAC supplied in the login) — `CFileDB.cpp:790`
   - `AccountName` = the logged-in account name — `CFileDB.cpp:792`
3. Broadcast to all game servers with a live socket and non-empty mode (`CFileDB.cpp:794-800`): each `pUser[i].cSock.SendOneMessage(...)`.

### 4.2 Producer — gm command, `CFileDB.cpp:2068-2101`

Triggered by case `_MSG_DBPrimaryAccount` (GAME2DB, `_MSG_DBPrimaryAccount = (23|FLAG_GAME2DB)` = `0x823`, `Basedef.h:1085`), which TMSrv sends when a player whispers `contaprincipal` (`_MSG_MessageWhisper.cpp:1112-1127`). DBSrv rebuilds and broadcasts the same `MSG_DBCheckPrimaryAccount` unconditionally (no `GetAccountsByMac` gate), then replies `MSG_DBClientMessage` "Sua conta agora é a primária." (`CFileDB.cpp:2092-2100`).

### 4.3 DBSrv → TMSrv transport

`SendOneMessage` on each connected TMSrv socket (CPSock framing + obfuscation as in §2). Receiver `ID` = 0 (never set by producer).

### 4.4 TMSrv handler — `ProcessDBMessage.cpp:148-182`

`ProcessDBMessage` casts the buffer to `MSG_STANDARD` and reads `Type`/`ID` (`ProcessDBMessage.cpp:41`). Since `ID == 0`, the `if (conn == 0)` broadcast branch is taken (`ProcessDBMessage.cpp:56`), `switch(std->Type)` reaches `case _MSG_DBCheckPrimaryAccount` (`:148`), and the buffer is recast to `MSG_DBCheckPrimaryAccount *m` (`:150`).

ASCII sequence diagram:

```
 Player(acct A)        DBSrv                      TMSrv(1..N)
      |  MSG_DBAccountLogin(GAME2DB)                 |
      |---------------->  +GetAccountsByMac<=1?      |
      |                   +build MSG_DBCheckPrimary  |
      |                   +broadcast to ALL TMSrv    |
      |  (DBSrv->TMSrv) MSG_DBCheckPrimaryAccount    |
      |                       |---------------------->  conn==0 broadcast switch
      |                       |                         case _MSG_DBCheckPrimaryAccount
      |                       |                         for i in 1..MAX_USER:
      |                       |                           if Mode==EMPTY || Sock==0: skip
      |                       |                           if Mac[i] != m->Mac: skip
      |                       |                           if AcctName[i]==m->AccountName:
      |                       |                               OnlyTrade[i]=0   (primary)
      |                       |                           else:
      |                       |                               OnlyTrade[i]=1
      |                       |                               RemoveParty(i)
      |                       |                               if Mode==USER_PLAY and
      |                       |                                  outside village (no 0x80 attr):
      |                       |                                  SendClientMessage(_NN_OnlyVillage)
      |                       |                                  DoRecall(i)
```

## 5. Validation & Guards

Execution order inside `case _MSG_DBCheckPrimaryAccount` (`ProcessDBMessage.cpp:152-180`). Guards `G1`–`G3` skip sessions; `G4` is a conditional side-effect gate.

| # | Guard | Source | Effect |
|---|---|---|---|
| G1 | `pUser[i].Mode == USER_EMPTY \|\| pUser[i].cSock.Sock == 0` | `:154` | `continue` — skip empty/dead slots. Loop from `i=1` to `MAX_USER-1` (`MAX_USER=1000`, `Basedef.h:56`; slot 0 reserved). |
| G2 | `memcmp(pUser[i].Mac, m->Mac, sizeof(m->Mac)) != 0` | `:157` | `continue` — only act on sessions sharing the broadcast MAC. |
| G3 | `strncmp(pUser[i].AccountName, m->AccountName, ACCOUNTNAME_LENGTH) == 0` | `:160` | Primary branch (G3 passes). |
| G4 | `pMob[i].Mode == USER_PLAY` AND `(Village<0 \|\| Village>=5)` AND `(mapAttribute & 0x80)==0` | `:169-174` | Recall gate for non-primary accounts. `Village = BASE_GetVillage(...)` (`:171`); `mapAttribute = GetAttribute(...)` (`:172`). |

Pre-handler guard (outside case): `ProcessDBMessage` requires `(std->Type & FLAG_DB2GAME)` and `0 <= std->ID < MAX_USER` (`:43`); this message passes both (`Type=0x414` has the DB2GAME bit; `ID=0`).

`BASE_GetVillage(x,y)` (`Basedef.cpp:4258-4267`): returns `0..4` if inside a guild-zone city bounds, else `5`. So `Village < 0 || Village >= 5` means **not inside any city** (`Village==5`).

`GetAttribute(x,y)` (`GetFunc.cpp:1257-1273`): reads the map-attribute byte; bit `0x80` tested at `:174`.

## 6. Game Mechanics & Business Logic

Semantics — the "primary account" concept:

- For each same-MAC session **matching** the broadcast account (`G3` true): that session is the **primary account** → `pUser[i].OnlyTrade = 0` (`:162`). `OnlyTrade` (`CUser.h:115`) gates trading/party restrictions (e.g. `_MSG_AcceptParty` sends `_NN_ONLYTRADE` when `OnlyTrade` is set).
- For each same-MAC session **not matching** (a second/alt account on the same machine): it is non-primary → `OnlyTrade = 1` (`:166`), restricting it to trade-only behavior.
  - `RemoveParty(i)` (`:167`, `Server.cpp:7354`) forcibly removes the player from any party and clears their party membership (kicks summoned mobs).
  - If the alt is `USER_PLAY` **and** currently outside all villages (`Village>=5`) **and** the tile is not a no-recall attribute tile (`mapAttribute & 0x80 == 0`):
    - `SendClientMessage(i, g_pMessageStringTable[_NN_OnlyVillage])` (`:176`) — chat notice "only in village" (`_NN_OnlyVillage` = index 172, `Language.h:192`).
    - `DoRecall(i)` (`:177`, `Server.cpp:7558`) — teleport the alt back to its city/guild spawn (recall logic: city spawn, guild-zone spawn, newbie spawn, `GetEmptyMobGrid`, `MSG_Action` with `Effect=1`).

Design intent: on a shared machine/MAC, only one account is the "primary" (full party/combat privileges). When the primary logs in, all other sessions of that MAC are downgraded to `OnlyTrade=1`, pulled out of their parties, and if they're out in the field they are recalled home with a village-only notice. When a second account of the same MAC logs in (the ≤1 gate fails), no broadcast is issued and the existing primary is left untouched.

## 7. Side Effects

For each processed session `i` (`ProcessDBMessage.cpp`):

| Side effect | Source | Trigger |
|---|---|---|
| `pUser[i].OnlyTrade = 0` | `:162` | Primary (G3 true) — full trade/party rights |
| `pUser[i].OnlyTrade = 1` | `:166` | Non-primary (G3 false) — trade-only restriction |
| `RemoveParty(i)` | `:167` | Non-primary — dissolves the player's party (`Server.cpp:7354`) |
| `SendClientMessage(i, _NN_OnlyVillage)` | `:176` | Non-primary, `USER_PLAY`, outside village, non-0x80 tile — shows chat notice (`SendFunc.cpp:27-45`) |
| `DoRecall(i)` | `:177` | Same condition as above — teleports player to spawn (`Server.cpp:7558`) |

No persistent DB writes; all effects are in-memory game-server state on the TMSrv side.

## 8. Related Packets

| Packet | Constant / direction | Relationship |
|---|---|---|
| `MSG_DBAccountLogin` | `_MSG_DBAccountLogin = (3\|FLAG_GAME2DB)` (`Basedef.h:978`) | Inbound trigger that produces the broadcast (`CFileDB.cpp:782`) |
| `MSG_DBCNFAccountLogin` | `_MSG_DBCNFAccountLogin = (22\|FLAG_DB2GAME)` (`Basedef.h:1094`) | Companion reply sent back to the connecting server before the broadcast (`CFileDB.cpp:764-780`) |
| `MSG_DBPrimaryAccount` | `_MSG_DBPrimaryAccount = (23\|FLAG_GAME2DB)` = `0x823` (`Basedef.h:1085-1092`) | TMSrv→DBSrv request (gm `contaprincipal`, `_MSG_MessageWhisper.cpp:1112-1127`); DBSrv replies by re-broadcasting `MSG_DBCheckPrimaryAccount` (`CFileDB.cpp:2068-2101`) |
| `MSG_DBClientMessage` | `_MSG_DBClientMessage = (19\|FLAG_DB2GAME)` (`Basedef.h:1033`) | Confirmation message sent by the gm-command producer (`CFileDB.cpp:2092-2100`) |
| `MSG_MessagePanel` | `_MSG_DBMessagePanel`/client panel | `SendClientMessage` payload (`SendFunc.cpp:32-44`) |
| `MSG_Action` | — | `DoRecall` teleport visual (`Server.cpp:7591-7601`) |

## 9. Discrepancies & Open Questions

1. **`GetAccountsByMac` counts only logged-in accounts** (`CFileDB.cpp:2357-2360` iterates entries with `Login != 0`), not all registered accounts on the MAC. So the "≤1" primary gate depends on *concurrent logins*, not total registrations. Intent appears correct but is worth confirming against the original TM.
2. **The gm path (`_MSG_DBPrimaryAccount`) bypasses the `GetAccountsByMac` gate** (`CFileDB.cpp:2068-2101`): it unconditionally broadcasts and marks the requester's MAC primary. This can forcibly downgrade a legitimately different "primary" on the same machine. Likely intended (it is a manual override) but it is an asymmetry with the automatic path.
3. **`RemoveParty` called even when the player is not in a party** (`:167`): `RemoveParty` (Server.cpp:7354) handles `leader == 0` via the "novolider" path and early-returns on bad leader; a non-member is a no-op but still traverses the branch — harmless, minor redundancy.
4. **`DoRecall` condition** uses `BASE_GetVillage` over guild-zone bounds only; areas inside villages of the 5 guild zones return `0..4`, everything else `5` (`Basedef.cpp:4266`). The `Village < 0` branch of the guard is dead code (function never returns negative).
5. **`strncmp(..., ACCOUNTNAME_LENGTH)`** (`:160`) compares 16 bytes without requiring NUL — trailing zeros are zeroed by producers' `memset`, so equality semantics hold; a non-NUL-padded 16-byte account could theoretically match/mismatch unexpectedly, but producers zero-init.
6. **No `ID` is set** by either producer (memset to 0) — the handler relies on `conn==0` broadcast routing (`ProcessDBMessage.cpp:56`). If DBSrv ever set a per-server `ID`, the message would take the wrong branch. Currently consistent.
7. `_NN_OnlyVillage` string content is loaded from an external message table (`g_pMessageStringTable`); exact wording not resolvable from source (index 172, `Language.h:192`).

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `legacy/Code/Basedef.h` | 925-930 | `_MSG` macro (header layout) |
| `legacy/Code/Basedef.h` | 932-941 | Direction flags (`FLAG_DB2GAME = 0x0400`) |
| `legacy/Code/Basedef.h` | 1041 | `_MSG_DBCheckPrimaryAccount = (20\|FLAG_DB2GAME)` |
| `legacy/Code/Basedef.h` | 1042-1049 | `struct MSG_DBCheckPrimaryAccount` |
| `legacy/Code/Basedef.h` | 1085-1092 | `_MSG_DBPrimaryAccount` / `MSG_DBPrimaryAccount` |
| `legacy/Code/Basedef.h` | 65 | `ACCOUNTNAME_LENGTH = 16` |
| `legacy/Code/Basedef.h` | 56 | `MAX_USER = 1000` |
| `legacy/Code/Basedef.h` | 48 | `MAX_SERVER = 10` |
| `legacy/Code/Basedef.h` | 916-919 | `STRUCT_BLOCKMAC { int Mac[4]; }` |
| `legacy/Code/Basedef.cpp` | 4258-4267 | `BASE_GetVillage` |
| `legacy/Code/Basedef.cpp` | 6475 | `BASE_CheckPacket` (disabled) |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 39-60 | Dispatch entry, `conn==0` broadcast branch |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 148-182 | Handler `case _MSG_DBCheckPrimaryAccount` |
| `legacy/Code/DBSrv/CFileDB.cpp` | 782-801 | Producer (account login, `GetAccountsByMac<=1`) |
| `legacy/Code/DBSrv/CFileDB.cpp` | 2068-2101 | Producer (gm `_MSG_DBPrimaryAccount`) |
| `legacy/Code/DBSrv/CFileDB.cpp` | 2351-2367 | `GetAccountsByMac` |
| `legacy/Code/TMSrv/_MSG_MessageWhisper.cpp` | 1112-1127 | `contaprincipal` gm → `_MSG_DBPrimaryAccount` |
| `legacy/Code/TMSrv/Server.cpp` | 7354 | `RemoveParty` |
| `legacy/Code/TMSrv/Server.cpp` | 7558 | `DoRecall` |
| `legacy/Code/TMSrv/SendFunc.cpp` | 27-45 | `SendClientMessage` |
| `legacy/Code/TMSrv/GetFunc.cpp` | 1257-1273 | `GetAttribute` |
| `legacy/Code/TMSrv/CUser.h` | 93, 115 | `Mac[4]`, `OnlyTrade` |
| `legacy/Code/TMSrv/Language.h` | 192 | `_NN_OnlyVillage = 172` |
| `legacy/Code/CPSock.h` | 38-50 | `MAX_MESSAGE_SIZE`, `INITCODE`, `HEADER` |
| `legacy/Code/CPSock.cpp` | 513-591 | `AddMessage` (framing/obfuscation/checksum) |
| `legacy/Code/CPSock.cpp` | 686-693 | `SendOneMessage` |
