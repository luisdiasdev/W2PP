# MSG_MessageWhisper

## 1. Summary (table)

| Property | Value |
|---|---|
| Constant | `_MSG_MessageWhisper` |
| Definition | `Basedef.h:1447` |
| Flags | `FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME` |
| Numeric value | `(52 \| 0x0100 \| 0x0200)` = `0x334` (820) |
| Payload struct | `MSG_MessageWhisper` (`Basedef.h:1448-1453`) |
| Struct size (`sizeof`) | **128 bytes** (see §3) |
| Expected `Size` field | `128` |
| Direction | Bidirectional client ↔ TMSrv (same type both ways) |
| Dispatch | `ProcessClientMessage.cpp:164-166` → `Exec_MSG_MessageWhisper` |
| Handler | `TMSrv/_MSG_MessageWhisper.cpp:20` |
| DB relay | None (delivery is same-process via `pUser[].cSock`) |
| Purpose | Private whisper (direct message) + a large set of slash-command shortcuts and channel chat carried in the same type |

> **NOTE on hex value.** The command brief states `= 0x234`. That is inconsistent with the flags it lists: `FLAG_GAME2CLIENT=0x0100` and `FLAG_CLIENT2GAME=0x0200` are both OR-ed onto the base `52 (0x34)`, giving `0x34 | 0x100 | 0x200 = 0x334`. `0x234` would correspond to `0x34 | 0x200` alone (only `CLIENT2GAME`). Per the "code over comments" rule, the source-derived value **0x334** is authoritative. See §9.

---

## 2. Wire Framing (protocol preamble)

Standard CPSock framing applies, no per-packet deviation.

- **Connection init**: on `connect`, both peers transmit `INITCODE = 0x1F11F311` (4 bytes, little-endian) as the first bytes of the stream (`CPSock.cpp:249-250`). The receiver's `ReadMessage` consumes these once (`Init==0` branch, `CPSock.cpp:366-383`) before parsing packets.
- **On-wire packet layout**: `[Size:2][KeyWord:1][CheckSum:1][Type:2][ID:2][ClientTick:4]` then `Size-12` payload bytes. `Size` = full packet length incl. header (`CPSock.cpp:390`).
- **Bounds**: `Size` must satisfy `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE (8192)` (`CPSock.cpp:397`). Buffer granularity `RECV_BUFFER_SIZE = SEND_BUFFER_SIZE = 128*1024` (`CPSock.h:35-36`).
- **Obfuscation**: the payload from byte offset `4` is XOR/arithmetic-mangled per byte with a position-rotating key from the `pKeyWord[512]` table (`CPSock.cpp:29`, key bytes at `pKeyWord[rst*2+1]`):
  - `mod = i & 0x3`; `Trans = pKeyWord[rst*2+1]`, `rst = pos % 256`, `pos` starts at `KeyWord = pKeyWord[iKeyWord*2]`.
  - mod 0: `- (Trans<<1)`; mod 1: `+ (Trans>>3)`; mod 2: `- (Trans<<2)`; mod 3: `+ (Trans>>5)`
  - Encode (`AddMessage`, `CPSock.cpp:558-581`) and decode (`ReadMessage`, `CPSock.cpp:430-453`) are inverse operations.
- **CheckSum**: `CheckSum = Sum2 - Sum1`, where `Sum1 = Σ decoded payload bytes` and `Sum2 = Σ encoded payload bytes`, computed over bytes `[4, Size)` (`CPSock.cpp:554-584`). On decode, a mismatch sets `ErrorCode=1` but the packet is **still returned** (`CPSock.cpp:458-464`).
- **Send**: `AddMessage` stamps `Size/KeyWord/CheckSum/ClientTick`, obfuscates into `pSendBuffer`, then `SendMessageA` → `send()` (`CPSock.cpp:513-591`, `617-684`). `SendOneMessage = AddMessage + SendMessageA` (`CPSock.cpp:686-693`).
- **Size validation (packet-level)**: `BASE_CheckPacket` (`Basedef.cpp:6475`) would enforce `_MSG_MessageWhisper && Size != sizeof(MSG_MessageWhisper)` (`Basedef.cpp:6502`), but its whole body is **commented out**, so it always returns 0 — the size is NOT validated in production.
- **Dispatch chain**: `CPSock.ReadMessage` (`Server.cpp:3975`) → `ProcessClientMessage(User, Msg, FALSE)` (`Server.cpp:4001`) → switch on `std->Type` (`ProcessClientMessage.cpp:66`) → `case _MSG_MessageWhisper` (`ProcessClientMessage.cpp:164`) → `Exec_MSG_MessageWhisper(conn, pMsg)` (`ProcessClientMessage.cpp:165`).

