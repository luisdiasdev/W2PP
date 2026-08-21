# MSG_DBNotice

## 1. Summary

| Field | Value |
|---|---|
| Type constant | `_MSG_DBNotice` = `(30 \| FLAG_DB2GAME \| FLAG_GAME2DB \| FLAG_GAME2CLIENT)` = `0xD1E` |
| Base (low byte) | `30` (`0x1E`) |
| Direction flags | `FLAG_GAME2CLIENT` = `0x0100` · `FLAG_DB2GAME` = `0x0400` · `FLAG_GAME2DB` = `0x0800` |
| Direction | Game→DB (producer) → DB relays → DB→Game (handler) → Game→Client (broadcast). Full loop: TMSrv → DBSrv → all TMSrv → all clients |
| Struct | `MSG_DBNotice` (`Basedef.h:1408-1412`) |
| Transport form | Dedicated struct (NOT `MSG_STANDARD`); header + `String[96]` |
| `sizeof` | **108 bytes** |
| `Size` field value | 108 (`0x6C`) |
| TMSrv producer (GM cmd) | `TMSrv/imple.cpp:1185-1201` (gm command `notice`, level ≥ 1) |
| TMSrv producer (GM whisper) | `TMSrv/_MSG_MessageWhisper.cpp:1088-1101` (whisper target `not`, Level ≥ 1000) |
| DBSrv relay | `DBSrv/CFileDB.cpp:1612-1622` (case `_MSG_DBNotice`, forward to all game servers) |
| TMSrv handler | `TMSrv/ProcessDBMessage.cpp:141-146` (case `_MSG_DBNotice`, `conn==0`) |
| Client-side effect | `SendNotice` → `SendClientMessage` → `MSG_MessagePanel` to every `USER_PLAY` client |
| Purpose | Server-wide text notice/announcement, relayed DB→all game servers→all players. |

`0xD1E` = `0x1E | 0x100 | 0x400 | 0x800` = `30 + 256 + 1024 + 2048` = `3358`. The struct shares the same shape as `MSG_MagicTrumpet` (base 29 → `0xD19`); both carry a single `char String[MESSAGE_LENGTH]`.

## 2. Wire Framing

Standard CPSock framing applies; no per-packet deviation beyond the normal obfuscation of the payload.

- Transport: TCP between DBSrv and each TMSrv; connection bootstrap sends/receives `INITCODE = 0x1F11F311` (`CPSock.h:40`, `CPSock.cpp:249`, `:373`) as the peer-validating magic.
- `SendOneMessage` (`CPSock.cpp:686-693`) → `AddMessage` (`CPSock.cpp:513-591`) copies the caller buffer into the socket send buffer.
- `AddMessage` overwrites framing fields on the caller's buffer: `Size = Size` (passed in), `KeyWord = random table index`, `CheckSum` computed, `ClientTick = CurrentTime` (`CPSock.cpp:535-541`). The **first 4 bytes** (`Size`, `KeyWord`, `CheckSum`) are copied verbatim (`CPSock.cpp:586`).
- Bytes from **offset 4** to `Size-1` are position-rotating per-byte transformed using `pKeyWord`, modulo `i&0x3` (add/sub of `Trans` shifted left/right — `CPSock.cpp:558-581`), and `CheckSum = Sum2 - Sum1` (`CPSock.cpp:583-584`). This covers `Type`(4), `ID`(6), `ClientTick`(8) and the entire `String[96]` (12..107).
- The relay is a **broadcast**: DBSrv loops over every connected game-server socket and calls `pUser[i].cSock.SendOneMessage(Msg, sizeof(MSG_DBNotice))` for each (`CFileDB.cpp:1614-1621`). Each socket gets its own randomized `KeyWord`/obfuscation.
- Receiver side validates `Size` within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` (`CPSock.cpp:397`); `HEADER` is 12 bytes (`CPSock.h:42-50`), `MAX_MESSAGE_SIZE = 8192` (`CPSock.h:38`).
- `BASE_CheckPacket` is disabled (`Basedef.cpp:6475` — body fully commented out); no extra integrity gate on the receive path.

Per-packet framing deviation: none structurally — the packet is a header + 96-byte C-string broadcast verbatim over the standard framing.

## 3. Binary Layout

Packing context: `Basedef.h` is **not** uniformly packed. The explicit `#pragma pack(push,1)` regions cover only lines **808-835**, **1212-1246**, **1465-1492**, **2063-2097**. The `MSG_DBNotice` struct (`Basedef.h:1408-1412`) falls in **none** of these regions, so MSVC default **/Zp8** alignment applies: each member is aligned to `min(sizeof(member), 8)`, and the struct size is rounded up to the largest member alignment (4 here, since the widest member is `unsigned int`).

