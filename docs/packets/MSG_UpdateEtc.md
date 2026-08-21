# MSG_UpdateEtc

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_UpdateEtc` = `(55 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME)` = `0x0337` = `823` (`Basedef.h:1494`) |
| Sequence ID | 55 |
| Direction(s) | Flagged Bidirectional (`FLAG_GAME2CLIENT` \| `FLAG_CLIENT2GAME`) but **effectively outbound-only**: TMSrv → client. **No inbound `case _MSG_UpdateEtc` exists** in `ProcessClientMessage.cpp` (see §4/§9). No DB relay. |
| Wire struct | `MSG_UpdateEtc` (`Basedef.h:1495-1509`) — `_MSG` header + `Hold, Exp, Learn, ScoreBonus, SpecialBonus, SkillBonus, Magic, Coin` |
| Total size | **48 bytes** (`sizeof(MSG_UpdateEtc)`; see §3.4) |
| Packing | **Default MSVC `/Zp8`** — NOT in any `#pragma pack(push,1)` region. The last pack(1) region ends with `#pragma pack(pop)` at `Basedef.h:1492`; this struct begins at `1495`, **after** that region. The four pack(1) regions are `Basedef.h:808-835`, `1212-1246`, `1465-1492`, `2063-2097`. |
| Producer | `SendEtc(int conn)` @ `TMSrv/SendFunc.cpp:1195-1231` (sets `Type`, `Size`, `ID` and all payload fields, then `AddMessage`). No other producer exists (`_MSG_UpdateEtc` appears only in `SendFunc.cpp` and `Basedef.cpp`). |
| Aliases | Low byte 55 is used **only** by `_MSG_UpdateEtc`. Neighbors: `_MSG_UpdateScore` = 54 (`Basedef.h:1466`), `_MSG_CNFMobKill` = 56 (`Basedef.h:1511`). |
| Related | `_MSG_UpdateScore` (54, `0x0336`) — sibling stat-refresh packet (`Basedef.h:1466`, produced by `SendScore`); `_MSG_UpdateCarry` (produced by `SendCarry`, `SendFunc.cpp:1534`) refreshes inventory+coin. `SendEtc` and `SendScore` are frequently invoked together. |

## 2. Wire Framing

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with a 4-byte `INITCODE = 0x1F11F311` magic sent by the connecting side and validated by the receiver before any framed message is parsed (`CPSock.h:40`; sent `CPSock.cpp:249-250`; validated `CPSock.cpp:372-377`). This magic is a **connection-level handshake token, not a per-packet prefix**.
- Payload bytes **from offset 4 onward** (`i = 4 .. Size-1`) are obfuscated per byte with a position-rotating **add/sub transform** (NOT XOR) keyed by `KeyWord`: `iKeyWord` (random 0-255) indexes the shared `pKeyWord[512]` table, `pos` seeds at `pKeyWord[iKeyWord*2]`, and `mod = i&0x3` selects one of four transforms:
  - `mod 0`: `b = b + (Trans << 1)`
  - `mod 1`: `b = b - (Trans >> 3)`
  - `mod 2`: `b = b + (Trans << 2)`
  - `mod 3`: `b = b - (Trans >> 5)`
  - Send: `CPSock.cpp:558-581`; inverse (deobfuscate) on receive: `CPSock.cpp:414-437`.
