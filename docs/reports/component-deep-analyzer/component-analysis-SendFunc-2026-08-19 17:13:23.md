# Component Deep Analysis Report

**Component:** SendFunc
**Component Type:** Outbound packet builders (server-to-client messaging layer)
**Project Scope:** W2PP legacy C/C++ codebase (`legacy/`)
**Primary Files:** `legacy/Code/TMSrv/SendFunc.cpp` (1817 lines), `legacy/Code/TMSrv/SendFunc.h` (73 lines)
**Analysis Date:** 2026-08-19 17:13:23
**Ignored Folders:** `.git`, `.opencode`

---

## 1. Executive Summary

`SendFunc` is the outbound messaging subsystem of the W2PP map server (`TMSrv`). It is a large collection of 46 free (non-class) C functions that construct typed network packets (`MSG_*` structures defined in `Basedef.h`) and enqueue them onto the per-connection transmit buffer of each connected client via the `CPSock::AddMessage` / `SendMessageA` socket abstraction.

The component occupies the **sink** position of the server's message pipeline: game logic in message handlers (`_MSG_*.cpp`), timers (`ProcessSecMinTimer.cpp`), the kill processor (`MobKilled.cpp`), and DB handling (`ProcessDBMessage.cpp`) all converge on `SendFunc` to push authoritative state updates to clients. It is one of the most heavily referenced translation units in the project; its most-called function `SendClientMessage` alone is invoked ~671 times across the codebase.

Architecturally, `SendFunc` is a **procedural, flat module** with no classes of its own and no encapsulation; it operates directly on global server state arrays (`pUser`, `pMob`, `pMobGrid`, `pItemGrid`, `pItem`, `ChargedGuildList`, `Pista`, `g_pGuildWar`, `g_pGuildAlly`) declared in `Server.h` and `Basedef.h`. It reuses read-builder helpers from the sibling component `GetFunc` (`GetCreateMob`, `GetCreateItem`, `GetAffect`, `GetGuilty`) to serialize game state into wire formats.

Key findings:
- The component is the **single canonical path for all outbound client communication** on the map server, providing both point-to-point sends and grid/map/area/kingdom/guild scoped broadcasts.
- It implements a **visibility grid protocol**: `GridMulticast` reconciles which mobs and items enter/leave a player's viewport as the player moves across the `pMobGrid`/`pItemGrid` world grid, issuing `SendCreateMob`/`SendRemoveMob`/`SendCreateItem`/`SendRemoveItem` accordingly. This is the most complex and highest-risk logic in the component.
- **No automated tests exist** anywhere for the legacy codebase (search across `legacy/` for test/spec/unit/gtest patterns returned zero results). All test artifacts found were inside the ignored `.opencode/node_modules`.
- The component embeds a substantial amount of **hard-coded, undocumented business rules** (magic item IDs, coordinate regions, guild/kingdom thresholds, event flags) and contains several apparent latent bugs (duplicate condition in `SendEnvEffectLeader`, missing `conn` bounds guards in several send functions, unbounded `sprintf`).
- The header exposes functions with a naming-convention-driven API but no documented contract, no versioning, and no central dispatch table; correctness relies on every caller observing per-function guard conventions.

---

## 2. Data Flow Analysis

Data flows **from game-logic callers → SendFunc builders → per-client socket buffers → wire**.

```
1. Game logic mutates authoritative server state
   (pMob[conn].MOB.CurrentScore.Hp, pUser[conn].Coin, pMobGrid[y][x], etc.)
   e.g. _MSG_UseItem.cpp, _MSG_Attack.cpp, MobKilled.cpp, ProcessSecMinTimer.cpp
2. Caller invokes a SendFunc* builder
   e.g. SendScore(conn), SendEtc(conn), SendCreateMob(conn, tmob, bSend)
3. Builder allocates a stack MSG_* struct (e.g. MSG_UpdateScore) and zero-fills it
   memset(&sm_vus, 0, sizeof(MSG_UpdateScore))
4. Builder sets the packet header fields
   Size, Type (_MSG_UpdateScore), ID (conn or ESCENE_FIELD=30000)
5. Builder copies authoritative state into the packet
   memcpy(&sm_vus.Score, &pMob[conn].MOB.CurrentScore, sizeof(STRUCT_SCORE))
6. For broadcast builders, scope selection is performed
   grid loop over pMobGrid / pItemGrid, or user loop over pUser[]
   (GridMulticast, MapaMulticast, SendMessageArea, SendNotice, etc.)
7. Packet is appended to the target client's transmit buffer
   pUser[x].cSock.AddMessage((char*)&sm, sizeof(sm))
   CPSock::AddMessage encrypts payload (XOR/shift scheme with pKeyWord) and computes checksum
8. Optional immediate flush
   if (bSend) pUser[x].cSock.SendMessageA()  →  winsock send()
9. Failure handling
   AddMessage returns FALSE (buffer full / invalid socket) →
   caller typically invokes CloseUser(conn) to drop the connection
10. Side effects during building
    Some builders mutate server state (pMob[conn].LastTime/TargetX,
    pMobGrid[ty][tx] = conn in GridMulticast; pMob[conn].Affect[i].Time=450 in SendAffect)
```

