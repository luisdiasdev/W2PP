# MSG_MessageChat

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_MessageChat` = `(51 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME)` = `0x0333` = `819` (`Basedef.h:1439`) |
| Sequence ID | 51 |
| Direction(s) | Bidirectional Client ↔ TMSrv (`FLAG_GAME2CLIENT` \| `FLAG_CLIENT2GAME`). Same struct and same `Type` value in both directions. No DB relay. |
| Wire struct | `MSG_MessageChat` (`Basedef.h:1440-1444`) — `_MSG` header + `char String[MESSAGE_LENGTH]` |
| Total size | **108 bytes** (`sizeof(MSG_MessageChat)` = 12 header + 96 payload; see §3.4) |
| Packing | **Default MSVC `/Zp8`** — NOT in any `#pragma pack(push,1)` region (pack(1) regions: `Basedef.h:808-835`, `1212-1246`, `1465-1492`, `2063-2097`; this struct at 1440-1444 falls outside all of them) |
| Handler | `Exec_MSG_MessageChat` @ `TMSrv/_MSG_MessageChat.cpp:20`; dispatched from `case _MSG_MessageChat` @ `TMSrv/ProcessClientMessage.cpp:92-94` |
| Aliases | Low byte 51 is used **only** by `_MSG_MessageChat` — no other constant shares it. Note: 51 is the only difference from the neighboring `_MSG_MessageWhisper` (52, `Basedef.h:1447`) which is a structurally different packet. |
| Related | `_MSG_MessageWhisper` `(52\|GAME2CLIENT\|CLIENT2GAME)`=`0x0334` (struct `MSG_MessageWhisper`, `Basedef.h:1447-1453`); `_MSG_MessagePanel` `(104\|GAME2CLIENT)` (`SendClientMessage` builds `MSG_MessagePanel`); `_MSG_GuildDisable` `(164\|GAME2CLIENT\|CLIENT2GAME)`=`0x02A4` (`Basedef.h:2155`) emitted on `guildon`/`guildoff` |

## 2. Wire Framing

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with 4-byte `INITCODE = 0x1F11F311` magic before any framed message.
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating XOR transform keyed by `KeyWord` (index into shared `pKeyWord[512]`).
- `CheckSum` = `Sum2 - Sum1` (raw vs. transformed payload sums); validated on receive.
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` else the buffer is reset.
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is **disabled** (body commented out, returns `FALSE`) — but the commented-out central validation for this packet was `m->Size != sizeof(MSG_MessageChat)` → `code = 1` (`Basedef.cpp:6501`).

Per-packet notes:
- No deviation from standard framing. It is a plain bidirectional chat frame carried on the client socket.
- `Type = 0x0333` on the wire from the client and from the server. The handler does **not** rewrite `Type`; it rewrites only `ID = conn` (`_MSG_MessageChat.cpp:27`) and forces `String[MESSAGE_LENGTH-1] = String[MESSAGE_LENGTH-2] = 0`.
- `Size` is expected to be `sizeof(MSG_MessageChat)` = 108. **No in-handler size validation** — the handler casts to `MSG_MessageChat` and reads up to 96 chars of `String`.
- Relays are done via `GridMulticast` which re-sends the **same buffer** to every player in view (`pUser[tmob].cSock.AddMessage((char*)msg, msg->Size)`, `SendFunc.cpp:968`), so the relayed `Size` stays 108.
- Server→client uses `SendClientMessage` which builds a **different** packet, `MSG_MessagePanel`, not `_MSG_MessageChat` (see §6).

## 3. Binary Layout

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 108) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_MessageChat` = 0x0333 |
| 6 | 2 | `ID` | `short` | Sender connection slot; handler overwrites with `conn` (`_MSG_MessageChat.cpp:27`). On relay it carries the original speaker's ID. |
| 8 | 4 | `ClientTick` | `unsigned int` | Client tick; must not equal `SKIPCHECKTICK` (235543242) or the dispatcher drops the packet (`ProcessClientMessage.cpp:63`) |

### 3.2 Payload

Packing context: **default `/Zp8`** (not a pack(1) region — see §1). Each member aligned to `min(sizeof(member), 8)`; the struct size rounds up to the largest member alignment (4, from `int`/`unsigned int`).