### 3.1 Header (`_MSG`, `Basedef.h:925-930`)

| Offset | Size | Type | Align | Field | Semantics |
|---|---|---|---|---|---|
| 0 | 2 | short | 2 | `Size` | Total message size incl. header (= 108) |
| 2 | 1 | char | 1 | `KeyWord` | Obfuscation table index / key |
| 3 | 1 | char | 1 | `CheckSum` | `Sum2 - Sum1` checksum |
| 4 | 2 | short | 2 | `Type` | `_MSG_DBNotice` = `0xD1E` |
| 6 | 2 | short | 2 | `ID` | Set to 0 by producers (broadcast) → receiver `conn==0` |
| 8 | 4 | unsigned int | 4 | `ClientTick` | Client tick (set by CPSock) |

Header size = **12 bytes**. Offsets: 0,2,3,4,6,8 — no padding within the header (`ClientTick` at 8 is 4-aligned). `HEADER` (`CPSock.h:42-50`) has identical layout.

### 3.2 Payload

| Offset | Size | Type | Align | Field | Semantics |
|---|---|---|---|---|---|
| 12 | 96 | char[96] | 1 | `String` | Notice text, `MESSAGE_LENGTH`=96 (`Basedef.h:129`) |

Offset math:
- `_MSG` occupies bytes 0–11 (12 bytes).
- `String[96]`: `char` align 1, placed at offset 12 → bytes 12–107. No alignment padding required before it (12 is already aligned to 1).
- Total = 12 + 96 = **108 bytes**. Largest member alignment = 4; 108 % 4 = 0 → `sizeof = 108`, no trailing padding.

### 3.3 Nested struct expansions

None. `MSG_DBNotice` contains only the `_MSG` header and a single `char String[96]`; there are no nested `STRUCT_*` types. Layout is identical to `MSG_MagicTrumpet` (`Basedef.h:1401-1405`), which differs only in the `Type` constant, not in bytes.

### 3.4 Size verification

- Producer sets `sm_dbn.Size = sizeof(MSG_DBNotice)` and sends `sizeof(MSG_DBNotice)` (`imple.cpp:1192`, `:1198`; `_MSG_MessageWhisper.cpp:1093`, `:1099`) → Size = 108.
- Relay re-sends the received buffer with `sizeof(MSG_DBNotice)` (`CFileDB.cpp:1620`).
- `memset(&sm_dbn, 0, sizeof(MSG_DBNotice))` before filling (`imple.cpp:1190`, `_MSG_MessageWhisper.cpp:1091`) → `KeyWord`/`CheckSum`/`ID`/`ClientTick` initially zeroed; CPSock fills framing fields at send time.
- `MSG_DBNotice` is **not** inside any `#pragma pack(1)` region; with /Zp8 default it is 108 bytes. If it were (incorrectly) treated as packed, the layout would be identical (no internal padding exists) — but the authoritative layout is the /Zp8 one.
- No size mismatch: all `sizeof` uses agree (108).

All members are set by producers and consumed by the handler — no UNKNOWN members.

## 4. Lifecycle & Flow

### 4.1 Producer — GM command `notice`, `TMSrv/imple.cpp:1185-1201`