---

## 3. Binary Layout

### 3.1 Header

`MSG_MessageWhisper` (and every `_MSG`-derived struct) begins with the common `_MSG` macro (`Basedef.h:925-930`). Header fields are identical to `HEADER` (`CPSock.h:42-50`).

`MSG_MessageWhisper` is declared at `Basedef.h:1448`, which is **before** the nearest `#pragma pack(push,1)` region at `Basedef.h:1465-1492`. The preceding pack region closes at `Basedef.h:1246`. Therefore the struct uses the **MSVC default /Zp8 packing** (alignment = min(8, natural alignment of each member); native C/C++ alignment applies; there is **no** `#pragma pack` affecting it). The default project packing is `#pragma pack()` → /Zp8.

| Offset | Size | Type | Align | Field | Semantics |
|---|---|---|---|---|---|
| 0 | 2 | `short` | 2 | `Size` | total packet length incl. header = 128 |
| 2 | 1 | `char` | 1 | `KeyWord` | obfuscation key table index |
| 3 | 1 | `char` | 1 | `CheckSum` | `Sum2 - Sum1` checksum |
| 4 | 2 | `short` | 2 | `Type` | `_MSG_MessageWhisper` = 0x334 |
| 6 | 2 | `short` | 2 | `ID` | conn index (sender on c→s; target on s→c delivery) |
| 8 | 4 | `unsigned int` | 4 | `ClientTick` | client tick / anti-speed timestamp |
| **12** | | | | | **end of header (no padding)** |

Header size = **12 bytes**, zero padding.

### 3.2 Payload

| Offset | Size | Type | Align | Field | Semantics |
|---|---|---|---|---|---|
| 12 | 16 | `char[16]` | 1 | `MobName` | target mob name (c→s); source name (s→c) |
| 28 | 100 | `char[100]` | 1 | `String` | whisper text / command arg / channel text |
| **128** | | | | | **end of struct** |

- `NAME_LENGTH = 16` (`Basedef.h:132`)
- `MESSAGEWHISPER_LENGTH = 100` (`Basedef.h:130`)
- `MESSAGE_LENGTH = 96` (`Basedef.h:129`) — used by the handler to clamp `String`.

### 3.3 Nested struct expansions

None — `MSG_MessageWhisper` contains only the inline `_MSG` macro plus two flat `char` arrays; no nested aggregate types.

### 3.4 Size verification

Padding analysis under /Zp8:

- `Size` (short, @0), `KeyWord` (char @2), `CheckSum` (char @3), `Type` (short @4 — even ✓), `ID` (short @6 — even ✓), `ClientTick` (int @8 — 4-aligned ✓).
- `MobName` (char @12, align 1 ✓), `String` (char @28, align 1 ✓).
- **No padding inserted anywhere.**
- Struct alignment = 4 (from `unsigned int ClientTick`); total 128 is a multiple of 4 → **no tail padding**.

`sizeof(MSG_MessageWhisper) = 12 + 16 + 100 = 128 bytes`.

Cross-check against source usages (all consistent, no mismatch):

| Where | Usage |
|---|---|
| `Basedef.cpp:6502` | `m->Type == _MSG_MessageWhisper && m->Size != sizeof(MSG_MessageWhisper)` (disabled block) |
| `_MSG_MessageWhisper.cpp:1173` | `pUser[i].cSock.AddMessage((char*)m, sizeof(MSG_MessageWhisper))` (guild chat) |
| `_MSG_MessageWhisper.cpp:1201` | `AddMessage(..., sizeof(MSG_MessageWhisper))` (party chat) |
| `_MSG_MessageWhisper.cpp:1220` | `AddMessage(..., sizeof(MSG_MessageWhisper))` (party chat) |
| `_MSG_MessageWhisper.cpp:1370` | `pUser[target].cSock.AddMessage((char*)m, sizeof(MSG_MessageWhisper))` (whisper delivery) |

