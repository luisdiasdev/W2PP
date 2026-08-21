# MSG_MessagePanel

## 1. Summary

| Attribute | Value |
|---|---|
| Packet name | `MSG_MessagePanel` |
| Type constant | `_MSG_MessagePanel` = `(1 \| FLAG_GAME2CLIENT)` = `(1 \| 0x0100)` = **0x101** (Basedef.h:1194) |
| Wire struct | `MSG_MessagePanel` (Basedef.h:1195-1199) |
| Payload | `char String[128]` |
| Total size (Size field) | **140** = `sizeof(MSG_MessagePanel)` |
| Direction | **TMSrv → client** (server → game client) |
| Producer | `TMSrv::SendClientMessage` (SendFunc.cpp:27-45); also `DBSrv::CFileDB::SendDBMessage` (CFileDB.cpp:2170-2183) for the DB mirror |
| Handler (client→game) | **None** — no inbound leg exists |
| Handler (DB→game relay) | **None** — `case _MSG_DBMessagePanel` absent from ProcessDBMessage.cpp |
| DB mirror | `_MSG_DBMessagePanel` = `(1 \| FLAG_DB2GAME)` = `(1 \| 0x0400)` = **0x401** (Basedef.h:976), same wire struct |
| Aliases / related | `_MSG_MessageBoxOk` = 0x102 (Basedef.h:1201), `_MSG_DBMessageBoxOk` = 0x402 (Basedef.h:977) — share shape but separate struct |

The packet is a one-way "on-screen message panel / message-box text" notification sent by the game
server to a single connected client. It carries an arbitrary display string. It has **no inbound
client leg** (GAME2CLIENT-only) and its DB mirror (`_MSG_DBMessagePanel`) has **no handler** in the
game server, making the DB→game relay effectively dead code (see §9).

## 2. Wire Framing

Protocol preamble (verified in CPSock.cpp, CPSock.h):

1. **Connection handshake** — sender writes the 4-byte magic `INITCODE = 0x1F11F311`
   (CPSock.h:40) as the first 4 bytes of the stream (CPSock.cpp:249-250). Receiver validates it once
   (CPSock.cpp:371-383); first 4 bytes are consumed before any packet is read.
2. **Per-packet framing** (`CPSock::ReadMessage`, CPSock.cpp:353-467):
   - `Size` (u16) at offset 0 must satisfy `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE(8192)`
     (CPSock.cpp:397, CPSock.h:38). `Size` is the **total** packet length incl. the 12-byte header.
   - `KeyWord` byte at offset 2 indexes the crypto table `pKeyWord[iKeyWord*2]`; payload bytes from
     offset 4 are obfuscated position-rotating per-byte (CPSock.cpp:430-453). For the send path the
     inverse transform is applied (AddMessage, CPSock.cpp:558-581). KeyWord is set to `rand()%256` on
     send (CPSock.cpp:535).
   - `CheckSum` byte at offset 3 = `Sum2 - Sum1` (mod 256) over payload bytes (CPSock.cpp:425-455 for
     receive; 554-583 for send).
   - `ClientTick` is stamped with `CurrentTime` on send (CPSock.cpp:541).
3. **Dispatch**: TMSrv reads with `CPSock::ReadMessage`, then routes by `Type`: client-facing
   `ProcessClientMessage` switch (ProcessClientMessage.cpp:66) for CLIENT2GAME; DB-facing
   `ProcessDBMessage` switch (ProcessDBMessage.cpp:58, 210) for DB2GAME/GAME2DB. Neither switch has a
   `default:` case; unknown types are silently ignored.
4. **BASE_CheckPacket (Basedef.cpp:6475) is DISABLED** — the whole body is commented out (`/*` at
   6476). Its size checks for `_MSG_MessagePanel` (Basedef.cpp:6482) and `_MSG_DBMessagePanel`
   (Basedef.cpp:6554) would require `Size == sizeof(MSG_MessagePanel)`, but are never executed.

