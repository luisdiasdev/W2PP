# Component Deep Analysis Report

**Component:** UpdateItem Handler (`Exec_MSG_UpdateItem`)

**Primary location:** `Code/TMSrv/_MSG_UpdateItem.cpp` (106 lines, 2,667 bytes)

**Secondary locations (delegation and callee bodies analysed for this report):**

- `Code/TMSrv/CCastleZakum.cpp:72-208` (`CCastleZakum::OpenCastleGate`)
- `Code/TMSrv/Server.cpp:7472-7556` (`UpdateItem`)
- `Code/Basedef.cpp:5292-5339` (`BASE_UpdateItem`)
- `Code/Basedef.cpp:1647-1842` (`BASE_GetItemAbility`)
- `Code/TMSrv/ProcessClientMessage.cpp:180-182` (dispatch site)
- `Code/Basedef.h:1836-1847` (wire contract)

**Analysis scope:** entire project root `E:\wyd\test-setup\w2pp`

**Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`

**Report date:** 2026-11-15

---

## 1. Executive Summary

`Exec_MSG_UpdateItem` is a 106-line free function that implements the world-gate / door interaction handler of TMSrv. It receives the bidirectional game message `_MSG_UpdateItem` (protocol tag `116`, wire value `884` / `0x374`), validates the session and the requested gate/state pair, delegates Castle Zakum doors to the event module, then performs a keyed unlock, forces the gate to `STATE_OPEN`, and re-broadcasts the request packet to the 33x33 view window.

**Role in the system**

- It is one of 61 handler translation units declared by `Code/TMSrv/ProcessClientMessage.h:101` and dispatched by the single `switch` in `Code/TMSrv/ProcessClientMessage.cpp:180-182`.
- It is the **only** client-reachable entry point that can change `CItem::State` for a gate (all other callers of `UpdateItem` are server-internal: `Reboot`, `SetArenaDoor`, `SetColoseumDoor`, `SetColoseumDoor2`, `SetCastleDoor`, `ProcessSecMinTimer`, `imple.cpp` GM commands, `MobKilled`).
- It is the **only** caller of `CCastleZakum::OpenCastleGate` in the whole tree, making it the coupling seam between the generic gate subsystem and the Castle Zakum event module.

**Key findings**

1. **The requested state is ignored.** Line 98 always calls `UpdateItem(gateid, STATE_OPEN, ...)`. A client can only ever *open* a gate through this handler; closing/locking is exclusively server-side. The `State` value from the packet is used only as a *decision input* (key requirement predicate) and as *echoed payload* in the broadcast.
2. **The fallback key logic is a divergent duplicate** of `CCastleZakum::OpenCastleGate` (`Code/TMSrv/_MSG_UpdateItem.cpp:54-90` vs `Code/TMSrv/CCastleZakum.cpp:74-103`). The copy silently drops the `Quest == CastleQuestLevel` validation and the entry-gate party/timer orchestration.
3. **`UpdateItem` (`Code/TMSrv/Server.cpp:7472-7556`) never reaches its map-eviction code.** `pItem[Gate].State` is assigned at line 7481, so the `if (itemstate == state)` test at line 7495 is always true and the function returns at line 7504. The 44-line eviction loop (7510-7553) and the `CreateMob("GATE", ...)` branch (7499-7500) are unreachable dead code for **every** caller.
4. **`g_pGroundMask` is never populated in this source tree** (definition `Code/Basedef.cpp:173`, reads only). Consequently `BASE_UpdateItem` performs no height-map mutation, so opening a gate has no server-side collision effect; the physical door behaviour is delegated to the client.
5. **No proximity, no ownership/existence, and no message-length validation.** Unlike `_MSG_GetItem` (`Code/TMSrv/_MSG_GetItem.cpp:74-82`) which enforces a 3-tile radius, this handler will open any keyed gate anywhere in the world for a player who merely carries the key item. `BASE_CheckPacket` — which contains a `_MSG_UpdateItem` size rule at `Code/Basedef.cpp:6517` — has its entire body commented out and always returns `FALSE` (`Code/Basedef.cpp:6620-6622`), so the length rule is inert.
6. **The client's own packet is fanned out verbatim** (`GridMulticast(..., (MSG_STANDARD*)m, 0)`, line 101), so spectators receive the *client-requested* `State`, `ID`, `Size`, `KeyWord`, and `ClientTick`, not the authoritative server state. The server-internal pattern (e.g. `Code/TMSrv/Server.cpp:6078-6088`) builds a fresh message with `ID = ESCENE_FIELD (30000)` and `State = pItem[].State`.
7. **Zero test coverage.** No test project, framework, harness, or assertion exists anywhere in the repository (evidence in section 13).

---

## 2. Data Flow Analysis

```
 1. Client sends Type=_MSG_UpdateItem (884), Size=20, ID=<conn>, payload {ItemID, State}
 2. CPSock::ReadMessage decodes the header/keyword obfuscation and checksum
    (Code/CPSock.cpp:386-467); only Size > 8192 or Size < 12 are rejected
 3. Server.cpp:3975-4001 (WSA_READ) -> ProcessClientMessage(User, Msg, FALSE)
 4. ProcessClientMessage: validates std->ID in [0, MAX_USER), ping filter,
    ClientTick==SKIPCHECKTICK filter  (Code/TMSrv/ProcessClientMessage.cpp:42-64)
 5. switch(std->Type) case _MSG_UpdateItem -> Exec_MSG_UpdateItem(conn, pMsg)
 6. Session/liveness gate: pMob[conn].MOB.CurrentScore.Hp == 0 || pUser[conn].Mode != USER_PLAY
    -> SendHpMode(conn) + AddCrackError(conn,1,16) + return
 7. Request-state domain gate: 0 <= m->State <= 5, else AddCrackError(conn,50,50) + return
 8. Item-namespace gate: 10000 <= m->ItemID < 15000 (10000 + MAX_ITEM), else AddCrackError(50,50) + return
 9. gateid = m->ItemID - 10000 ; redundant re-check gateid in [0, MAX_ITEM) (dead branch)
10. Delegation: CCastleZakum::OpenCastleGate(conn, gateid, m)
      -> returns FALSE only when gatekey < 10 || gatekey > 14 (every other outcome is owned)
      -> TRUE  => handler returns; castle flow (key consume, entry gate orchestration,
                  UpdateItem, GridMulticast, Log) already performed inside the module
11. Fallback key predicate: key required only when pItem[gateid].State == 3 (LOCKED)
      or m->State == 3 ; gatekey == 0 => no key needed
12. Key search: linear scan i in [0, MAX_CARRY=64) over pMob[conn].MOB.Carry[i],
      match on BASE_GetItemAbility(..., EF_KEYID=39); EF_QUEST=58 read but unused
13. Key possession failure: if g_pMessageStringTable[79] (_NN_No_Key) is sent,
      unless pItem[gateid].ITEM.sIndex == 773 ; return
14. Key consumption: memset(Carry[i], 0) + SendItem(conn, ITEM_PLACE_CARRY, i, ...)
15. State application: UpdateItem(gateid, STATE_OPEN, &heigth)
      -> BASE_UpdateItem(GridCharge, CurrentState, STATE_OPEN, PosX, PosY, pHeightGrid, Rotate, &height)
      -> rejects illegal transition (delta == 0) -> return FALSE
      -> mask height pass over g_pGroundMask[GridCharge][Rotate][6][6] (all-zero in this tree)
      -> pItem[gateid].State = 1 ; .Height = height ; .Delay = 0
      -> returns TRUE unconditionally at Server.cpp:7495-7505
16. Broadcast: if (isUpdate) GridMulticast(pItem[gateid].PosX, pItem[gateid].PosY,
      (MSG_STANDARD*)m, 0)  -> 33x33 view window, every mob index in pMobGrid