Example: `SendScore` (SendFunc.cpp:1136)
```
1. Caller (e.g. after damage) invokes SendScore(conn)
2. Fills MSG_UpdateScore header (Type=_MSG_UpdateScore, ID=conn)
3. Copies CurrentScore, Critical, SaveMana, Guild, GuildLevel, Resists, Magic from pMob[conn].MOB
4. Calls GetAffect() (GetFunc) to serialize the affect list
5. Applies guild-suppression rules (GuildDisable flag; BrState castle region 2604..2648 x 1708..1744)
6. GridMulticast(...) broadcasts to all viewers in the 33x33 view grid
7. Calls SendAffect(conn) to push the affect list packet
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Connection handle bounds check (conn must satisfy 0 < conn < MAX_USER=1000) before messaging | SendFunc.cpp:29, 181, 290, 529, 1057, 1197, 1235, 1371, 1440, 1474, 1510, 1536, 1592, 1624, 1739 |
| Validation | Only send to clients in USER_PLAY mode | SendFunc.cpp:59, 74, 109, 159, 171, 264, 278, 293, 538, 541, 1018, 1060, 1200, 1238, 1374, 1443, 1477, 1513, 1539, 1575, 1595, 1627 |
| Validation | Only send to clients with an active socket (cSock.Sock != 0) | SendFunc.cpp:296, 1018, 1046, 1063, 1203, 1241, 1377, 1446, 1480, 1516, 1542, 1578, 1598, 1630 |
| Business Logic | Notice suppression: messages starting with "'x" are not broadcast | SendFunc.cpp:54 |
| Business Logic | Chief-only notices target players with GuildLevel == 9 belonging to a "charged" guild | SendFunc.cpp:77, 112, 87-97, 129-138 |
| Business Logic | Summon chief teleports guild leaders of charged guilds to a computed seat location | SendFunc.cpp:143-151 |
| Business Logic | Trade-mode-aware mob creation: use MSG_CreateMobTrade when target is trading | SendFunc.cpp:304-317 |
| Business Logic | View-grid reconciliation: create/remove mobs & items as they enter/leave the 33x33 viewport | SendFunc.cpp:620-823 (GridMulticast) |
| Business Logic | Kingdom chat exclusion: SyncKingdomMulticast skips users with KingChat != 0 | SendFunc.cpp:278 |
| Business Logic | Event-scene hard-coded costume injection for a specific region (896..1150 x 1405..1538) | SendFunc.cpp:898-916 |
| Business Logic | Level-up quarter bonuses: segment thresholds 1-4 trigger different messages and rewards | SendFunc.cpp:920-965 |
| Business Logic | Max HP/MP normalization to a 16-bit field (> 32000 scaled by /100) | SendFunc.cpp:1496-1497 |
| Business Logic | Mount equipment encoding: slot 14 items 2360-2389 encode level in high bits; level 0 hides mount | SendFunc.cpp:1103-1127 |
| Business Logic | Guild suppression in Battle Royale castle region | SendFunc.cpp:1176-1189 |
| Business Logic | Divine affect expiry: affects of type 34 near expiry are clamped/derived from DivineEnd | SendFunc.cpp:1784-1807 |
| Business Logic | PK flag derivation: guilty, PKMode, RvRState, CastleState or GTorreState → PK state 1 | SendFunc.cpp:1752-1764 |
| Business Logic | Send buffer failure → disconnect the client (CloseUser) | SendFunc.cpp:557, 801-806, 1050, 1230, 1256, 1413, 1469, 1505, 1531, 1558, 1585, 1619, 1644 |
| Business Logic | Guild war/ally info resolved through g_pGuildWar / g_pGuildAlly tables | SendFunc.cpp:1310-1366, 1416-1436 |
| Business Logic | Shop tax applied from owning guild zone CityTax | SendFunc.cpp:1405-1410 |

### Detailed breakdown of the business rules

---

### Business Rule: Connection Bounds and Client-State Validation

**Overview:**
Almost every `Send*` function begins with a defensive guard that validates the target connection handle and the client's session state before any packet construction or socket write occurs. The canonical guard sequence is (1) `conn <= 0 || conn >= MAX_USER` bounds check, (2) `pUser[conn].Mode != USER_PLAY` state check, and (3) `pUser[conn].cSock.Sock == 0` socket validity check. This reflects the single-threaded event-loop design of the server where slots in the fixed-size `pUser[MAX_USER]` array may be empty, mid-login, or disconnected at any time.

**Detailed description:**
The component operates on a fixed-capacity global array `pUser[MAX_USER]` (MAX_USER = 1000). Slot index 0 is reserved/unused (hence `conn <= 0` guards), and indices >= MAX_USER are not valid client connections (they are the starting index for NPCs/mobs in `pMob`). The `USER_PLAY` mode constant gates whether the slot represents an in-game, active character; clients in login, character-selection, or logging-out states must not receive gameplay packets. The socket-validity check guards against writes through an uninitialized or already-closed socket handle. Functions such as `SendClientMessage` (line 29), `SendClientMessageOk` (181), `SendCreateMob` (290), `SendAutoTrade` (529), `SendItem` (1057), `SendEtc` (1197), `SendCargoCoin` (1235), `SendShopList` (1371), `SendReqParty` (1440), `SendAddParty` (1474), `SendRemoveParty` (1510), `SendCarry` (1536), `SendSetHpMp` (1592), and `SendHpMode` (1624) all enforce the same trio. The PK-info sender `SendPKInfo` (1739) validates both `conn` and `target`. This rule is central to the reliability of the system: an out-of-range or non-playing index would otherwise cause undefined behavior in the array index and a null/garbage socket write.

**Rule workflow:**
1. Caller invokes a `Send*` builder with a connection index and payload arguments.
2. The builder first checks `conn <= 0 || conn >= MAX_USER`; if violated, it returns immediately (no packet).
3. The builder checks `pUser[conn].Mode == USER_PLAY`; non-playing clients are skipped.
4. The builder checks `pUser[conn].cSock.Sock` is non-zero; disconnected clients are skipped.
5. Only after all guards pass does the function construct the packet and enqueue it.

---

### Business Rule: Notice Suppression Prefix ('x)

**Overview:**
`SendNotice` broadcasts a system notice to every playing user, but deliberately suppresses any message whose first two characters are the literal sequence `'x`. This is a private administrative escape hatch embedded in the broadcast path.

**Detailed description:**
`SendNotice` (line 47) first writes the message into a `char Notice[512]` buffer prefixed with `"not "` and logs it to the system log (`Log(Notice, "-system", NULL)`), so the notice is always recorded server-side. Immediately after logging, it checks `if (Message[0] == '\'' && Message[1] == 'x') return;`. If the message begins with an apostrophe followed by the letter `x`, the function aborts before the broadcast loop, meaning the message is logged but never delivered to any client. This is a server-side-only "quiet" command pattern; the intended use is inferred to be a hidden operator/console command that should not be visible to players. Because the check is on the raw message text, it is a text-level, not permission-level, filter: any caller that passes a message starting with `'x` will have it suppressed, regardless of the caller's authority.

**Rule workflow:**
1. `SendNotice(Message)` is called with the notice text.
2. The notice is formatted as `"not <Message>"` and written to the system log unconditionally.
3. The message's first two characters are inspected.
4. If they equal `'x`, the function returns without broadcasting (logged, not sent).
5. Otherwise the function loops over all `pUser[i]` in `USER_PLAY` mode and delivers via `SendClientMessage(i, Message)`.

---

### Business Rule: Chief-Only Notices and Summon (Guild-Level 9 + Charged Guild)

