# MSG_RemoveMob

## 1. Summary

| Property | Value |
|---|---|
| Type constant | `_MSG_RemoveMob` = `(101 \| FLAG_GAME2CLIENT)` = `0x0165` = `357` (`Basedef.h:1582`) |
| Sequence ID | 101 |
| Direction(s) | **TMSrv → Client only** (`FLAG_GAME2CLIENT`). Server-initiated despawn/removal of a mob from the client's world. **No** `FLAG_CLIENT2GAME` bit, and there is **no** `case _MSG_RemoveMob` in the client→game inbound dispatcher (`ProcessClientMessage.cpp` case list spans 68-320+ with no such case) — confirmed server-only. |
| Wire struct | `MSG_RemoveMob` (`Basedef.h:1583-1588`) — `_MSG` header + `int RemoveType` |
| Total size | **16 bytes** (`sizeof(MSG_RemoveMob)` = 12 header + 4 payload; see §3.4) |
| Packing | **Default MSVC `/Zp8`** — NOT in any `#pragma pack(push,1)` region (pack(1) regions: `Basedef.h:808-835`, `1212-1246`, `1465-1492`, `2063-2097`; this struct at 1583-1588 falls outside all of them) |
| Producer (direct) | `SendRemoveMob(int dest, int sour, int Type, int bSend)` @ `TMSrv/SendFunc.cpp:494-508` — sends to a **single** target user socket |
| Producer (broadcast) | `DeleteMob(int conn, int Type)` @ `TMSrv/Server.cpp:7020-7054` — builds the frame and broadcasts via `GridMulticast` to all players in view |
| Aliases | Low byte 101 is used **only** by `_MSG_RemoveMob`. Neighboring constants share the flag but not the sequence: `_MSG_NoViewMob` (105, `Basedef.h:1580`, bidirectional), `_MSG_PKInfo` (102, `Basedef.h:1590`), `_MSG_CreateItem` (110, `Basedef.h:1592`). |
| Related | `_MSG_CreateMob` (spawn/create the mob in view), `_MSG_CNFMobKill` (kill confirmation), `_MSG_NoViewMob` (client request that re-syncs a single mob via `SendRemoveMob`) |

## 2. Wire Framing