17. Audit: sprintf(temp, "etc,opengate gateid:%d X:%d Y:%d", ...) ; Log(temp, AccountName, IP)
18. No DB persistence, no event emission, no response to the initiating client
```

**Data transformation summary**

| Stage | Input | Output | Owner |
|-------|-------|--------|-------|
| Wire decode | framed bytes | `MSG_Standard + 8` bytes | `CPSock::ReadMessage` |
| Interpretation | `char *pMsg` | `MSG_UpdateItem {ItemID, State}` | handler line 22 |
| Normalisation | `ItemID` | `gateid = ItemID - 10000` | handler line 42 |
| Authorisation | `gateid`, `conn`, carry | `BOOL handled` | `CCastleZakum::OpenCastleGate` |
| Authorisation (fallback) | `gateid`, carry, `State` | consume / reject | handler lines 54-90 |
| Application | `gateid`, `STATE_OPEN` | `CItem::State/Height/Delay` | `UpdateItem` → `BASE_UpdateItem` |
| Projection | `pMsg` | multicast to view grid | `GridMulticast` |
| Audit | gateid, X, Y | text log file | `Log` |

---

## 3. Business Rules & Logic

## Overview of the business rules

| # | Rule Type | Rule Description | Location |
|---|-----------|------------------|----------|
| BR-01 | Validation (session) | Reject when HP == 0 or `pUser[conn].Mode != USER_PLAY` (22); respond with `SendHpMode` and `AddCrackError(conn,1,16)` | `Code/TMSrv/_MSG_UpdateItem.cpp:24-29` |
| BR-02 | Validation (domain) | Accepted request state is `0 <= State <= 5`; violation accumulates `AddCrackError(conn,50,50)` | `Code/TMSrv/_MSG_UpdateItem.cpp:30-34` |
| BR-03 | Validation (identity) | Gate identity namespace is `ItemID ∈ [10000, 15000)`; violation accumulates `AddCrackError(conn,50,50)` | `Code/TMSrv/_MSG_UpdateItem.cpp:36-40` |
| BR-04 | Normalisation | `gateid = ItemID - 10000` with a redundant bound re-check `[0, MAX_ITEM)` | `Code/TMSrv/_MSG_UpdateItem.cpp:42-49` |
| BR-05 | Delegation | Castle Zakum precedence: if `OpenCastleGate` returns TRUE the handler yields entirely | `Code/TMSrv/_MSG_UpdateItem.cpp:51-52`; `Code/TMSrv/CCastleZakum.cpp:72-208` |
| BR-06 | Business Logic | Kingdom classification by `EF_KEYID`: `10` = castle entry gate, `11..14` = inner castle doors, everything else = generic gate | `Code/TMSrv/CCastleZakum.cpp:79-80,106,188` |
| BR-07 | Validation (quest) | Castle gate keys must carry `EF_QUEST == CastleQuestLevel` (entry gate exempt); mismatched keys are rejected as "no key" | `Code/TMSrv/CCastleZakum.cpp:91,97` |
| BR-08 | Business Logic | Entry gate (`KEYID 10`) starts/joins the Castle Zakum run: cleans the arena, spawns the run's mob table, starts the countdown, registers the party | `Code/TMSrv/CCastleZakum.cpp:106-185` |
| BR-09 | Validation (concurrency) | A second Castle Zakum run is refused while `CastleQuestTime != -1`; the requester is told who owns the run | `Code/TMSrv/CCastleZakum.cpp:111-127` |
| BR-10 | Business Logic (fallback) | Key needed only when current state is LOCKED (3) or requested state is 3; `EF_KEYID == 0` gates never need a key | `Code/TMSrv/_MSG_UpdateItem.cpp:59-63` |
| BR-11 | Business Logic (fallback) | Key possession is proven by one matching `EF_KEYID` in the 64 carry slots; the quest tag is read but not enforced | `Code/TMSrv/_MSG_UpdateItem.cpp:69-79` |
| BR-12 | Validation (fallback) | Missing key → `_NN_No_Key` client message, suppressed for item `sIndex 773`; no state change | `Code/TMSrv/_MSG_UpdateItem.cpp:81-87` |
| BR-13 | Business Logic (inventory) | A successful unlock destroys exactly one matching key item and resyncs that carry slot | `Code/TMSrv/_MSG_UpdateItem.cpp:89-90`; `Code/TMSrv/CCastleZakum.cpp:195-196` |
| BR-14 | Business Logic | Target state is hard-coded `STATE_OPEN` (1); `m->State` never becomes the applied state | `Code/TMSrv/_MSG_UpdateItem.cpp:98` |
| BR-15 | State machine | Only `OPEN→(CLOSED|LOCKED)` and `(CLOSED|LOCKED)→OPEN` are legal; `delta == 0` aborts | `Code/Basedef.cpp:5300-5306` |
| BR-16 | State machine | Gate lock states at boot: all init items are created OPEN, then every `EF_KEYID ∈ [1,14]` gate is force-set to LOCKED | `Code/TMSrv/Server.cpp:7178-7186,7280` |
| BR-17 | Business Logic (timer) | A keyed gate left OPEN for two consecutive minute ticks is relocked (`Delay` latch: 0→1→lock) | `Code/TMSrv/ProcessSecMinTimer.cpp:2142-2209` |
| BR-18 | Projection | On a successful state change the request packet is multicast to the 33x33 view grid, caster included | `Code/TMSrv/_MSG_UpdateItem.cpp:100-101`; `Code/TMSrv/SendFunc.cpp:843` |
| BR-19 | Audit | An `etc,opengate` record with gateid/X/Y, account and IP is written on **every** path that reaches line 103 | `Code/TMSrv/_MSG_UpdateItem.cpp:103-104` |
| BR-20 | Anti-cheat | Tampering accumulates 50 penalty points; ≥ 2,000,000,000 forces `CharLogOut` | `Code/TMSrv/Server.cpp:684-707` |
| BR-21 | Uniqueness | `pItem[0]` is never allocatable; valid item indices start at 1 | `Code/TMSrv/Server.cpp:6754-6763` |
| BR-22 | Latent capability | Treasure-box interception hook is disabled by comment | `Code/TMSrv/_MSG_UpdateItem.cpp:93-94` |

## Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Session liveness and play-mode gate

**Overview**:
Every gate interaction is discarded unless the caller is a living character in `USER_PLAY` mode; the client is resynchronised and the anomaly is scored.

**Detailed description**:
The first statement of the handler reads two independent pieces of entity state: `pMob[conn].MOB.CurrentScore.Hp` and `pUser[conn].Mode`. Both must indicate a live, in-world character. `USER_PLAY` is `22` (`Code/TMSrv/CUser.h:36`), the state entered only after the DB confirms a character selection; a session still in `USER_SELCHAR` (11) or in any transitional mode is therefore rejected before any world object is touched. The `Hp == 0` half of the predicate is what blocks dead-but-still-connected characters, a state TMSrv keeps for a short window while the death/revive sequence runs.

The response is deliberately split in two. `SendHpMode` (`Code/TMSrv/SendFunc.cpp:1622-1645`) emits `_MSG_SetHpMode` with the authoritative `Hp` and `Mode`, so a desynchronised client repairs itself, and the call closes the connection itself if `AddMessage` fails. `AddCrackError(conn, 1, 16)` then adds 1 point to `pUser[conn].NumError` and, because `Type == 16` is not one of the silent types (3, 8, 15), writes a `cra point: 1  type: 16` log line (`Code/TMSrv/Server.cpp:684-690`).

This is the weakest of the three anti-tamper tiers in the handler. A normal logged-out client that spams the packet gains 1 point per packet, so it takes 2×10^9 packets to trigger the forced `CharLogOut`; contrast BR-02/BR-03 which charge 50 points per packet. The rule is functionally a resynchronisation rule with a faint forensic side effect rather than a real brake.

**Rule workflow**:
```
if (Hp == 0) OR (Mode != USER_PLAY):
    SendHpMode(conn)                 -> _MSG_SetHpMode {Hp, Mode}
    AddCrackError(conn, 1, 16)       -> NumError += 1; log "cra point: 1  type: 16"
    return                           -> no state change, no multicast, no audit record
else:
    continue to BR-02
```

---

### Business Rule: BR-02 — Request-state domain (0..5) with tamper scoring

**Overview**:
The `State` field of `MSG_UpdateItem` is constrained to the closed interval `[0,5]`; any value outside it is treated as a tampering attempt, never as a state request.

**Detailed description**:
`MSG_UpdateItem` carries `int State` (`Code/Basedef.h:1846`) with no enum typing, so the raw client value is a full 32-bit signed integer. The handler rejects anything outside 0..5 at line 30 and charges 50 crack points. Because `AddCrackError(conn,50,50)` is invoked with `Type == 50`, the call is also logged (`Code/TMSrv/Server.cpp:686-690`) with the account name and IP, producing an actionable forensic trail for repeated attempts.

The accepted domain is broader than the domain the rest of the system can actually honour. The gate state machine defines exactly three states: `STATE_OPEN = 1`, `STATE_CLOSED = 2`, `STATE_LOCKED = 3` (`Code/Basedef.h:1835-1837`). States `0`, `4` and `5` therefore pass validation but cannot produce a transition, because BR-14 forces the target to `1` and BR-15 only permits `2→1` and `3→1`. Such requests fall through to `UpdateItem`, which returns `FALSE`, and the handler still writes the `etc,opengate` audit line, producing a log entry for a no-op. This is an accepted-domain vs. meaningful-domain mismatch, not a crash path: the numeric range check is effective as an anti-fuzz guard.

The upper bound of 5 is also meaningful defensively. It caps the value early, so downstream code that stores the client value into an `int` field (`sm.State`, `MSG_UpdateItem` reinterpretation at `Code/TMSrv/Simple`-style broadcast sites) never receives an out-of-band integer derived from an unbounded value.

**Rule workflow**:
```
if (State < 0) OR (State > 5):
    AddCrackError(conn, 50, 50)   -> NumError += 50; log "cra point: 50  type: 50"
    return
else:
    continue to BR-03
Note: 0, 4, 5 are accepted here but cannot produce a legal transition (BR-15),
      so they terminate as a no-op after BR-19 logs the attempt.
```

---

### Business Rule: BR-03 — Gate identity namespace (ItemID offset 10000)

**Overview**:
Ground objects are addressed over the wire by a synthetic identifier `index + 10000`; the handler accepts only `ItemID ∈ [10000, 15000)` and derives the array index by subtraction.

**Detailed description**:
`pItem` is a fixed array of `MAX_ITEM = 5000` `CItem` records (`Code/TMSrv/CItem.h:45`, `Code/Basedef.h:103`). The wire format never exposes raw indices; every ground-object message (`_MSG_GetItem`, `_MSG_DropItem`, `_MSG_CreateItem`, `_MSG_UpdateItem`) uses the `+10000` offset — visible in the producer side at `Code/TMSrv/GetFunc.cpp:1205` and `Code/TMSrv/Server.cpp:7298,9434`. The handler mirrors that contract: `m->ItemID - 10000` is the only accepted mapping, and the check at line 36 rejects anything below 10000 (so raw indices, negative values and 0 are all invalid) or at or above 15000 (so out-of-array indices are excluded before the subtraction, avoiding signed-overflow tricks such as `INT_MIN - 10000`).

Notably, the check does **not** verify that the addressed slot is currently in use. `pItem[gateid].Mode` is never inspected, and `pItemGrid[PosY][PosX]` is never consulted, whereas `_MSG_GetItem` performs both the `Mode == 0` rejection and the `pItemGrid[y][x] != itemID` consistency check (`Code/TMSrv/_MSG_GetItem.cpp:55,164-172`). A freed slot has `Mode == 0` but retains its last `State`, `ITEM`, `GridCharge`, `Rotate`, `PosX/PosY`, so a request aimed at a decayed gate remains addressable. Whether this is exploitable depends on BR-15: an all-zero slot has `State == 0` and cannot transition, but a slot whose gate was destroyed while `State == 3` (for example a treasure gate cleared by `CreateTreasureBox`/`Mode = 2` lifecycle) could still pass.

The boundary arithmetic is also duplicated: BR-04 re-tests the derived index, which the identity check has already made impossible. The duplication is harmless but hides the fact that only one of the two checks is load-bearing.

**Rule workflow**:
```
ItemID = m->ItemID
if (ItemID < 10000) OR (ItemID >= 10000 + 5000):
    AddCrackError(conn, 50, 50)
    return
gateid = ItemID - 10000            -> guaranteed in [0,4999]
(no existence check on pItem[gateid].Mode, no pItemGrid consistency check)
```

---

### Business Rule: BR-04 — Index normalisation with redundant re-validation

**Overview**:
`gateid` is derived, then re-validated against `MAX_ITEM`; the second test is unreachable-true and serves only as defensive documentation.

**Detailed description**:
Line 42 computes `gateid = m->ItemID - 10000` after BR-03 has already established `10000 <= ItemID < 15000`, and line 45 re-tests `gateid < 0 || gateid >= MAX_ITEM`. Given `MAX_ITEM == 5000` and the preceding range, both disjuncts are provably false, so the branch at lines 45-49 can never execute. This is a defensive-copy artefact: the same idiom appears in handlers where the subtraction is done first and validated second (a shape closer to `_MSG_GetItem.cpp:50-55`).

The practical consequence is that this dead branch also makes `AddCrackError(conn,50,50)` appear three times in a 30-line span, which in a binary-diff review makes the two live guards harder to distinguish from the dead one. It has no behavioural effect and is safe.

`gatestate` (line 43) is assigned from `m->State` and never read; the live copy used later is `gatestate2` (line 55). The pair is a decompiler artefact of a variable that was reused in the original binary.

**Rule workflow**:
```
gateid    = ItemID - 10000
gatestate = m->State            (dead store)
if (gateid < 0) OR (gateid >= 5000):   -> never true after BR-03
    AddCrackError(conn, 50, 50); return
