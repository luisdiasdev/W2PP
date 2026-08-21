# MSG_MagicTrumpet

Server-wide announcement / "magic trumpet" (megaphone) broadcast packet. A player uses a
magic-trumpet item and their line is fanned out to **every** player on **every** game server in
the group, via a DB-server round trip. It is a `_MSG` header plus a 96-byte text payload.

## 1. Summary

| Attribute | Value |
|---|---|
| Type constant | `_MSG_MagicTrumpet` (Basedef.h:1400) |
| Base number | `29` (shared with `_MSG_StillPlaying` = `29 \| FLAG_GAME2CLIENT`) |
| Flag expression | `29 \| FLAG_DB2GAME \| FLAG_GAME2DB \| FLAG_GAME2CLIENT` |
| Computed wire Type value | `0x0D1D` = 3357 (see §9 note; `0xD19` claimed in task = 3353 = `25\|flags`, does **not** match source) |
| Flags present | `FLAG_DB2GAME` 0x0400, `FLAG_GAME2DB` 0x0800, `FLAG_GAME2CLIENT` 0x0100 (Basedef.h:932,935,936) |
| Wire struct | `MSG_MagicTrumpet` (Basedef.h:1401-1405) |
| Layout | MSVC default `/Zp8` (struct is outside every `#pragma pack(1)` region) |
| `sizeof(MSG_MagicTrumpet)` | **108** bytes |
| Expected `Size` field | **108** |
| Payload | `char String[MESSAGE_LENGTH]`, `MESSAGE_LENGTH = 96` (Basedef.h:129) |
| Struct alias (same shape) | `MSG_DBNotice` (`_MSG_DBNotice` = `30\|...` = 0x0D1E, Basedef.h:1407-1412) |
| DB→game handler | `TMSrv/ProcessDBMessage.cpp:136` (via `SyncMulticast`, SendFunc.cpp:260) |
| game→DB producer | `TMSrv/_MSG_MessageWhisper.cpp:125-138` (`spk` whisper command, item sIndex 3330) |
| DB fan-out (DB→game×N + DB→admin) | `DBSrv/CFileDB.cpp:1591-1610` |
| game→client broadcast | via `SyncMulticast` → `cSock.AddMessage` (SendFunc.cpp:260) |
| client→game handler | **NONE** (no `FLAG_CLIENT2GAME`; no case in ProcessClientMessage.cpp) |

## 2. Wire Framing (protocol preamble)

Standard framing for all `_MSG_*` packets, implemented in `CPSock.cpp`; no per-packet
deviation for MSG_MagicTrumpet.

- **Magic / INITCODE**: first 4 bytes on every connection must equal `INITCODE = 0x1F11F311`
  (CPSock.h:40). Checked once at `ReadMessage` when `Init == 0` (CPSock.cpp:366-383); mismatch →
  `ErrorCode = 2`, connection rejected.
- **Size bound**: after the 4-byte magic, `Size` is read at payload offset 0 (CPSock.cpp:390).
  `Size > MAX_MESSAGE_SIZE (8192)` or `Size < sizeof(HEADER) (12)` → reset buffers, reject
  (CPSock.cpp:397-406). MSG_MagicTrumpet's `Size = 108` is well within `[12, 8192]`.
- **Obfuscation (receive side)**: for `i = 4..Size-1`, bytes are de-obfuscated with a
  position-rotating transform keyed by `KeyWord`; transform selected by `i & 0x3`
  (CPSock.cpp:430-453). Offset 4..11 are the header's `Type/ID/ClientTick`; `String` bytes
  (offset 12..107) are the `mod = 12&3 = 0,1,2,3,...` sequence.
- **CheckSum**: `CheckSum = Sum2 - Sum1` where `Sum1` = sum of *de-obfuscated* payload bytes and
  `Sum2` = sum of *obfuscated* (on-the-wire) bytes (CPSock.cpp:424-455). A mismatch sets
  `ErrorCode = 1` but **still returns the packet** (CPSock.cpp:457-464).
- **Send side** mirrors the transform (AddMessage, CPSock.cpp:535-590): random `iKeyWord = rand()%256`,
  `KeyWord = pKeyWord[iKeyWord*2]`, transforms with the inverse shift, computes `CheckSum`,
  overwrites header `Size/KeyWord/CheckSum/ClientTick`, copies the 4-byte header raw
  (CPSock.cpp:586), and queues the frame.