Expected `Size` field = **128**. No `sizeof` mismatch found.

---

## 4. Lifecycle & Flow

### Client → TMSrv (sender)

1. Client builds `MSG_MessageWhisper`, sets `Type=0x334`, `MobName=target name`, `String=message`, sends via CPSock (`AddMessage`).
2. `ReadMessage` (`Server.cpp:3975`) → `ProcessClientMessage` (`Server.cpp:4001`, `isServer=FALSE`).
3. Dispatcher guards (`ProcessClientMessage.cpp:38-66`):
   - `std->ID` out of `[0, MAX_USER)` → log `err,packet ...` and drop (`:42-51`).
   - `ServerDown >= 120` → drop (`:53`).
   - sets `pUser[conn].LastReceiveTime = SecCounter` (`:56-57`).
   - `Type == _MSG_Ping` → drop (`:59-60`).
   - `isServer==FALSE && ClientTick == SKIPCHECKTICK` → drop (`:63-64`).
4. `case _MSG_MessageWhisper:` (`:164`) → `Exec_MSG_MessageWhisper(conn, pMsg)` (`:165`).
5. Handler (see §5/§6) resolves target by name and either delivers or replies/returns.

### TMSrv → target client (whisper delivery)

1. `m->ID = target` (`_MSG_MessageWhisper.cpp:1325`).
2. `m->MobName` overwritten with the **sender's** `MobName` (`:1367`) — so the delivered packet carries source name in `MobName`, and `ID` = target conn.
3. `pUser[target].cSock.AddMessage((char*)m, sizeof(MSG_MessageWhisper))` (`:1370`) → `ReadMessage` on the target's socket → dispatcher → (client-side) displayed as incoming whisper.
4. `LastChat` bookkeeping on both sides (`:1327`, `:1368`) so `/r` reply works.

### ASCII sequence diagram (whisper delivery)

```
ClientA             TMSrv (Exec_MSG_MessageWhisper)           ClientB
  |  MSG_MessageWhisper(Type=0x334, MobName=B, String="hi")     |
  |---------------------->  ReadMessage → ProcessClientMessage  |
  |                          guards (:42-64)                     |
  |                          MuteChat? (:1129)                   |
  |                          MobName[0]==0? no (channel chat)    |
  |                          MobName[0]!=0 (:1284)               |
  |                          reply alias? /r (:1291)             |
  |                          target=GetUserByName(B) (:1306)     |
  |                          target==0? -> _NN_Not_Connected     |
  |                          target online? -> _NN_Not_Connected |
  |                          target.Whisper block? (:1320)       |
  |                          m->ID=B; m->MobName=A (:1325,:1367) |
  |                          AddMessage(m,128) (:1370)           |
  |                                                         AddMessage → ReadMessage
  |                                                         display: [A]> hi
  |  ChatLog("chat_sms,...") (:1372)
```

### Same-server only — no cross-server whisper

Target resolution is `GetUserByName` (`Server.cpp:6793`), which iterates **only the local** `pMob[1..MAX_USER)` array (`MAX_USER=1000`, `Basedef.h:56`), matching `pMob[i].MOB.MobName` (or, for a `+`-prefixed name, `pUser[i].AccountName`), and requires `pUser[i].Mode == USER_PLAY` and `pMob[i].Mode != MOB_EMPTY`. There is **no** DB round-trip and no cross-server lookup: a whisper to a user on another TMSrv instance always fails with "not connected". `ProcessDBMessage` never relays whisper packets (only zeroes `pUser[conn].Whisper` on login, `ProcessDBMessage.cpp:387`).

---

## 5. Validation & Guards

Execution order inside `Exec_MSG_MessageWhisper` (`TMSrv/_MSG_MessageWhisper.cpp`), in the order they run:

| # | Line | Guard | Action on failure |
|---|---|---|---|
| 1 | 26 | `m->MobName[NAME_LENGTH-1]=0; [NAME_LENGTH-2]=0` | force NUL-terminate (offsets 15,14) — not a guard |
| 2 | 27 | `m->String[MESSAGEWHISPER_LENGTH-1]=0` | force NUL-terminate (offset 99) |
| 3 | 29-30 | `pUser[conn].Mode != USER_PLAY` | silent `return` |
| 4 | 33-38 | `MobName=="cp"` | chat command (not whisper) |
| 5 | 41-49 | `MobName=="getout"` | chat command |
| 6 | 52-78 | `MobName=="srv"` | chat command (server change) |
| 7 | 81-93 | `MobName=="gfame"` | chat command |
| 8 | 96-143 | `MobName=="spk"` | chat command (magic trumpet) |
| 9 | 146-165 | `MobName=="qst"` | chat command |
| 10 | 168-270 | `MobName=="create"` | chat command (guild create) |
| 11 | 273-365 | `MobName=="subcreate"` | chat command |
| 12 | 368-414 | `MobName=="abandonar"` | chat command |
| 13 | 417-481 | `MobName=="handover"` | chat command |
| 14 | 484-490 | `MobName=="nt"` | chat command |
| 15 | 493-507 | `MobName=="nig"` | chat command |
| 16 | 510-559 | `MobName=="tab"` | chat command |
| 17 | 562-571 | `MobName=="snd"` | chat command |
| 18 | 574-579 | `MobName=="day"` | chat command |
| 19 | 582-594 | `MobName==_NN_Kingdom / "kingdom"` | chat command |
| 20 | 597-608 | `MobName==_NN_King / "king"` | chat command |
| 21 | 611-645 | `MobName==_NN_Summon_Guild / "summonguild"` | chat command |
| 22 | 648-767 | `MobName==_NN_Summon / "summon"` | chat command (admin=Level≥1000) |
| 23 | 770-784 | `MobName=="time"` | chat command |
| 24 | 787-793 | `MobName=="gm"/"GM"` | chat command (ProcessImple) |
| 25 | 796-908 | `MobName==_NN_Relocate / "relo"` | chat command |
| 26 | 911-927 | `MobName=="expulsar"` | chat command |
| 27 | 930-984 | `MobName=="fimguerra"` | chat command |
| 28 | 987-1041 | `MobName=="fimirma"` | chat command |
| 29 | 1044-1085 | `MobName=="pin"` | chat command (PIN code) |
| 30 | 1088-1101 | `MobName=="not"` (Level≥1000) | chat command (notice) |
| 31 | 1104-1109 | `MobName=="wp"` | chat command |
| 32 | 1112-1127 | `MobName=="contaprincipal"` | chat command |
| 33 | 1129-1133 | `pUser[conn].MuteChat == 1` | `SendClientMessage(_NN_No_Speak)`; return (blocks ALL whisper packet content) |
| 34 | 1136-1282 | `MobName[0]==0` → channel chat (see §6) | dispatch per `String[0]` prefix |
| 35 | 1284 | `MobName[0]!=0` → player whisper path | |
| 36 | 1286-1287 | `MobName[15]=0; MobName[14]=0` | force NUL (2 bytes) |
| 37 | 1291-1304 | `MobName` ∈ `{_NN_Reply,"r","ñº","¦õ"}` → use `pUser[conn].LastChat` | if `LastChat[0]==0` → `_NN_No_One_To_Reply`; return |
| 38 | 1306-1312 | `target = GetUserByName(MobName) == 0` | `_NN_Not_Connected`; return |
| 39 | 1314-1318 | `pUser[target].Mode != USER_PLAY` | `_NN_Not_Connected`; return |
| 40 | 1320-1324 | `pUser[target].Whisper && Level<1000` | `_NN_Deny_Whisper`; return |
| 41 | 1325 | `m->ID = target` | — |
| 42 | 1327 | `memcpy(pUser[conn].LastChat, m->MobName, NAME_LENGTH)` | — |
| 43 | 1329-1360 | `String[0]==0` → "user info" query (see §6) | reply; return |
| 44 | 1362-1363 | `String[0]=='-'` or `'='` → set to `' '` | strip channel prefixes |
| 45 | 1365 | `String[MESSAGE_LENGTH]=0` (index 96) | clamp text |
| 46 | 1367 | `m->MobName = sender's MobName` | — |
| 47 | 1368 | `pUser[target].LastChat = sender name` | — |
| 48 | 1370 | `pUser[target].cSock.AddMessage(m, sizeof(MSG_MessageWhisper))` | deliver |
| 49 | 1372-1373 | `ChatLog("chat_sms,...")` | log |

