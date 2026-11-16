# Component Deep Analysis Report

**Component:** SendFunc Outbound Projection Layer
**Primary location:** `Code/TMSrv/SendFunc.cpp` (39,988 bytes, 1,817 lines), `Code/TMSrv/SendFunc.h` (73 lines)
**Project root:** `E:\wyd\test-setup\w2pp`
**Project:** W2PP — decompiled C/C++ (Win32, MSVC) MMORPG game server emulator for W2
**Analysis scope:** entire project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis date:** 2026-11-15 22:32:55
**Analyst role:** component-level static analysis (read-only; no files were modified)

---

## 1. Executive Summary

`SendFunc.cpp` is TMSrv's **outbound projection layer**: a 50-function, C-style facade that converts internal world state (`pMob[]`, `pUser[]`, `pItem[]`, `pMobGrid[][]`, `pItemGrid[][]`) into fixed-layout `MSG_*` wire structures and hands them to the transport (`CPSock::AddMessage` / `SendMessageA` / `SendOneMessage`). It is the only place in the server where per-recipient byte layouts are assembled for 26 concrete `MSG_*` types plus 5 generic signal envelopes (`MSG_STANDARD`, `MSG_STANDARDPARM`, `MSG_STANDARDPARM2`, `MSG_STANDARDPARM3`, `MSG_STANDARDSHORTPARM2`).

**Role in the system.** The architectural report classifies this component as an *Outbound Adapter / Serializer* with **afferent coupling ~59** and **efferent coupling 5**. That is confirmed statically: the unit includes exactly five internal headers (`Basedef.h`, `SendFunc.h`, `GetFunc.h`, `Server.h`, `Language.h`) plus the CRT `<memory>` header, while 47 of the 50 exported symbols are referenced from **45 distinct translation units**, with `SendClientMessage` alone having ~637 external call sites and `SendItem` ~381.

**Key findings.**

1. **The layer is not a pure serializer.** Three functions mutate authoritative world state: `GridMulticast(conn, tx, ty, msg)` relocates the entity between grid cells and rewrites `LastX/LastY/TargetX/TargetY` (SendFunc.cpp:643, 657, 815-822), `GridMulticast(tx, ty, msg, skip)` advances player level/segment state via `CMob::CheckGetLevel()` and applies PK points and item-level rewards for every recipient (SendFunc.cpp:920-965), and `SendWarInfo` normalizes `pMob[].MOB.Guild`, `g_pGuildWar[]` and `g_pGuildAlly[]` as a side effect of sending (SendFunc.cpp:1421-1428).
2. **Fan-out is an unbounded O(N) in-process loop.** There is no queue, no rate limit, and no back-pressure other than "close the socket". Each broadcast walks `pUser[0..999]`, `pMobGrid[4096][4096]` windows (1,089 cells per pass) or `g_pGuildWar[0..65535]` directly on the game thread.
3. **Buffer-safety is delegated and inconsistent.** `CPSock::AddMessage` is the only bounds check (`nSendPosition + Size >= 128 KB`); the return value is inspected by only 12 of ~40 call sites, and on failure the policy varies between "silently drop", "log and truncate name fields", and "`CloseUser`". Two concrete over-reads exist inside this component: `SendAutoTrade` submits `sizeof(MSG_UpdateCarry)` (528 bytes) for a 196-byte `MSG_SendAutoTrade` stack object (SendFunc.cpp:556), and `SendClientMessage`/`SendChat`/`SendSay`/`SendClientMessageOk` copy a fixed 96 bytes out of caller-supplied strings of unknown length (SendFunc.cpp:39, 189, 343, 1656).
4. **Two visibility predicates are formally wrong.** `GridMulticast(conn, ...)` line 716/737 mixes `||` and `&&` so the north/south half of the visibility test is enabled only when `tx != 0`; `SendMessageArea` line 1683 compares `TargetY` against the `x2` bound, so the area filter never evaluates correctly.
5. **Two exported functions are dead** (`PartyGridMulticast`, `SendMessageArea`), one is declared-but-never-called (`SendClientSignalParm2`), and one has a computed-but-unused discriminator (`FoundCharged` in `SendNoticeChief`).
6. **No tests exist anywhere in the repository** for this component or for the project as a whole (evidence in Section 11).

---

## 2. Data Flow Analysis

### 2.1 Canonical end-to-end flow (movement / state change)

```
 1. Client frame received            CPSock::Receive -> ReadMessage        Code/CPSock.cpp:336
 2. Routed to handler                ProcessClientMessage                  Code/TMSrv/ProcessClientMessage.cpp:38
 3. Handler validates + mutates      Exec_MSG_Action                       Code/TMSrv/_MSG_Action.cpp:21
    state (position, HP, exp, ...)   (coordinate range gate at line 192)
 4. Handler calls fan-out entry      GridMulticast(conn, tx, ty, msg)      SendFunc.cpp:620
 5. Grid ownership repaired          GetEmptyMobGrid / pMobGrid writes    SendFunc.cpp:629-657
 6. Old viewport scanned             (33x33 around previous TargetX/Y)    SendFunc.cpp:704-743
 7. New viewport scanned             (33x33 around tx/ty)                 SendFunc.cpp:745-811
 8. Per-recipient payload rewrite    MSG_CNFMobKill / MSG_CreateMob       SendFunc.cpp:893-966
 9. Envelope handoff                 CPSock::AddMessage                    Code/CPSock.cpp:513
10. Header stamping + obfuscation    Size/KeyWord/CheckSum/ClientTick       Code/CPSock.cpp:535-586
11. Optional immediate flush         CPSock::SendMessageA -> send()         Code/CPSock.cpp:617
12. Entity state committed           LastX/LastY/TargetX/TargetY            SendFunc.cpp:815-822
```

### 2.2 Three fan-out topologies implemented in this component

```
A. Slot-space fan-out (O(MAX_USER)=1000, no geometry)
   SendNotice            loop i in [0,MAX_USER)   -> USER_PLAY            SendFunc.cpp:57
   SendNoticeChief       loop i + guild scan      -> GuildLevel == 9      SendFunc.cpp:72
   SendNoticeArea        loop i + rect predicate  -> inclusive bounds     SendFunc.cpp:157
   SendGuildNotice       loop i + guild equality  -> same guild           SendFunc.cpp:169
   SyncMulticast         loop i, i != conn        -> everyone else        SendFunc.cpp:262
   SyncKingdomMulticast  loop i, clan + KingChat  -> kingdom chat         SendFunc.cpp:276
   MapaMulticast         loop i from 1            -> TargetX/128 == tx    SendFunc.cpp:1664
   SendMessageArea       loop i + (broken) rect   -> unused               SendFunc.cpp:1678
   SendWeather           loop i + socket probe    -> everyone             SendFunc.cpp:1573

B. Grid-space fan-out (O(1089) cells per pass, 3 passes)
   GridMulticast(conn,...)  old window -> diff -> new window                SendFunc.cpp:704,728,745
   GridMulticast(tx,ty,..)  33x33 window around (tx,ty)                     SendFunc.cpp:875
   PartyGridMulticast       33x33 window + party list (dormant)             SendFunc.cpp:998
   SendGridMob              33x33 window, populate viewer's client          SendFunc.cpp:591
   SendSignalParmArea       raw x/y cell walk (no window, caller-supplied)  SendFunc.cpp:1695
   SendShortSignalParm2Area raw x/y cell walk                               SendFunc.cpp:1717

C. Directed fan-out (O(1) recipients)
   SendClientMessage / SendClientMessageOk / SendClientSignal(Parm..)       SendFunc.cpp:27,179,197-258
   SendCreateMob / SendCreateItem / SendRemoveMob / SendRemoveItem          SendFunc.cpp:288,320,494,510
   SendItem / SendEquip / SendScore / SendEtc / SendCargoCoin               SendFunc.cpp:1055-1257
   SendGuildList / SendShopList / SendWarInfo / SendReqParty / SendAddParty SendFunc.cpp:1259-1506
   SendRemoveParty / SendCarry / SendSetHpMp / SendHpMode / SendPKInfo      SendFunc.cpp:1508-1767
   SendAffect / SendAutoTrade                                               SendFunc.cpp:527,1769
```

### 2.3 Message construction pipeline (per payload)

```
1. Stack-allocate the concrete MSG_* struct
2. memset(&sm, 0, sizeof(MSG_*))            <- every builder without exception
3. Set envelope: Type = _MSG_* constant (already OR-ed with FLAG_GAME2CLIENT 0x0100)
4. Set Size  = sizeof(MSG_*)                <- pre-set for reference; AddMessage overwrites it
5. Set ID    = player slot | ESCENE_FIELD(30000)
6. Copy domain fields from pMob[conn].MOB / pUser[conn] / pItem[n]
7. Optionally rewrite per recipient (GridMulticast only)
8. Encode visuals via BASE_VisualItemCode / BASE_VisualAnctCode
9. CPSock::AddMessage(buf, size) -> encrypts payload, stamps header, appends to 128 KB buffer
10. Optional SendMessageA() -> single send() attempt, then buffer reset on full drain
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation | Recipient slot must satisfy `0 < conn < MAX_USER` (slot 0 is a reserved invalid sentinel and never receives) | SendFunc.cpp:29, 181, 290, 529, 1057, 1197, 1235, 1371, 1440, 1474, 1510, 1536, 1592, 1624, 1739, 1771 |
| BR-02 | Validation | Recipient must be in `USER_PLAY` (22) state | SendFunc.cpp:59, 159, 264, 293, 538, 600(implicit), 1060, 1200, 1238, 1275, 1374, 1443, 1477, 1513, 1539, 1575, 1595, 1627, 1666, 1680, 1707, 1730 |
| BR-03 | Validation | Recipient must hold a live socket (`pUser[i].cSock.Sock != 0`) | SendFunc.cpp:296, 1018, 1046, 1063, 1203, 1241, 1377, 1446, 1480, 1516, 1542, 1578, 1598, 1630 |
| BR-04 | Encoding | Panel messages are fixed-width: exactly `MESSAGE_LENGTH` (96) bytes copied into a 128-byte field, last two bytes forced to NUL | SendFunc.cpp:39-42 |
| BR-05 | Business Logic | A notice whose text starts with the two-character marker `'x` is logged but never broadcast | SendFunc.cpp:54-55 |
| BR-06 | Business Logic | Chief-tier notices target only players at `MOB.GuildLevel == 9` with a positive guild id in `ChargedGuildList` | SendFunc.cpp:77-99 |
| BR-07 | Business Logic | Chief summon teleports guild leaders to a coordinate derived from their charged server/zone slot | SendFunc.cpp:140-151 |
| BR-08 | Business Logic | Area notices use inclusive rectangle containment on `TargetX/TargetY` | SendFunc.cpp:162 |
| BR-09 | Business Logic | Guild notices are scoped by exact `MOB.Guild` equality | SendFunc.cpp:173 |
| BR-10 | Contract | All generic signals share one envelope: `Type`, `ID`, optional `Parm1..3`; no slot validation is performed | SendFunc.cpp:197-258 |
| BR-11 | Fan-out | Exclusion multicast sends to every playing user except the origin slot | SendFunc.cpp:264 |
| BR-12 | Fan-out | Kingdom multicast additionally requires `MOB.Clan == Kingdom` and `KingChat == 0` | SendFunc.cpp:278 |
| BR-13 | Business Logic | Mob appearance uses `MSG_CreateMob`, unless the subject is in trade mode 1, in which case `MSG_CreateMobTrade` is emitted | SendFunc.cpp:304-317 |
| BR-14 | Invariant | One entity per grid cell; the grid is self-healing and repairs lost/duplicated occupancy during broadcast | SendFunc.cpp:629-657, 765-771, 786-789 |
| BR-15 | Business Logic | Viewport is 33x33 cells (±16) clamped to `[0, 4096)`; the clamped window is re-anchored at map borders | SendFunc.cpp:659-702, 845-873 |
| BR-16 | Fan-out | Visibility diff: leavers get `MSG_RemoveMob`/`MSG_DecayItem`, newcomers get `MSG_CreateMob`/`MSG_CreateItem` + `MSG_PKInfo`, the caller's message goes to both sets exactly once | SendFunc.cpp:704-811 |
| BR-17 | Validation | Ground items are projectable only when `0 < item < MAX_ITEM`, `Mode != 0` and `ITEM.sIndex != 0` | SendFunc.cpp:739, 754, 611 |
| BR-18 | Encoding | `MSG_CNFMobKill.Exp/Hold` and `MSG_CreateMob.Equip[1]/AnctCode[1]/Equip[15]/AnctCode[15]` are rewritten per recipient before each enqueue | SendFunc.cpp:893-923 |
| BR-19 | Business Logic | Level-segment cascade: `CheckGetLevel()` returns 1..4 and triggers bonus messages, `SendScore`, `SendEmotion`, `SendEtc`, `+5 PK`, and a cosmetic re-broadcast | SendFunc.cpp:920-965 |
| BR-20 | Fan-out | Party multicast excludes the leader, the skip slot, `PartyChat`-muted users and members whose leader differs | SendFunc.cpp:1004-1051 |
| BR-21 | Encoding | Equipment visuals are encoded per slot; slot 14 items in `[2360,2390)` follow mount rules and may be hidden or level-augmented | SendFunc.cpp:1095-1128 |
| BR-22 | Business Logic | Guild identity is concealed when `GuildDisable` is set or when `BrState` is active inside the arena rectangle `x∈[2604,2648]`, `y∈[1708,1744]` | SendFunc.cpp:1173-1189 |
| BR-23 | Encoding | `SendEtc`/`SendCargoCoin`/`SendCarry`/`SendSetHpMp`/`SendHpMode` project non-combat character facets | SendFunc.cpp:1195-1645 |
| BR-24 | Business Logic | Guild roster is concatenated into ≤70-character chunks, flushed as separate panel messages, and followed by war/ally enumeration over `[1, 65535]` | SendFunc.cpp:1266-1366 |
| BR-25 | Encoding | Shop list flattens 27 carry slots in a 9-column layout and attaches the city tax of the merchant's village | SendFunc.cpp:1390-1410 |
| BR-26 | Business Logic | `SendWarInfo` normalizes guild ids and resolves mutual war before emitting `_MSG_SendWarInfo(Parm1=war, Parm2=Clan, Parm3=ally)` | SendFunc.cpp:1416-1436 |
| BR-27 | Encoding | Party handshake compresses HP/MP above 32000 by dividing by 100 and encodes `Target = 52428` (0xCCCC) | SendFunc.cpp:1490-1502 |
| BR-28 | Fan-out | Weather is broadcast to all playing users with a live socket; failure closes the user | SendFunc.cpp:1573-1587 |
| BR-29 | Fan-out | Map-scope multicast matches `TargetX/128 == tx && TargetY/128 == ty` and starts at slot 1 | SendFunc.cpp:1664-1666 |
| BR-30 | Fan-out | Area message filter intends `x1 <= X <= x2 && y1 <= Y <= y2` but the implemented predicate is defective | SendFunc.cpp:1683 |
| BR-31 | Fan-out | Grid-cell area signals walk the raw coordinate rectangle (not the 33x33 window) and only target players | SendFunc.cpp:1695-1733 |
| BR-32 | Business Logic | PK flag is 1 if the subject is guilty, in PK mode, or in RvR/Castle/G-Torre state; `NewbieEventServer` forces 1 | SendFunc.cpp:1752-1764 |
| BR-33 | Business Logic | Affect type 34 with `Time >= 32000000` is an absolute divine expiry; remaining time is decoded, and ≤3600 s collapses to 450 | SendFunc.cpp:1784-1814 |
| BR-34 | Resilience | Queue overflow (`AddMessage` FALSE) and socket failure are handled per call site: drop, log, or `CloseUser` | SendFunc.cpp:556, 799-806, 1049, 1229, 1255, 1412, 1568, 1584, 1618, 1643 |
| BR-35 | Encoding | Auto-trade projection must serialize `sizeof(MSG_SendAutoTrade)` but submits `sizeof(MSG_UpdateCarry)` | SendFunc.cpp:556 |

---

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 Recipient slot range gate

**Overview:**
Every directed send must be addressed to a slot index in the half-open range `(0, MAX_USER)`. Index `0` is treated as "no recipient" and is silently discarded; `MAX_USER` is 1000 (`Basedef.h:56`) and marks the boundary between player slots (`pMob[0..999]`) and NPC/mob ids (`pMob[1000..24999]`).

**Detailed description:**
The range check is the first executable statement of the majority of the directed senders, and it protects the `pUser[conn]` lookup that immediately follows. Because `pUser` is a fixed global array of `CUser` (`Server.h:64`) and mobs occupy the same `pMob` array above index 999, an index such as `1500` would read a valid `CMob` but an out-of-bounds `CUser`. The guard therefore encodes a structural invariant of the process, not merely a defensive taste: *slots are players, ids above 999 are not addressable as clients*.

The rule is applied inconsistently. `SendClientMessage`, `SendClientMessageOk`, `SendCreateMob`, `SendAutoTrade`, `SendItem`, `SendEtc`, `SendCargoCoin`, `SendShopList`, `SendReqParty`, `SendAddParty`, `SendRemoveParty`, `SendCarry`, `SendSetHpMp`, `SendHpMode`, `SendPKInfo` and `SendAffect` all enforce it. `SendClientSignal`, `SendClientSignalParm`, `SendClientSignalParm2`, `SendClientSignalParm3`, `SendClientSignalShortParm2`, `SendCreateItem`, `SendRemoveMob`, `SendRemoveItem`, `SendGuildList`, `SendWarInfo`, `SendEquip`, `SendScore`, `SendChat`, `SendSay`, `SendEmotion` do not — they dereference `pUser[conn]` or `pMob[conn]` directly. In practice those callers pass validated slots, but the safety property is a *caller obligation* rather than an enforced precondition, and several of these functions are exported through `SendFunc.h` to 45 translation units.

