# MSG_CNFMobKill

## 1. Summary

| Attribute | Value |
|---|---|
| Symbol | `_MSG_CNFMobKill` |
| Type value | `(56 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME)` = `56 \| 0x100 \| 0x200` = **`0x338`** |
| Flags | `GAME2CLIENT (0x100)` + `CLIENT2GAME (0x200)` → flag-wise bidirectional, but **no inbound leg exists** (see §4) |
| Wire struct | `MSG_CNFMobKill` (Basedef.h:1512–1520) |
| Packing | **default MSVC `/Zp8`** — struct at 1512 sits AFTER `#pragma pack(pop)` at 1492 and BEFORE `#pragma pack(push,1)` at 2063. NOT in any pack(1) region → member alignment applies. `long long Exp` forces 8-byte alignment → **4 bytes padding** |
| `sizeof(MSG_CNFMobKill)` | **32 bytes** (12 header + 16 payload + 4 padding) |
| Producer (outbound) | `void MobKilled(int target, int conn, int PosX, int PosY)` — MobKilled.cpp:41; packet built at MobKilled.cpp:173–181 |
| Send path | `GridMulticast(...)` (MobKilled.cpp:196/634/2036/2043/2161) → **per-recipient customization** in SendFunc.cpp:920–966 → `AddMessage` (SendFunc.cpp:968) |
| Inbound handler | **none** — no `case _MSG_CNFMobKill` in `ProcessClientMessage.cpp` or anywhere else (grep: only Basedef.h, Basedef.cpp, MobKilled.cpp, SendFunc.cpp) |
| Producer struct-size cross-check | `MobKilled.cpp:177` `sm.Size = sizeof(MSG_CNFMobKill)` → 32 ✓ no mismatch |
| Role | TMSrv→client confirmation that a mob died, carrying the killer/killed IDs plus the **per-recipient** current `Exp` and `Hold` so each nearby client can render its own exp/hold feedback and run level-up checks |
| Source TODO | Basedef.h:1511 `// TODO: Check, confirm, confirm structure.` — structure layout unconfirmed by the original author |

## 2. Wire Framing (protocol preamble)

Common framing applies identically to this packet (no per-packet deviation). Source: CPSock.cpp.

- Magic / init handshake: `INITCODE = 0x1F11F311` (CPSock.h:40, checked CPSock.cpp:249,373).
- `Size` in `[sizeof(HEADER), MAX_MESSAGE_SIZE]` where `HEADER = sizeof(MSG_STANDARD) = 12` and `MAX_MESSAGE_SIZE = 8192` (CPSock.h:38; check at CPSock.cpp:397). For this packet `Size` is fixed at **32**.
- Obfuscation: payload bytes from offset 4 are transformed per-byte with a keyed scheme:
  - `iKeyWord = rand()%256` (CPSock.cpp:535), `KeyWord = pKeyWord[iKeyWord*2]` (536), stored in header field `KeyWord` (539).
  - Byte transform depends on `mod = i & 0x3` (CPSock.cpp:566–578): `+ (Trans<<1)`, `- (Trans>>3)`, `+ (Trans<<2)`, `- (Trans>>5)` where `Trans = pKeyWord[(pos%256)*2 + 1]`, `pos` starts at `KeyWord`.
- Checksum: `CheckSum = Sum2 - Sum1` where `Sum1` = sum of raw payload bytes (offset 4..Size-1), `Sum2` = sum of transformed bytes (CPSock.cpp:554–584); stored in header field `CheckSum` (584).
- Server-side validation `BASE_CheckPacket` (Basedef.cpp:6475) is **entirely commented out** → **DISABLED**. Its body still lists `_MSG_CNFMobKill` at Basedef.cpp:6505 (`m->Type == _MSG_CNFMobKill && m->Size != sizeof(MSG_CNFMobKill)`), but it never executes.

Per-packet deviations: **none** — `MSG_CNFMobKill` uses the standard 12-byte header + generic XOR obfuscation + checksum described above. The packet is multicast, so the same buffer is re-obfuscated per recipient socket by `AddMessage`.

## 3. Binary Layout

`MSG_CNFMobKill` (Basedef.h:1512–1520) is **NOT** inside any `#pragma pack(1)` region. The pack(1) regions are only 808–835, 1212–1246, 1465–1492, 2063–2097 (Basedef.h). Offset 1511 is after the 1465–1492 pop and before the 2063 push, so the compiler default **`/Zp8`** applies (MSVC default packing = 8-byte members on 8-byte boundaries, others on their natural alignment).

