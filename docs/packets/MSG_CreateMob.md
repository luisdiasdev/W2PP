# MSG_CreateMob

## 1. Summary

| Field | Value |
|---|---|
| Type constant | `_MSG_CreateMob` = `(100 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME)` = `100 \| 0x0100 \| 0x0200` = **0x364** (Basedef.h:1553) |
| Direction | **GAME2CLIENT only** (TMSrv → client). Although the constant ORs in `FLAG_CLIENT2GAME`, there is **no inbound dispatcher** for it — see §9. |
| Wire struct | `MSG_CreateMob` (Basedef.h:1554–1581) — sizeof **232 bytes** |
| Producer | `GetCreateMob(int mob, MSG_CreateMob *sm)` — TMSrv/GetFunc.cpp:947 |
| Broadcast | `GridMulticast` area broadcast — TMSrv/SendFunc.cpp:843 (per-recipient filter on `_MSG_CreateMob` at :893); also direct `AddMessage` to a single conn (SendFunc.cpp:308) |
| Purpose | TMSrv spawns / creates a mob **or player** in the world for a client. Carries position, name, visual equipment, affects, guild info, the base score (needed to render HP/nameplate), a `CreateType` state flag, and `Hold` (carrying weight). Used for the character's own appearance to *other* players on login (ProcessDBMessage.cpp:990), for mob spawns, and for dynamic appearance/level-up refresh. |
| Same-shape sibling | `_MSG_CreateMobTrade` = `(99 \| flags)` = **0x363** / `MSG_CreateMobTrade` (Basedef.h:1525–1551) — nearly identical layout, differing only in the trailing field (`Tab[26]`+`Desc[24]` instead of `Tab[26]`+`Hold`) |
| Disabled validation | `BASE_CheckPacket` — Basedef.cpp:6475 (whole body commented out; returns 0) |

> **Note (verified against source):** despite the packet-brief referring to this as "a large struct embedding STRUCT_MOB", **MSG_CreateMob does NOT embed `STRUCT_MOB`**. It is a flattened struct of explicit scalar fields plus one `STRUCT_SCORE Score` (and `unsigned short Equip[16]` visual codes — *not* `STRUCT_ITEM`). The full `STRUCT_MOB` (Basedef.h:438) is the *source* from which the producer copies fields, never a member of the wire struct. See §9.

## 2. Wire Framing

Standard CPSock framing, no per-packet deviation (a plain 12-byte `_MSG` header, CPSock.h:42–50).

**Common header `_MSG`** (Basedef.h:925–930, 12 bytes, LP32 little-endian, MSVC default /Zp8):

| Offset | Size | Type | Field | Semantics |
|---|---|---|---|---|
| 0 | 2 | short | Size | Total message length incl. header |
| 2 | 1 | char | KeyWord | Index into `pKeyWord` table |
| 3 | 1 | char | CheckSum | `Sum2 - Sum1` |
| 4 | 2 | short | Type | Message type constant |
| 6 | 2 | short | ID | Sender/recipient id |
| 8 | 4 | unsigned int | ClientTick | Tick/timestamp |

**Preamble / de-framing** (CPSock.cpp):
- `INITCODE = 0x1F11F311` magic; first 4 bytes of the stream verify the connection, then the payload begins at offset 4.
- Payload from byte 4 is obfuscated **per-byte XOR** keyed by `KeyWord` (an index into the `pKeyWord` table); `KeyWord` itself is written into byte 2 of the header.
- `CheckSum = Sum2 - Sum1` where the sums are over the (keyed) message bytes.
- `Size` is validated to be within `[sizeof(MSG_STANDARD), MAX_MESSAGE_SIZE]`.
- `BASE_CheckPacket` (which would enforce `Size == sizeof(MSG_CreateMob)`) is **disabled** — the entire body is commented out (Basedef.cpp:6475). No size cross-check is applied at the server.

