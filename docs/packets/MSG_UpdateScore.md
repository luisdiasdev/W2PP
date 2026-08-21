# MSG_UpdateScore

## 1. Summary

| Attribute | Value |
|---|---|
| Symbol | `_MSG_UpdateScore` |
| Type value | `(54 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME)` = `54 \| 0x100 \| 0x200` = **`0x336`** |
| Flags | `GAME2CLIENT (0x100)` + `CLIENT2GAME (0x200)` → bidirectional flag-wise |
| Wire struct | `MSG_UpdateScore` (Basedef.h:1467) |
| Packing | **`#pragma pack(push, 1)` region 1465–1492** (Basedef.h:1465, 1492) — no padding |
| `sizeof(MSG_UpdateScore)` | **152 bytes** (12 header + 140 payload) |
| Producer (outbound) | `SendScore(int conn)` — SendFunc.cpp:1136, sends at 1190 via `GridMulticast` |
| Inbound handler | `ProcessClientMessage.cpp:106` — treated as **cheat** (Log + `AddCrackError(conn, 2, 91)`) |
| Producer struct-size cross-check | `SendFunc.cpp:1149` `sm_vus.Size = sizeof(MSG_UpdateScore)` → 152 ✓ no mismatch |
| Role | Server→client push of a character's full stat block (stats, current HP/MP, affect list, resists, regen, magic, special) so nearby clients can render/overlay them |

## 2. Wire Framing (protocol preamble)

Common framing applies identically to this packet (no per-packet deviation). Source: CPSock.cpp.

- Magic / init handshake: `INITCODE = 0x1F11F311` (CPSock.h:40, checked CPSock.cpp:249,373).
- `Size` in `[sizeof(HEADER), MAX_MESSAGE_SIZE]` where `HEADER = sizeof(MSG_STANDARD) = 12` and `MAX_MESSAGE_SIZE = 8192` (CPSock.h:38; check at CPSock.cpp:397). For this packet `Size` is fixed at 152.
- Obfuscation: payload bytes from offset 4 are transformed per-byte with a keyed XOR-ish scheme:
  - `iKeyWord = rand()%256` (CPSock.cpp:535), `KeyWord = pKeyWord[iKeyWord*2]` (536), stored in header field `KeyWord` (539).
  - Byte transform depends on `mod = i & 0x3` (CPSock.cpp:566–578): `+ (Trans<<1)`, `- (Trans>>3)`, `+ (Trans<<2)`, `- (Trans>>5)` where `Trans = pKeyWord[(pos%256)*2 + 1]`, `pos` starts at `KeyWord`.
- Checksum: `CheckSum = Sum2 - Sum1` where `Sum1` = sum of raw payload bytes (offset 4..Size-1), `Sum2` = sum of transformed bytes (CPSock.cpp:554–584); stored in header field `CheckSum` (584).
- Server-side validation `BASE_CheckPacket` (Basedef.cpp:6475) is **entirely commented out** → **DISABLED**. Its body still lists `_MSG_UpdateScore` at Basedef.cpp:6503 (`m->Type == _MSG_UpdateScore && m->Size != sizeof(MSG_UpdateScore)`), but it never executes.

Per-packet deviations: **none** — `MSG_UpdateScore` uses the standard 12-byte header + generic XOR obfuscation + checksum described above.

## 3. Binary Layout

`MSG_UpdateScore` (Basedef.h:1467–1491) sits inside `#pragma pack(push, 1)` (Basedef.h:1465) through `#pragma pack(pop)` (Basedef.h:1492). **This packet IS in a pack(1) region**, so there is no member padding and every member offset equals the previous end (little-endian x86, LP32).

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

### 3.2 Payload — 140 bytes

| Field | Type | Offset | Size |
|---|---|---|---|
| Score | STRUCT_SCORE | 12 | 48 |
| Critical | unsigned char | 60 | 1 |
| SaveMana | unsigned char | 61 | 1 |
| Affect | unsigned short[MAX_AFFECT=32] | 62 | 64 |
| Guild | unsigned short | 126 | 2 |
| GuildLevel | unsigned short | 128 | 2 |
| Resist | char[4] | 130 | 4 |
| RegenHP | unsigned char | 134 | 1 |
| RegenMP | unsigned char | 135 | 1 |
| CurrHp | int | 136 | 4 |
| CurrMp | int | 140 | 4 |
| Magic | int | 144 | 4 |
| Special | unsigned char[4] | 148 | 4 |

Payload total: **140 bytes** (48 + 1 + 1 + 64 + 2 + 2 + 4 + 1 + 1 + 4 + 4 + 4 + 4 = 140).

Padding rows: **none** (pack(1)).