Standard W2PP framing (`CPSock.cpp`):
- Connection opens with 4-byte `INITCODE = 0x1F11F311` magic before any framed message.
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating XOR transform keyed by `KeyWord` (index into shared `pKeyWord[512]`).
- `CheckSum` = `Sum2 - Sum1` (raw vs. transformed payload sums); validated on receive.
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` else the buffer is reset.
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is **disabled** (body commented out, returns `FALSE`) — but the commented-out central validation for this packet was `m->Size != sizeof(MSG_RemoveMob)` → `code = 1` (`Basedef.cpp:6507`).

Per-packet notes:
- No deviation from standard framing. It is a plain server→client despawn frame.
- Both producers set `Type = _MSG_RemoveMob` (0x0165) and `Size = sizeof(MSG_RemoveMob)` = 16, then `ID = sour`/`conn` and `RemoveType = Type`.
- `SendRemoveMob` sends the frame directly to one socket: `pUser[dest].cSock.AddMessage((char*)&sm, sizeof(MSG_RemoveMob))`, then `SendMessageA()` only if `bSend` is set (`SendFunc.cpp:504-507`).
- `DeleteMob` broadcasts via `GridMulticast(pMob[conn].TargetX, pMob[conn].TargetY, (MSG_STANDARD*)&sm, conn)` (`Server.cpp:7030`), which re-sends the same 16-byte buffer to every player in the mob's view window.
- `KeyWord`, `CheckSum`, `ClientTick` are left as 0 by the producers (`memset`); the transport layer assigns/validates `KeyWord`/`CheckSum`.

## 3. Binary Layout

### 3.1 Header (12 bytes, `_MSG` macro, `Basedef.h:925-930`)

| Offset | Size | Field | Type | Description |
|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | Total packet size incl. header (expected 16) |
| 2 | 1 | `KeyWord` | `char` | Transport obfuscation table index |
| 3 | 1 | `CheckSum` | `char` | Transport checksum (`Sum2 - Sum1`) |
| 4 | 2 | `Type` | `short` | `_MSG_RemoveMob` = 0x0165 |
| 6 | 2 | `ID` | `short` | **Mob slot being removed** (`sour` in `SendRemoveMob`, `conn` in `DeleteMob`) |
| 8 | 4 | `ClientTick` | `unsigned int` | 0 from producers; transport-owned |

### 3.2 Payload

Packing context: **default `/Zp8`** (not a pack(1) region — see §1). Each member aligned to `min(sizeof(member), 8)`; the struct size rounds up to the largest member alignment (4, from `int`/`unsigned int`).

Header ends at offset 12, which is already 4-aligned; `int RemoveType` has alignment 4. **No padding anywhere.**

| Offset | Size | Field | Type | Align | Pad | Description |
|---|---|---|---|---|---|---|
| 12 | 4 | `RemoveType` | `int` | 4 | 0 | Despawn reason/type. Struct comment: `1 : morte (death), 2 : logout` (`Basedef.h:1587`). In practice callers pass 0, 1, 2, or 3 — see §6. |

**Total payload: 4 bytes → total struct 16 bytes.**

### 3.3 Nested struct expansions

`MSG_RemoveMob` embeds only the `_MSG` header macro (primitive fields) plus a flat `int`. It contains **no `STRUCT_*` members**, so there are no nested expansions.

### 3.4 Size verification

| Check | Value | Source |
|---|---|---|
| Header (`_MSG`) | `short(2)+char(1)+char(1)+short(2)+short(2)+unsigned int(4)` = 12 | `Basedef.h:925-930` |
| Payload | `int(4)` at offset 12 (12 is 4-aligned → no pad) | `Basedef.h:1587` |
| `sizeof(MSG_RemoveMob)` | 12 + 4 = **16**; 16 % 4 = 0 → no tail padding | — |
| `Size` field set to | `sizeof(MSG_RemoveMob)` = 16 in both producers | `SendFunc.cpp:500` (`SendRemoveMob`), `Server.cpp:7026` (`DeleteMob`) |
| Central (disabled) check expected | `m->Size == sizeof(MSG_RemoveMob)` = 16 | `Basedef.cpp:6507` |
| Cross-check | `SendRemoveMob` adds exactly `sizeof(MSG_RemoveMob)` bytes to the socket; `GridMulticast` uses `msg->Size` | `SendFunc.cpp:504`, `Server.cpp:7030` |

No mismatch between build size (16), the (disabled) central check (16), and the computed layout (16).

## 4. Lifecycle & Flow

This packet is **GAME2CLIENT only** — there is no client→game inbound dispatch leg (no `case _MSG_RemoveMob` in `ProcessClientMessage.cpp`). All flows are server→client.

### 4.1 Producer A — `SendRemoveMob` (direct, single recipient)

```
[Server logic decides a mob is no longer visible to one player]
   └─ SendRemoveMob(dest, sour, Type, bSend)          (SendFunc.cpp:494)
        ├─ memset(&sm, 0, sizeof(MSG_RemoveMob))      (:497)
        ├─ sm.Type = _MSG_RemoveMob (0x0165)          (:499)
        ├─ sm.Size = sizeof(MSG_RemoveMob) = 16       (:500)
        ├─ sm.ID = sour  (mob slot to remove)         (:501)
        ├─ sm.RemoveType = Type                       (:502)
        ├─ pUser[dest].cSock.AddMessage(&sm, 16)      (:504)  -- queue to ONE socket
        └─ if (bSend) pUser[dest].cSock.SendMessageA() (:506-507)
```

Call sites of `SendRemoveMob`:
- **View-region sync** inside `GridMulticast(int conn, int tx, int ty, MSG_STANDARD *msg)` (`SendFunc.cpp:620`): when an entity moves, cells that fall outside the new view window cause mutual removal — `SendRemoveMob(tmob, conn, 0, 0)` and `SendRemoveMob(conn, tmob, 0, 0)` (`SendFunc.cpp:718-722`). `Type=0`.
- **`_MSG_NoViewMob` handler** (`TMSrv/_MSG_NoViewMob.cpp:34,46,49`): when a player requests a specific mob and it is not in view (or is `MOB_EMPTY`, or is not a playing user), the server answers with `SendRemoveMob(conn, MobID, 0, 0)` — an explicit "that mob is not here" despawn. `Type=0`.
- **`_MSG_Attack`** (`TMSrv/_MSG_Attack.cpp:319,336,349`): after certain combat cases where a target must be removed from the attacking player's view, `SendRemoveMob(conn, idx, 1, 0)` is used. `Type=1` (death-like).

### 4.2 Producer B — `DeleteMob` (broadcast to view grid)

```
DeleteMob(conn, Type)                                  (Server.cpp:7020)
   ├─ memset(&sm, 0, sizeof(MSG_RemoveMob))            (:7023)
   ├─ sm.Type = _MSG_RemoveMob; sm.Size = 16           (:7025-7026)
   ├─ sm.ID = conn; sm.RemoveType = Type               (:7027-7028)
   ├─ GridMulticast(pMob[conn].TargetX, pMob[conn].TargetY,
   │      (MSG_STANDARD*)&sm, conn)                    (:7030) -- every player in view
   └─ if (Type != 0):                                  (:7032)
        ├─ if conn >= MAX_USER (a mob, not a PC):      (:7034)
        │    ├─ decrement generator count             (:7036-7044)
        ├─ pMob[conn].MOB.CurrentScore.Hp = 0         (:7047)
        ├─ pMob[conn].Mode = 0                        (:7048)
        ├─ pMobGrid[TargetY][TargetX] = 0             (:7049)
        └─ RemoveParty(conn)                          (:7051)