- **`BASE_CheckPacket`** (Basedef.cpp:6475) — the per-packet `sizeof` validator — is entirely
  commented out (`/* ... */`), i.e. **DISABLED**. No `Size`-vs-`sizeof` sanity check runs on the
  wire (the body at Basedef.cpp:6476-* is dead code).

## 3. Binary Layout

### 3.1 Header

The `_MSG` macro (Basedef.h:925-930) defines the 12-byte common header. Packing context: the
`#pragma pack(push, 1)` regions in Basedef.h are **only** lines 808-835, 1212-1246, 1465-1492,
2063-2097. `MSG_MagicTrumpet` (1401) lies in none of them → **MSVC default `/Zp8`**.

| Offset | Size | Type | Field | Alignment | Semantics |
|---|---|---|---|---|---|
| 0 | 2 | short | `Size` | 2 | Total packet size incl. header = 108 |
| 2 | 1 | char | `KeyWord` | 1 | Obfuscation key index (`iKeyWord`), set per-send |
| 3 | 1 | char | `CheckSum` | 1 | `Sum2 - Sum1`, validated at receive |
| 4 | 2 | short | `Type` | 2 | `_MSG_MagicTrumpet` = 0x0D1D |
| 6 | 2 | short | `ID` | 2 | 0 for this packet (producer sets `sm_mt.ID = 0`) |
| 8 | 4 | unsigned int | `ClientTick` | 4 | Timestamp / anti-replay counter; = `CurrentTime` on send |

Header size = 12 bytes; no padding (members are naturally aligned within the struct; max member
alignment = 4). **Header math**: 2+1+1 = 4; `Type` at 4 (aligned 2, 4%2=0); `ID` at 6 (6%2=0);
`ClientTick` at 8 (8%4=0); end 12 (12%4=0). ✓ `sizeof(MSG_STANDARD) = 12` confirmed.

### 3.2 Payload

| Offset | Size | Type | Field | Alignment | Semantics |
|---|---|---|---|---|---|
| 12 | 96 | char[96] | `String` | 1 | Null- (or data-) padded announcement text, `MESSAGE_LENGTH = 96` |

`String` is a `char` array → alignment 1; placed immediately at offset 12 (12%1=0), no padding.
Under `/Zp8` no tail padding is inserted for a char array. **Payload math**: offset 12 + 96 = 108.

### 3.3 Nested struct expansions

None — `MSG_MagicTrumpet` contains only the `_MSG` macro header and a flat `char[96]`. No nested
structs. The producer additionally pokes `String[94] = 1` (see §6) — a raw byte, not a member.

### 3.4 Size verification

- `sizeof(MSG_MagicTrumpet) = sizeof(_MSG header) + 96 = 12 + 96 = 108`.
- Struct alignment = 4 (max member alignment, from `unsigned int ClientTick`); `108 % 4 = 0` → no
  trailing padding. **`sizeof = 108`**.
- Expected `Size` field = **108**.
- Cross-check against source: producer sets `sm_mt.Size = sizeof(MSG_MagicTrumpet)` and
  `DBServerSocket.SendOneMessage((char*)&sm_mt, sizeof(MSG_MagicTrumpet))`
  (_MSG_MessageWhisper.cpp:128,138). DBSrv forwards with `SendOneMessage(Msg, sizeof(MSG_MagicTrumpet))`
  (CFileDB.cpp:1599,1608). `SyncMulticast` sends `m->Size` bytes (SendFunc.cpp:266). All agree on 108.
- No mismatches found; no `UNKNOWN` members (all fields identified).

## 4. Lifecycle & Flow

Flag legs actually present in code:

1. **game→DB (producer)** — `TMSrv` player executes the `spk` whisper command
   (_MSG_MessageWhisper.cpp:96-143). Requires item `sIndex == 3330` in carry; consumes one unit;
   builds `MSG_MagicTrumpet` with `String = "[MobName]> <text>"`, `String[94] = 1`; sends to
   `DBServerSocket` (game→DB socket). No direct client broadcast here — the packet **always** goes
   through the DB server.