### 3.3 Nested struct expansions

`STRUCT_SCORE` (Basedef.h:414–436) — defined **outside** the pack(1) region (default alignment) but is naturally aligned, so its 48-byte layout has no internal padding and embeds identically inside the pack(1) `MSG_UpdateScore`:

| Field | Type | Offset | Size |
|---|---|---|---|
| Level | int | 0 | 4 |
| Ac | int | 4 | 4 |
| Damage | int | 8 | 4 |
| Merchant | unsigned char | 12 | 1 |
| AttackRun | unsigned char | 13 | 1 |
| Direction | unsigned char | 14 | 1 |
| ChaosRate | unsigned char | 15 | 1 |
| MaxHp | int | 16 | 4 |
| MaxMp | int | 20 | 4 |
| Hp | int | 24 | 4 |
| Mp | int | 28 | 4 |
| Str | short | 32 | 2 |
| Int | short | 34 | 2 |
| Dex | short | 36 | 2 |
| Con | short | 38 | 2 |
| Special | short[4] | 40 | 8 |

STRUCT_SCORE total: **48 bytes**.

### 3.4 Size verification

```
sizeof(MSG_UpdateScore)
  = sizeof(_MSG) 12
  + sizeof(STRUCT_SCORE) 48
  + 1 (Critical) + 1 (SaveMana)
  + MAX_AFFECT*2 (32*2 = 64)        ; MAX_AFFECT = 32 (Basedef.h:122)
  + 2 (Guild) + 2 (GuildLevel)
  + 4 (Resist)
  + 1 (RegenHP) + 1 (RegenMP)
  + 4 (CurrHp) + 4 (CurrMp)
  + 4 (Magic)
  + 4 (Special)
  = 12 + 48 + 2 + 64 + 4 + 4 + 2 + 8 + 4 + 4
  = 152
```

Producer cross-check: `SendScore` sets `sm_vus.Size = sizeof(MSG_UpdateScore)` (SendFunc.cpp:1149) → matches the computed 152. Disabled check table `BASE_CheckPacket` also uses `sizeof(MSG_UpdateScore)` (Basedef.cpp:6503). **No mismatch.**

UNKNOWN members: none — all fields have a defined producer assignment (see §6). `Merchant` (inside Score) and `Magic` are documented as such in source comments but assigned from `MOB.Magic`.

## 4. Lifecycle & Flow

**Outbound (GAME2CLIENT) — legitimate path:**

```
[trigger event]
   │  (level-up, use-item, equip change, combat damage, regen tick,
   │   login, quest, trading, etc. — see §6 callers)
   ▼
SendScore(int conn)  ── SendFunc.cpp:1136
   │  fill MSG_UpdateScore from pMob[conn] (CurrHp/CurrMp/Score/Critical/
   │  SaveMana/Guild/GuildLevel/Affect/Resist/Special/Magic)
   │  Size = sizeof(...)=152 ; ID = conn          :1149-1150
   ▼
GridMulticast(TargetX, TargetY, (MSG_STANDARD*)&sm_vus, 0)  :1190
   │  → routes to all clients in the grid cell
   ▼
SendAffect(conn)  :1192   (companion affect update)
   ▼
CPSock send path: XOR obfuscation + CheckSum (CPSock.cpp:535-584) → client
```

Callers of `SendScore` (SendFunc.cpp:1136; declared SendFunc.h:51): ~100 call sites across
- `MobKilled.cpp:60,101,2053` — on kill/XP.
- `_MSG_UseItem.cpp`, `_MSG_CombineItemEhre.cpp:171`, `_MSG_ApplyBonus.cpp`, `_MSG_Attack.cpp`, `_MSG_TradingItem.cpp:360`, `_MSG_MessageChat.cpp:43,56`, `_MSG_Quest.cpp`, `_MSG_Restart.cpp:30`.
- `ProcessSecMinTimer.cpp:613,634,787,...` — periodic HP/MP regen refresh.
- `Server.cpp:5424,5746,5770,...,7841,8184,8206,...` — stat/score recompute (`GetCurrentScore`), login, etc.
- `ProcessDBMessage.cpp:1076` — after DB-driven state changes.

**Inbound (CLIENT2GAME) — cheat path (ProcessClientMessage.cpp:106–110):**

```
client → server  _MSG_UpdateScore (Type 0x336)
   │
   ▼
ProcessClientMessage(int conn, char *pMsg, BOOL isServer)  :38
   ├─ generic pre-checks: ID range :42, ServerDown :53, Ping :59,
   │    ClientTick==SKIPCHECKTICK :63
   ▼
switch(std->Type)  :66
   └─ case _MSG_UpdateScore:  :106
         Log("cra client send update score", pUser[conn].AccountName, pUser[conn].IP);  :108
         AddCrackError(conn, 2, 91);  :109
         break;                        :110
```