```

`DeleteMob` call sites (representative) show the trigger taxonomy:
- **Death** (`Type=1`): `MobKilled.cpp:199,637,1704,1990,2030,2037,2044`; `_MSG_Attack.cpp:1678,1700`; `ProcessSecMinTimer.cpp:1369,1382,1574`; `Server.cpp:6640,9037`; `CWarTower.cpp:100`; `DeleteMobMapa` (`Server.cpp:8716` → `:8726`).
- **Logout / despawn-reset** (`Type=2`): `Server.cpp:4356,6002,6005,6919,7118` (e.g. zone/instance cleanup, logout).
- **Transformation / scripted removal** (`Type=3`): `_MSG_Buy.cpp:306,309`; `_MSG_UseItem.cpp:2817`; `CCastleZakum.cpp:142,336,371`; `Server.cpp:4207,5389,7391,7394,7445,7526,9388`; `ProcessSecMinTimer.cpp:769,1396,1493,1510,1544,1589,1625`; `MobKilled.cpp:1368,1417`.

### 4.3 ASCII flow

```
                TMSrv                                             Client(s)
                  │                                                    │
  [mob dies | despawns | leaves view | logout | script]
                  │                                                    │
  DeleteMob(conn,T) ──── build MSG_RemoveMob{Size=16, Type=0x165, ID=conn, RemoveType=T}
        │              GridMulticast(x,y,&sm,conn) ──────────────► [each player in view]
        │                    (Server.cpp:7030, SendFunc.cpp:843)         despawn mob ID=conn
        │
  SendRemoveMob(dest,sour,T,bSend) ── AddMessage(&sm,16) ───────► [one player]
        │                    (SendFunc.cpp:494-507)                    despawn mob ID=sour
        ▼
  [server-side cleanup: Hp=0, Mode=0, grid cleared, gen count,
   RemoveParty]  (Server.cpp:7047-7051, only when Type!=0)