**Per-packet note:** outbound to the client, the struct is filled and its `Size` set to `sizeof(MSG_CreateMob)` = 232 by the producer (GetFunc.cpp:980). There is no client→game inbound path, so no inbound re-framing to describe.

## 3. Binary Layout

Packing context: `MSG_CreateMob` (Basedef.h:1554) and its nested `STRUCT_SCORE` (Basedef.h:414) and `STRUCT_ITEM` (Basedef.h:398) all fall **outside** every `#pragma pack(push, 1)` region (808–835, 1212–1246, 1465–1492, 2063–2097). Therefore all use **MSVC default /Zp8** (max member alignment = 4 here, since the largest members are `int`/`unsigned int`/`STRUCT_SCORE`). All field offsets below are the natural alignment (4 for `int`, 2 for `short`, 1 for `char`/`unsigned char`).

**Verification via compiler** (LP64 test harness, identical field list):
`sizeof(STRUCT_SCORE)=48`, `sizeof(MSG_CreateMob)=232`; offsets `MobName=18 Equip=34 Affect=66 Guild=130 GuildMemberType=132 Unknow=133 Score=136 CreateType=184 AnctCode=186 Tab=202 Hold=228`. These match the hand computation below exactly.

### 3.1 Header (`_MSG`, offsets 0–11)

| Offset | Size | Type | Field | Semantics |
|---|---|---|---|---|
| 0 | 2 | short | Size | 232 |
| 2 | 1 | char | KeyWord | obfuscation key index |
| 3 | 1 | char | CheckSum | Sum2 - Sum1 |
| 4 | 2 | short | Type | 0x364 (`_MSG_CreateMob`) |
| 6 | 2 | short | ID | `ESCENE_FIELD` (30000) — set by producer (GetFunc.cpp:980) |
| 8 | 4 | unsigned int | ClientTick | `CurrentTime` (GetFunc.cpp:981) |

Header size: 12 bytes (offsets 0–11).

### 3.2 Payload (offsets 12–231)

| Offset | Size | Type | Field | Semantics |
|---|---|---|---|---|
| 12 | 2 | short | PosX | `pMob[mob].TargetX` (GetFunc.cpp:977) |
| 14 | 2 | short | PosY | `pMob[mob].TargetY` (GetFunc.cpp:978) |
| 16 | 2 | unsigned short | MobID | `mob` slot index (GetFunc.cpp:979) |
| 18 | 16 | char[16] | MobName | `NAME_LENGTH`=16; player name / mob name (GetFunc.cpp:953) |
| 34 | 32 | unsigned short[16] | Equip | `MAX_EQUIP`=16 visual item codes (GetFunc.cpp:1018–1041) |
| 66 | 64 | unsigned short[32] | Affect | `MAX_AFFECT`=32 (GetFunc.cpp:1045) |
| 130 | 2 | unsigned short | Guild | guild id (GetFunc.cpp:988) |
| 132 | 1 | char | GuildMemberType | `GuildLevel` (GetFunc.cpp:989) |
| 133 | 3 | char[3] | Unknow | UNKNOWN payload (reserved/padding slot) |
| 136 | 48 | STRUCT_SCORE | Score | `pMob[mob].MOB.CurrentScore` (GetFunc.cpp:983); see 3.3 |
| 184 | 2 | unsigned short | CreateType | state flag, see §6 (GetFunc.cpp:998–1011) |
| 186 | 16 | unsigned char[16] | AnctCode | ancient codes per equip slot (GetFunc.cpp:1019,1021) |
| 202 | 26 | char[26] | Tab | `pMob[mob].Tab` copied verbatim (GetFunc.cpp:1013) |
| 228 | 4 | int | Hold | `pMob[mob].extra.Hold` carrying weight (GetFunc.cpp:1012) |

Payload size: 232 − 12 = 220 bytes. Total `sizeof(MSG_CreateMob)` = **232**.

**Padding rows:** none required — the natural alignment of every field falls on its type boundary without gaps (last int `Hold` at 228 is 4-aligned). All 232 bytes are field-occupied; the struct is fully packed under /Zp8 with no hidden holes.