The client→game leg is **never processed as data** — it is rejected as an anti-cheat violation (see §5).

## 5. Validation & Guards

| # | Guard | Location | Behavior |
|---|---|---|---|
| 1 | ID range check | ProcessClientMessage.cpp:42 | `std->ID < 0 \|\| >= MAX_USER` → log + return (no dispatch) |
| 2 | ServerDown window | ProcessClientMessage.cpp:53 | `ServerDown >= 120` → return |
| 3 | Ping short-circuit | ProcessClientMessage.cpp:59 | `_MSG_Ping` → return |
| 4 | ClientTick spoof check | ProcessClientMessage.cpp:63 | non-server packet with `ClientTick == SKIPCHECKTICK` → return |
| 5 | **Cheat rejection (inbound score)** | ProcessClientMessage.cpp:106–110 | `_MSG_UpdateScore` from a client → `Log("cra client send update score", ...)` + `AddCrackError(conn, 2, 91)` |
| 6 | Size validation (wire) | CPSock.cpp:397 | `Size > MAX_MESSAGE_SIZE \|\| Size < sizeof(HEADER)` → rejected |
| 7 | Checksum validation (wire) | CPSock.cpp:458 | `Sum != CheckSum` → rejected |
| 8 | Size equality (`BASE_CheckPacket`) | Basedef.cpp:6503 | **DISABLED** — entire body commented out (6476). Would flag `Size != sizeof(MSG_UpdateScore)` |

Anti-cheat detail: `AddCrackError(int conn, int val, int Type)` (Server.cpp:684) adds `val` to `pUser[conn].NumError`; logs except for types 3/8/15; if `NumError >= 2000000000` it sends a bad-network message, force-logs-out the character (`CharLogOut`) and returns TRUE (Server.cpp:692–704). Here `AddCrackError(conn, 2, 91)` adds 2 points with cheat-code 91 (type 2 → logged). Because `MSG_UpdateScore` is client-writable in the official protocol, a modified/forged client can attempt to push arbitrary stats; the server treats any inbound instance as an exploit attempt.

## 6. Game Mechanics & Business Logic

`MSG_UpdateScore` carries the full stat snapshot for a character so nearby observers can display/overlay it. Field semantics (verified from the producer, SendFunc.cpp:1136–1193):

- `Score` (`STRUCT_SCORE`) — copied wholesale from `pMob[conn].MOB.CurrentScore` (SendFunc.cpp:1152): Level, Ac (defense), Damage, Merchant, AttackRun (speed), Direction, ChaosRate, MaxHp, MaxMp, Hp, Mp, Str, Int, Dex, Con, Special[4].
- `CurrHp` / `CurrMp` — from `MOB.CurrentScore.Hp/Mp` (SendFunc.cpp:1145–1146) (redundant with `Score.Hp/Mp` but carried for the client overlay).
- `Critical` — `MOB.Critical` (1154).
- `SaveMana` — `MOB.SaveMana` (1155).
- `Guild` / `GuildLevel` — `MOB.Guild/GuildLevel` (1156–1157); `Guild` forced to 0 when `GuildDisable` (1173–1174) or inside the Brazil-event BrState zone (1176–1189).
- `Affect[32]` — packed status effects via `GetAffect` (GetFunc.cpp:1174): `out[i] = (Type << 8) + (value & 0xFF)` from `affect[i].Time`, capped at 2550000.
- `Resist[4]` — `MOB.Resist[0..3]` (1161–1164).
- `Special[4]` — hard-coded to `0xCC` for all four slots (1166–1169).
- `Magic` — `MOB.Magic` (1171).
- `RegenHP` / `RegenMP` — **not set** by `SendScore` (remain 0 from the `memset` at 1139). UNKNOWN producer intent; likely unused/legacy.

The producer does not validate or compute anything on the packet — it is a pure serialization of server-authoritative state. There is no client-driven business logic for this packet because the inbound leg is rejected as a cheat (§5). The companion `SendAffect(conn)` (SendFunc.cpp:1769) immediately follows to deliver the full affect list.

## 7. Side Effects

- **Outbound:** rebroadcasts the character's authoritative stats to every client in the grid cell (`GridMulticast`, SendFunc.cpp:1190). No server-side state mutation.
- **Outbound companion:** `SendAffect(conn)` (SendFunc.cpp:1192) pushes the affect/status list.
- **Inbound (cheat):** `AddCrackError(conn, 2, 91)` increments `pUser[conn].NumError` (Server.cpp:692). Cumulative threshold `>= 2000000000` triggers a bad-network message and forced logout (Server.cpp:694–704). `Log("cra client send update score", AccountName, IP)` records the attempt (ProcessClientMessage.cpp:108; via AddCrackError log at Server.cpp:689).
- No database writes, no item/exp/money changes, no file I/O.