> **Whisper block / ignore model.** There is a single opt-out flag `pUser[x].Whisper` (a "whisper off/block" toggle set via `_MSG_MessageChat` `/whisper` command, `_MSG_MessageChat.cpp:111-121`). When set, incoming whispers to that player are rejected unless the sender is level ≥ 1000 (admin). There is **no** per-player ignore list, and no "whisper to offline user / cross-server user" path beyond the generic "not connected" error.

---

## 6. Game Mechanics & Business Logic

**Primary mechanic — whisper**: `/w <name> <message>` (or any direct `MobName != ""`) routes to the local player named in `MobName`, delivering a copy of the same `MSG_MessageWhisper` packet to the target socket with `MobName` rewritten to the source name and `ID` set to the target conn index. `LastChat` is maintained on both sides for `/r` reply.

**Secondary mechanics** (all multiplexed through the same packet type by abusing `MobName` as a command token):

1. **Channel chat** (when `MobName[0]==0`, `:1136-1282`) — text is dispatched purely by the first char of `String`:
   - `-` : **Guild chat** — loops all `USER_PLAY` users; requires same guild (or ally if `String[1]=='-'`); skips self and `pUser[i].Guildchat`; `m->ID=conn`; `AddMessage` to each (`:1139-1182`). Log `chat_guild,...`.
   - `=` : **Party chat** — to `Leader` and each `PartyList[i]` member (skips `PartyChat`), `:1185-1226`. Log `chat_party,...`.
   - `@@`: **Kingdom chat** — `SyncKingdomMulticast`, with a 3-second anti-spam timer via `pUser[conn].Message` (`:1229-1253`). Log `chat_kingdom,...`.
   - `@` : **Citizen chat** — `SyncMulticast`, same 3s anti-spam (`:1256-1279`). Log `chat_cidadao,...`.
2. **User-info query**: whisper with empty `String` (`:1329-1360`) returns a formatted status line (Citizen/Fame, plus guild name if any, plus optional `Snd` message) to the **sender** via `SendClientMessage` — this is the "look up a player" trick.
3. **Command shortcuts**: `cp`, `getout`, `srv`, `gfame`, `spk`, `qst`, `create`, `subcreate`, `abandonar`, `handover`, `nt`, `nig`, `tab`, `snd`, `day`, `kingdom`, `king`, `summonguild`, `summon`, `time`, `gm`, `relo`, `expulsar`, `fimguerra`, `fimirma`, `pin`, `not`, `wp`, `contaprincipal` — each a self-contained mini-handler (see §5 rows 4-32). These are **not** whispers.

**Routing rules**:
- Target lookup is **local-only** (same TMSrv). Cross-server whisper is unsupported → "not connected".
- Offline target → `_NN_Not_Connected` (no queuing, no offline delivery).
- No "whisper failed" confirmation packet is emitted; failures use the generic `SendClientMessage` (MessagePanel) path.

**Related business state**:
- `pUser[x].MuteChat` — blocks all whisper packet processing (mute).
- `pUser[x].Whisper` — target-side opt-out of incoming whispers (level≥1000 bypasses).
- `pUser[x].LastChat[16]` — last whisper partner, drives `/r`.
- `pUser[x].Message` — anti-spam tick for kingdom/citizen chat (3000 ms).

---

## 7. Side Effects

**Outgoing packets (whisper path, `:1284-1375`)**:
- `pUser[target].cSock.AddMessage(m, 128)` — the `MSG_MessageWhisper` itself, delivered to the target (`:1370`).
- `SendClientMessage(conn, ...)` — to the **sender** on each failure (`_NN_No_One_To_Reply` `:1297`, `_NN_Not_Connected` `:1310/:1316`, `_NN_Deny_Whisper` `:1322`) and for the user-info query (`:1351`, `:1357`).
- `SendClientMessage(conn, _NN_No_Speak)` — when muted (`:1131`).