2. **DB→game×N + DB→admin (DB fan-out)** — `DBSrv` `Server.cpp:924 ReadMessage` →
   `ProcessClientMessage` (Server.cpp:946) → `cFileDB.ProcessMessage` (CFileDB.cpp:236, switch at
   240) → `case _MSG_MagicTrumpet` (CFileDB.cpp:1591). Requires `std->Type & FLAG_GAME2DB`
   (Server.cpp:1343). Forwards the raw buffer to every connected game server `pUser[i]` (loop
   `MAX_SERVER = 10`) and every admin console `pAdmin[i]` (loop `MAX_ADMIN = 10`)
   (CFileDB.cpp:1593-1609). The originating game server receives its own trumpet back.

3. **DB→game (receive handler)** — each game server's `DBServerSocket.ReadMessage`
   (TMSrv/Server.cpp:3889) → `ProcessDBMessage(Msg)` (TMSrv/Server.cpp:3914). Requires
   `std->Type & FLAG_DB2GAME`, `0 <= ID < MAX_USER` (ProcessDBMessage.cpp:43); `conn == 0`
   dispatches into the DB-message switch (ProcessDBMessage.cpp:56-58) → `case _MSG_MagicTrumpet`
   (ProcessDBMessage.cpp:136) → `SyncMulticast(0, (MSG_STANDARD*)Msg, 0)` (SendFunc.cpp:260).

4. **game→client (broadcast)** — `SyncMulticast` iterates all `MAX_USER` slots; for each with
   `Mode == USER_PLAY` and `conn != i` (origin = 0 here, so all players) it
   `AddMessage((char*)m, m->Size)` with `bSend = 0` (buffered, flushed on next `SendMessageA`)
   (SendFunc.cpp:262-271). The **raw** MSG_MagicTrumpet (including `String[94]=1` and header) is
   delivered to every client; the client renders the announcement because `Type` carries
   `FLAG_GAME2CLIENT`.

5. **client→game** — **NO handler.** `_MSG_MagicTrumpet` has no `FLAG_CLIENT2GAME`; the
   client-message dispatcher `TMSrv/ProcessClientMessage.cpp` switch (ProcessClientMessage.cpp:66)
   contains no `case _MSG_MagicTrumpet` and no `Exec_MSG_MagicTrumpet` exists in `_MSG_*.cpp`.
   Clients cannot originate a trumpet.

Sequence (normal player broadcast):

```
Player ──"spk <text>" whisper──▶ TMSrv (item 3330)
 TMSrv ──MSG_MagicTrumpet (game→DB)──▶ DBSrv
 DBSrv ──MSG_MagicTrumpet (DB→game)──▶ each TMSrv (MAX_SERVER)
 DBSrv ──MSG_MagicTrumpet (DB→NP)────▶ each admin console (MAX_ADMIN)
 each TMSrv ──MSG_MagicTrumpet (game→client)──▶ all USER_PLAY clients (SyncMulticast)
```

## 5. Validation & Guards

Execution-order guard table for the producer path (`_MSG_MessageWhisper.cpp`):

| # | Location | Guard | On failure |
|---|---|---|---|
| 1 | :29 | `pUser[conn].Mode != USER_PLAY` → return | drop |
| 2 | :96 | `strcmp(m->MobName, "spk") != 0` → other branch | not a trumpet |
| 3 | :100 | `pUser[conn].MuteChat == 1` | `SendClientMessage(..._NN_No_Speak)` + return (muted) |
| 4 | :106-122 | scan `pMob[conn].MOB.Carry[0..MaxCarry)` for `sIndex == 3330`; `i == MaxCarry` → return | no item → drop |
| 5 | :111-116 | consume one unit 3330 (`BASE_GetItemAmount` / `BASE_SetItemAmount` / `BASE_ClearItem`) | — |

DB-side guard: `ProcessClientMessage` requires `Type & FLAG_GAME2DB` and `0 <= ID <= MAX_USER`
(DBSrv/Server.cpp:1343); otherwise logs `err,packet ...` and drops.

Game-side receive guard: `ProcessDBMessage` requires `Type & FLAG_DB2GAME`, `0 <= ID < MAX_USER`
(ProcessDBMessage.cpp:43), and dispatch only when `conn == 0` (ProcessDBMessage.cpp:56).

There is **no** rate limiting, cooldown, or cost other than the single item-3330 unit consumed.
No GM/administrator-only producer exists in code (see §9).

## 6. Game Mechanics & Business Logic