## 8. Related Packets

- `MSG_UpdateEtc` (`_MSG_UpdateEtc`, Basedef.h:1494) — partner stat packet (Exp/Learn/ScoreBonus/SpecialBonus/SkillBonus/Magic/Coin); often sent right after `SendScore` (e.g. level-up path SendFunc.cpp:951).
- `MSG_UpdateCarry` (`_MSG_UpdateCarry`, Basedef.h:1455) — carry inventory + coin.
- `MSG_UpdateEquip` / `SendEquip` — equipment visuals, sent on equip change.
- `MSG_SetHpMp` (`SendSetHpMp`, ProcessSecMinTimer.cpp:616) — targeted HP/MP update used when only HP/MP changed.
- `MSG_CNFMobKill`, `MSG_CreateMob`, `MSG_RemoveMob` — entity lifecycle packets that accompany stat broadcasts.

## 9. Discrepancies & Open Questions

1. **Why is the client→game leg a cheat?** In the stock protocol the client both sends and receives `MSG_UpdateScore` (both flag bits set). The server here treats any inbound instance as a forging attempt: a hostile client could inject fabricated stats (HP/MP/str/etc.) or corrupt the shared buffer. Hence the unconditional Log + `AddCrackError(conn,2,91)` (ProcessClientMessage.cpp:106–110) with no data processing.
2. **pack(1) consistency with outbound build:** the struct is in pack(1) (1465–1492) and the producer builds it with the same struct and `sizeof(MSG_UpdateScore)` (1149), so the wire layout and the send buffer agree. No mismatch.
3. **Redundant CurrHp/CurrMp vs Score.Hp/Mp:** `CurrHp/CurrMp` duplicate `Score.Hp/Mp`; the producer sets both (1145–1146, 1152). Likely historical duplication retained for the client overlay. UNKNOWN whether the client distinguishes them.
4. **`RegenHP`/`RegenMP` never set:** producer leaves them 0 (memset 1139); UNKNOWN intended semantics — possibly vestigial regeneration fields.
5. **`Special[4]` hard-coded 0xCC** (1166–1169): the producer sends a constant, so clients receive no real special-point data here; `STRUCT_SCORE.Special` is separate and carries real values.
6. **`BASE_CheckPacket` disabled** (Basedef.cpp:6475): the size guard that would enforce `Size==152` is commented out; size integrity relies solely on the generic wire checks (CPSock.cpp:397,458).

## 10. Source References

| File | Lines | Role |
|---|---|---|
| legacy/Code/Basedef.h | 925–930 | `_MSG` header macro |
| legacy/Code/Basedef.h | 932–939 | FLAG_* constants |
| legacy/Code/Basedef.h | 122 | `MAX_AFFECT = 32` |
| legacy/Code/Basedef.h | 414–436 | `STRUCT_SCORE` |
| legacy/Code/Basedef.h | 1465–1492 | `#pragma pack(push,1)` region; `_MSG_UpdateScore` (1466); `MSG_UpdateScore` (1467–1491) |
| legacy/Code/Basedef.cpp | 6475–6503 | `BASE_CheckPacket` (disabled); `_MSG_UpdateScore` size check at 6503 |
| legacy/Code/CPSock.h | 38,40 | `MAX_MESSAGE_SIZE`, `INITCODE` |
| legacy/Code/CPSock.cpp | 397,425–458 | wire size + checksum validation |
| legacy/Code/CPSock.cpp | 535–584 | send-side XOR obfuscation + CheckSum |
| legacy/Code/TMSrv/SendFunc.cpp | 1136–1193 | `SendScore` producer |
| legacy/Code/TMSrv/SendFunc.cpp | 946 | level-up caller |
| legacy/Code/TMSrv/SendFunc.cpp | 1769 | `SendAffect` companion |
| legacy/Code/TMSrv/GetFunc.cpp | 1174–1193 | `GetAffect` affect packing |
| legacy/Code/TMSrv/ProcessClientMessage.cpp | 38–110 | dispatch + cheat case (106–110) |
| legacy/Code/TMSrv/Server.cpp | 684–707 | `AddCrackError` |
| legacy/Code/TMSrv/ProcessSecMinTimer.cpp | 605–634 | periodic regen senders |
| legacy/Code/TMSrv/Server.cpp | 5418–5429 | stat recompute → `SendScore` |
