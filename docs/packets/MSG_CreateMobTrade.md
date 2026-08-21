# MSG_CreateMobTrade

## 1. Summary

| Field | Value |
|---|---|
| Type constant | `_MSG_CreateMobTrade` = `(99 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME)` = `99 \| 0x0100 \| 0x0200` = **0x363** (Basedef.h:1525) |
| Direction | **GAME2CLIENT only** (TMSrv → client). Although the constant ORs in `FLAG_CLIENT2GAME`, there is **no inbound dispatcher** for it — confirmed absent from `ProcessClientMessage.cpp` and `ProcessDBMessage.cpp` (see §9). |
| Wire struct | `MSG_CreateMobTrade` (Basedef.h:1526–1551) — sizeof **252 bytes** |
| Producer | `GetCreateMobTrade(int mob, MSG_CreateMobTrade *sm)` — TMSrv/GetFunc.cpp:1072 |
| Broadcast | **Grid area multicast** via `GridMulticast` (SendFunc.cpp:843, send at :968) OR **direct** `AddMessage` to a single conn (SendFunc.cpp:316). See §4. |
| Purpose | TMSrv spawns / refreshes a **player who is currently in trade (merchant) mode** for other clients in view. Structurally identical to `MSG_CreateMob` except the trailing field: instead of `int Hold` it carries `char Desc[MAX_AUTOTRADETITLE]` — the auto-trade shop title. It is the "mob-create" used when the target player has `TradeMode == 1` (an open merchant shop). |
| Sibling | `_MSG_CreateMob` = `(100 \| flags)` = **0x364** / `MSG_CreateMob` (Basedef.h:1553–1578) — same layout up to `Tab[26]`, then `int Hold` (4 B) instead of `Desc[24]` (24 B). Difference = +20 B. |
| Disabled validation | `BASE_CheckPacket` — Basedef.cpp:6475 (whole body commented out; returns 0) |

> **Note (verified against source):** despite the packet-brief referring to this as "embedding STRUCT_MOB", **MSG_CreateMobTrade does NOT embed `STRUCT_MOB`** (Basedef.h:438). It is a flattened struct of explicit scalar fields plus one `STRUCT_SCORE Score` (and `unsigned short Equip[16]` visual codes — *not* `STRUCT_ITEM`). The full `STRUCT_MOB` is only the *source* the producer copies from; it is never a member of the wire struct. The nested struct that must be expanded is `STRUCT_SCORE` (Basedef.h:414), not `STRUCT_MOB`. See §9.

## 2. Wire Framing

Standard CPSock framing, no per-packet deviation (a plain 12-byte `_MSG` header).

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
- `INITCODE = 0x1F11F311` magic (CPSock.h:40); sent as the first 4 bytes of the stream on connect (CPSock.cpp:249–250), then the payload begins at offset 4.
- Payload from byte 4 is obfuscated **per-byte XOR** (add/subtract keyed by `pKeyWord`, CPSock.cpp:558–581) keyed by `KeyWord` (an index into the `pKeyWord` table); `KeyWord` itself is written into byte 2 of the header.
- `CheckSum = Sum2 - Sum1` (CPSock.cpp:583) where the sums are over the (keyed) message bytes.
- `Size` is validated to be within `[sizeof(MSG_STANDARD), MAX_MESSAGE_SIZE]` (CPSock.h:38 `MAX_MESSAGE_SIZE = 8192`).
- `BASE_CheckPacket` (which would enforce `Size == sizeof(MSG_CreateMobTrade)`) is **disabled** — the entire body is commented out (Basedef.cpp:6475). No size cross-check is applied at the server.

**Per-packet note:** outbound to the client, the struct is `memset`-zeroed by the caller, filled by the producer, and its `Size` set to `sizeof(MSG_CreateMobTrade)` = 252 (GetFunc.cpp:1105). There is no client→game inbound path, so no inbound re-framing to describe.

## 3. Binary Layout

Packing context: `MSG_CreateMobTrade` is declared at Basedef.h:1525, **outside** every `#pragma pack(push,1)` region (the enclosing regions are 1465–1492 and 2063–2097; 1525 falls between them). Therefore the struct and its nested `STRUCT_SCORE` compile under the **MSVC default /Zp8** packing (member alignment = `min(size, 8)`). Little-endian x86, LP32.