Header ends at offset 12, which is 4-aligned; `char String[96]` has alignment 1. No padding anywhere.

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 96 | `String` | `char[MESSAGE_LENGTH]` | 1 | 0 | Chat text, NUL-terminated by the handler at `[94]` and `[95]` (`_MSG_MessageChat.cpp:24-25,178-179`). Parsed by `sscanf("%s %s", szCmd, szString)` into a command and its argument. |

**Total payload: 96 bytes → total struct 108 bytes.**

### 3.3 Nested struct expansions

`MSG_MessageChat` embeds only the `_MSG` header macro (primitive fields) plus a flat `char[96]` array. It contains **no `STRUCT_*` members**, so there are no nested expansions.

### 3.4 Size verification

| Check | Value | Source |
|---|---|---|
| Header (`_MSG`) | `short(2)+char(1)+char(1)+short(2)+short(2)+unsigned int(4)` = 12 | `Basedef.h:925-930` |
| Payload | `char[96]` = 96 (align 1, offset 12 already 4-aligned → no pad) | `Basedef.h:1443` |
| `sizeof(MSG_MessageChat)` | 12 + 96 = **108**; 108 % 4 = 0 → no tail padding | — |
| `Size` field set to | `sizeof(MSG_MessageChat)` = 108 in both build sites | `SendFunc.cpp:339,341` (`SendChat`), `SendFunc.cpp:1653` (`SendSay`) |
| Central (disabled) check expected | `m->Size == sizeof(MSG_MessageChat)` = 108 | `Basedef.cpp:6501` |
| Cross-check in handler | None — handler performs no `Size` validation; it reads only `ID` and `String` | `_MSG_MessageChat.cpp:22-27` |

No mismatch between build size (108), the (disabled) central check (108), and the computed layout (108).

## 4. Lifecycle & Flow

### 4.1 Client → TMSrv (send chat / slash-command)

```
Client ── MSG_MessageChat (0x0333, Size=108) ──► CPSock.ReadMessage ──► Server.cpp WSA_READ
   ──► ProcessClientMessage(conn, pMsg, FALSE)
        ├─ guard ID in [0,MAX_USER)                       (ProcessClientMessage.cpp:42)
        ├─ guard ServerDown < 120                         (:53)
        ├─ guard ClientTick != SKIPCHECKTICK              (:63)
        └─ switch(Type) → case _MSG_MessageChat (:92)
              └─ Exec_MSG_MessageChat(conn, pMsg)         (:93) → TMSrv/_MSG_MessageChat.cpp:20
```

`Exec_MSG_MessageChat` flow (`TMSrv/_MSG_MessageChat.cpp`):
1. NUL-terminate `String` at [94]/[95] (24-25).
2. `m->ID = conn` (27).
3. Mode gate: `pUser[conn].Mode != USER_PLAY` → return (29-30).
4. `sscanf(m->String, "%s %s", szCmd, szString)` (35) — split into command word + rest.
5. Slash-command handling — each handled command returns early (37-169): `guildon`, `guildoff`, `guildtax`, `guild`, `whisper`, `partychat`, `kingdomchat`, `guildchat`, `chatting`.
6. BrState zone filter (171-176): if `BrState && conn < MAX_USER && BRItem > 0` and the player's `TargetX/TargetY` is inside one of two event rectangles (2604,1708)-(2648,1744) or (896,1405)-(1150,1538), overwrite `String` with `"??????"` (censor).
7. Re-NUL-terminate `String` (178-179).
8. Mute gate: `pUser[conn].MuteChat == 1` → `SendClientMessage` "_NN_No_Speak" + return (181-185).
9. If `Mode == USER_PLAY`: `GridMulticast(pMob[conn].TargetX, pMob[conn].TargetY, (MSG_STANDARD*)pMsg, conn)` (187-194) — relay to all other players in view.
10. Else: `SendClientMessage` "DEBUG:Client send chatting message with wrong status" + `Log("err,send chatting message with wrong status", ...)` (195-199).
11. `ChatLog("chat, <MobName> : <String>", AccountName, IP)` (201-202).

### 4.2 TMSrv → Client (relay)