**Outgoing packets (channel/command paths)**:
- Guild/party chat fan-out via `AddMessage(m,128)` to each member (`:1173`, `:1201`, `:1220`).
- Kingdom/citizen chat via `SyncMulticast` / `SyncKingdomMulticast` (`:1247`, `:1274`).
- Various command handlers send `MSG_STANDARDPARM`/`MSG_STANDARDPARM2`/`MSG_GuildInfo`/`MSG_MagicTrumpet`/`MSG_DBServerChange`/`MSG_DBNotice`/`MSG_DBActivatePinCode`/`MSG_DBPrimaryAccount` to `DBServerSocket` and/or `GridMulticast` (e.g. `:76`, `:138`, `:254`, `:357`, `:402`, `:478`, `:982`, `:1039`, `:1082`, `:1099`, `:1124`).

**Logs** (all through `Log`/`ChatLog` with `AccountName`, `IP`):
- `chat_sms,%s %s : %s` (sender, receiver, text) — `:1372`.
- `chat_guild, %s : %s guild:%s` — `:1179`.
- `chat_party, %s : %s` — `:1223`.
- `chat_kingdom, %s : %s reino:%d` — `:1249`.
- `chat_cidadao, %s : %s` — `:1276`.
- `etc,getout ...` `:46`; `etc,summon ...` `:757`; `etc,relo ...` `:898`; `etc,subcreate ...` `:344`; `etc,abandonar ...` `:373`; `etc,subdelete ...` `:389`; `etc,handover ...` `:447`; `etc,kingdom ...` `:591`; `etc,king ...` `:605`; `etc,summonguild ...` `:642`; `sys,guild medal ...` `:258`.

**pUser / pMob state mutation**:
- `m->ID`, `m->MobName`, `m->String` rewritten in place on the received buffer (delivery relay).
- `pUser[conn].LastChat`, `pUser[target].LastChat` (whisper partner tracking).
- `pUser[conn].Message` (anti-spam tick for @/@@ chat).
- Command handlers mutate `pMob[conn].extra.*`, `MOB.Guild`, `MOB.GuildLevel`, `MOB.Coin`, `Tab`, `Snd`, etc. (e.g. `:43`, `:210-213`, `:405-406`, `:519`, `:564`).

**DB forwarding**: no whisper DB relay exists. DB traffic only for the embedded command shortcuts (guild info, magic trumpet, notice, PIN, primary account, server change).

---

## 8. Related Packets

| Packet | Type | Relation |
|---|---|---|
| `MSG_MessagePanel` / `_MSG_MessagePanel` | `(1\|FLAG_GAME2CLIENT)` | Generic on-screen message used for all whisper failures and command feedback (`SendClientMessage`, `SendFunc.cpp:27-45`) |
| `MSG_MessageChat` / `_MSG_MessageChat` | `(51\|both)` | Public/say chat; `/whisper` toggle that sets the `pUser.Whisper` block flag (`_MSG_MessageChat.cpp:111-121`) |
| `MSG_DBServerChange` | — | Sent by the `srv` command shortcut (`:67-76`) |
| `MSG_MagicTrumpet` | — | Sent by the `spk` shortcut (`:125-138`) |
| `MSG_GuildInfo` | — | Sent by guild command shortcuts (`:238-254`, `:347-357`, etc.) |
| `MSG_MessageBoxOk` | — | Message-box sibling used elsewhere for confirmations |
| `_MSG_Ping` | — | Dispatcher filter before dispatch (`ProcessClientMessage.cpp:59`) |

---

## 9. Discrepancies & Open Questions