Little-endian x86, LP32.

### 3.1 Header (`_MSG`, Basedef.h:925–930) — 12 bytes

| Field | Type | Offset | Size |
|---|---|---|---|
| Size | short | 0 | 2 |
| KeyWord | char | 2 | 1 |
| CheckSum | char | 3 | 1 |
| Type | short | 4 | 2 |
| ID | short | 6 | 2 |
| ClientTick | unsigned int | 8 | 4 |

Header total: **12 bytes** → payload starts at offset **12**.

### 3.2 Payload — 20 bytes (16 fields + 4 padding)

| Field | Type | Offset | Size | Notes |
|---|---|---|---|---|
| Hold | int | 12 | 4 | offset 12 is 4-aligned ✓ |
| KilledMob | unsigned short | 16 | 2 | |
| Killer | unsigned short | 18 | 2 | |
| *(padding)* | — | 20 | 4 | `long long` needs 8-align; 20 % 8 = 4 → pad to 24 |
| Exp | long long | 24 | 8 | offset 24 is 8-aligned ✓ |

Payload total: **20 bytes** (12 + 2 + 2 + 4 pad + 8 = 20).

Padding rows: **1** — 4 bytes at offset 20–23 inserted before `Exp` (8-byte alignment requirement of `long long` under `/Zp8`). This is the only reason the struct is 32 rather than 28 bytes.

### 3.3 Nested struct expansions

None. `MSG_CNFMobKill` contains only scalar members plus the `_MSG` macro header. No `STRUCT_*` nested members to expand.

### 3.4 Size verification

```
sizeof(MSG_CNFMobKill)
  = sizeof(_MSG)             12   ; offsets 0..11
  + sizeof(int)  Hold         4   ; offsets 12..15  (12 % 4 == 0)
  + sizeof(ushort) KilledMob  2   ; offsets 16..17
  + sizeof(ushort) Killer     2   ; offsets 18..19
  + padding                   4   ; offsets 20..23  (align Exp to 8)
  + sizeof(long long) Exp     8   ; offsets 24..31  (24 % 8 == 0)
  = 12 + 4 + 2 + 2 + 4 + 8
  = 32
```

Producer cross-check: `MobKilled` sets `sm.Size = sizeof(MSG_CNFMobKill)` (MobKilled.cpp:177) → matches the computed 32. The send loop uses `msg->Size` (SendFunc.cpp:968) which is this same 32. Disabled check table `BASE_CheckPacket` also uses `sizeof(MSG_CNFMobKill)` (Basedef.cpp:6505). **No mismatch.**

UNKNOWN members: none structurally — all four payload fields have a defined producer/sender assignment (see §6). However, the **semantics of `Exp`/`Hold` are only populated at send time**, per recipient (see §4/§6), so the values in the buffer built by `MobKilled` are initially zero.

## 4. Lifecycle & Flow

**Outbound (GAME2CLIENT) — legitimate path.** The packet is produced by `MobKilled(target, conn, PosX, PosY)` (MobKilled.cpp:41), the single central handler invoked when any mob/user dies.

```
[combat death / timer death]
   │  callers:
   │   _MSG_Attack.cpp:1743     MobKilled(TargetKilled[i], conn, 0, 0)
   │   Server.cpp:9203          MobKilled(Target, attacker, 0, 0)
   │   ProcessSecMinTimer.cpp:1940  MobKilled(Target, index, 0, 0)
   │   imple.cpp:1661,1718      MobKilled(i, conn, 0, 0)
   ▼
void MobKilled(target, conn, PosX, PosY)               MobKilled.cpp:41
   │  MSG_CNFMobKill sm; memset(0, sizeof)             :173-174
   │  sm.Type = _MSG_CNFMobKill                         :176
   │  sm.Size = sizeof(MSG_CNFMobKill) (=32)            :177
   │  sm.ID   = ESCENE_FIELD (=30000, Basedef.h:170)    :178
   │  sm.KilledMob = target                             :180
   │  sm.Killer    = conn                               :181
   │  NOTE: sm.Hold and sm.Exp are NOT set here (zero)  :173-181
   ▼
GridMulticast(TargetX, TargetY, (MSG_STANDARD*)&sm, 0)  MobKilled.cpp:196/634/2036/2043/2161
   │  (one of several send sites depending on the
   │   mob-vs-user / clan / zone branch)
   ▼
GridMulticast(tx,ty,msg,skip)                           SendFunc.cpp:843
   │  iterate grid cells pMobGrid[y][x] around (tx,ty)  :875-879
   │  for each recipient tmob != skip, Mode != MOB_EMPTY
   ▼
if (msg->Type == _MSG_CNFMobKill)                       SendFunc.cpp:920
   │  ((MSG_CNFMobKill*)msg)->Exp  = pMob[tmob].MOB.Exp      :922
   │  ((MSG_CNFMobKill*)msg)->Hold = pMob[tmob].extra.Hold   :923
   │  → per-recipient customization (see §6)
   │  level-up / quarter-bonus checks (Segment 1..4)   :925-965
   ▼
pUser[tmob].cSock.AddMessage((char*)msg, msg->Size)     SendFunc.cpp:968
   ▼
CPSock send path: XOR obfuscation + CheckSum per socket  CPSock.cpp:535-584 → client
```