- `CheckSum = Sum2 - Sum1` (sum of transformed payload bytes minus sum of raw payload bytes); written back to the header after the transform loop (`CPSock.cpp:583-584`) and validated on receive (`CPSock.cpp:443+`).
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` else the receive buffer is reset (`CPSock.cpp:379-383`, `MAX_MESSAGE_SIZE = 8192`, `CPSock.h:38`).
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is **disabled** (body commented out) — but the commented-out central validation for this packet was `m->Size != sizeof(MSG_UpdateEtc)` → `code = 1` (`Basedef.cpp:6504`).

Per-packet notes:
- No deviation from standard framing. It is a plain outbound stat-refresh frame on the client socket.
- `Type = 0x0337` on the wire. The producer does **not** rewrite `Type`; it sets `Size = sizeof(MSG_UpdateEtc)` and `ID = conn` (`SendFunc.cpp:1210-1213`).
- `Size` is set to `sizeof(MSG_UpdateEtc)` = 48, and `AddMessage` is called with the same `sizeof` (`SendFunc.cpp:1211,1229`).
- There is **no inbound leg**: a client that sends `Type=0x0337` gets no `case` in the dispatcher, so it is silently ignored (see §4/§9). (Contrast `_MSG_UpdateScore`, which has an inbound `case` that flags the sender as a cracker: `ProcessClientMessage.cpp:106-110`.)

## 3. Binary Layout

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 48) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index (random 0-255) |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_UpdateEtc` = 0x0337 |
| 6 | 2 | `ID` | `short` | Target client connection slot (`conn`); producer sets `ID = conn` (`SendFunc.cpp:1213`) |
| 8 | 4 | `ClientTick` | `unsigned int` | Client tick; set to `CurrentTime` by `AddMessage` (`CPSock.cpp:541`) |

Header = 12 bytes, offsets 0-11, no internal padding.

### 3.2 Payload

Packing context: **default `/Zp8`** (this struct is outside every pack(1) region — see §1). Member alignment = `min(sizeof(member), 8)`; `long long` aligns to 8. The struct's own alignment is 8 (largest member alignment), so `sizeof` rounds up to a multiple of 8.

The `_MSG` header ends at offset 12 (4-aligned). Layout math:

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 4 | `Hold` | `unsigned int` | 4 | 0 | User "Hold" value = `pMob[conn].extra.Hold` (`SendFunc.cpp:1226`); `unsigned int` in the `CUser` extra struct (`Basedef.h:576`). Full 4-byte semantics UNKNOWN (see §9). |
| 16 | 8 | `Exp` | `long long` | 8 | 0 | Experience points = `pMob[conn].MOB.Exp` (`SendFunc.cpp:1215`) |
| 24 | 8 | `Learn` | `long long` | 8 | 0 | Learned-skill bitfield = `pMob[conn].MOB.LearnedSkill` (`SendFunc.cpp:1219`). Source is a 4-byte `long` (`Basedef.h:461`), so the high 4 bytes of this 8-byte field stay 0 (memset). |
| 32 | 2 | `ScoreBonus` | `unsigned short` | 2 | 0 | Unspent score points (Str/Int/Dex/Con) = `pMob[conn].MOB.ScoreBonus` (`SendFunc.cpp:1221`) |
| 34 | 2 | `SpecialBonus` | `unsigned short` | 2 | 0 | Unspent special points = `pMob[conn].MOB.SpecialBonus` (`SendFunc.cpp:1217`) |
| 36 | 2 | `SkillBonus` | `unsigned short` | 2 | 0 | Points usable to buy skills = `pMob[conn].MOB.SkillBonus` (`SendFunc.cpp:1222`) |
| 38 | 2 | `Magic` | `unsigned short` | 2 | 0 | Magic value = `pMob[conn].MOB.Magic` (`SendFunc.cpp:1227`). **Truncation**: source `MOB.Magic` is `unsigned int` (4 bytes, `Basedef.h:463`) but the wire field is `unsigned short` (2 bytes); the high 2 bytes are dropped. |
| 40 | 4 | `Coin` | `int` | 4 | 0 | Money = `pMob[conn].MOB.Coin` (`SendFunc.cpp:1224`) |
| 44 | 4 | *(padding)* | — | 8 | 4 | Trailing padding to align `sizeof` to the struct alignment (8). Zeroed by `memset(&sm, 0, sizeof(MSG_UpdateEtc))` (`SendFunc.cpp:1208`). |

**Payload total: 36 bytes (offsets 12-47) → total struct 48 bytes.**