continue to BR-05
```

---

### Business Rule: BR-05 — Castle Zakum delegation precedence

**Overview**:
Before any generic gate logic runs, the Castle Zakum module is offered the request; if it claims the gate the handler returns without executing a single line of its own key or update logic.

**Detailed description**:
Line 51 calls `CCastleZakum::OpenCastleGate(conn, gateid, m)` and `return`s immediately when the result is `TRUE`. The module's claim predicate is purely the item's `EF_KEYID` (`Code/TMSrv/CCastleZakum.cpp:79-80`): values `10` to `14` are claimed and every other value — including `0` (ordinary door), `1..9` (non-castle keys) and `15+` (arena/colosseum/treasure keys) — releases the request back to the handler. The name of the return value is therefore misleading: `TRUE` means *"this gate belongs to Castle Zakum and the request has been fully processed"*, not *"the door opened"*. Rejections inside the module (no key, wrong quest level, run already active) also return `TRUE` (`Code/TMSrv/CCastleZakum.cpp:102,109,126,191`), so a failed castle unlock is still a terminal outcome for the handler.

This precedence ordering is what keeps the two key schemes from conflicting. Because `OpenCastleGate` owns `KEYID 10..14` unconditionally, the fallback block in the handler only ever sees `KEYID ∈ {0, 1..9, 15, 16..}` — which is exactly why its own `EF_KEYID == 0` exemption (BR-10) and its single-key match (BR-11) are sufficient, and why the missing `Quest` check there is not directly exploitable: a castle key can never reach the fallback path.

The module performs the same tail as the handler (`UpdateItem(gateid, STATE_OPEN, ...)`, `GridMulticast`, `Log`, `Code/TMSrv/CCastleZakum.cpp:198-207`), which means the open-animation, the audit string and the multicast are byte-identical between the two paths — including the shared misspelled local `heigth`.

**Rule workflow**:
```
handled = CCastleZakum::OpenCastleGate(conn, gateid, m)
   gatekey = EF_KEYID(pItem[gateid].ITEM)
   if gatekey not in [10,14]: return FALSE
   ... quest/party/timer orchestration (BR-06..BR-09) ...
   consume key, UpdateItem(STATE_OPEN), GridMulticast, Log
   return TRUE
if (handled == TRUE) return;     -> handler body below is skipped entirely
continue to BR-10
```

---

### Business Rule: BR-06 — Key-domain classification (generic vs. castle vs. arena)

**Overview**:
`EF_KEYID` (`39`) is the single discriminator that partitions the world's gated objects into behavioural families; the bound `10..14` is the Castle Zakum band.

**Detailed description**:
`EF_KEYID` is effect id `39` (`Code/ItemEffect.h:79`), stored in one of the three `stEffect` slots of `STRUCT_ITEM` and read back through `BASE_GetItemAbility`, which sums every matching effect slot and applies the sanctification multiplier for non-structural effect types (`Code/Basedef.cpp:1647-1842`). Because `EF_KEYID` is read through the generic ability resolver rather than a dedicated accessor, the returned `keyid` is the *total* key id of the item, not the raw byte in `stEffect[0]` — a distinction that matters if an item carries the same effect in two slots.

Three bands are observable in the tree: `EF_KEYID == 0` (ungated door, handled entirely by the fallback path), `EF_KEYID == 15` (arena / colosseum / castle-inner door variant, referenced at `Code/TMSrv/Server.cpp:6092-6095` and `Code/TMSrv/Server.cpp:7499` where a `"GATE"` NPC is spawned on close), and `EF_KEYID ∈ [10,14]` (Castle Zakum: 10 = entry gate, 11..14 = the four interior doors). Values `1..9` belong to the generic keyed-gate family used by the dungeon/castle doors created in `Reboot` (`Code/TMSrv/Server.cpp:7178-7186`) and relocked by the minute timer (`Code/TMSrv/ProcessSecMinTimer.cpp:2157`).

The classification lives in the callee rather than the handler, so the handler is unaware of *why* a gate was claimed. Adding a new key band therefore requires no change here but does require the new band to be excluded from the fallback path, or its quest semantics will silently degrade (see BR-11).

**Rule workflow**:
```
gatekey = BASE_GetItemAbility(&pItem[gateid].ITEM, EF_KEYID)
gatekey == 0        -> generic, keyless       -> handler fallback (BR-10)
gatekey in [1,9]    -> generic keyed          -> handler fallback (BR-10..BR-13)
gatekey == 10       -> Castle entry gate      -> CCastleZakum (BR-07..BR-09)
gatekey in [11,14]  -> Castle inner door      -> CCastleZakum (BR-07, BR-09)
gatekey == 15       -> arena/colosseum door   -> handler fallback + "GATE" NPC on close
gatekey > 15        -> generic keyed variant  -> handler fallback
```

---

### Business Rule: BR-07 — Quest-tag validation on castle keys

**Overview**:
A castle key is only accepted when its `EF_QUEST` (`58`) value equals the currently active `CastleQuestLevel`, except for the entry gate (`KEYID 10`), which is allowed to *set* the level.

**Detailed description**:
The scan at `Code/TMSrv/CCastleZakum.cpp:86-95` reads both `EF_KEYID` and `EF_QUEST` for every carry slot and continues until it finds a key whose id matches the gate **and** whose quest tag matches `CastleQuestLevel`, unless the gate is the entry gate. The entry-gate exemption is what lets the party leader's key *start* a run: `CastleQuestLevel` is `-1` before the first run, so a strict comparison would deadlock the feature.

The consequence is a per-run key economy: `CCastleZakum::KeyDrop` (`Code/TMSrv/CCastleZakum.cpp:299-312`) stamps `EF_QUEST = CastleQuestLevel` onto any dropped key in the `11..14` band before handing it to the killer, so interior keys are only usable inside the run that produced them. When the run ends, `CastleQuestLevel` remains at the last value while `CastleQuestTime` returns to `-1`, so leftover keys become stale-but-valid only for a future run of the same level.

The fallback path in the handler reads the same `EF_QUEST` value (line 76) into a local `Quest` that is never used, which is the clearest evidence that the fallback block was copy-adapted from this rule and the quest comparison was removed rather than refactored. Since the fallback never sees `KEYID 10..14` (BR-05), the omission is currently inert.

**Rule workflow**:
```
for i in [0, MAX_CARRY):
    key   = EF_KEYID(Carry[i])
    Quest = EF_QUEST(Carry[i])
    if key == gatekey AND (Quest == CastleQuestLevel OR gatekey == 10): break
if key != gatekey OR (Quest != CastleQuestLevel AND gatekey != 10):
    send _NN_No_Key (unless sIndex == 773); return TRUE
```

---

### Business Rule: BR-08 — Castle Zakum entry-gate orchestration

**Overview**:
Opening the entry gate (`KEYID 10`) with a valid quest key converts a bare door interaction into a full event start: arena purge, mob table spawn, countdown, leader election and party registration.

**Detailed description**:
The entry-gate branch (`Code/TMSrv/CCastleZakum.cpp:106-185`) validates the quest index against `[0, MAX_CASTLE_QUEST)` first, so a key carrying an unconfigured level is rejected without side effects. It then purges the reserved rectangle `x ∈ [2180,2296], y ∈ [1160,1269]` by walking `pMobGrid`, skipping anything below `MAX_USER` (i.e. players), marking each NPC generator's `MinuteGenerate = -1` and calling `DeleteMob(tmob, 3)` — a swept cleanup that guarantees a run never inherits mobs from a previous or parallel run.

Spawning then iterates the configured `CastleQuest[Quest].MOB_INITIAL..MOB_END` and calls `GenerateMob(x, 0, 0)` twice per generator, deliberately setting `MinuteGenerate = 1` for non-boss generators so the minute scheduler keeps replenishing them. `CastleQuestLevel` and `CastleQuestTime = QuestTime - 1` are then published to the module globals, which is what BR-07 and BR-09 read on subsequent interactions.

Leader resolution is `pMob[conn].Leader`, defaulting to `conn` when the caller is solo, so a party member can open the door and still have the run attributed to the leader. The leader name is copied into `CastleLeader`, `_MSG_StartTime` is signalled to the leader and to every `PartyList` member in `USER_PLAY`, and `CastleQuestParty[0..MAX_PARTY]` is refreshed from `pMob[Leader].PartyList` with the leader appended at index `MAX_PARTY`. That table is the whitelist consulted by `CCastleZakum::CheckMove` to keep non-participants out of the arena.

**Rule workflow**:
```
if gatekey == 10:
    if Quest not in [0, MAX_CASTLE_QUEST): return TRUE          (reject)
    if CastleQuestTime != -1: announce owner + count; return TRUE (BR-09)
    purge mobs in [2180..2296]x[1160..1269]; MinuteGenerate = -1; DeleteMob(...,3)
    for g in [MOB_INITIAL..MOB_END]: MinuteGenerate = 1 (non-boss); GenerateMob x2
    CastleQuestLevel = Quest ; CastleQuestTime = QuestTime - 1
    Leader = pMob[conn].Leader ?: conn ; CastleLeader = pMob[Leader].MOB.MobName
    SendClientSignalParm(Leader, ESCENE_FIELD, _MSG_StartTime, QuestTime)
    for each party member in USER_PLAY: signal _MSG_StartTime
    CastleQuestParty[0..MAX_PARTY-1] = pMob[Leader].PartyList ; [MAX_PARTY] = Leader
```

---

### Business Rule: BR-09 — Single active run / inner-door gating

**Overview**:
At most one Castle Zakum run may exist at a time; a competing open request is answered with the current owner and party size, and interior doors refuse keys from the wrong level.

**Detailed description**:
The concurrency guard is a single global sentinel, `CastleQuestTime`, whose value `-1` means "no run". While it differs, the module counts the live entries of `CastleQuestParty` (skipping `0` and values ≥ `MAX_USER`) and formats `_SD_Zakum_Quest_by_S_N` with the leader name and the count before returning `TRUE` — a refusal that consumes the handler's request and produces no state change. The run is later torn down by `CCastleZakum::ProcessSecTimer` (`Code/TMSrv/CCastleZakum.cpp:314-343`), which at `CastleQuestTime == 0` clears the arena and returns the sentinel to `-1`.

Interior doors (`KEYID 11..14`) have their own, weaker check: `if (Quest != CastleQuestLevel) return TRUE;` (`Code/TMSrv/CCastleZakum.cpp:190-191`). Nothing verifies that the key holder is *in* `CastleQuestParty`, and nothing verifies proximity; a member of a rival party who obtained a key for the active level can open interior doors. Because the doors are one-way in this handler (BR-14), the practical effect is early access rather than denial of service.

The two checks also answer with different fidelity: the entry gate names the owner, the inner gate returns silently. This asymmetry means a rejected inner-door attempt leaves no client-visible trace and no audit line, since the handler has already returned at BR-05 and the module does not log before the refusal at line 191.

**Rule workflow**:
```
if gatekey == 10 and CastleQuestTime != -1:
    count live CastleQuestParty members; format owner/count; send; return TRUE