A second consequence is that **slot 0 can never be a message recipient in the guarded functions**. This is consistent with the grid encoding (`pMobGrid` uses `0` as "empty cell"), and with `MapaMulticast`, which deliberately starts its loop at `i = 1` (SendFunc.cpp:1664). A listener at slot 0 is structurally invisible to the whole outbound layer.

**Rule workflow:**

```
caller -> SendX(conn, ...)
          if (conn <= 0 || conn >= MAX_USER) return;      // guarded variants
          pUser[conn].cSock.AddMessage(...)               // or pMob[conn] deref
```
*Confidence: high (directly observable in source).*

---

### Business Rule: BR-02 Playing-state gate

**Overview:**
A message may only be enqueued for a slot whose `pUser[i].Mode == USER_PLAY` (macro value 22, defined at `CUser.h:36`). Slots that are free, in account login, or in character selection must never receive gameplay traffic.

**Detailed description:**
`CUser::Mode` is a coarse session-state machine. Below `USER_PLAY` the slot has a socket but not an in-world character, so `pMob[i]` is not meaningful; enqueuing a `MSG_UpdateScore` or `MSG_CreateMob` for such a slot would either leak stale world data or desynchronise the client's scene graph. Every broadcast therefore filters on this state before touching `pMob[]`: `SendNotice` (line 59), `SendNoticeChief` (74), `SendNoticeArea` (159), `SendGuildNotice` (171), `SyncMulticast` (264), `SyncKingdomMulticast` (278), `MapaMulticast` (1666), `SendMessageArea` (1680), `SendWeather` (1575), `SendSignalParmArea` (1707), `SendSignalShortParm2Area` (1730), `SendGuildList` (1275), plus the directed senders listed at BR-03.

Two notable exemptions exist. `GridMulticast` never consults `Mode == USER_PLAY`; it trusts `pMobGrid` occupancy and, for payload fan-out, only checks `tmob < MAX_USER` (lines 713, 891). The second pass of `GridMulticast(conn, ...)` partially compensates by testing `pMob[conn].Mode == MOB_EMPTY` (775) and `pMob[tmob].Mode == MOB_EMPTY` (787), but these are the *mob* mode, not the *user* mode. As a result a half-connected player slot that still holds a grid cell can receive create/remove messages before `USER_PLAY` is set — an ordering assumption that depends on logout code clearing the grid first (`CloseUser`, `Server.cpp:6834`, does exactly that). This is a cross-component contract, not an internal guarantee.

`SendNoticeChief` and `SendSummonChief` also apply stricter filters on top of `USER_PLAY` (see BR-06 and BR-07), and `PartyGridMulticast` requires both `USER_PLAY` and a live socket (line 1018) before enqueuing.

**Rule workflow:**

```
for each slot i:
    if (pUser[i].Mode != USER_PLAY) continue;
    envelope = build();  pUser[i].cSock.AddMessage(envelope, size);
```
*Confidence: high.*

---

### Business Rule: BR-03 Socket-liveness gate

**Overview:**
Before enqueuing, the implementation checks `pUser[i].cSock.Sock != 0`. A slot can be `USER_PLAY` yet have a torn-down socket, and writing into the per-slot 128 KB buffer of a dead socket would silently grow until `AddMessage` refuses it.

**Detailed description:**
`CPSock::Sock` is the raw Winsock handle (`CPSock.h:56`); `CloseSocket` zeroes it. The gate appears in `SendCreateMob` (296), `SendItem` (1063), `SendEtc` (1203), `SendCargoCoin` (1241), `SendShopList` (1377), `SendReqParty` (1446), `SendAddParty` (1480), `SendRemoveParty` (1516), `SendCarry` (1542), `SendSetHpMp` (1598), `SendHpMode` (1630), `SendWeather` (1578) and `PartyGridMulticast` (1018, 1046). It is *not* applied by `SendClientMessage`, which means global notices such as `SendNotice` will enqueue into dead slots; the failure then surfaces as an `err,add buffer invalid` log line from `CPSock::AddMessage` (`CPSock.cpp:527-533`) repeated for every notice.

The distinction between the two checks matters operationally: `Mode` is a logical state, `Sock` is a resource state. Testing only `Mode` produces log spam and wasted 128 KB writes; testing only `Sock` risks sending world data to a client that is still in character selection. The component uses both, but never uniformly, so the exact guarantee per function must be read from the source rather than assumed from the API.

Neither `SendClientSignal*` nor `SendRemoveMob`/`SendRemoveItem`/`SendCreateItem` perform this check at all. Those are the four lowest-level primitives: they are pure "write these bytes into this slot's queue" operations, deliberately thin, which is why higher layers are expected to have validated.

**Rule workflow:**

```
if (pUser[conn].Mode != USER_PLAY) return;
if (pUser[conn].cSock.Sock == 0)   return;
build(); AddMessage(); [SendMessageA() when bSend]
```
*Confidence: high.*

---

### Business Rule: BR-04 Fixed-width panel encoding and forced NUL termination

**Overview:**
`SendClientMessage` always copies exactly `MESSAGE_LENGTH` (96) bytes from the source string into `MSG_MessagePanel.String`, which is 128 bytes wide, and then forces `String[95] = 0` and `String[94] = 0`.

**Detailed description:**
`MSG_MessagePanel` is declared with `char String[128]` and a comment explaining the historical reason ("Correct size to fix SendScore Hp Bug", `Basedef.h:1198`), while the copy length remains `MESSAGE_LENGTH` = 96 (`Basedef.h:129`). The buffer is zero-filled first, so bytes 96..127 are always zero and the client sees a NUL-terminated string. Forcing *both* the last and second-to-last byte to zero is an idiom used throughout this codebase: it guarantees termination even if the source filled all 96 bytes, and it adds one byte of slack for clients that append a character before rendering.

The same idiom is applied to `pMob[i].MOB.MobName[NAME_LENGTH-1..-2]` before `strcat` in `SendGuildList` (1292-1293) and to `pUser[tmob].AccountName[...]` before logging in `GridMulticast` (801-802). The important business consequence is that **notice text is hard-truncated at 94 visible characters with no ellipsis and no continuation framing**; a longer localized string is silently cut. Because `g_pMessageStringTable` entries are formatted with `sprintf` upstream (e.g. `BASE_GetLanguage`, `Basedef.cpp:1305`), overflow of the *source* is not this component's problem, but truncation of the *destination* always is.

The mirror-image defect is `SendClientMessageOk` (179-195) and `SendChat`/`SendSay` (343, 1656): they `memcpy` 96 bytes into `MSG_MessageBoxOk.String[96]` / `MSG_MessageChat.String[96]` **without** the two-byte forced termination. If the source is exactly 96 non-NUL bytes the client receives an unterminated string. `SendClientMessage` is safe; its three siblings are not, despite doing the same conceptual thing.

**Rule workflow:**

```
memset(&sm, 0, sizeof(MSG_MessagePanel));
sm.Size = 96+12; sm.Type = _MSG_MessagePanel; sm.ID = 0;
memcpy(sm.String, Message, 96);       // unconditional 96-byte read
sm.String[95] = 0; sm.String[94] = 0;
pUser[conn].cSock.AddMessage(&sm, sizeof(MSG_MessagePanel));
```
*Confidence: high.*

---

### Business Rule: BR-05 Notice suppression marker

**Overview:**
`SendNotice` logs every notice as `"not <text>"` to the `-system` log, then drops the broadcast entirely if the first two characters are the apostrophe-plus-`x` sequence (`'x`).

**Detailed description:**
This is an operator-facing moderation control. Message text beginning with `'x` is treated as *server-log-only*: it is written to the system log, visible to administrators reading logs, but never reaches a player. The check happens **after** logging, so the suppression is auditable — the log line proves the value was produced even though nothing was sent. Ordering here is functional, not incidental.

The marker is tested positionally with no length guard (`Message[0] == '\'' && Message[1] == 'x'`). For an empty string, `Message[0]` reads the terminator and short-circuits, so index 1 is not read; for a one-character string `"'"`, `Message[1]` reads the terminator — safe by NUL. The comparison is case-sensitive, so `'X` is broadcast normally.

`SendNotice` is the single most-used moderation/announcement primitive: ~31 external call sites across 8 files, including `MobKilled.cpp:679,688,1715,1782`, `Server.cpp`, `ProcessSecMinTimer.cpp:108`, `_MSG_GetItem.cpp`, `_MSG_CombineItemOdin.cpp`. Every one of them inherits both the logging behaviour (one log write per call, always) and the `'x` suppression. Because the log is written unconditionally, high-frequency notice sources produce steady log volume even when suppressed.

Note that `SendNoticeChief` (64) performs the same `sprintf` + `Log` but **does not** implement the `'x` check, so the suppression convention silently does not apply to chief-tier notices.

**Rule workflow:**

```
sprintf(Notice, "not %s", Message);  Log(Notice, "-system", NULL);
if (Message[0] == '\'' && Message[1] == 'x') return;   // log-only
for i in [0,1000): if (pUser[i].Mode == USER_PLAY) SendClientMessage(i, Message);
```
*Confidence: high.*

---

### Business Rule: BR-06 Chief notice targeting

**Overview:**
`SendNoticeChief` addresses only guild leaders of charged (castle-holding) guilds: `USER_PLAY`, `MOB.GuildLevel == 9`, `MOB.Guild > 0`, and membership of that guild in `ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]` (10 × 5 = 50 slots).

**Detailed description:**
`GuildLevel == 9` is the fixed rank reserved for guild masters in this protocol's guild hierarchy, so the filter is a rank test rather than a permission test. The charged-guild scan (`ChargedGuildList[j][k] == Guild`, lines 87-97) restricts delivery to leaders of guilds that currently hold a castle/tax zone; a guild master without a charge is excluded. Iteration order is `j` (server 0..9) outer and `k` (zone 0..4) inner, so at most 50 comparisons per candidate leader and at most 50,000 comparisons for a full sweep of 1,000 slots.

The decisive defect is that `FoundCharged` is computed and never used. It is declared at line 85, set at line 93, and then `SendClientMessage(i, Message)` is called at line 99 unconditionally for every `GuildLevel == 9`, `Guild > 0` player. The break at line 94 only terminates the inner `k` loop, so the scan also keeps running unnecessarily. The intended predicate was evidently `if (!FoundCharged) continue;`. As shipped, the rule effectively degrades to "all guild masters with a positive guild id", and the `ChargedGuildList` scan is 50,000 wasted comparisons per notice. This is documented as a behavioural fact of the current build, not as a recommendation.

**Rule workflow:**

```
for i in [0,1000):
    if Mode != USER_PLAY            -> continue
    if MOB.GuildLevel != 9          -> continue
    if MOB.Guild <= 0               -> continue
    scan ChargedGuildList[10][5] for MOB.Guild  -> FoundCharged (unused)
    SendClientMessage(i, Message)   <-- always
```
*Confidence: high; the intent (gate on `FoundCharged`) is inferred from the variable's presence and name.*

---

### Business Rule: BR-07 Chief summon coordinate mapping

**Overview:**
`SendSummonChief` teleports every qualifying guild master to a castle-zone seat coordinate computed from the `(Server, GuildZone)` pair in which their guild appears in `ChargedGuildList`.

**Detailed description:**
The coordinate formula is arithmetic on identifiers only, with no map lookup: `tx = 7 * Server / 5 + 317`, `ty = 4025 - 2 * Server % 5`, then `tx = tx + GuildZone` if `Server / 5` is non-zero, otherwise `tx = tx - GuildZone`. Because `Server ∈ [0,9]`, `Server/5` is 1 only for servers 5..9, so the lower half of the server list shifts the seat in the opposite direction — the layout mirrors the castle map's two-row arrangement. `ty` uses C precedence so `2 * Server % 5` is `(2*Server) % 5`, giving a repeating 0,2,4,1,3 pattern across servers.

The scan collects `Server` and `GuildZone` from the *last* match (no `break` out of the `j` loop), so if a guild holds multiple zones the last one wins — a deterministic but undocumented tie-break. The `break` at line 135 exits only the inner `k` loop; the outer `j` loop continues and may overwrite both variables.

The failure path is also notable: `if (FoundCharged == 0) return;` sits **inside** the per-slot loop (140-141). If the first guild master encountered has no charge, the function aborts and no subsequent leader is summoned, instead of skipping that leader. Combined with the fact that this function is invoked from the administrative interpreter (`imple.cpp`, 1 call site), the practical effect is that a single uncharged guild master in a low slot index can suppress the entire summon.

**Rule workflow:**

```
for i in [0,1000): USER_PLAY, GuildLevel==9, Guild>0
    scan ChargedGuildList -> (FoundCharged, Server, GuildZone)  [last match wins]
    if (!FoundCharged) return;            // aborts whole broadcast
    tx = 7*Server/5 + 317;  ty = 4025 - (2*Server)%5;
    tx = (Server/5) ? tx + GuildZone : tx - GuildZone;
    DoTeleport(i, tx, ty);
```
*Confidence: high for mechanics; medium for intent (the mirrored layout is inferred from the arithmetic).*

---

### Business Rule: BR-08 Rectangular area notice

**Overview:**
`SendNoticeArea(Message, x1, y1, x2, y2)` delivers a panel message to every playing user whose `TargetX` and `TargetY` lie inside the **inclusive** rectangle `[x1,x2] × [y1,y2]`.

**Detailed description:**
All four comparisons are inclusive (`>= x1 && <= x2` on X, `>= y1 && <= y2` on Y), which differs from the grid-window convention used elsewhere in the file (half-open `[start, start+size)`). Callers must therefore reason in one convention here and the other in `GridMulticast`. Real call sites pass hand-tuned town rectangles, e.g. `MobKilled.cpp:993` uses `(1676,1556)-(1776,1636)` and `:998` uses `(1676,1816)-(1776,1892)` for king-kill announcements.

The rectangle is compared against `TargetX/TargetY`, i.e. the *movement target* of the entity, not its rendered position. Two players standing on the same tile can therefore disagree about area membership if one is mid-step toward a tile outside the rectangle. There is no distance, line-of-sight or map-id component: coordinates in this world are globally unique across the 4096×4096 space, so rectangles implicitly encode both place and map. This is why no additional map check exists.

`SendNoticeArea` has 14 external call sites in 3 files and applies no `Mode == USER_PLAY` short-circuit beyond the initial continue, no socket check, and no suppression marker support (unlike `SendNotice`).

**Rule workflow:**

```
for i in [0,1000):
    if Mode != USER_PLAY continue;
    if (X >= x1 && X <= x2 && Y >= y1 && Y <= y2) SendClientMessage(i, Message);
```
*Confidence: high.*

---

### Business Rule: BR-09 Guild-scoped notice

**Overview:**
`SendGuildNotice(Guild, Message)` delivers only to playing users whose `pMob[i].MOB.Guild` equals the requested guild id.

**Detailed description:**
The comparison is exact equality on the numeric guild id, with no validation that `Guild` is in `[1, 65535]`. Passing `0` would therefore broadcast to every guildless player, since `MOB.Guild` defaults to 0 — a caller-side hazard rather than an enforced rule. Eight external call sites exist, all in a single translation unit, and the function is the standard mechanism for guild-wide administrative text.

Unlike `SendGuildList`, which stops at `Guild <= 0` (line 1263), this function has no guard. Unlike `SendNoticeChief`, no rank test is applied: every member receives the message. The function performs no logging of its own, so guild notices are invisible in the `-system` log unless the caller logs separately.

**Rule workflow:**

```
for i in [0,1000):
    if (Mode == USER_PLAY && pMob[i].MOB.Guild == Guild) SendClientMessage(i, Message);
```
*Confidence: high.*

---

### Business Rule: BR-10 Generic signal envelope contract

**Overview:**
Five functions project arbitrary `(Type, ID, Parm…)` tuples onto the wire: `SendClientSignal(Type,ID)`, `Parm` (one int), `Parm2` (two ints), `Parm3` (three ints), and `ShortParm2` (two shorts). Together with `MSG_STANDARD` they form the "generic signal" protocol used for events that carry no world state.

**Detailed description:**
`MSG_STANDARD` carries only the 12-byte `_MSG` header (`Basedef.h:925-946`). Each variant extends it by fixed-width integer fields. Because the header already contains `Type`, `ID`, and `ClientTick`, a signal is fully described by `(Type, ID, Parm…)` and needs no per-message struct — this is the mechanism behind the 191 message-type constants that have no dedicated structure, e.g. `_MSG_UpdateCargoCoin` (`Basedef.h:1523`, documented as "TODO: Use MSG_STANDARDPARM").

The contract is *positional and untyped*: `SendClientSignalParm(conn, id, signal, parm)` performs zero validation of `conn`, `id` or `signal`. `id` is conventionally `ESCENE_FIELD` (30000, `Basedef.h:170`) when the signal refers to the scene rather than an entity, and a slot/mob index otherwise. `SendWarInfo` uses `SendClientSignalParm3(conn, 30000, _MSG_SendWarInfo, Guild, Clan, Ally)` (1435), which is the canonical three-parameter usage.

Usage distribution shows the primitive is under-exploited: `SendClientSignalParm` has ~100 external call sites in 17 files, `SendClientSignal` ~20 in 8 files, while `SendClientSignalParm2` has **zero** callers anywhere in the project, `SendClientSignalParm3` and `SendClientSignalShortParm2` are used only internally (by `SendWarInfo` and `SendShortSignalParm2Area` respectively, and both of those are effectively dead — see BR-31).

**Rule workflow:**

```
memset(&sm, 0, sizeof(MSG_STANDARDPARMn));
sm.Type = signal; sm.ID = id; sm.Parm… = parm…;
pUser[conn].cSock.AddMessage(&sm, sizeof(sm));     // no gate, no flush
```
*Confidence: high.*

---

### Business Rule: BR-11 Exclusion multicast

**Overview:**
`SyncMulticast(conn, msg, bSend)` enqueues a pre-built envelope to **every** `USER_PLAY` user except `conn`. With `bSend != 0` it also forces an immediate `send()` per recipient.