**Inbound (CLIENT2GAME) — none.** Despite the `FLAG_CLIENT2GAME` bit set on the constant, there is **no** `case _MSG_CNFMobKill` anywhere in the codebase. A repo-wide grep for `_MSG_CNFMobKill` returns only:
- Basedef.h:1511 (definition),
- Basedef.cpp:6505 (disabled `BASE_CheckPacket` table),
- MobKilled.cpp:176 (producer),
- SendFunc.cpp:920 (multicast filter).

`ProcessClientMessage.cpp` has no case for it, so a client-sent packet of this type is **not dispatched and is silently ignored** (falls through the switch default). The `CLIENT2GAME` flag is spurious/unused. There is no dedicated cheat handler for this type either — it simply goes unhandled.

## 5. Validation & Guards

| # | Guard | Location | Behavior |
|---|---|---|---|
| 1 | ID range check | ProcessClientMessage.cpp:42 | `std->ID < 0 \|\| >= MAX_USER` → log + return (no dispatch). Applies to any inbound message; since there is no `_MSG_CNFMobKill` case, never relevant for this type. |
| 2 | ServerDown window | ProcessClientMessage.cpp:53 | `ServerDown >= 120` → return |
| 3 | Ping short-circuit | ProcessClientMessage.cpp:59 | `_MSG_Ping` → return |
| 4 | ClientTick spoof check | ProcessClientMessage.cpp:63 | non-server packet with `ClientTick == SKIPCHECKTICK` → return |
| 5 | Size validation (wire) | CPSock.cpp:397 | `Size > MAX_MESSAGE_SIZE \|\| Size < sizeof(HEADER)` → rejected |
| 6 | Checksum validation (wire) | CPSock.cpp:458 | `Sum != CheckSum` → rejected |
| 7 | Size equality (`BASE_CheckPacket`) | Basedef.cpp:6505 | **DISABLED** — entire body commented out (6476). Would flag `Size != sizeof(MSG_CNFMobKill)` (i.e. != 32) |

There is no runtime size check in the outbound producer; `Size` is set to `sizeof(MSG_CNFMobKill)` (MobKilled.cpp:177) and trusted downstream (`AddMessage` reuses `msg->Size`, SendFunc.cpp:968). Integrity on receive relies solely on the generic wire checks (CPSock.cpp:397,458) plus the absence of an inbound dispatch case.

## 6. Game Mechanics & Business Logic

`MSG_CNFMobKill` conveys **"a mob died"** to every client in the kill grid cell. Field semantics (verified from producer MobKilled.cpp:173–181 + send-time customization SendFunc.cpp:920–966):

- `KilledMob` (`unsigned short`) — the **target** mob/entity that died; set to `target` (MobKilled.cpp:180).
- `Killer` (`unsigned short`) — the killer's index; set to `conn` (MobKilled.cpp:181). For a player-killed-by-summon, `conn` is resolved to the summoner before the packet is finalized (MobKilled.cpp:190–205).
- `ID` — set to `ESCENE_FIELD` (30000), the "server sent message" marker (MobKilled.cpp:178; Basedef.h:170).
- `Hold` (`int`) — **per-recipient** current `extra.Hold` (exp stored on level-up). Not set in the producer; filled at multicast time as `((MSG_CNFMobKill*)msg)->Hold = pMob[tmob].extra.Hold` (SendFunc.cpp:923). So each nearby client receives its *own* hold value.
- `Exp` (`long long`) — **per-recipient** current total experience. Not set in the producer; filled at multicast time as `((MSG_CNFMobKill*)msg)->Exp = pMob[tmob].MOB.Exp` (SendFunc.cpp:922). Each nearby client receives its *own* current Exp.