**Per-packet deviations for MSG_MessagePanel:** none. It uses the standard header + 12-byte preamble,
no custom framing.

## 3. Binary Layout

`MSG_MessagePanel` (Basedef.h:1195-1199) is **outside** every `#pragma pack(push,1)` region
(pack(1) regions are only Basedef.h:808-835, 1212-1246, 1465-1492, 2063-2097), so it uses the MSVC
default **/Zp8** alignment. Target: little-endian x86, LP32.

### 3.1 Header

Header = `_MSG` macro (Basedef.h:925-930), 12 bytes:

| Field | Offset | Size | Type | Align | Pad | Semantics |
|---|---|---|---|---|---|---|
| `Size` | 0 | 2 | `short` | 2 | 0 | Total packet size incl. header = 140 |
| `KeyWord` | 2 | 1 | `char` | 1 | 0 | Crypto table index (set on send, CPSock.cpp:539) |
| `CheckSum` | 3 | 1 | `char` | 1 | 0 | `Sum2 - Sum1` obfuscation checksum |
| `Type` | 4 | 2 | `short` | 2 | 0 | `_MSG_MessagePanel` = 0x101 (send); overwritten on DB relay |
| `ID` | 6 | 2 | `short` | 2 | 0 | Client slot index (target user) |
| `ClientTick` | 8 | 4 | `unsigned int` | 4 | 0 | Timestamp stamped on send (CPSock.cpp:541) |

### 3.2 Payload

| Field | Offset | Size | Type | Align | Pad | Semantics |
|---|---|---|---|---|---|---|
| `String[128]` | 12 | 128 | `char[128]` | 1 | 0 | Display text; only first 96 bytes (MESSAGE_LENGTH) are ever populated by producers |

### 3.3 Nested struct expansions

None. `String` is a flat `char[128]`; no nested structs or unions.

### 3.4 Size verification

`/Zp8` layout math:

- `Size` @0 .. 1 (offset 2) — align 2 ✓
- `KeyWord` @2 (offset 3) — align 1 ✓
- `CheckSum` @3 (offset 4) — align 1 ✓
- `Type` @4 .. 5 (offset 6) — align 2 ✓
- `ID` @6 .. 7 (offset 8) — align 2 ✓
- `ClientTick` @8 .. 11 (offset 12) — align 4 ✓
- `String[128]` @12 .. 139 — align 1 ✓

`12 (header) + 128 (String) = 140 bytes`. Largest natural alignment is 4 (`unsigned int ClientTick`),
so struct alignment = 4; 140 is already a multiple of 4, so **no trailing padding**.

- `sizeof(MSG_MessagePanel)` = **140**
- Producer sets `sm.Size = sizeof(MSG_MessagePanel)` = 140 (SendFunc.cpp:35, CFileDB.cpp:2176) ✓
- Disabled check would require `Size == sizeof(MSG_MessagePanel)` = 140 (Basedef.cpp:6482, 6554) ✓
- No packing mismatch; report says MESSAGE_LENGTH=96 is the copy/truncation bound, not the buffer
  size (the `String[128]` comment "Correct size to fix SendScore Hp Bug", Basedef.h:1198).

## 4. Lifecycle & Flow

### Game → client (the only live leg)

Producer: `TMSrv::SendClientMessage(int conn, char *Message)` (SendFunc.cpp:27-45):

1. Guard `conn <= 0 || conn >= MAX_USER` → early return (SendFunc.cpp:29-30).
2. Zero `MSG_MessagePanel sm_mp` (SendFunc.cpp:33).
3. `sm_mp.Size = sizeof(MSG_MessagePanel)` (140), `sm_mp.Type = _MSG_MessagePanel` (0x101),
   `sm_mp.ID = 0` (SendFunc.cpp:35-37). Note: `ID` is set to 0 here, not the connection index.