```

## 5. Validation & Guards

This packet is produced server-side only; there is **no** inbound dispatch, so the client-side validation of this frame is not implemented in this TMSrv codebase (client binary is closed). The only server-side guard is in the disabled central size check. `DeleteMob` itself performs grid cleanup unconditionally when `Type != 0`.

| # | Guard / Check | Condition → Action | Source |
|---|---|---|---|
| 1 | Central (disabled) size check | `m->Type == _MSG_RemoveMob && m->Size != sizeof(MSG_RemoveMob)` → `code = 1` — never enforced (`BASE_CheckPacket` body commented out) | `Basedef.cpp:6507`, `Basedef.cpp:6475` |
| 2 | `DeleteMob` conn validity | implicit via callers; `pMob[conn]` indexed before broadcast. No explicit range guard inside `DeleteMob` | `Server.cpp:7020-7030` |
| 3 | Grid cleanup gate | `if (Type != 0)` gates the generator-count decrement + `Mode`/`Hp`/grid reset + `RemoveParty` | `Server.cpp:7032-7052` |
| 4 | Generator bound | `geneidx >= 0 && geneidx < MAX_NPCGENERATOR` before `CurrentNumMob--`; clamped at 0 | `Server.cpp:7036-7044` |
| 5 | `SendRemoveMob` bSend | frame queued via `AddMessage` always; `SendMessageA()` only when `bSend` truthy | `SendFunc.cpp:504-507` |
| 6 | NoViewMob range | `MobID <= 0 \|\| MobID >= MAX_MOB` → log + return (before any `SendRemoveMob`) | `_MSG_NoViewMob.cpp:26-30` |

## 6. Game Mechanics & Business Logic

- **Purpose**: tells a client to despawn/remove mob slot `ID` from its rendered world. The client needs (a) the mob slot to remove — carried in `ID` — and (b) the removal reason/type — carried in `RemoveType`.
- **`RemoveType` semantics** (from the struct comment, `Basedef.h:1587`): `1 : morte (death)`, `2 : logout`. The value drives the client's despawn animation/cleanup choice.
  - Observed callers use a wider set than the two documented values:
    - `0` — view-region sync (`SendFunc.cpp:718-722`) and NoViewMob negative answer (`_MSG_NoViewMob.cpp:34,46,49`); also `SendRemoveMob` from `_MSG_Attack` used 1. Value 0 = "just remove from view", no death/logout semantics.
    - `1` — death (`MobKilled.cpp`, `_MSG_Attack.cpp:1678,1700`).
    - `2` — logout / instance despawn (`Server.cpp:4356,6002,6919,7118`).
    - `3` — transformation/scripted removal (`_MSG_Buy.cpp`, `CCastleZakum.cpp`, `_MSG_UseItem.cpp:2817`). Meaning on the client for 0/3 UNKNOWN.
- **`ID`** = the mob's global slot in `pMob[]`. In `DeleteMob`, `ID = conn`; in `SendRemoveMob`, `ID = sour`. The client indexes its local entity table by this slot to remove the entity.
- **Broadcast model**: `DeleteMob` uses `GridMulticast` centered on the mob's `TargetX/TargetY` with `skip = conn`, so every player whose view window contains the mob receives the despawn; the removed mob itself (if a PC, `conn < MAX_USER`) does not receive its own frame.
- **Direct model**: `SendRemoveMob` targets one socket explicitly, used for per-player view-region sync and NoViewMob responses.
- **Server-side cleanup** is coupled to the packet only in `DeleteMob` and only when `Type != 0`: `Hp=0`, `Mode=0`, grid cell cleared, generator `CurrentNumMob` decremented (clamped ≥0), and `RemoveParty(conn)` (`Server.cpp:7047-7051`). `SendRemoveMob` performs **no** server-side state cleanup — it is purely a client-visibility operation.

## 7. Side Effects

| # | Effect | Detail | Source |
|---|---|---|---|
| 1 | Client despawn | Recipient removes mob `ID` from the world per `RemoveType` | `Server.cpp:7030`, `SendFunc.cpp:504` |
| 2 | View-region reconciliation | Moving entities push removals for out-of-view cells on both sides (mob↔PC) | `SendFunc.cpp:718-722` |
| 3 | Mob state reset (Type!=0) | `MOB.CurrentScore.Hp = 0`, `Mode = 0` | `Server.cpp:7047-7048` |
| 4 | Grid cell cleared | `pMobGrid[TargetY][TargetX] = 0` | `Server.cpp:7049` |
| 5 | NPC generator decrement | `mNPCGen.pList[geneidx].CurrentNumMob--`, clamped ≥0 (mobs only, `conn >= MAX_USER`) | `Server.cpp:7034-7044` |
| 6 | Party removal | `RemoveParty(conn)` when `Type != 0` | `Server.cpp:7051` |
| 7 | NoViewMob negative answer | Client that asked for a non-visible mob gets a RemoveMob to force desync-clearing | `_MSG_NoViewMob.cpp:34,46,49` |

## 8. Related Packets

- `_MSG_CreateMob` (`Basedef.cpp:6506` has its size check; built in `SendCreateMob`) — the spawn/create counterpart that adds mob `ID` to a client's view; RemoveMob is its inverse.
- `_MSG_CNFMobKill` (`Basedef.cpp:6505`) — kill confirmation packet emitted alongside death-path `DeleteMob`/`SendRemoveMob(…, 1, …)`.
- `_MSG_NoViewMob` (`105|GAME2CLIENT|CLIENT2GAME`, `Basedef.h:1580`) — client request that indirectly triggers `SendRemoveMob` responses (`_MSG_NoViewMob.cpp`).
- `_MSG_PKInfo` (`102|GAME2CLIENT`, `Basedef.h:1590`) — sent by `SendPKInfo`; used in the same visibility-reconciliation path (see `_MSG_NoViewMob.cpp:43` pairing with `SendCreateMob`/`SendPKInfo`).
- `_MSG_DecayItem` / `_MSG_CreateItem` (`Basedef.h:1592,1609`) — the item-spawn/despawn analogs (`SendRemoveItem`, `SendFunc.cpp:510`).

## 9. Discrepancies & Open Questions

1. **`RemoveType` wider than documented.** The struct comment (`Basedef.h:1587`) documents only `1 : morte (death)`, `2 : logout`, but callers also pass `0` (view-sync / NoViewMob) and `3` (transform/scripted). The client-side meaning of `0` and `3` is UNKNOWN (closed client).
2. **`SendRemoveMob` vs `DeleteMob` split.** Two separate producers for the same wire packet: `SendRemoveMob` is single-recipient and does **no** state cleanup; `DeleteMob` is grid-broadcast and performs full server-side cleanup only when `Type != 0`. This split is intentional in source but worth confirming it matches client expectations (a `Type=0` from `DeleteMob` would skip cleanup — `DeleteMob` is never actually called with `Type=0` in the codebase).
3. **No inbound leg.** Confirmed there is no `case _MSG_RemoveMob` in `ProcessClientMessage.cpp`; the packet is strictly GAME2CLIENT. `ClientTick`/`KeyWord`/`CheckSum` are zeroed by producers and filled by the transport — no application-level meaning.
4. **Central size validation disabled** (`Basedef.cpp:6475`), so the `m->Size == sizeof(MSG_RemoveMob)` check at `Basedef.cpp:6507` is never enforced at runtime.
5. **`RemoveType=3` usage sites** (`_MSG_Buy.cpp`, `_MSG_UseItem.cpp:2817`, `CCastleZakum.cpp`) remove mobs on shop/transform/script actions but the exact client despawn animation for `3` is UNKNOWN.

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `legacy/Code/Basedef.h` | 925-930 | `_MSG` header macro (Size/KeyWord/CheckSum/Type/ID/ClientTick) |
| `legacy/Code/Basedef.h` | 932-941 | `FLAG_GAME2CLIENT`/etc. |
| `legacy/Code/Basedef.h` | 1580 | `_MSG_NoViewMob` |
| `legacy/Code/Basedef.h` | 1582 | `_MSG_RemoveMob = (101 \| FLAG_GAME2CLIENT)` = 0x0165 |
| `legacy/Code/Basedef.h` | 1583-1588 | `MSG_RemoveMob` struct (`_MSG` + `int RemoveType`) |
| `legacy/Code/Basedef.h` | 1590,1592 | `_MSG_PKInfo`, `_MSG_CreateItem` |
| `legacy/Code/Basedef.cpp` | 6475 | `BASE_CheckPacket` (disabled) |
| `legacy/Code/Basedef.cpp` | 6507 | Disabled central size check for `MSG_RemoveMob` |
| `legacy/Code/TMSrv/SendFunc.cpp` | 494-508 | `SendRemoveMob` producer (direct) |
| `legacy/Code/TMSrv/SendFunc.cpp` | 620-726 | `GridMulticast(int conn,…)` view-region sync; RemoveMob at 718-722 |
| `legacy/Code/TMSrv/SendFunc.cpp` | 843-972 | `GridMulticast(int tx,int ty,…)` broadcast |
| `legacy/Code/TMSrv/SendFunc.h` | 41 | `SendRemoveMob` prototype |
| `legacy/Code/TMSrv/Server.cpp` | 7020-7054 | `DeleteMob` producer (broadcast + cleanup) |
| `legacy/Code/TMSrv/Server.h` | 136 | `DeleteMob` prototype |
| `legacy/Code/TMSrv/Server.cpp` | 8716-8726 | `DeleteMobMapa` (→ `DeleteMob(i,1)`) |
| `legacy/Code/TMSrv/_MSG_NoViewMob.cpp` | 20-50 | NoViewMob handler using `SendRemoveMob` |
| `legacy/Code/TMSrv/_MSG_Attack.cpp` | 319,336,349,1678,1700 | Death-path `SendRemoveMob`/`DeleteMob` |
| `legacy/Code/TMSrv/MobKilled.cpp` | 199,637,1704,1990,2030,2037,2044 | Death-path `DeleteMob(target,1)` |
| `legacy/Code/TMSrv/ProcessClientMessage.cpp` | 68-320+ | Inbound dispatch (contains **no** `_MSG_RemoveMob` case) |
| `legacy/Code/TMSrv/_MSG_Buy.cpp` | 306,309 | `DeleteMob(TargetID,3)` |
| `legacy/Code/TMSrv/CCastleZakum.cpp` | 142,336,371 | `DeleteMob(tmob,3)` |
| `legacy/Code/TMSrv/_MSG_UseItem.cpp` | 2817 | `DeleteMob(tmob,3)` |