### 3.3 Nested struct expansions

**STRUCT_SCORE** (Basedef.h:414–437, /Zp8, sizeof 48) — embedded as `Score` @136:

| Offset (rel) | Offset (abs) | Size | Type | Field |
|---|---|---|---|---|
| 0 | 136 | 4 | int | Level |
| 4 | 140 | 4 | int | Ac |
| 8 | 144 | 4 | int | Damage |
| 12 | 148 | 1 | unsigned char | Merchant |
| 13 | 149 | 1 | unsigned char | AttackRun |
| 14 | 150 | 1 | unsigned char | Direction |
| 15 | 151 | 1 | unsigned char | ChaosRate |
| 16 | 152 | 4 | int | MaxHp |
| 20 | 156 | 4 | int | MaxMp |
| 24 | 160 | 4 | int | Hp |
| 28 | 164 | 4 | int | Mp |
| 32 | 168 | 2 | short | Str |
| 34 | 170 | 2 | short | Int |
| 36 | 172 | 2 | short | Dex |
| 38 | 174 | 2 | short | Con |
| 40 | 176 | 8 | short[4] | Special[4] |

**STRUCT_ITEM** (Basedef.h:398–412, /Zp8, sizeof 12) — **NOT** present as an array member of MSG_CreateMob. It is the *source* type from which the producer derives the `Equip[i]` visual codes via `BASE_VisualItemCode` (GetFunc.cpp:1018). Included for completeness of the STRUCT_MOB expansion in the codebase, but not part of this packet's wire layout.

```
STRUCT_ITEM { short sIndex; @0
              union { short sValue; / { uchar cEffect; uchar cValue; } } stEffect[3]; @2..7 }
```
sizeof STRUCT_ITEM = 2 + (3 × 2) = 8 bytes.

**STRUCT_MOB** (Basedef.h:438–481, /Zp8) — the *source* object, **not a wire member**. Full byte layout (for reference, since it is the data the producer reads):

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 16 | char[16] | MobName |
| 16 | 1 | char | Clan |
| 17 | 1 | unsigned char | Merchant |
| 18 | 2 | unsigned short | Guild |
| 20 | 1 | unsigned char | Class |
| 21 | 2 | unsigned short | Rsv |
| 23 | 1 | unsigned char | Quest |
| 24 | 4 | int | Coin |
| 28 | 8 | long long | Exp |
| 36 | 2 | short | SPX |
| 38 | 2 | short | SPY |
| 40 | 48 | STRUCT_SCORE | BaseScore |
| 88 | 48 | STRUCT_SCORE | CurrentScore |
| 136 | 128 | STRUCT_ITEM[16] | Equip (16 × 8) |
| 264 | 512 | STRUCT_ITEM[64] | Carry (64 × 8) |
| 776 | 4 | long | LearnedSkill |
| 780 | 4 | unsigned int | Magic |
| 784 | 2 | unsigned short | ScoreBonus |
| 786 | 2 | unsigned short | SpecialBonus |
| 788 | 2 | unsigned short | SkillBonus |
| 790 | 1 | unsigned char | Critical |
| 791 | 1 | unsigned char | SaveMana |
| 792 | 4 | unsigned char[4] | SkillBar[4] |
| 796 | 1 | unsigned char | GuildLevel |
| 797 | 2 | unsigned short | RegenHP |
| 799 | 2 | unsigned short | RegenMP |
| 801 | 4 | char[4] | Resist[4] |

sizeof STRUCT_MOB = **805**. (Alignment: max member 8 (long long Exp @28) → struct rounded to multiple of 8 = 808 in some compilers; see §9.)

### 3.4 Size verification