4. `memcpy(sm_mp.String, Message, MESSAGE_LENGTH)` copies at most 96 bytes into the 128-byte buffer
   (SendFunc.cpp:39); then hard-nulls `String[94]` and `String[95]` (indices `MESSAGE_LENGTH-1`,
   `MESSAGE_LENGTH-2`, SendFunc.cpp:41-42) so the last two bytes are always 0.
5. `pUser[conn].cSock.AddMessage(&sm_mp, sizeof(MSG_MessagePanel))` (SendFunc.cpp:44) — queues into
   the per-connection send buffer and obfuscates.

`SendClientMessage` is the workhorse for all on-screen notifications — called from **47 TMSrv files**
(e.g. `_MSG_AccountLogin.cpp:33,48,58,86`, `_MSG_CombineItemOdin.cpp`, `_MSG_GetItem.cpp`,
`MobKilled.cpp`, `ProcessSecMinTimer.cpp:180`). Callers pass either a formatted `temp[]` buffer or a
`g_pMessageStringTable[_NN_*]` localized string.

Flow diagram:

```
[Game logic] --call--> SendClientMessage(conn, msg)   (SendFunc.cpp:27)
   |  guard conn in [1, MAX_USER)
   |  fill MSG_MessagePanel { Size=140, Type=0x101, ID=0, String=msg }
   v
pUser[conn].cSock.AddMessage(...)   (CPSock.cpp:513)  -> obfuscate payload, compute CheckSum, stamp ClientTick
   v
CPSock::SendMessageA()  (CPSock.cpp:617)  -> send() over socket
   v
[Client] --ReadMessage--> deobfuscate (CPSock.cpp:353)  -> renders panel text from String
```

There is **no client→game leg**: the client never sends `_MSG_MessagePanel` (0x101) back, and
`ProcessClientMessage` (switch at ProcessClientMessage.cpp:66) contains no `case _MSG_MessagePanel`.

### DB → game (dead relay)

Producer: `DBSrv::CFileDB::SendDBMessage(int svr, unsigned short id, char *msg)`
(CFileDB.cpp:2170-2183):

1. Fills `MSG_MessagePanel sm` with `Type = _MSG_DBMessagePanel` (0x401), `ID = id`,
   `Size = sizeof(MSG_MessagePanel)`, `strncpy(sm.String, msg, MESSAGE_LENGTH)` (CFileDB.cpp:2174-2178).
2. `pUser[svr].cSock.SendOneMessage(&sm, sizeof(MSG_MessagePanel))` (CFileDB.cpp:2180) — sends to the
   game server.

However:
- **`SendDBMessage` is never called** anywhere in DBSrv (grep across the tree finds only its
  declaration CFileDB.h:55 and definition CFileDB.cpp:2170) → it is dead code.
- **No `case _MSG_DBMessagePanel` exists** in `ProcessDBMessage.cpp` (the switch at 58/210 has no
  such case; the nearest is `case _MSG_DBMessageBoxOk` at 1099, which relays MessageBoxOk, not the
  panel). A DB-originated `0x401` packet would fall through with no handler and be dropped silently
  (no `default:` case).

So the only live producer is `SendClientMessage` on the game server; the DB mirror is unreachable.

## 5. Validation & Guards

Execution order in the game→client producer `SendClientMessage` (SendFunc.cpp:27-45):

| # | Guard / check | Location | Behavior on fail |
|---|---|---|---|
| 1 | `conn <= 0` | SendFunc.cpp:29 | Return, nothing sent |
| 2 | `conn >= MAX_USER` | SendFunc.cpp:29 | Return, nothing sent |
| 3 | `nSendPosition + Size >= SEND_BUFFER_SIZE` | CPSock.cpp:518 (AddMessage) | Log "err,add buffer full", return FALSE, drop |
| 4 | `Sock <= 0` | CPSock.cpp:527 (AddMessage) | Log "err,add buffer invalid", return FALSE, drop |
| 5 | `Sock <= 0` / buffer overrun | CPSock.cpp:623-656 (SendMessageA) | Drop and reset send buffer |