- **Trigger**: the `spk` whisper command (`_MSG_MessageWhisper.cpp:96`). The player whispers to a
  "mob name" of `spk` with the announcement text in `m->String`.
- **Item gate**: the player must hold at least one `sIndex == 3330` item in their carry bag
  (_MSG_MessageWhisper.cpp:108). One unit is consumed per broadcast (amount-1, or cleared if the
  last unit; :111-116).
- **Text assembly**: `sprintf(temp, "[%s]> %s", pMob[conn].MOB.MobName, m->String)`
  (_MSG_MessageWhisper.cpp:132); then `strncpy(sm_mt.String, temp, 96)` (:134).
- **Marker byte**: `sm_mt.String[94] = 1` (_MSG_MessageWhisper.cpp:136) — a hard-coded byte set
  inside the payload, whose client-side meaning is UNKNOWN (not part of the struct; likely a
  formatting/style flag in the client, or a termination sentinel the client interprets).
- **Routing**: always game→DB→(all game servers + all admins)→game→clients. The speaker's own
  client receives the message via the DB round-trip (SyncMulticast with origin `conn = 0`).
- **Coverage**: broadcast reaches `USER_PLAY` players on **all** `MAX_SERVER` game servers in the
  group (CFileDB.cpp:1593-1599; SyncMulticast), not just the speaker's server.
- **Mute**: a muted player (`MuteChat == 1`) is refused before the item is consumed
  (_MSG_MessageWhisper.cpp:100-104).

## 7. Side Effects

- **Client fan-out**: every `USER_PLAY` player on every game server receives the raw
  `MSG_MagicTrumpet` (`SyncMulticast`, SendFunc.cpp:262-271).
- **Admin fan-out**: every connected admin console (`pAdmin[i]`, `MAX_ADMIN`) receives the raw
  buffer (CFileDB.cpp:1602-1608).
- **Item consumption**: one `sIndex == 3330` unit removed from the speaker's carry
  (_MSG_MessageWhisper.cpp:111-116), plus an item-update side effect via `SendItem` if the item
  was fully consumed (`BASE_ClearItem`).
- **Chat log**: `sprintf(temp, "chat_spk,%s %s", pMob[conn].MOB.MobName, sm_mt.String);`
  `ChatLog(temp, pUser[conn].AccountName, pUser[conn].IP)` (_MSG_MessageWhisper.cpp:140-141).
- **Buffered send**: on the game server, broadcast is queued with `AddMessage` (`bSend = 0`,
  flushed on next `SendMessageA`), not an immediate socket write.
- **DB-side logging**: any rejected/odd DB packet logs `err,packet Type:%d ID:%d Size:%d KeyWord:%d`
  (DBSrv/Server.cpp:1349). `ProcessDBMessage` similarly logs on guard failure
  (ProcessDBMessage.cpp:47).
- No `pUser` state (other than item consumption) and no timer/cooldown is modified.

## 8. Related Packets

| Packet | Type / hex | Relation |
|---|---|---|
| `MSG_DBNotice` / `_MSG_DBNotice` | `30\|DB2GAME\|GAME2DB\|GAME2CLIENT` = 0x0D1E (Basedef.h:1407-1412) | Identical struct shape (`_MSG` + `char[96]`); server-wide notice produced by `notice` GM cmd (imple.cpp:1185-1201) and handled at ProcessDBMessage.cpp:141 via `SendNotice` (SendFunc.cpp:47). |
| `_MSG_StillPlaying` | `29 \| GAME2CLIENT` = 0x011D (Basedef.h:1396) | Shares base number 29 with `_MSG_MagicTrumpet` but only the GAME2CLIENT flag. |
| `_MSG_NPNotice` | NP→admin notice (DBSrv/Server.cpp:268-281) | Admin-message path (`MSG_NPNotice`), distinct from the trumpet. |
| `_MSG_MessageWhisper` | client whisper (Basedef.h, `MESSAGEWHISPER_LENGTH = 100`) | The inbound carrier that triggers the `spk` producer. |
| `_MSG_MessagePanel` | panel notice (SendFunc.cpp:27-45) | `SendNotice`/`SendNoticeChief` delivery primitive used by related notices. |

## 9. Discrepancies & Open Questions