- Producer sets `sm->Size = sizeof(MSG_CreateMob)` (GetFunc.cpp:980) and `memset`s the struct first (GetFunc.cpp:949).
- GridMulticast sends `sizeof(MSG_CreateMob)` bytes via `AddMessage` (SendFunc.cpp:308) and broadcasts `sizeof(MSG_CreateMob)` (SendFunc.cpp:345).
- `BASE_CheckPacket`'s size check for `_MSG_CreateMob` (`if (m->Type == _MSG_CreateMob && m->Size != sizeof(MSG_CreateMob)) code = 1;`, Basedef.cpp:6495) is present but **dead** (function body disabled).
- **Expected Size = 232.** No mismatch between the struct's computed size and the `sizeof()` usages; no truncation.

**UNKNOWN members:** `char Unknow[3]` (offset 133) — reserved, never written by the producer (left zero after `memset`). Semantics undocumented.

## 4. Lifecycle & Flow

**Outbound (GAME2CLIENT)** — the only leg. There is no client→game inbound case (confirmed: no `case _MSG_CreateMob` exists in ProcessClientMessage.cpp).

```
                 ┌──────────────────────────── TMSrv ────────────────────────────┐
 Trigger events  │                                                               │
  (mob spawn /   │  GetCreateMob(mob, &sm)            MSG_CreateMob sm           │
   player enter, │  GetFunc.cpp:947                   (232 bytes)                │
   level-up,     │      │  fills: MobName,PosX/Y,MobID,                          │
   appearance)   │      │  Score,Equip,AnctCode,Affect,                          │
                 │      │  Guild,GuildMemberType,CreateType,                     │
                 │      │  Tab,Hold; Type=0x364, Size=232,                       │
                 │      │  ID=ESCENE_FIELD, ClientTick=CurrentTime               │
                 │      ▼                                                       │
                 │  GridMulticast(tx,ty,(MSG_STANDARD*)&sm,skip)                 │
                 │  SendFunc.cpp:843                                             │
                 │      │  for each mob in 33×33 view grid                      │
                 │      ▼  per-recipient filter @893 (_MSG_CreateMob):           │
                 │  AddMessage((char*)&sm, sizeof(MSG_CreateMob))                │
                 │  SendFunc.cpp:308 (direct) / :345 (broadcast)                 │
                 └──────────────┬──────────────────────────────────────────┘
                                ▼  CPSock framing (obfuscate + XOR)
                           Client renders mob/player
```

**Path A — character login (player visible to others).** In `ProcessDBMessage()` handling `_MSG_DBCNFCharacterLogin`, after sending `MSG_CNFClientCharacterLogin` to the player itself (ProcessDBMessage.cpp:984), the server builds `MSG_CreateMob sm2`, calls `GetCreateMob(conn, &sm2)` (ProcessDBMessage.cpp:992), forces `sm2.CreateType = 2` (ProcessDBMessage.cpp:994), registers the player into the grid `pMobGrid[sm.PosY][sm.PosX] = conn` (ProcessDBMessage.cpp:996), then `GridMulticast(sm.PosX, sm.PosY, (MSG_STANDARD*)&sm2, 0)` (ProcessDBMessage.cpp:997) so nearby players see the character appear. Followed by `SendPKInfo` and `SendGridMob` (ProcessDBMessage.cpp:999–1000).

**Path B — mob spawn / appearance refresh.** `SendFunc.cpp:299–345` fills a `MSG_CreateMob`/`MSG_CreateMobTrade` pair, sends the trade one directly, and broadcasts the mob one via `GridMulticast(pMob[conn].TargetX, pMob[conn].TargetY, (MSG_STANDARD*)&sm, conn)` (SendFunc.cpp:345) — skipping the source conn. Also used for level-up appearance: `GetCreateMob(tmob, &sm_lupc)` then `GridMulticast(...)` (SendFunc.cpp:956–960).

**GridMulticast filter (SendFunc.cpp:893–917):** when the message type is `_MSG_CreateMob` and the target recipient is a player (`tmob < MAX_USER`), the sender applies a hardcoded **PVP-area appearance override**: if `PosX/Y` fall in the box `x∈[896,1150]`, `y∈[1405,1538]`, it rewrites `Equip[1]`/`AnctCode[1]` (hcitem.sIndex 3505) and `Equip[15]`/`AnctCode[15]` (hcitem.sIndex 3999) — likely a battle-arena forced appearance (SendFunc.cpp:895–914).