### 3.3 Nested struct expansions

`MSG_UpdateEtc` embeds only the `_MSG` header macro (primitive fields) plus flat primitive members. It contains **no `STRUCT_*` members** (no `STRUCT_SCORE`, no `STRUCT_ITEM`, no `STRUCT_AFFECT`), so there are no nested struct expansions.

### 3.4 Size verification

Manual `/Zp8` computation:

```
_MSG header (Basedef.h:925-930):
  short          Size         @ 0  (2)
  char           KeyWord      @ 2  (1)
  char           CheckSum     @ 3  (1)
  short          Type         @ 4  (2)   (4-aligned)
  short          ID           @ 6  (2)
  unsigned int   ClientTick   @ 8  (4)   (4-aligned)
  => 12 bytes (0-11), no padding

Payload (default /Zp8):
  unsigned int   Hold         @ 12 (4)   -> 12-15
  long long      Exp          @ 16 (8)   -> 16-23   (8-aligned, no pad)
  long long      Learn        @ 24 (8)   -> 24-31   (8-aligned)
  unsigned short ScoreBonus   @ 32 (2)   -> 32-33
  unsigned short SpecialBonus @ 34 (2)   -> 34-35
  unsigned short SkillBonus   @ 36 (2)   -> 36-37
  unsigned short Magic        @ 38 (2)   -> 38-39
  int            Coin         @ 40 (4)   -> 40-43   (4-aligned)
  padding                      @ 44 (4)   -> 44-47   (struct alignment = 8; 44 % 8 != 0, round up to 48)

  sizeof(MSG_UpdateEtc) = 48 bytes
```

Cross-check against producer: `sm.Size = sizeof(MSG_UpdateEtc)` and `AddMessage((char*)&sm, sizeof(MSG_UpdateEtc))` (`SendFunc.cpp:1211,1229`) — the wire `Size` field and the send length both equal the compiler-computed 48. No `sizeof` mismatch detected.

## 4. Lifecycle & Flow

`MSG_UpdateEtc` is **outbound-only** (TMSrv → client). It is produced solely by `SendEtc` and pushed through the per-user socket.

```
SendEtc(conn)                                    SendFunc.cpp:1195
  ├─ guards: conn in [1,MAX_USER), Mode==USER_PLAY, Sock!=0  1197-1204
  ├─ MSG_UpdateEtc sm; memset(0, sizeof)                      1206-1208
  ├─ Type = _MSG_UpdateEtc; Size = sizeof; ID = conn          1210-1213
  ├─ payload fill (see §3.2)                                  1215-1227
  └─ cSock.AddMessage(&sm, sizeof) → else CloseUser           1229-1230
        └─ CPSock::AddMessage  CPSock.cpp:513
             ├─ buffer/socket guards                            518-533
             ├─ random KeyWord, Size/CheckSum/ClientTick set   535-541
             ├─ obfuscate bytes 4..Size-1 (add/sub, pKeyWord)  558-581
             └─ CheckSum = Sum2 - Sum1                         583-584
        └─ later flushed by CPSock::SendMessageA (CPSock.cpp:617)
```

Who calls `SendEtc` (24 call sites across the codebase) and when:

- **On login / character-enter**: `ProcessDBMessage.cpp:1075` — immediately after the DB confirms a character login, `SendEtc(conn)` then `SendScore(conn)` push the full "etc" stat set to the client. This is the primary "on login" refresh.
- **On stat/bonus change (`_MSG_ApplyBonus`)**: `_MSG_ApplyBonus.cpp:66,110,126,208` — after spending `ScoreBonus`/`SpecialBonus`/`SkillBonus`, `SendEtc` refreshes the client's remaining bonus-point counters (typically followed by `SendScore`).
- **On economy events (coins)**: `_MSG_Buy.cpp:282`, `_MSG_Sell.cpp:233`, `_MSG_ReqBuy.cpp:161`, `_MSG_GetItem.cpp:124` — coin/currency changes refresh `Coin`.
- **On quest / XP / level events**: `_MSG_Quest.cpp:228,541,593,678,1449,1887,2132`, `MobKilled.cpp:1755,2226,2234`, `_MSG_Attack.cpp:231,1042,1253,1262,1773` — XP (`Exp`) and learned-skill (`Learn`) updates after kills/quests.
- **On item combine/use**: `_MSG_UseItem.cpp` (multiple), `_MSG_CombineItem*.cpp` (Ailyn/Ehre/Tiny) — refresh after item effects alter stats/coin.
- **Misc**: `_MSG_InviteGuild.cpp:73`, `_MSG_MessageWhisper.cpp:214`, `_MSG_ReqRanking.cpp:68-69`, `_MSG_ReqTeleport.cpp:43`, `_MSG_Challange.cpp:75`, `_MSG_Restart.cpp:45`, `CCastleZakum.cpp:252,289` (party member refresh), and GM/admin `imple.cpp:157,503,554,1593,1600` (e.g. `/learn`, `/class`, NPC save/read).

There is **no inbound client→game dispatcher case** for `_MSG_UpdateEtc`. `ProcessClientMessage.cpp` contains `case _MSG_UpdateScore` (`:106`, which logs the sender as a cracker and adds `AddCrackError`) but **no `case _MSG_UpdateEtc`**. A client frame with `Type=0x0337` therefore matches no `case` in the main dispatch switch and is ignored. So the "`FLAG_CLIENT2GAME`" bit is vestigial for this packet.

## 5. Validation & Guards

Guards in the producer `SendEtc` (`SendFunc.cpp:1197-1204`):

| # | Guard | Action on fail | Location |
|---|---|---|---|
| 1 | `conn <= 0 \|\| conn >= MAX_USER` | `return` (drop) | `SendFunc.cpp:1197-1198` |
| 2 | `pUser[conn].Mode != USER_PLAY` | `return` (drop) | `SendFunc.cpp:1200-1201` |
| 3 | `pUser[conn].cSock.Sock == 0` | `return` (drop) | `SendFunc.cpp:1203-1204` |
| 4 | `AddMessage` returns `FALSE` | `CloseUser(conn)` | `SendFunc.cpp:1229-1230` |

Transport-level guards in `CPSock::AddMessage` (`CPSock.cpp:518-533`): send buffer would overflow (`nSendPosition + Size >= SEND_BUFFER_SIZE` → `FALSE`) or socket invalid (`Sock <= 0` → `FALSE`).

Packet-level size validation: `BASE_CheckPacket` is disabled (commented out, `Basedef.cpp:6475`); the historical check for this type was `m->Size != sizeof(MSG_UpdateEtc)` → `code = 1` (`Basedef.cpp:6504`). With the check disabled, receive-side framing relies only on the generic `Size` bounds check (`CPSock.cpp:379-383`).

## 6. Game Mechanics & Business Logic

The packet refreshes the client's "etc" (miscellaneous/secondary) character stats, distinct from the primary `STRUCT_SCORE` stats carried by the sibling `_MSG_UpdateScore`. Field semantics per producer mapping (`SendFunc.cpp:1215-1227`) and `Basedef.h`:

- **`Exp`** (`long long`) — total experience points, `pMob[conn].MOB.Exp`. Drives level-ups (e.g. `pMob[conn].CheckGetLevel()` called after `SendEtc` in `imple.cpp:157-158`).
- **`Learn`** (`long long`) — learned-skill bitmask, `pMob[conn].MOB.LearnedSkill`. Source is a 4-byte `long`; the packet field is 8 bytes (high dword zero).
- **`ScoreBonus`** (`ushort`) — unspent primary-stat points (Str/Int/Dex/Con); decremented by `_MSG_ApplyBonus` (`_MSG_ApplyBonus.cpp:45`, then `SendEtc`).
- **`SpecialBonus`** (`ushort`) — unspent special points; decremented by `_MSG_ApplyBonus` (`_MSG_ApplyBonus.cpp:113`).
- **`SkillBonus`** (`ushort`) — points usable to purchase skills.
- **`Magic`** (`ushort`) — magic value, `pMob[conn].MOB.Magic` (an `unsigned int`, truncated to 16 bits on the wire).
- **`Coin`** (`int`) — money, `pMob[conn].MOB.Coin`; refreshed on buy/sell/drop/get/quest rewards.
- **`Hold`** (`unsigned int`) — the user's "Hold" counter, `pMob[conn].extra.Hold` (`CUser` extra struct). Purpose not spelled out in the producer; its exact client meaning is UNKNOWN (see §9).

There is no business logic inside the packet handler (there is no handler). All logic lives in the callers that mutate the underlying `pMob[conn].MOB` / `pUser[conn].extra` state before invoking `SendEtc`.

## 7. Side Effects

- **Client display refresh**: the receiving client updates its on-screen experience, learned-skill, bonus-point (score/special/skill), magic, coin and "hold" displays to the pushed values.
- **Server-side**: `SendEtc` itself has no state mutation — it only serializes current state. Side effects are limited to:
  - the socket send buffer append (`AddMessage`), and
  - `CloseUser(conn)` if the send buffer is full or the socket is invalid (`SendFunc.cpp:1229-1230`).
- **Framing side effects** in `AddMessage`: `pSMsg->KeyWord` randomized, `pSMsg->CheckSum` written as `Sum2 - Sum1`, `ClientTick = CurrentTime`, `LastSendTime = CurrentTime` (`CPSock.cpp:535-542`).
- **No persistence / DB side effect**: the packet does not touch DBSrv; state changes that trigger it are already saved by their own logic.

## 8. Related Packets

- **`_MSG_UpdateScore`** (`54 | GAME2CLIENT | CLIENT2GAME` = `0x0336`, `Basedef.h:1466`, struct `MSG_UpdateScore` `Basedef.h:1467-1491`) — the sibling primary-stat packet (`STRUCT_SCORE` + affects + current HP/MP). Produced by `SendScore`; `SendScore` + `SendEtc` are called back-to-back at login (`ProcessDBMessage.cpp:1075-1076`), on stat spending (`_MSG_ApplyBonus.cpp`), and on NPC edits (`imple.cpp:502-503`). Unlike `MSG_UpdateEtc`, `_MSG_UpdateScore` **has** an inbound `case` in `ProcessClientMessage.cpp:106-110`, which flags the sender as a cracker (`Log` + `AddCrackError(conn, 2, 91)`).
- **`_MSG_UpdateCarry`** — inventory/coin refresh produced by `SendCarry` (`SendFunc.cpp:1534-1559`, sets `Carry[MAX_CARRY]` and `Coin`). `Coin` is duplicated between `MSG_UpdateCarry` and `MSG_UpdateEtc`.
- **`_MSG_CNFMobKill`** (`56`, `Basedef.h:1511`) — structurally similar stat frame (`Hold`, `KilledMob`, `Killer`, `Exp`), the next sequence slot after `_MSG_UpdateEtc`.

## 9. Discrepancies & Open Questions