if gatekey in [11,14] and Quest != CastleQuestLevel:
    return TRUE                              (silent refusal, no log)
otherwise: consume key -> UpdateItem(STATE_OPEN) -> GridMulticast -> Log
```

---

### Business Rule: BR-10 — Key requirement predicate (fallback path)

**Overview**:
Outside Castle Zakum, a key is required only when the interaction touches the LOCKED state (3) — either the gate is currently locked or the client asked for state 3 — and never when the gate item carries no key id.

**Detailed description**:
Lines 59-63 implement a two-clause exemption before the expensive 64-slot scan: `if (state != 3 && gatestate2 != 3) goto NoNeedKey;` and `if (gatekey == 0) goto NoNeedKey;`. The first clause removes the key cost for transitions that never involve the locked state, i.e. opening an already-unlocked (`STATE_OPEN`) or `STATE_CLOSED` door, and for "close" requests on doors that are open. The second removes it for keyless props such as town doors and decorative gates.

The first clause is the more interesting one, because `state` is `pItem[gateid].State`, a server-owned value, while `gatestate2` is client-supplied. Taking the union (`&&` of two negations) means a client can *force* the key requirement by requesting `State = 3` for a gate that is currently unlocked (`STATE_OPEN`). That is the only client-controlled input that can make the server demand a key, and the cost of a false demand is that the player receives `_NN_No_Key` and nothing happens. It cannot be used to skip a key requirement, since skipping requires `State` to be neither 3 nor the current state 3 for a locked gate — impossible for a locked gate whose current state is already 3.

Combined with BR-14 (target state is always `STATE_OPEN`) and BR-15 (`2→1`, `3→1` only), the predicate reduces in practice to: *locked gates always need a key; open gates never do; closed-but-unlocked gates never do*.

**Rule workflow**:
```
if pItem[gateid].State != 3 AND m->State != 3: goto NoNeedKey
if gatekey == 0:                              goto NoNeedKey
-> key scan (BR-11)
NoNeedKey:
   UpdateItem(gateid, STATE_OPEN)  (BR-14/BR-15)
```

---

### Business Rule: BR-11 — Key possession proof over the carry array (fallback path)

**Overview**:
Possession is proven by `BASE_GetItemAbility(Carry[i], EF_KEYID) == gatekey` for some `i` in `[0, 64)`; the matching key's quest tag is read but discarded.

**Detailed description**:
The loop at lines 69-79 walks all 64 carry slots (`MAX_CARRY = 64`, `Code/Basedef.h:76`) and calls `BASE_GetItemAbility` twice per iteration in the Castle module (key id and quest) or once in the handler (key id, then quest only on a match). Matching is by key id only — the effect value, not the item index — so any item carrying the correct `EF_KEYID` opens the door regardless of its name, sanctification or other effects. Because `BASE_GetItemAbility` sums matching slots and applies the sanc multiplier to non-structural effects, an item with `EF_KEYID` in `stEffect[0]` and again in `stEffect[1]` yields the sum, which will not match a single-value gate key; this is a theoretical compatibility constraint on item design rather than an exploitable path.

The scan terminates on the first match (`break`), so the consumed key is always the lowest-indexed match, and the count is effectively 1 regardless of duplicates. Because the loop also exits on `key != gatekey` continue, the sentinel `key` after a failed scan is the **last** slot's key id, and the check at line 81 (`gatekey != key`) is therefore equivalent to "no slot matched" — sound, but sensitive to the loop's last-iteration value rather than to an explicit found flag.

The semantic divergence from BR-07 is the important part: the fallback accepts *any* key with the right id, and the `Quest` local (line 66, written at line 76) is never compared. This is a pure copy artefact; it only becomes a live defect if a new quest-tagged key band is placed below 10 or above 14.

**Rule workflow**:
```
key = 0; Quest = -1; i = 0
for i in [0,64):
    key = EF_KEYID(Carry[i])
    if key != gatekey: continue
    Quest = EF_QUEST(Carry[i])        (dead value)
    break
if gatekey != key: goto BR-12
```

---

### Business Rule: BR-12 — Missing-key feedback with sIndex 773 suppression

**Overview**:
A failed possession proof sends `_NN_No_Key` ("You do not have the key") and aborts, except for ground items whose template index is `773`, which fail silently.

**Detailed description**:
`g_pMessageStringTable[_NN_No_Key]` is index `79` (`Code/TMSrv/Language.h:99`), the localised catalog loaded by `BASE_InitializeMessage("Language.txt")` during boot (`Code/TMSrv/Server.cpp:7160`). `SendClientMessage` writes a `MSG_STANDARD`-style text payload through `AddMessage` on the session socket (`Code/TMSrv/SendFunc.cpp:27ff`). The user-visible effect is a modal chat line; there is no dedicated failure opcode in this protocol.

The `sIndex != 773` guard is the only place in the component that references a literal item template index, and its meaning cannot be confirmed from this repository because the item catalogue (`ItemList.csv` / `ItemList.bin`) lives outside the tracked tree (`Code/Basedef.cpp:2748,2835` reference `../../Common/ItemList.bin`; no `Common` directory is present). What can be stated with certainty is the behaviour: item `773` is a gated prop whose failed interaction must be silent — consistent with a decorative or quest-flagged gate that the client already knows is locked, where a "no key" toast would be noise or a spoiler. The same guard appears identically in `CCastleZakum::OpenCastleGate` (`Code/TMSrv/CCastleZakum.cpp:99`), confirming it is a shared contract of the gate subsystem rather than a one-off.

Note the asymmetry with BR-03: the handler validates the *identity namespace* but never checks whether the addressed slot holds item 773 versus any other index, so the suppression test reads a possibly-stale `sIndex` from a freed slot.

**Rule workflow**:
```
if gatekey != key:
    if pItem[gateid].ITEM.sIndex != 773:
        SendClientMessage(conn, g_pMessageStringTable[79])
    return                      (no state change, no multicast, no audit line)
```

---

### Business Rule: BR-13 — Single-key consumption and slot resync

**Overview**:
A successful unlock destroys exactly one matching key item in the carry array and pushes the emptied slot back to the client.

**Detailed description**:
On a successful match the handler (line 89) and the Castle module (line 195) both `memset` the matched `STRUCT_ITEM` to zero and immediately call `SendItem(conn, ITEM_PLACE_CARRY, i, ...)` (`ITEM_PLACE_CARRY == 1`, `Code/Basedef.h:87`) so the client clears the icon without a full inventory refresh. `SendItem` short-circuits when the connection is out of range, not in `USER_PLAY`, or has no socket (`Code/TMSrv/SendFunc.cpp:1055-1064`), which makes the call safe even in the scripted/GM paths that reuse `UpdateItem`.

Because the slot is zeroed before the state change is attempted, a *failed* state transition after a successful key match would still consume the key. In the current code that cannot happen for the handler's own path: BR-15 accepts `1→3`/`3→1`/`2→1`, and the handler only ever requests `1`, so any gate whose key was matched is in state 2 or 3 (BR-10) and the transition is legal. The window exists, however, for a future caller that requests a state the machine rejects.

There is no server-side inventory-slot guard beyond the loop index: `i` is guaranteed `< 64` because the only path reaching the `memset` is the one that broke out of the loop on a match, and the `pMob[conn].MOB.Carry[i]` access is therefore in bounds by construction. This is one of the few places in the tree where an index derived from a scan is provably safe.

**Rule workflow**:
```
if gatekey == key (match at index i):
    memset(&pMob[conn].MOB.Carry[i], 0, sizeof(STRUCT_ITEM))
    SendItem(conn, ITEM_PLACE_CARRY, i, &pMob[conn].MOB.Carry[i])
    -> fall through to NoNeedKey
```

---

### Business Rule: BR-14 — Server-forced target state `STATE_OPEN`

**Overview**:
Regardless of `m->State`, the applied transition is always to `STATE_OPEN` (1); the handler can open but never close or lock a gate.

**Detailed description**:
Line 98 is the only state-writing call in the handler and it hard-codes `STATE_OPEN`. The client's `State` value never reaches `UpdateItem`, so the two request shapes the protocol nominally supports — open and close — collapse into one: open. Closing is exclusively a server decision, executed by `CCastleZakum::ProcessSecTimer` (`CastleQuestTime == 0` path), `ProcessSecMinTimer`'s minute relock (BR-17), the arena/colosseum schedulers (`SetArenaDoor`, `SetColoseumDoor`, `SetColoseumDoor2`, `SetCastleDoor`), and GM commands in `Code/TMSrv/imple.cpp:315-415`.

This is a sound authority split (clients may request entry, the server owns denial-of-entry), but it interacts badly with BR-18: the broadcast echoes `m`, whose `State` field still holds whatever the client sent. A client that requests `State = 3` on an unlocked door causes the server to open it and simultaneously tells every observer in the 33x33 window that it is `LOCKED` — the classic "server state and client presentation disagree" divergence. Whether the W2 client renders from `State` or re-requests is not determinable from the server source.

A second consequence is that the handler has no "already open" fast path: a second request for an already-open gate proceeds through BR-10 (no key needed), reaches `UpdateItem`, fails BR-15 (`delta == 0`), and falls to BR-19's audit line — a logged no-op rather than a silent one.

**Rule workflow**:
```
isUpdate = UpdateItem(gateid, STATE_OPEN, &heigth)
isUpdate == FALSE -> BR-15 rejected the transition (still reaches BR-19)
isUpdate == TRUE  -> state applied (and, per BR-18, broadcast)
m->State is never used as a target state
```

---

### Business Rule: BR-15 — Gate state machine (transition legality)

**Overview**:
A gate may only move between the open state and a locked/closed state; `BASE_UpdateItem` encodes this as a `delta` of `+1`/`-1` and returns `FALSE` for every other pair.

**Detailed description**:
`BASE_UpdateItem` (`Code/Basedef.cpp:5292-5339`) computes `delta = +1` for `1→(3|2)` and `delta = -1` for `(3|2)→1`; `delta == 0` returns `FALSE`. That is the entire state machine: two legal directions, no self-transitions, and `STATE_CLOSED` (2) treated as interchangeable with `STATE_LOCKED` (3) for transition purposes. The `delta` then multiplies every non-zero cell of `g_pGroundMask[maskidx][rotate][y][x]` and adds it into `pHeightGrid`, clamped to `[0,255]`, with out-of-map cells aborting the row.

Two observations reduce the practical scope of this rule in the current tree. First, `g_pGroundMask` is never written anywhere (definition `Code/Basedef.cpp:173`; all other references are reads at `Basedef.cpp:5312,5331,5352` and `TMSrv/Server.cpp:7514`), so every mask cell is zero, the inner body is skipped, and the height-grid mutation is a no-op. Second, `MAX_GROUNDMASK == 6` and `maskidx` is `pItem[...].GridCharge`, which `UpdateItem` re-checks — so the "illegal mask" branch in `UpdateItem` (`Server.cpp:7489-7493`) is also unreachable, because `BASE_UpdateItem` would already have returned `FALSE` for the same condition.

The consequence is that gate *collision* is entirely client-side: the server tracks `State` and broadcasts it, but `pHeightGrid[y][x] == 127` (the impassable marker written by `BASE_ApplyAttribute`, `Code/Basedef.cpp:2583-2606`) is never toggled by a gate. `BASE_GetRoute` (`Code/Basedef.cpp:5538ff`) therefore sees the same walkability before and after an unlock.

**Rule workflow**:
```
BASE_UpdateItem(GridCharge, CurrentState, NextState, PosX, PosY, pHeight, Rotate, &height):
    if maskidx outside [0,6): return FALSE
    delta = +1 if Current==1 and Next in {2,3}
          = -1 if Next==1 and Current in {2,3}
          =  0 otherwise
    if delta == 0: return FALSE          <- the state machine
    for mask cells: height += val*delta, clamped [0,255]   <- inert (mask all zero)
    return TRUE