Dispatcher-level validation: `CPSock::ReadMessage` enforces `Size` bounds (CPSock.cpp:397) and
checksum equality (CPSock.cpp:458) but still returns the packet when checksum mismatches (with
`ErrorCode=1`). `BASE_CheckPacket` size validation is **disabled** (Basedef.cpp:6475).

There are **no business-layer guards** for the panel content itself: any non-null `Message` is
copied (bounded to 96 bytes) and displayed. No `pUser[conn].Mode` check, no permission/level check.

## 6. Game Mechanics & Business Logic

- **Purpose:** display an on-screen message panel / message-box text to a single client. It is the
  generic "toast"/notification channel distinct from `_MSG_MessageBoxOk` (a modal OK box carrying
  `Useless1`/`Useless2` + `String[96]`).
- **When sent:** whenever game logic wants to inform the player — login errors
  (`_MSG_AccountLogin.cpp:33,48,58,86`), wrong combinations (`_MSG_CombineItemOdin.cpp:152,363,378,...`),
  item/skill results (`_MSG_GetItem.cpp:99,103`), party/guild events (`_MSG_InviteGuild.cpp:60,90`),
  periodic billing (`ProcessSecMinTimer.cpp:180`), etc.
- **Message formatting:** callers pre-format into a `temp[]` buffer (via `sprintf`, frequently using
  a localized `g_pMessageStringTable[_NN_*]` string) or pass the localized string directly
  (e.g. `_MSG_AccountLogin.cpp:32,47,86`).
- **Truncation semantics:** the copy is bounded to `MESSAGE_LENGTH` (96) bytes (SendFunc.cpp:39),
  and the final two bytes `String[94]`,`String[95]` are forced to 0 (SendFunc.cpp:41-42). So at most
  94 non-null display characters can be delivered even though the wire buffer is 128 bytes.
- **No server-side response:** the client does not acknowledge this packet; there is no paired
  confirmation.

## 7. Side Effects

- **Outgoing packets:** none triggered directly. The packet is a terminal notification; it does not
  cause the server to send any other packet.
- **DB relays:** none (the `_MSG_DBMessagePanel` DB mirror is not relayed to the client — no case in
  ProcessDBMessage; contrast `case _MSG_DBMessageBoxOk` which rewrites Type to `_MSG_MessageBoxOk`
  and relays, ProcessDBMessage.cpp:1099-1108).
- **Logs:** only transport-level logs on buffer/socket failure (CPSock.cpp:520, 529); no business
  logging of panel content.
- **pUser state:** none modified. `SendClientMessage` only writes to `pUser[conn].cSock`'s send
  buffer; `pUser[conn].Mode` and all other state are untouched. (The `ID=0` field is set but unused
  by the receiver, since there is no receiver on the server side.)
- **Persistence:** none (no DB write).

## 8. Related Packets

| Packet | Type | Relation |
|---|---|---|
| `_MSG_MessageBoxOk` / `MSG_MessageBoxOk` | 0x102 (Basedef.h:1201) | Sibling display packet: modal OK box with `Useless1`,`Useless2`,`String[96]`. Shares the display role but is a separate struct (1202-1208). |
| `_MSG_DBMessagePanel` / `MSG_MessagePanel` | 0x401 (Basedef.h:976) | DB→game mirror of the same wire struct; producer exists (CFileDB.cpp:2170) but is unused and unhandled. |
| `_MSG_DBMessageBoxOk` / `MSG_MessageBoxOk` | 0x402 (Basedef.h:977) | DB→game mirror that **is** relayed: ProcessDBMessage.cpp:1099 rewrites `Type` to `_MSG_MessageBoxOk` and forwards to client. |

## 9. Discrepancies & Open Questions

1. **Dead DB mirror:** `_MSG_DBMessagePanel` (0x401) is produced by `SendDBMessage` (CFileDB.cpp:2170)
   but (a) `SendDBMessage` has zero callers and (b) TMSrv has no `case _MSG_DBMessagePanel`. Any
   0x401 packet arriving at the game server is silently dropped. Likely an abandoned/legacy path.