**Mob-side CreateType tweak:** for non-player mobs (`mob >= MAX_USER`, 1000), `Score.Ac` is forced to 0 if `Clan==4` else 1 (GetFunc.cpp:993–997), and `Guild`/`GuildMemberType` are cleared if `GuildDisable==1` (GetFunc.cpp:990–992).

## 5. Validation & Guards

| # | Guard / check | Location | Effect |
|---|---|---|---|
| 1 | `BASE_CheckPacket` size enforcement | Basedef.cpp:6475–6495 | **Disabled** (whole body commented out) — no inbound size validation, and none needed (outbound only) |
| 2 | `memset(sm, 0, sizeof(MSG_CreateMob))` | GetFunc.cpp:949 | zero-init; any field not explicitly set is 0 |
| 3 | `mob < MAX_USER` branch | GetFunc.cpp:956 | only players get kill-counter / PK-point bytes packed into `MobName[12..15]` |
| 4 | `gv = GetGuilty(mob); if (gv>0) chaos=0` | GetFunc.cpp:969–970 | PK name-color suppressed for guilty players |
| 5 | `pMob[mob].GuildDisable == 1` | GetFunc.cpp:990 | guild shown as 0/0 |
| 6 | `mob >= MAX_USER` | GetFunc.cpp:992 | non-player: `Score.Ac` forced 0/1 by `Clan` |
| 7 | equip slot 14 sword durability / anct tier | GetFunc.cpp:1024–1040 | hides broken 2360–2389 weapons (`selfdead=1`); encodes upgrade tier into high bits |
| 8 | `BrState != 0` + pos box (2604–2648, 1708–1744) | GetFunc.cpp:1046–1051 | replaces `MobName` with "??????", blanks `Equip[15]`/`Guild` |
| 9 | grid bounds clamp | SendFunc.cpp:846–866 | `GridMulticast` clamps the 33×33 window to `MAX_GRIDX/Y` and origin |
| 10 | `pMob[tmob].Mode == MOB_EMPTY` (0) skip | SendFunc.cpp:878 / CMob.h:26 | empty grid slots not sent to |
| 11 | `tmob <= 0 || tmob == skip` | SendFunc.cpp:873 | skips invalid / the source conn itself |

**CreateType semantics** (unsigned short, GetFunc.cpp:998–1011):
- base `CreateType = 0`
- `| 0x80` if `GuildLevel == 9` (guild leader) → otherwise
- `| 0x40` if `GuildLevel >= 6` (guild member/rank)
- overridden to **2** on character login (ProcessDBMessage.cpp:994)

## 6. Game Mechanics & Business Logic

- **Positioning:** `PosX/PosY` are the mob's grid target coordinates (`pMob[mob].TargetX/TargetY`, GetFunc.cpp:977–978). The client uses these to place the sprite in the world and to run the per-recipient override box check (§4).
- **Identity:** `MobID` = the server slot index (`mob`, GetFunc.cpp:979), which the client uses as the persistent handle for this mob. `ID` in the header = `ESCENE_FIELD` (30000), i.e. the server as sender.
- **Appearance:** the client renders the mob from `Equip[16]` visual item codes + `AnctCode[16]` (GetFunc.cpp:1018–1021, both derived from `STRUCT_ITEM` via `BASE_VisualItemCode`/`BASE_VisualAnctCode`), the `MobName` (with kill-count/PK bytes embedded for players), and `Score` (level, HP/MP, stats — required for the nameplate, HP bar and combat stats).
- **Player-specific name encoding:** `MobName[12]`=PK points, `[13]`=current-kill count, `[14]`=total-kill low byte, `[15]`=total-kill high byte (GetFunc.cpp:957–975).
- **Guild display:** `Guild` + `GuildMemberType` (from `MOB.GuildLevel`) render the guild tag; `CreateType` conveys leadership (0x80 leader / 0x40 member).
- **Combat stat overrides for NPCs:** for `mob >= MAX_USER`, `Score.Ac` is set 0 (Clan 4) or 1 (else) rather than the real AC (GetFunc.cpp:992–997) — NPCs expose a simplified armor value.
- **Carrying capacity:** `Hold` = `pMob[mob].extra.Hold` (GetFunc.cpp:1012) so the client shows the mob's load/weight; `Tab` copied verbatim (GetFunc.cpp:1013).
- **Self-death visual:** when a player's equipped slot-14 weapon is broken (2360–2389 with `sValue <= 0`), it is rendered as empty and `selfdead=1` is returned (GetFunc.cpp:1024–1029) — a "broken weapon" / dead-state signal to the caller.