UpdateItem: update == 0 -> return FALSE ; else State=Next, Height=height, Delay=0, return TRUE
```

---

### Business Rule: BR-16 — Boot-time gate initialisation (OPEN then LOCKED)

**Overview**:
Every `InitItem.csv` prop is created `STATE_OPEN`; immediately afterwards `Reboot` force-locks every gate whose `EF_KEYID` is in `[1,14]`.

**Detailed description**:
`CreateItem` (`Code/TMSrv/Server.cpp:7254-7317`) sets `Mode = 1`, `State = STATE_OPEN`, `Delay = 90`, `GridCharge = EF_GROUND`, and `Height = pHeightGrid[y][x]`, then multicasts `_MSG_CreateItem` — but during `Reboot` there are no clients yet, so the multicast is wasted work. The subsequent loop (`Server.cpp:7166-7186`) re-reads `EF_KEYID` for each created item and calls `UpdateItem(ID, STATE_LOCKED, &Height)` when the id is non-zero and `< 15`.

Only `KEYID 15` and ids above 14 escape the lock, so the arena/colosseum doors start open while every castle, dungeon and keyed door starts locked. That asymmetry is the reason `EF_KEYID 15` props are the ones `SetCastleDoor` re-creates a `"GATE"` NPC for when closing (`Code/TMSrv/Server.cpp:6092-6095`): they are physically represented by an NPC rather than by the item's own collision, and the state must be mirrored into that NPC.

Because BR-15 rejects `1→3` only when the current state is not 1, the boot lock is a legal `1→3` transition and succeeds for every keyed prop. Note that the initial `State = STATE_OPEN` plus `Delay = 90` means the minute-timer relock (BR-17) is already armed for keyed gates before any player connects.

**Rule workflow**:
```
for each InitItem:
    CreateItem(x, y, &Item, rotate, 1)   -> Mode=1, State=STATE_OPEN, Delay=90,
                                            GridCharge=EF_GROUND, Height=pHeightGrid[y][x]
    keyid = EF_KEYID(Item)
    if keyid != 0 and keyid < 15:
        UpdateItem(ID, STATE_LOCKED)     -> gate now locked at boot
```

---

### Business Rule: BR-17 — Minute-timer automatic relock (interaction with BR-10)

**Overview**:
A keyed gate that has been open for two consecutive minute ticks is relocked automatically, using the `Delay` field as a one-tick latch.

**Detailed description**:
`ProcessSecMinTimer` iterates from `i = 17` of the init-item table (`Code/TMSrv/ProcessSecMinTimer.cpp:2142`), resolves the item through `pItemGrid`, skips empty or out-of-catalogue items, and — for any gate with a non-zero `EF_KEYID` that is currently `STATE_OPEN` and has `keyid < 15` — uses `pItem[...].Delay` as a latch: `0 → 1` on the first tick (with `continue`), then `UpdateItem(ipg, 3, ...)`, an explicit `State = STATE_LOCKED`, `Delay = 0`, and a `_MSG_CreateItem` re-broadcast. The result is a two-minute window for keyed gates: the door opens, stays open for at most two minute ticks, then returns to `STATE_LOCKED`.

This rule is what makes the handler's "open" a *temporary grant* rather than a persistent toggle, and it is why BR-14's inability to close is not a loss of function. It is also the strongest argument that `UpdateItem`'s internal `Delay = 0` reset (`Server.cpp:7487`) is load-bearing: without it, the handler's own open would leave `Delay` at whatever the boot value (90) was and the latch would never arm.

The relock writes `_MSG_CreateItem` rather than `_MSG_UpdateItem`, i.e. it re-creates the door for observers instead of animating a state change. Clients therefore receive a different message family for close than for open — the open path (BR-18) sends `_MSG_UpdateItem`, the close path sends `_MSG_CreateItem`. `SetArenaDoor`/`SetColoseumDoor`/`SetCastleDoor` follow the same convention, so this is the house style, not an oversight.

**Rule workflow**:
```
(twice per minute)
for i in [17, g_dwInitItem):
    ipg = pItemGrid[PosY][PosX] ; skip if sIndex invalid
    if EF_KEYID != 0 and State == STATE_OPEN and EF_KEYID < 15:
        if Delay == 0: Delay = 1; continue          (arm)
        UpdateItem(ipg, STATE_LOCKED); State = STATE_LOCKED; Delay = 0
        GridMulticast(_MSG_CreateItem with State=LOCKED)
```

---

### Business Rule: BR-18 — View-grid broadcast of the request packet

**Overview**:
When the transition succeeds, the *client's own packet* is multicast to a 33x33 window centred on the gate, with the caster included.

**Detailed description**:
`GridMulticast(tx, ty, (MSG_STANDARD*)m, 0)` (`Code/TMSrv/_MSG_UpdateItem.cpp:100-101`) reuses the received buffer. `GridMulticast` (`Code/TMSrv/SendFunc.cpp:843-972`) clamps the window to `[0, MAX_GRIDX/Y)`, iterates `pMobGrid`, skips indices `<= 0` and equal to `skip` (here `0`, so nothing is skipped), and then delivers the raw bytes to every `tmob < MAX_USER` with a live socket via `pUser[tmob].cSock.AddMessage((char*)msg, msg->Size)`.

Three properties follow directly from reusing the client buffer. First, `msg->Size` is client-supplied: a client that sends a valid-looking `Size` larger than the true payload can make the server push bytes beyond the received message to every neighbour — bounded by `MAX_MESSAGE_SIZE` (8192) and by the receive buffer contents, but still an amplification primitive. Second, `msg->ID` is the sender's id rather than `ESCENE_FIELD` (30000), so observers see a packet whose `ID` names a player on a `_MSG_UpdateItem` — a shape the server's own producers never generate. Third, `msg->State` is the requested state, not `pItem[gateid].State`, which is the divergence described in BR-14.

The in-process convention for the same operation is visible three times in `Server.cpp` (`5918-5920`, `5966-5968`, `6086-6088`): build a zeroed `MSG_UpdateItem`, set `ID = ESCENE_FIELD`, `Type = _MSG_UpdateItem`, `ItemID = gate + 10000`, `Size = sizeof(MSG_UpdateItem)`, `State = pItem[gate].State`, then multicast. The handler's echo path is the only producer that does not follow it.

**Rule workflow**:
```
if isUpdate:
    GridMulticast(pItem[gateid].PosX, pItem[gateid].PosY, (MSG_STANDARD*)m, skip=0)
       -> window clamped to map bounds (33x33 nominal)
       -> for each occupied pMobGrid cell with tmob in (0, MAX_USER): AddMessage(msg, msg->Size)
       -> caster receives its own request back
```

---

### Business Rule: BR-19 — Unconditional audit record

**Overview**:
Every path that survives to line 103 writes `etc,opengate gateid:%d X:%d Y:%d` with the account name and IP, including paths where no state change occurred.

**Detailed description**:
The `sprintf(temp, ...)` / `Log(temp, pUser[conn].AccountName, pUser[conn].IP)` pair is placed after the `NoNeedKey` label and is not guarded by `isUpdate`. It therefore records: successful opens, no-op requests for already-open gates (BR-15 rejection), requests for accepted-but-meaningless states 0/4/5 (BR-02/BR-15), and keyless opens of already-open doors. It does **not** record key failures (returns at BR-12) or Castle Zakum outcomes (returns at BR-05), the latter being logged inside the module with an identical format (`Code/TMSrv/CCastleZakum.cpp:205-206`).

`Log` (`Code/TMSrv/Server.cpp:6701-6708` region) stamps the line with `dd/mm/yyyy hh:mm:ss`, appends account and IP, and writes to the shared log file handle; `temp` is a single global `char temp[4096]` (`Code/TMSrv/Server.h:382`), safe here because TMSrv is single-threaded and the format string has three integer substitutions.

The practical value of the record is forensic ("who opened which gate"), while its weakness is exactly its unconditionality: an operator searching for successful unlocks cannot distinguish them from denied-by-state requests, because the same text is emitted for both.

**Rule workflow**:
```
sprintf(temp, "etc,opengate gateid:%d X:%d Y:%d", gateid, pItem[gateid].PosX, pItem[gateid].PosY)
Log(temp, pUser[conn].AccountName, pUser[conn].IP)
return                                    <- last statement; result of isUpdate not reflected
```

---

### Business Rule: BR-20 — Anti-tamper accumulation and forced logout

**Overview**:
Three violation classes feed `pUser[conn].NumError` (1, 50 and 50 points); reaching 2,000,000,000 forces a client message plus `CharLogOut`.

**Detailed description**:
`AddCrackError` (`Code/TMSrv/Server.cpp:684-707`) logs the increment unless `Type` is one of the silent types 3, 8 or 15, adds `val` to the session counter, and on the 2×10^9 threshold sends `_NN_Bad_Network_Packets` (catalog index 23, `Code/TMSrv/Language.h:43`), calls `CharLogOut(conn)`, logs `cra char logout type: %d`, and returns `TRUE`. The handler uses `Type = 16` (session anomaly, 1 point) and `Type = 50` (identity/state anomaly, 50 points); neither is silent, so all three call sites are logged.

The asymmetry is intentional in spirit — packet-level tampering costs 50× more than a stale session — but the absolute threshold makes both effectively non-punitive: 40 million tampered `State` values are needed to force a logout, while a normal session's lifetime is bounded by idle timeouts and reconnects. The counter is per-session and is not persisted, so a reconnect resets the score. Its real function is log-rate signalling to operators.

A side effect worth noting: `AddCrackError` returns `TRUE` on forced logout, but none of the three call sites in this handler inspects the return value; after a forced logout the handler simply returns, which is the correct outcome because `CharLogOut` has already transitioned the session.

**Rule workflow**:
```
AddCrackError(conn, val, Type):
    if Type not in {3,8,15}: log "cra point: val  type: Type"
    NumError += val
    if NumError >= 2000000000:
        SendClientMessage(_NN_Bad_Network_Packets); CharLogOut(conn)
        log "cra char logout type: Type"; return TRUE
    return FALSE