Because `Exp`/`Hold` are **not** the exp gained from this kill, but the recipient's live Exp/Hold totals, the kill-exp bookkeeping itself happens elsewhere in `MobKilled` (not in this packet). The actual exp awarded is computed and applied per party member inside `MobKilled`'s `UNK_3`/party branch (MobKilled.cpp:214–343, 350–627): `GetExpApply` (214/255/350) → modifiers (party size, `ExpBonus`, RvR, Newbie, DOUBLEMODE, KefraLive, level penalties) → `pMob[party].MOB.Exp += exp` (335/616) and `Hold` deduction (318–333/600–614) → `MSG_UpdateExpRanking` sent to DB (340–341/621–622).

The send-time customization in `GridMulticast` then also drives **level-up feedback**: `Segment = pMob[tmob].CheckGetLevel()` (SendFunc.cpp:925); for `Segment 1..4` it emits the quarter-bonus / level-up messages (`_NN_Level_Up`, `_NN_3_Quarters_Bonus`, `_NN_2_Quarters_Bonus`, `_NN_1_Quarters_Bonus`, SendFunc.cpp:932–944), calls `SetCircletSubGod` / `DoItemLevel` on segment 4 (931/935), then `SendScore(tmob)`, `SendEmotion(tmob,14,3)`, `SendEtc(tmob)` (946–951), increments PK point +5 on segment 4 (953–954), rebuilds `MSG_CreateMob` (956–960) and logs `"lvl %s level up to %d"` (962–963).

The packet itself carries **no drop data** — drops are delivered separately as `MSG_CreateItem` (see §8).

## 7. Side Effects

- **Outbound:** the packet is multicast to every client in the kill grid cell (`GridMulticast`, MobKilled.cpp:196/634/2036/2043/2161), one re-obfuscated copy per socket via `AddMessage` (SendFunc.cpp:968).
- **Per-recipient mutation of the shared buffer:** while iterating, `GridMulticast` writes `Exp`/`Hold` onto the same `MSG_CNFMobKill` buffer for each recipient (SendFunc.cpp:922–923) — the last recipient's values remain in the buffer after the loop.
- **Level-up side effects (send-time, per recipient):** `SetCircletSubGod` (931), `DoItemLevel` (935), `SendClientMessage` quarter/level-up notices (932–944), `SendScore` (946), `SendEmotion` (947), `SendEtc` (951), `SetPKPoint(+5)` on segment 4 (953–954), `MSG_CreateMob` rebuild + `GridMulticast` (956–960), and log `"lvl %s level up to %d"` with `AccountName`/`IP` (962–963).
- **Kill-exp side effects (in `MobKilled`, separate from the packet):** per-party `pMob[party].MOB.Exp += exp` (335/616), `extra.Hold` deduction (318–333/600–614), `extra.DayLog.Exp` accumulation (311–315), and `MSG_UpdateExpRanking` → `DBServerSocket.SendOneMessage` (340–341/621–622). Drops via `CreateItem(...)` → `MSG_CreateItem` (MobKilled.cpp:2193/2327/2384). `DeleteMob` on non-persistent mobs (637/2030/2037/2044).
- No direct DB write from this packet (the ranking update is via the separate `MSG_UpdateExpRanking`).

## 8. Related Packets

- `MSG_RemoveMob` (`_MSG_RemoveMob`) — removal of the dead mob's corpse from client view; accompanies `DeleteMob` after `GridMulticast` of `MSG_CNFMobKill` (MobKilled.cpp:637–644).
- `MSG_CreateItem` (`_MSG_CreateItem`) — created by `CreateItem(...)` during drop processing (MobKilled.cpp:2193/2327/2384); carries the dropped loot item + position.
- `MSG_CreateMob` (`_MSG_CreateMob`) — rebuilt and multicast on a recipient's level-up inside the `MSG_CNFMobKill` filter (SendFunc.cpp:956–960).
- `MSG_GetItem` (`_MSG_GetItem`) — client pick-up of an item dropped after the kill.
- `MSG_UpdateExpRanking` — sent to DBSrv after kill-exp is applied (MobKilled.cpp:340–341/621–622).
- `MSG_UpdateScore` / `MSG_UpdateEtc` — stat/exp push issued to a recipient on level-up during the `MSG_CNFMobKill` filter (SendFunc.cpp:946; docs/packets/MSG_UpdateScore.md).

## 9. Discrepancies & Open Questions