## 7. Side Effects

- **Area broadcast:** `GridMulticast` fans the packet out to every player whose mob resides within the 33×33 view grid centered on the spawn point, applying the per-recipient `_MSG_CreateMob` appearance-override filter (SendFunc.cpp:843–917).
- **Direct send:** `AddMessage` to a single connection (SendFunc.cpp:308) — e.g. the trade-shape sibling sent to one player.
- **Grid registration:** on login the player is written into `pMobGrid[PosY][PosX] = conn` before broadcast (ProcessDBMessage.cpp:996), making the character addressable by other players' grid scans.
- **Appearance refresh on level-up / equip change:** a fresh `GetCreateMob` + `GridMulticast` (SendFunc.cpp:956–960) re-broadcasts the mob's updated appearance to the area.
- **Producer caller state:** `selfdead` return signals a broken-equipped weapon to the caller (GetFunc.cpp:1029).
- **Per-recipient mutation:** the broadcast mutates `Equip[1]`, `Equip[15]`, `AnctCode[1]`, `AnctCode[15]` on the in-flight struct inside the arena box (SendFunc.cpp:895–914) — a shared-buffer write that leaks across recipients in the same broadcast (see §9).

## 8. Related Packets

| Packet | Type | Relation |
|---|---|---|
| `MSG_CreateMobTrade` | `_MSG_CreateMobTrade` 0x363 | Same-shape sibling; same 12-byte header + PosX/PosY..Score prefix; differs in trailing `Desc[24]` and no `Hold` (Basedef.h:1525–1551). Sent immediately alongside MSG_CreateMob in the same producer (SendFunc.cpp:301–316) |
| `MSG_RemoveMob` | `_MSG_RemoveMob` 0x165 | Complementary teardown; `RemoveType` 1=morte, 2=logout (Basedef.h:1583–1588; SendFunc.cpp:494). Sent when a mob/player leaves the area or dies |
| `MSG_CNFMobKill` | `_MSG_CNFMobKill` | Kill-confirmation packet; in the same GridMulticast filter block, `Exp`/`Hold` are patched per recipient (SendFunc.cpp:920) |
| `MSG_CNFClientCharacterLogin` | `_MSG_CharacterLogin` 0x114 | The packet sent to the player themselves on login (ProcessDBMessage.cpp:984); MSG_CreateMob is the *complement* broadcast to others |
| `MSG_PKInfo` | `_MSG_PKInfo` 0x166 | PK info sent right after login broadcast (ProcessDBMessage.cpp:999) |
| `SendGridMob` | — | Sends all mobs in view to a player (ProcessDBMessage.cpp:1000) |

## 9. Discrepancies & Open Questions