**Overview:**
`SendNoticeChief` and `SendSummonChief` target guild leaders (players whose `pMob[i].MOB.GuildLevel == 9`) who belong to a "charged" guild — a guild recorded in the global `ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]` matrix as currently holding a guild-zone seat. `SendNoticeChief` delivers a notice only to those leaders; `SendSummonChief` additionally teleports each eligible leader to a computed throne/seat coordinate.

**Detailed description:**
The eligibility filter has three conditions, all of which must hold for a player to be targeted: (1) the player is in `USER_PLAY` mode, (2) `pMob[i].MOB.GuildLevel == 9` (guild master level), and (3) the player's `Guild` id (which must be `> 0`) is present in the two-dimensional `ChargedGuildList` array. The scan iterates `j` over `MAX_SERVER` (10) servers and `k` over `MAX_GUILDZONE` (5) zones to test membership. In `SendSummonChief` (line 103), if no charged guild is found for a candidate (`FoundCharged == 0`), the function returns for that user. When found, the destination tile is derived from the server and zone indices via the arithmetic `tx = 7 * Server / 5 + 317` and `ty = 4025 - 2 * Server % 5`, with the zone index added or subtracted from `tx` depending on whether `Server / 5` is non-zero; the leader is then moved with `DoTeleport(i, tx, ty)`. This maps a logical (server, zone) seat to a physical world coordinate for the guild-castle/kingdom war system.

**Rule workflow:**
1. Iterate over all users; skip any not in `USER_PLAY`.
2. Skip users whose `GuildLevel != 9` (not a guild master).
3. Read `Guild = pMob[i].MOB.Guild`; skip if `Guild <= 0`.
4. Scan `ChargedGuildList` over all servers/zones for a match with `Guild`.
5. (Notice variant) If matched, send the message via `SendClientMessage`.
6. (Summon variant) If matched, compute the seat `(tx, ty)` from server/zone indices and call `DoTeleport(i, tx, ty)`.

---

### Business Rule: Trade-Mode-Aware Mob Creation

**Overview:**
`SendCreateMob` (line 288) decides between two packet layouts when informing a client that another entity should be created: a standard `MSG_CreateMob` or a trading-specific `MSG_CreateMobTrade`. The choice depends on whether the target entity is currently in an open trade session.

**Detailed description:**
When a client (`conn`) needs to see another entity (`otherconn`), the builder determines whether the other entity is currently trading. If `otherconn` is out of the valid user range, or its `pUser[otherconn].TradeMode != 1` (i.e., not actively trading), a normal `MSG_CreateMob` is produced via `GetCreateMob(otherconn, &sm)` (a `GetFunc` helper) and delivered. If the other entity is trading (`TradeMode == 1`), the specialized `MSG_CreateMobTrade` layout is used instead, produced by `GetCreateMobTrade`. The trade variant carries additional fields (a `Desc[MAX_AUTOTRADETITLE]` description and trade title tab), reflecting that a trading character must render with its shop/trade presentation on other clients' screens. In both paths the packet is enqueued and, importantly, flushed immediately via `SendMessageA()` (as opposed to being batched) — the `bSend` parameter is effectively ignored for this function and the send is always performed.

**Rule workflow:**
1. Validate `conn` bounds and `USER_PLAY`/socket state.
2. If `otherconn <= 0 || otherconn >= MAX_USER || pUser[otherconn].TradeMode != 1`:
   - Build standard `MSG_CreateMob` via `GetCreateMob`.
   - Enqueue and flush to `conn`; return.
3. Otherwise build `MSG_CreateMobTrade` via `GetCreateMobTrade`, enqueue, and flush.
4. The entity appears to `conn` with either normal or trading presentation depending on its trade state.

---

### Business Rule: View-Grid Reconciliation (GridMulticast)

**Overview:**
`GridMulticast` (lines 260-823, two overloads) is the core visibility-management routine. It maintains which mobs and items are visible to each player by tracking the player's position on the world grid (`pMobGrid`/`pItemGrid`, each `MAX_GRIDX x MAX_GRIDY` = 4096x4096) and multicasting a message to every player within the 33x33 view window centered on a tile. It simultaneously reconciles entity creation/removal so clients see mobs and items appear and disappear as they enter and leave the viewport.

**Detailed description:**
The function computes the view rectangle from `VIEWGRIDX`/`VIEWGRIDY` (33) and `HALFGRIDX`/`HALFGRIDY` (16) centered on the origin tile, clamping against the world bounds (`MAX_GRIDX`/`MAX_GRIDY`). It handles the case where the moving player's grid registration is inconsistent: it calls `GetEmptyMobGrid` to relocate the entity if its stored grid position collides with another entity, and it logs diagnostic messages ("PC do not have his own grid", "PC step in other mob's grid") when grid ownership is violated. It then updates `pMobGrid[ty][tx] = conn` to reflect the new position. The reconciliation pass walks the old viewport and the new viewport: mobs in the old view that fall outside the new view are removed from those viewers (`SendRemoveMob`), and mobs in the new view not previously visible are created (`SendCreateMob` + `SendPKInfo`); items undergo the analogous `SendRemoveItem`/`SendCreateItem` cycle. Finally, the `msg` (typically an action packet, cast to `MSG_Action*`) is delivered to all visible users, and the moving entity's `LastTime`, `LastSpeed`, `LastX`, `LastY`, `TargetX`, `TargetY` are updated from the action. There is also a secondary overload (line 843) that is a pure "deliver message to viewers in a view window" form without the position-update semantics, used by chat, effects, and score broadcasts; it embeds the hard-coded costume injection and level-up bonus logic described in separate rules below.

**Rule workflow:**
1. Compute the sender's old view window and the destination view window, clamped to world bounds.
2. Validate/repair the entity's grid registration via `GetEmptyMobGrid`; log ownership anomalies.
3. Set `pMobGrid[newY][newX] = conn` to move the entity on the grid.
4. Pass 1: for viewers in the old window, send `SendRemoveMob` for entities that left the new window.
5. Pass 2: for viewers in the new window, send `SendCreateItem`/`SendRemoveItem` for items that entered/left.
6. Pass 3: for entities entering the view, send `SendCreateMob` + `SendPKInfo` (both directions).
7. Deliver the multicast `msg` to all visible clients (checking `AddMessage` return; log on failure).
8. Update the moving entity's last-position/action fields from the `MSG_Action*`.

---

### Business Rule: Event-Scene Costume Injection (Hard-Coded Region)

