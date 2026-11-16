# Component Deep Analysis Report

**Component:** Client Handler Interface Contract
**Primary location:** `Code/TMSrv/ProcessClientMessage.h`
**Secondary location (contract implementation that defines the semantics):** `Code/TMSrv/ProcessClientMessage.cpp`
**Project:** W2PP (decompiled Win32/MSVC MMORPG server emulator, solution `W2PP Code Project.sln`)
**Subsystem:** TMSrv (game/world server)
**Analysis date:** 2026-11-15 22:09:12
**Scope:** entire project root `E:\wyd\test-setup\w2pp`
**Excluded:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Mode:** static analysis only; no files were modified, no code was executed, no tests were run

---

## 1. Executive Summary

### Purpose

`Code/TMSrv/ProcessClientMessage.h` is not a normal header. It is the **implicit interface contract of the entire TMSrv inbound command surface**. In 132 lines it does three distinct jobs at once:

1. **Declares the single entry point of the inbound client path** — `void ProcessClientMessage(int conn, char *pMsg, BOOL isServer);` (`Code/TMSrv/ProcessClientMessage.h:70`).
2. **Declares the 58 `Exec_MSG_*` handler entry points** (`Code/TMSrv/ProcessClientMessage.h:74-131`) — one per inbound message family — in a fixed, two-parameter ABI.
3. **Aggregates the whole server's type universe** by including 14 project headers (`Code/TMSrv/ProcessClientMessage.h:29-43`) and re-declaring 12 global externs (`Code/TMSrv/ProcessClientMessage.h:45-68`), so that every handler translation unit is born with `pUser`, `pMob`-adjacent globals, all message structs, all send helpers and all game-rule helpers already visible.

Because every one of the 58 handler `.cpp` files includes only this header, the header is the de-facto "server API" for handlers: it defines the calling convention, the payload-cast convention, the global-state visibility, and — through the dispatcher — the validation order that handlers are allowed to assume.

### Role in the system

| Aspect | Value |
|---|---|
| Layer | Inbound Adapter boundary / shared contract (TMSrv) |
| Upstream caller | `MainWndProc` → `WSA_READ` → `Server.cpp` receive loop (`Code/TMSrv/Server.cpp:3975-4001`) |
| Downstream consumers | 58 handler TUs (`Code/TMSrv/_MSG_*.cpp`) + 8 non-handler TUs |
| Transport | TCP, port `GAME_PORT 8281` (`Code/Basedef.h:44`), custom 12-byte `HEADER` + XOR/offset obfuscated payload |
| Message space | 191 `_MSG_*` constants total; 64 `case` labels dispatched; 58 handler targets |
| Coupling (from architecture report) | Afferent ~60, Efferent ~14 — confirmed below as 66 and 14 |

### Key findings

1. **The contract is a documented-by-convention ABI, not a compiler-enforced interface.** It consists of `void Exec_MSG_X(int conn, char *pMsg)`; nothing in the language prevents a handler from having a different shape, and nothing enforces any of the behavioural rules listed in Section 3.
2. **Count discrepancy with the upstream architectural report.** The architectural report states "61 `Exec_MSG_*` entry points"; the header actually declares **58** (`Code/TMSrv/ProcessClientMessage.h:74-131`, verified by exact match count: 58 declarations, 58 unique names, 58 definitions, 58 dispatch call sites). The "61" figure is off by three and is not reproducible from the source.
3. **The dispatcher performs only four checks and none of them is authorisation.** It validates `pMsg->ID` range, the `ServerDown` gate, liveness stamping, and two packet-suppression rules (Section 3, BR-04 to BR-08). Session-state, HP and ownership validation is entirely delegated to handlers: **31 of the 58 handlers never test `USER_PLAY`, and 26 of them apply no session gate of any kind**.
4. **The dispatcher's most consequential undocumented assumption is `sizeof(struct)`.** `CPSock::ReadMessage` only guarantees `HEADER <= Size <= 8192` and `Size <= bytes buffered` (`Code/CPSock.cpp:397-411`); it never compares `Size` with the structure a handler casts to. Only `Exec_MSG_AccountLogin` verifies `Size < sizeof(MSG_AccountLogin)` (`Code/TMSrv/_MSG_AccountLogin.cpp:41-54`). Every other handler reads `sizeof(MSG_*)` bytes from a buffer whose declared packet length may be only 12 bytes.
5. **Two handlers violate the transmission half of the contract**: `Exec_MSG_Action` passes `sizeof(m)` — the size of a *pointer* — to `SendOneMessage`/`AddMessage` (`Code/TMSrv/_MSG_Action.cpp:272`, `:278`), so only 4 bytes of a 48-byte `MSG_Action` frame are queued.
6. **The header is a recompilation amplifier.** 66 translation units include it; all 14 subsystem headers are reachable from it; it also duplicates extern declarations that already exist in `Server.h` (`hWndMain`, `ServerGroup`, `CurrentTime`, `BrState`, `pUser`, `mNPCGen`, grid arrays, `g_pGuildWar`/`g_pGuildAlly`, `GuildImpostoID`, `CurrentWeather`), including a magic literal `65536` element size for guild arrays against `MAX_GUILD 4096`.
7. **There is no test infrastructure of any kind in the repository** — no test project, no test framework reference, no test file. The contract is verified exclusively at runtime by log lines and by the `AddCrackError` counter. Evidence in Section 11.

---

## 2. Data Flow Analysis

### 2.1 Primary flow: one client packet, from socket to handler side effects

```
 1. TCP bytes arrive on port 8281; FD_READ posted to hWndMain
 2. MainWndProc (WSA_READ) -> Server.cpp receive loop                Code/TMSrv/Server.cpp:3975
 3. pUser[conn].cSock.Receive() was already called; buffer positions advance
 4. CPSock::ReadMessage(&Error, &ErrorCode)                          Code/CPSock.cpp:353
 5.   a. first 4 bytes must equal INITCODE 0x1F11F311                Code/CPSock.cpp:366-383
 6.   b. require >= sizeof(HEADER) = 12 buffered                      Code/CPSock.cpp:386
 7.   c. Size must be in [sizeof(HEADER), MAX_MESSAGE_SIZE 8192]      Code/CPSock.cpp:397-406
 8.   d. Size must be <= bytes currently buffered                     Code/CPSock.cpp:408-411
 9.   e. in-place deobfuscation of bytes [4 .. Size) using pKeyWord   Code/CPSock.cpp:430-453
10.   f. checksum mismatch sets ErrorCode=1 but the packet is still returned  Code/CPSock.cpp:457-464
11. if (Error == 1 || Error == 2) -> log and break (caller discards)  Code/TMSrv/Server.cpp:3990-3999
12. ProcessClientMessage(User, Msg, FALSE)                            Code/TMSrv/Server.cpp:4001
13.   ID gate: 0 <= std->ID < MAX_USER (1000) else log + return       Code/TMSrv/ProcessClientMessage.cpp:42-51
14.   maintenance gate: ServerDown >= 120 -> return                    Code/TMSrv/ProcessClientMessage.cpp:53-54
15.   liveness stamp: pUser[conn].LastReceiveTime = SecCounter         Code/TMSrv/ProcessClientMessage.cpp:56-57
16.   _MSG_Ping -> return (consumed, never routed)                    Code/TMSrv/ProcessClientMessage.cpp:59-60
17.   isServer == FALSE && ClientTick == SKIPCHECKTICK -> return      Code/TMSrv/ProcessClientMessage.cpp:63-64
18.   switch (std->Type) -> Exec_MSG_* (conn, pMsg)                   Code/TMSrv/ProcessClientMessage.cpp:66-311
19. Handler (58 variants):
20.   a. casts pMsg to MSG_* / MSG_STANDARDPARM* (no copy, aliasing write-back)
21.   b. reads pUser[conn].Mode / pMob[conn].MOB.CurrentScore.Hp (when it does)
22.   c. mutates pMsg in place, or forwards it to DBServerSocket (Type/ID rewritten)
23.   d. mutates pUser[conn] / pMob[conn] / pMob[n] / global tables
24.   e. queues responses: Send* helpers -> cSock.AddMessage (batched)
25.      or immediate flush: cSock.SendOneMessage -> AddMessage + SendMessageA
26. return to step 12 loop until ReadMessage returns NULL              Code/TMSrv/Server.cpp:3973-4002
27. Independent 1-second flush loop sends any queued bytes             Code/TMSrv/ProcessSecMinTimer.cpp:201-215
```

### 2.2 Secondary flow: handler-initiated DB round trip (login family)

```
 1. Client -> _MSG_AccountLogin                     Code/TMSrv/_MSG_AccountLogin.cpp:21
 2. Handler rewrites m->Type = _MSG_DBAccountLogin, m->ID = conn,
    sets pUser[conn].Mode = USER_LOGIN, pMob[conn].Mode = MOB_EMPTY
 3. DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_AccountLogin))
 4. DB reply arrives on WSA_READDB -> ProcessDBMessage  Code/TMSrv/ProcessDBMessage.cpp:354+
 5. DB handler sets pUser[conn].Mode = USER_SELCHAR and emits _MSG_CNFAccountLogin
 6. Only then is _MSG_CharacterLogin legal (handler requires USER_SELCHAR)
 7. _MSG_CharacterLogin -> _MSG_DBCharacterLogin, Mode = USER_CHARWAIT  Code/TMSrv/_MSG_CharacterLogin.cpp:73-81
 8. DB confirm sets pUser[conn].Mode = USER_PLAY                           Code/TMSrv/ProcessDBMessage.cpp:906
```

The contract consequence: a handler may only move the session into an **intermediate** mode; the terminal modes (`USER_SELCHAR`, `USER_PLAY`, `USER_SAVING4QUIT`) are written by `ProcessDBMessage.cpp`, not by handlers. Mode constants: `Code/TMSrv/CUser.h:26-37`.

### 2.3 Tertiary flow: server-injected synthetic packets (re-entrant entry into the same contract)