```

---

### Business Rule: BR-21 — Index 0 is reserved

**Overview**:
`pItem[0]` is never handed out by the allocator, so a derived `gateid == 0` can never refer to a live gate.

**Detailed description**:
`GetEmptyItem` starts its scan at `i = 1` and returns `0` only as an error sentinel (`Code/TMSrv/Server.cpp:6754-6763`), and both `CreateItem` and `CreateTreasureBox` abort on `empty == 0` (`Server.cpp:7269-7270`, `9405-9406`). The handler's identity check (`ItemID < 10000` rejected) means `gateid == 0` is only reachable with `ItemID == 10000`, which is accepted by BR-03 but always refers to a slot that no allocator ever populated.

The behaviour in that case is benign but non-obvious: `pItem[0].State == 0`, `GridCharge == 0`, so `BASE_UpdateItem` returns `FALSE` (BR-15) and nothing happens except the BR-19 audit line. There is no explicit "reserved slot" rejection in the handler, so the safety depends on the invariant being maintained by the allocator rather than by the gate contract.

**Rule workflow**:
```
ItemID == 10000 -> gateid == 0 -> slot never allocated -> State 0, GridCharge 0
   -> BASE_UpdateItem delta == 0 -> FALSE -> no state change, audit only
```

---

### Business Rule: BR-22 — Disabled treasure-box interception

**Overview**:
A commented block would have offered `gateid` to `CEncampment::OpenTreasureBox` before the update; the class no longer exists in the tree.

**Detailed description**:
Lines 93-94 are a commented call to `CEncampment::OpenTreasureBox(conn, gateid)`, guarded by `== TRUE` and a `return`. The `CEncampment` type has no declaration or definition anywhere in `Code/`; the only other references are also comments (`Code/TMSrv/imple.cpp:258`, `Code/TMSrv/MobKilled.cpp:981`, `Code/TMSrv/ProcessSecMinTimer.cpp:1351,2045`, `Code/TMSrv/Server.cpp:3628`). This is a removed event subsystem whose call sites were commented out rather than deleted, leaving the handler with a visible seam where a future event type would be inserted.

Its placement matters for re-enablement: it sits *after* key consumption (BR-13) and *before* `UpdateItem`, so the original design allowed the treasure-box handler to consume a key and short-circuit the door animation. Restoring it as-is would reintroduce the "key consumed, door not opened" ordering that BR-13 currently avoids only because the transition is guaranteed legal.

**Rule workflow**:
```
[NoNeedKey]
/* if (CEncampment::OpenTreasureBox(conn, gateid) == TRUE) return; */   (dead)
UpdateItem(gateid, STATE_OPEN, &heigth)                                  (live)
```

---

## 4. Component Structure

```
Code/TMSrv/
├── _MSG_UpdateItem.cpp          # THE COMPONENT (106 lines): Exec_MSG_UpdateItem only
│                                #   lines 20-106: single function, 5 validation blocks,
│                                #   1 delegation call, 1 key-scan loop, 1 forced update,
│                                #   1 multicast, 1 audit record
├── ProcessClientMessage.h       # declares Exec_MSG_UpdateItem (line 101); fans in 14 headers
├── ProcessClientMessage.cpp     # dispatcher: case _MSG_UpdateItem (lines 180-182)
│
├── CCastleZakum.h / .cpp        # DELEGATE: OpenCastleGate (72-208) owns KEYID 10..14
│   └── module globals            #   CastleQuestLevel, CastleQuestTime, CastleLeader,
│                                 #   CastleQuestParty, CastleQuest[MAX_CASTLE_QUEST]
├── Server.cpp                   # UpdateItem (7472-7556)  [state application]
│                                #   AddCrackError (684-707), CharLogOut (7085+), Log (6701+)
│                                #   Reboot (7127-7207)     [boot-time locking]
│                                #   SetArenaDoor (6009), SetColosseumDoor (5887/5935),
│                                #   SetCastleDoor (6060)   [server-side sibling patterns]
├── Basedef.cpp                  # BASE_UpdateItem (5292-5339) [transition + height pass]
│                                #   BASE_GetItemAbility (1647-1842) [EF_KEYID/EF_QUEST]
│   └── Basedef.h                #   MSG_UpdateItem (1839-1847), STATE_* (1835-1837),
│                                #   MAX_ITEM/MAX_CARRY/MAX_USER, ITEM_PLACE_*
├── SendFunc.cpp                 # SendHpMode (1622), SendItem (1055), SendClientMessage (27),
│                                #   GridMulticast (843-972)
├── GetFunc.cpp                  # GetCreateItem (1195-1237): the *authoritative* state
│                                #   projection (`sm->State = pItem[idx].State`)
├── ProcessSecMinTimer.cpp       # minute relock loop (2142-2209) — BR-17
├── imple.cpp                    # GM gate commands calling UpdateItem (315-415)
├── CItem.h / .cpp               # CItem { ITEM, Mode, PosX, PosY, GridCharge, State,
│                                #          Rotate, Delay, Decay, Height, Money, Open,
│                                #          ItemQuest, Unk[20] } ; pItem[MAX_ITEM]
├── CUser.h                      # USER_PLAY = 22, USER_SELCHAR = 11
└── Language.h                   # _NN_No_Key = 79, _NN_Bad_Network_Packets = 23
```

**Component boundary.** The component is a single translation unit with one external symbol, `Exec_MSG_UpdateItem(int, char*)`. It declares no state, owns no data, and has no file-scope statics: all mutable state lives in `pItem`, `pMob`, `pUser`, `pHeightGrid`, and the `CCastleZakum` globals. It is therefore stateless and re-entrancy-neutral, but also unable to enforce any invariant that the globals do not already carry.

**Container/composition note.** The component is compiled into `TMSrv.exe` only (`Code/TMSrv/TMSrv.vcxproj:162`); `DBSrv` has no equivalent. There is no library seam, no interface, and no dependency injection — the message contract *is* the interface.

---

## 5. Dependency Analysis

**Internal Dependencies (compile-time / link-time)**

```
ProcessClientMessage.cpp:181
  └─> Exec_MSG_UpdateItem                        (this component)
        ├─> pMob[conn].MOB.CurrentScore.Hp        [CMob global, Server.cpp:? / CMob.cpp]
        ├─> pUser[conn].Mode / AccountName / IP   [CUser global]
        ├─> USER_PLAY                             [CUser.h:36]
        ├─> SendHpMode                            [SendFunc.cpp:1622]
        ├─> AddCrackError                         [Server.cpp:684]
        │     ├─> CharLogOut                      [Server.cpp:7085]
        │     └─> Log                             [Server.cpp:6701]
        ├─> pItem[gateid]                         [CItem global, CItem.h:45]
        ├─> CCastleZakum::OpenCastleGate          [CCastleZakum.cpp:72]
        │     ├─> BASE_GetItemAbility             [Basedef.cpp:1647]
        │     ├─> SendClientMessage               [SendFunc.cpp:27]
        │     ├─> pItem / pMob / pMobGrid / nNPCGen
        │     ├─> DeleteMob / GenerateMob / CreateMob
        │     ├─> SendClientSignalParm            [SendFunc.cpp:208]
        │     ├─> UpdateItem  -> BASE_UpdateItem  [Server.cpp:7472 -> Basedef.cpp:5292]
        │     ├─> GridMulticast                   [SendFunc.cpp:843]
        │     └─> Log
        ├─> BASE_GetItemAbility (EF_KEYID=39, EF_QUEST=58)
        ├─> SendClientMessage + g_pMessageStringTable[_NN_No_Key]
        ├─> SendItem (ITEM_PLACE_CARRY=1)
        ├─> UpdateItem
        │     └─> BASE_UpdateItem -> g_pGroundMask, pHeightGrid, pMobGrid, GetEmptyMobGrid,
        │                            GetAction, DeleteMob, GridMulticast, pUser[].cSock
        └─> GridMulticast + Log + sprintf(temp)
```

**External Dependencies**

| Dependency | Version / form | Purpose in this component | Notes |
|------------|----------------|---------------------------|-------|
| MSVC toolset | v143 (VS 2022), Win32 `Application` | Build (`TMSrv.vcxproj:28`) | `/DYNAMICBASE` disabled in Debug; no CFG/NX/SAFESEH overrides |
| Winsock 1.1 | `CPSock.cpp` wrapper | Delivers/queues the packets consumed here | 128 KB fixed buffers; static obfuscation table |
| CRT | `sprintf`, `memset`, `memcpy` | Buffer/format operations | `_CRT_SECURE_NO_WARNINGS` is globally defined |
| `../../Common/ItemList.bin` | external data, absent from repo | Supplies `g_pItemList[]` used by `BASE_GetItemAbility` and `sIndex 773` semantics | Verified absent: no `Common/` directory in the workspace |
| `../../Common/InitItem.csv` | external data, absent | Defines the gate props created at boot (BR-16) | Same |
| `../../Common/Settings/CastleQuest.txt` | external data, absent | Defines levels, timers, prizes used by the delegate (BR-08) | `CCastleZakum.cpp:44` |
| `Language.txt` | external data, absent | Localised `_NN_No_Key` / `_NN_Bad_Network_Packets` | Loaded `Server.cpp:7160` |
| `heightmap.dat` | external binary, absent | Initial `pHeightGrid` including `127` blockers | `Server.cpp:7146-7156` |
| Log file handle | runtime file | `etc,opengate`, `cra point`, `cra char logout` records | Global `fLogFile` |

**Absent dependencies (notable).** No database write, no DB message emission, no `ItemLog` call (contrast `_MSG_GetItem.cpp:231` which logs the acquired item), no `SaveUser`, no event bus, no mutex/lock (single-threaded), no telemetry.

---

## 6. Afferent and Efferent Coupling

Coupling is measured per C-construct (free function, method, module-level global), consistent with the C++ procedural paradigm of this codebase.

| Component (C construct) | Afferent (Ca) | Efferent (Ce) | Critical | Evidence |
|--------------------------|---------------|---------------|----------|----------|
| `Exec_MSG_UpdateItem` | 1 | 14 (headers) / 21 (symbol targets) | High | Caller: `ProcessClientMessage.cpp:181`. Ce: 14 `#include`s via `ProcessClientMessage.h`; 21 distinct symbols (see §5) |
| `CCastleZakum::OpenCastleGate` | 1 | 9 | High | Only caller `_MSG_UpdateItem.cpp:51`; Ce = pItem, BASE_GetItemAbility, SendClientMessage, DeleteMob, GenerateMob, mNPCGen, SendClientSignalParm, UpdateItem, GridMulticast, Log |
| `UpdateItem` (global) | 10 | 6 | High | 10 call sites: `imple.cpp:315,339,363,378,400,415`, `MobKilled.cpp:731`, `ProcessSecMinTimer.cpp:2166`, `Server.cpp:5903,5951,6027,6074,7184`, `CCastleZakum.cpp:200`, `_MSG_UpdateItem.cpp:98` (13 sites, 10 distinct callers), Ce = BASE_UpdateItem, BASE_GetItemAbility, pItem, Log, CreateMob, map/eviction APIs |
| `BASE_UpdateItem` | 1 | 3 | Medium | Only caller `Server.cpp:7476`; Ce = `g_pGroundMask`, `pHeightGrid`, mask bounds globals |
| `BASE_GetItemAbility` | 33 | 2 | High | Broadest item-rule dependency in the tree; Ce = `g_pItemList`, `BASE_GetItemSanc` |
| `AddCrackError` | 21 | 4 | Medium | Ce = `Log`, `pUser`, `SendClientMessage`, `CharLogOut` |
| `SendHpMode` | 23 | 4 | Low | Ce = `pUser`, `pMob`, `CloseUser`, `MSG_SetHpMode` |
| `SendItem` | 36 | 3 | Low | Ce = `pUser`, `MSG_SendItem`, `STRUCT_ITEM` |
| `SendClientMessage` | high (>40) | 3 | Low | Ce = `AddMessage`, `g_pMessageStringTable`, `temp` |
| `GridMulticast` | 53 | 6 | Medium | Ce = `pMobGrid`, `pMob`, `pUser`, `MSG_STANDARD`, `SetCircletSubGod`, `DoItemLevel` |
| `pItem` (`CItem[MAX_ITEM]`) | high (every item path) | 1 | High | Shared mutable global; the handler's only write target |
| `MSG_UpdateItem` (struct) | 4 producers + this consumer | 0 | Medium | `Basedef.h:1840`; produced in `Server.cpp:5907,5955,6031,6078` |