**Overview:**
Within the pure-broadcast `GridMulticast` overload (line 843), when the multicast message is a `MSG_CreateMob`, the function checks whether the entity's coordinates fall inside a specific hard-coded region (`xx >= 896 && yy >= 1405 && xx <= 1150 && yy <= 1538`) and, if so, forcibly overrides the entity's equipment slots 1 and 15 with hard-coded item IDs (3505 and 3999).

**Detailed description:**
The coordinates correspond to a fixed map region — inferred to be a special event/PvP arena area on the game map. For any `MSG_CreateMob` being multicasted whose `PosX`/`PosY` fall within the bounding box, the function overwrites `Equip[1]` with the visual code of item 3505 and `Equip[15]` with the visual code of item 3999, and correspondingly sets the `AnctCode[1]` and `AnctCode[15]` ancestral-code fields via `BASE_VisualItemCode`/`BASE_VisualAnctCode`. This forces every entity in that region to appear with a uniform costume/weapon loadout, regardless of the entity's actual inventory. The values 3505/3999 and the coordinate bounds are hard-coded literals with no accompanying configuration or comment explaining their meaning — a notable maintainability concern. Because the injection mutates the shared `msg` buffer before it is delivered to all viewers in the loop, the same augmented appearance is broadcast to every client in the view window.

**Rule workflow:**
1. `GridMulticast` begins iterating mobs in the view window for a `MSG_CreateMob` message.
2. Read the entity's `PosX`, `PosY` from the `MSG_CreateMob` payload.
3. If within 896..1150 x 1405..1538, override `Equip[1]`/`AnctCode[1]` (item 3505) and `Equip[15]`/`AnctCode[15]` (item 3999).
4. Deliver the (now possibly augmented) message to every visible client.

---

### Business Rule: Level-Up Quarter Bonuses and Rewards (MSG_CNFMobKill)

**Overview:**
When a `MSG_CNFMobKill` (mob-kill confirmation) message passes through the broadcast `GridMulticast` overload (line 920), the function derives the killer's level-segment from `CheckGetLevel()` and applies per-segment bonus messages, score/stat updates, PK-point increments, and a level-up broadcast for the final segment.

**Detailed description:**
For each visible viewer `tmob`, the function sets the packet's `Exp` and `Hold` from the killer mob's state (`pMob[tmob].MOB.Exp`, `extra.Hold`) and computes `Segment = pMob[tmob].CheckGetLevel()`. The segment (1-4) represents which "quarter" of the level progression was crossed (e.g., 1st, 2nd, 3rd quarter, or final full level). For segments 1-3, it sends the corresponding localized bonus message from `g_pMessageStringTable` (`_NN_1_Quarters_Bonus`, `_NN_2_Quarters_Bonus`, `_NN_3_Quarters_Bonus`). For segment 4 (a full level-up), it calls `SetCircletSubGod(tmob)`, sends the level-up message, and — if the killer is a `MORTAL` class master — calls `DoItemLevel(tmob)` to grant level-up item bonuses. After the per-segment messages, it always calls `SendScore(tmob)` and `SendEmotion(tmob, 14, 3)` to push updated stats and a level-up emotion animation. For the full level-up (segment 4) it additionally calls `SendEtc`, raises the PK point total by 5 via `GetPKPoint`/`SetPKPoint`, re-broadcasts an updated `MSG_CreateMob` (via `GetCreateMob` + nested `GridMulticast`) so other clients see the new appearance, and logs the level-up with the account name and IP.

**Rule workflow:**
1. A `MSG_CNFMobKill` is multicasted to viewers.
2. For each viewer, copy `Exp`/`Hold` from the killer's state.
3. Compute `Segment = pMob[tmob].CheckGetLevel()`.
4. Segment 4: `SetCircletSubGod`; level-up message; `DoItemLevel` if MORTAL class master.
5. Segments 3/2/1: send the respective quarter-bonus message.
6. Always call `SendScore` + `SendEmotion(tmob, 14, 3)`.
7. Segment 4 only: `SendEtc`, +5 PK points, re-broadcast `MSG_CreateMob`, log level-up.

---

### Business Rule: Max HP/MP Field Normalization

**Overview:**
`SendAddParty` (line 1472) encodes the joining member's HP and MaxHP into the party confirmation packet, but normalizes values above 32000 by dividing by 100 so they fit within the packet's 16-bit field representation.

**Detailed description:**
The party-membership packet `MSG_CNFAddParty` carries `Hp` and `MaxHp` fields whose usable range is limited to 16 bits. Characters whose current or maximum HP exceeds 32000 (a level/rebalance threshold) cannot be represented directly. The builder therefore applies a per-field transformation: if `MaxHp > 32000`, it stores `(MaxHp+1) / 100`; otherwise it stores the raw value. The same rule applies to `Hp`. The `+1` before the integer division is a rounding-up safeguard so that a value such as 32000..32099 maps to 321 rather than truncating to 320. This is an asymmetric, lossy encoding: the client is expected to interpret a sub-321 value as "already scaled" versus a large raw value, and the scaling is only applied on the high side. The rule reflects a protocol constraint where the party roster's HP fields were too small for end-game content, patched by a conditional scaling heuristic.

**Rule workflow:**
1. `SendAddParty(Leaderconn, conn, PartyID)` validates the leader's connection state.
2. Set `PartyID`/`Leaderconn` fields (30000 sentinel for non-zero party id).
3. Read `Level`, `MaxHp`, `Hp` from the joining member's `pMob[conn].MOB.CurrentScore`.
4. If `MaxHp > 32000`, store `(MaxHp+1)/100`; else store `MaxHp`. Same for `Hp`.
5. Deliver via `SendOneMessage`; on failure, `CloseUser(Leaderconn)`.

---

### Business Rule: Mount Equipment Encoding and Hiding

**Overview:**
`SendEquip` (line 1084) encodes the player's equipment visuals for broadcast, applying two special rules to slot 14 (the mount slot): a mount item whose level is zero is hidden entirely, and a mounted item's level is packed into the high bits of the item code.

**Detailed description:**
`SendEquip` builds a `MSG_UpdateEquip` from `pMob[conn].MOB.Equip[]`, computing each slot's visual item code and ancestral code via `BASE_VisualItemCode`/`BASE_VisualAnctCode`. For slot 14, it first tests whether the item code falls in the mount range `>= 2360 && < 2390`. If the mount's `stEffect[0].sValue <= 0` (level 0 / not owned), the visual code is zeroed and a `SendMount` flag is raised so the client hides the mount. If the mount is valid, the builder reads `MountLevel = stEffect[1].cEffect / 10`, clamps it to `[0, 13]`, multiplies by 4096, and adds it to the item code — encoding the mount's level in the high bits of the 16-bit code so the client can render different mount tiers. After broadcasting the equip update via `GridMulticast`, if `SendMount` was raised the function separately calls `SendItem(conn, ITEM_PLACE_EQUIP, 14, ...)` to push the actual slot-14 item so the client's inventory panel shows the (hidden) mount data explicitly.