Inside `ProcessImple(conn, level, str)` (`imple.cpp:46`), gated by the `if (level >= 1)` block (`imple.cpp:1164`). `level` comes from the caller: for the whisper path `level = pMob[conn].MOB.CurrentScore.Level - 1000` (`_MSG_MessageWhisper.cpp:789`), so level ≥ 1 means a character Level ≥ 1001 (GM tier).

1. `memcpy(temp, str + 7, MESSAGE_LENGTH)` (`:1187`) — strips the leading `notice ` (6 chars + space) from the command string.
2. `memset` a `MSG_DBNotice`, set `Size = sizeof(...)` (108), `ID = 0`, `Type = _MSG_DBNotice` (`:1189-1194`).
3. `strncpy(sm_dbn.String, temp, MESSAGE_LENGTH-1)` (`:1196`).
4. `DBServerSocket.SendOneMessage((char*)&sm_dbn, sizeof(MSG_DBNotice))` (`:1198`) — GAME2DB leg.

### 4.2 Producer — GM whisper `not`, `TMSrv/_MSG_MessageWhisper.cpp:1088-1101`

`else if (strcmp(m->MobName, "not") == 0 && pMob[conn].MOB.CurrentScore.Level >= 1000)` — a whisper whose target name is literally `not` from a Level ≥ 1000 (GM) character:
- builds the same `MSG_DBNotice` (`Size=108`, `ID=0`, `Type=_MSG_DBNotice`) and `strncpy(sm_dbn.String, m->String, MESSAGE_LENGTH-1)` (`:1090-1097`)
- `DBServerSocket.SendOneMessage(...)` (`:1099`).

### 4.3 DBSrv relay — `DBSrv/CFileDB.cpp:1612-1622`

DBSrv receives the packet on a game-server socket. `ProcessClientMessage` (`Server.cpp:1339`) requires `std->Type & FLAG_GAME2DB` and `0 <= std->ID <= MAX_USER` (`Server.cpp:1343`); `_MSG_DBNotice` carries `FLAG_GAME2DB` and `ID=0`, so both pass. It then calls `cFileDB.ProcessMessage(msg, conn)` (`Server.cpp:1356`), whose `switch(std->Type)` (`CFileDB.cpp:240`) hits `case _MSG_DBNotice` (`:1612`):

- loop `for (i = 0; i < MAX_SERVER; i++)` over the **game-server** sockets (`pUser[i]`); skip `USER_EMPTY` or `Sock == 0`; else `pUser[i].cSock.SendOneMessage(Msg, sizeof(MSG_DBNotice))` (`:1614-1621`).
- Note: unlike `_MSG_MagicTrumpet` (`CFileDB.cpp:1591-1610`), this relay does **not** also forward to the admin/NP sockets (`pAdmin[0..MAX_ADMIN]`).

### 4.4 TMSrv handler — `TMSrv/ProcessDBMessage.cpp:141-146`

`ProcessDBMessage` casts the buffer to `MSG_STANDARD` and reads `Type`/`ID` (`:41`). Pre-guard requires `(std->Type & FLAG_DB2GAME)` and `0 <= std->ID < MAX_USER` (`:43`); `Type=0xD1E` has the DB2GAME bit and `ID=0`. Since `ID == 0`, the `if (conn == 0)` broadcast branch is taken (`:56`), `switch(std->Type)` reaches `case _MSG_DBNotice` (`:141`), buffer recast to `MSG_DBNotice *m` (`:143`), then `SendNotice(m->String)` (`:145`).

### 4.5 Game→Client broadcast — `TMSrv/SendFunc.cpp:47-62` (`SendNotice`)