**Detailed description:**
This is the server-side half of "sync"-style chat, used so that the originator can see its own echo locally while everybody else receives the authoritative copy. `_MSG_MessageWhisper.cpp:1274` calls it with `bSend = 0`, letting the normal timer-driven flush coalesce the writes. The exclusion test is `conn != i`, i.e. identity comparison of slot indices — there is no party, guild, map or distance filter, so any caller receives a global broadcast.

`bSend = 1` converts one multicast into N `send()` calls inside the game loop, one per recipient. Since `SendMessageA` performs a single `send()` attempt and does not retry (see BR-34), forcing the flush mainly serves to make partial writes visible earlier, at the cost of syscall amplification (up to 999 calls). The component never uses `bSend = 1` internally for this function; all 2 external call sites pass `0`.

`SyncMulticast` does not validate the message pointer beyond the caller's implicit guarantee, does not verify `msg->Size`, and passes `msg->Size` as the length to `AddMessage` — so an envelope whose `Size` field disagrees with its actual storage causes an over-read inside `AddMessage`. This is the general structural weakness of every function that takes `MSG_STANDARD *m` (see also BR-12, BR-29, BR-30).

**Rule workflow:**

```
for i in [0,1000):
    if (Mode == USER_PLAY && i != conn) { AddMessage(m, m->Size); if (bSend) SendMessageA(); }
```
*Confidence: high.*

---

### Business Rule: BR-12 Kingdom multicast filters

**Overview:**
`SyncKingdomMulticast(conn, Kingdom, msg, bSend)` restricts delivery to users whose `MOB.Clan == Kingdom` **and** whose `KingChat` flag is 0, excluding the origin slot.

**Detailed description:**
`Kingdom` is the clan/kingdom id, matched against the recipient's `pMob[i].MOB.Clan`. The `KingChat == 0` term is a per-user opt-out: users who have disabled king chat (a client-side toggle mirrored into `CUser::KingChat`, `CUser.h:107`) are skipped. Both conditions must hold, and the origin is excluded by the same `conn != i` test as `SyncMulticast`.

Semantically this is a *channel* rather than a *location*: it is the only fan-out in the component that combines a persistent identity attribute (clan) with a mutable preference (KingChat) and no spatial predicate at all. It has exactly one external caller (`_MSG_MessageWhisper.cpp:1247`), which uses it for kingdom-wide chat, with `bSend = 0`.

The filter order matters for cost: `Mode == USER_PLAY` is evaluated first, then `conn != i`, then `Clan`, then `KingChat`. Because `pMob[i]` is read for every playing slot regardless of clan, the loop cost is dominated by the array walk, not the comparisons.

**Rule workflow:**

```
for i in [0,1000):
    if (Mode == USER_PLAY && i != conn && MOB.Clan == Kingdom && KingChat == 0)
        { AddMessage(m, m->Size); if (bSend) SendMessageA(); }
```
*Confidence: high.*

---

### Business Rule: BR-13 Mob appearance projection with trade substitution

**Overview:**
`SendCreateMob(conn, otherconn, bSend)` projects entity `otherconn` into slot `conn`'s client. If `otherconn` is a valid player in `TradeMode == 1` (an auto-trade stall), the payload is the richer `MSG_CreateMobTrade`; otherwise `MSG_CreateMob`.

**Detailed description:**
The substitution rule exists so that a player-owned stall renders with its title and item list (`MSG_CreateMobTrade.Desc[MAX_AUTOTRADETITLE]`, `Basedef.h:1550`) rather than as a plain avatar. The test is `otherconn <= 0 || otherconn >= MAX_USER || pUser[otherconn].TradeMode != 1` (304) — note the ordering: an NPC (`otherconn >= MAX_USER`) can never be a stall, so the plain struct is always used for mobs. `TradeMode == 1` is thus the sole discriminator, and it is read from `CUser`, not from the mob record.

`GetCreateMob` and `GetCreateMobTrade` (`GetFunc.cpp:947` and `:1072`) fill the struct from `pMob[otherconn]`; the two structs share a 100-byte prefix up to `Tab[26]` and diverge afterwards (`Hold` vs `Desc`). Both payloads are enqueued with `if (AddMessage(...)) SendMessageA();` — meaning the **`bSend` parameter is dead**: the flush is unconditional whenever the enqueue succeeded. This is a real behavioural difference from its siblings `SendCreateItem`/`SendRemoveMob`/`SendRemoveItem`, which honour `bSend`. Every internal caller passes `bSend = 0` (SendGridMob 604, GridMulticast 780/792), so in practice every projected mob appearance incurs an immediate `send()` — 1,089 of them per grid broadcast in the worst case, which is the single largest syscall amplifier in the component.

The function's gates (`conn` range, `USER_PLAY`, live socket, lines 290-297) are the complete BR-01/02/03 stack, making it one of the safest entry points despite the flush behaviour.

**Rule workflow:**

```
if (conn invalid | Mode != USER_PLAY | Sock == 0) return;
if (otherconn invalid || TradeMode != 1) { GetCreateMob(otherconn,&sm); AddMessage? SendMessageA(); return; }
GetCreateMobTrade(otherconn,&sm2); AddMessage? SendMessageA();   // bSend ignored
```
*Confidence: high for behaviour; the "dead parameter" is directly observable (bSend is never read after entry).*

---

### Business Rule: BR-14 Grid single-occupancy invariant and self-healing repair

**Overview:**
`pMobGrid[y][x]` must hold at most one entity, and the entity's own `(TargetX, TargetY)` must point at the cell that contains it. `GridMulticast(conn, tx, ty, msg)` re-establishes this invariant on every call before fanning out.

**Detailed description:**
The cell array is `unsigned short pMobGrid[MAX_GRIDY][MAX_GRIDX]` (`Server.h:54`), 4096×4096 = 16.7 M cells, using `0` as the empty marker — which is why slot 0 can never own a cell and therefore never appears in a grid fan-out. The repair sequence is defensive in depth. First, if the entity's recorded cell currently holds a *different* entity, `GetEmptyMobGrid` (`GetFunc.cpp:1901`) re-seats it, and a diagnostic is logged (`PC do not have his own grid` for `conn < MAX_USER`, `NPC do not have his own grid` otherwise). Then the recorded cell is unconditionally zeroed (643). Next, if the *destination* cell `pMobGrid[ty][tx]` is occupied by something else, the entity is moved to a free cell again, and `PC step in other mob's grid` / `NPC charge other mob's grid` is logged (648-655). Finally the destination cell is claimed (657).

Two consequences follow. First, the destination cell is claimed **before** any bounds validation of `tx`/`ty`; the component relies entirely on the caller having validated coordinates. `_MSG_Action.cpp:192` performs exactly this validation (`TargetX <= 0 || >= 4096`), which is the only reason the write at 657 is safe in the shipped paths. Second, because the repair runs on every broadcast, a corrupted grid self-corrects during normal play — but the correction is *silent state mutation performed by a send function*, so any caller that computes a target from a grid read must tolerate the array changing under it.

The same invariant is enforced opportunistically in the read passes: cells pointing at `MOB_EMPTY` entities are zeroed (765-767 with log `MOB GRID HAS EMPTY MOB`, 786-788, 776, 601, 614). `SendGridMob` applies the identical cleanup for the viewer's initial viewport (600-615).

**Rule workflow:**

```
tmob = grid[Y][X(conn)];
if (tmob != conn && tmob) GetEmptyMobGrid(conn, &X, &Y) + log;
grid[Y][X] = 0;                                  // release
if (grid[ty][tx] != conn && grid[ty][tx]) GetEmptyMobGrid(conn, &X, &Y) + log;
grid[ty][tx] = conn;                             // claim (tx/ty assumed valid)
... fan-out passes ...
TargetX = tx; TargetY = ty;                      // commit
```
*Confidence: high.*

---

### Business Rule: BR-15 Viewport geometry and border clamping

**Overview:**
The visibility window is `VIEWGRIDX × VIEWGRIDY = 33 × 33` cells (`Basedef.h:95-96`) centred with `HALFGRIDX/HALFGRIDY = 16`, so the anchor cell's `-16..+16` neighbourhood is visible. Windows are clamped to `[0, MAX_GRIDX/Y)` with `MAX_GRID = 4096`.

**Detailed description:**
The computation appears three times with the same shape: `StartX = tx - 16`, then shrink `SizeX` if `StartX + SizeX >= 4096`, then clamp `StartX` to `>= 0`. The east/north-edge branch is correct: shrinking `SizeX` by the overflow keeps the window inside the map while preserving the anchor. The west/south-edge branch is not. In `GridMulticast(tx, ty, msg, skip)` (856-868) the code does:

```
if (StartX < 0) { StartX = 0; SizeX = SizeX + StartX; }
```

`StartX` has already been assigned 0, so `SizeX + 0` is a no-op: the window silently loses `16` columns instead of shifting east to re-centre, and the same for rows. In `GridMulticast(conn, ...)` (670-674) and `SendGridMob` (579-583) there is not even an attempt — `StartX = 0` and `SizeX` is left at 33, which is the correct outcome only because the earlier east-edge clamp already accounted for the map boundary. The net effect: near the western and southern borders of the 4096² world, the visible window shrinks to as little as 17 cells wide instead of sliding.

`SendEnvEffect`/`SendEnvEffectKingdom`/`SendEnvEffectLeader` avoid the issue by computing a single centre point `x1 + (x2-x1)/2` and delegating to the 4-argument `GridMulticast` (384, 430, 491), i.e. an area effect is approximated by one 33×33 window at the rectangle's midpoint rather than by the rectangle itself. `SendEnvEffect` retains the rectangle-accurate implementation only as commented-out code (366-382), with the per-cell `MAX_USER` guard preserved in the comment — evidence that the rectangle walk was abandoned in favour of the window.

**Rule workflow:**

```
StartX = tx - HALFGRIDX(16); StartY = ty - HALFGRIDY(16);
if (StartX + 33 >= 4096) SizeX -= (StartX + SizeX - 4096);
if (StartY + 33 >= 4096) SizeY -= (StartY + SizeY - 4096);
if (StartX < 0) StartX = 0;      // width NOT compensated  (defect)
if (StartY < 0) StartY = 0;
iterate [StartX, StartX+SizeX) x [StartY, StartY+SizeY)
```
*Confidence: high (arithmetic is explicit).*

---

### Business Rule: BR-16 Visibility delta fan-out

**Overview:**
`GridMulticast(conn, tx, ty, msg)` performs a three-pass diff between the entity's **previous** viewport and its **new** viewport: pass 1 delivers the caller's message to the old window, pass 2 emits item-removal for cells leaving the window, pass 3 emits item/mob creation for cells entering the window plus the caller's message.

**Detailed description:**
Pass 1 (704-725) walks the old window (`sx1..sx2`, `sy1..sy2`, anchored on the *pre-move* `pMob[conn].TargetX/Y`) and, for every cell holding an entity other than `conn`, enqueues the caller's message. It then applies the "left the new window" test and emits reciprocal `SendRemoveMob` in both directions: `SendRemoveMob(tmob, conn, 0, 0)` tells the other client that `conn` disappeared, and `SendRemoveMob(conn, tmob, 0, 0)` tells `conn` that `tmob` disappeared.

Pass 2 (728-743) repeats the sweep over `pItemGrid` and issues `SendRemoveItem(conn, titem, 0)` for items that fell out of view — only to `conn`, since ground items are not entities that need reciprocal notification.

Pass 3 (745-811) walks the **new** window and, for cells that were *not* in the old window, emits `SendCreateItem`, `SendCreateMob` plus `SendPKInfo` in both directions, and finally the caller's message again for the newly visible entities. Because the guard is "outside the old window", entities in the intersection receive the message exactly once (pass 1) and are not re-created; entities entering the view receive it exactly once (pass 3). The design is therefore correct in its *set semantics* even though it makes two full 1,089-cell sweeps plus one 1,089-cell item sweep per move.

The visibility predicate itself is defective: `if (x < tx1 || x >= tx2 || y < ty1 || y >= ty2 && tx)` (716, and 737 for items). C precedence binds `&&` tighter than `||`, so the term is `(x < tx1) || (x >= tx2) || (y < ty1) || ((y >= ty2) && tx)`. The intended `(y >= ty2)` test is thus gated on `tx != 0`, and `tx` is a coordinate that *is* legitimately 0 in the west column of the map. When `tx == 0`, an entity leaving northward is not recognized as leaving. The presence of `&& tx` — a coordinate used as a boolean — indicates a lost comparison in the original source.

Every enqueue in pass 3 checks the `AddMessage` return value and logs `err,gridmulticast add <slot>-<mode> <account>` after forcing the account name's last two bytes to NUL (797-807) — unique in the component: this is the only place that reports per-recipient queue overflow with identity.

**Rule workflow:**

```
old = window(pre-move TargetX/Y); new = window(tx, ty)
pass1 over old:  if tmob ∉ {0, conn}: AddMessage(msg); if tmob left new -> RemoveMob both ways
pass2 over old:  if item left new -> RemoveItem(conn)
pass3 over new:  if cell ∉ old:
                    items -> CreateItem(conn)
                    mobs  -> CreateMob both ways + PKInfo both ways
                    msg   -> AddMessage(tmob) [with overflow logging]
commit LastX/LastY/TargetX/TargetY from the MSG_Action payload
```
*Confidence: high for the three-pass structure; high for the precedence defect (language semantics are unambiguous).*

---

### Business Rule: BR-17 Ground item visibility gate

**Overview:**
A ground item may be projected to a client only if `0 < itemid < MAX_ITEM` (5000), `pItem[itemid].Mode != 0`, and `pItem[itemid].ITEM.sIndex != 0`.

**Detailed description:**
`pItemGrid` stores item ids in the same 4096×4096 address space as mobs (`Server.h:55`), so a cell can legitimately contain both an entity and an item. The three-part gate distinguishes "cell occupied by a live item" from a stale id: `MAX_ITEM` bounds the array, `Mode` marks liveness, and `sIndex != 0` marks a non-empty item definition (an item with index 0 is an empty slot that nevertheless has a live container).

The rule appears in pass 2 (739), pass 3 (754) and in `SendGridMob` (609-615). Cells that fail the `Mode` test are zeroed (`pItemGrid[y][x] = 0`), continuing the self-healing policy of BR-14 — item grid corruption is repaired during fan-out, silently in `GridMulticast`, with no log line. `SendGridMob` does the same at 614.

`SendRemoveItem` is the inverse projection and does not re-check the gate: it trusts its caller, writes `_MSG_DecayItem` with `ItemID = 10000 + itemid` (518), and takes `bSend` seriously. The `10000 +` offset is a wire convention separating item ids from entity ids in the client's id space.

**Rule workflow:**

```
for each cell in window:
    if (itemid == 0) continue;
    if (itemid > 0 && itemid < MAX_ITEM && pItem[itemid].Mode && pItem[itemid].ITEM.sIndex)
        -> SendCreateItem / SendRemoveItem
    else if (!pItem[itemid].Mode) pItemGrid[y][x] = 0;
```
*Confidence: high.*

---

### Business Rule: BR-18 Per-recipient payload rewriting

**Overview:**
`GridMulticast(tx, ty, msg, skip)` mutates the caller's message buffer between recipients. For `_MSG_CNFMobKill` it writes the recipient's own `Exp` and `Hold`; for `_MSG_CreateMob` it overrides two equipment slots with a hard-coded cosmetic set inside a fixed world rectangle.

**Detailed description:**
For `_MSG_CNFMobKill` (920-923) the function assigns `((MSG_CNFMobKill*)msg)->Exp = pMob[tmob].MOB.Exp` and `->Hold = pMob[tmob].extra.Hold`, where `tmob` is the **recipient**, not the killed mob. The caller in `MobKilled.cpp:173-181` fills `KilledMob`, `Killer` and leaves `Exp/Hold` at zero before calling. The rewrite therefore implements "each client sees its own accumulated experience and carried money in the kill confirmation" — a per-viewer projection that cannot be expressed with a single shared buffer, which is exactly why the mutation exists. The message pointer aliases the caller's stack object, so after the broadcast returns, `MSG_CNFMobKill.Exp` holds the *last* recipient's value; any caller that inspects the struct afterwards sees corrupted data. There are 53 call sites for this overload, all of which discard the buffer afterwards.

For `_MSG_CreateMob` (893-918) the rule is a map-specific cosmetic override: if the payload's `PosX/PosY` fall inside `x ∈ [896,1150]`, `y ∈ [1405,1538]`, equipment slot 1 is forced to item 3505 and slot 15 to item 3999, both re-encoded through `BASE_VisualItemCode`/`BASE_VisualAnctCode` (`Basedef.cpp:6598`, `:6662`) for the correct slot index. This is a hard-coded "wings/aura" appearance for a specific region, with no configuration indirection. Note the guard is `xx >= 896 && yy >= 1405 && xx <= 1150 && yy <= 1538` — inclusive, and using the *payload* coordinates rather than the entity's current position, so the appearance depends on where the create message claims the entity stands.

Both rewrites happen inside `if (msg != NULL && tmob < MAX_USER)`, so only player recipients trigger them; NPC-only viewports receive the unmodified shared buffer.

**Rule workflow:**

```
for each entity tmob in window (tmob>0, tmob!=skip, tmob<MAX_USER):
    if msg->Type == _MSG_CreateMob and pos in [896..1150]x[1405..1538]:
        msg.Equip[1]=Visual(3505); msg.AnctCode[1]=Anct(3505)
        msg.Equip[15]=Visual(3999); msg.AnctCode[15]=Anct(3999)
    if msg->Type == _MSG_CNFMobKill:
        msg->Exp = pMob[tmob].MOB.Exp; msg->Hold = pMob[tmob].extra.Hold
        (see BR-19 cascade)
    AddMessage(msg, msg->Size)
```
*Confidence: high for mechanics; the design intent of the `Exp` rewrite is inferred (medium) from the caller's zero-initialisation.*