```
 1. Timer/AI code decides an NPC or player acts
 2. ProcessAffect(idx) builds MSG_Attack/attack variants on the stack  Code/TMSrv/Server.cpp:4513-4622
 3. Loop guard idx >= MAX_USER -> skip (contract: ID must be < MAX_USER)  Code/TMSrv/Server.cpp:4518
 4. ProcessClientMessage(idx, (char*)&sm, TRUE)                        Code/TMSrv/Server.cpp:4622
    also Code/TMSrv/Server.cpp:5061-5062, :5370-5371, :7449-7464
 5. Dispatcher: isServer == TRUE skips the SKIPCHECKTICK drop rule
 6. Same handler runs with a stack-local packet whose Size/ID/Damage fields are hand-set
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---|---|---|---|
| BR-01 | Interface ABI | Every handler is `void Exec_MSG_<Name>(int conn, char *pMsg)` with no return value; one handler per translation unit; all declared in the contract header | `Code/TMSrv/ProcessClientMessage.h:74-131` |
| BR-02 | Identity | The acting entity is the `conn` argument; `pMsg->ID` must never be trusted as identity | `Code/TMSrv/ProcessClientMessage.h:70`; `Code/TMSrv/Server.cpp:7456-7464` |
| BR-03 | Ordering | Handlers execute synchronously, one at a time, in arrival order, on the UI thread; no handler calls another handler | `Code/TMSrv/Server.cpp:3973-4002`; 0 inter-handler call sites |
| BR-04 | Validation (dispatcher) | `pMsg->ID` must satisfy `0 <= ID < MAX_USER (1000)`, otherwise the packet is logged and dropped | `Code/TMSrv/ProcessClientMessage.cpp:42-51` |
| BR-05 | Validation (dispatcher) | While `ServerDown >= 120`, all client packets are dropped without reply | `Code/TMSrv/ProcessClientMessage.cpp:53-54` |
| BR-06 | Liveness | Any accepted packet from `0 < conn < MAX_USER` stamps `LastReceiveTime = SecCounter` before routing | `Code/TMSrv/ProcessClientMessage.cpp:56-57` |
| BR-07 | Suppression | `_MSG_Ping` is consumed by the dispatcher and never reaches a handler | `Code/TMSrv/ProcessClientMessage.cpp:59-60` |
| BR-08 | Anti-spoof | A client packet with `ClientTick == SKIPCHECKTICK` is dropped; trusted internal injections set `isServer = TRUE` to bypass | `Code/TMSrv/ProcessClientMessage.cpp:63-64`; `Code/Basedef.h:172` |
| BR-09 | Routing | Message types without a `case` label are silently discarded: no log, no crack counter, no reply | `Code/TMSrv/ProcessClientMessage.cpp:66-311` (no `default`) |
| BR-10 | Anti-cheat | `_MSG_UpdateScore` from a client is a honeypot: log plus `AddCrackError(conn, 2, 91)` | `Code/TMSrv/ProcessClientMessage.cpp:106-110` |
| BR-11 | Routing | Six wire variants share three handlers; the handler must branch on `m->Type` | `Code/TMSrv/ProcessClientMessage.cpp:96-100, 192-196, 266-269` |
| BR-12 | Session state | Each handler must reject callers whose session is not in the required mode (typically `USER_PLAY`) and, for gameplay actions, whose character HP is 0 | Pattern at `Code/TMSrv/_MSG_UseItem.cpp:45-79`; enforced by 27/58 |
| BR-13 | Ownership of buffer | `pMsg` aliases the socket receive buffer; handlers may rewrite `Type`/`ID`/payload in place and forward to DB, or build a new local frame | `Code/TMSrv/_MSG_AccountLogin.cpp:73-92` |
| BR-14 | Server authority | Client-supplied item descriptors must be re-validated against server-side inventory before any mutation | `Code/TMSrv/_MSG_CombineItem.cpp:25-44`; `Code/Basedef.cpp:2340-2372` |
| BR-15 | Anti-cheat escalation | Protocol violations accrue `NumError` via `AddCrackError`; the `TRUE` return means "already logged out, abort now" | `Code/TMSrv/Server.cpp:684-707`; use at `Code/TMSrv/_MSG_Action.cpp:77-78` |
| BR-16 | Delivery | Responses are queued (`AddMessage`) or queued+flushed (`SendOneMessage`); unflushed frames are delivered by the 1-second timer sweep | `Code/CPSock.cpp:686-693`; `Code/TMSrv/ProcessSecMinTimer.cpp:201-215` |
| BR-17 | Error reporting | A handler cannot report failure to the dispatcher; all failures are communicated by client message, log line, or crack counter | `Code/TMSrv/ProcessClientMessage.h:70` |
| BR-18 | Internal injection | Server-generated packets must use a user index as `ID` and must tolerate synthetic field values (`Damage = -1`, `ClientTick = SKIPCHECKTICK`, `ID = 0`) | `Code/TMSrv/Server.cpp:4518, 4600-4622, 5061, 7456-7464` |
| BR-19 | Aggregate header | Every handler TU receives all 14 subsystem headers and 12 global externs through this one include; re-declarations must match `Server.cpp` definitions exactly | `Code/TMSrv/ProcessClientMessage.h:29-68`; `Code/TMSrv/Server.cpp:254-389` |
| BR-20 | Type-space integrity | The 191 `_MSG_*` constants must remain pairwise distinct so `switch(std->Type)` stays unambiguous; no duplicate numeric value was found | `Code/Basedef.h:976-2200` region; verified 0 collisions |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 - Handler signature ABI

**Overview**
Every inbound message family is served by exactly one free function named `Exec_MSG_<SysName>`, with the fixed C ABI `void f(int conn, char *pMsg)`. The header declares 58 such functions; each is defined in its own `Code/TMSrv/_MSG_<Name>.cpp` file, and each file includes only this header.

**Detailed description**
The ABI is the load-bearing part of the contract. `conn` is the connection slot (0..999), `pMsg` is a non-const pointer to the raw frame, and the return type is `void`. There is no context object, no request/response descriptor, no status code and no ownership handle. Consequently the handler has full, unchecked access to every global in the server (`pUser`, `pMob`, grid arrays, guild tables) and to the socket send path. This is what allows 58 small functions to implement an entire game protocol without any plumbing, and it is also why no boundary can be enforced: the compiler cannot distinguish a handler that mutates only `conn` from one that mutates `pMob[7]`.

The second ABI element is the **name**. The handler name is the message name with an `Exec_` prefix (`Exec_MSG_UseItem` for `_MSG_UseItem`). Because the dispatcher is a hand-written `switch`, the name correspondence is the only cross-reference the reader has; there is no table, no registration macro, no reflection. Adding a message family requires three coordinated edits (constant in `Basedef.h`, declaration in this header, `case` in the dispatcher) with no compiler help if one is forgotten — the failure mode is the silent drop described in BR-09.

The third element is `pMsg` mutability. Because the pointer is `char *` and not `const char *`, the contract explicitly permits write-back. Handlers rely on this for two purposes: rewriting the header in place to forward the very same buffer to the DB server (`Code/TMSrv/_MSG_AccountLogin.cpp:73-81`, `_MSG_CharacterLogin.cpp:73-81`, `_MSG_CreateCharacter.cpp:37-45`, `_MSG_DeleteCharacter.cpp:28-35`, `_MSG_AccountSecure.cpp:28-31`, `_MSG_CapsuleInfo.cpp:30-34`), and canonicalising the payload before re-broadcast (`_MSG_Action.cpp:267-278` rewrites `TargetX/TargetY/Effect` before multicasting to the neighbourhood).

**Rule workflow**
1. Client packet `Type` selects a `case` in the dispatcher.
2. Dispatcher calls `Exec_MSG_X(conn, pMsg)` with the raw buffer pointer.
3. Handler casts `pMsg` to the concrete `MSG_*` structure (zero-copy).
4. Handler validates, mutates and/or forwards.
5. Handler returns `void`; control returns unconditionally to the dispatcher.

---

### Business Rule: BR-02 - Identity comes from `conn`, never from `pMsg->ID`

**Overview**
Although every frame carries an `ID` field in its 12-byte header, handlers must treat the `conn` argument as the only authoritative identity. `ID` is repurposed as a routing tag (destination connection for DB replies, or a literal 0 for server-injected frames).

**Detailed description**
The dispatcher validates `std->ID` (`Code/TMSrv/ProcessClientMessage.cpp:42`) but never rewrites it for the handler's benefit, and handlers do not read it. Instead they use `conn` throughout: `pUser[conn]`, `pMob[conn]`. This is a deliberate and necessary split, because the same handler is also driven by three server-internal injections in which `ID` is meaningless or wrong: `Code/TMSrv/Server.cpp:7456` sets `sm.ID = 0` while dispatching with `partyconn` as `conn`; `Code/TMSrv/Server.cpp:4596` sets `sm.ID = idx` where `idx` is a mob/user index used for attack attribution and `ClientTick = 0xE0A1ACA` (`SKIPCHECKTICK`) marks it as trusted.

The rule has a security purpose as well. If a handler trusted `m->ID`, a client could target another session by simply editing the header of its own packet — the dispatcher would accept any value `< MAX_USER`, including another player's slot. Because every mutating statement is anchored on `conn`, the worst a forged `ID` can do is fail the range gate (dropping the packet) or be ignored. The only place the DB leg reads `ID` is the DB server side, after the handler has overwritten it with `conn` (`m->ID = conn`), which keeps that inversion intentional rather than accidental.

The cost of this rule is that the `ID` field carries two incompatible meanings over the same wire type — "source connection" for client→game and "destination connection" for game→DB/game→client. Any reader of `MSG_STANDARD` must know which leg of the pipeline it is looking at; a malicious or malformed `ID` on a client→game packet has no effect, which is the intended safe default.

**Rule workflow**
1. Client sends frame with arbitrary `ID`.
2. Dispatcher checks `0 <= ID < MAX_USER`; failure means log-and-drop.
3. Handler ignores `ID` entirely, deriving the actor from `conn`.
4. If the frame is forwarded to the DB, the handler overwrites `ID = conn` so the reply can be routed back.

---

### Business Rule: BR-03 - Synchronous, single-threaded, in-order dispatch

**Overview**
Packets are processed on the Win32 message-pump thread, in the exact order they were decoded, and are fully handled before the next frame is touched. Handlers never call other handlers; they cooperate through global state and shared helpers.

**Detailed description**
The receive loop drains the socket buffer with `while (1) { Msg = ReadMessage(...); if (!Msg) break; ProcessClientMessage(...); }` (`Code/TMSrv/Server.cpp:3973-4002`). There is no worker thread, no queue, no `PostMessage` deferral, and no mutex anywhere in the path; consequently all 58 handlers execute on the same thread as the Windows message loop that also runs the second/minute timers, AI and broadcasting. This gives a strong serialisation guarantee: two packets from the same connection can never interleave, and a handler can read-modify-write `pUser[conn]` without any locking. It also gives a strong denial-of-service guarantee in the negative direction: a slow handler blocks the entire server, one that writes an infinite loop hangs everything, and one that blocks on I/O stalls the world.

Cross-connection interleaving is at frame granularity: connection A's packet is fully handled before connection B's next packet is read, because the loop drains one connection's buffer per `WSA_READ` message and each connection posts its own message. The practical consequence for handler authors is that any "check then act" sequence inside one handler is atomic with respect to other players, but a sequence split across two packets is not — which is why stateful flows (trade, party, challenge) all carry explicit state fields (`Trade.OpponentID`, `TradeMode`, `LastReqParty`) rather than relying on timing.

No handler calls another handler: a full search for `Exec_MSG_*` call sites returns only the 58 dispatcher call sites in `Code/TMSrv/ProcessClientMessage.cpp`. Reuse happens at the helper level instead (`RemoveTrade`, `RemoveParty`, `CharLogOut`, `DoTeleport`, `DoRecall`, `SendScore`, `SendSetHpMp`), and those helpers may call `CloseUser`-adjacent teardown logic, which is why `AddCrackError` has to signal "already logged out" via its return value (BR-15).

**Rule workflow**
1. `WSA_READ` for one connection.
2. `ReadMessage` decodes one frame.
3. `ProcessClientMessage` runs one handler to completion.
4. Loop repeats until the buffer is drained.
5. Return to the message pump; timers and other connections proceed.

---

### Business Rule: BR-04 - Dispatcher ID range gate

**Overview**
Before any routing, `ProcessClientMessage` requires `std->ID >= 0 && std->ID < MAX_USER` (1000). Violations are logged with type/size/keyword and the frame is discarded.

**Detailed description**
This is the only bounds check the dispatcher performs on frame content, and it exists to protect the server-injected path rather than the client path: internal callers construct packets whose `ID` is an entity index, and entity indexes at or above `MAX_USER` are NPCs/mobs that have no `pUser` entry. Without the gate, a synthesized packet for a mob could drive a handler that indexes `pUser[idx]` out of the intended range. The 4622-line injection site is guarded twice — once by `ProcessAffect`'s own `if (idx >= MAX_USER ...) goto LABEL_59;` (`Code/TMSrv/Server.cpp:4518`) and once here — while `Code/TMSrv/Server.cpp:5061` and `:5370` guard explicitly at the call site, and `:7449` guards with `partyconn < MAX_USER`.

For client traffic the rule is effectively a sanity assertion: legitimate clients echo their own slot, so a violation means a broken or hostile client, and the server prefers a log line over a crack counter. The log format is `err,packet Type:%d ID:%d Size:%d KeyWord:%d` written with `sprintf` into the shared global `temp[4096]` (`Code/TMSrv/ProcessClientMessage.cpp:46-48`), which is also the buffer every handler uses for its own formatting — an implicit "no handler may assume `temp` survives a call" convention.

The gate does not validate `conn`. `conn` is only used a few lines later for the liveness stamp and is otherwise trusted from the caller; all four call sites are internal and pass a validated index. A future caller that passed an unvalidated `conn` would therefore reach handlers able to index `pUser[conn]` out of bounds, because the dispatcher's check is on `std->ID`, a different variable.

**Rule workflow**
1. Cast `pMsg` to `MSG_STANDARD`.
2. Test `ID < 0 || ID >= MAX_USER`.
3. On failure: format and log, return without reply.
4. On success: continue to the maintenance gate (BR-05).

---

### Business Rule: BR-05 - Maintenance gate

**Overview**
If `ServerDown >= 120`, the dispatcher returns immediately: no handler, no reply, no liveness stamp. `ServerDown` is `-1000` in normal operation and is incremented once per second after a reboot is scheduled, reaching 120 two minutes later, at which point the process closes.

**Detailed description**
The variable is tri-state: `-1000` means "not scheduled", `1..119` means "countdown in progress (server still serving)", and `>= 120` means "closing now". It is initialised at `Code/TMSrv/Server.cpp:46`, set to `1` by the reboot path (`Code/TMSrv/Server.cpp:4094`, `Code/TMSrv/imple.cpp:651`), incremented in `ProcessSecMinTimer` (`Code/TMSrv/ProcessSecMinTimer.cpp:96-113`), and consumer-visible through periodic notices (`ServerDown % 20 == 1` triggers a message from the string table).

The important contract detail is the **ordering**: the gate runs before the liveness stamp and before the ping check, so during the final two minutes a connected client stops accumulating `LastReceiveTime` updates and receives no `SendClientMessage` acknowledgements, even though the countdown notices themselves continue to be pushed. Handlers therefore cannot rely on being called at all; any handler that performs a save or a release on the assumption that "the client will send something before shutdown" is unsound, and the authoritative save path is `SaveAll`/`CharLogOut` driven from timers, not from handlers.

The gate is also a clean shutdown affordance: because `ProcessClientMessage` is the single funnel, one `if` disables the whole inbound surface without touching 58 handlers. The parallel accept path applies the same policy earlier, refusing connections when `ServerDown != -1000` (`Code/TMSrv/Server.cpp:4032`).

**Rule workflow**
1. ID gate passed.
2. Test `ServerDown >= 120`.
3. If true: return; frame silently discarded, no client feedback.
4. If false: continue to liveness stamping.

---

### Business Rule: BR-06 - Liveness stamping before routing

**Overview**
For `0 < conn < MAX_USER`, every packet that reaches step 4 refreshes `pUser[conn].LastReceiveTime = SecCounter`, regardless of type, validity beyond the ID gate, and regardless of whether a handler will be found.

**Detailed description**
`LastReceiveTime` is the input to the idle-timeout sweep (`CheckIdle`), which is what makes the server tolerant of clients that only send pings. Because the stamp happens **before** the ping branch (`Code/TMSrv/ProcessClientMessage.cpp:56-60`), a client that sends only `_MSG_Ping` frames stays connected forever without ever invoking a handler — an intentional design in this protocol generation but a notable one, since ping frames are also the cheapest possible traffic: 12 bytes each.

The guard `conn > 0 && conn < MAX_USER` silently excludes slot 0. Slot 0 is reserved: `GetEmptyUser` returns `0` to mean "no free slot" (`Code/TMSrv/Server.cpp:4011-4018`) and the accept path rejects indexes `>= MAX_USER - ADMIN_RESERV` with a "reconnect" message (`Code/TMSrv/Server.cpp:4022-4027`). Handlers still index `pUser[0]` freely, so slot 0 is a valid in-memory object that is simply never a live client.

The stamp is also the only per-packet bookkeeping the dispatcher performs. Nothing counts packets, nothing rate-limits, and nothing tracks per-type frequency; throttling is entirely a handler concern (`LastAttackTick`, `LastActionTick`, `LastIllusionTick`, `PotionTime`, `UseItemTime`, `AttackTime`).

**Rule workflow**
1. Test `conn > 0 && conn < MAX_USER`.
2. If true: `pUser[conn].LastReceiveTime = SecCounter`.
3. Continue to ping suppression and routing.

---

### Business Rule: BR-07 - Ping is consumed, never routed

**Overview**
`_MSG_Ping` (`160 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME`, numeric 928) causes an immediate return. There is no echo, no reply, and no handler.

**Detailed description**
The frame's only observable effect is the liveness stamp from BR-06, which already happened. The client therefore cannot use ping as a round-trip latency probe against TMSrv: no response is generated on this path, so a client that measures ping must rely on application-level traffic or on the lower-level TCP behaviour. Removing the early return would not help either, since no `case _MSG_Ping` exists in the switch — the frame would fall through the silent-drop path (BR-09).

The rule's real function is performance: ping is the highest-frequency frame type in a live shard, and short-circuiting it after one comparison avoids a `switch` dispatch over 64 labels and, more importantly, avoids any chance of a handler being written for it that would allocate, log, or broadcast. It also keeps ping out of the crack counters: an over-enthusiastic anti-cheat rule would otherwise punish normal keep-alive traffic.

Note the asymmetry with `_MSG_UpdateScore`: the same 6-argument header layout, but one Type is dropped silently and the other is an active trap. The dispatcher's policy is therefore per-type and hand-curated, not systematic.

**Rule workflow**
1. Liveness stamp applied.
2. Test `std->Type == _MSG_Ping`.
3. If true: return, no reply.
4. If false: continue to the tick-spoof check.

---

### Business Rule: BR-08 - `SKIPCHECKTICK` anti-spoof

**Overview**
A client-originated packet whose `ClientTick` field equals `SKIPCHECKTICK` (235543242 / `0x0E0A1ACA`, `Code/Basedef.h:172`) is dropped. Server-injected packets pass `isServer = TRUE` and bypass the check.

**Detailed description**
`ClientTick` is the client's millisecond timestamp, used by handlers as a movement/attack cadence input (`_MSG_Action.cpp:63`, `_MSG_Attack.cpp` attack-rate checks). The sentinel value is the "trusted" marker: internal injections set `sm.ClientTick = 0xE0A1ACA` (`Code/TMSrv/Server.cpp:4600`) and several handlers use the same sentinel to disable their rate limiters, e.g. `if (pUser[conn].LastActionTick != 235543242 && movetime < ...)` (`Code/TMSrv/_MSG_Action.cpp:92`, `:115`).

If a client could set the sentinel, it would inherit every "trusted" bypass in the codebase at once — unlimited movement, unlimited attack cadence, illusions without cooldown. BR-08 is therefore the single choke point that keeps the sentinel meaningful: the check happens before any handler sees the value, and it is the *only* place the `isServer` flag has any effect. Removing or reordering this check would silently disable a whole family of rate limits inside handlers that have no way to detect the substitution.

The rule also demonstrates the contract's trust model in one line: trust is a **caller-supplied boolean** (`isServer`), not a property of the frame. Any new code path that calls `ProcessClientMessage(..., TRUE)` with attacker-influenced content would bypass anti-speed checks. All four current internal call sites construct their frames locally on the stack, so the boolean is safe today; the risk is structural, not actual.

**Rule workflow**
1. Test `isServer == FALSE && std->ClientTick == SKIPCHECKTICK`.
2. If true: return (log nothing; no crack counter).
3. If false: dispatch normally; handlers may compare against the sentinel to disable rate limits.

---

### Business Rule: BR-09 - Silent discard of unhandled message types

**Overview**
The `switch (std->Type)` has 64 `case` labels and **no `default`**. Any Type not listed — including the ~127 other constants defined in `Basedef.h`, and every unknown value — falls through to `return;` at the end of the function.

**Detailed description**
The absence of a `default` is the most consequential silent behaviour in the component. Three classes of traffic land there: legitimate protocol frames that TMSrv does not implement (e.g. `_MSG_CreateItem`, `_MSG_CNFGetItem`, `_MSG_ShopList`, `_MSG_ItemSold`, `_MSG_CloseShop`, `_MSG_SetHpMode`, `_MSG_UpdateCarry`, `_MSG_UpdateEquip`, `_MSG_CNFDropItem` and many DB-only constants), client emissions of game→client types, and malformed values. For none of them is anything logged, so a protocol mismatch between client and server surfaces at the client as "nothing happens" and at the server as complete silence.

This makes the component fail-open with respect to diagnosis and fail-closed with respect to behaviour. Operationally that is usually the right trade (logging every unknown frame would flood the log under a scanning client), but it has two costs. First, when a feature is added on the client side without a matching `case`, there is no server-side evidence at all. Second, the honeypot at BR-10 is the only type that reports a wrong-direction packet, so a client probing the space can discover which types are traps only by being logged out — an asymmetry a careful attacker can exploit to map the implemented surface.

Note also that a `default:` branch cannot simply be added: the dispatcher is shared by client traffic and by DB-origin frames (BR-18), and the DB shares the same numeric Type space for a few constants (`_MSG_War`, `_MSG_GuildAlly`, `_MSG_AccountSecure`), so logging every unmatched Type would produce noise on any future unification of the two dispatchers.

**Rule workflow**
1. `switch (std->Type)` evaluates 64 labels.
2. No match: control reaches `return;` at the function end (`Code/TMSrv/ProcessClientMessage.cpp:311-312`).
3. No log, no reply, no counter, no state change.

---

### Business Rule: BR-10 - `_MSG_UpdateScore` honeypot

**Overview**
`_MSG_UpdateScore` (numeric 822) is legitimately a game→client frame. If a client sends it, the dispatcher logs the account name and IP and calls `AddCrackError(conn, 2, 91)` — the only handler-less type with an active penalty.

**Detailed description**
This is the component's single explicit tamper detector. The frame declares a full `STRUCT_SCORE`, affect list, guild, HP/MP and magic — precisely the fields a cheater would like to assert. Because the constant is declared with both directions (`54 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME`, `Code/Basedef.h:1466`), the Type is accepted by the wire format and reachable by the client; the dispatcher therefore treats it as a probe and penalises it instead of ignoring it, which is strictly better than BR-09's silence.

The penalty is `2` points against a threshold of `2,000,000,000` (`Code/TMSrv/Server.cpp:694`), so a single probe is harmless and the rule functions as telemetry rather than as a ban. Only a client sending on the order of a billion such frames — or, more realistically, a client that trips the heavier penalties elsewhere (`val = 5` and `val = 10` at various handler sites) — will be disconnected. The log line uses the account name and IP from `pUser[conn]`, which ties the event to an identity even when the session is not yet authenticated.

Note the ordering subtlety: `AddCrackError` is called *after* the ID-gate and liveness stamp, so the probe still refreshes `LastReceiveTime` and still reaches `pUser[conn]`. It cannot be used to disconnect an idle victim, because the penalty requires a frame from that connection.

**Rule workflow**
1. Type matched to `case _MSG_UpdateScore`.
2. `Log("cra client send update score", pUser[conn].AccountName, pUser[conn].IP)`.
3. `AddCrackError(conn, 2, 91)`: increments `NumError`; if the total reaches the threshold the user is logged out.
4. Break out of the switch; no reply is sent.

---

### Business Rule: BR-11 - Wire-variant aliasing

**Overview**
Three handler families accept multiple Type values and must therefore branch on `m->Type` themselves: movement (`_MSG_Action` 876, `_MSG_Action2` 870, `_MSG_Action3` 872 → `Exec_MSG_Action`), combat (`_MSG_Attack` 871, `_MSG_AttackOne` 925, `_MSG_AttackTwo` 926 → `Exec_MSG_Attack`), and one crafting variant (`_MSG_CombineItemOdin` 722, `_MSG_CombineItemOdin2` 738 → `Exec_MSG_CombineItemOdin`).

**Detailed description**
Aliasing exists because the protocol encodes semantically different actions in the same structure with a different Type tag: `_MSG_Action` is a walk, `_MSG_Action2` is a stop/stand, `_MSG_Action3` is the "illusion" skill move that costs MP (`Code/TMSrv/_MSG_Action.cpp:72-145`). The dispatcher funnels them so that the shared validation preamble (mode, HP, trade interruption, viewport limits, grid occupancy) is written once — a deliberate reuse win — but it transfers the obligation to re-dispatch to the handler. The handler does this with an `if/else if` chain on `m->Type` (`_MSG_Action.cpp:72`, `:113`, `:136`), which means the handler must know the wire constants, not just its payload struct.

The danger of the pattern is that the aliased branches have different privileges. `_MSG_Action3` charges mana and enforces a class/skill requirement before the shared movement code runs (`_MSG_Action.cpp:74-111`); if a client sent `_MSG_Action` instead, the mental path would be skipped while the movement still commits. That is guarded by the handler's own branch, but it is exactly the kind of rule that a future refactor of the preamble could silently break, because the compiler cannot see that `Type` selects a privilege class.

For combat the alias set is wider still: `_MSG_Attack` (area), `_MSG_AttackOne` (single target, used by AI injections) and `_MSG_AttackTwo` share `Exec_MSG_Attack`, and the handler must distinguish "client claims area attack" from "server simulated a single-target hit", because only the former needs cadence and range validation. This is the reason `Exec_MSG_Attack` is 1790 lines: the aliasing concentrates four distinct security surfaces in one function.

**Rule workflow**
1. Dispatcher groups the labels; one handler is invoked.
2. Handler branches on `m->Type` to select the privilege/validation profile.
3. Handler applies the rules for that variant.
4. Responses echo the same Type so peers can distinguish the action.

---

### Business Rule: BR-12 - Session-state and vitality preconditions belong to the handler

**Overview**
The dispatcher performs no authorisation. Each handler must reject callers whose session is not in the required mode (normally `USER_PLAY`), and most gameplay handlers must additionally reject dead characters (`MOB.CurrentScore.Hp == 0`) and any active trade (`Trade.OpponentID` / `TradeMode`).

**Detailed description**
The canonical preamble is visible in `Exec_MSG_UseItem` (`Code/TMSrv/_MSG_UseItem.cpp:45-79`) and `Exec_MSG_TradingItem` (`Code/TMSrv/_MSG_TradingItem.cpp:24-42`): reject if `Mode != USER_PLAY` (reply `SendHpMode`), reject if `Trade.OpponentID` (tear the trade down on both sides), reject if `TradeMode` (auto-trade is open), reject if `Hp == 0` (reply `SendHpMode` plus a crack point). Twenty-seven handlers implement the `USER_PLAY` rejection; one (`Restart`) branches on `USER_PLAY` without rejecting; four substitute a flow-specific gate (`AccountLogin` requires `USER_ACCEPT`; `CreateCharacter`, `DeleteCharacter` and `CharacterLogin` require `USER_SELCHAR`); and twenty-six apply **no session gate at all** — `SetShortSkill`, `PKMode`, `Deprivate`, `ChangeCity`, `ReqTeleport`, `Quest`, the ten `CombineItem*` handlers, `CharacterLogout`, `RemoveParty`, `War`, `GuildAlly`, `CapsuleInfo`, `AccountSecure`, `NoViewMob`, `InviteGuild`, `Challange` and `ChallangeConfirm`.

The risk of the unguarded group is not uniform. For the forwarding handlers (`CapsuleInfo`, `AccountSecure`, `War`, `GuildAlly`, `CreateCharacter`-style flows) the DB server validates independently, so the missing local gate is mostly redundant. For the **local mutation** handlers it is not redundant: `Exec_MSG_SetShortSkill` writes `pMob[conn].MOB.SkillBar` and `pUser[conn].CharShortSkill` unconditionally (`Code/TMSrv/_MSG_SetShortSkill.cpp:24-25`); `Exec_MSG_PKMode` writes `pUser[conn].PKMode` before any check (`Code/TMSrv/_MSG_PKMode.cpp:24-25`); `Exec_MSG_ChangeCity` writes the merchant byte from the current position (`Code/TMSrv/_MSG_ChangeCity.cpp:23-32`). Those three allow a pre-login or post-logout connection to alter persistent character state, which contradicts the precondition every other gameplay handler enforces and is the clearest internal inconsistency of the contract.

**Rule workflow**
1. Cast `pMsg`.
2. Test `pUser[conn].Mode != USER_PLAY` (or the flow-specific mode).
3. On failure: call `SendHpMode(conn)` or a specific message, optionally `AddCrackError`, return.
4. Test trade state and HP; tear down or reject.
5. Proceed to payload validation.

---

### Business Rule: BR-13 - Buffer ownership and mutate-and-forward

**Overview**
`pMsg` points into the CPSock receive buffer. A handler may write through it, and six handlers exploit this to forward the *same* buffer to the DB server after rewriting `Type` and `ID`. Handlers must therefore finish all reads of the payload before transferring it.

**Detailed description**
`CPSock::ReadMessage` returns `&pRecvBuffer[nProcPosition]` after de-obfuscating in place (`Code/CPSock.cpp:414`, `:430-453`). The buffer is not copied, not reference-counted and not protected; it belongs to the connection and is only repositioned (`nRecvPosition = nProcPosition = 0`) when fully drained. A handler that hands the pointer to `DBServerSocket.SendOneMessage` is therefore handing over a pointer into the *client* socket's buffer, which is safe only because `SendOneMessage` copies the bytes synchronously into the DB socket's send buffer (`Code/CPSock.cpp:686-693`).

The forwarding pattern is uniform: rewrite `m->Type` to the DB-side constant, set `m->ID = conn`, send `sizeof(MSG_X)`, and advance the session mode so the reply is interpretable. `Exec_MSG_AccountLogin` does all four (`_MSG_AccountLogin.cpp:73-95`), `Exec_MSG_CharacterLogin` does them at `:73-81`, `Exec_MSG_CreateCharacter` at `:37-45`, `Exec_MSG_DeleteCharacter` at `:28-35`, `Exec_MSG_AccountSecure` at `:28-31`, and `Exec_MSG_CapsuleInfo` at `:30-34`. The size passed is always `sizeof(STRUCT)` — not the client's declared `Size` — which is the point where the inbound Size contract (BR-04/BR-09 do not check it) silently substitutes "the structure the server expects" for "the bytes the client sent".

Because the pointer is the live buffer, a handler that queues an outgoing message that *points at* `pMsg` rather than copying it would alias the receive buffer; no current handler does this (`AddMessage` copies), but nothing prevents it.

**Rule workflow**
1. Handler reads all needed fields.
2. Rewrites `Type`/`ID` (and optionally payload) in place.
3. Hands the pointer to `DBServerSocket.SendOneMessage`, which copies.
4. Advances the session mode.
5. Returns; the receive buffer may be reset by the next `ReadMessage`.

---

### Business Rule: BR-14 - Server-authoritative revalidation of client item descriptors

**Overview**
Every item operation must resolve the item through the server's own memory (`GetItemPointer`) and, where the client also supplies an item descriptor, compare it byte-for-byte with the server copy before mutating anything.

**Detailed description**
`GetItemPointer(STRUCT_MOB *mob, STRUCT_ITEM *cargo, int type, int pos)` is the shared resolver (`Code/Basedef.cpp:2340-2372`). It bounds-checks the container (`EQUIP` < 16, `CARRY` < 64, `CARGO` < 128), rejects unknown container ids, and finally rejects any slot whose `sIndex` is outside `[0, MAX_ITEMLIST)`. Returning `NULL` is the handler's signal to abort; handlers are expected to check it (`_MSG_UseItem.cpp:88-93`, `_MSG_DropItem.cpp:99`, `_MSG_Sell.cpp:84`, `_MSG_TradingItem.cpp:76-80`). This is the correct design — one validator for 4 containers — but it means the contract's bounds guarantee lives in `Basedef.cpp`, not in this component, and a handler that dereferences the result without a `NULL` check loses it entirely.

The second half of the rule is anti-desync. The `CombineItem` family receives a full `MSG_CombineItem` containing both `Item[MAX_COMBINE]` descriptors and `InvenPos[MAX_COMBINE]` slot indexes. Before using either, the handler walks the arrays and, for each non-empty descriptor, verifies `memcmp(&pMob[conn].MOB.Carry[invPos], &m->Item[i], sizeof(STRUCT_ITEM)) == 0` (`_MSG_CombineItem.cpp:25-44`, identical blocks in all eleven combine variants, e.g. `_MSG_CombineItemShany.cpp:25-44`). A mismatch is logged as `err,msg_Combine - item remove or changed.` and answered with `_MSG_CombineComplete, 0`. The purpose is to prevent a client from crafting with an item it no longer owns, and to detect an inventory desync instead of silently consuming the wrong items.

Note the residual gap: the comparison is against the *carry* array only, using `pMob[conn].MaxCarry` as the upper bound rather than `MAX_CARRY`; slots between `MaxCarry` and `MAX_CARRY` are treated as invalid via the bound check, which is stricter than `GetItemPointer` and therefore safe, but the two definitions of "valid carry slot" now differ across the codebase.

**Rule workflow**
1. Handler extracts `type`/`pos` (or the combine arrays) from the payload.
2. `GetItemPointer` bounds-checks and validates `sIndex`; `NULL` aborts.
3. Combine family: `memcmp` client descriptor against server inventory; mismatch logs and replies "failed".
4. Only then mutate (consume, move, sell, drop) and send the confirmation.

---

### Business Rule: BR-15 - `AddCrackError` escalation and the `TRUE`-return duty

**Overview**
Protocol violations are accumulated per session by `AddCrackError(conn, val, Type)`. It increments `pUser[conn].NumError` and, when the total reaches `2,000,000,000`, sends "bad network packets", calls `CharLogOut(conn)` and returns `TRUE`. Any handler that receives `TRUE` must return immediately.

**Detailed description**
The function (`Code/TMSrv/Server.cpp:684-707`) is deliberately crude: a saturating counter with a single threshold, plus a log line suppressed for three types (`3`, `8`, `15`) that are considered noise. There is no decay, no per-type accounting, no time window and no separate storage, so `NumError` is a session-lifetime accumulator that a client can only reset through a successful relog (it is zeroed in `Exec_MSG_Restart`, `Code/TMSrv/_MSG_Restart.cpp:26`). The threshold is high enough that ordinary desync cannot reach it, which makes it a backstop for automated exploit loops rather than a per-incident punishment.

The contract obligation on handlers is the return value. `Exec_MSG_Action` demonstrates correct use: `if (AddCrackError(conn, 10, 28) == TRUE) return;` (`_MSG_Action.cpp:77-78`) — the handler stops before touching movement state, because `CharLogOut` has already torn the session down and any subsequent write would be operating on a logged-out character. Across the handler set there are 42 `AddCrackError` call sites and only three test the result (`_MSG_Action.cpp:77`, `_MSG_Attack.cpp:186`, `_MSG_Attack.cpp:207`). Call sites that ignore the return value are therefore not merely sloppy; they continue mutating state after logout, which can resurrect a saved character in memory (the canonical "dupe" hazard in this generation of server). The dominant pattern is a bare `AddCrackError(conn, 1, nn);` followed by an unconditional `return;` from the current branch, which is safe only when the very next statement is also a return.

The `Type` argument is a numeric fault code (not the message type) used purely for log correlation; the same code appears with the same meaning in `CrackLog` text logs. There is no table of codes in the component, so the mapping exists only in the compiled call sites.

**Rule workflow**
1. Handler detects an anomaly.
2. `AddCrackError(conn, val, code)` logs (unless suppressed) and adds `val` to `NumError`.
3. If the threshold is crossed: message, `CharLogOut`, return `TRUE`.
4. Handler checks the result and returns before any further mutation.

---

### Business Rule: BR-16 - Response delivery: queue, flush, or wait for the timer

**Overview**
Handlers never write to a socket. They call `Send*` helpers in `SendFunc.cpp`, which either append to the per-connection send buffer (`AddMessage`, batched) or append and immediately transmit (`SendOneMessage` = `AddMessage` + `SendMessageA`). Anything left unflushed is transmitted by the once-per-second sweep in `ProcessSecMinTimer`.

**Detailed description**
The two halves of this rule interact in a way that is easy to get wrong. `SendClientMessage`, `SendClientSignal*`, `SendScore`, `SendItem`, `SendSetHpMp`, `SendEtc` and friends all end in `AddMessage` (`Code/TMSrv/SendFunc.cpp:44` for the canonical example) — they queue. Only `SendOneMessage`, and explicit `cSock.SendMessageA()` calls, actually push bytes. The safety net is `ProcessSecMinTimer.cpp:201-215`, which walks slots `1..MAX_USER`, and for any user with `Mode != USER_EMPTY` and `nSendPosition != 0` calls `SendMessageA()`; a failure there logs and closes the connection.

Consequences a handler author must respect: (a) a queued reply is not observable by the client until the tick or an explicit flush, so a handler cannot sequence a reply before a subsequent handler's reply by ordering alone — the *queue order* is what the client sees, and that order is preserved because all handlers run on one thread; (b) the sweep skips `Mode == USER_EMPTY`, so messages queued for a session that is torn down mid-handler are never flushed (intentional); (c) a handler that must answer before doing something slow has to flush itself, which is why the login handlers call `SendMessageA()` explicitly (`_MSG_AccountLogin.cpp:35,50,61,88`, `_MSG_CharacterLogin.cpp:42`, `_MSG_CharacterLogout.cpp:26`); (d) `AddMessage` silently drops the frame if the 128 KB send buffer or the socket is unusable (`Code/CPSock.cpp:518-533`), and no handler checks that return value, so "reply sent" is never guaranteed.

The string-side convention is equally part of the contract: user-visible text comes from `g_pMessageStringTable` indexed by `_NN_*`/`_SN_*`/`_DN_*`/`_DD_*` constants (`Code/TMSrv/Language.h`), so that a language pack can replace it. Some handlers violate this and embed literals (`"Wait a moment."`, `"Deleting Character. wait a moment."`, `"Login now, wait a moment."`), which are untranslatable.

**Rule workflow**
1. Handler calls a `Send*` helper.
2. Helper builds a `MSG_*` on the stack and calls `AddMessage` (queue) — or `SendOneMessage` (queue + flush).
3. Handler continues; further replies append after it.
4. Either the handler flushes explicitly, or the 1-second sweep flushes.
5. `SendMessageA` failure closes the connection on the sweep path only.

---

### Business Rule: BR-17 - No error channel back to the dispatcher

**Overview**
Handlers return `void`. Success, rejection, partial completion and fatal error are indistinguishable at the call site; the dispatcher never learns whether a packet did anything, and it continues draining the buffer regardless.

**Detailed description**
This is the deliberate simplification that makes the 58-way switch writable. It also fixes the semantics of every failure path: a rejection is a *client-visible message*, optionally plus a log line and a crack counter, and nothing else. Because the dispatcher cannot react, a handler that fails must return early (all 58 do, using `return;`) and must leave state consistent — there is no transaction, no rollback and no retry at this layer. The corollary is that a handler that mutates state and *then* discovers an error must undo it manually; several do (`RemoveTrade` on both sides, `SendItem` to resynchronise a mutated slot, `_MSG_UseItem.cpp:64-79`), and the ones that do not are the dupe candidates.

The pattern also means the dispatcher cannot implement policy it does not own: no rate limiting, no per-type quotas, no circuit breaking, no duplicate suppression. Everything of that kind is re-implemented per handler using the per-user tick fields, and the implementations are not uniform (`LastAttackTick`, `LastActionTick`, `LastIllusionTick`, `LastMove`, `LastChat`, `PotionTime`, `UseItemTime`, `AttackTime`, `Message`).

**Rule workflow**
1. Handler determines outcome.
2. Outcome is expressed only as: client message(s), `Log`/`CrackLog`/`ItemLog` lines, `AddCrackError` points, and state mutation.
3. Handler returns `void`; control returns to the dispatcher loop.

---

### Business Rule: BR-18 - Internal (server-originated) injection contract

**Overview**
`ProcessClientMessage(conn, pMsg, TRUE)` is called from four sites inside the world tick to run gameplay handlers for server-simulated actions. Injected frames must use a user index below `MAX_USER` as `ID`, mark `ClientTick` with `SKIPCHECKTICK`, and tolerate synthetic payload values.

**Detailed description**
The four sites are: the `Fada` companion attack (`Code/TMSrv/Server.cpp:4622`, inside `ProcessAffect`, guarded at `:4518`), two area-skill simulations (`:5061-5062`, `:5370-5371`, both guarded by `if (idx < MAX_USER)`), and the party-invite re-dispatch (`:7449-7464`). They exist because the AI and skill code wants the *player* code path (damage application, broadcast, death handling) without duplicating it; the dispatcher is therefore reused as an internal command bus.

The constraints that follow for handler authors are subtle. Injected frames set fields to values a real client could not produce: `sm.ClientTick = 0xE0A1ACA` (the trusted sentinel), `sm.Dam[0].Damage = -1` (meaning "compute damage server-side"), `sm.SkillIndex = 32/34/36` (forced skill), and in the party case `sm.ID = 0` with `sm.LeaderID = novolider` while dispatching against `partyconn`. Any handler that assumes `Damage > 0` means "client claims damage", or that `ID` equals the sender, breaks under injection. The party case is the sharpest illustration: `_MSG_AcceptParty` must take the leader from `LeaderID` and the accepter from `conn`, ignoring `ID`.

The `isServer` flag itself carries exactly one rule (BR-08) and nothing else; it is *not* an authorisation bypass for the mode checks in BR-12. This matters because an injected frame arrives with whatever mode the target session is in — usually `USER_PLAY`, but the contract does not guarantee it, and a handler that rejects on mode will silently drop the injection. Conversely, `RemoveParty`'s re-dispatch is guarded by the caller for `partyconn < MAX_USER`, which is the pattern any new injection must copy.

**Rule workflow**
1. Tick code decides an entity acts.
2. Build `MSG_*` on the stack, zero-filled, with `Type`, `Size = sizeof(MSG_*)`, `ID = <user index>`, `ClientTick = SKIPCHECKTICK`.
3. Guard `idx < MAX_USER` before calling.
4. `ProcessClientMessage(idx, (char*)&sm, TRUE)`.
5. The same handler runs, seeing a trusted frame; it must not assume client authorship.

---

### Business Rule: BR-19 - Aggregate-header wiring contract

**Overview**
This header includes 14 project headers and 7 C-runtime/Win32 headers, and re-declares 12 globals. Every handler therefore compiles against the entire server type universe, and every re-declaration here must match the definition in `Server.cpp` exactly.

**Detailed description**
The include list (`ProcessClientMessage.h:29-43`) is `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`, `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h`, `CMob.h`, `CUser.h`, `CNPCGene.h`, `CReadFiles.h`, `CCastleZakum.h`, `CWarTower.h`. Because `Server.h` itself includes `Basedef.h`, `CPSock.h`, `CItem.h`, `CMob.h`, `CUser.h` and `CNPCGene.h`, the closure is the whole server; `CCastleZakum.h` and `CWarTower.h` add the two remaining gameplay statics. The compile-time effect is that 66 translation units depend on this single file, and any change to any of those 14 headers recompiles all 66 — including the ones that only ever call `SendHpMode`.

The extern block (`ProcessClientMessage.h:45-68`) duplicates declarations already present in `Server.h:44-64`: `hWndMain`, `ServerGroup`, `CurrentTime`, `BrState`, `g_pGuildWar`, `g_pGuildAlly`, `pMobGrid`, `pItemGrid`, `pHeightGrid`, `CurrentWeather`, `GuildImpostoID`, `mNPCGen`, `pUser`, plus one type not declared there (`STRUCT_GUILDINFO GuildInfo[65536]`). Duplication is legal only while the declarations stay identical; there is no compile-time guarantee that a future edit to one is mirrored in the other, and the definitions live in `Code/TMSrv/Server.cpp:254-389`. A mismatch in array extent, element type or const-ness would surface as a linker error at best and as silent out-of-bounds access at worst.

One concrete inconsistency inside this rule: the guild-related arrays are declared with a literal `65536` (`ProcessClientMessage.h:54-56`) while the game's own guild bound is `MAX_GUILD 4096` (`Code/Basedef.h:147`) and the zone bound is `MAX_GUILDZONE 5`. The `65536` sizes are therefore a wire/index space, not a game limit, and any handler that iterated to `MAX_GUILD` while the table is indexed by a wire field would be operating on a different domain than the one the table was sized for.

**Rule workflow**
1. A handler TU includes only `ProcessClientMessage.h`.
2. Preprocessor expands 14 project headers into the TU.
3. All 12 externs become visible, resolved at link time against `Server.cpp` definitions.
4. Any edit to an included header or to an extern's type rebuilds all 66 TUs.

---

### Business Rule: BR-20 - Type-space integrity

**Overview**
`switch (std->Type)` requires every dispatched constant to be numerically unique, and requires handlers not to interpret the `FLAG_*` bits inside `Type` as payload. The 191 `_MSG_*` constants in `Basedef.h` were checked for collisions: none exist.

**Detailed description**
`Type` is a `short` constructed as `(base | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME | FLAG_DB2GAME | FLAG_GAME2DB | ...)` (`Code/Basedef.h:925-946`). Direction flags are 0x0100/0x0200/0x0400/0x0800/0x1000/0x2000/0x4000, leaving 8 bits of index space per flag combination, and several base numbers are reused across different flag combinations by design (e.g. base 102 is `_MSG_PKInfo` = 358 for game→client and `_MSG_Action2` = 870 for both directions; base 139 is `_MSG_UpdateWeather` = 395 and `_MSG_Quest` = 651). The dispatcher compares the full composite value, so these are distinct; the human reader must therefore never reason about the base number alone, and a handler must never mask out the flags.

The check I performed (evaluate every `const short _MSG_* = (...)` in `Basedef.h`, including the `FLAG_*` identifiers) yields 191 constants and zero duplicate values, which is what makes the 64-label switch well-defined. The invariant is fragile for one reason: the constants are `const short` at namespace scope with hand-written flag expressions, and adding a family means picking a base byte and a direction mask that is free in that mask. Nothing checks it — the failure mode of a collision is that one `case` becomes unreachable (the compiler rejects duplicate case values only if both labels appear in the same switch; if the duplicate is not dispatched, one message family is silently absorbed by another), which would be a silent protocol bug.

**Rule workflow**
1. Author picks a base byte plus direction flags for the new message.
2. Constant is added to `Basedef.h` as `const short`.
3. Declaration is added to this header and a `case` to the dispatcher.
4. Only then does the family become reachable; until then it is silently dropped (BR-09).

---

## 4. Component Structure

```
Code/TMSrv/ProcessClientMessage.h           132 lines - the contract
├── 1-18    GPL header
├── 19      #pragma once                       (no include guard name, no include of stdint)
├── 21-27   Win32/CRT includes                 Windows.h, stdio.h, fcntl.h, time.h, math.h, io.h, errno.h
├── 29-43   project includes (14)              Basedef.h, CPSock.h, ItemEffect.h, Language.h,
│                                              CItem.h, Server.h, GetFunc.h, SendFunc.h, CMob.h,
│                                              CUser.h, CNPCGene.h, CReadFiles.h,
│                                              CCastleZakum.h, CWarTower.h
├── 45-68   extern block (12 names + 1 struct) hWndMain, ServerGroup, CurrentTime, BrState,
│                                              g_pGuildWar[65536], g_pGuildAlly[65536],
│                                              GuildInfo[65536], pMobGrid/pItemGrid/pHeightGrid
│                                              [4096][4096], CurrentWeather, GuildImpostoID[5],
│                                              mNPCGen, pUser[1000]
├── 70      ProcessClientMessage(int, char*, BOOL)      <- the single inbound entry point
└── 74-131  58 x void Exec_MSG_<Name>(int, char*)       <- the handler ABI
```

Consumers of the contract, by role:

```
Code/TMSrv/
├── ProcessClientMessage.cpp     dispatcher: ID gate, ServerDown, liveness, ping, SKIPCHECKTICK,
│                                64 case labels -> 58 handlers + 1 honeypot + 5 aliases
├── Server.cpp                   4 synthetic dispatches (4622, 5062, 5371, 7464); recv loop at 3975-4001
├── ProcessDBMessage.cpp         DB-side state machine that produces the modes handlers require
├── ProcessSecMinTimer.cpp       per-second flush sweep (201-215) + ServerDown countdown
├── MobKilled.cpp                includes contract for Send*/SendFunc; no handler
├── CCastleZakum.cpp             castle entry rules; includes contract
├── CWarTower.cpp                tower war; includes contract
├── imple.cpp                    admin/GM commands; includes contract
└── _MSG_*.cpp (58 files)        one Exec_MSG_* each; include the contract and nothing else
```

Handler file inventory with size and gate coverage (measured):

| Handler file | Lines | Mode gate | HP gate | AddCrackError | Early returns |
|---|---|---|---|---|---|
| `_MSG_UseItem.cpp` | 5727 | yes | yes | 1 | 277 |
| `_MSG_Quest.cpp` | 2713 | no | no | 0 | 8 |
| `_MSG_Attack.cpp` | 1790 | yes | yes | 12 | 21 |
| `_MSG_MessageWhisper.cpp` | 1379 | yes | yes | 0 | 117 |
| `_MSG_CombineItemOdin.cpp` | 581 | no | no | 0 | 25 |
| `_MSG_TradingItem.cpp` | 431 | yes | yes | 1 | 17 |
| `_MSG_Trade.cpp` | 426 | yes | yes | 1 | 30 |
| `_MSG_CombineItemEhre.cpp` | 396 | no | no | 0 | 9 |
| `_MSG_Action.cpp` | 341 | yes | yes | 9 | 19 |
| `_MSG_Buy.cpp` | 314 | yes | yes | 1 | 19 |
| `_MSG_GetItem.cpp` | 253 | yes | yes | 1 | 16 |
| `_MSG_ApplyBonus.cpp` | 251 | yes | yes | 1 | 14 |
| `_MSG_Sell.cpp` | 237 | yes | yes | 1 | 15 |
| `_MSG_MessageChat.cpp` | 204 | yes | no | 0 | 12 |
| `_MSG_ReqBuy.cpp` | 201 | yes | yes | 1 | 16 |
| `_MSG_CombineItemAilyn.cpp` | 153 | no | no | 0 | 6 |
| `_MSG_AcceptParty.cpp` | 151 | yes | no | 0 | 11 |
| `_MSG_DropItem.cpp` | 147 | yes | yes | 1 | 14 |
| `_MSG_CombineItem.cpp` | 145 | no | no | 0 | 7 |
| `_MSG_CombineItemTiny.cpp` | 142 | no | no | 0 | 6 |
| `_MSG_Challange.cpp` | 136 | no | no | 0 | 7 |
| `_MSG_CombineItemAgatha.cpp` | 131 | no | no | 0 | 5 |
| `_MSG_CombineItemLindy.cpp` | 123 | no | no | 0 | 8 |
| `_MSG_SendAutoTrade.cpp` | 121 | yes | yes | 1 | 11 |
| `_MSG_CombineItemAlquimia.cpp` | 116 | no | no | 0 | 5 |
| `_MSG_CombineItemShany.cpp` | 116 | no | no | 0 | 6 |
| `_MSG_PutoutSeal.cpp` | 113 | yes | yes | 1 | 8 |
| `_MSG_CharacterLogin.cpp` | 111 | no (`USER_SELCHAR`) | no | 0 | 2 |
| `_MSG_CombineItemExtracao.cpp` | 109 | no | no | 0 | 6 |
| `_MSG_UpdateItem.cpp` | 107 | yes | yes | 4 | 8 |
| `_MSG_SendReqParty.cpp` | 101 | yes | no | 0 | 6 |
| `_MSG_InviteGuild.cpp` | 99 | no | no | 0 | 9 |
| `_MSG_AccountLogin.cpp` | 97 | no (`USER_ACCEPT`) | no | 0 | 4 |
| `_MSG_SplitItem.cpp` | 96 | yes | no | 0 | 9 |
| `_MSG_ReqRanking.cpp` | 78 | yes | no | 0 | 7 |
| `_MSG_ReqTeleport.cpp` | 65 | no | no | 0 | 1 |
| `_MSG_PKMode.cpp` | 65 | no | no | 0 | 0 |
| `_MSG_Deposit.cpp` | 57 | yes | yes | 1 | 1 |
| `_MSG_REQShopList.cpp` | 56 | yes | yes | 1 | 4 |
| `_MSG_Withdraw.cpp` | 56 | yes | yes | 1 | 1 |
| `_MSG_QuitTrade.cpp` | 55 | yes | yes | 1 | 1 |
| `_MSG_NoViewMob.cpp` | 51 | no | no | 0 | 2 |
| `_MSG_CreateCharacter.cpp` | 51 | no (`USER_SELCHAR`) | no | 0 | 1 |
| `_MSG_Restart.cpp` | 50 | yes (`==`) | yes | 0 | 0 |
| `_MSG_DeleteItem.cpp` | 50 | yes | no | 0 | 4 |
| `_MSG_GuildAlly.cpp` | 48 | no | no | 0 | 3 |
| `_MSG_RemoveParty.cpp` | 48 | no | no | 0 | 0 |
| `_MSG_War.cpp` | 48 | no | no | 0 | 3 |
| `_MSG_DeleteCharacter.cpp` | 46 | no (`USER_SELCHAR`) | no | 0 | 0 |
| `_MSG_ReqTradeList.cpp` | 46 | yes | yes | 1 | 3 |
| `_MSG_Motion.cpp` | 38 | yes | yes | 1 | 1 |
| `_MSG_ChangeCity.cpp` | 37 | no | no | 0 | 0 |
| `_MSG_ChallangeConfirm.cpp` | 35 | no | no | 0 | 1 |
| `_MSG_CapsuleInfo.cpp` | 32 | no | no | 0 | 0 |
| `_MSG_AccountSecure.cpp` | 32 | no | no | 0 | 1 |
| `_MSG_CharacterLogout.cpp` | 28 | no | no | 0 | 0 |
| `_MSG_SetShortSkill.cpp` | 27 | no | no | 0 | 0 |
| `_MSG_Deprivate.cpp` | 26 | no | no | 0 | 0 |

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (declaration-level, directed)

```
ProcessClientMessage.h
├── ..\Basedef.h        message constants, message structs, STRUCT_MOB/ITEM/SCORE, limits, SKIPCHECKTICK
├── ..\CPSock.h         HEADER, CPSock (AddMessage / SendOneMessage / SendMessageA / ReadMessage)
├── ..\ItemEffect.h     EF_* item effect codes used by handlers via Basedef helpers
├── Language.h          _NN_ / _SN_ / _DN_ / _DD_ string-table indices
├── CItem.h             CItem (pItem[]) ground-item model
├── Server.h            server API: AddCrackError, CloseUser, CharLogOut, DoTeleport, DoRecall,
│                       RemoveTrade, RemoveParty, SaveUser, Log, CrackLog, ItemLog, globals
├── GetFunc.h           GetItemPointer users, GetAttribute, GetAction, GetCreateMob, PK/guilt helpers
├── SendFunc.h          30 Send* helpers (SendClientMessage, SendHpMode, SendItem, SendScore, ...)
├── CMob.h              CMob (pMob[]), MOB_* modes
├── CUser.h             CUser (pUser[]), USER_* modes, per-session state
├── CNPCGene.h          mNPCGen spawner
├── CReadFiles.h        config/file loaders
├── CCastleZakum.h      castle entry rules called from _MSG_Action
└── CWarTower.h         tower war rules
```

### 5.2 Dependency chains observed at runtime

```
MainWndProc
  -> Server.cpp recv loop
    -> CPSock::ReadMessage
    -> ProcessClientMessage                 [this component's entry point]
      -> Exec_MSG_* (58)
        -> SendFunc.*        (client replies)
        -> GetFunc.*         (LOS, attributes, PK, teleport cost)
        -> Server.*          (AddCrackError, RemoveTrade, DoorFor, DoTeleport, CharLogOut)
        -> Basedef.*         (GetItemPointer, BASE_GetItemAbility, BASE_GetVillage, BASE_GetRoute)
        -> CItem / CMob / CUser / CCastleZakum / CWarTower
        -> DBServerSocket.SendOneMessage   (login family, guild, war, capsule)
```

### 5.3 External dependencies

| Dependency | Version / identity | Purpose | Evidence |
|---|---|---|---|
| WinSock 2 / `Ws2_32` | Win32 SDK | TCP transport | `Code/CPSock.cpp`, `Code/CPSock.h` |
| MSVC toolset | `v143` | build | `Code/TMSrv/TMSrv.vcxproj` |
| Win32 GUI (`user32`, `gdi32`) | Win32 SDK | `MainWndProc` hosts the WSA messages and the server UI | `Code/TMSrv/Server.cpp:254`, `MainWndProc` |
| DBSrv (in-repo process) | port 7514 | persistence and authorisation of login/character/guild flows | `Code/Basedef.h:45`, `DBServerSocket` |
| W2 game client (external, not in repo) | protocol `APP_VERSION 6975` | protocol peer; version gate in `_MSG_AccountLogin` | `Code/Basedef.h:42`, `_MSG_AccountLogin.cpp:44` |

No third-party libraries are linked by TMSrv beyond the platform: the `.vcxproj` lists only project sources and headers.

---

## 6. Afferent and Efferent Coupling

C-style code has no classes here; the coupling unit is the **translation unit** (and, for handlers, the function). Afferent (Ca) counts TUs that reference the component's exported names; efferent (Ce) counts distinct internal components the component depends on.

| Component (unit) | Afferent (Ca) | Efferent (Ce) | Critical | Notes |
|---|---|---|---|---|
| `ProcessClientMessage.h` (contract) | 66 | 14 (+7 CRT/Win32) | High | 58 handler TUs + 8 non-handler TUs include it |
| `ProcessClientMessage` (dispatcher fn) | 1 (Server.cpp) | 58 handlers + 14 subsystem units | High | single funnel for all inbound traffic |
| `Exec_MSG_UseItem` | 1 | 14 | High | 5727 lines; 277 early returns; largest unit |
| `Exec_MSG_Quest` | 1 | 14 | High | 2713 lines; no mode/HP gate |
| `Exec_MSG_Attack` | 1 | 14 | High | 1790 lines; 12 crack call sites; 3 wire aliases |
| `Exec_MSG_MessageWhisper` | 1 | 14 | Medium | 1379 lines; 117 early returns |
| `Exec_MSG_Action` | 1 | 14 | High | owns movement commit and the `sizeof(m)` defect |
| `_MSG_CombineItem*` (11 units) | 1 each | 14 each | Medium | near-identical clones (coupling by copy) |
| `_MSG_*` small forwarders (15 units) | 1 each | 14 each | Low | 26-101 lines, thin adapters |
| `Server.cpp` (recv loop + injectors) | - | contract + 14 | High | the only dispatcher caller |

Observations on cohesion and coupling:

- **Uniform efferent coupling of 14 is an artefact**, not a design property: every handler file includes the same aggregate header, so all handlers appear to depend on all subsystems regardless of what they actually call. Real per-handler fan-out is much smaller (typically 2-5 helpers), but the compile-time graph cannot express it.
- **Afferent coupling of 66 on a header** means the contract is the strongest node in the subsystem's dependency graph: it is the only file whose edit can force a full-server rebuild.
- **Handlers are cohesive at the file level** (one message family per file, one exported function) but **incohesive internally** (a 5727-line function mixing parsing, validation, item effects, party notification, logging and broadcasting).
- **No handler-to-handler edges exist** (0 call sites), so the 58 units form a star around `ProcessClientMessage` and communicate only through `pUser`/`pMob` and helper functions — high implicit coupling through shared mutable globals, low explicit coupling.
- **The clone family** (`CombineItem`, `...Tiny`, `...Shany`, `...Lindy`, `...Ehre`, `...Ailyn`, `...Agatha`, `...Odin`, `...Alquimia`, `...Extracao`) shares an identical 20-line desync preamble copy-pasted eleven times; any fix to that preamble must be applied eleven times.

---

## 7. Endpoints (Protocol Entry Points)

The component exposes no HTTP/REST/GraphQL/gRPC surface. Its endpoints are **TCP binary message types** on port 8281. Table columns: message constant, numeric `Type` value (composite of base byte and direction flags), payload structure the handler casts to, and whether the handler enforces a session/HP gate.

| Endpoint (message) | Numeric Type | Payload struct | Handler | Session gate |
|---|---|---|---|---|
| `_MSG_AccountLogin` | 525 | `MSG_AccountLogin` | `Exec_MSG_AccountLogin` | `USER_ACCEPT` + version + size |
| `_MSG_AccountSecure` | 4062 | `MSG_AccountSecure` | `Exec_MSG_AccountSecure` | conn range only |
| `_MSG_CharacterLogin` | 531 | `MSG_CharacterLogin` | `Exec_MSG_CharacterLogin` | `USER_SELCHAR` (internal) |
| `_MSG_CharacterLogout` | 533 | none | `Exec_MSG_CharacterLogout` | none |
| `_MSG_CreateCharacter` | 527 | `MSG_CreateCharacter` | `Exec_MSG_CreateCharacter` | `USER_SELCHAR` |
| `_MSG_DeleteCharacter` | 529 | `MSG_DeleteCharacter` | `Exec_MSG_DeleteCharacter` | `USER_SELCHAR` |
| `_MSG_MessageChat` | 819 | `MSG_MessageChat` | `Exec_MSG_MessageChat` | `USER_PLAY` |
| `_MSG_MessageWhisper` | 820 | `MSG_MessageWhisper` | `Exec_MSG_MessageWhisper` | `USER_PLAY` |
| `_MSG_Action` | 876 | `MSG_Action` | `Exec_MSG_Action` | `USER_PLAY` + HP |
| `_MSG_Action2` | 870 | `MSG_Action` | `Exec_MSG_Action` | `USER_PLAY` + HP |
| `_MSG_Action3` | 872 | `MSG_Action` | `Exec_MSG_Action` | `USER_PLAY` + HP + class/skill/MP |
| `_MSG_Motion` | 874 | `MSG_Motion` | `Exec_MSG_Motion` | `USER_PLAY` + HP |
| `_MSG_NoViewMob` | 873 | `MSG_STANDARDPARM` | `Exec_MSG_NoViewMob` | MobID range only |
| `_MSG_Restart` | 649 | `MSG_STANDARD` | `Exec_MSG_Restart` | `USER_PLAY` (branch, not reject) |
| `_MSG_Deprivate` | 652 | `MSG_STANDARDPARM` | `Exec_MSG_Deprivate` | none |
| `_MSG_Challange` | 654 | `MSG_STANDARDPARM` | `Exec_MSG_Challange` | none |
| `_MSG_ChallangeConfirm` | 655 | `MSG_STANDARDPARM2` | `Exec_MSG_ChallangeConfirm` | none |
| `_MSG_ReqTeleport` | 656 | none | `Exec_MSG_ReqTeleport` | none |
| `_MSG_ChangeCity` | 657 | none (`pMsg` unused) | `Exec_MSG_ChangeCity` | none |
| `_MSG_REQShopList` | 635 | `MSG_REQShopList` | `Exec_MSG_REQShopList` | `USER_PLAY` + HP |
| `_MSG_Deposit` | 904 | `MSG_STANDARDPARM` | `Exec_MSG_Deposit` | `USER_PLAY` + HP |
| `_MSG_Withdraw` | 903 | `MSG_STANDARDPARM` | `Exec_MSG_Withdraw` | `USER_PLAY` + HP |
| `_MSG_RemoveParty` | 894 | `MSG_STANDARDPARM` | `Exec_MSG_RemoveParty` | none |
| `_MSG_SendReqParty` | 895 | `MSG_SendReqParty` | `Exec_MSG_SendReqParty` | `USER_PLAY` |
| `_MSG_AcceptParty` | 939 | `MSG_AcceptParty` | `Exec_MSG_AcceptParty` | target `USER_PLAY` |
| `_MSG_TradingItem` | 886 | `MSG_TradingItem` | `Exec_MSG_TradingItem` | `USER_PLAY` + HP |
| `_MSG_PKMode` | 921 | `MSG_STANDARDPARM` | `Exec_MSG_PKMode` | none |
| `_MSG_ReqTradeList` | 922 | `MSG_STANDARDPARM` | `Exec_MSG_ReqTradeList` | `USER_PLAY` + HP |
| `_MSG_UpdateItem` | 884 | `MSG_UpdateItem` | `Exec_MSG_UpdateItem` | `USER_PLAY` + HP |
| `_MSG_Quest` | 651 | `MSG_STANDARDPARM2` | `Exec_MSG_Quest` | NPC index range only |
| `_MSG_SetShortSkill` | 888 | `MSG_SetShortSkill` | `Exec_MSG_SetShortSkill` | none |
| `_MSG_Attack` | 871 | `MSG_Attack` | `Exec_MSG_Attack` | `USER_PLAY` + HP |
| `_MSG_AttackOne` | 925 | `MSG_Attack` | `Exec_MSG_Attack` | `USER_PLAY` + HP |
| `_MSG_AttackTwo` | 926 | `MSG_Attack` | `Exec_MSG_Attack` | `USER_PLAY` + HP |
| `_MSG_DropItem` | 626 | `MSG_DropItem` | `Exec_MSG_DropItem` | `USER_PLAY` + HP |
| `_MSG_GetItem` | 624 | `MSG_GetItem` | `Exec_MSG_GetItem` | `USER_PLAY` + HP |
| `_MSG_QuitTrade` | 900 | none (outbound only) | `Exec_MSG_QuitTrade` | `USER_PLAY` + HP |
| `_MSG_UseItem` | 883 | `MSG_UseItem` | `Exec_MSG_UseItem` | `USER_PLAY` + HP |
| `_MSG_ApplyBonus` | 631 | `MSG_ApplyBonus` | `Exec_MSG_ApplyBonus` | `USER_PLAY` + HP |
| `_MSG_SendAutoTrade` | 919 | `MSG_SendAutoTrade` | `Exec_MSG_SendAutoTrade` | `USER_PLAY` + HP |
| `_MSG_ReqBuy` | 920 | `MSG_ReqBuy` | `Exec_MSG_ReqBuy` | `USER_PLAY` + HP |
| `_MSG_Buy` | 889 | `MSG_Buy` | `Exec_MSG_Buy` | `USER_PLAY` + HP |
| `_MSG_Sell` | 890 | `MSG_Sell` | `Exec_MSG_Sell` | `USER_PLAY` + HP |
| `_MSG_Trade` | 899 | `MSG_Trade` | `Exec_MSG_Trade` | `USER_PLAY` + HP |
| `_MSG_CombineItem` | 934 | `MSG_CombineItem` | `Exec_MSG_CombineItem` | none |
| `_MSG_CombineItemTiny` | 960 | `MSG_CombineItem` | `Exec_MSG_CombineItemTiny` | none |
| `_MSG_CombineItemShany` | 708 | `MSG_CombineItem` | `Exec_MSG_CombineItemShany` | none |
| `_MSG_CombineItemLindy` | 707 | `MSG_CombineItem` | `Exec_MSG_CombineItemLindy` | none |
| `_MSG_CombineItemEhre` | 723 | `MSG_CombineItem` | `Exec_MSG_CombineItemEhre` | none |
| `_MSG_CombineItemAilyn` | 949 | `MSG_CombineItem` | `Exec_MSG_CombineItemAilyn` | none |
| `_MSG_CombineItemAgatha` | 954 | `MSG_CombineItem` | `Exec_MSG_CombineItemAgatha` | none |
| `_MSG_CombineItemOdin` | 722 | `MSG_CombineItem` | `Exec_MSG_CombineItemOdin` | none |
| `_MSG_CombineItemOdin2` | 738 | `MSG_CombineItem` | `Exec_MSG_CombineItemOdin` | none |
| `_MSG_CombineItemAlquimia` | 737 | `MSG_CombineItem` | `Exec_MSG_CombineItemAlquimia` | none |
| `_MSG_CombineItemExtracao` | 724 | `MSG_STANDARDPARM2` | `Exec_MSG_CombineItemExtracao` | none |
| `_MSG_ReqRanking` | 927 | `MSG_STANDARDPARM2` | `Exec_MSG_ReqRanking` | `USER_PLAY` |
| `_MSG_DeleteItem` | 740 | `MSG_DeleteItem` | `Exec_MSG_DeleteItem` | `USER_PLAY` |
| `_MSG_SplitItem` | 741 | `MSG_SplitItem` | `Exec_MSG_SplitItem` | `USER_PLAY` |
| `_MSG_InviteGuild` | 981 | `MSG_STANDARDPARM2` | `Exec_MSG_InviteGuild` | none |
| `_MSG_GuildAlly` | 3602 | `MSG_GuildAlly` | `Exec_MSG_GuildAlly` | guild + leader (rank 9) |
| `_MSG_War` | 3598 | `MSG_STANDARDPARM2` | `Exec_MSG_War` | guild + leader (rank 9) |
| `_MSG_CapsuleInfo` | 717 | `MSG_STANDARDPARM` | `Exec_MSG_CapsuleInfo` | none |
| `_MSG_PutoutSeal` | 972 | `MSG_PutoutSeal` | `Exec_MSG_PutoutSeal` | `USER_PLAY` + HP |
| `_MSG_Ping` | 928 | - | consumed by dispatcher | n/a |
| `_MSG_UpdateScore` | 822 | - | honeypot (`AddCrackError`) | n/a |

Counts: 64 dispatched labels = 58 handler invocations + 5 alias labels + 1 honeypot; 1 consumed type (`_MSG_Ping`).

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| W2 game client | External peer | All inbound gameplay commands; the contract exists for this peer | TCP :8281, 12-byte HEADER + obfuscated body, `INITCODE` handshake | Little-endian C structs, fixed `sizeof` per message | Drop-on-error: ID gate, size gate, checksum (`ErrorCode=1` discards), silent drop for unknown types; per-type crack counters |
| DBSrv (`DBServerSocket`) | Internal process | Account login, character login, character create/delete, capsule info, guild ally, war declaration | TCP :7514, same CPSock framing | Same `MSG_*` structs with DB-side Type constants | `SendOneMessage` return ignored; mode advanced before reply; no timeout; failure surfaces only as a missing DB reply |
| DBSrv → TMSrv (`ProcessDBMessage`) | Internal process | Completes the state machine by writing `USER_SELCHAR` / `USER_PLAY` | TCP :7514 | Same framing | Separate dispatcher (`ProcessDBMessage.cpp:61-1314`); handlers depend on its side effects |
| Server-internal callers | In-process | AI/skill/party simulation re-uses handlers | Direct C call | Stack-local `MSG_*` with `isServer=TRUE` | Contract: `ID < MAX_USER`, `ClientTick = SKIPCHECKTICK`; guards at `Server.cpp:4518, 5061, 5370, 7449` |
| Windows message pump | Platform | Delivery of `WSA_READ`; all handlers run on this thread | Win32 messages | `WPARAM`/`LPARAM` = socket, event | `WSAGETSELECTERROR` handled in `Server.cpp:3930-3968` |
| Global state (`pUser`, `pMob`, grids, guild tables) | In-process, implicit | The actual "API" handlers mutate | Direct memory | C structs in `.bss` | None: no locks, no versioning, no invariants enforced |
| String table (`g_pMessageStringTable`) | In-process data | Localised client text | Array indexed by `_NN_*` constants | `char*` | Missing index yields null/garbage into `sprintf`; no bounds check |
| Log files (`Log`, `CrackLog`, `ItemLog`, `ChatLog`) | File I/O | Audit and anti-cheat trail | Text lines via `fLogFile`/`fCrackLogFile`/`fItemLogFile` | `sprintf` into global `temp[4096]` | Write failures ignored; `temp` shared by all handlers |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Message Router / Command dispatch (Switch-based) | `switch (std->Type)` over 64 labels → 58 handlers | `Code/TMSrv/ProcessClientMessage.cpp:66-311` | Single funnel, zero allocation, O(1) average dispatch via compiler jump table |
| Type-tagged flat message space | 191 `_MSG_*` constants = base byte OR direction flags | `Code/Basedef.h:925-2200` | One integer selects both handler and direction; verified collision-free |
| Header-as-interface (aggregate header) | 14 subsystem includes + 12 externs in one file | `Code/TMSrv/ProcessClientMessage.h:29-68` | Gives every handler the whole server without per-file include lists |
| Zero-copy in-place mutation | Handlers cast `char*` to `MSG_*` and write back | all `_MSG_*.cpp`; forwards at `_MSG_AccountLogin.cpp:73-92` | Avoids copies on a hot path; enables mutate-and-forward to DB |
| Adapter / Proxy to DB | Login and guild handlers rewrite Type/ID and forward | `_MSG_AccountLogin`, `_MSG_CharacterLogin`, `_MSG_AccountSecure`, `_MSG_CapsuleInfo`, `_MSG_War`, `_MSG_GuildAlly` | Keeps persistence out of the game server while preserving the wire contract |
| Bouncer / Guard clause | Early `return` after `SendHpMode` / message / crack point | 27 handlers; 787 early returns across the family | Uniform rejection shape despite `void` signature |
| Sentry / Honeypot | `_MSG_UpdateScore` from client is penalised | `Code/TMSrv/ProcessClientMessage.cpp:106-110` | Detects direction confusion and probing |
| Trust sentinel | `SKIPCHECKTICK` = "internal, skip rate limits" | `Code/Basedef.h:172`; `ProcessClientMessage.cpp:63`; `_MSG_Action.cpp:92,115` | Lets AI-injected frames bypass anti-speed checks |
| Counter-based escalation (Circuit breaker, crude) | `NumError` accumulator with single threshold → forced logout | `Code/TMSrv/Server.cpp:684-707` | Backstop against automated exploit loops |
| Deferred write-behind | `AddMessage` queue + 1-second flush sweep | `Code/CPSock.cpp:513`; `ProcessSecMinTimer.cpp:201-215` | Batches replies; bounds syscalls to ~1 Hz per user |
| Shared-state session store | `CUser pUser[1000]` / `CMob pMob[25000]` globals | `Code/TMSrv/Server.cpp:269-270` | Implicit context for all handlers; no session objects passed |
| State machine (implicit) | `USER_EMPTY → USER_ACCEPT → USER_LOGIN → USER_SELCHAR → USER_CHARWAIT → USER_PLAY → USER_SAVING4QUIT` | `Code/TMSrv/CUser.h:26-37`; transitions in handlers + `ProcessDBMessage.cpp:906` | Gates handler legality; enforced per handler, not centrally |
| Copy-paste variant family | 11 `CombineItem*` handlers with identical preamble | `Code/TMSrv/_MSG_CombineItem*.cpp` | Variant-per-NPC crafting without a table-driven engine |
| Two-phase login | Handler sends to DB and parks the session in a wait mode; DB reply completes it | `_MSG_AccountLogin.cpp:92-95` → `ProcessDBMessage.cpp:354-400` | Keeps the accept thread non-blocking |

Architectural boundary assessment:

- The component **is** the boundary. Above it: transport and framing (`CPSock`). Below it: domain logic. The boundary is enforced only by convention — handlers may call any subsystem and write any global, and 27 of them do so without even checking session mode.
- The **direction of dependency is inverted** relative to a clean architecture: the dispatcher depends on 58 concrete handlers (compile-time) rather than on a table of interfaces, and every handler depends on the entire server (transitive aggregate header).
- There is **no separation between validation, authorisation and execution** at the boundary; each handler owns all three for its message, in its own order, which is why the ordering is inconsistent across the family (some check HP before trade, some after; some check mode twice, some not at all).

---

## 10. Technical Debt & Risks

| ID | Risk Level | Component Area | Issue | Impact |
|---|---|---|---|---|
| TD-01 | High | Dispatcher / all handlers | Packet `Size` is never compared with the structure a handler casts to (`CPSock.cpp:397` only checks 12..8192; only `_MSG_AccountLogin.cpp:41` validates). Handlers read `sizeof(MSG_*)` bytes from a frame that may declare 12 | Out-of-frame reads of stale buffer contents; crafted short frames drive handlers with attacker-influenced stale bytes as arguments |
| TD-02 | High | `_MSG_Action.cpp:272`, `:278` | `SendOneMessage((char*)m, sizeof(m))` / `AddMessage((char*)m, sizeof(m))` pass the size of a **pointer** (4 bytes) instead of `sizeof(MSG_Action)` (48) | Truncated movement frames reach peers and the sender; clients see desync/ghost positions; the frame is below the protocol minimum |
| TD-03 | High | Contract BR-12 (26 handlers) | No session gate at all in `SetShortSkill`, `PKMode`, `ChangeCity`, `Deprivate`, `ReqTeleport`, `Quest`, the ten `CombineItem*` handlers, `CharacterLogout`, `RemoveParty`, `War`, `GuildAlly`, `CapsuleInfo`, `AccountSecure`, `NoViewMob`, `InviteGuild`, `Challange`, `ChallangeConfirm` (31 handlers in total never test `USER_PLAY`) | Pre-login/post-logout connections can mutate persistent character state (`MOB.SkillBar`, `CharShortSkill`, `PKMode`, merchant byte) |
| TD-04 | High | `_MSG_Quest.cpp` (2713 lines) | Validates only the NPC index; no mode, HP, trade or distance gate before a very large effect surface | Unauthenticated/quest-state mutation reachable with a 16-byte frame |
| TD-05 | High | Anti-cheat return handling | `AddCrackError` returns `TRUE` after `CharLogOut`, but only 3 of 42 call sites test the value | Continued mutation after logout; in-memory character resurrection / dupe hazard |
| TD-06 | High | Contract BR-19 | Aggregate header couples 66 TUs to 14 headers; duplicates 12 externs already in `Server.h`; `GuildInfo[65536]` vs `MAX_GUILD 4096` domain mismatch | Whole-server recompilation on any header edit; silent type drift between the two declaration sites (link-time or OOB at worst) |
| TD-07 | Medium | `_MSG_Action.cpp:160-196` | Viewport/position validation uses several magic literals (`3378/3398/1427/1448`, `4096`) and mixes `Coord` domains with `/128` region math | Rule drift; a new map region silently bypasses or over-restricts movement |
| TD-08 | Medium | `_MSG_TradingItem.cpp:100,134` | Bounds checks use `DestSlot > MAX_CARGO` / `SrcSlot > MAX_CARGO` where `>=` is required, and `DestSlot`/`SrcSlot` are `unsigned char` so negative checks are impossible | Off-by-one slot (index 128) accepted by the explicit check; safety depends entirely on the later `GetItemPointer` call |
| TD-09 | Medium | `_MSG_ChangeCity.cpp`, `_MSG_Deprivate.cpp` | `pMsg` accepted but ignored; parameters unvalidated; behaviour derived from current position only | Dead contract surface; a client can toggle city/merchant byte at will |
| TD-10 | Medium | `_MSG_CapsuleInfo.cpp:30-34` | Client-supplied `Parm` forwarded verbatim as the DB capsule index with no ownership or range validation | Cross-account capsule read is only prevented by the DB server |
| TD-11 | Medium | Error reporting | 787 early returns across 58 handlers, no error codes, no metrics; failures invisible except as ad-hoc strings | Undiagnosable client/server mismatches; no way to measure rejection rates per message |
| TD-12 | Medium | Text handling | Literal messages bypass `g_pMessageStringTable` (`"Wait a moment."`, `"Login now, wait a moment."`, `"Deleting Character. wait a moment."`); `sprintf(temp, g_pMessageStringTable[...], ...)` uses table content as a format string (`_MSG_AccountLogin.cpp:32,47`, `_MSG_CharacterLogin.cpp:38,61`) | Untranslatable text; format-string exposure if a language pack ever contains `%s`/`%n` |
| TD-13 | Medium | Global `temp[4096]` | One shared formatting buffer for dispatcher and all handlers, no locking, no ownership rule | A handler that stores `temp` across a helper call (which may itself format) reads corrupted text; latent under any thread introduction |
| TD-14 | Medium | Dispatcher BR-09 | No `default:` case: 127+ defined types and all unknown values are dropped with no log and no counter | Protocol drift is undetectable server-side; probing is silent except for the one honeypot |
| TD-15 | Medium | `ProcessClientMessage.h:19-43` | `#pragma once` only, no versioned guard; 7 raw CRT/Win32 includes in a contract header; `LONG`-era types (`BOOL`) in the ABI | Non-portable, untestable outside Win32; cannot be included by a host-side test harness |
| TD-16 | Low | Duplicate `GetItemPointer` calls | `_MSG_TradingItem.cpp:76-80` resolves the same two pointers twice into 4 variables (`DestItem`/`dItem`, `SrcItem`/`sItem`) | Wasted work; two names for one address invites divergence during maintenance |
| TD-17 | Low | `Server.cpp:4593` | `memset(&sm, 0, sizeof(MSG_AttackOne))` on a `MSG_Attack` local (`MSG_Attack` is the larger struct) | Silent partial initialisation of the injected frame |
| TD-18 | Low | Dispatcher logging | `sprintf(temp, "err,packet Type:%d ID:%d Size:%d KeyWord:%d", ...)` inside the ID gate; `Log` called before any mode check | Unauthenticated peers can drive log volume at line rate |
| TD-19 | Low | Magic numbers | `MAX_GUILDZONE` indices `g_pGuildZone[4]`, castle coordinates, `clan == 7 / 8`, `BrGrid` thresholds hardcoded in handlers | Region/event logic cannot be configured; duplicated literals across handlers |
| TD-20 | Low | `_MSG_UseItem.cpp` (5727 lines) | One function implements potions, scrolls, mounts, seals, revival, stat items, teleports; 277 early returns | Untestable, unreviewable; every item change risks all item behaviour |

No recommendation, estimate or remediation sequencing is provided, per the analysis-only scope.

---

## 11. Test Coverage Analysis

### 11.1 Existence of tests: none

**Conclusion: the project contains zero automated tests for this component (and none anywhere in the repository).** Evidence, verified against the project root `E:\wyd\test-setup\w2pp` (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`):

| Check | Result | Evidence |
|---|---|---|
| Test-like file names anywhere in repo | none | recursive search for `*test*`, `*Test*`, `*TEST*`, `*spec*`, `*Spec*` returned no files |
| Test project in the solution | none | `W2PP Code Project.sln` declares exactly 2 projects: `DBSrv` (`Code\DBSrv\DBSrv.vcxproj`) and `TMSrv` (`Code\TMSrv\TMSrv.vcxproj`), plus a "Solution Items" folder |
| Build system with test targets | none | only the two `.vcxproj` + `.filters`; no `CMakeLists.txt`, no `*.cmake`, no `makefile` |
| Test framework vendored or referenced | none | no `gtest*`, `catch*`, `doctest`, `boost/test` matches anywhere |
| Test fixtures / mocks / golden packets | none | no directories named `test`, `tests`, `spec`, `fixtures`, `mocks` |
| CI pipeline | none | no `.github/`, `.gitlab-ci.yml`, `azure-pipelines.yml`, `appveyor.yml` at root |
| Assertions in production code | none in this component | no `assert`/`ASSERT`/`_ASSERTE` in `ProcessClientMessage.h/.cpp` or the 58 handlers |
| Primary source | decompiled code | repository README/licence describe a decompiled emulator; original test assets would not survive decompilation |

### 11.2 Coverage table

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `ProcessClientMessage.h` (contract) | 0 | 0 | 0% | No compile-time ABI check; no consumer stub; no signature conformance test |
| `ProcessClientMessage` (dispatcher) | 0 | 0 | 0% | 6 branches (ID gate, ServerDown, conn range, ping, tick spoof, honeypot) + 64 labels, none exercised |
| `Exec_MSG_AccountLogin` | 0 | 0 | 0% | Version gate, size gate, mode gate, 3-fail lockout, MAC capture: all untested |
| `Exec_MSG_CharacterLogin` | 0 | 0 | 0% | Slot bounds, billing matrix, mode transition: untested |
| `Exec_MSG_Action` | 0 | 0 | 0% | Tick/rate/viewport/bounds/route logic plus the `sizeof(m)` defect: untested |
| `Exec_MSG_UseItem` | 0 | 0 | 0% | Largest unit (5727 lines, 277 returns): untested |
| `Exec_MSG_Attack` | 0 | 0 | 0% | 3 wire aliases, 12 crack points: untested |
| `Exec_MSG_Quest` | 0 | 0 | 0% | 2713-line NPC script surface: untested |
| `CombineItem*` family (11 units) | 0 | 0 | 0% | Shared desync check duplicated 11x, never asserted |
| Remaining 41 handlers | 0 | 0 | 0% | Thin adapters and forwarders: untested |

### 11.3 What substitutes for testing

The component is verified only by **runtime instrumentation**, which is logging rather than testing:

- `Log(str1, str2, ip)` text lines from every handler (`Code/TMSrv/Server.cpp:124`), including the dispatcher's ID-gate line (`ProcessClientMessage.cpp:46-48`).
- `CrackLog` for suspicious sequences (e.g. `_MSG_AccountLogin.cpp:60`, `_MSG_UseItem.cpp:83`).
- `ItemLog` / `ChatLog` for audit trails.
- `AddCrackError` counters with a single threshold at `2,000,000,000` (`Server.cpp:684-707`).
- An opt-in compile-time packet trace: `#ifdef _PACKET_DEBUG` in the dispatcher caller (`Server.cpp:3980-3988`) which logs `Type`/`Size` and, when defined, also relaxes the client-version check in `_MSG_AccountLogin.cpp:41-54`.

These mechanisms can *report* a violation but cannot *prevent* one, cannot be asserted against in CI, and do not cover the 58 handlers' success paths at all. The contract's most important rules (BR-01, BR-02, BR-04, BR-08, BR-11, BR-13, BR-18) are invisible to any test that would not itself construct raw frames and inspect `pUser`/`pMob` — which is exactly what a harness would have to do, and what does not exist.

### 11.4 Testability assessment (analysis, not recommendation)

Structural properties that block testing today are stated as facts:

- The unit of test would be a free function whose behaviour depends on 14 headers, two 1000/25000-element global arrays, a live socket object and a Win32 message pump; there is no seam to substitute.
- Handlers reach the network directly (`pUser[conn].cSock`, `DBServerSocket`), so observing outputs requires intercepting a real `CPSock`.
- The contract has no interface in the language sense (no vtable, no function pointer table, no registration), so a mock dispatcher cannot be linked in place of the real one without editing the source.
- The dispatcher's `isServer` parameter is the only pre-existing test hook: `TRUE` bypasses exactly one check (BR-08) and nothing else.

---

## Appendix A - Verification notes

- Handler count: `ProcessClientMessage.h:74-131` contains 58 declarations; `Exec_MSG_` definitions in `Code/TMSrv/_MSG_*.cpp` = 58; `Exec_MSG_*(conn, pMsg)` call sites in `ProcessClientMessage.cpp` = 58. The architectural report's "61" is not reproducible.
- Dispatched labels: 64 `case _MSG_*` labels = 58 handler calls + 5 alias labels (`_MSG_Action2`, `_MSG_Action3`, `_MSG_AttackOne`, `_MSG_AttackTwo`, `_MSG_CombineItemOdin2`) + 1 honeypot (`_MSG_UpdateScore`).
- Include fan-in: 66 `.cpp` files include `ProcessClientMessage.h` (58 handlers + `CCastleZakum`, `CWarTower`, `imple`, `MobKilled`, `ProcessClientMessage`, `ProcessDBMessage`, `ProcessSecMinTimer`, `Server`).
- Message constants: 191 `const short _MSG_*` definitions parsed from `Basedef.h`; zero duplicate numeric values after flag expansion.
- Gate coverage: 27 of 58 handlers test `Mode != USER_PLAY` and reject; 1 (`_MSG_Restart`) branches on `Mode == USER_PLAY`; 3 require `USER_SELCHAR`; 1 requires `USER_ACCEPT`; the remaining 26 test no session state at all. Twenty-two handlers also test `CurrentScore.Hp`.
- All file paths in this report are relative to the project root; line numbers refer to the files as they exist at analysis time.