- `sprintf(Notice, "not %s", Message); Log(Notice, "-system", NULL)` (`:51-52`).
- Early return if `Message[0]=='\'' && Message[1]=='x'` (`:54-55`) — suppresses notices starting with `'x`.
- Loop `for (i = 0; i < MAX_USER; i++)`: if `pUser[i].Mode == USER_PLAY`, `SendClientMessage(i, Message)` (`:57-61`).
- `SendClientMessage` (`SendFunc.cpp:27-45`) builds `MSG_MessagePanel` (`_MSG_MessagePanel = (1|FLAG_GAME2CLIENT)`, `Basedef.h:1194-1199`), `memcpy` the message into `String[128]`, and `pUser[conn].cSock.AddMessage(...)` (`:44`) — queued GAME2CLIENT delivery.

ASCII sequence diagram:

```
 GM char (Level≥1000)        TMSrv (producer)              DBSrv                    TMSrv(1..N)          Clients
        |  cmd "notice X" /          |                        |                          |                     |
        |  whisper "not" X           |                        |                          |                     |
        |---------------------------->  build MSG_DBNotice    |                          |                     |
        |                            |  DBServerSocket.Send   |                          |                     |
        |                            |------------------------> ProcessClientMessage    |                     |
        |                            |                        |  (FLAG_GAME2DB ok, ID=0) |                     |
        |                            |                        |  CFileDB.ProcessMessage  |                     |
        |                            |                        |  case _MSG_DBNotice      |                     |
        |                            |                        |  for i in MAX_SERVER:    |                     |
        |                            |                        |    SendOneMessage ----> |  ProcessDBMessage    |
        |                            |                        |                         |  conn==0 branch      |
        |                            |                        |                         |  case _MSG_DBNotice  |
        |                            |                        |                         |  SendNotice(String)  |
        |                            |                        |                         |  for i in MAX_USER:  |
        |                            |                        |                         |    if USER_PLAY:     |
        |                            |                        |                         |      SendClientMessage|
        |                            |                        |                         |      (MSG_MessagePanel) ->  all players
```

## 5. Validation & Guards

Execution order. Pre-handler guard applies on every DB→game message; the case body itself has **no internal guards** — it unconditionally broadcasts.

| # | Guard | Source | Effect |
|---|---|---|---|
| G0 | `!(std->Type & FLAG_DB2GAME) \|\| std->ID < 0 \|\| std->ID >= MAX_USER` | `ProcessDBMessage.cpp:43` | Reject + log `err,packet ...` if missing DB2GAME bit or bad ID. Passes for this packet (`0xD1E` has the bit, `ID=0`). |
| G1 | `std->ID == 0` | `ProcessDBMessage.cpp:56` | Broadcast branch selection. `ID=0` (set by producers) → `conn==0` global branch. |
| G2 | `pUser[i].Mode == USER_EMPTY \|\| pUser[i].cSock.Sock == 0` | `SendFunc.cpp:59` | `SendNotice` skips non-playing / dead slots. |
| G3 | `Message[0]=='\'' && Message[1]=='x'` | `SendFunc.cpp:54` | Suppress notices beginning with `'x` (dev/cancel marker). |

DBSrv side guard (`Server.cpp:1343`): requires `FLAG_GAME2DB` and `0 <= ID <= MAX_USER` before `ProcessMessage`; passes.

There is **no** per-field validation of `String` in the DBSrv relay (`CFileDB.cpp:1612`) or in the TMSrv handler (`ProcessDBMessage.cpp:141`) — the string is trusted and broadcast as-is. Producers truncate to `MESSAGE_LENGTH-1` (`imple.cpp:1196`, `_MSG_MessageWhisper.cpp:1097`) but do not force a NUL terminator on `String[MESSAGE_LENGTH-1]`.

## 6. Game Mechanics & Business Logic