---

### Business Rule: BR-19 Level-segment notification cascade

**Overview:**
When broadcasting `_MSG_CNFMobKill`, the function calls `pMob[tmob].CheckGetLevel()` for every recipient and, on a non-zero segment (1..4), emits the corresponding bonus message, re-sends score/emotion, and — at segment 4 — applies the full level-up sequence including `+5` PK points and a cosmetic re-broadcast.

**Detailed description:**
`CMob::CheckGetLevel()` (`CMob.cpp:1034`) is a stateful evaluator: it compares `MOB.Exp` against four thresholds derived from the level table (quartiles of the current level's span) and against `nextexp`; if the player crossed `nextexp` it increments `BaseScore.Level`, raises MaxHp/MaxMp by the class increment tables, recomputes the derived score, and returns `4`. Otherwise it returns `1`, `2` or `3` at the respective quartile, but only once — the mob's `Segment` field (declared `CMob.h:58`) latches progress so a quartile message is not repeated. Segments 1..3 also refill HP/MP to maximum. The function returns `0` at `MAX_LEVEL`, at the class-specific cap (`MAX_CLEVEL` for celestial tiers), and when a level-gated quest prerequisite is unmet.

The cascade in `SendFunc.cpp` maps the return value to user-visible consequences: `4` → `SetCircletSubGod`, `_NN_Level_Up` panel message, and `DoItemLevel` for `MORTAL` classes; `3` → `_NN_3_Quarters_Bonus`; `2` → `_NN_2_Quarters_Bonus`; `1` → `_NN_1_Quarters_Bonus`. In all four cases it then calls `SendScore(tmob)` and `SendEmotion(tmob, 14, 3)` — the latter itself performing a `GridMulticast` around the player, i.e. a nested fan-out inside the outer fan-out loop. At segment 4 it additionally calls `SendEtc`, adds `GetPKPoint(tmob) + 5` via `SetPKPoint`, rebuilds a `MSG_CreateMob` for the player with `GetCreateMob` and broadcasts it so that everyone nearby redraws the new level's appearance, and logs `lvl <name> level up to <level>` with the account name and IP.

Two structural observations. First, the cascade is **distributed per viewer**: because `tmob` iterates recipients, level progression is evaluated for every player present in the window, not only for the killer. Any player who has banked experience and is standing nearby will be promoted during somebody else's kill broadcast. Second, the nested `GridMulticast` calls (via `SendEmotion`, `SendScore` → `SendAffect`, and the level-4 re-broadcast) mean the outer 1,089-cell loop can trigger up to four additional 1,089-cell loops per promoting player, and the outer loop continues iterating a `pMobGrid` that the nested calls may have rewritten. There is no recursion guard.

**Rule workflow:**

```
for each player tmob in window:
    msg.Exp = pMob[tmob].MOB.Exp; msg.Hold = pMob[tmob].extra.Hold;
    seg = pMob[tmob].CheckGetLevel();
    if (seg in 1..4):
        if seg==4: SetCircletSubGod; _NN_Level_Up; if MORTAL -> DoItemLevel
        if seg==3: _NN_3_Quarters_Bonus
        if seg==2: _NN_2_Quarters_Bonus
        if seg==1: _NN_1_Quarters_Bonus
        SendScore(tmob); SendEmotion(tmob,14,3);
        if seg==4: SendEtc; SetPKPoint(+5); rebuild+broadcast CreateMob; log
    AddMessage(msg)
```
*Confidence: high for the mapping; high for the per-viewer distribution (the loop variable is unambiguous); medium for the "unintended" characterisation.*

---

### Business Rule: BR-20 Party multicast eligibility

**Overview:**
`PartyGridMulticast(tx, ty, msg, skip, Leaderconn)` is intended to send to all nearby non-party members plus the full party roster of `Leaderconn`, excluding the skip slot and `PartyChat`-muted users. **The function has zero call sites in the entire project.**

**Detailed description:**
The spatial part (998-1025) walks the 33×33 window and, for entities that are *not* the leader and *not* led by the leader (`pMob[tmob].Leader != Leaderconn`), enqueues the message provided the recipient is `USER_PLAY`, has a live socket, is not the skip slot and has `PartyChat == 0`. The roster part (1027-1052) then addresses the leader directly and iterates `pMob[Leaderconn].PartyList[0..MAX_PARTY-1]` (12 entries), skipping invalid slots, the skip slot, `PartyChat != 0`, and non-playing or socket-less members, closing the user's connection if the enqueue fails.

The dead-code status makes several latent issues moot but worth recording for anyone reviving it: the roster loop and the spatial loop can address the same party member twice when a member is both nearby and in the roster (no de-duplication); the spatial loop's `!Leaderconn ||` short-circuit means that with `Leaderconn == 0` the leader filter is disabled entirely; and the DOUBLE `Mode`/`Sock` check at 1042 and 1046 tests the same field twice with different partners.

Because nothing calls it, the effective party fan-out in the shipped server is not this function at all — party chat travels through `SyncMulticast`/chat handlers instead.

**Rule workflow:**

```
spatial: for cells in window: tmob valid, !=Leaderconn, Leader != Leaderconn,
             Mode==USER_PLAY, Sock!=0, !=skip, PartyChat==0 -> AddMessage
roster : Leaderconn valid & PLAY -> AddMessage (if != skip)
         for i in [0,12): partyconn valid, !=skip, PLAY, PartyChat==0, Sock!=0
                          -> AddMessage else CloseUser
```
*Confidence: high (including the zero-caller finding).*

---

### Business Rule: BR-21 Equipment visual encoding and mount rules

**Overview:**
`SendEquip(conn, skip)` re-projects all 16 equipment slots (plus their enchant codes) into a `MSG_UpdateEquip` broadcast, with special handling for slot 14 mount items in the item-index range `[2360, 2390)`.

**Detailed description:**
Every slot is encoded as `sm_ue.Equip[i] = BASE_VisualItemCode(item, i)` and `sm_ue.AnctCode[i] = BASE_VisualAnctCode(item)`, so the client receives a *renderable* pair (base visual id for that slot + ancient/enchant code) rather than raw item indices. The slot index is passed into the encoder because the same item can map differently per slot.

Slot 14 (`i == 14`) is the mount/board slot and carries two rules. First, if the encoded value is in `[2360, 2390)` **and** `Equip[14].stEffect[0].sValue <= 0`, the item is a depleted/expired mount: the slot is zeroed so the client renders no mount, `SendMount` is latched, and the encoding loop `continue`s. Second, if the value is in the range and the effect *is* present, the mount level is read from `stEffect[1].cEffect`, divided by 10, clamped to `[0,13]`, multiplied by 4096 and **added** to the base visual code. That encodes level 0..13 into the high bits of the same 16-bit field — a bit-packed projection with a hard ceiling of 13 despite there being no obvious protocol limit.

After the encoding loop the built struct is broadcast through the 4-argument `GridMulticast` with `skip` (1130), and if the mount was suppressed, `SendItem(conn, ITEM_PLACE_EQUIP, 14, &pMob[conn].MOB.Equip[14])` re-sends the raw slot to the owner only (1132-1133) so the owner's inventory view stays authoritative. The asymmetry is deliberate: other players must not see the depleted mount, but the owner must still see the item.

Note that `SendEquip` does not validate `conn` before dereferencing `pMob[conn].MOB.Equip`, even though every other directed sender does.

**Rule workflow:**

```
for i in [0,16):
    Equip[i] = Visual(Equip[i], i); AnctCode[i] = Anct(Equip[i]);
    if (i==14 && Equip[14] in [2360,2390) && stEffect[0].sValue <= 0) { Equip[14]=0; SendMount=1; continue; }
    if (i==14 && Equip[14] in [2360,2390)) { lvl=clamp(stEffect[1].cEffect/10,0,13); Equip[14] += lvl*4096; continue; }
GridMulticast(TargetX, TargetY, &sm_ue, skip);
if (SendMount) SendItem(conn, ITEM_PLACE_EQUIP, 14, &Equip[14]);
```
*Confidence: high.*

---

### Business Rule: BR-22 Score projection and guild concealment

**Overview:**
`SendScore(conn)` broadcasts `MSG_UpdateScore` — the full derived combat stat block — and applies three concealment rules: `GuildDisable` zeroes the guild, `BrState` inside the arena rectangle zeroes guild and guild level, and `Special[0..3]` are forced to `0xCC`.

**Detailed description:**
The payload is assembled from five sources: `pMob[conn].MOB.CurrentScore` (copied whole via `memcpy` into `Score`, 1152), `Critical`, `SaveMana`, `Guild`, `GuildLevel`, `Affect[]` (via `GetAffect`, `GetFunc.cpp:1174`), `Resist[0..3]` and `Magic`. `CurrHp`/`CurrMp` are copied only when `conn < MAX_USER` (1143-1147), leaving them zero for NPC stat broadcasts — the struct is shared between player and NPC projection.

The `GuildDisable` rule (1173-1174) is a hard suppression of guild identity, presumably for disguise/appearance-suppression gameplay. The `BrState` rule (1176-1189) is a *location-based* concealment: only when the global `BrState` is non-zero (battle-royale mode active) and the player stands inside `x ∈ [2604,2648]`, `y ∈ [1708,1744]`, both `Guild` and `GuildLevel` are zeroed. This is the same inclusive-rectangle idiom as BR-08, applied to identity instead of messaging. Both rules write to the local payload only; `pMob[conn].MOB.Guild` is untouched, so the concealment is per-broadcast and reversible.

`Special[0..3] = 0xCC` (1166-1169) is a protocol constant (204, matching the `0xCCCC` "target" constant in BR-27); the client interprets it as "no special attribute preview". It is unconditionally applied, so the four special stat points a player allocated are never exposed in this message.

The broadcast uses the 4-argument `GridMulticast` with `skip = 0`, meaning **no slot is skipped** and the player receives their own update (recipient `tmob == conn` is filtered by `tmob != conn` inside `GridMulticast` only for `conn`, and here `conn` is the *anchor coordinate*, not a skip target — so the anchor player's grid cell, if `pMobGrid` points to themselves, is emitted). It then calls `SendAffect(conn)` (1192), which sends the affect array again as a dedicated message, duplicating the `Affect[]` field already present in `MSG_UpdateScore`.

`SendScore` has ~96 external call sites across 13 files — it is the second most-used projection after `SendClientMessage`, and every one of them pays the full 1,089-cell broadcast.

**Rule workflow:**

```
build MSG_UpdateScore from CurrentScore, Critical, SaveMana, Guild, GuildLevel, GetAffect, Resist, Magic
if (GuildDisable) Guild = 0;
if (BrState && pos in [2604,2648]x[1708,1744]) { Guild = 0; GuildLevel = 0; }
Special[0..3] = 0xCC;
GridMulticast(TargetX, TargetY, &sm_vus, 0);
SendAffect(conn);
```
*Confidence: high.*

---

### Business Rule: BR-23 Character facet projections (`SendEtc`, `SendCargoCoin`, `SendCarry`, `SendSetHpMp`, `SendHpMode`)

**Overview:**
Five single-recipient projections carry the non-combat facets of a character: progression and money (`SendEtc`), cargo coin (`SendCargoCoin`), the 64-slot carry inventory plus coin (`SendCarry`), HP/MP with server-side request thresholds (`SendSetHpMp`), and HP with the session mode (`SendHpMode`).

**Detailed description:**
`SendEtc` (1195-1231) projects `Exp` (64-bit), `Learn` (64-bit), `Hold`, `Coin`, `Magic` and the three bonus pools (`ScoreBonus`, `SpecialBonus`, `SkillBonus`) into `MSG_UpdateEtc`. It is the only message in the component carrying 64-bit fields, consistent with the level curve needing more than 2³¹ experience.

`SendCargoCoin` (1233-1257) reuses the `MSG_STANDARDPARM` wire layout with `Type = _MSG_UpdateCargoCoin`, `ID = ESCENE_FIELD`, `Parm = pUser[conn].Coin`. It therefore sends the **account-level** coin (`CUser::Coin`, `CUser.h:51`), which is a different store from the character-level `pMob[conn].MOB.Coin` used by `SendCarry` and `SendEtc`. The two stores are projected by different functions and can legitimately differ — an important, undocumented distinction for anyone reconciling currency.

`SendCarry` (1534-1559) memcpy's all 64 `STRUCT_ITEM` entries (512 bytes) plus the character coin into `MSG_UpdateCarry`. At 528 bytes this is the largest routinely-sent message in the component; the transport ceiling is 8192 (`CPSock.h:38`), so headroom is ample, but the 128 KB send buffer holds only ~248 such messages before `AddMessage` refuses.

`SendSetHpMp` (1590-1620) is the only projection that *recomputes server state before sending*: it calls `SetReqHp(conn)` and `SetReqMp(conn)` (`Server.cpp:8659`, `:8674`) to derive the delayed-regeneration thresholds from the current HP/MP, then reads them back from `pUser[conn].ReqHp/ReqMp`. The message thus carries both the instantaneous values and the server's regeneration bookkeeping, keeping the client's prediction model in sync.

`SendHpMode` (1622-1645) sends current HP plus `pUser[conn].Mode`. Because the mode value is sent to the client, the *client* is told the server-side session state — the message is used as the "you are now playing" handshake by callers such as `_MSG_REQShopList.cpp:26`.

All five apply BR-01/02/03 and close the connection when the enqueue fails (`CloseUser`), which is the strictest back-pressure policy in the component.

**Rule workflow:**

```
if (conn invalid || Mode != USER_PLAY || Sock == 0) return;
build payload from pMob[conn].MOB / pUser[conn];
if (!AddMessage(...)) CloseUser(conn);
```
*Confidence: high.*

---

### Business Rule: BR-24 Guild roster chunking and war/ally enumeration

**Overview:**
`SendGuildList(conn)` renders the guild roster as a sequence of panel messages, each ≤70 characters, then appends war and alliance status lines derived from `g_pGuildWar[]` and `g_pGuildAlly[]` (both `unsigned short[65536]`).

**Detailed description:**
The roster loop (1273-1297) walks all 1,000 slots, filters `USER_PLAY` and exact guild equality, increments `members`, and appends `MobName` plus a space to a 128-byte accumulation buffer. Before appending, if the current length has reached 70 (`max_size`), the buffer is flushed as a panel message and cleared. The threshold of 70 is well below both the 128-byte buffer and the 94-visible-character limit of BR-04, so a flush can never truncate; it also keeps the client's chat window lines short. Name termination is forced (`MobName[15]` and `[14]` = 0) before the `strcat`, protecting against an unterminated 16-byte name (BR-04 idiom). After the loop, any residue is flushed, and if `members == 0` the localized `_NN_No_Guild_Members` line is sent instead.

The war/ally phase is the expensive part. It first re-validates the guild id against `[1, 65536)` (1305-1308), fetches the guild's war target via `g_pGuildWar[Guild]` and resolves its display name through `BASE_GetGuildName(ServerGroup, id, buf)` (`Basedef.cpp:1314`, which decodes `id` into `(server = id / MAX_GUILD, guild = id % MAX_GUILD)` and renders `[UNHANDLED]` behaviour for out-of-range groups). If the war pointer is mutual (`g_pGuildWar[gwar] == Guild`), the player is told "*you* are at war"; otherwise "*war to* <name>". Then it scans **every id from 1 to 65535** looking for guilds that declared war on the player's guild, skipping self and the already-reported mutual target, and emits one panel message per match. The alliance phase repeats the pattern for `g_pGuildAlly[]`, again scanning 65,535 ids twice (once for the direct ally, once for incoming alliances).

The total cost of one call is therefore ~1,000 slot reads + up to 131,070 global reads + one panel message per result. `SendGuildList` is invoked from `_MSG_MessageChat.cpp` (1 call site) on the guild-chat path, so a single guild chat line can trigger over 130,000 iterations on the game thread. This is a performance characteristic of the rule, not an implementation detail.

**Rule workflow:**

```
if (MOB.Guild <= 0) return;
for i in [0,1000): PLAY && Guild match -> members++; if (len >= 70) flush; append "Name "
if (str[0]) flush;  if (members == 0) panel(_NN_No_Guild_Members);
if (Guild invalid) return;
report mutual/enemy war; for j in [1,65535) report guilds at war with us
report mutual/mutual ally; for j in [1,65535) report guilds allied to us
```
*Confidence: high.*

---

### Business Rule: BR-25 Shop list flattening and city tax

**Overview:**
`SendShopList(conn, MobIndex, ShopType)` projects a merchant's 27-slot carry array into `MSG_ShopList` using a 9-column × 3-row layout, and attaches the city tax of the village containing the merchant.

**Detailed description:**
`MAX_SHOPLIST` is 27 (`Basedef.h:82`), and the mapping from display position to carry slot is `invpos = (i % 9) + ((i / 9) * MAX_SHOPLIST)`. With `i` in `[0,27)`, `i % 9` gives the column and `i / 9` the row, so row 0 maps to carry 0..8, row 1 to carry 27..35, and row 2 to carry 54..62 — the maximum index is 62, inside `MAX_CARRY = 64`. The three `tx`/`ty` locals computed on lines 1392-1393 are dead (never read). The item copy uses two 4-byte integer assignments (1398-1399) instead of `memcpy`, an idiom repeated for `item` copies elsewhere; it is correct because `STRUCT_ITEM` is exactly 8 bytes (`Basedef.h:398-412`).

The tax rule resolves the merchant's *current* position through `BASE_GetVillage(posX, posY)` (`Basedef.cpp:4258`), which linearly probes the five `g_pGuildZone[]` city rectangles and returns `0..4`, or `5` if the position is outside every city. Since `5 >= MAX_GUILDZONE`, the `Tax` is zeroed outside cities and set to `g_pGuildZone[village].CityTax` inside. The tax is therefore a function of where the merchant stands, not of where the buyer stands — a merchant walked into a different guild's city would charge that city's tax on the next shop-list request.

`MobIndex` is not validated inside this function; the sole caller (`_MSG_REQShopList.cpp:33-48`) validates `MAX_USER <= target < MAX_MOB`, checks `Merchant != 0`, and verifies `GetInView(conn, target)` before calling — a caller obligation, like the coordinate contract of BR-14. `ShopType` is passed straight through from the caller (1 for normal merchants, 3 for merchant type 19).

**Rule workflow:**

```
gates (conn, PLAY, Sock)
for i in [0,27): invpos = (i%9) + (i/9)*27; List[i] = Carry[invpos]
village = BASE_GetVillage(merchant.X, merchant.Y);
Tax = (village in [0,5)) ? g_pGuildZone[village].CityTax : 0;
if (!AddMessage(...)) CloseUser(conn);
```
*Confidence: high.*

---

### Business Rule: BR-26 War-info normalization (mutating projection)

**Overview:**
`SendWarInfo(conn, Clan)` emits `_MSG_SendWarInfo` with `Parm1 = mutual enemy guild`, `Parm2 = the supplied Clan`, `Parm3 = ally guild`, after **normalizing the player's guild and the guild's war/ally pointers in place** and requiring the war relation to be declared in both directions.

**Detailed description:**
The normalization block (1421-1428) is intended to clamp corrupt ids: if `pMob[conn].MOB.Guild` is outside `[1,65536)` it is *written back as 0*; if `g_pGuildWar[Guild]` or `g_pGuildAlly[Guild]` is outside the range it is written back as 0. Because the arrays are `unsigned short[65536]` (`Server.h:51-52`, `:306-307`), the `>= max_guild` half of each test is unreachable and the `<= 0` half is true only for the value 0 — so the statement "set it to 0 if it is 0" never repairs anything for the war/ally arrays, while the player-guild branch can genuinely reset a corrupt id. `max_guild` is a local named 65536, shadowing the array bound rather than using the constant `MAX_GUILD` used by `BASE_GetGuildName`.

The mutual-war rule (1430-1433) is the substantive business logic: the reported enemy is non-zero only if the player has a guild, that guild's war pointer is non-zero, **and** the pointed-to guild's own war pointer points back. A one-sided declaration therefore produces `Parm1 = 0` (no war reported) while the "war to" line of BR-24 would still be rendered — the two projectors disagree about one-sided wars. `Clan` is forwarded unchanged as `Parm2`, and the ally slot is read directly from `g_pGuildAlly[Guild]` without a mutuality check.

That a *send* function writes authoritative guild state is the rule's most consequential property: any caller that treats `SendWarInfo` as side-effect-free will observe guild ids being zeroed. There are 4 external call sites in 2 files.

**Rule workflow:**

```
if (MOB.Guild invalid) MOB.Guild = 0;
if (g_pGuildWar[MOB.Guild] invalid) g_pGuildWar[...] = 0;      // dead condition
if (g_pGuildAlly[MOB.Guild] invalid) g_pGuildAlly[...] = 0;    // dead condition
Guild = (Guild && g_pGuildWar[Guild] && g_pGuildWar[g_pGuildWar[Guild]] == Guild) ? g_pGuildWar[Guild] : 0;
SendClientSignalParm3(conn, ESCENE_FIELD, _MSG_SendWarInfo, Guild, Clan, g_pGuildAlly[MOB.Guild]);
```
*Confidence: high.*

---

### Business Rule: BR-27 Party handshake projections

**Overview:**
Three functions project the party handshake: `SendReqParty(conn, Leaderconn, PartyID)` (invitation), `SendAddParty(Leaderconn, conn, PartyID)` (acceptance), and `SendRemoveParty(conn, connExit)` (removal). They share the conventions `ID = ESCENE_FIELD`, `Leaderconn = 30000` for a server-initiated party, and the magic `Target = 52428` (`0xCCCC`).

**Detailed description:**
`SendReqParty` (1438-1470) renders the *inviter* into a `MSG_SendReqParty`: class, level, MaxHp, Hp, `PartyPos = 0`, `PartyID` passed through, and the inviter's name copied with `strcpy` (not bounded) into a 16-byte field. The HP fields are sent raw, without the compression rule below, so 5-digit HP values are truncated by the `short` field.

`SendAddParty` (1472-1506) is the counterpart: if `PartyID == 0` the `Leaderconn` field carries the joining player's slot, otherwise the constant `30000`. HP and MaxHp are **compressed**: values above 32000 become `(value+1)/100`. This is the only numeric compression rule in the component and it is asymmetric — a client must know which message carries compressed values to decode correctly. `PartyID` is then set to `conn` (with a `//Check:` author comment signalling uncertainty), `Target` to `(short)52428` (0xCCCC, i.e. −13108 signed / 52428 unsigned), and the name `strcpy`'d.

`SendRemoveParty` (1508-1532) sends `Leaderconn = connExit` with `unk = 0`, so the removal notice identifies who left rather than who was notified.

The transport differs too: `SendReqParty` and `SendRemoveParty` use `AddMessage` (deferred flush), while `SendAddParty` uses `SendOneMessage` (`CPSock.cpp:686`, AddMessage + immediate `SendMessageA`). All three `CloseUser` on failure. Acceptance therefore jumps the queue ahead of deferred traffic — the handshake is deliberately prioritised.

**Rule workflow:**

```
SendReqParty   : gates; ID=ESCENE_FIELD; fill from pMob[Leaderconn]; strcpy name; AddMessage else CloseUser
SendAddParty   : gates on Leaderconn; Leaderconn = (PartyID==0) ? conn : 30000;
                 MaxHp/Hp = (>32000) ? (v+1)/100 : v; PartyID = conn; Target = 0xCCCC;
                 strcpy name; SendOneMessage else CloseUser
SendRemoveParty: gates; Leaderconn = connExit; unk = 0; AddMessage else CloseUser
```
*Confidence: high for mechanics; medium for the meaning of `Target = 0xCCCC` (inferred from the constant's reuse).*

---

### Business Rule: BR-28 Weather broadcast

**Overview:**
`SendWeather()` enqueues `MSG_UpdateWeather` with the global `CurrentWeather` to every playing user holding a live socket, closing any user whose enqueue fails.

**Detailed description:**
The message is built once (`Type = _MSG_UpdateWeather`, `ID = ESCENE_FIELD`, `CurrentWeather`) and enqueued for each eligible slot. The loop applies both BR-02 (`Mode == USER_PLAY`) and BR-03 (live socket) before touching the queue, which is the correct pairing and the reason this broadcast is comparatively cheap on dead slots. It performs **no** visibility, map, or distance filtering: weather is global in this implementation, which is consistent with the single global `CurrentWeather` variable that drives it.

A commented-out geographic filter survives at line 1582 — `if ((TargetX / 128) < 12 && (TargetY / 128) > 25)` — showing that per-map weather was considered (the same `/128` segmentation used by `MapaMulticast`) and abandoned in favour of a global value. Had it been enabled it would have been unsatisfiable in most cases, since `< 12` and `> 25` are mutually distant bands.

The failure policy is the strict variant: `if (!AddMessage(...)) CloseUser(i)`. Since weather changes are rare (driven by `ProcessSecMinTimer.cpp:2291,2296,2301,2309`), the strictness is affordable here but it means a transient queue overflow on a healthy client disconnects it — a notable asymmetry with `SendNotice`, which happily drops the same client's notices.

**Rule workflow:**

```
build MSG_UpdateWeather{Type, Size, ID=ESCENE_FIELD, CurrentWeather};
for i in [0,1000): if PLAY && Sock:  if (!AddMessage) CloseUser(i);
```
*Confidence: high.*

---

### Business Rule: BR-29 Map-scope multicast

**Overview:**
`MapaMulticast(tx, ty, m, bSend)` delivers to every playing user whose `TargetX / 128 == tx` **and** `TargetY / 128 == ty`, iterating slots from **1** (slot 0 excluded) and optionally flushing per recipient.

**Detailed description:**
The world's 4096×4096 coordinate space is partitioned into 32×32 blocks of 128×128 units; `(TargetX/128, TargetY/128)` is the map/zone cell index, so `MapaMulticast(9,1,...)` means "everyone in zone column 9, row 1". Call sites in `ProcessSecMinTimer.cpp` (1050, 1070, 1089, 1169) use concrete pairs such as `(9,1)`, `(8,2)`, `(10,2)` and `(6,28)` with a comment naming the region ("Pesa M"), confirming these are hard-coded region ids for scheduled area events.

The loop starts at `i = 1`, which is the explicit expression of the BR-01 "slot 0 is not addressable" invariant, and requires `Mode == USER_PLAY`. It does **not** check the socket, does not validate `m->Size`, and does not validate that `tx`/`ty` are in `[0,32)`. Two external call sites exist, both in the minute timer, both with `bSend = 0`.

The absence of a socket check means a slot in `USER_PLAY` with a torn socket will accumulate messages in its 128 KB buffer until `AddMessage` starts refusing them — for a repeated timer-driven event, this can fill an hour's worth of buffer with dead traffic.

**Rule workflow:**

```
for i in [1,1000):
    if (Mode == USER_PLAY && X/128 == tx && Y/128 == ty)
        { AddMessage(m, m->Size); if (bSend) SendMessageA(); }
```
*Confidence: high.*

---

### Business Rule: BR-30 Area message filter (defective predicate)

**Overview:**
`SendMessageArea(x1, y1, x2, y2, m, bSend)` is intended to deliver a pre-built envelope to all playing users inside the rectangle `[x1,x2] × [y1,y2]`. The implemented predicate compares the **Y** coordinate against the **x2** bound. The function also has **zero call sites**.

**Detailed description:**
The condition (1683) reads `pMob[i].TargetX >= x1 && pMob[i].TargetY <= x2 && pMob[i].TargetY >= y1 && pMob[i].TargetY <= y2`. Three of the four terms constrain Y; `TargetX` is only lower-bounded. The effect is a vertical band `Y ∈ [y1, min(x2, y2)]` with `X ≥ x1` — no upper X bound at all. For the intended callers (rectangles in the 1600-2700 × 1400-1900 region) this would have delivered to every player east of `x1` in a narrow Y band, i.e. a cross-map broadcast rather than a local one.

Because the function is never called, the defect is latent rather than active: the shipped server achieves area messaging through `SendNoticeArea` (BR-08, correct predicate on a text payload) and `GridMulticast` (BR-15/16, window predicate with the separate `&& tx` defect). Anyone wiring a new caller to `SendMessageArea` would inherit the broken filter silently, since there is no assertion or test to catch it.

The remainder of the function follows the `MapaMulticast` shape: `Mode == USER_PLAY` gate, `AddMessage(m, m->Size)`, optional per-recipient flush. No socket check, no `m->Size` validation.

**Rule workflow:**

```
for i in [0,1000):
    if (Mode == USER_PLAY && X >= x1 && Y <= x2 && Y >= y1 && Y <= y2)  // defect: X never upper-bounded
        { AddMessage(m, m->Size); if (bSend) SendMessageA(); }
```
*Confidence: high for the defect; high for the zero-caller finding.*

---

### Business Rule: BR-31 Grid-cell area signals

**Overview:**
`SendSignalParmArea` and `SendShortSignalParm2Area` walk a caller-supplied coordinate rectangle **cell by cell** (`pMobGrid[y][x]`), skip empty cells and non-player occupants, and emit a generic signal to each occupant.

**Detailed description:**
Unlike the 33×33 viewport functions, these two take the raw rectangle and iterate `x` in `[x1,x2)` outer, `y` in `[y1,y2)` inner — a column-major walk over an unclamped span. Bounds are enforced per cell (`x < 0 || x >= MAX_GRIDX || y < 0 || y >= MAX_GRIDY`), so an oversized rectangle costs iterations but cannot read out of bounds. Occupants are filtered by `tmob != 0`, `tmob < MAX_USER` and `pUser[tmob].Mode == USER_PLAY` — mobs are never addressed, which makes these the only grid-walk functions that explicitly exclude NPCs by index rather than by `Mode`.

Because they address grid cells rather than slots, an entity occupying several cells (a state the self-healing of BR-14 is designed to prevent) would receive duplicate signals; conversely, an entity whose grid entry is stale would be missed even if it is inside the rectangle. The slot-based area functions (BR-08, BR-30) do not have this failure mode, since they iterate authoritative positions.

Delivery is delegated to `SendClientSignalParm` / `SendClientSignalShortParm2`, which perform no validation, so the safety of the composite depends entirely on the filters above. `SendSignalParmArea` has 5 external call sites in 1 file; `SendShortSignalParm2Area` has 1 external call site — and that single caller is the only consumer of `SendClientSignalShortParm2`, so the "short parm" variant of the protocol is reachable through exactly one path.

**Rule workflow:**

```
for x in [x1,x2): for y in [y1,y2):
    if (x/y out of [0,4096)) continue;
    tmob = pMobGrid[y][x];
    if (tmob == 0 || tmob >= MAX_USER) continue;
    if (pUser[tmob].Mode != USER_PLAY) continue;
    SendClientSignalParm/ShortParm2(tmob, id, signal, parm…);
```
*Confidence: high.*

---

### Business Rule: BR-32 PK flag projection

**Overview:**
`SendPKInfo(conn, target)` tells client `conn` whether entity `target` should be rendered as "hostile/PK-enabled". The flag is `1` when `NewbieEventServer != 0`, or when any of `GetGuilty(target)`, `pUser[target].PKMode`, `RvRState`, `CastleState`, `GTorreState` is non-zero; otherwise `0`.

**Detailed description:**
This is a *visibility* rule, not a rules-of-engagement rule: it does not decide whether an attack is legal (that lives in the attack handler), it only decides whether the client draws the red-name affordance. `GetGuilty` (`GetFunc.cpp:2114`) reads the per-entity guilt counter; `PKMode` is the player's voluntary PK toggle (`CUser.h:89`); the three globals are server-wide event states (RvR, castle siege, G-Torre). Any one suffices, so during a global event every entity is flagged hostile regardless of individual guilt.

The newbie-event branch short-circuits the whole computation: when `NewbieEventServer != 0` the flag is unconditionally `1`, which is why newbie servers render everyone as attackable — a deliberate tutorial-arena behaviour that ignores guilt, PK mode and event state entirely.

Both arguments are validated (`0 < conn < MAX_USER`, `0 < target < MAX_USER`), so the payload can only describe a player, never an NPC — `pUser[target]` would otherwise index a mob id into the user array. `SendPKInfo` is called 4 times internally (SendGridMob 605, GridMulticast 781, 793) with the pair `(viewer, subject)`, so the pairing convention is `conn` = recipient, `target` = described entity. It has 2 external call sites. The `Size` field is set explicitly to `sizeof(MSG_STANDARDPARM)` while `Type` is `_MSG_PKInfo`, i.e. it reuses the parm envelope (BR-10) for a boolean.

**Rule workflow:**

```
if (conn or target out of (0, MAX_USER)) return;
sm.Type = _MSG_PKInfo; sm.ID = target;
if (NewbieEventServer == 0)
    Parm = (GetGuilty(target) || pUser[target].PKMode || RvRState || CastleState || GTorreState) ? 1 : 0;
else
    Parm = 1;
pUser[conn].cSock.AddMessage(&sm, sizeof(MSG_STANDARDPARM));
```
*Confidence: high.*

---

### Business Rule: BR-33 Affect projection and divine-timer decode

**Overview:**
`SendAffect(conn)` projects all 32 affect slots (`MAX_AFFECT = 32`) into `MSG_SendAffect`, converting the absolute-expiry encoding used by affect type 34 into a relative duration, with a one-hour collapse rule.

**Detailed description:**
The dual encoding exists because most affects are durations measured in server ticks, while one family (type 34, the "divine" buff) is stored as an absolute Unix expiry. The discriminator is `Type == 34 && Time >= 32000000` (1784) — 32,000,000 seconds is roughly one year, a magnitude no tick-count duration would reach, so the test reliably separates the encodings.

For an absolute entry, the remaining time is `pMob[conn].extra.DivineEnd - now`. If that remainder is **≤ 3600 seconds**, the entry is normalized *in place* to `Time = 450` and the payload carries 450 — presumably "less than an hour left, show the short form". Otherwise the remaining seconds are converted to the client's display unit by `((remaining / 60 / 60 / 24) * AFFECT_1D) - 1`, where `AFFECT_1D = 10800` (`Basedef.h:208`) is the per-day tick constant: whole days remaining × ticks-per-day, minus one tick. Sub-day remainders therefore round down to `-1`, i.e. the client receives a negative duration for a buff with 23 hours left — the sentinel presumably means "expiring".

The `else if (Type >= 1)` branch (1808) copies ordinary affects verbatim: Type, Value, Level, Time. Type 0 (empty slot) is skipped entirely, leaving the zeroed payload, so the client can distinguish "no affect" from "affect with value 0". All 32 slots are always transmitted (the message is fixed-size), so the client reconciles by full replacement rather than by delta.

`SendAffect` takes no `skip`, does no `Mode`/`Sock` check beyond the slot range, and is called both internally (from `SendScore`, 1192) and — in the current build — nowhere externally. Because `MSG_UpdateScore` already contains an `Affect[]` array populated by `GetAffect`, the affect state is transmitted twice per score update.

**Rule workflow:**

```
memset(&sm, 0, sizeof(MSG_SendAffect)); sm.Type=_MSG_SendAffect; sm.ID = conn;
for i in [0,32):
    a = pMob[conn].Affect[i];
    if (a.Type == 34 && a.Time >= 32000000) {
        remaining = pMob[conn].extra.DivineEnd - now;
        if (remaining <= 3600) { a.Time = 450; copy(a); continue; }
        copy(a, Time = (remaining/60/60/24)*AFFECT_1D - 1);
    } else if (a.Type >= 1) copy(a, verbatim);
pUser[conn].cSock.AddMessage(&sm, sizeof(MSG_SendAffect));
```
*Confidence: high for the mechanics; medium for the intended semantics of the `-1` and the `450` collapse.*

---

### Business Rule: BR-34 Back-pressure and enqueue-failure policy

**Overview:**
The only flow-control mechanism available is `CPSock::AddMessage` returning `FALSE` when `nSendPosition + Size >= SEND_BUFFER_SIZE` (128 KB) or when `Sock <= 0`. The component's response to that `FALSE` is not uniform: it is ignored, logged, or converted into a connection close depending on the function.

**Detailed description:**
The transport contract is narrow. `AddMessage` (`CPSock.cpp:513`) refuses the message when the buffer is nearly full or the socket is closed, logging `err,add buffer full ...` / `err,add buffer invalid ...` in both cases; it never blocks, never grows the buffer, and never drops an older message. `SendMessageA` (`CPSock.cpp:617`) makes exactly one `send()` attempt (the `for (int i = 0; i < 1; i++)` loop at 658 is a single iteration), advances `nSentPosition` by whatever was actually written, and only resets the buffer when everything drained — so a partial write is retained and retried on the next flush, driven by the socket's writability in the select loop. `RefreshSendBuffer` (605) compacts the unconsumed remainder. There is no per-slot queue limit other than the 128 KB byte budget, and no per-message limit other than `MAX_MESSAGE_SIZE = 8192` (which `AddMessage` does not enforce against `Size`).

Against that contract the component applies four distinct policies:

1. **Ignore** — the vast majority of builders discard the return value: `SendClientMessage` (44), `SendClientSignal*` (205, 217, 230, 244, 257), `SendCreateItem` (327), `SendRemoveMob` (504), `SendRemoveItem` (521), `SendChat`, `SendSay`, `SendItem` (1081), `SendEquip`, `SendWarInfo`, `SendPKInfo` (1766), `SendAffect` (1816), `SyncMulticast` (266), `SyncKingdomMulticast` (280), `MapaMulticast` (1668), `SendMessageArea` (1685), `PartyGridMulticast` (1021). Overflow is silent at this level; the only evidence is the transport's log line.
2. **Close the connection** — `SendAutoTrade`, `SendEtc`, `SendCargoCoin`, `SendShopList`, `SendReqParty`, `SendAddParty` (via `SendOneMessage`), `SendRemoveParty`, `SendCarry`, `SendWeather`, `SendSetHpMp`, `SendHpMode`, and the party-roster loop of `PartyGridMulticast` (1050). A full queue on a *single recipient* is treated as a dead or misbehaving client.
3. **Log with identity** — `GridMulticast`'s third pass (797-807) logs `err,gridmulticast add <slot>-<mode> <account>` and continues to the next recipient. This is the only place where a broadcast failure is attributed to a specific player.
4. **Log a generic error** — `SendAutoTrade` closes the user but nothing else logs; the `AddMessage`-internal log is the only trace.

The policy inconsistency is functional, not cosmetic: two clients with equally full buffers are treated differently depending on which message family filled them. And because `SendClientMessage` ignores failures, a flooding global notice on a congested shard produces only transport log lines, while the very same condition reached through `SendEtc` disconnects the player. Notably, no policy implements *deferral* — there is no retry queue, so a message refused by `AddMessage` is lost permanently.

**Rule workflow:**

```
AddMessage(buf, size):
    if (nSendPosition + Size >= 131072) { log "add buffer full"; return FALSE; }
    if (Sock <= 0)                     { log "add buffer invalid"; return FALSE; }
    stamp Size/KeyWord/CheckSum/ClientTick, obfuscate payload, advance nSendPosition; return TRUE;
caller policy: ignore (default) | if(!ok) CloseUser(conn) | if(!ok) Log(identity)
flush policy : SendMessageA one-shot send(); retain partial; RefreshSendBuffer on next call
```
*Confidence: high.*

---

### Business Rule: BR-35 Auto-trade projection (length mismatch)

**Overview:**
`SendAutoTrade(conn, otherconn)` copies the subject's stall struct into a local `MSG_SendAutoTrade`, patches `ID`, `Index` and the envelope, then enqueues it with `sizeof(MSG_UpdateCarry)` — 528 bytes — instead of `sizeof(MSG_SendAutoTrade)` (196 bytes).

**Detailed description:**
The gate stack is the complete BR-01/02 set applied to *both* slots (529-542): `conn` and `otherconn` must each be in `(0, MAX_USER)`, the subject must have `TradeMode != 0` and be `USER_PLAY`, and the recipient must be `USER_PLAY`. The payload is then built by `memcpy(&sm, &pUser[otherconn].AutoTrade, sizeof(MSG_SendAutoTrade))` (547) — a copy from `CUser::AutoTrade` (`CUser.h:65`) — followed by `sm.ID = ESCENE_FIELD`, `sm.Index = otherconn`, and the envelope fields.

`MSG_SendAutoTrade` is `_MSG` (12) + `Title[24]` + `Item[12]` (96) + `CarryPos[12]` + `Coin[12]` (48) + `Tax` (2) + `Index` (2) = 196 bytes. `MSG_UpdateCarry` is `_MSG` (12) + `Carry[64]` (512) + `Coin` (4) = 528 bytes. Passing 528 to `AddMessage` has three consequences, all observable in the transport code: the header's `Size` field is set to 528 (line 538) so the client is told a 528-byte message is coming; the obfuscation loop reads `pMsg[i]` for `i ∈ [4,528)` (558), i.e. 332 bytes beyond the 196-byte stack object; and those stack bytes are encrypted and transmitted to the client. The stack over-read is a memory-disclosure defect confined to the caller's own frame (the recipient is the client being served), and the `Size` mismatch guarantees the client receives a frame it cannot parse as either message type.

The failure branch is the strict policy: `if (!AddMessage(...)) CloseUser(conn)` (556-557).

This is the clearest instance in the component of a rule expressed in code but contradicted by the literal used, and it is worth noting that a compiler cannot catch it: both expressions are `sizeof` of POD structs, and the function takes `char*`.

**Rule workflow:**

```
if (conn invalid) return;  if (otherconn invalid) return;
if (pUser[otherconn].TradeMode == 0) return;
if (pUser[otherconn].Mode != USER_PLAY) return;  if (pUser[conn].Mode != USER_PLAY) return;
memcpy(&sm, &pUser[otherconn].AutoTrade, sizeof(MSG_SendAutoTrade));    // 196 bytes
sm.ID = ESCENE_FIELD; sm.Index = otherconn; sm.Type = _MSG_SendAutoTrade;
if (!AddMessage(&sm, sizeof(MSG_UpdateCarry))) CloseUser(conn);         // 528 bytes  <-- mismatch
```
*Confidence: high (both sizes are determinable from `Basedef.h`).*

---

## 4. Component Structure

```
Code/TMSrv/
├── SendFunc.h                          # 50 declarations, 4-line licence header, no includes/guards beyond __SENDFUNC__
│                                       #   -> depends on MSG_STANDARD / STRUCT_ITEM being already declared
└── SendFunc.cpp                        # 1,817 lines, 50 functions, 39,988 bytes
    │
    ├── [A] Panel / notice group (BR-04..BR-09)          lines  27-195   6 functions
    │   ├── SendClientMessage          27   primitive: panel text -> one slot
    │   ├── SendNotice                 47   global notice + logging + 'x suppression
    │   ├── SendNoticeChief            64   guild-master notice (ChargedGuildList scan)
    │   ├── SendSummonChief           103   guild-master teleport to castle seat
    │   ├── SendNoticeArea            155   inclusive rectangle notice
    │   ├── SendGuildNotice           167   guild-id equality notice
    │   └── SendClientMessageOk       179   modal message box (unused)
    │
    ├── [B] Generic signal group (BR-10)                 lines 197-258   5 functions
    │   ├── SendClientSignal          197   Type + ID
    │   ├── SendClientSignalParm      208   + Parm            (100 external call sites)
    │   ├── SendClientSignalParm2     220   + Parm1/Parm2     (never called)
    │   ├── SendClientSignalParm3     233   + Parm1..3        (internal: SendWarInfo)
    │   └── SendClientSignalShortParm2 247  + short Parm1/2   (internal: SendShortSignalParm2Area)
    │
    ├── [C] Exclusion multicast (BR-11, BR-12)           lines 260-286   2 functions
    │   ├── SyncMulticast             260   all users except sender
    │   └── SyncKingdomMulticast      274   + Clan == Kingdom && KingChat == 0
    │
    ├── [D] Entity appearance group (BR-13, BR-17, BR-21) lines 288-525  6 functions
    │   ├── SendCreateMob             288   MSG_CreateMob | MSG_CreateMobTrade (TradeMode==1)
    │   ├── SendCreateItem            320   GetCreateItem -> one slot
    │   ├── SendRemoveMob             494   Msg_RemoveMob with RemoveType
    │   └── SendRemoveItem            510   MSG_DecayItem, ItemID = 10000 + itemid
    │
    ├── [E] Chat / effect entry points (BR-15)           lines 333-492   5 functions
    │   ├── SendChat                  333   local chat -> 4-arg GridMulticast (skip sender)
    │   ├── SendSay                  1647   NPC speech -> 4-arg GridMulticast
    │   ├── SendEnvEffect             348   area effect centred via midpoint
    │   ├── SendEnvEffectKingdom      387   only if a non-clan member is in the rect
    │   └── SendEnvEffectLeader       433   only if no party leader is in the rect
    │
    ├── [F] Auto-trade and viewer bootstrap (BR-35, BR-14) lines 527-618  2 functions
    │   ├── SendAutoTrade             527   MSG_SendAutoTrade  (size mismatch, BR-35)
    │   └── SendGridMob               560   initial 33x33 populate on login/teleport
    │
    ├── [G] Core fan-out engine (BR-14..BR-19)           lines 620-1053  4 functions
    │   ├── GridMulticast(conn,tx,ty,msg)      620   3-pass visibility diff + grid repair (203 lines)
    │   ├── SendEmotion                        825   motion -> 4-arg GridMulticast
    │   ├── GridMulticast(tx,ty,msg,skip)      843   single-window fan-out + per-recipient rewrite (129 lines)
    │   └── PartyGridMulticast                 974   party-aware window fan-out (never called)
    │
    ├── [H] Character state projections (BR-22, BR-23)    lines 1055-1257 6 functions
    │   ├── SendItem               1055   one inventory slot
    │   ├── SendEquip              1084   16 slots + mount rules -> GridMulticast
    │   ├── SendScore              1136   full stat block + guild concealment -> GridMulticast
    │   ├── SendEtc                1195   exp / learn / coin / bonuses
    │   └── SendCargoCoin          1233   account coin via MSG_STANDARDPARM
    │
    ├── [I] Social / economy projections (BR-24..BR-27)   lines 1259-1532 6 functions
    │   ├── SendGuildList          1259   chunked roster + 2x65,535 war/ally scan
    │   ├── SendShopList           1369   27-slot flatten + village tax
    │   ├── SendWarInfo            1416   mutual-war resolve (mutates guild state)
    │   ├── SendReqParty           1438   invitation
    │   ├── SendAddParty           1472   acceptance (HP/100 compression, SendOneMessage)
    │   └── SendRemoveParty        1508   removal
    │
    ├── [J] Misc directed projections (BR-23, BR-28)      lines 1534-1645 5 functions
    │   ├── SendCarry              1534   64-slot inventory + coin (528 bytes)
    │   ├── SendWeather            1561   global weather broadcast
    │   ├── SendSetHpMp            1590   HP/MP + SetReqHp/SetReqMp
    │   ├── SendHpMode             1622   HP + session Mode
    │   └── SendSay                1647   (grouped with [E] semantically)
    │
    └── [K] Area/scoped fan-out and PK/affect (BR-29..BR-33) lines 1662-1817 6 functions
        ├── MapaMulticast          1662   /128 zone match, i from 1
        ├── SendMessageArea        1676   defective X predicate (never called)
        ├── SendSignalParmArea     1693   raw rect cell walk -> SendClientSignalParm
        ├── SendShortSignalParm2Area 1715 raw rect cell walk -> short parm
        ├── SendPKInfo             1737   hostile-rendering flag
        └── SendAffect             1769   32 affect slots + divine decode
```

**Include and build wiring.**

| Item | Evidence |
|------|----------|
| Compilation unit | `Code/TMSrv/TMSrv.vcxproj:106` — `<ClCompile Include="SendFunc.cpp" />` |
| Header wiring | `Code/TMSrv/TMSrv.vcxproj:183` — `<ClInclude Include="SendFunc.h" />` |
| Filter entries | `Code/TMSrv/TMSrv.vcxproj.filters:78` (cpp), `:305` (h) |
| Includes | `SendFunc.cpp:19-25` — `<memory>` (unused), `..\Basedef.h`, `SendFunc.h`, `GetFunc.h`, `Server.h`, `Language.h` |
| Preprocessor (Debug) | `_CRT_SECURE_NO_WARNINGS;_PACKET_DEBUG;WIN32;_DEBUG;_WINDOWS` (vcxproj:61) |
| Preprocessor (Release) | `_CRT_SECURE_NO_WARNINGS;WIN32;NDEBUG;_WINDOWS` (vcxproj:79) |
| `_PACKET_DEBUG` effect on this unit | none directly; enables `BASE_CheckPacket` logging inside `CPSock::AddMessage` (`CPSock.cpp:544-552`) for every message this unit emits |

---

## 5. Dependency Analysis

### 5.1 Internal dependency graph

```
                        ┌─────────────────────────────────────────────┐
                        │ 45 dependent translation units              │
                        │ (handlers, dispatchers, timers, Server.cpp) │
                        └───────────────────┬─────────────────────────┘
                                            │ SendFunc.h (50 symbols)
                                            ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│ SendFunc.cpp                                                                     │
│                                                                                  │
│  [A] notices ──► SendClientMessage ────────────────┐                             │
│  [C] exclusion ────────────────────────────────────┤                             │
│  [K] Mapa / Area ──────────────────────────────────┤                             │
│                                                    ▼                             │
│  [G] GridMulticast(5)  ──► SendRemoveMob / SendRemoveItem                        │
│          │            ──► SendCreateMob / SendCreateItem                         │
│          │            ──► SendPKInfo                                             │
│          │            ──► GetEmptyMobGrid ────────────────► GetFunc.cpp:1901     │
│          │                                                                       │
│          └──► commit LastX/LastY/TargetX/TargetY                                  │
│                                                                                  │
│  [G] GridMulticast(4)  ──► BASE_VisualItemCode ──────────► Basedef.cpp:6598       │
│          │            ──► BASE_VisualAnctCode ──────────► Basedef.cpp:6662       │
│          │            ──► CMob::CheckGetLevel ──────────► CMob.cpp:1034          │
│          │            ──► SetCircletSubGod ─────────────► Server.cpp:8855        │
│          │            ──► DoItemLevel ──────────────────► Server.cpp:8805        │
│          │            ──► GetPKPoint / SetPKPoint ──────► GetFunc.cpp:2105, 2145 │
│          │            ──► SendScore ──► SendAffect / SendEmotion                 │
│          │            ──► SendEtc                                                 │
│          │            ──► GetCreateMob ─────────────────► GetFunc.cpp:947        │
│          │            ──► g_pMessageStringTable ────────► Language.h             │
│          ▼                                                                       │
│   CPSock::AddMessage ──► CPSock.cpp:513  (header + obfuscation + 128KB queue)     │
│   CPSock::SendMessageA ──► CPSock.cpp:617                                         │
│   CPSock::SendOneMessage ──► CPSock.cpp:686  (only SendAddParty)                  │
│                                                                                  │
│  [H] SendEquip ──► BASE_Visual* ; [I] SendGuildList ──► BASE_GetGuildName         │
│  [I] SendShopList ──► BASE_GetVillage ──► Basedef.cpp:4258                        │
│  [J] SendSetHpMp ──► SetReqHp / SetReqMp ──► Server.cpp:8659, 8674                │
│  [K] SendPKInfo ──► GetGuilty ──► GetFunc.cpp:2114                                │
│  [D] SendCreateMob ──► GetCreateMobTrade ──► GetFunc.cpp:1072                     │
│  [D] SendCreateItem ──► GetCreateItem ──► GetFunc.cpp:1195                        │
│  [A] SendSummonChief ──► DoTeleport ──► Server.cpp:8212                           │
│  (all) CloseUser ──► Server.cpp:6834 ;  Log ──► Server.h:124                      │
└──────────────────────────────────────────────────────────────────────────────────┘
                                            │
                                            ▼
                           Winsock send() (CPSock.cpp:662)
```

### 5.2 Internal dependencies table

| Dependency | Kind | Provider | Used by (examples) | Coupling character |
|---|---|---|---|---|
| `CPSock::AddMessage` | object method | `Code/CPSock.cpp:513` | ~40 call sites in this unit | Hard, per-slot, single point of queueing |
| `CPSock::SendMessageA` | object method | `Code/CPSock.cpp:617` | 15 call sites | Hard; one-shot `send()` |
| `CPSock::SendOneMessage` | object method | `Code/CPSock.cpp:686` | `SendAddParty` (1504) | Hard |
| `pUser[MAX_USER]` | global array | `Server.h:64`, `Server.cpp` | every directed sender | **Hard, pervasive** (shared mutable state) |
| `pMob[MAX_MOB]` | global array | `Server.h:230` | every projection | **Hard, pervasive** |
| `pItem[MAX_ITEM]` | global array | `Server.h:235` | `GridMulticast`, `SendGridMob` | Hard |
| `pMobGrid[4096][4096]` | global array | `Server.h:54/309` | grid fan-out, `SendGridMob` | **Hard; written by this unit** |
| `pItemGrid[4096][4096]` | global array | `Server.h:55/310` | grid fan-out | Hard; written by this unit |
| `GetCreateMob` | function | `GetFunc.cpp:947` | `SendCreateMob`, `GridMulticast` | Contract: fills `MSG_CreateMob` |
| `GetCreateMobTrade` | function | `GetFunc.cpp:1072` | `SendCreateMob` | Contract |
| `GetCreateItem` | function | `GetFunc.cpp:1195` | `SendCreateItem` | Contract |
| `GetAffect` | function | `GetFunc.cpp:1174` | `SendScore` | Contract |
| `GetEmptyMobGrid` | function | `GetFunc.cpp:1901` | `GridMulticast(5)`, `SendGridMob` | **Mutates grid through out-params** |
| `GetGuilty` / `GetPKPoint` / `SetPKPoint` | functions | `GetFunc.cpp:2105-2145` | `SendPKInfo`, `GridMulticast(4)` | Hard |
| `SetCircletSubGod` / `DoItemLevel` | functions | `Server.cpp:8855`, `:8805` | `GridMulticast(4)` level-4 path | **Crosses into gameplay** |
| `SetReqHp` / `SetReqMp` | functions | `Server.cpp:8659`, `:8674` | `SendSetHpMp` | **Mutates user state** |
| `DoTeleport` | function | `Server.cpp:8212` | `SendSummonChief` | **Crosses into gameplay** |
| `CloseUser` | function | `Server.cpp:6834` | 12 call sites | **Destructive failure policy** |
| `Log` | function | `Server.h:124` | 6 call sites | Diagnostics |
| `BASE_VisualItemCode` / `BASE_VisualAnctCode` | functions | `Basedef.cpp:6598`, `:6662` | `SendEquip`, `GridMulticast(4)` | Encoding contract |
| `BASE_GetGuildName` | function | `Basedef.cpp:1314` | `SendGuildList` | Encoding contract |
| `BASE_GetVillage` | function | `Basedef.cpp:4258` | `SendShopList` | Domain rule |
| `CMob::CheckGetLevel` | object method | `CMob.cpp:1034` | `GridMulticast(4)` | **State machine, mutates level** |
| `ChargedGuildList` | global | `Basedef`/`Server` | `SendNoticeChief`, `SendSummonChief` | Read-only |
| `g_pGuildWar[65536]` / `g_pGuildAlly[65536]` | globals | `Server.h:51-52` | `SendGuildList`, `SendWarInfo` | Read + **written** by `SendWarInfo` |
| `g_pGuildZone[5]` | global | `Server.h:417` | `SendShopList` | Read-only |
| `g_pMessageStringTable[]` | global | `Language.h` | `GridMulticast(4)`, `SendGuildList` | Read-only |
| `BrState`, `RvRState`, `CastleState`, `GTorreState`, `NewbieEventServer`, `CurrentWeather`, `ServerGroup` | globals | `Server.h` | `SendScore`, `SendPKInfo`, `SendWeather`, `SendGuildList` | Read-only |
| `Pista[7]` | global | `Server.h:399` | `SendEnvEffectLeader` | Read-only |
| `temp[4096]` | global buffer | `Server.h:382`, `Server.cpp:277` | `GridMulticast(4)` logging | **Shared mutable scratch** |

### 5.3 External dependencies

| Dependency | Type | Purpose | Notes |
|---|---|---|---|
| Winsock 1.1 (`send`, `WSAGetLastError`) | OS library | Transmit framed bytes | Reached indirectly via `CPSock::SendMessageA` |
| C runtime (`memset`, `memcpy`, `strcpy`, `strcat`, `strlen`, `sprintf`, `rand`, `time`) | CRT | Buffer init, string assembly, key-word randomness, divine timer decode | `sprintf` used with caller-built format strings from `g_pMessageStringTable` |
| `<memory>` | C++ standard library | Declared but unused (`SendFunc.cpp:19`) | Zero functional dependency |

There are **no** third-party libraries, no database access, no network clients and no configuration files consumed by this component. It is purely in-process projection plus the CPSock queue.

---

## 6. Afferent and Efferent Coupling

Units of analysis are the 50 exported functions (C-style free functions; the codebase has no classes for protocol) plus the two files.

### 6.1 Per-function coupling

"Afferent" = external call sites in other translation units (this unit's own internal calls excluded). "Efferent" = distinct external symbols (functions/methods/globals outside `SendFunc.cpp`) the function touches.

| Component (function) | Afferent (external call sites) | Afferent (files) | Efferent | Critical | Notes |
|---|---|---|---|---|---|
| `SendClientMessage` | 637 | 45 | 2 | **High** | Universal text primitive; 17 internal uses too |
| `SendItem` | 381 | 27 | 2 | **High** | Inventory-slot projection used by nearly every handler |
| `SendClientSignalParm` | 100 | 17 | 1 | **High** | Generic parm signal; no validation |
| `SendScore` | 96 | 13 | 6 | **High** | 1,089-cell broadcast + `SendAffect` |
| `GridMulticast(tx,ty,msg,skip)` | 78 | 20 | 15 | **Critical** | Rewrites payloads, triggers level cascade |
| `SendEtc` | 53 | 21 | 3 | Medium | Widest file spread after `SendItem` |
| `SendSay` | 94 | 7 | 2 | Medium | Concentrated in `_MSG_Quest.cpp` (88) |
| `SendNotice` | 31 | 8 | 3 | High | Global broadcast + log + suppression |
| `SendEnvEffectLeader` | 43 | 1 | 4 | Medium | Region-event effect gate |
| `SendEnvEffect` | 27 | 2 | 2 | Medium | Midpoint approximation |
| `SendHpMode` | 26 | 23 | 1 | Medium | One line per handler; used as "you are playing" nudge |
| `SendClientSignal` | 20 | 8 | 1 | Medium | Envelope only |
| `SendSetHpMp` | 16 | 6 | 4 | Medium | Recomputes `ReqHp/ReqMp` |
| `SendNoticeArea` | 14 | 3 | 2 | Medium | Inclusive rectangle |
| `SendCarry` | 12 | 4 | 2 | Medium | 528-byte message |
| `SendAddParty` | 11 | 2 | 3 | Low | `SendOneMessage` (immediate flush) |
| `SendEquip` | 9 | 8 | 4 | Medium | Mount rules; no `conn` gate |
| `SendEnvEffectKingdom` | 3 | 1 | 3 | Low | Clan-exclusion gate |
| `SendGuildNotice` | 7 | 1 | 2 | Low | Guild equality |
| `SendRemoveMob` | 5 | 2 | 3 | Medium | Visibility diff primitive |
| `SendWeather` | 4 | 2 | 2 | Low | Global broadcast |
| `SendWarInfo` | 4 | 2 | 4 | **High** | **Mutates guild state** |
| `SendRemoveParty` | 3 | 1 | 3 | Low | |
| `SendCargoCoin` | 2 | 3 | 3 | Low | Account coin ≠ character coin |
| `SendGridMob` | 2 | 2 | 5 | High | Reads `pMob[conn]` before validating `conn` |
| `SendAutoTrade` | 2 | 2 | 3 | **High** | **Length mismatch over-read (BR-35)** |
| `SendShopList` | 1 | 1 | 5 | Medium | No `MobIndex` validation |
| `MapaMulticast` | 1 | 1 | 2 | Medium | 4 timer call sites |
| `SendChat` | 1 | 1 | 2 | Low | |
| `SendReqParty` | 1 | 1 | 3 | Low | |
| `SendGuildList` | 1 | 1 | 5 | **High** | 2 × 65,535 scan per call |
| `SendSummonChief` | 1 | 1 | 4 | Medium | `return` inside loop aborts broadcast |
| `SendNoticeChief` | 1 | 1 | 4 | Medium | Dead `FoundCharged` |
| `SyncMulticast` | 1 | 2 | 2 | Medium | 999-recipient exclusion |
| `SyncKingdomMulticast` | 1 | 1 | 2 | Low | |
| `SendRemoveParty` | 3 | 1 | 3 | Low | |
| `SendShortSignalParm2Area` | 1 | 1 | 2 | Low | Only consumer of `SendClientSignalShortParm2` |
| `SendSignalParmArea` | 4 | 1 | 2 | Low | Raw rect walk |
| `SendCreateMob` | 1 | 1 | 4 | **High** | `bSend` dead → unconditional flush |
| `SendPKInfo` | 2 | 2 | 3 | Low | |
| `SendEnvEffectLeader` | 43 | 1 | 4 | Medium | |
| `GridMulticast(conn,tx,ty,msg)` | 1 | 1 | 8 | **Critical** | Grid mutation + 3 passes + `MSG_Action` punning |
| `SendEmotion` | 89 | 8 | 2 | Medium | Nested fan-out |
| `PartyGridMulticast` | **0** | 0 | 5 | Low | **Dead** |
| `SendMessageArea` | **0** | 0 | 2 | Low | **Dead + defective predicate** |
| `SendClientMessageOk` | **0** | 0 | 1 | Low | **Dead** |
| `SendClientSignalParm2` | **0** | 0 | 1 | Low | **Dead** |
| `SendClientSignalParm3` | 0 | 0 | 1 | Low | Internal only (`SendWarInfo`) |
| `SendClientSignalShortParm2` | 0 | 0 | 1 | Low | Internal only |
| `SendCreateItem` | 0 | 0 | 3 | Medium | Internal only (2 callers) |
| `SendRemoveItem` | 0 | 0 | 2 | Medium | Internal only (2 callers) |
| `SendAffect` | 0 | 0 | 2 | Low | Internal only (`SendScore`) |
| `SendRemoveParty` | 3 | 1 | 3 | Low | |
| `SendCarry` | 12 | 4 | 2 | Medium | |

*Note: file-level afferent was measured by scanning every `.cpp`/`.h` in `Code/` except `SendFunc.cpp`/`SendFunc.h` for `\bName\s*\(`. "Critical" reflects blast radius and state-mutation/safety risk, not code volume.*

### 6.2 File-level coupling

| Component | Afferent (Ca) | Efferent (Ce) | Instability Ce/(Ca+Ce) | Critical |
|---|---|---|---|---|
| SendFunc Outbound Projection Layer (`SendFunc.cpp`) | ~59 (matches architecture report) | 5 (`Basedef.h`, `GetFunc.h`, `Server.h`, `Language.h`, `SendFunc.h`) | 0.08 | **High** |
| SendFunc.h (contract) | ~45 TU | 0 (relies on caller's prior includes) | 0.00 | Medium |
| CPSock (downstream) | ~40 | 2 | 0.05 | **High** |
| GetFunc (upstream contract) | — | — | — | Medium |

The layer is **strongly stable** in Martin's sense: many dependents, few dependencies, low instability. That stability is purchased by total reliance on global mutable state — the `Ce = 5` figure understates real coupling because `Server.h` transitively brings `Basedef.h`, `CPSock.h` and every global array into the unit. Cohesion is *functional* (all 50 functions project outbound state) but not *structural*: the file mixes header construction, geometry, fan-out policy, gameplay side effects (level-up, PK, teleport), string chunking and trade/normalization rules.

---

## 7. Endpoints

This component **exposes no network endpoints**. It is outbound-only: it contains no listener, no message dispatcher and no request/response contract. Inbound routing belongs to `ProcessClientMessage.cpp` and `ProcessDBMessage.cpp`; the actual listener is `CPSock::StartListen` (`CPSock.cpp:128`) bound by `Server.cpp` on TCP 8281.

For completeness, the outbound wire surface emitted by this component is:

| Wire type | Constant | Struct size (bytes) | Emitted by |
|---|---|---|---|
| `_MSG_MessagePanel` (1) | `Basedef.h:1194` | 12 + 128 = 140 | `SendClientMessage`, `SendGuildList` |
| `_MSG_MessageBoxOk` (2) | `:1201` | 12 + 8 + 96 = 116 | `SendClientMessageOk` |
| `_MSG_MessageChat` (51) | `:1439` | 12 + 96 = 108 | `SendChat`, `SendSay` |
| `_MSG_UpdateCarry` (133) | `:1455` | 12 + 512 + 4 = 528 | `SendCarry` |
| `_MSG_UpdateScore` (54) | `:1466` | packed; header + `STRUCT_SCORE` + affects | `SendScore` |
| `_MSG_UpdateEtc` (55) | `:1494` | header + 64-bit exp/learn | `SendEtc` |
| `_MSG_CNFMobKill` (56) | `:1511` | header + Hold + ids + 64-bit Exp | `GridMulticast` (caller-built) |
| `_MSG_UpdateCargoCoin` (57) | `:1523` | 16 (`MSG_STANDARDPARM`) | `SendCargoCoin` |
| `_MSG_CreateMobTrade` (99) | `:1525` | header + visuals + Desc[24] | `SendCreateMob` |
| `_MSG_CreateMob` (100) | `:1553` | header + visuals + Hold | `SendCreateMob`, `GridMulticast` |
| `_MSG_RemoveMob` (101) | `:1582` | 16 | `SendRemoveMob` |
| `_MSG_PKInfo` (102) | `:1590` | 16 (`MSG_STANDARDPARM`) | `SendPKInfo` |
| `_MSG_Motion` (106) | `:1710` | 20 | `SendEmotion` |
| `_MSG_UpdateEquip` (107) | `:1720` | header + 16×2 + 16 | `SendEquip` |
| `_MSG_CreateItem` (110) | `:1592` | header + coords + item | `SendCreateItem` |
| `_MSG_DecayItem` (111) | `:1609` | 16 | `SendRemoveItem` |
| `_MSG_ShopList` (124) | `:1650` | header + 27×8 + tax = 244 | `SendShopList` |
| `_MSG_CNFAddParty` (125) | `:1861` | header + 5 shorts + name + target | `SendAddParty` |
| `_MSG_RemoveParty` (126) | `:1898` | 16 | `SendRemoveParty` |
| `_MSG_SendReqParty` (127) | `:1878` | header + class/pos + 4 shorts + name | `SendReqParty` |
| `_MSG_SetHpMp` (129) | `:1659` | 12 + 16 = 28 | `SendSetHpMp` |
| `_MSG_SendItem` (130) | `:1672` | 12 + 4 + 8 = 24 | `SendItem` |
| `_MSG_SendAutoTrade` (151) | `:1908` | 196 | `SendAutoTrade` |
| `_MSG_EnvEffect` (162) | `:2140` | 24 | `SendEnvEffect*` |
| `_MSG_SendWarInfo` (168) | `:2157` | 24 (`MSG_STANDARDPARM3`) | `SendWarInfo` |
| `_MSG_SendAffect` (185) | `:2186` | header + 32 × `STRUCT_AFFECT` | `SendAffect` |
| `_MSG_UpdateWeather` (—) | `:1794` | header + weather | `SendWeather` |
| `_MSG_SetHpMode` (—) | `:1812` | header + hp + mode | `SendHpMode` |
| generic | `MSG_STANDARD` / `PARM` / `PARM2` / `PARM3` / `SHORTPARM2` | 12 / 16 / 20 / 24 / 16 | `SendClientSignal*` |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| `CPSock::AddMessage` | In-process transport API | Enqueue one framed message into the per-slot 128 KB buffer | Direct call | `char*` + explicit `Size` | Returns `FALSE` on `nSendPosition + Size >= 131072` or `Sock <= 0`; caller policy varies (BR-34) |
| `CPSock::SendMessageA` | In-process transport API | Flush the buffer with one `send()` | Direct call | — | Single attempt; on failure retains `nSentPosition` and retries on next flush; resets on `Sock <= 0` |
| `CPSock::SendOneMessage` | In-process transport API | AddMessage + SendMessageA in one call | Direct call | `char*` + `Size` | Returns `SendMessageA` result; `SendAddParty` closes on failure |
| W2 game client (TCP 8281) | External client | Receives all projected `MSG_*` frames | Custom binary over TCP with XOR-style obfuscation and checksum | `HEADER` (short Size, char KeyWord, char CheckSum, short Type, short ID, uint ClientTick) + payload | None at this layer; malformed frames are a client-side issue |
| `pMobGrid` / `pItemGrid` | Shared global state (16.7 M cells each) | Visibility source of truth; **written** during fan-out | Direct memory | `unsigned short` cell values | Self-healing repair with `Log` diagnostics (BR-14) |
| `pUser` / `pMob` / `pItem` | Shared global state | Payload source; `pUser[i].cSock` is the transport handle | Direct memory | Class instances | Read-mostly, except `SendWarInfo`'s guild normalization |
| `CMob::CheckGetLevel` | Gameplay subsystem | Level/segment progression evaluation | Direct call | Returns 0..4 | Mutates level, HP/MP, bonuses; no failure return |
| `GetFunc` family (`GetCreateMob`, `GetCreateMobTrade`, `GetCreateItem`, `GetAffect`, `GetEmptyMobGrid`, `GetGuilty`, `GetPKPoint`, `SetPKPoint`) | Gameplay/encoding subsystem | Fill payload structs; mutate grid, PK points | Direct call | Out-params | No error signalling |
| `Server.cpp` helpers (`CloseUser`, `DoTeleport`, `SetReqHp`, `SetReqMp`, `SetCircularSubGod`, `DoItemLevel`, `Log`) | Gameplay infrastructure | Failure policy, teleport, regeneration thresholds, level rewards, logging | Direct call | `void`/`void*` | `CloseUser` is irreversible (closes socket, clears grid) |
| `Basedef.cpp` helpers (`BASE_VisualItemCode`, `BASE_VisualAnctCode`, `BASE_GetGuildName`, `BASE_GetVillage`) | Domain contract | Visual encoding, guild name resolution, village lookup | Direct call | Out-params / return | `BASE_GetGuildName` handles out-of-range groups by writing an empty string |
| `g_pMessageStringTable[]` | Localization table | Localized notice text | Direct memory | `char*[]` | Index assumed in range; no bounds check at call sites in this unit |
| `g_pGuildWar[65536]` / `g_pGuildAlly[65536]` / `g_pGuildZone[5]` / `ChargedGuildList[10][5]` | Guild world state | War/ally relations, city tax, castle ownership | Direct memory | `unsigned short` / structs | Read-only except the dead normalization in `SendWarInfo` |
| Event-state globals (`BrState`, `RvRState`, `CastleState`, `GTorreState`, `NewbieEventServer`, `CurrentWeather`) | Event state | Visibility/flag decisions | Direct memory | `int` | None |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Adapter / Projection | Each `SendX` converts domain objects to one wire struct | whole file | Decouples internal representation from protocol layout |
| Facade | 50 flat C functions hide CPSock, encoding, and fan-out | `SendFunc.h` | One-call API for 45 translation units |
| Parameter Object | `MSG_STANDARD *m` passed pre-built to 5 multicast functions | 260, 274, 662, 1676 | Avoid rebuilding the envelope per recipient |
| Fan-out / in-process publish | `for i in [0,MAX_USER)` and 33×33 grid sweeps | 57, 262, 875, 1664 | Broadcast without an event bus |
| Spatial index (implicit) | `pMobGrid[4096][4096]`, `pItemGrid[][]` | `Server.h:54-55` | O(1) occupancy lookup for the visibility window |
| Spatial partitioning (implicit) | coordinate `/128` map segmentation | 1666 | Zone-scoped fan-out |
| Object Pool / fixed arrays | `pUser[1000]`, `pMob[25000]`, `pItem[5000]` with `Mode` flags | `Server.h` | No dynamic allocation on the hot path |
| Envelope / Header Object | `_MSG` macro shared by every struct | `Basedef.h:925` | Uniform 12-byte framing |
| Null Object | `MOB_EMPTY = 0`, `pMobGrid == 0`, affect `Type == 0` | `CMob.h:26`, `Server.cpp` | Absence represented by zero rather than pointers |
| Marker / sentinel constants | `ESCENE_FIELD = 30000`, `Target = 0xCCCC`, `Special = 0xCC` | `Basedef.h:170`, 1500, 1166 | Out-of-band protocol signals |
| Guard Clause | Early `return` on range/mode/socket failures | 29, 290, 1057, 1371, 1739 | Flattened validation |
| Template Method (informal) | `memset` → set `Type` → set `Size` → fill → `AddMessage` | every builder | Uniform construction sequence |
| Self-healing state | grid cells zeroed when they point at dead entities | 601, 614, 766, 788 | Eventual consistency of the spatial index |
| Defensive double-pass | recompute clamp after `GetEmptyMobGrid` | 631-657 | Tolerates a corrupted grid |
| Fire-and-forget with implicit retry | one `send()`; partial writes retained | `CPSock.cpp:658-683` | Non-blocking I/O on the game thread |
| Lazy flush | `bSend` flag defers `SendMessageA` | 260, 320, 494, 510 | Batch writes per tick |

**Architectural observations.**

* Layering is conventional only. The nominal stack is handlers → `SendFunc` → `CPSock`, but three functions reach *backwards* into gameplay (`CheckGetLevel`, `SetPKPoint`, `SetReqHp`) and one writes shared guild state (`SendWarInfo`), so the "outbound adapter" also acts as a gameplay participant.
* The component is the de-facto protocol contract holder: 26 struct layouts and 5 envelope shapes are instantiated here, and `SendFunc.h` exposes all of them to 45 units without any type-safety wrapper.
* There is no abstraction for "recipient set". Slot loops, grid sweeps and equalities are open-coded at 20+ sites with subtly different gates — the root cause of the BR-01/02/03 inconsistencies.
* Construction is stack-based and `memset`-first everywhere, so there is no allocation churn; the cost model is dominated by loop iteration counts and syscalls, not allocation.
* `#pragma pack(push,1)` is applied to `MSG_UpdateScore` only (`Basedef.h:1465-1492`); all other structs use natural alignment. Wire compatibility therefore depends on the compiler's default packing (8 on MSVC x86) and on every field being naturally aligned — a fragile invariant for a protocol contract.

---

## 10. Technical Debt & Risks

| Risk | Component Area | Issue | Impact |
|---|---|---|---|
| **High** | `SendAutoTrade` (556) | `AddMessage` called with `sizeof(MSG_UpdateCarry)` (528) for a 196-byte `MSG_SendAutoTrade` | 332-byte stack over-read obfuscated and transmitted; wrong `Size` header; client cannot parse the frame |
| **High** | `SendClientMessage` (39), `SendChat` (343), `SendSay` (1656) | Unconditional 96-byte `memcpy` from caller strings of unknown length | Over-read past short string literals into adjacent memory, transmitted to clients |
| **High** | `SendClientMessageOk` (189) | 96-byte `memcpy` into a 96-byte field with no forced termination | Unterminated string at the client boundary (contrast with 41-42) |
| **High** | `GridMulticast(5)` (716, 737) | `\|\|`/`&&` precedence defect: `y >= ty2 && tx` | Northward visibility exits are not detected when `tx == 0`; stale entities never removed from clients in the map's west column |
| **High** | `SendMessageArea` (1683) | `TargetY <= x2` instead of `TargetX <= x2`; no upper X bound | Area messaging degenerates to a cross-map band (latent: zero callers) |
| **High** | `GridMulticast(5)` (813-822) | Blind cast of any `MSG_STANDARD*` to `MSG_Action*` and read of `ClientTick/Speed/PosX/PosY` | Type-unsafe read of unrelated payload fields; writes them into `pMob[conn].LastTime/LastSpeed/LastX/LastY` |
| **High** | `GridMulticast(5)` (657) | `pMobGrid[ty][tx] = conn` before any `tx`/`ty` validation | Out-of-bounds global write if a caller skips coordinate validation (caller obligation, `_MSG_Action.cpp:192`) |
| **High** | `GridMulticast(4)` (920-965) | Gameplay cascade (level-up, PK +5, item level) executed per recipient inside a broadcast loop, with nested `GridMulticast` calls | Silent progression from unrelated events; O(N²) work; re-entrant iteration over `pMobGrid` |
| **High** | `SendWarInfo` (1421-1428) | Send function writes `pMob[].MOB.Guild`, `g_pGuildWar[]`, `g_pGuildAlly[]`; the array normalizations are unreachable (`unsigned short` vs `65536`) | Hidden side effects; dead defensive code |
| **High** | `SendGuildList` (1333-1366) | Two full scans of 65,535 guild ids per call, on the guild-chat path | ~131k global reads per chat line on the game thread |
| **High** | `SendCreateMob` (308-317) | `bSend` parameter never read; flush is unconditional | Up to 1,089 `send()` syscalls per grid broadcast |
| **High** | `SendScore` (1190) | Called ~96 times, each a 1,089-cell broadcast + `SendAffect` | Quadratic amplification under combat load |
| Medium | `SendNoticeChief` (85-99) | `FoundCharged` computed, never used; inner `break` only exits `k` | 50,000 wasted comparisons per notice; wrong audience (all guild masters) |
| Medium | `SendSummonChief` (140-141) | `return` inside the `for` loop aborts the whole broadcast | One uncharged leader suppresses all summons |
| Medium | `SendSummonChief` (125-138) | No `break` out of `j`; last match wins; `break` exits only `k` | Non-obvious tie-break for multi-zone guilds |
| Medium | `SendGridMob` (562-566) | `pMob[conn].TargetX/Y` read **before** the `conn` range check | OOB read on invalid slot |
| Medium | `SendClientSignal*` (197-258), `SendRemoveMob`, `SendRemoveItem`, `SendCreateItem` | No slot validation at all | OOB global access if a caller passes an unvalidated slot |
| Medium | `SendEquip` (1097), `SendScore` (1145), `SendShopList` (1398), `SendGuildList` (1261), `SendWarInfo` (1421) | Dereference `pMob[conn]`/`pMob[MobIndex]` with no bounds check | Depends entirely on caller validation (`_MSG_REQShopList.cpp:33` is the only explicit one) |
| Medium | BR-15 border clamp (670-674, 856-868) | `StartX = 0` without compensating `SizeX` (and the `SizeX + StartX` compensation is a no-op) | Viewport shrinks to 17 cells at the west/south map edges |
| Medium | `SendNotice` (44 → `CPSock.cpp:527`) | Ignores `AddMessage` failure and performs no socket check | Log spam and wasted buffer writes for dead slots on every global notice |
| Medium | `GridMulticast(4)` (968) | `AddMessage` return value ignored (contrast pass 3 at 797) | Silent loss of the caller's message for congested recipients |
| Medium | `SendEnvEffectLeader` (469-484) | `Pista[4].Party[1].LeaderID` tested twice; parties 2..N never tested | Gate is wrong for >2-member parties; 1-file coupling |
| Medium | `SendEnvEffectKingdom` (405-427) | Counts non-clan users, then broadcasts to the *whole* area including clan members | Clan members receive effects intended to be excluded |
| Medium | `SendAddParty` (1496-1497) vs `SendReqParty` (1459-1460) | Asymmetric HP encoding (÷100 only in one message) | Client must special-case decoding per message |
| Medium | `MSG_UpdateScore` packed, all others natural-aligned | Mixed packing discipline on a binary protocol | Any struct edit silently changes the wire layout |
| Medium | `GridMulticast(4)` (806-807) | Forces `AccountName[14..15] = 0` to sanitize a log line | Mutates persistent account data during error logging |
| Low | Dead code: `PartyGridMulticast` (974), `SendMessageArea` (1676), `SendClientMessageOk` (179), `SendClientSignalParm2` (220) | 0 call sites each | Maintenance burden; defective code can be wired in later |
| Low | `SendNoticeChief` lacks the `'x` suppression of `SendNotice` | Inconsistent moderation control | Suppressed text can leak via the chief channel |
| Low | `SendChat`/`SendEnvEffect`/`SendCargoCoin` set `Size` twice (341, 356, 1248-1249) | Copy/paste artefact | None functional |
| Low | `SendShopList` (1392-1393) | `tx`/`ty` computed, never used | Dead computation |
| Low | `<memory>` include (19) | Unused | Build time only |
| Low | `SendFunc.h` has no includes | Relies on caller including `Basedef.h` first | Header is not self-contained |
| Low | `SendScore` (1152) `memcpy` of `STRUCT_SCORE` into a packed struct member | Relies on identical layout and no padding in `STRUCT_SCORE` | Any future padding change breaks the score block |
| Low | `GridMulticast(4)` `int Unk = 0;` (886, 1011) | Dead variable in the `MOB_EMPTY` branch | Signals an incomplete branch |
| Low | `SendAffect` (1806) | `(remaining/60/60/24)*AFFECT_1D - 1` yields `-1` for sub-day remainder | Negative duration sent to the client |

---

## 11. Test Coverage Analysis

### 11.1 Test inventory: none

**No test files exist for this component, and no test framework exists anywhere in the repository.** The evidence is exhaustive rather than a spot check:

| Probe | Command / location | Result |
|---|---|---|
| Test-named files across the repo (excluding ignored folders) | `Get-ChildItem -Recurse -File -Include *test*,*Test*,*spec*,*.py,*.ps1,*.sh,*.bat,*.yml,*.yaml` | Only documentation `.md` files and two report trees; **zero** executables or test sources |
| Solution contents | `W2PP Code Project.sln` | 2 projects: `DBSrv`, `TMSrv`. No test project |
| TMSrv project contents | `Code/TMSrv/TMSrv.vcxproj` | 87 source/header entries; no test target, no `AdditionalDependencies` for a test harness |
| Build targets | `Code/TMSrv/TMSrv.vcxproj:61,79` | Configurations are `Debug|Win32` and `Release|Win32` only; no `Test` configuration |
| Third-party test frameworks | search for `gtest`, `catch`, `cppunit`, `doctest`, `boost/test` across `Code/` | Not present; the only external dependencies are Winsock 1.1 and the CRT (see `docs/reports/dependency-auditor/`) |
| Test fixtures / mocks / golden files | search for `fixture`, `mock`, `stub`, `golden` in `Code/` | None |
| CI configuration | search for `.github`, `azure-pipelines`, `appveyor`, `.gitlab-ci` | None present at the project root |
| Project documentation | `README.md`, `docs/reports/MANIFEST.md` | No test instructions or coverage claims |
| Call-graph evidence | codegraph blast-radius on 59 of the component's symbols | Repeatedly reports "no tests found within 3 caller hops" for every symbol in the unit |

### 11.2 Coverage assessment by component area

| Component area | Unit tests | Integration tests | Coverage | Test quality | Risk |
|---|---|---|---|---|---|
| Slot-range/mode/socket gates (BR-01..03) | 0 | 0 | 0% | Not exercised | Medium — 45 units depend on the contract |
| Panel encoding + termination (BR-04) | 0 | 0 | 0% | Not exercised | **High** — over-read and unterminated-string paths |
| Notice fan-out + suppression (BR-05..09) | 0 | 0 | 0% | Not exercised | Medium |
| Generic signal envelopes (BR-10) | 0 | 0 | 0% | Not exercised | Medium |
| Exclusion / kingdom multicast (BR-11..12) | 0 | 0 | 0% | Not exercised | Low |
| Mob appearance + trade substitution (BR-13) | 0 | 0 | 0% | Not exercised | **High** — unconditional flush |
| Grid invariant + repair (BR-14) | 0 | 0 | 0% | Not exercised | **High** — writes global grid |
| Viewport geometry + borders (BR-15) | 0 | 0 | 0% | Not exercised | Medium — arithmetic defect at edges |
| Visibility delta, 3 passes (BR-16) | 0 | 0 | 0% | Not exercised | **Critical** — set semantics unverified |
| Item visibility gate (BR-17) | 0 | 0 | 0% | Not exercised | Low |
| Per-recipient rewriting (BR-18) | 0 | 0 | 0% | Not exercised | **High** — mutates the caller's buffer |
| Level-segment cascade (BR-19) | 0 | 0 | 0% | Not exercised | **Critical** — gameplay + nested fan-out |
| Party multicast (BR-20) | 0 | 0 | 0% (dead) | Not exercised | Low |
| Equip / mount encoding (BR-21) | 0 | 0 | 0% | Not exercised | Medium — bit-packed levels |
| Score + guild concealment (BR-22) | 0 | 0 | 0% | Not exercised | **High** — 96 call sites |
| Character facets (BR-23) | 0 | 0 | 0% | Not exercised | Medium |
| Guild roster chunking (BR-24) | 0 | 0 | 0% | Not exercised | **High** — 131k-iteration scan |
| Shop list + tax (BR-25) | 0 | 0 | 0% | Not exercised | Medium — index arithmetic |
| War info normalization (BR-26) | 0 | 0 | 0% | Not exercised | **High** — mutates guild state |
| Party handshake (BR-27) | 0 | 0 | 0% | Not exercised | Medium — asymmetric compression |
| Weather broadcast (BR-28) | 0 | 0 | 0% | Not exercised | Low |
| Map-scope multicast (BR-29) | 0 | 0 | 0% | Not exercised | Low |
| Area message filter (BR-30) | 0 | 0 | 0% (dead) | Not exercised | Medium — defective if revived |
| Grid-cell area signals (BR-31) | 0 | 0 | 0% | Not exercised | Low |
| PK flag (BR-32) | 0 | 0 | 0% | Not exercised | Low |
| Affect + divine decode (BR-33) | 0 | 0 | 0% | Not exercised | Medium — negative durations |
| Back-pressure policy (BR-34) | 0 | 0 | 0% | Not exercised | **High** — silent loss and spurious disconnects |
| Auto-trade projection (BR-35) | 0 | 0 | 0% | Not exercised | **Critical** — 332-byte over-read |

### 11.3 Testability constraints (observed, not recommendations)

* Every projection reads `pUser`/`pMob`/`pItem`/`pMobGrid` globals defined in `Server.cpp`, so a unit test would need to link the entire TMSrv translation set or re-declare the arrays.
* `pUser[].cSock` is a concrete `CPSock` by value (`CUser.h`), not an interface — there is no seam for a capture double, and the 128 KB buffers are `malloc`ed in the constructor.
* Fan-out entry points have side effects beyond I/O (`pMobGrid` writes, `TargetX/Y` commit, `CheckGetLevel`, `SetPKPoint`, `SetReqHp`), so a "pure send" assertion is not expressible without stubbing gameplay.
* `GridMulticast`'s behaviour depends on the 16.7 M-cell globals being pre-populated, i.e. on world simulation state; the smallest meaningful fixture is a room with entities and items.
* Failures surface only through `Log(...)` (a `void` function writing to a file) and through `CloseUser`, i.e. there is no error channel to assert on.
* Two exported functions are unreachable (0 callers), so any coverage number for the unit is capped below 100% by construction.

---

## Appendix A — Verification notes

* All line references were read directly from the on-disk files at analysis time: `Code/TMSrv/SendFunc.cpp` (1,817 lines), `Code/TMSrv/SendFunc.h` (73 lines), `Code/CPSock.cpp` (694+ lines), `Code/CPSock.h`, `Code/Basedef.h` (2,505 lines), `Code/TMSrv/CMob.cpp`, `Code/TMSrv/CMob.h`, `Code/TMSrv/CUser.h`, `Code/TMSrv/Server.h`, `Code/TMSrv/_MSG_Action.cpp`, `Code/TMSrv/_MSG_REQShopList.cpp`, `Code/TMSrv/MobKilled.cpp`, `Code/TMSrv/ProcessSecMinTimer.cpp`, `Code/TMSrv/_MSG_MessageWhisper.cpp`.
* Coupling counts were produced by a case-sensitive word-boundary scan (`\bName\s*\(`) over every `.cpp`/`.h` in `Code/` excluding `SendFunc.cpp`/`SendFunc.h`. Counts are call-site counts, not distinct caller functions; a single function may contribute many.
* Struct sizes were derived arithmetically from the declarations in `Basedef.h` under MSVC x86 default packing, except `MSG_UpdateScore` which is explicitly `#pragma pack(1)`.
* Where intent is inferred rather than stated (e.g. the purpose of `'x` suppression, the semantic of `Target = 0xCCCC`, the intended predicate of `FoundCharged`), the report marks it as inferred and does not treat it as established behaviour.
* No project file was modified during this analysis.