Relay re-uses the incoming `MSG_MessageChat` buffer (same 108-byte frame, `Type` untouched, `ID` = original speaker):
```
GridMulticast(tx, ty, (MSG_STANDARD*)pMsg, conn)   (SendFunc.cpp:843)
   └─ for each grid cell in VIEWGRID window around (tx,ty)
        tmob = pMobGrid[y][x]
        skip tmob <= 0 or tmob == skip(conn)         (:881)
        if tmob < MAX_USER: pUser[tmob].cSock.AddMessage((char*)msg, msg->Size)   (:891,:968)
```
- `skip = conn` → the speaker does **not** receive his own chat back.
- Only players in the view grid around the speaker receive it; range is the local `VIEWGRID`/`HALFGRID` window (843-873).

There is **no DB forwarding** for `MSG_MessageChat` — `ProcessDBMessage.cpp` contains no `_MSG_MessageChat` case (only resets chat toggles at :388-392, unrelated to this packet).

### 4.3 Server-initiated chat relays

`SendChat(conn, Message)` (`SendFunc.cpp:333-346`) and `SendSay(mob, Message)` (`SendFunc.cpp:1647-1660`) are the two server-side builders of a fresh `MSG_MessageChat` frame (both `Size = sizeof(MSG_MessageChat) = 108`, `Type = _MSG_MessageChat`, `ID = conn`/`mob`), each fed into `GridMulticast`. `SendSay` is used for NPC/mob spoken text.

## 5. Validation & Guards

Guard table in execution order (`TMSrv/_MSG_MessageChat.cpp` unless noted):

| # | Guard / Check | Condition → Action | Source |
|---|---|---|---|
| 1 | Dispatcher: ID range | `std->ID < 0 \|\| std->ID >= MAX_USER` → log `err,packet...` + return | `ProcessClientMessage.cpp:42-51` |
| 2 | Dispatcher: shutdown | `ServerDown >= 120` → return | `ProcessClientMessage.cpp:53` |
| 3 | Dispatcher: internal tick | `isServer==FALSE && std->ClientTick == SKIPCHECKTICK` → return (anti-spoof) | `ProcessClientMessage.cpp:63` |
| 4 | Mode gate | `pUser[conn].Mode != USER_PLAY` → return silently | `_MSG_MessageChat.cpp:29-30` |
| 5 | String termination | force `String[94]=0`, `String[95]=0` (both ends) | `:24-25, :178-179` |
| 6 | Mute gate | `pUser[conn].MuteChat == 1` → `SendClientMessage(_NN_No_Speak)` + return | `:181-185` |
| 7 | BrState zone censor | `BrState && conn<MAX_USER && BRItem>0` + inside event rect → `String = "??????"` | `:171-176` |
| 8 | Wrong-status message | `Mode != USER_PLAY` (re-check) → `SendClientMessage("DEBUG:...wrong status")` + `Log("err,...")` | `:195-199` |
| 9 | Guild tax bounds | `guildtax`: `MOB.GuildLevel != 9` → return; `tax<0 \|\| tax>30 \|\| (szString[0]!=48 && !tax)` → error msg `_NN_Guild_Tax_0_to_30`; `TaxChanged[i]==1` → `_NN_Only_Once_Per_Day` | `:62-101` |

**No explicit:**
- empty-string check (an empty `String` just yields an empty `szCmd`; falls through to `GridMulticast` of an empty string),
- message-length check beyond the unconditional `[94]/[95]` NUL-ing,
- general profanity/badword filter (only the `BrState` zone censor at :171-176, and only in the `BrState` event),
- per-channel (whisper/party/guild/world) content routing in this packet — those channels are separate packet types; toggles here only flip local `pUser` flags.

## 6. Game Mechanics & Business Logic