- **What it is**: a server-wide text announcement. The text is authored by a GM (level ≥ 1 command `notice`, or a Level ≥ 1000 whisper to target `not`), shipped to DBSrv, relayed to every connected game server, and each server shows it to all in-game players via the message panel.
- **Who can trigger it**: only GM/admin characters. Command path gated by `level >= 1` (`imple.cpp:1164`), i.e. `CurrentScore.Level >= 1001` for the whisper route (`_MSG_MessageWhisper.cpp:789`). The direct `not` whisper path requires `CurrentScore.Level >= 1000` (`_MSG_MessageWhisper.cpp:1088`).
- **Broadcast mechanics**: DB relays to all `MAX_SERVER` game-server sockets (`CFileDB.cpp:1614-1621`); each TMSrv's `SendNotice` fans out to every `USER_PLAY` client (`SendFunc.cpp:57-61`). Client delivery is `MSG_MessagePanel` (`SendFunc.cpp:32-44`), which is also the generic `SendClientMessage` used for dozens of other game notices.
- **`'x` suppression**: any notice text starting with `'x` is silently dropped at broadcast time (`SendFunc.cpp:54`) — an in-band cancel/quiet marker, distinct from the `''x` prefix in `SendClientMessage`'s panel used elsewhere.
- **Asymmetry vs `MSG_MagicTrumpet`**: both are relays of a single `String[96]`, but at DBSrv `MSG_MagicTrumpet` forwards to **both** game servers (`pUser[0..MAX_SERVER)`) **and** admin/NP sockets (`pAdmin[0..MAX_ADMIN)`) (`CFileDB.cpp:1591-1610`), while `_MSG_DBNotice` forwards **only** to game servers (`CFileDB.cpp:1612-1622`). On the TMSrv side `_MSG_MagicTrumpet` is rebroadcast wholesale via `SyncMulticast(0, (MSG_STANDARD*)Msg, 0)` (`ProcessDBMessage.cpp:136-139`) rather than re-derived into a message panel like `SendNotice` does.

## 7. Side Effects

| Side effect | Source | Trigger |
|---|---|---|
| `Log("not <text>", "-system", NULL)` | `SendFunc.cpp:51-52` | Every `SendNotice` call (including from `_MSG_NPNotice`/timers/etc.) |
| `SendClientMessage(i, Message)` → `MSG_MessagePanel` to each `USER_PLAY` client | `SendFunc.cpp:60`, `:32-44` | All in-game players receive the notice as a chat/message-panel line |
| DBSrv→N×TMSrv socket send | `CFileDB.cpp:1614-1621` | Relay of the received `MSG_DBNotice` to every live game server |
| TMSrv→DBSrv socket send (producer) | `imple.cpp:1198`, `_MSG_MessageWhisper.cpp:1099` | GM triggers the notice |
| `SendNotice` early return (no effect) | `SendFunc.cpp:54-55` | Text begins with `'x` |
| No persistent state | — | No DB writes, no `pUser` field mutations; the packet is purely a message relay |

`SendNotice` is shared infrastructure: besides `_MSG_DBNotice` it is invoked by `_MSG_NPNotice` (`ProcessDBMessage.cpp:119`), `_MSG_MessageDBImple` (`:130` via `Log`/`ProcessImple`), timers (`ProcessSecMinTimer.cpp:108`), item-combine (`_MSG_CombineItemOdin.cpp`), war/kingdom events (`Server.cpp`), `_MSG_GetItem.cpp`, `MobKilled.cpp`, `CWarTower.cpp`. So `_MSG_DBNotice` is just one of many triggers into the same notice pipeline.

## 8. Related Packets

| Packet | Constant / direction | Relationship |
|---|---|---|
| `MSG_MagicTrumpet` | `_MSG_MagicTrumpet = (29\|FLAG_DB2GAME\|FLAG_GAME2DB\|FLAG_GAME2CLIENT)` = `0xD19` (`Basedef.h:1400-1405`) | Identical struct shape; DB relay also reaches admin sockets (`CFileDB.cpp:1591-1610`); TMSrv rebroadcasts raw via `SyncMulticast` (`ProcessDBMessage.cpp:136-139`) instead of `SendNotice`. |
| `MSG_NPNotice` | `_MSG_NPNotice = (9\|FLAG_NP2DB\|FLAG_DB2NP\|FLAG_DB2GAME)` (`Basedef.h:2286-2294`) | NP/admin-originated notice with `Parm1` gate → same `SendNotice` pipeline (`ProcessDBMessage.cpp:114-121`); produced by `SendAdminMessage` (`Server.cpp:268-281`). Distinct struct (has `Parm1/Parm2/AccountName`). |
| `MSG_MessageDBImple` | — | DB→game console command carrying `Level`; also funnels into `ProcessImple` and the same `Log`/`SendNotice`-adjacent path (`ProcessDBMessage.cpp:123-134`). |
| `MSG_MessagePanel` | `_MSG_MessagePanel = (1\|FLAG_GAME2CLIENT)` (`Basedef.h:1194-1199`) | The actual GAME2CLIENT packet delivered to each client by `SendClientMessage` (`SendFunc.cpp:32-44`). |

