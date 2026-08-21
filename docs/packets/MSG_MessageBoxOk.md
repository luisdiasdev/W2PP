# MSG_MessageBoxOk

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_MessageBoxOk` = `(2 \| FLAG_GAME2CLIENT)` = `0x0102` = `258` (`Basedef.h:1201`) |
| Sequence ID | 2 |
| Direction(s) | **TMSrv → Client only** (`FLAG_GAME2CLIENT`). **No client→game leg** — `ProcessClientMessage.cpp` has no `_MSG_MessageBoxOk` case (grep: no matches). |
| DB mirror | `_MSG_DBMessageBoxOk` = `(2 \| FLAG_DB2GAME)` = `0x0402` = `1026` (`Basedef.h:977`). DB→game→client relay only; **no DBSrv producer exists** in the current tree (see §6, §9). |
| Wire struct | `MSG_MessageBoxOk` (`Basedef.h:1202-1208`) — `_MSG` header + `int Useless1` + `int Useless2` + `char String[MESSAGE_LENGTH]` |
| Total size | **116 bytes** (`sizeof(MSG_MessageBoxOk)` = 12 header + 4 + 4 + 96 payload; see §3.4) |
| Packing | **Default MSVC `/Zp8`** — NOT in any `#pragma pack(push,1)` region (pack(1) regions: `Basedef.h:808-835`, `1212-1246`, `1465-1492`, `2063-2097`; this struct at 1202-1208 falls outside all of them) |
| Handler | **None on the client leg.** Server producers/relays: `SendClientMessageOk` (`TMSrv/SendFunc.cpp:179-195`, **no callers** — dead code) and DB→game relay `case _MSG_DBMessageBoxOk` (`TMSrv/ProcessDBMessage.cpp:1099-1108`). |
| Aliases | Sequence 2 is shared by the game→client constant (`_MSG_MessageBoxOk`, `Basedef.h:1201`) and the DB mirror (`_MSG_DBMessageBoxOk`, `Basedef.h:977`) — distinguished only by their flag bits (0x0100 vs 0x0400). Structurally **identical** in shape to the sibling `_MSG_MessagePanel` (seq 1, `MSG_MessagePanel`, `Basedef.h:1195-1199`) except for the two leading `int` fields. |
| Related | `_MSG_MessagePanel` `(1\|GAME2CLIENT)`=`0x0101` + DB mirror `_MSG_DBMessagePanel` (seq 1, `Basedef.h:976`); `_MSG_DBClientMessage` `(19\|DB2GAME)`=`0x0413` (`MSG_DBClientMessage`, `Basedef.h:1034-1039`) — the other "DB pushes text to client" channel. |

## 2. Wire Framing

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with 4-byte `INITCODE = 0x1F11F311` magic before any framed message (`CPSock.h:40`, sent `CPSock.cpp:249-250`, validated `CPSock.cpp:366-383`).
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating transform keyed by `KeyWord` (index into shared `pKeyWord[512]`, `CPSock.cpp:29`); send transform at `CPSock.cpp:558-581`, inverse (receive) at `CPSock.cpp:430-453`.
- `CheckSum` = `Sum2 - Sum1` (transformed vs. raw payload sums); computed on send (`CPSock.cpp:583-584`), validated on receive (`CPSock.cpp:455-464`).
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE=8192]` else the receive buffer is reset (`CPSock.cpp:397-406`); `HEADER` = the `_MSG` field block (`CPSock.h:42-50`).
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is **disabled** (body commented out, returns `FALSE`) — but the commented-out central size validation for both constants was `m->Size != sizeof(MSG_MessageBoxOk)` → `code = 1` (`Basedef.cpp:6483` for game leg, `Basedef.cpp:6555` for DB mirror).

Per-packet notes:
- No deviation from standard framing. It is a server→client "OK message box" frame carried on the **client socket** (`pUser[conn].cSock`).
- `Size` is expected to be `sizeof(MSG_MessageBoxOk)` = 116. Set by `AddMessage` from the passed `Size` arg (`CPSock.cpp:538`), and both producers/relays pass `sizeof(MSG_MessageBoxOk)` (see §3.4).
- The DB→game relay rewrites `Type` to the client-facing `_MSG_MessageBoxOk` and `ID` to `0` before forwarding (`ProcessDBMessage.cpp:1103-1104`).

## 3. Binary Layout

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 116) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_MessageBoxOk` = 0x0102 (set by producer/relay; on the DB wire it arrives as 0x0402) |
| 6 | 2 | `ID` | `short` | Target client slot; relay sets to 0 (`ProcessDBMessage.cpp:1104`) |
| 8 | 4 | `ClientTick` | `unsigned int` | Set to `CurrentTime` by `AddMessage` (`CPSock.cpp:541`) |