**Interpretation**

- **Afferent 1 for the handler** is exact and structural: a single dispatch edge. Any change to the function signature or semantics is binary-compatible for the rest of the server but immediately changes player-facing behaviour.
- **Efferent 14** matches the header fan-in of `ProcessClientMessage.h` (14 includes: `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`, `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h`, `CMob.h`, `CUser.h`, `CNPCGene.h`, `CReadFiles.h`, `CCastleZakum.h`, `CWarTower.h`). Two of those (`CReadFiles.h`, `CWarTower.h`) are unused by this TU, so the *semantic* efferent set is 12 headers / 21 symbols. The header-hub pattern is what inflates Ce for all 61 handlers equally.
- **Cohesion within the handler** is functional-ish but not single-purpose: it performs session validation, namespace validation, event delegation, authorisation, inventory mutation, state mutation, projection and auditing. Its seven responsibilities map onto seven different modules, which is why Ce is high while Ca is 1 — the classic "orchestrating handler" shape of a `switch`-dispatched server.
- **Instability** `I = Ce / (Ca + Ce) = 14 / 15 ≈ 0.93`. The component is highly unstable by Martin's metric; it should depend only on stable things and nothing should depend on it — which is exactly the case for the delegates (`UpdateItem`, `BASE_UpdateItem` are stable-ish) but violated by the shared mutable globals (`pItem`).

---

## 7. Endpoints

The component exposes no HTTP/REST/GraphQL/gRPC surface. Its only endpoint is a binary game-protocol message handled in-process.

| Endpoint (protocol message) | Wire `Type` | Flags | Direction | Payload | Handler |
|------------------------------|-------------|-------|-----------|---------|---------|
| `_MSG_UpdateItem` | `116 \| 0x0100 \| 0x0200` = **884** (`0x374`) | `FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME` | bidirectional | `MSG_UpdateItem { int ItemID; int State; }` (20 bytes total) | `Exec_MSG_UpdateItem` (`ProcessClientMessage.cpp:180`) |

**Payload layout**

| Offset | Size | Field | Source | Handler use |
|--------|------|-------|--------|-------------|
| 0 | 2 | `Size` | client | trusted for the echo fan-out (BR-18) |
| 2 | 1 | `KeyWord` | client | decode key index (already consumed by `CPSock`) |
| 3 | 1 | `CheckSum` | client | validated in `CPSock.cpp:455-464` |
| 4 | 2 | `Type` | client | must equal 884 to reach the handler |
| 6 | 2 | `ID` | client | range-checked by dispatcher only (`[0, MAX_USER)`); echoed back verbatim |
| 8 | 4 | `ClientTick` | client | `SKIPCHECKTICK` filter in dispatcher; echoed back |
| 12 | 4 | `ItemID` | client | BR-03 / BR-04 |
| 16 | 4 | `State` | client | BR-02, BR-10 (predicate only), echoed in BR-18 |

**Outbound effects (no dedicated response opcode)**

| Effect | Message | Recipients |
|--------|---------|-----------|
| State-change animation | `_MSG_UpdateItem` (echo of the request) | 33x33 view grid, caster included |
| Key consumed | `_MSG_SendItem` (slot = the emptied carry index) | caster only |
| Key missing | chat text `_NN_No_Key` | caster only |
| Session invalid | `_MSG_SetHpMode` | caster only |
| Anti-cheat threshold | chat text `_NN_Bad_Network_Packets` + logout signal | caster only |

Note: no success acknowledgement is ever sent to the requester. The requester learns the outcome only by observing the echoed `_MSG_UpdateItem` (which carries the *requested* state) or the disappearance of the key item.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| W2 client (build 7662) | External client | Gate/door interaction | TCP 8281, custom binary framing (`CPSock`) | `HEADER` + fixed structs, keyword/checksum obfuscation | Keyword mismatch → drop; checksum mismatch → **still delivered** (`CPSock.cpp:457-464`); size bounds only `[12, 8192]` |
| `CCastleZakum` event module | Internal module (same binary) | Castle key/quest/party/timer orchestration | Direct call | `(int conn, int gateid, MSG_UpdateItem*)` → `BOOL` | `TRUE` = handled (success **or** silent refusal); `FALSE` = not a castle gate. No exceptions, no error code |
| `UpdateItem` / `BASE_UpdateItem` | Internal rules library (`Basedef.cpp`) | Legal transition + height-map pass | Direct call | `(int gate, int state, int *height)` → `int` | `FALSE` = illegal transition (`delta == 0`) or bad mask; no diagnostic returned |
| `pItem[]` (CItem array) | In-process shared state | Gate record (state, height, delay, item, position) | Memory | `CItem[5000]` | Index validated by BR-03; **existence not validated** |
| `pMobGrid[][]` | In-process grid | Locate observers for fan-out; evict occupants (dead code) | Memory | `unsigned short[4096][4096]` | Bounds clamped in `GridMulticast`; `UpdateItem`'s own loop checks `[1,4094]` but is unreachable |
| `pHeightGrid[][]` | In-process terrain | Collision/height mutation on transition | Memory | `char[4096][4096]`, `127` = impassable | Mutated only through the (always-zero) mask; no write ever occurs in this tree |
| Log file (`fLogFile`) | File system | `etc,opengate`, `cra point`, `cra char logout` | `fprintf` on a shared `FILE*` | Plain text, `dd/mm/yyyy hh:mm:ss` | Errors ignored (return value of `fprintf` not checked) |
| Language catalogue (`Language.txt`) | Boot-time data | `_NN_No_Key`, `_NN_Bad_Network_Packets` | In-memory string table | `g_pMessageStringTable[]` | If not loaded, index yields empty/NULL; no null guard at the call sites |
| Item catalogue (`ItemList.bin`) | Boot-time data | `EF_KEYID`, `EF_QUEST`, `EF_GROUND`, `nPos`, `nUnique` lookups | In-memory `g_pItemList[6500]` | `STRUCT_ITEMLIST` | `BASE_GetItemAbility` returns `0` for `idx <= 0 \|\| idx >= MAX_ITEMLIST` — a silent zero that means "no key" |
| DBSrv (`TMSrv.exe` → `DBSrv.exe`) | Internal service | **None** — this component never persists or queries | TCP 7514 | — | Not integrated |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Type-switch dispatcher (Front Controller) | single `switch(std->Type)` mapping 61 message types to `Exec_MSG_*` | `Code/TMSrv/ProcessClientMessage.cpp:66-311` | One entry point; per-message handler units |
| Handler / Command object (function form) | `Exec_MSG_UpdateItem(int conn, char *pMsg)` | `Code/TMSrv/_MSG_UpdateItem.cpp:20` | Uniform `(conn, raw buffer)` contract across all 61 handlers |
| Delegation / Chain of Responsibility | handler → `CCastleZakum::OpenCastleGate` → handler fallback | `_MSG_UpdateItem.cpp:51` | Event module claims its gate band; ownership by return value |
| Guard-clause validation (fail-fast) | 4 sequential `if (...) return;` blocks | `_MSG_UpdateItem.cpp:24-49` | Session, state, namespace, index validation |
| Return-value ownership protocol | `BOOL TRUE` = terminal (success **or** refusal) | `CCastleZakum.cpp:102,109,126,191` | Collapses "handled" and "denied"; the caller cannot distinguish |
| Table-driven content | `g_pItemList[]`, `g_pGroundMask[]`, `CastleQuest[]`, `g_pInitItem[]` | throughout | No hard-coded item/gate tables in logic |
| State machine (implicit, integer-coded) | `delta` computation in `BASE_UpdateItem` | `Code/Basedef.cpp:5300-5306` | `OPEN ⇄ (CLOSED\|LOCKED)`; no enum, no transition table |
| Two-phase commit surrogate | validate → mutate `pItem` → project to clients | `_MSG_UpdateItem.cpp:98-101` | Server is authoritative; clients are projections |
| Fire-and-forget fan-out (publish) | `GridMulticast` to the 33x33 window | `Code/TMSrv/SendFunc.cpp:843` | O(N) per gate change, no event bus |
| Defensive re-validation (duplicated guards) | third `gateid` bound test | `_MSG_UpdateItem.cpp:45-49` | Decompiler artefact; documents intent, executes never |
| Latch flag | `pItem[].Delay` as 0→1 latch for the minute relock | `ProcessSecMinTimer.cpp:2159-2165` | Avoids an extra timestamp field |
| Sentinel global | `CastleQuestTime == -1` = no active run | `CCastleZakum.cpp:67,111` | Single-run concurrency control |
| Magic-number protocol | `+10000` item namespace, `sIndex 773`, `EF_KEYID` bands 10-14/15 | multiple | Wire/inventory decoupling; no named constants for 10000 or 773 |
| Duplicated business logic (anti-pattern) | key scan + consumption implemented twice | `_MSG_UpdateItem.cpp:54-90` vs `CCastleZakum.cpp:74-103` | Divergence already visible (`Quest` check lost) |