### 3.1 Header

`_MSG` macro expansion (12 bytes, alignment 4 due to `unsigned int ClientTick`):

| Offset | Size | Align | Field |
|---|---|---|---|
| 0 | 2 | 2 | `short Size` |
| 2 | 1 | 1 | `char KeyWord` |
| 3 | 1 | 1 | `char CheckSum` |
| 4 | 2 | 2 | `short Type` |
| 6 | 2 | 2 | `short ID` |
| 8 | 4 | 4 | `unsigned int ClientTick` |

Header block: **12 bytes** (`@0..11`).

### 3.2 Payload

Payload begins at offset 12 (already 4-aligned, so no header padding). Fields in declaration order:

| Offset | Size | Align | Field | Notes |
|---|---|---|---|---|
| 12 | 2 | 2 | `short PosX` | |
| 14 | 2 | 2 | `short PosY` | |
| 16 | 2 | 2 | `unsigned short MobID` | index into `pMob[]`; also player conn id |
| 18 | 16 | 1 | `char MobName[NAME_LENGTH=16]` | bytes 12–15 repurposed: `[12]`=chaos/PK, `[13]`=cur kill, `[14..15]`=total kill |
| 34 | 32 | 2 | `unsigned short Equip[MAX_EQUIP=16]` | visual item codes |
| 66 | 64 | 2 | `unsigned short Affect[MAX_AFFECT=32]` | packed affect codes |
| 130 | 2 | 2 | `unsigned short Guild` | |
| 132 | 1 | 1 | `char GuildMemberType` | |
| 133 | 3 | 1 | `char Unknow[3]` | alignment pad to STRUCT_SCORE |
| 136 | 48 | 4 | `STRUCT_SCORE Score` | (expanded in §3.3) |
| 184 | 2 | 2 | `unsigned short CreateType` | state bitflags (guild level) |
| 186 | 16 | 1 | `unsigned char AnctCode[16]` | ancient-code visuals |
| 202 | 26 | 1 | `char Tab[26]` | tab/title text block |
| 228 | 24 | 1 | `char Desc[MAX_AUTOTRADETITLE=24]` | auto-trade shop title |

**Math / alignment verification (Zp8):**
- After header (`@12`), every scalar is placed at a multiple of its size (shorts at even offsets, ints in `STRUCT_SCORE` at 4-multiples). `char` arrays pack contiguously with no intra-array padding.
- `Unknow[3]` ends at `@136`; 136 is divisible by 4 → `STRUCT_SCORE`'s `int Level` is correctly aligned with **no padding row**.
- `Desc[24]` runs `@228..251`; struct ends at **252**.
- Struct alignment = 4 (largest member alignment: `int`/`unsigned int`). 252 % 4 = 0 → `sizeof(MSG_CreateMobTrade) = 252`, no trailing padding.

Total struct size = **252 bytes**; header 12 + payload 240 = 252 ✓.

### 3.3 Nested struct expansions

**`STRUCT_SCORE`** (Basedef.h:414–436, Zp8, size **48**, alignment 4) — embedded at `@136`:

| Offset(+136) | Size | Field |
|---|---|---|
| 136 | 4 | `int Level` |
| 140 | 4 | `int Ac` |
| 144 | 4 | `int Damage` |
| 148 | 1 | `unsigned char Merchant` |
| 149 | 1 | `unsigned char AttackRun` |
| 150 | 1 | `unsigned char Direction` |
| 151 | 1 | `unsigned char ChaosRate` |
| 152 | 4 | `int MaxHp` |
| 156 | 4 | `int MaxMp` |
| 160 | 4 | `int Hp` |
| 164 | 4 | `int Mp` |
| 168 | 2 | `short Str` |
| 170 | 2 | `short Int` |
| 172 | 2 | `short Dex` |
| 174 | 2 | `short Con` |
| 176 | 8 | `short Special[4]` |

Score block = 48 bytes (`@136..183`). No further nested structs (`STRUCT_MOB`, `STRUCT_ITEM`, `STRUCT_AFFECT` are **not** members of this wire struct — see §9).

### 3.4 Size verification