2. **`ID` never used:** `SendClientMessage` sets `sm_mp.ID = 0` (SendFunc.cpp:37) while the real
   target is the `conn` argument used only to select `pUser[conn].cSock` (SendFunc.cpp:44). The wire
   `ID` field carries no routing meaning on the send side.
3. **Buffer size vs truncation mismatch:** wire buffer is 128 bytes (`String[128]`, Basedef.h:1198)
   but producers only ever fill 96 (MESSAGE_LENGTH) and force the last two to 0. The 128-byte size is
   intentional per the code comment ("Correct size to fix SendScore Hp Bug"); the extra 32 bytes are
   padding for safety, never populated.
4. **MESSAGE_LENGTH constant (96) vs String[128]:** the disabled size check and all producers use
   `sizeof(MSG_MessagePanel)`=140, which is consistent. No active inconsistency, but note the copy
   length (96) differs from the field capacity (128) by design.
5. **No client acknowledgement** — the display result (e.g. whether the panel rendered) is never
   verified by the server.
6. **Open question:** whether any out-of-tree client depends on `_MSG_DBMessagePanel` semantics; in
   this source tree it is unreachable.

## 10. Source References

| File | Lines | Content |
|---|---|---|
| legacy/Code/Basedef.h | 1194 | `_MSG_MessagePanel = (1 \| FLAG_GAME2CLIENT)` |
| legacy/Code/Basedef.h | 1195-1199 | `struct MSG_MessagePanel { _MSG; char String[128]; }` |
| legacy/Code/Basedef.h | 976 | `_MSG_DBMessagePanel = (1 \| FLAG_DB2GAME)` |
| legacy/Code/Basedef.h | 1201-1208 | `_MSG_MessageBoxOk` / `MSG_MessageBoxOk` |
| legacy/Code/Basedef.h | 977 | `_MSG_DBMessageBoxOk` |
| legacy/Code/Basedef.h | 925-930 | `_MSG` header macro |
| legacy/Code/Basedef.h | 932-941 | FLAG_* constants |
| legacy/Code/Basedef.h | 129 | `MESSAGE_LENGTH = 96` |
| legacy/Code/TMSrv/SendFunc.cpp | 27-45 | `SendClientMessage` producer (game→client) |
| legacy/Code/DBSrv/CFileDB.cpp | 2170-2183 | `SendDBMessage` producer (DB→game, dead) |
| legacy/Code/DBSrv/CFileDB.h | 55 | `SendDBMessage` declaration |
| legacy/Code/TMSrv/ProcessDBMessage.cpp | 58, 210 | DB message switch (no `_MSG_DBMessagePanel` case) |
| legacy/Code/TMSrv/ProcessDBMessage.cpp | 1099-1108 | `case _MSG_DBMessageBoxOk` relay (contrast) |
| legacy/Code/TMSrv/ProcessClientMessage.cpp | 66 | Client message switch (no `_MSG_MessagePanel` case) |
| legacy/Code/CPSock.cpp | 513-591 | `AddMessage` — obfuscation, checksum, stamping |
| legacy/Code/CPSock.cpp | 617-684 | `SendMessageA` — flush send buffer |
| legacy/Code/CPSock.cpp | 353-467 | `ReadMessage` — deobfuscation + validation |
| legacy/Code/CPSock.cpp | 249-250, 371-383 | INITCODE handshake |
| legacy/Code/CPSock.h | 40 | `INITCODE 0x1F11F311` |
| legacy/Code/CPSock.h | 38 | `MAX_MESSAGE_SIZE 8192` |
| legacy/Code/Basedef.cpp | 6475 | `BASE_CheckPacket` — **disabled** (body commented) |
| legacy/Code/Basedef.cpp | 6482, 6554 | Size checks for `_MSG_MessagePanel` / `_MSG_DBMessagePanel` |