- **General chat** is the core purpose: a player sends a 96-char string that the server rebroadcasts to every other player in the sender's view grid (`GridMulticast`, `SendFunc.cpp:843-972`), with `ID` set to the sender. The sender is excluded (`skip = conn`).
- **Slash commands** are parsed from the first whitespace-delimited token and short-circuit with `return` (never rebroadcast):
  - `guildon` / `guildoff` — toggle `pMob[conn].GuildDisable` (0↔1), call `SendScore(conn)` and `SendClientSignalParm(conn, ESCENE_FIELD, _MSG_GuildDisable, 0|1)` (`:37-61`). Emits related packet `_MSG_GuildDisable` (0x02A4).
  - `guildtax <N>` — guild-level-9 leaders only; for the matching `g_pGuildZone[i].ChargeGuild`, set `CityTax = tax` (0..30), flag `TaxChanged[i]=1`, `SendClientMessage` `_NN_Only_Once_Per_Day` if already changed, write guild file (`CReadFiles::WriteGuild()`), log `"sys,<String>"` (`:62-102`).
  - `guild` — `SendGuildList(conn)` (`:104-109`).
  - `whisper` — toggle `pUser[conn].Whisper`, ack "Whisper : Off/On" (`:111-121`).
  - `partychat` — toggle `pUser[conn].PartyChat`, ack (`:123-133`).
  - `kingdomchat` — toggle `pUser[conn].KingChat`, ack (`:135-145`).
  - `guildchat` — toggle `pUser[conn].Guildchat`, ack (`:147-157`).
  - `chatting` — toggle `pUser[conn].Chatting`, ack (`:159-169`).
  - These toggles gate which channels the **client** may display (server does not re-filter channel content here).