- Producer sets `sm->Size = sizeof(MSG_CreateMobTrade)` at GetFunc.cpp:1105.
- `sizeof(MSG_CreateMobTrade)` used at SendFunc.cpp:302,316 and GetFunc.h:41 declaration, `_MSG_SendAutoTrade.cpp:112`, `_MSG_MessageWhisper.cpp:531` — all consistent with 252.
- `memcpy(&sm->Score, &pMob[mob].MOB.CurrentScore, sizeof(STRUCT_SCORE))` (GetFunc.cpp:1110) — confirms `Score` is exactly one `STRUCT_SCORE` (48 B), matching §3.3.
- `GetAffect(sm->Affect, pMob[mob].Affect)` fills `unsigned short Affect[32]` = 64 B (GetFunc.cpp:1169, 1174) — matches §3.2.
- Sibling `MSG_CreateMob` sizeof = 232 (same layout through `Tab[26]`, then `int Hold` `@228..231`). Difference = 252 − 232 = **20 bytes** (the 24-B `Desc` replacing the 4-B `Hold`).

Expected `Size` on the wire = **252**. No mismatch with `sizeof()` usage. Unknown/obscure members: `Unknow[3]` (never written, alignment filler) and `GuildMemberType` (declared but not assigned by the producer — remains 0 from `memset`).

## 4. Lifecycle & Flow

Outbound-only (TMSrv → client). Three producer call-sites, all funneling through `GetCreateMobTrade`:

**A. Direct single-recipient — `SendCreateMob(conn, otherconn, bSend)`** (SendFunc.cpp:288)
- Guards: `conn` in `(0, MAX_USER)`, `pUser[conn].Mode == USER_PLAY`, socket alive (SendFunc.cpp:290–297).
- Branch (SendFunc.cpp:304): if `otherconn` is **not** a player in trade mode (`otherconn <= 0 || otherconn >= MAX_USER || pUser[otherconn].TradeMode != 1`) → fall back to `GetCreateMob` + `MSG_CreateMob` (SendFunc.cpp:306–309).
- Otherwise (otherconn is a player with `TradeMode == 1`) → `GetCreateMobTrade(otherconn, &sm2)` and `AddMessage` to `pUser[conn]`, then `SendMessageA()` (SendFunc.cpp:314–317).
- Called from the on-screen-mob refresh loop `SendGridMob` (SendFunc.cpp:560, call at :604), the reverse-visibility path (:780, :792), and `_MSG_NoViewMob.cpp:42`.

**B. Grid area multicast on opening a shop — `_MSG_SendAutoTrade` handler** (_MSG_SendAutoTrade.cpp:105–119)
- When a player opens an auto-trade shop, `pUser[conn].TradeMode = 1` (:105), then `GetCreateMobTrade(conn, &sm_cmt)` (:115), `sm_cmt.Score.Con = 0` (:117), and `GridMulticast(targetx, targety, &sm_cmt, 0)` (:119).

**C. Grid area multicast on changing the tab text — `_MSG_MessageWhisper` handler** (_MSG_MessageWhisper.cpp:510–534)
- When the player types `/tab` (strcmp `m->MobName == "tab"`), after updating `pMob[conn].Tab` (:519), if `TradeMode == 0` it multicasts `MSG_CreateMob` (:523–526), **else** (`TradeMode != 0`) it multicasts `MSG_CreateMobTrade` (:530–533).

**Multicast path** — `GridMulticast(tx, ty, msg, skip)` (SendFunc.cpp:843): iterates the `VIEWGRID` cell rectangle around `(tx,ty)`; for each occupant `tmob != skip` it sends `AddMessage(msg, msg->Size)` to `pUser[tmob].cSock` (:968). Note: the special per-recipient `_MSG_CreateMob` rendering patch (SendFunc.cpp:893–918) is keyed **only** on `msg->Type == _MSG_CreateMob`, so it does **not** apply to `MSG_CreateMobTrade`.

**Send path** — `CPSock::AddMessage` (CPSock.cpp:513): sets `Size/KeyWord/CheckSum/ClientTick` (514–541), obfuscates payload `@4..Size-1` keyed by `pKeyWord` (558–581), computes `CheckSum = Sum2 - Sum1` (583), copies raw header `@0..3` (586), appends to the send buffer (588). `SendMessageA()` (CPSock.cpp:617) flushes over the socket.