- **No inbound handler**: `_MSG_UpdateEtc` is declared with both `FLAG_GAME2CLIENT | FLAG_CLIENT2GAME`, but there is **no `case _MSG_UpdateEtc`** in `ProcessClientMessage.cpp` (only `_MSG_UpdateScore` has one, and it treats the inbound frame as a crack attempt). The `CLIENT2GAME` bit appears vestigial / unused for this packet.
- **`Hold` semantics**: `Hold` is assigned from `pMob[conn].extra.Hold` (`SendFunc.cpp:1226`; `unsigned int` at `Basedef.h:576`). The producer gives no hint of its gameplay meaning — UNKNOWN (possibly a premium/currency counter). Not defined/used elsewhere in an obvious packet producer.
- **`Magic` truncation**: the wire field is `unsigned short`, but the source `pMob[conn].MOB.Magic` is `unsigned int` (`Basedef.h:463`). Values ≥ 65536 are truncated on the wire (high 2 bytes dropped). Whether that is intended (client field is 16-bit) or a latent bug is UNKNOWN.
- **`Learn` width mismatch**: the packet field is `long long`, but the source `MOB.LearnedSkill` is a 4-byte `long` (`Basedef.h:461`). The high dword is always zero; the client presumably only reads the low 32 bits. UNKNOWN whether the 8-byte field is a client expectation or legacy residue.
- **Obfuscation is add/sub, not XOR**: the framing uses a 4-way add/sub transform keyed by `pKeyWord` and `mod = i&0x3` (`CPSock.cpp:558-581`), and `INITCODE` is a connection-handshake magic, not a per-packet prefix. Any description of per-packet "XOR obfuscation" or an in-band `INITCODE` magic would be inaccurate for this codebase.
- **Size check disabled**: `BASE_CheckPacket` is disabled (`Basedef.cpp:6475`), so the historical `m->Size != sizeof(MSG_UpdateEtc)` validation (`Basedef.cpp:6504`) is never enforced at runtime.
- **Trailing padding on the wire**: the 4 bytes at offsets 44-47 are compiler padding (struct alignment 8); they are sent as zeros (memset) and carry no meaning.

## 10. Source References

- `legacy/Code/Basedef.h:925-930` — `_MSG` header macro (Size, KeyWord, CheckSum, Type, ID, ClientTick).
- `legacy/Code/Basedef.h:932-941` — flag constants (`FLAG_GAME2CLIENT` 0x100, `FLAG_CLIENT2GAME` 0x200, etc.).
- `legacy/Code/Basedef.h:1465-1492` — preceding `#pragma pack(push,1)` region (`MSG_UpdateScore`), closed by `#pragma pack(pop)` at 1492.
- `legacy/Code/Basedef.h:1494-1509` — `_MSG_UpdateEtc` constant (1494) and `MSG_UpdateEtc` struct (1495-1509).
- `legacy/Code/Basedef.h:461,463` — `MOB.LearnedSkill` (long), `MOB.Magic` (unsigned int) source fields.
- `legacy/Code/Basedef.h:576` — `extra.Hold` (unsigned int) in the `CUser` extra struct.
- `legacy/Code/Basedef.cpp:6475` — `BASE_CheckPacket` (disabled).
- `legacy/Code/Basedef.cpp:6504` — historical size check for `_MSG_UpdateEtc`.
- `legacy/Code/TMSrv/SendFunc.cpp:1195-1231` — `SendEtc` producer (guards, field fill, `AddMessage`).
- `legacy/Code/TMSrv/SendFunc.cpp:1534-1559` — `SendCarry` (`MSG_UpdateCarry`).
- `legacy/Code/CPSock.h:38,40` — `MAX_MESSAGE_SIZE`, `INITCODE`.
- `legacy/Code/CPSock.cpp:249-250,372-377` — INITCODE handshake send/validate.
- `legacy/Code/CPSock.cpp:513-591` — `AddMessage` framing (obfuscation, checksum, buffer).
- `legacy/Code/CPSock.cpp:414-437` — receive deobfuscation.
- `legacy/Code/TMSrv/ProcessClientMessage.cpp:106-110` — `case _MSG_UpdateScore` (cracker flag); no `_MSG_UpdateEtc` case.
- `legacy/Code/TMSrv/ProcessDBMessage.cpp:1075-1076` — `SendEtc`/`SendScore` on login.
- `legacy/Code/TMSrv/_MSG_ApplyBonus.cpp:45,66,110,113,126,208` — bonus spending + `SendEtc`.
- `legacy/Code/TMSrv/imple.cpp:157,503,554,1593,1600` — GM/admin uses of `SendEtc`.