- **Type hex**: The task statement gives `_MSG_MagicTrumpet = 0xD19`, but the authoritative source
  expression `29 | FLAG_DB2GAME(0x0400) | FLAG_GAME2DB(0x0800) | FLAG_GAME2CLIENT(0x0100)`
  (Basedef.h:1400, 932, 935, 936) computes to **0x0D1D = 3357**. `0xD19` (= 3353) would be
  `25 | flags` (i.e. `_MSG_CharacterLoginFail`'s base). Source wins → documented Type is 0x0D1D.
- **`String[94] = 1`** (_MSG_MessageWhisper.cpp:136): hard-coded marker inside the payload; exact
  client interpretation UNKNOWN (not part of the struct definition).
- **No GM/admin trumpet**: unlike `notice`/`chiefnotice` (imple.cpp:1185,1494), there is **no**
  GM command that produces `_MSG_MagicTrumpet` — the only in-code producer is the player `spk`
  command gated by item 3330. No rate/cost beyond the single item unit.
- **client→game leg unused**: `_MSG_MagicTrumpet` carries no `FLAG_CLIENT2GAME` and
  `ProcessClientMessage.cpp` has no case — dead direction, as expected.
- **Speaker receives own message via DB**: the producer never sends directly to the client; the
  origin server gets its own trumpet back through the DB round trip (SyncMulticast origin `conn=0`).
- **`BASE_CheckPacket` disabled** (Basedef.cpp:6475): no `Size`-vs-`sizeof` validation is enforced
  at the socket layer; packets rely on the framing `Size` bounds check (CPSock.cpp:397).
- **CheckSum mismatch tolerated**: `ReadMessage` still returns the packet when the checksum fails
  (CPSock.cpp:457-464); only the app-level flag guards catch malformed trumpets.

## 10. Source References

| File | Line(s) | Content |
|---|---|---|
| legacy/Code/Basedef.h | 129 | `MESSAGE_LENGTH 96` |
| legacy/Code/Basedef.h | 925-930 | `_MSG` macro (12-byte header) |
| legacy/Code/Basedef.h | 932-941 | `FLAG_*` constants |
| legacy/Code/Basedef.h | 1400 | `_MSG_MagicTrumpet = (29\|DB2GAME\|GAME2DB\|GAME2CLIENT)` |
| legacy/Code/Basedef.h | 1401-1405 | `struct MSG_MagicTrumpet { _MSG; char String[96]; }` |
| legacy/Code/Basedef.h | 1407-1412 | `_MSG_DBNotice` / `MSG_DBNotice` (same shape) |
| legacy/Code/Basedef.h | 808,835,1212,1246,1465,1492,2063,2097 | `#pragma pack` regions (struct not packed) |
| legacy/Code/Basedef.cpp | 6475+ | `BASE_CheckPacket` — disabled (commented out) |
| legacy/Code/TMSrv/ProcessDBMessage.cpp | 39-58, 136-139 | DB→game guard + `case _MSG_MagicTrumpet` → `SyncMulticast` |
| legacy/Code/TMSrv/SendFunc.cpp | 260-272 | `SyncMulticast` broadcast to `USER_PLAY` clients |
| legacy/Code/TMSrv/_MSG_MessageWhisper.cpp | 96-143 | `spk` producer (item 3330, build, send, ChatLog) |
| legacy/Code/TMSrv/ProcessClientMessage.cpp | 66+ | Client dispatcher — no MagicTrumpet case |
| legacy/Code/TMSrv/Server.cpp | 3889, 3914 | DBServerSocket read → `ProcessDBMessage` |
| legacy/Code/DBSrv/CFileDB.cpp | 236, 1591-1610 | DB switch, `case _MSG_MagicTrumpet` fan-out |
| legacy/Code/DBSrv/Server.cpp | 924, 946, 1339-1357, 1343 | game-server read → `ProcessClientMessage` → `cFileDB.ProcessMessage` |
| legacy/Code/CPSock.h | 38-50 | `MAX_MESSAGE_SIZE`, `INITCODE`, `HEADER` |
| legacy/Code/CPSock.cpp | 366-467, 513-591 | framing, obfuscation, checksum, send-side build |
| legacy/Code/TMSrv/CUser.h | 26, 36 | `USER_EMPTY 0`, `USER_PLAY 22` |
| legacy/Code/Basedef.h | 48-58 | `MAX_SERVER 10`, `MAX_ADMIN 10`, `MAX_USER 1000` |