1. **Hex value mismatch (confirmed).** The brief says `= 0x234`, but the source (`Basedef.h:1447` with flags at `:932-933`) computes `0x34 | 0x0100 | 0x0200 = 0x334`. `0x234` only includes `FLAG_CLIENT2GAME`. Documented value used here: **0x334**.
2. **`MobName` double-NUL overwrite.** `_MSG_MessageWhisper.cpp:26` sets `MobName[15]=0` and `MobName[14]=0` (both `NAME_LENGTH-1` and `NAME_LENGTH-2`), truncating a full-length 16-char name to 14 chars. Whether this is intentional or a quirk is UNKNOWN; it is applied to the **sender**'s own buffer.
3. **`String[MESSAGE_LENGTH]=0` (index 96) vs `MESSAGEWHISPER_LENGTH=100`.** The handler clamps the text at index 96 (`:1365`) although the buffer is 100 bytes — 4 trailing bytes are unused for whisper text. In guild chat, `String[96]=3` is set as a flag (`:1143`). Purpose of bytes [96..100) is partially a chat-channel marker; exact client semantics UNKNOWN.
4. **No cross-server whisper.** `GetUserByName` (`Server.cpp:6793`) is local-only; whispers to users on other TMSrv instances always return "not connected". Whether the original WYD client expected cross-server whisper is UNKNOWN; this fork does not implement it.
5. **No whisper history / offline queueing.** There is no offline whisper storage; a failed delivery is dropped with only a MessagePanel error.
6. **`BASE_CheckPacket` disabled.** The only size guard for this packet is commented out (`Basedef.cpp:6475-…`), so oversized/undersized whisper packets are not rejected by size at the application layer (transport `Size` bounds still apply).
7. **The `_MSG` header `ID` semantics differ by direction.** c→s `ID` = sender conn; s→c delivery `ID` = target conn. Not an issue but worth noting for sniffers.

---

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `legacy/Code/Basedef.h` | 925-930 | `_MSG` macro (header layout) |
| `legacy/Code/Basedef.h` | 932-941 | `FLAG_*` constants |
| `legacy/Code/Basedef.h` | 129-132 | `MESSAGE_LENGTH=96`, `MESSAGEWHISPER_LENGTH=100`, `NAME_LENGTH=16` |
| `legacy/Code/Basedef.h` | 172 | `SKIPCHECKTICK` |
| `legacy/Code/Basedef.h` | 1447-1453 | `_MSG_MessageWhisper` + `MSG_MessageWhisper` struct |
| `legacy/Code/Basedef.h` | 1465-1492 | following `#pragma pack(push,1)` region (NOT covering this struct → /Zp8) |
| `legacy/Code/Basedef.cpp` | 6475, 6502 | `BASE_CheckPacket` (disabled) whisper size check |
| `legacy/Code/CPSock.h` | 35-50 | buffer sizes, `MAX_MESSAGE_SIZE`, `INITCODE`, `HEADER` |
| `legacy/Code/CPSock.cpp` | 29 | `pKeyWord[512]` key table |
| `legacy/Code/CPSock.cpp` | 249-250, 366-383 | INITCODE handshake |
| `legacy/Code/CPSock.cpp` | 353-467 | `ReadMessage` (framing, bounds, obfuscation, checksum) |
| `legacy/Code/CPSock.cpp` | 513-591 | `AddMessage` (encode, checksum) |
| `legacy/Code/CPSock.cpp` | 686-693 | `SendOneMessage` |
| `legacy/Code/TMSrv/ProcessClientMessage.cpp` | 38-66, 164-166, 311 | dispatcher + guards + `case _MSG_MessageWhisper` |
| `legacy/Code/TMSrv/_MSG_MessageWhisper.cpp` | 20-1378 | `Exec_MSG_MessageWhisper` handler |
| `legacy/Code/TMSrv/_MSG_MessageWhisper.cpp` | 1284-1375 | actual whisper delivery path |
| `legacy/Code/TMSrv/_MSG_MessageChat.cpp` | 111-121 | `/whisper` toggle (`pUser.Whisper`) |
| `legacy/Code/TMSrv/Server.cpp` | 3975, 4001 | `ReadMessage` → `ProcessClientMessage` |
| `legacy/Code/TMSrv/Server.cpp` | 6793-6827 | `GetUserByName` (local-only lookup) |
| `legacy/Code/TMSrv/SendFunc.cpp` | 27-45 | `SendClientMessage` (MessagePanel builder) |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 387 | `pUser[conn].Whisper = 0` on login (no whisper relay) |