### 3.2 Payload

Packing context: **default `/Zp8`** (not a pack(1) region — see §1). Each member aligned to `min(sizeof(member), 8)`; the struct size rounds up to the largest member alignment (4, from `int`/`unsigned int`).

Header ends at offset 12, which is already 4-aligned, so `int Useless1` starts at 12 with no padding. `Useless1` (align 4) then `Useless2` (align 4) each consume 4 bytes, ending at 20. `char String[96]` has alignment 1, so it starts at 20 with no padding.

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 4 | `Useless1` | `int` | 4 | 0 | Unused. Commented `//Useless` on the builder (`SendFunc.cpp:179`). Copied through, **never read** anywhere in the server code (see §9). |
| 16 | 4 | `Useless2` | `int` | 4 | 0 | Unused. Same treatment as `Useless1`. |
| 20 | 96 | `String` | `char[MESSAGE_LENGTH]` | 1 | 0 | Message-box text (`MESSAGE_LENGTH = 96`, `Basedef.h:129`). Copied with `memcpy(..., MESSAGE_LENGTH)` (`SendFunc.cpp:189`); buffer is zero-filled first (`memset`, `SendFunc.cpp:185`) so it is implicitly NUL-terminated. |

**Total payload: 4 + 4 + 96 = 104 bytes → total struct 116 bytes.**

### 3.3 Nested struct expansions

`MSG_MessageBoxOk` embeds only the `_MSG` header macro (primitive fields) plus two flat `int` fields and a flat `char[96]` array. It contains **no `STRUCT_*` members**, so there are no nested expansions. The `_MSG` macro expands to `short Size; char KeyWord; char CheckSum; short Type; short ID; unsigned int ClientTick;` (`Basedef.h:925-930`).

### 3.4 Size verification

| Check | Value | Source |
|---|---|---|
| Header (`_MSG`) | `short(2)+char(1)+char(1)+short(2)+short(2)+unsigned int(4)` = 12 | `Basedef.h:925-930` |
| `Useless1` | `int` = 4 @ offset 12 (already 4-aligned → no pad) | `Basedef.h:1205` |
| `Useless2` | `int` = 4 @ offset 16 (4-aligned → no pad) | `Basedef.h:1206` |
| `String` | `char[96]` = 96 @ offset 20 (align 1 → no pad) | `Basedef.h:1207` |
| `sizeof(MSG_MessageBoxOk)` | 12 + 4 + 4 + 96 = **116**; 116 % 4 = 0 → no tail padding | — |
| `Size` field set to | `sizeof(MSG_MessageBoxOk)` = 116 in the builder (`SendFunc.cpp:194`) and the relay (`ProcessDBMessage.cpp:1106`) | — |
| Central (disabled) check expected | `m->Size == sizeof(MSG_MessageBoxOk)` = 116 (both `_MSG_MessageBoxOk` and `_MSG_DBMessageBoxOk`) | `Basedef.cpp:6483`, `Basedef.cpp:6555` |

No mismatch between build size (116), the (disabled) central check (116), and the computed layout (116).

## 4. Lifecycle & Flow

### 4.1 DB → Game → Client (relay leg)

The only **live** path in the codebase is the DB mirror relay, but it is **never triggered** because no DBSrv code emits `_MSG_DBMessageBoxOk` (see §6, §9). When it *would* fire:

```
DBSrv ── _MSG_DBMessageBoxOk (0x0402, Size=116) ──► DBServerSocket ──► Server.cpp:3914 ProcessDBMessage(Msg)
   ──► ProcessDBMessage.cpp:39
        ├─ guard: Type has FLAG_DB2GAME && 0 <= ID < MAX_USER   (ProcessDBMessage.cpp:43)
        └─ conn = std->ID                                        (:54)   // target client slot on game
             switch(Type) → case _MSG_DBMessageBoxOk            (:1099)
                  MSG_MessageBoxOk *m = (MSG_MessageBoxOk*)Msg   (:1101)
                  m->Type = _MSG_MessageBoxOk  (0x0102)          (:1103)
                  m->ID = 0                                       (:1104)
                  pUser[conn].cSock.SendOneMessage((char*)m, sizeof(MSG_MessageBoxOk))  (:1106)
                       └─ AddMessage(...) + SendMessageA()       (CPSock.cpp:686-693)
                            ├─ sets Size=116, KeyWord=rand, CheckSum, ClientTick  (CPSock.cpp:538-542)
                            └─ obfuscates payload from offset 4  (CPSock.cpp:558-581)
Client ◄── MSG_MessageBoxOk (0x0102, Size=116) ──┘
```

`conn = std->ID` is bounded to `[0, MAX_USER)` by the guard at `ProcessDBMessage.cpp:43`, and it is used directly as the client slot (`pUser[conn].cSock`), confirming the DB `ID` field doubles as the target game-server client index for this relay. The relay reuses the incoming DB buffer in place, only rewriting `Type` and `ID`; `Useless1`, `Useless2`, and `String` are passed through untouched.

### 4.2 Game → Client (direct builder — dead code)

`SendClientMessageOk(int conn, char *Message, int Useless1, int Useless2)` (`SendFunc.cpp:179-195`, declared `SendFunc.h:28`) is the **only** builder of a fresh `MSG_MessageBoxOk` on the game server. It is **never called** anywhere in the tree (grep: only the definition and the declaration match). It:

1. Guards `conn <= 0 || conn >= MAX_USER` → return (`SendFunc.cpp:181-182`).
2. `memset(&sm_mbo, 0, sizeof(MSG_MessageBoxOk))` (`:185`).
3. `sm_mbo.Type = _MSG_MessageBoxOk` (`:187`).
4. `memcpy(sm_mbo.String, Message, MESSAGE_LENGTH)` (`:189`; implicitly NUL-terminated by the zero-fill — note it does **not** force `[94]/[95]=0` like `SendClientMessage` does for the panel, `SendFunc.cpp:41-42`).
5. Sets `Useless1`/`Useless2` from args (`:191-192`).
6. `pUser[conn].cSock.AddMessage((char*)&sm_mbo, sizeof(MSG_MessageBoxOk))` (`:194`).

### 4.3 Client → Game leg

**None.** `ProcessClientMessage.cpp` contains no `_MSG_MessageBoxOk` case (grep: no matches), and the packet is `FLAG_GAME2CLIENT` only. The client has no inbound (server-ward) leg for this frame; it is strictly server→client.

## 5. Validation & Guards