1. **TODO comment (Basedef.h:1511):** `// TODO: Check, confirm, confirm structure.` — the original author was unsure of this structure. The 32-byte layout depends critically on `long long Exp`'s 8-byte alignment under `/Zp8`; if the intent was a compact 28-byte struct, a `pack(1)` region would be required. This is the main structural uncertainty.
2. **`CLIENT2GAME` flag with no inbound handler:** the constant sets `FLAG_CLIENT2GAME`, yet no `case _MSG_CNFMobKill` exists in `ProcessClientMessage.cpp` (or anywhere). A client-sent instance is silently ignored (falls through the switch). The flag appears to be vestigial — unlike `MSG_UpdateScore`, there isn't even a cheat-rejection handler.
3. **`Exp`/`Hold` semantics vs the field names:** the fields are named as if they carried the kill's gained exp, but the producer leaves them zero and `GridMulticast` overwrites them with the *recipient's total* `MOB.Exp` and `extra.Hold` (SendFunc.cpp:922–923). The per-recipient overwrite also leaves the shared buffer holding the last recipient's values after the loop — benign because the buffer is not reused as-is afterward, but fragile.
4. **`BASE_CheckPacket` disabled (Basedef.cpp:6475):** the size guard that would enforce `Size==32` is commented out; size integrity relies solely on generic wire checks (CPSock.cpp:397,458).
5. **Send-path fragmentation:** the packet can be multicast from five different sites in `MobKilled` (196/634/2036/2043/2161) depending on the mob-vs-user/clan/zone branch; not all branches are guaranteed to reach the `MSG_CNFMobKill` filter identically. Not independently verified that every branch's `sm` retains identical contents.

## 10. Source References

| File | Lines | Role |
|---|---|---|
| legacy/Code/Basedef.h | 1511 | `_MSG_CNFMobKill = (56\|GAME2CLIENT\|CLIENT2GAME) = 0x338` + TODO comment |
| legacy/Code/Basedef.h | 1512–1520 | `MSG_CNFMobKill` struct (Hold int, KilledMob ushort, Killer ushort, Exp long long) |
| legacy/Code/Basedef.h | 925–930 | `_MSG` header macro |
| legacy/Code/Basedef.h | 932–939 | FLAG_* constants (GAME2CLIENT 0x100, CLIENT2GAME 0x200) |
| legacy/Code/Basedef.h | 170 | `ESCENE_FIELD = 30000` |
| legacy/Code/Basedef.h | 808/835, 1212/1246, 1465/1492, 2063/2097 | pack(1) regions — 1511 is **outside** them (default /Zp8) |
| legacy/Code/Basedef.cpp | 6475–6505 | `BASE_CheckPacket` (disabled); `_MSG_CNFMobKill` size check at 6505 |
| legacy/Code/CPSock.h | 38,40 | `MAX_MESSAGE_SIZE`, `INITCODE` |
| legacy/Code/CPSock.cpp | 397,425–458 | wire size + checksum validation |
| legacy/Code/CPSock.cpp | 535–584 | send-side XOR obfuscation + CheckSum |
| legacy/Code/TMSrv/MobKilled.cpp | 41,173–181 | `MobKilled` producer; `sm.Size = sizeof(MSG_CNFMobKill)` (177) |
| legacy/Code/TMSrv/MobKilled.cpp | 196,634,2036,2043,2161 | `GridMulticast` send sites for `MSG_CNFMobKill` |
| legacy/Code/TMSrv/MobKilled.cpp | 214–343, 350–627 | kill-exp application (per party) + `MSG_UpdateExpRanking` |
| legacy/Code/TMSrv/MobKilled.cpp | 2193,2327,2384 | drop `CreateItem` → `MSG_CreateItem` |
| legacy/Code/TMSrv/SendFunc.cpp | 843–968 | `GridMulticast`; `_MSG_CNFMobKill` filter 920–966; `AddMessage` 968 |
| legacy/Code/TMSrv/SendFunc.cpp | 922–923 | per-recipient `Exp` / `Hold` assignment |
| legacy/Code/TMSrv/ProcessClientMessage.cpp | 38–66 | dispatch switch — **no** `_MSG_CNFMobKill` case (no inbound leg) |
| legacy/Code/TMSrv/_MSG_Attack.cpp | 1743 | `MobKilled` caller (combat) |
| legacy/Code/TMSrv/Server.cpp | 9203 | `MobKilled` caller |
| legacy/Code/TMSrv/ProcessSecMinTimer.cpp | 1940 | `MobKilled` caller (timer) |
| legacy/Code/TMSrv/imple.cpp | 1661,1718 | `MobKilled` callers |