## 9. Discrepancies & Open Questions

1. **No client→game leg**: there is no `case _MSG_DBNotice` in `TMSrv/ProcessClientMessage.cpp` (grep finds `_MSG_DBNotice` only in `Basedef.h`, `CFileDB.cpp`, `_MSG_MessageWhisper.cpp`, `ProcessDBMessage.cpp`, `imple.cpp`). The `FLAG_GAME2CLIENT`/`FLAG_GAME2DB` bits describe roles the packet plays in different legs, not client-originated traffic. A client never sends this packet; the client→game direction has **no handler**.
2. **DBSrv never originates a notice**: `SendAdminMessage` (`Server.cpp:268-281`) builds `_MSG_NPNotice`, not `_MSG_DBNotice`. So DBSrv is purely a relay; it cannot spawn a `_MSG_DBNotice` of its own. If admin-console broadcast was intended, the `_MSG_NPNotice` path (`SendAdminMessage`) is the one used.
3. **Relay asymmetry vs `MSG_MagicTrumpet`**: `_MSG_DBNotice` skips the admin/NP sockets at DBSrv (`CFileDB.cpp:1612-1622`), while `_MSG_MagicTrumpet` forwards to them (`CFileDB.cpp:1602-1609`). Likely intentional (game-server notices vs a trumpet shown to staff too), but the asymmetry is undocumented in source.
4. **`String` not NUL-forced**: producers `strncpy(..., MESSAGE_LENGTH-1)` (`imple.cpp:1196`, `_MSG_MessageWhisper.cpp:1097`) without explicitly zeroing `String[MESSAGE_LENGTH-1]`; `memset` to 0 before fill (`imple.cpp:1190`, `_MSG_MessageWhisper.cpp:1091`) makes the tail zero in practice, so `%s` usage in `SendNotice` is safe. If a producer omitted the `memset`, the 96-byte field could overrun when `Log`/`SendClientMessage` read it as a C-string.
5. **`MESSAGE_LENGTH` truncation**: the notice is capped at 95 chars + NUL; longer GM text is silently truncated. `SendClientMessage` copies `MESSAGE_LENGTH` bytes into `MSG_MessagePanel.String[128]` (`SendFunc.cpp:39`), so no overflow, but no warning is given to the GM.
6. **`'x` suppression marker** (`SendFunc.cpp:54`) is shared by all `SendNotice` callers — a notice starting with `'x` is dropped with no feedback; a GM typing `'x...` may wonder why nothing appeared.
7. **`temp` copy width**: producer uses `memcpy(temp, str + 7, MESSAGE_LENGTH)` (`imple.cpp:1187`) — copies a full 96 bytes even if the command string is shorter; `temp` is a 256-byte buffer so safe, but trailing bytes after the first NUL are garbage (harmless due to `strncpy` + `%s`).
8. The `_MSG` `_MSG_DBNotice` and `_MSG_MagicTrumpet` differ only in the base constant (30 vs 29) — identical bytes otherwise. There is no in-code distinction that would prevent the two from being used interchangeably; the DB relay is the only behavioral differentiator.

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `legacy/Code/Basedef.h` | 129 | `MESSAGE_LENGTH = 96` |
| `legacy/Code/Basedef.h` | 925-930 | `_MSG` macro (header layout) |
| `legacy/Code/Basedef.h` | 932-941 | Direction flags (`FLAG_DB2GAME = 0x0400`, `FLAG_GAME2DB = 0x0800`, `FLAG_GAME2CLIENT = 0x0100`) |
| `legacy/Code/Basedef.h` | 1407 | `_MSG_DBNotice = (30\|FLAG_DB2GAME\|FLAG_GAME2DB\|FLAG_GAME2CLIENT)` |
| `legacy/Code/Basedef.h` | 1408-1412 | `struct MSG_DBNotice { _MSG; char String[MESSAGE_LENGTH]; }` |
| `legacy/Code/Basedef.h` | 1400-1405 | `_MSG_MagicTrumpet` / `struct MSG_MagicTrumpet` (same shape) |
| `legacy/Code/Basedef.h` | 1194-1199 | `_MSG_MessagePanel` / `struct MSG_MessagePanel` |
| `legacy/Code/Basedef.h` | 2286-2294 | `_MSG_NPNotice` / `struct MSG_NPNotice` |
| `legacy/Code/Basedef.cpp` | 6475 | `BASE_CheckPacket` (disabled) |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 39-52 | Dispatch entry + pre-guard |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 56 | `if (conn == 0)` broadcast branch |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 141-146 | Handler `case _MSG_DBNotice` → `SendNotice(m->String)` |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 136-139 | `case _MSG_MagicTrumpet` → `SyncMulticast` |
| `legacy/Code/TMSrv/imple.cpp` | 46 | `ProcessImple` signature |
| `legacy/Code/TMSrv/imple.cpp` | 1164 | `if (level >= 1)` gate |
| `legacy/Code/TMSrv/imple.cpp` | 1185-1201 | Producer (gm command `notice`) |
| `legacy/Code/TMSrv/_MSG_MessageWhisper.cpp` | 787-793 | Whisper `gm`/`GM` → `ProcessImple`, `level = Level-1000` |
| `legacy/Code/TMSrv/_MSG_MessageWhisper.cpp` | 1088-1101 | Producer (whisper `not`, Level ≥ 1000) |
| `legacy/Code/TMSrv/SendFunc.cpp` | 27-45 | `SendClientMessage` → `MSG_MessagePanel` |
| `legacy/Code/TMSrv/SendFunc.cpp` | 47-62 | `SendNotice` (log, `'x` gate, broadcast) |
| `legacy/Code/DBSrv/Server.cpp` | 1339-1357 | `ProcessClientMessage` (dispatch into `cFileDB.ProcessMessage`) |
| `legacy/Code/DBSrv/Server.cpp` | 268-281 | `SendAdminMessage` → `_MSG_NPNotice` (admin notice producer) |
| `legacy/Code/DBSrv/CFileDB.cpp` | 236-241 | `CFileDB::ProcessMessage` + switch |
| `legacy/Code/DBSrv/CFileDB.cpp` | 1612-1622 | Relay `case _MSG_DBNotice` (game-server broadcast) |
| `legacy/Code/DBSrv/CFileDB.cpp` | 1591-1610 | `case _MSG_MagicTrumpet` (also broadcasts to `pAdmin`) |
| `legacy/Code/Basedef.h` | 48 | `MAX_SERVER = 10` |
| `legacy/Code/Basedef.h` | 56 | `MAX_USER = 1000` |
| `legacy/Code/Basedef.h` | 60 | `MAX_ADMIN = 10` |
| `legacy/Code/CPSock.h` | 38-50 | `MAX_MESSAGE_SIZE`, `INITCODE`, `HEADER` |
| `legacy/Code/CPSock.cpp` | 513-591 | `AddMessage` (framing/obfuscation/checksum) |
| `legacy/Code/CPSock.cpp` | 686-693 | `SendOneMessage` |
| `legacy/Code/CPSock.cpp` | 353-467 | `ReadMessage` (Size range check, deobfuscation, checksum) |