**Rule workflow:**
1. Build `MSG_UpdateEquip` for `conn` over all `MAX_EQUIP` (16) slots.
2. For each slot compute visual item code and ancestral code.
3. Slot 14 only: if code in 2360..2389 and level (`stEffect[0].sValue`) <= 0, zero the code and set `SendMount`.
4. Slot 14 only: if code in 2360..2389 and valid, encode `(stEffect[1].cEffect/10)` clamped to 0..13, times 4096, into the code.
5. `GridMulticast` the equip update to viewers (skipping `skip`).
6. If `SendMount`, `SendItem(conn, ITEM_PLACE_EQUIP, 14, ...)` to send the raw slot-14 item.

---

### Business Rule: Guild Suppression in Battle Royale Castle Region

**Overview:**
`SendScore` (line 1136) conditionally blanks the guild identity of a player when a global `BrState` (Battle Royale) flag is active and the player is located inside a specific castle-region bounding box, preventing guild affiliation from being displayed to other clients in that zone.

**Detailed description:**
`SendScore` fills a `MSG_UpdateScore` with the player's current score, critical, save-mana, guild, guild-level, affects, resists, magic, and special bytes. Before broadcasting via `GridMulticast`, it applies two guild-suppression rules. First, if `pMob[conn].GuildDisable` is set, the `Guild` field is forced to 0. Second, if the global `BrState != 0` and `conn` is a real user (`conn < MAX_USER`), it reads the player's `TargetX`/`TargetY`; if those coordinates fall inside the box `2604..2648 x 1708..1744` (a hard-coded castle/arena region), both `Guild` and `GuildLevel` are zeroed. The effect is that players inside the Battle Royale castle are rendered guild-less to onlookers, masking faction identity during the event. The region coordinates and the `BrState` global are hard-coded/globally toggled elsewhere; the suppression is applied only in this score-broadcast path, meaning other packet types could still leak guild info unless they apply equivalent guards.

**Rule workflow:**
1. Populate `MSG_UpdateScore` with the full score/affect/guild state.
2. If `GuildDisable`, set `Guild = 0`.
3. If `BrState != 0` and user coordinates are within 2604..2648 x 1708..1744, set `Guild = 0` and `GuildLevel = 0`.
4. `GridMulticast` the score to all viewers; then `SendAffect(conn)`.

---

### Business Rule: Divine Affect Expiry Handling

**Overview:**
`SendAffect` (line 1769) serializes the player's affect (status-effect) list, but for divine buffs (affect type 34) it converts the remaining duration from an absolute timestamp to a compact relative value and clamps it near expiry.

**Detailed description:**
`SendAffect` builds a `MSG_SendAffect` from `pMob[conn].Affect[]`. For each slot, if the affect `Type == 34` (a divine blessing) and its stored `Time >= 32000000` (an absolute/unix-like timestamp), the function reads the current wall-clock time via `time(&now)` and the effect's end timestamp `pMob[conn].extra.DivineEnd`. If the remaining time `(DivineEnd - now)` is `<= 3600` seconds (one hour), the stored `Time` is forced to 450 (a short remaining-duration sentinel) and copied through. Otherwise the remaining duration is computed as `(int)(((DivineEnd - now) / 60 / 60 / 24 * AFFECT_1D) - 1)` — i.e., days remaining multiplied by `AFFECT_1D` (10800, the tick count per day) minus one — and stored. This converts a wall-clock expiry into the game's internal per-day tick units so the client can render the remaining buff time. Non-type-34 affects (and type 34 with a non-absolute time) are copied verbatim. The packet is then enqueued to the client's send buffer.

**Rule workflow:**
1. Build `MSG_SendAffect` for `conn`.
2. For each of `MAX_AFFECT` slots, inspect `pMob[conn].Affect[i]`.
3. If `Type == 34 && Time >= 32000000`: read `now`; if `(DivineEnd - now) <= 3600`, store `Time = 450`; else store `(DivineEnd - now)/86400 * AFFECT_1D - 1`.
4. Else if `Type >= 1`, copy type/value/level/time verbatim.
5. Enqueue the packet via `pUser[conn].cSock.AddMessage`.

---

### Business Rule: PK Flag Derivation

**Overview:**
`SendPKInfo` (line 1737) computes a binary PK (player-kill) state for a target and sends it to a viewing client, aggregating several PK-relevant flags into a single 0/1 value.

**Detailed description:**
`SendPKInfo(conn, target)` validates both connection handles, then builds a `MSG_STANDARDPARM` with `Type = _MSG_PKInfo` and `ID = target`. If the global `NewbieEventServer == 0` (not a newbie-protected event server), the function computes `guilty = GetGuilty(target)` and sets `state = 1` if any of `guilty`, `pUser[target].PKMode`, `RvRState`, `CastleState`, or `GTorreState` is non-zero; otherwise `state = 0`. If `NewbieEventServer != 0`, `state` is forced to 1 regardless. The resulting `Parm` (0 or 1) tells the viewing client whether the target should be rendered as a PK-flagged character (red name). This consolidates five independent server flags into one packet bit so clients uniformly reflect the "hostile/flagged" visual state across guild wars, castle sieges, and tower events.

**Rule workflow:**
1. Validate `conn` and `target` bounds.
2. Build `MSG_STANDARDPARM` (`_MSG_PKInfo`, `ID = target`).
3. If `NewbieEventServer == 0`: `state = (GetGuilty(target) || PKMode || RvRState || CastleState || GTorreState) ? 1 : 0`.
4. Else `state = 1`.
5. Set `Parm = state` and enqueue to `conn`.

---

### Business Rule: Failure-Driven Disconnect (AddMessage Contract)

**Overview:**
Several builders treat a `FALSE` return from `CPSock::AddMessage` (or `SendOneMessage`) as a fatal condition and immediately drop the client with `CloseUser(conn)`, on the principle that an un-sendable packet means the connection's transmit buffer is corrupt, full, or the socket is invalid — a state from which recovery is not attempted.