**Architectural decisions observed**

1. *Server owns door state; client renders it.* No reconciliation message carries the authoritative state to the requester (BR-18 divergence).
2. *Event content is data-driven; event behaviour is code.* `CastleQuest.txt` supplies levels and timers, but the orchestration is hard-coded in `OpenCastleGate`.
3. *Layered ordering within one function:* session → domain → delegation → authorisation → mutation → projection → audit. The layering is consistent across all handlers, which is why the code is navigable despite the duplication.
4. *No persistence in the interaction path.* Gates are ephemeral server state reconstructed from `InitItem.csv` at every boot; this is what makes BR-16 necessary.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| **High** | `_MSG_UpdateItem.cpp:51-90` | Key-authorisation logic duplicated from `CCastleZakum::OpenCastleGate`, already divergent (missing `Quest == CastleQuestLevel`) | Any fix applied to one copy silently misses the other; new key bands (1-9, 15+) inherit the weaker contract |
| **High** | `Server.cpp:7481-7505` | `UpdateItem` returns before its eviction loop: `itemstate` is assigned from the just-written state, so `itemstate == state` is always true → 44 lines + `CreateMob("GATE")` unreachable | Entities standing in a closing gate are never pushed out; "GATE" NPC spawn on close never happens via this path (only `SetCastleDoor` compensates) |
| **High** | `Basedef.cpp:173` + `Basedef.cpp:5312` | `g_pGroundMask` is never initialised anywhere in the tree (no loader, no writes) | Gate state has no server-side collision effect; walkability is client-authoritative — a modified client can walk through locked doors |
| **High** | `_MSG_UpdateItem.cpp:100-101` | Client packet echoed verbatim: `Size` (client-controlled) drives `AddMessage` length; `State`/`ID` are client values | Memory disclosure toward all observers (bounded by 8192-byte buffer) and client/server state divergence |
| **High** | `_MSG_UpdateItem.cpp:36-49` | No existence check (`pItem[gateid].Mode`) and no `pItemGrid` consistency check, unlike `_MSG_GetItem.cpp:55,164` | Requests can address freed/recycled item slots whose stale `State == 3` still satisfies BR-10 |
| **High** | `_MSG_UpdateItem.cpp` (absent rule) | No proximity/range check, unlike the 3-tile rule in `_MSG_GetItem.cpp:74-82` | A player carrying the key can open any gate on the map from anywhere |
| **Medium** | `Basedef.cpp:6475-6622` | `BASE_CheckPacket` body is fully commented out; it returns `FALSE` unconditionally, so its `_MSG_UpdateItem` size rule (line 6517) is inert and only compiles under `_PACKET_DEBUG` | No per-message length validation anywhere in the accept path |
| **Medium** | `_MSG_UpdateItem.cpp:98` + `1845` | Target state hard-coded `STATE_OPEN`; `m->State` only predicates/echoes | Requested "close" silently becomes "open"; protocol has no way to express the server's actual reason for refusal |
| **Medium** | `_MSG_UpdateItem.cpp:30-34` vs `Basedef.h:1835-1837` | Accepted state domain `[0,5]` is wider than the meaningful domain `{1,2,3}` | States 0/4/5 pass validation, log an audit line, and do nothing |
| **Medium** | `_MSG_UpdateItem.cpp:103-104` | Audit line written on failure/no-op paths too (`isUpdate` ignored) | Log analysis cannot separate successful unlocks from rejected requests |
| **Medium** | `CCastleZakum.cpp:188-191` | Inner-door check validates only the key's quest level, not membership of `CastleQuestParty`, and refuses silently | Non-participants with a level-matching key can enter; no forensic trace |
| **Medium** | `CCastleZakum.cpp:79` + `_MSG_UpdateItem.cpp:57` | `EF_KEYID` read through `BASE_GetItemAbility`, which **sums** duplicate effect slots and applies the sanc multiplier (for non-structural types) | Item design with duplicated `EF_KEYID` yields a non-matching sum; key ids are item-authoring-sensitive |
| **Medium** | `_MSG_UpdateItem.cpp:43,66,76` | Dead stores: `gatestate`, `Quest` | Obscures which client input is actually consumed; decompiler residue in live code |
| **Medium** | `_MSG_UpdateItem.cpp:93-94` | Commented `CEncampment::OpenTreasureBox` seam; the class no longer exists in the tree | Dormant feature; re-enabling as-is would consume keys without opening |
| **Low** | `_MSG_UpdateItem.cpp:45-49` | Unreachable third `gateid` bound check | Misleads readers about the real validation chain |
| **Low** | `Server.cpp:7489-7493` | `GridCharge >= 6 \|\| < 0` guard unreachable (already rejected by `BASE_UpdateItem`) | Dead diagnostic path |
| **Low** | `Basedef.cpp:5341-5365` | `BASE_UpdateItem2` has zero callers | Dead API surface |
| **Low** | `_MSG_UpdateItem.cpp:96` | `heigth` typo, mirrored into `CCastleZakum.cpp:198` | Naming debt propagated to the delegate |
| **Low** | `_MSG_UpdateItem.cpp:103` | `sprintf` into shared global `temp[4096]` | Safe with the current literal format, but no bounds discipline is visible to a reader |
| **Low** | `Server.cpp:684-707` | `NumError` threshold 2×10^9 makes the anti-cheat counter effectively cosmetic; counter resets on reconnect | Tampering generates logs but no throttling |
| **Low** | `CPSock.cpp:455-464` | Delivers packets whose checksum failed (error code set, message still returned) | Weakens the obfuscation layer; relies on handler validation |
| **Low** | `CItem.cpp:22-30` | Constructor leaves `PosX/PosY/Height/Money/Open/ITEM/Unk` uninitialised (safe only because `pItem` is a zero-initialised global) | Reusing `CItem` as a value type anywhere would expose uninitialised reads |

**Security assessment (evidence-based, no recommendations)**

- *Input validation:* three independent numeric guards before any array access; `ItemID` offset arithmetic is overflow-safe because the upper bound is checked first. Missing: existence, proximity, payload length.
- *Memory safety:* the only index reaching a write (`Carry[i]`) is loop-derived and bounded by construction; `pItem[gateid]` is bounded by BR-03. `sprintf` targets are literal-format, fixed-size globals.
- *Privilege:* no GM/admin check is required — the rule is possession of the key item only. Admin gate control lives separately in `imple.cpp:315-415`.
- *Anti-cheat:* counter-based, non-punitive at the stated threshold, log-only for the values used here.
- *Confidentiality:* the echo path can leak up to `Size` bytes of the receive buffer to nearby players if `CHECKSUM`-valid traffic supplies an inflated `Size`.

---

## 11. Test Coverage Analysis

**Result: no tests exist for this component — or for any component in the repository.**

Evidence (executed against the project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`):

1. Recursive filename search for `test`, `spec`, `Test`, `Spec`, `assert`, `gtest`, `catch2`, `CMakeLists`, `Makefile`, `*.yml`, `*.yaml`, `*.json` → **0 matches**.
2. Content search for `assert(`, `ASSERT(`, `TEST(`, `TEST_F(`, `CppUnit`, `gtest`, `doctest`, `catch2` across `Code/*.cpp`, `Code/*.h`, `Code/TMSrv/*`, `Code/DBSrv/*` → **0 matches**.
3. Solution composition: `W2PP Code Project.sln` contains exactly two projects, `DBSrv` and `TMSrv`, both `<ConfigurationType>Application</ConfigurationType>` — no test project, no unit-test framework package reference, no `packages.config` / `vcpkg.json`.
4. Source inventory: `Code/` 2 `.cpp` + 3 `.h`; `Code/TMSrv/` 73 `.cpp` + 14 `.h`; `Code/DBSrv/` 5 `.cpp` + 5 `.h`. None is a test double, mock, stub or fixture.
5. No CI configuration (no `.github/`, `.gitlab-ci.yml`, `azure-pipelines.yml`) and no test runner script.
6. Codegraph reports "no tests found within 3 caller hops" for `Exec_MSG_UpdateItem`, `OpenCastleGate` and `UpdateItem`, consistent with the above.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `Exec_MSG_UpdateItem` | 0 | 0 | 0% | None |
| `CCastleZakum::OpenCastleGate` (delegate) | 0 | 0 | 0% | None |
| `UpdateItem` / `BASE_UpdateItem` (callees) | 0 | 0 | 0% | None |
| `GridMulticast` / `SendItem` / `SendHpMode` (projection) | 0 | 0 | 0% | None |

**Untested branch inventory (verification targets, not recommendations)**

| Branch | Location | Observable behaviour if wrong |
|--------|----------|------------------------------|
| Dead/live session | `_MSG_UpdateItem.cpp:24` | Ghost unlocks by dead characters |
| State domain `[0,5]` | `:30` | Out-of-band states reaching `UpdateItem` |
| `ItemID` namespace `[10000,15000)` | `:36` | Out-of-bounds `pItem` access |
| Redundant index check | `:45` | none (dead) |
| Delegation claim boundary (`KEYID` 10..14) | `:51` / `CCastleZakum.cpp:79` | Castle gates handled by the wrong path |
| Key-needed predicate (`state != 3 && requested != 3`) | `:59` | Free entry to locked doors |
| `gatekey == 0` exemption | `:62` | Keyless doors refusing entry |
| Carry scan / break index | `:69-79` | Wrong key consumed, or OOB `Carry[i]` |
| No-key feedback + `sIndex 773` suppression | `:81-87` | Spurious toasts, or silent refusals |
| Key consumption + `SendItem` resync | `:89-90` | Client/server inventory divergence |
| Forced `STATE_OPEN` | `:98` | Door never opens / wrong animation |
| `isUpdate` gate on multicast | `:100` | Neighbours desynchronised on no-op |
| Audit line | `:103` | Missing forensic trail |
| `BASE_UpdateItem` delta table (4 pairs) | `Basedef.cpp:5300-5306` | Illegal transitions accepted |
| `UpdateItem` unreachable eviction loop | `Server.cpp:7495-7553` | Occupants not evicted on close |
| Minute relock latch | `ProcessSecMinTimer.cpp:2157-2187` | Doors never close, or close instantly |
| Boot-time lock of `KEYID 1..14` | `Server.cpp:7178-7186` | All gates open after restart |

**Testability observations.** The component is hard to test as written: it depends on five global arrays (`pItem`, `pMob`, `pUser`, `pMobGrid`, `pHeightGrid`), two global tables (`g_pItemList`, `g_pGroundMask`), one global message catalogue, the network layer, and a file logger. There is no seam to inject a fake session or a fake item table, and `Exec_MSG_UpdateItem` reads its payload through a hard cast from `char*`. The only naturally testable unit in the chain is `BASE_UpdateItem`, which takes all of its inputs as parameters except `g_pGroundMask` and the `g_Height*` mapping globals.

---

*End of report.*