```
                    [TMSrv]
        SendCreateMob(SendFunc:288)        _MSG_SendAutoTrade:119      _MSG_MessageWhisper:533
                 |   (otherconn is            |  TradeMode=1,           |  TradeMode!=0, after
                 |    player, TradeMode==1)    |  Score.Con=0            |  /tab title change
                 v                            v                         v
              GetCreateMobTrade(GetFunc.cpp:1072) <---------------------+
                 |  fill Type/Size/ID/ClientTick/MobName/Pos/Score/...
                 v
      +----------+----------+---------+
      | SendCreateMob direct  | GridMulticast(SendFunc:843)   |
      | AddMessage -> conn    | AddMessage -> each viewer      |
      +------------------------+------------------------------+
                 v
      CPSock::AddMessage (CPSock.cpp:513): obfuscate + checksum
                 v
      CPSock::SendMessageA (CPSock.cpp:617)  -->  [client]  (no inbound handler)
```

## 5. Validation & Guards

| # | Guard | Location | Behavior on failure |
|---|---|---|---|
| 1 | `mob >= MAX_USER` (non-player target) | GetFunc.cpp:1078–1082 | Logs `"err,getcreatemob request by non player"` and **returns 0 early** (struct left zeroed by caller `memset`). In practice never hit — callers only reach `GetCreateMobTrade` for a valid player in trade mode. |
| 2 | `conn <= 0 \|\| conn >= MAX_USER` | SendFunc.cpp:290 | `SendCreateMob` returns without sending. |
| 3 | `pUser[conn].Mode != USER_PLAY` | SendFunc.cpp:293 | Returns without sending (receiver not in play). |
| 4 | `!pUser[conn].cSock.Sock` | SendFunc.cpp:296 | Returns without sending (no socket). |
| 5 | `otherconn <= 0 \|\| otherconn >= MAX_USER \|\| TradeMode != 1` | SendFunc.cpp:304 | Falls back to `MSG_CreateMob` (normal spawn), NOT the trade variant. |
| 6 | `mob < MAX_USER` block | GetFunc.cpp:1084–1101 | Only for players: packs kill/chaos into `MobName` bytes 12–15. |
| 7 | `pMob[mob].GuildDisable == 1` | GetFunc.cpp:1114–1115 | `sm->Guild = 0` (hide guild). |
| 8 | `mob >= MAX_USER` AC override | GetFunc.cpp:1117–1118 | (dead code given guard #1) would force `Score.Ac = Clan != 4`. |
| 9 | Equip slot 14 mount-outfit (`2360–2389`) with `stEffect[0].sValue <= 0` | GetFunc.cpp:1138–1145 | Hides the outfit (`Equip[14] = 0`), sets `selfdead = 1`, continues. |
| 10 | Equip slot 14 mount-outfit sanc clamp | GetFunc.cpp:1147–1163 | Reads `stEffect[1].cEffect/10`, clamps to `[0,13]`, `<<12`, ORs into visual code. |
| 11 | Grid boundary clipping | SendFunc.cpp:845–868 | Multicast rectangle clipped to world bounds. |

`BASE_CheckPacket` (which would validate `Size == sizeof`) is **disabled** (Basedef.cpp:6475) — no server-side size/type cross-check.

## 6. Game Mechanics & Business Logic

`GetCreateMobTrade` (GetFunc.cpp:1072) builds a client view of a **player currently operating a merchant auto-trade shop**. Mechanics:

- **Trade-context creation**: the only gate is `pUser[target].TradeMode == 1` (SendFunc.cpp:304). Trade mode is set when the player successfully posts an auto-trade shop (`_MSG_SendAutoTrade.cpp:105`) and cleared on trade exit (`RemoveTrade`). This variant exists so nearby clients see the shop owner correctly rendered **with their shop title (`Desc`)** instead of a plain `MSG_CreateMob`.
- **Name-overloading** (players only, GetFunc.cpp:1084–1101): `MobName[13]` = current-kill count, `MobName[14..15]` = total kills (`tk%256`, `tk/256`), `MobName[12]` = PK/chaos point (`GetPKPoint`). These trailing `MobName` bytes are repurposed metadata (visible as nameplate data on the client).
- **Guild handling** (1112–1115): `Guild = pMob[].MOB.Guild`, zeroed when `GuildDisable == 1`.
- **CreateType semantics** (1120–1126): base `0`; `\|= 0x80` if `MOB.GuildLevel == 9` (guild leader); `\|= 0x40` if `MOB.GuildLevel != 0` (guild member). These are state bitflags for nameplate/color rendering.
- **Equipment** (1130–1164): `Equip[i] = BASE_VisualItemCode(item, i)` and `AnctCode[i] = BASE_VisualAnctCode(item)` for all `MAX_EQUIP` slots. Slot 14 (mount) gets special hide/sanctification packing (guards #9/#10), producing a `selfdead` return flag.
- **Affects** (1169, GetAffect:1174): `Affect[i] = (type << 8) | (value & 0xFF)` for `MAX_AFFECT`, with `value` clamped at 2550000.
- **Score** (1110): `Score = MOB.CurrentScore` (48-byte copy). `_MSG_SendAutoTrade.cpp:117` additionally forces `Score.Con = 0` in the shop-open broadcast.
- **Trade title** (1167): `Desc` = `pUser[mob].AutoTrade.Title` truncated to `MAX_AUTOTRADETITLE-1` (23). **This is the defining field** that distinguishes `MSG_CreateMobTrade` from `MSG_CreateMob`.
- **Tab text** (1166): `Tab` = `pMob[mob].Tab` (26 B), refreshed live via the `/tab` command (re-multicast, §4C).
- **Position** (1103–1104): `PosX/PosY = TargetX/TargetY`.

**Differences vs `MSG_CreateMob` (GetFunc.cpp:947):**
1. Trailing field: `Desc[24]` (auto-trade title) replaces `Hold` (carrying weight) — +20 B. `Hold` is meaningless for a merchant; `Desc` is needed to show the shop.
2. Producer returns `selfdead` (mount-outfit-hidden flag) which `MSG_CreateMob`'s equivalent path also computes, but `GetCreateMobTrade` early-returns 0 for `mob >= MAX_USER`, i.e. it is **player-only**, whereas `GetCreateMob` handles arbitrary mobs/NPCs.
3. Both pack kill/chaos into `MobName`, but `GetCreateMobTrade` only does so under `mob < MAX_USER` (and that block is the only path — the early return guarantees players only).

## 7. Side Effects

- **Network**: sends a 252-byte packet to the client's send buffer (AddMessage, CPSock.cpp:513); flushes on `SendMessageA()` (CPSock.cpp:617) when `bSend`/multicast triggers it.
- **Multicast fan-out**: `GridMulticast` delivers to every player whose grid cell is within the clipped `VIEWGRID` rectangle around the target, excluding `skip` (SendFunc.cpp:875–971).
- **No server-state mutation**: `GetCreateMobTrade` is a pure read/packer — it does **not** modify `pMob`/`pUser`/DB state. The only state changes that *trigger* it happen in callers (setting `TradeMode = 1`, updating `pMob[].Tab`).
- **Return value**: `selfdead` (1 when the mount outfit was hidden) is returned but **unused** by all three call-sites (SendFunc.cpp:314, _MSG_SendAutoTrade.cpp:115, _MSG_MessageWhisper.cpp:532).
- **Client-visible**: recipient sees the player rendered with guild color/leader state (`CreateType`), kill/chaos nameplate, equipment, affects, HP from `Score`, and the shop title (`Desc`) — i.e. "this player has a merchant shop open here".

## 8. Related Packets

| Packet | Type (hex) | Relationship |
|---|---|---|
| `MSG_CreateMob` | 0x364 (Basedef.h:1553) | Direct sibling; same layout except `Desc[24]` vs `Hold` (+20 B). Normal spawn; selected when target is **not** in trade mode (SendFunc.cpp:304–311). |
| `MSG_RemoveMob` | 0x365 (Basedef.h:1582) | Removes the spawned entity from client view (death/logout). Complement of create. |
| `MSG_SendAutoTrade` | 0x397 (Basedef.h:1908) | The shop-creation packet that sets `TradeMode = 1` and carries `Title`, which becomes `Desc` here (_MSG_SendAutoTrade.cpp:100–107). |
| `MSG_QuitTrade` | 0x384 (Basedef.h:2059) | Ends trade mode (`RemoveTrade` clears `TradeMode`), so subsequent views fall back to `MSG_CreateMob`. |
| `MSG_PKInfo` | 0x166 (Basedef.h:1590) | Sent alongside the `/tab`/trade refresh (SendAutoTrade/_MSG_MessageWhisper.cpp:536) to update PK-state nameplate data that overlaps `MobName` bytes. |

## 9. Discrepancies & Open Questions

1. **No STRUCT_MOB embedding**: the brief asserted the struct "embeds STRUCT_MOB"; it does not. It embeds `STRUCT_SCORE` (48 B). `STRUCT_MOB` (Basedef.h:438) is only the producer's source. Corrected above.
2. **No inbound leg**: `FLAG_CLIENT2GAME` is OR'd in, but there is **no `case _MSG_CreateMobTrade`** in `ProcessClientMessage.cpp` or `ProcessDBMessage.cpp` (grep: no match). Outbound-only in practice.
3. **`GuildMemberType`** (Basedef.h:1538) is never assigned by the producer — always 0 from the caller's `memset`. Unknown client-side meaning here.
4. **`Unknow[3]`** (Basedef.h:1540) is never written — pure alignment filler to reach 4-alignment for `STRUCT_SCORE` at `@136`.
5. **Dead code**: guard #1 (`mob >= MAX_USER`) early-returns before the AC override (#8) could ever run for the trade variant, so the `Score.Ac = Clan != 4` line is unreachable.
6. **`selfdead` unused**: all three call-sites discard the return value.
7. **`Score.Con = 0` only in the shop-open broadcast** (_MSG_SendAutoTrade.cpp:117), not in the `SendCreateMob`/`/tab` paths — an inconsistency in how the trade player's constitution is reported depending on trigger.
8. **War-zone anonymization** (`BrState`) described in docs/reports for `GetCreateMob` is **not** present in the `GetCreateMobTrade` producer — the trade variant does not anonymize names (matches report; worth confirming whether that's intended).
9. **`MAX_AUTOTRADETITLE-1` truncation** (GetFunc.cpp:1167) leaves `Desc[23]` as the only guaranteed terminator; field is 24 B but 23 max meaningful.

## 10. Source References

| Location | What |
|---|---|
| Basedef.h:1525 | `_MSG_CreateMobTrade = (99 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME)` = 0x363 |
| Basedef.h:1526–1551 | `struct MSG_CreateMobTrade` |
| Basedef.h:1553–1578 | `struct MSG_CreateMob` (sibling) |
| Basedef.h:925–930 | `_MSG` header macro |
| Basedef.h:414–436 | `STRUCT_SCORE` (nested) |
| Basedef.h:438–481 | `STRUCT_MOB` (source only, not embedded) |
| Basedef.h:75,81,122,132 | `MAX_EQUIP=16`, `MAX_AUTOTRADETITLE=24`, `MAX_AFFECT=32`, `NAME_LENGTH=16` |
| Basedef.h:808/835, 1212/1246, 1465/1492, 2063/2097 | `#pragma pack(push,1)` regions (struct is outside all → /Zp8) |
| Basedef.cpp:6475 | `BASE_CheckPacket` — disabled |
| TMSrv/GetFunc.cpp:1072 | `GetCreateMobTrade` producer |
| TMSrv/GetFunc.cpp:1105 | `sm->Size = sizeof(MSG_CreateMobTrade)` |
| TMSrv/GetFunc.cpp:1174 | `GetAffect` |
| TMSrv/SendFunc.cpp:288 | `SendCreateMob` (direct path, trade branch :304–317) |
| TMSrv/SendFunc.cpp:843 | `GridMulticast` |
| TMSrv/SendFunc.cpp:968 | multicast `AddMessage(msg, msg->Size)` |
| TMSrv/_MSG_SendAutoTrade.cpp:105–119 | shop-open → TradeMode=1 + trade multicast |
| TMSrv/_MSG_MessageWhisper.cpp:510–534 | `/tab` → trade multicast when TradeMode != 0 |
| TMSrv/ProcessClientMessage.cpp | **no** `_MSG_CreateMobTrade` case (outbound only) |
| CPSock.h:38,40 | `MAX_MESSAGE_SIZE=8192`, `INITCODE=0x1F11F311` |
| CPSock.cpp:249–250 | INITCODE handshake |
| CPSock.cpp:513–591 | `AddMessage` — obfuscation + checksum |
| CPSock.cpp:617 | `SendMessageA` |
| docs/reports/component-deep-analyzer/component-analysis-GetFunc-*.md | BR-25/26/27/28 context (verified vs source) |