**Detailed description:**
`CPSock::AddMessage` (CPSock.cpp:513) returns `FALSE` in two situations: the transmit buffer is full (`nSendPosition + Size >= SEND_BUFFER_SIZE`) or the socket handle is invalid (`Sock <= 0`). Both indicate a client that can no longer receive authoritative state, so continuing to accumulate packets would be pointless and could desynchronize the client. Builders therefore pattern-match on the return value: `if (!pUser[x].cSock.AddMessage(...)) CloseUser(x);`. This is applied in `SendAutoTrade` (557), `SendEtc` (1230), `SendCargoCoin` (1256), `SendShopList` (1413), `SendReqParty` (1469), `SendAddParty` (1505 via `SendOneMessage`), `SendRemoveParty` (1531), `SendCarry` (1558), `SendWeather` (1585), `SendSetHpMp` (1619), `SendHpMode` (1644), and `PartyGridMulticast` (1050). By contrast, the low-level signal builders and broadcast loops generally ignore the return value (e.g., `SendClientSignal` family, `SyncMulticast`, `GridMulticast` loop), logging the error but not disconnecting, because a failure in a multicast to one viewer should not drop unrelated clients. This creates two distinct resilience contracts within the same component: point-to-point critical updates disconnect on failure, while best-effort broadcasts degrade gracefully.

**Rule workflow:**
1. Builder constructs and enqueues a packet via `AddMessage`/`SendOneMessage`.
2. If the return is `TRUE`, normal flow continues.
3. If the return is `FALSE` (buffer full or invalid socket), the builder calls `CloseUser(conn)`.
4. The client is disconnected from the server; no retry is attempted.

---

## 4. Component Structure

The component is a single translation unit pair plus its shared header. There are no subdirectories or internal classes; organization is purely by function responsibility.

```
legacy/Code/TMSrv/
├── SendFunc.h                # 46 function prototypes (the public contract) - 73 lines
└── SendFunc.cpp              # Implementations - 1817 lines
```

Functional grouping within `SendFunc.cpp`:

| Group | Functions | Lines |
|-------|-----------|-------|
| Point-to-point text messaging | `SendClientMessage`, `SendClientMessageOk` | 27-45, 179-195 |
| Broadcast notices | `SendNotice`, `SendNoticeChief`, `SendNoticeArea`, `SendGuildNotice`, `SendSummonChief` | 47-177 |
| Raw signal builders (no payload / 1..3 parms / short parms) | `SendClientSignal`, `SendClientSignalParm`, `SendClientSignalParm2`, `SendClientSignalParm3`, `SendClientSignalShortParm2` | 197-258 |
| Whole-server / kingdom multicast | `SyncMulticast`, `SyncKingdomMulticast` | 260-286 |
| Entity create/remove | `SendCreateMob`, `SendCreateItem`, `SendRemoveMob`, `SendRemoveItem` | 288-331, 494-525 |
| Chat / say | `SendChat`, `SendSay` | 333-346, 1647-1660 |
| Environment effects | `SendEnvEffect`, `SendEnvEffectKingdom`, `SendEnvEffectLeader` | 348-492 |
| Auto-trade | `SendAutoTrade` | 527-558 |
| View-grid management | `SendGridMob`, `GridMulticast` (x2), `PartyGridMulticast` | 560-1053 |
| Character presentation | `SendEmotion`, `SendItem`, `SendEquip` | 825-841, 1055-1134 |
| Player state sync | `SendScore`, `SendEtc`, `SendCargoCoin`, `SendSetHpMp`, `SendHpMode`, `SendAffect`, `SendPKInfo` | 1136-1193, 1195-1231, 1233-1257, 1590-1645, 1769-1817, 1737-1767 |
| Guild / kingdom info | `SendGuildList`, `SendWarInfo`, `SendWeather` | 1259-1367, 1416-1436, 1561-1588 |
| Shop | `SendShopList` | 1369-1414 |
| Party | `SendReqParty`, `SendAddParty`, `SendRemoveParty`, `SendCarry` | 1438-1559 |
| Area / map scoped multicast | `MapaMulticast`, `SendMessageArea`, `SendSignalParmArea`, `SendShortSignalParm2Area` | 1662-1735 |

---

## 5. Dependency Analysis

### Internal Dependencies (within TMSrv / shared Code)

```
Callers → SendFunc.cpp:
  Server.cpp, GetFunc.cpp, MobKilled.cpp, ProcessClientMessage.cpp,
  ProcessDBMessage.cpp, ProcessSecMinTimer.cpp, CCastleZakum.cpp,
  CWarTower.cpp, imple.cpp, and all _MSG_*.cpp handlers

SendFunc.cpp → GetFunc (read-builder helpers):
  GetCreateMob, GetCreateMobTrade, GetCreateItem, GetAffect, GetGuilty, GetEmptyMobGrid

SendFunc.cpp → Server.h globals:
  pUser[], pMob[], pItem[], pMobGrid[][], pItemGrid[][], ChargedGuildList[][],
  Pista[], g_pGuildWar[], g_pGuildAlly[], g_pGuildZone[], ServerGroup,
  CurrentWeather, NewbieEventServer, RvRState, CastleState, GTorreState, BrState, temp

SendFunc.cpp → Server.cpp helpers:
  CloseUser, DoTeleport, SetReqHp, SetReqMp, DoItemLevel, SetCircletSubGod

SendFunc.cpp → CPSock (CUser.cSock):
  AddMessage, SendMessageA, SendOneMessage

SendFunc.cpp → Basedef.h:
  All MSG_* packet structures and _MSG_* type constants, STRUCT_MOB/ITEM/SCORE/AFFECT,
  MAX_* / VIEWGRID / HALFGRID constants, BASE_VisualItemCode / BASE_VisualAnctCode /
  BASE_GetGuildName / BASE_GetVillage

SendFunc.cpp → Language.h:
  g_pMessageStringTable (localized strings: _NN_*_Bonus, _SN_*_war, _NN_No_Guild_Members, etc.)
```

### External Dependencies

| Dependency | Type | Purpose |
|-----------|------|---------|
| Winsock2 (`WSAStartup`, `send`, `closesocket`, `socket`) | OS API | Network transport via CPSock |
| Windows API (`<Windows.h>`, `time`) | OS API | System time for divine affect expiry; platform bindings |
| C runtime (`<memory>`, `stdio` via transitive) | Standard library | `memcpy`, `memset`, `sprintf`, `strcpy`, `strcat` |