1. **No STRUCT_MOB embedding (contradicts brief).** `MSG_CreateMob` is a flattened struct; `STRUCT_MOB` is only the producer's source. The brief's "large struct embedding STRUCT_MOB" is inaccurate — the actual struct is 232 bytes of explicit fields (verified against Basedef.h:1554–1581 and the compiled `sizeof`).
2. **FLAG_CLIENT2GAME is nominally set but unused.** `_MSG_CreateMob` ORs in `FLAG_CLIENT2GAME`, yet there is **no inbound `case _MSG_CreateMob` in ProcessClientMessage.cpp** — outbound-only in practice, as expected for a server→client spawn. The flag appears to be convention for the "base" packet family (all these flags are ORed by habit), not evidence of a client→game leg.
3. **Dead size validation.** `BASE_CheckPacket`'s `_MSG_CreateMob` size check (Basedef.cpp:6495) can never fire — the function body is fully commented out.
4. **Broadcast buffer mutation leak.** The arena-override filter writes `Equip[1]/[15]/AnctCode[1]/[15]` directly into the shared `msg` buffer per recipient (SendFunc.cpp:895–914). After the first recipient inside the arena box is processed, the *next* recipient also in the box gets the same override (fine), but a recipient outside the box *following* an in-box one would receive the mutated values — the override is never reverted per iteration. Impact depends on GridMulticast's intended semantics; open question whether the box check is meant to gate the sender once.
5. **`Unknow[3]` (offset 133)** — reserved, zero-filled, undocumented semantics.
6. **`sizeof(STRUCT_MOB)` = 805** but rounded to 808 under strict /Zp8 (max-align long long = 8). Not directly on the wire; noted only because the brief asked to expand it fully.
7. **`Score.Merchant / AttackRun / Direction / ChaosRate`** are copied wholesale from `CurrentScore` (GetFunc.cpp:983) — only `Ac` is overridden for NPCs; the rest of the packet does not interpret these bytes, so the client is assumed to consume `Score` as-is.

## 10. Source References

| Symbol / line | Location |
|---|---|
| `_MSG_CreateMob` (0x364) | Basedef.h:1553 |
| `struct MSG_CreateMob` | Basedef.h:1554–1581 |
| `_MSG_CreateMobTrade` (0x363) / `MSG_CreateMobTrade` | Basedef.h:1525–1551 |
| `_MSG_RemoveMob` / `MSG_RemoveMob` | Basedef.h:1583–1588 |
| `_MSG` header macro | Basedef.h:925–930 |
| Flags (GAME2CLIENT / CLIENT2GAME) | Basedef.h:933–934 |
| `struct STRUCT_SCORE` | Basedef.h:414–437 |
| `struct STRUCT_ITEM` | Basedef.h:398–412 |
| `struct STRUCT_MOB` | Basedef.h:438–481 |
| Constants (NAME_LENGTH, MAX_EQUIP, MAX_AFFECT, MAX_CARRY) | Basedef.h:75,76,122,132 |
| Grid constants (VIEWGRID, HALFGRID, MAX_GRID) | Basedef.h:95–101 |
| `ESCENE_FIELD` (30000) | Basedef.h:170 |
| `MAX_USER` (1000) | Basedef.h:56 |
| `GetCreateMob()` producer | TMSrv/GetFunc.cpp:947–1070 |
| `GetCreateMobTrade()` | TMSrv/GetFunc.cpp:1072–1105 |
| `GridMulticast()` broadcast | TMSrv/SendFunc.cpp:843–972 |
| `_MSG_CreateMob` per-recipient filter (arena override) | TMSrv/SendFunc.cpp:893–917 |
| Direct `AddMessage` of MSG_CreateMob | TMSrv/SendFunc.cpp:299–316, 345 |
| Level-up appearance re-broadcast | TMSrv/SendFunc.cpp:956–960 |
| `SendRemoveMob` | TMSrv/SendFunc.cpp:494–504 |
| CreateMob on character login | TMSrv/ProcessDBMessage.cpp:988–1000 |
| `BASE_CheckPacket` (disabled) | Basedef.cpp:6475–6495 |
| `MOB_EMPTY` (0) | TMSrv/CMob.h:26 |
| `BASE_VisualItemCode` / `BASE_VisualAnctCode` (referenced) | Basedef.cpp (visual-code helpers) |