- **Server→client feedback** (ack/error messages) does NOT use `_MSG_MessageChat`; it uses `SendClientMessage` → `MSG_MessagePanel` (`SendFunc.cpp:27-45`: `Type=_MSG_MessagePanel`, `String` truncated to `MESSAGE_LENGTH`).
- **Muted players** receive `_NN_No_Speak` and their chat is dropped (`:181-185`).
- **NPC/mob speech** is pushed via `SendSay` → `MSG_MessageChat` (`SendFunc.cpp:1647-1660`), relayed with `skip=0` so NPC talk reaches everyone (including the `ID` mob's owner).

## 7. Side Effects

| # | Effect | Detail | Source |
|---|---|---|---|
| 1 | Relayed chat broadcast | `GridMulticast(...pMsg, conn)` re-sends the identical `MSG_MessageChat` frame (Size=108, Type=0x0333, ID=sender) to every other player in the sender's view grid | `_MSG_MessageChat.cpp:193`; `SendFunc.cpp:843-972` |
| 2 | `pUser` state mutation (slash cmds) | `GuildDisable`, `Whisper`, `PartyChat`, `KingChat`, `Guildchat`, `Chatting`, `CityTax`/`TaxChanged` | `:39-169` |
| 3 | Outgoing `SendScore(conn)` | on `guildon`/`guildoff` (`:43,55`) |
| 4 | Outgoing `_MSG_GuildDisable` signal | `SendClientSignalParm(conn, ESCENE_FIELD, _MSG_GuildDisable, parm)` (`:45,58`) |
| 5 | Outgoing `MSG_MessagePanel` acks | `SendClientMessage` for toggles, tax results, mute denial, wrong-status | `:78,84,92,116,118,...` |
| 6 | Server log | `Log("sys,%s", AccountName, IP)` on `guildtax` (`:96-97`) |
| 7 | Server log | `Log("err,send chatting message with wrong status", ...)` (`:198`) |
| 8 | Chat log | `ChatLog("chat, %s : %s", pMob[conn].MOB.MobName, m->String, AccountName, IP)` (`:201-202`) |
| 9 | DB write | `CReadFiles::WriteGuild()` on successful `guildtax` (`:94`) |
| 10 | BrState censoring | `String` overwritten with `"??????"` inside active event rects (`:171-176`) |

## 8. Related Packets

- `_MSG_MessageWhisper` (`52|GAME2CLIENT|CLIENT2GAME`) = `0x0334` — the dedicated whisper-channel packet (`Basedef.h:1447-1453`), separate from general chat.
- `_MSG_MessagePanel` (`104|GAME2CLIENT`) — used by `SendClientMessage` for all server text feedback (not `_MSG_MessageChat`).
- `_MSG_GuildDisable` (`164|GAME2CLIENT|CLIENT2GAME`) = `0x02A4` — emitted by `guildon`/`guildoff` commands (`Basedef.h:2155`).
- `_MSG_MessageBoxOk`, `_MSG_DBClientMessage` (`19|FLAG_DB2GAME`) — other message-panel/DB-message relatives (context only; no direct link).

## 9. Discrepancies & Open Questions

1. **Type hex in the command spec is wrong.** The task brief states `_MSG_MessageChat = 0x233`, but that equals `51 | FLAG_CLIENT2GAME` only. The authoritative source (`Basedef.h:1439`) declares `(51 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME)` = `51 | 0x0100 | 0x0200` = **0x0333** (819). Source wins; 0x0333 is the correct wire value.
2. **`BASE_CheckPacket` is disabled** (`Basedef.cpp:6475`), so the size consistency check (`m->Size == sizeof(MSG_MessageChat)`, `Basedef.cpp:6501`) is never enforced at runtime. The handler also performs no own size check — a malformed `Size` frame is still processed.
3. **`ClientTick` semantics**: the dispatcher only rejects `ClientTick == SKIPCHECKTICK` (`ProcessClientMessage.cpp:63`); no other validation, and the field is otherwise ignored by this handler. Exact client algorithm UNKNOWN.
4. **BrState/BrItem event rectangles** (`:171-176`) are hard-coded legacy event coordinates; the meaning/trigger conditions of `BrState`/`BRItem` UNKNOWN — verified only that the censor fires inside the two rects.
5. **`guildtax` parsing quirk**: `szString[0] != 48 && !tax` (48 = `'0'`) means a value starting with `'0'` bypasses the bound check — likely a bug/oddity; behaviour preserved from source.
6. **No profanity filter** exists in this handler beyond the BrState censor; the `Leader` variable at `:189-191` is computed but unused (only `GridMulticast` is called).

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `legacy/Code/Basedef.h` | 925-930 | `_MSG` header macro (Size/KeyWord/CheckSum/Type/ID/ClientTick) |
| `legacy/Code/Basedef.h` | 932-941 | `FLAG_GAME2CLIENT`/`FLAG_CLIENT2GAME`/etc. |
| `legacy/Code/Basedef.h` | 129 | `MESSAGE_LENGTH = 96` |
| `legacy/Code/Basedef.h` | 172 | `SKIPCHECKTICK = 235543242` |
| `legacy/Code/Basedef.h` | 1439 | `_MSG_MessageChat` constant |
| `legacy/Code/Basedef.h` | 1440-1444 | `MSG_MessageChat` struct |
| `legacy/Code/Basedef.h` | 1447-1453 | `_MSG_MessageWhisper` / `MSG_MessageWhisper` |
| `legacy/Code/Basedef.h` | 2155 | `_MSG_GuildDisable` |
| `legacy/Code/Basedef.cpp` | 6501 | Disabled central size check for `MSG_MessageChat` |
| `legacy/Code/TMSrv/ProcessClientMessage.cpp` | 38-66 | Dispatcher guards |
| `legacy/Code/TMSrv/ProcessClientMessage.cpp` | 92-94 | `case _MSG_MessageChat` dispatch |
| `legacy/Code/TMSrv/ProcessClientMessage.h` | 83 | `Exec_MSG_MessageChat` prototype |
| `legacy/Code/TMSrv/_MSG_MessageChat.cpp` | 20-203 | Handler implementation |
| `legacy/Code/TMSrv/SendFunc.cpp` | 27-45 | `SendClientMessage` (→ `MSG_MessagePanel`) |
| `legacy/Code/TMSrv/SendFunc.cpp` | 208-218 | `SendClientSignalParm` |
| `legacy/Code/TMSrv/SendFunc.cpp` | 333-346 | `SendChat` (builds `MSG_MessageChat`) |
| `legacy/Code/TMSrv/SendFunc.cpp` | 843-972 | `GridMulticast` relay |
| `legacy/Code/TMSrv/SendFunc.cpp` | 1647-1660 | `SendSay` (builds `MSG_MessageChat`) |
| `legacy/Code/TMSrv/Language.h` | 188,194,372 | `_NN_Guild_Tax_0_to_30`(168), `_NN_Only_Once_Per_Day`(174), `_NN_No_Speak`(353) |
| `legacy/Code/TMSrv/CUser.h` | 78-107 | `Whisper`/`PartyChat`/`Chatting`/`Guildchat`/`MuteChat`/`KingChat`/`LastChat` fields |
| `legacy/Code/TMSrv/CUser.h` | 36 | `USER_PLAY = 22` |
| `legacy/Code/TMSrv/CMob.h` | 26 | `MOB_EMPTY = 0` |
| `legacy/Code/Basedef.h` | 170 | `ESCENE_FIELD = 30000` |