Note: The `MSG_*` packets are also consumed by the game client (outside this repository's scope); the wire contract is defined solely in `Basedef.h` and is shared by both sides.

---

## 6. Afferent and Efferent Coupling

Coupling measured on the component's free functions (the C-style "components" of this codebase). Afferent Coupling (Ca) = number of call sites across all `.cpp` files; Efferent Coupling (Ce) = count of distinct external helpers/globals the function touches.

| Function | Afferent Coupling (call sites) | Efferent Coupling | Critical |
|----------|-------------------------------|-------------------|----------|
| SendClientMessage | 671 | 3 (AddMessage, pUser, constants) | High |
| SendItem | 385 | 3 | High |
| GridMulticast | 105 | 8 (pMobGrid, pItemGrid, GetEmptyMobGrid, SendCreateMob/RemoveMob/Item, SendPKInfo, pMob, BASE_* helpers) | High |
| SendClientSignalParm | 104 | 3 | Medium |
| SendSay | 104 | 3 | Medium |
| SendScore | 100 | 7 (GetAffect, GridMulticast, SendAffect, pMob, globals) | High |
| SendEmotion | 93 | 3 | Medium |
| SendEtc | 57 | 3 | Medium |
| SendEnvEffectLeader | 45 | 5 (GridMulticast, Pista, pMobGrid) | Medium |
| SendNotice | 33 | 3 | Low |
| SendEnvEffect | 29 | 3 | Medium |
| SendHpMode | 28 | 3 | Low |
| SendClientSignal | 22 | 2 | Low |
| SendSetHpMp | 18 | 5 (SetReqHp, SetReqMp, pUser, pMob) | Low |
| SendNoticeArea | 16 | 2 | Low |
| SendCarry | 14 | 3 | Low |
| MapaMulticast | 14 | 3 | Low |
| SendAddParty | 13 | 3 | Medium |
| SendEquip | 11 | 5 (BASE_* helpers, GridMulticast, SendItem) | Medium |
| SendGuildNotice | 9 | 2 | Low |
| SendRemoveMob | 9 | 2 | Low |
| SendSignalParmArea | 6 | 3 (pMobGrid, SendClientSignalParm) | Medium |
| SendPKInfo | 6 | 4 (GetGuilty, globals) | Medium |
| SendWeather | 6 | 3 | Low |
| SendCreateMob | 5 | 5 (GetCreateMob, GetCreateMobTrade, cSock) | Medium |
| SendEnvEffectKingdom | 5 | 4 (GridMulticast, pMobGrid) | Medium |
| SendRemoveParty | 5 | 3 | Low |
| SendWarInfo | 5 | 3 | Medium |
| SendGridMob | 4 | 6 (SendCreateMob, SendPKInfo, SendCreateItem, grids) | Medium |
| SendCargoCoin | 4 | 2 | Low |
| SendChat | 3 | 3 | Low |
| SyncMulticast | 3 | 3 | Low |
| SendAutoTrade | 3 | 3 | Medium |
| SendCreateItem | 3 | 3 | Low |
| SendShopList | 3 | 4 (BASE_GetVillage, g_pGuildZone) | Medium |
| SendNoticeChief | 2 | 5 (ChargedGuildList, SendClientMessage) | Medium |
| SendSummonChief | 2 | 6 (ChargedGuildList, DoTeleport) | Medium |
| SendClientMessageOk | 1 | 3 | Low |
| SendClientSignalParm2 | 1 | 2 | Low |
| SendClientSignalParm3 | 2 | 2 | Low |
| SendClientSignalShortParm2 | 2 | 2 | Low |
| SyncKingdomMulticast | 2 | 4 (KingChat, pMob) | Medium |
| SendRemoveItem | 2 | 2 | Low |
| SendAffect | 2 | 3 | Medium |
| SendGuildList | 2 | 7 (BASE_GetGuildName, g_pGuildWar, g_pGuildAlly, tables) | Medium |
| SendReqParty | 2 | 3 | Low |
| SendShortSignalParm2Area | 2 | 3 (SendClientSignalShortParm2) | Medium |
| PartyGridMulticast | 1 | 5 (pMobGrid, pUser, CloseUser) | Medium |
| SendMessageArea | 1 | 2 | Low |
| SendEnvEffect (see SendEnvEffect) | - | - | - |

Note: coupling counts are approximate, derived by grep over all `.cpp` files in `legacy/Code` (excluding the definition sites).

---

## 7. Endpoints

The component does **not** expose network endpoints (REST/GraphQL/gRPC). It is an internal server-to-client messaging layer. Its "interface" is the set of 46 C function prototypes declared in `SendFunc.h`, which constitute the outbound packet API consumed by the rest of the map server. The actual wire endpoints are the TCP connections managed by `CPSock` (the game protocol), onto which these builders write `MSG_*` packets.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Game clients | External (game client processes) | Deliver all authoritative state updates | TCP (Winsock2), custom binary protocol | `MSG_*` structs with `_MSG` header (Size/KeyWord/CheckSum/Type/ID/ClientTick) | `AddMessage` FALSE → `CloseUser`; multicast errors logged |
| GetFunc component | Internal (TMSrv) | Serialize game state into packets | C function calls | `MSG_CreateMob`, `MSG_CreateMobTrade`, `MSG_CreateItem`, affects, guilt | Direct calls; no error propagation |
| Server global state | Internal (TMSrv/Server) | Read/write authoritative world state | C global arrays | `pUser`, `pMob`, `pItem`, `pMobGrid`, `pItemGrid` | Guarded by mode/socket/bounds checks |
| Language table | Internal (Language.h) | Localized system messages | C global array | `g_pMessageStringTable` | Direct indexed access |
| DB server | Indirect (via ProcessDBMessage) | Cross-server guild/zone data | C messages | `g_pGuildWar`, `g_pGuildAlly`, `ChargedGuildList` | Resolved via `BASE_GetGuildName` |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Free-function utility module | Flat set of 46 C functions in one translation unit | SendFunc.cpp | Procedural outbound packet construction; no class state |
| Packet header macro (`_MSG`) | `#define _MSG` expands to Size/KeyWord/CheckSum/Type/ID/ClientTick | Basedef.h:925 | Uniform wire header for every message |
| Builder (packet assembly) | Each `Send*` constructs a stack `MSG_*` struct then enqueues | Throughout SendFunc.cpp | Serialize server state into protocol messages |
| Template method (signal families) | `SendClientSignal` → `Parm` → `Parm2` → `Parm3` share identical shape | SendFunc.cpp:197-258 | Progressive payload extension |
| Observer / publish-subscribe (broadcast) | `GridMulticast`, `MapaMulticast`, `SendNotice`, `SyncMulticast` iterate viewer set | SendFunc.cpp:260-1053 | Fan-out to in-range/in-scope clients |
| Spatial partition (visibility grid) | `pMobGrid`/`pItemGrid` 4096x4096 grid with 33x33 view window | SendFunc.cpp:620-823 | O(viewport) entity visibility reconciliation |
| Flyweight / shared singleton globals | Single global arrays `pUser`, `pMob` accessed by all functions | Server.h | Shared authoritative state |
| Anti-corruption / facade | SendFunc shields callers from packet layout and encryption details | SendFunc.cpp | Isolate wire-format concerns from game logic |
| Hand-rolled encryption | `pKeyWord` XOR/shift transform in `CPSock::AddMessage` | CPSock.cpp:554-588 | Obfuscate packet payload on the wire |
| Sentinel ID (`ESCENE_FIELD` = 30000) | Broadcast/scene packets use a reserved server ID | Basedef.h:170 | Distinguish server-originated messages |

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | SendClientSignal family, SendCreateItem, SendRemoveMob, SendRemoveItem, SendEmotion, SendScore, SendGuildList | Missing `conn` bounds guard before `pUser[conn]`/`pMob[conn]` access (e.g., SendClientSignal:205, SendScore:1136, SendGuildList:1259) | Out-of-range index → undefined behavior / crash on corrupted caller input |
| High | SendGuildList, SendNotice, SendNoticeChief | Unbounded `sprintf`/`strcat` into fixed `char[128]`/`char[512]` buffers (SendFunc.cpp:51, 68, 1266-1297) | Stack buffer overflow potential from long messages/names |
| High | SendEnvEffectLeader | Duplicated condition `if(tmob == Pista[4].Party[1].LeaderID)` twice (lines 475 and 480); third leader ID never checked (likely intended Party[2]) | Leader-exclusion logic incorrect; extra users may be excluded/included in effect broadcast |
| High | GridMulticast, SendEquip, SendCreateMob | Hard-coded magic values: costume region 896..1150 x 1405..1538, items 3505/3999, mount range 2360..2389, castle region 2604..2648 x 1708..1744 | Undocumented constants; broken if map/event data changes |
| High | SendGuildList / SendWarInfo / SendShopList | Hard-coded `max_guild = 65536` sentinel and `MAX_SHOPLIST` indexing arithmetic (SendFunc.cpp:1305, 1392-1399) | Magic bounds not tied to array sizes; fragile |
| Medium | SendAddParty | Lossy HP normalization `> 32000 ? (x+1)/100 : x` (1496-1497) | HP display precision loss for high-level characters |
| Medium | Multiple | `memcpy(..., MESSAGE_LENGTH)` (96) into buffers sized differently (e.g., MSG_MessagePanel.String[128]) with manual null-padding | Relies on exact constant alignment; copy sizes not derived from field size |
| Medium | Broadcast loops (SyncMulticast, MapaMulticast, SendMessageArea) | `AddMessage` return value ignored; no `CloseUser` on failure | Potential infinite accumulation on dead sockets; delayed disconnect |
| Medium | SendScore / SendAffect | Reliance on global flags (`BrState`, `NewbieEventServer`) and magic time thresholds (32000000, 3600, AFFECT_1D) | Cross-cutting event logic hidden in a messaging function |
| Medium | SendGuildList | `members` counted but `SendClientMessage(conn, str)` chunking uses `strlen`/`max_size=70` with `strcat`; no realloc guard | Long member lists may overflow the stack buffer across chunk boundaries |
| Low | GridMulticast | `msg != NULL` cast to `MSG_Action*` unconditionally (line 813) and dereferenced | Null or non-Action message → undefined behavior |
| Low | Component-wide | No automated tests, no documentation of the packet contract, no versioning | Changes to wire format or guards are high-risk and unverifiable |

---

## 11. Test Coverage Analysis

**No automated tests exist for the `SendFunc` component or the legacy codebase as a whole.**

An exhaustive search across the entire `legacy/` folder for test artifacts (filenames or directories matching `test`, `spec`, `unit`, `gtest`, `unittest`) returned zero results. The only test-related files on the repository are inside `.opencode/node_modules` (third-party JavaScript dependencies of the tooling harness, e.g., `effect/dist/testing`, `zod/src/v3/tests`), which are excluded per the `ignore-folders` parameter and are unrelated to the C/C++ codebase. The `W2PP Code Project.sln` / `.vcxproj` files reference no test projects.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| SendClientMessage | 0 | 0 | 0% | N/A — no tests |
| GridMulticast | 0 | 0 | 0% | N/A — no tests |
| SendScore / SendAffect | 0 | 0 | 0% | N/A — no tests |
| SendEquip (mount logic) | 0 | 0 | 0% | N/A — no tests |
| All other Send* functions | 0 | 0 | 0% | N/A — no tests |

This is a **High-severity risk**: the component contains complex, side-effecting logic (view-grid reconciliation, mount encoding, HP normalization, level-up rewards, PK-flag aggregation) that is entirely unverified by automated means. The business rules documented in Section 3 are implicit in the code and would need to be reverse-engineered into test fixtures to enable any regression coverage.

---

## 12. Notes on Scope and Confidence

- Component boundary was taken as the `SendFunc.h`/`SendFunc.cpp` translation unit pair in `legacy/Code/TMSrv/`, matching the requested "outbound packet builders" scope. Supporting files (`Basedef.h`, `Server.h`, `GetFunc.*`, `CPSock.*`) were analyzed only as dependencies.
- Folders `.git` and `.opencode` were excluded from the analysis.
- Coupling counts (Section 6) are static call-site counts and are approximate; they include calls from test-less production code only.
- Business rules labeled with magic values are documented with confidence "high" that they encode intended behavior (they are executed unconditionally on live paths), but their *semantic intent* (e.g., exact meaning of item 3505/3999 or the castle region) is inferred and not documented anywhere in the source; such inference is flagged in Section 10.
- The duplicate `Pista[4].Party[1].LeaderID` condition (SendFunc.cpp:475 vs 480) is flagged as a likely latent bug rather than intentional, based on the pattern of checking distinct party leaders in adjacent branches.

---

*End of Component Deep Analysis Report.*