| # | Guard / check | Location | Behavior on failure |
|---|---|---|---|
| 1 | `Type & FLAG_DB2GAME` and `0 <= ID < MAX_USER` | `ProcessDBMessage.cpp:43` | Logs `"err,packet Type:... Size:... KeyWord:..."` and returns (before dispatch) |
| 2 | `conn = std->ID` bounds (derived from #1) | `ProcessDBMessage.cpp:54` | — |
| 3 | `conn <= 0 || conn >= MAX_USER` (builder only) | `SendFunc.cpp:181` | returns, no send |
| 4 | `Size` within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` (transport) | `CPSock.cpp:397` | receive buffer reset, `ErrorCode=2` |
| 5 | `CheckSum == Sum2 - Sum1` (transport) | `CPSock.cpp:458` | `ErrorCode=1` set, packet still returned |
| 6 | `BASE_CheckPacket` size check (`m->Size != sizeof(MSG_MessageBoxOk)`) | `Basedef.cpp:6483` (game), `:6555` (DB) | **Disabled** — body commented out; no runtime effect |

There is **no in-handler size validation** on either leg: the relay casts `Msg` to `MSG_MessageBoxOk` and forwards `sizeof(MSG_MessageBoxOk)` bytes without checking the received `Size` (`ProcessDBMessage.cpp:1101-1106`).

## 6. Game Mechanics & Business Logic

- **Purpose:** instruct the client to display a modal **OK** message box whose text is `String` (96 bytes). This is the "pop a dialog the user must acknowledge with OK" variant, in contrast to the non-modal panel text of `_MSG_MessagePanel` (seq 1).
- **Who produces it:** In the current tree there is **no live producer**.
  - `SendClientMessageOk` (`SendFunc.cpp:179`) can build it but has **zero callers** — dead code, so no in-game GM/system message currently routes through it.
  - `_MSG_DBMessageBoxOk` is consumed on the game server (`ProcessDBMessage.cpp:1099`) but is **never emitted** by `DBSrv` — `CFileDB.cpp` and `Server.cpp` contain no `MSG_MessageBoxOk`/`_MSG_DBMessageBoxOk` construction. The DB sibling `_MSG_DBMessagePanel` *is* emitted via `CFileDB::SendDBMessage` (`CFileDB.cpp:2170-2183`) using `MSG_MessagePanel`, but there is no equivalent `MessageBoxOk` function on the DB side.
- **`Useless1` / `Useless2`:** Named "Useless" in the builder comment (`SendFunc.cpp:179`) and never read by any server code. Their meaning is **UNKNOWN** (§9). They are not mere padding — they occupy real wire bytes at offsets 12 and 16 and are preserved through the DB relay. The client presumably interprets or ignores them (client source is not in this repo).
- **Semantics of `String`:** plain text; zero-filled on build; up to 96 bytes. No escaping, no command parsing on the server.
- No timers, cooldowns, or state transitions are attached to this packet in server code.

## 7. Side Effects

- **Outgoing packets:** the relay re-sends the received frame verbatim (after `Type`/`ID` rewrite) to a single client via `SendOneMessage` (`ProcessDBMessage.cpp:1106`); the builder adds to a single client's send buffer (`SendFunc.cpp:194`). No multicast, no broadcast.
- **DB relays:** the game→client leg never relays back to the DB. The DB→game leg is itself the relay entry.
- **Logs:** none in the handler. Only the pre-dispatch guard logs on malformed DB frames (`ProcessDBMessage.cpp:47-49`).
- **`pUser` state:** no mutations. The packet is read-only w.r.t. `pUser`/`pMob`; `conn` is only used to select the target socket.
- **Transport:** each send re-frames with a fresh `KeyWord`/`CheckSum`/`ClientTick` (`CPSock.cpp:535-542`).

## 8. Related Packets

| Packet | Const (hex) | Struct | Relation |
|---|---|---|---|
| `_MSG_MessagePanel` | `0x0101` (`Basedef.h:1194`) | `MSG_MessagePanel` (`:1195-1199`) | Sibling game→client text frame; same seq family (1 vs 2), **no** leading int fields. Built by `SendClientMessage` (`SendFunc.cpp:27-45`). |
| `_MSG_DBMessagePanel` | `0x0401` (`Basedef.h:976`) | `MSG_MessagePanel` | DB mirror of the panel; emitted by `CFileDB::SendDBMessage` (`CFileDB.cpp:2170-2183`). |
| `_MSG_DBMessageBoxOk` | `0x0402` (`Basedef.h:977`) | `MSG_MessageBoxOk` | DB mirror of this packet (relay target `ProcessDBMessage.cpp:1099`). |
| `_MSG_DBClientMessage` | `0x0413` (`Basedef.h:1033`) | `MSG_DBClientMessage` (`:1034-1039`) | Alternate "DB pushes 96-byte text to client" channel (`String` only, no int fields). |

## 9. Discrepancies & Open Questions

- **`Useless1` / `Useless2` semantics — UNKNOWN.** Declared `int` at `Basedef.h:1205-1206`, passed through the builder (`SendFunc.cpp:191-192`) and preserved by the relay, but **never read** by any server code. No server-side meaning can be determined; the client-side interpretation is outside this repo. Named "Useless" in `SendFunc.cpp:179`, suggesting legacy/reserved fields (possibly original-client dialog flags/params).
- **No live producer.** Despite full plumbing (builder + relay), nothing in the current tree actually sends `MSG_MessageBoxOk`. The DB→game relay is dormant (no DBSrv emitter), and the direct builder is dead code. If behavior is expected, this is a gap.
- **`String` termination.** The builder relies on zero-fill for termination and does **not** force `String[94]=String[95]=0` (contrast `SendFunc.cpp:41-42` for `MSG_MessagePanel`). If a future caller passes a full 96-byte message, termination is still guaranteed by the `memset`, but this is a latent inconsistency.
- **`ID` semantics on the DB→game channel.** The same `ID` field serves both as the DB-server connection selector (for `conn == 0` system cases, `ProcessDBMessage.cpp:56`) and as the target client slot for client-targeted relays (this packet). This dual role is implicit and not documented in the protocol.
- **Disabled size validation.** `BASE_CheckPacket` (`Basedef.cpp:6475`) is compiled out; the relay does not independently verify `Size`, so a malformed 0x0402 frame would be forwarded at a wrong length (bounded only by the transport `MAX_MESSAGE_SIZE` check).

## 10. Source References

- `legacy/Code/Basedef.h:129` — `#define MESSAGE_LENGTH 96`
- `legacy/Code/Basedef.h:925-930` — `_MSG` header macro
- `legacy/Code/Basedef.h:932-941` — FLAG_* constants (`FLAG_GAME2CLIENT=0x0100`, `FLAG_DB2GAME=0x0400`)
- `legacy/Code/Basedef.h:976-977` — `_MSG_DBMessagePanel`, `_MSG_DBMessageBoxOk`
- `legacy/Code/Basedef.h:1195-1199` — `MSG_MessagePanel`
- `legacy/Code/Basedef.h:1201-1208` — `_MSG_MessageBoxOk`, `MSG_MessageBoxOk`
- `legacy/Code/Basedef.cpp:6483` — disabled size check for `_MSG_MessageBoxOk`
- `legacy/Code/Basedef.cpp:6555` — disabled size check for `_MSG_DBMessageBoxOk`
- `legacy/Code/CPSock.h:38-50` — `MAX_MESSAGE_SIZE`, `INITCODE`, `HEADER`
- `legacy/Code/CPSock.cpp:29` — `pKeyWord[512]`
- `legacy/Code/CPSock.cpp:353-467` — `ReadMessage` (receive: init, size, deobfuscate, checksum)
- `legacy/Code/CPSock.cpp:513-591` — `AddMessage` (send: frame, obfuscate, checksum)
- `legacy/Code/CPSock.cpp:686-693` — `SendOneMessage`
- `legacy/Code/TMSrv/ProcessDBMessage.cpp:39-54` — `ProcessDBMessage` entry + guards
- `legacy/Code/TMSrv/ProcessDBMessage.cpp:1099-1108` — `case _MSG_DBMessageBoxOk` relay
- `legacy/Code/TMSrv/SendFunc.cpp:179-195` — `SendClientMessageOk` builder (no callers)
- `legacy/Code/TMSrv/SendFunc.h:28` — `SendClientMessageOk` declaration
- `legacy/Code/TMSrv/Server.cpp:3914` — `ProcessDBMessage(Msg)` invocation (DB socket)
- `legacy/Code/TMSrv/ProcessClientMessage.cpp` — verified: no `_MSG_MessageBoxOk` case
- `legacy/Code/DBSrv/CFileDB.cpp:2170-2183` — `CFileDB::SendDBMessage` (`_MSG_DBMessagePanel` sibling)
- `legacy/Code/DBSrv/CFileDB.cpp`, `DBSrv/Server.cpp` — verified: no `_MSG_DBMessageBoxOk` producer
