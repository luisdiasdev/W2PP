# Component Deep Analysis Report

## Component: ProcessDBMessage

- **Analyzed on:** 2026-08-19 17:13:23
- **Project scope:** `legacy` (W2PP C/C++ codebase)
- **Component location:** `legacy/Code/TMSrv/ProcessDBMessage.cpp`, `legacy/Code/TMSrv/ProcessDBMessage.h`
- **Folders ignored:** `.git`, `.opencode`
- **Analyzer role:** Analysis and reporting only (no project files modified)

---

## 1. Executive Summary

`ProcessDBMessage` is the **DBSrv response dispatcher** for the TMSrv (Trade/Message Server) executable within the W2PP legacy codebase. It is a single monolithic C function, `void ProcessDBMessage(char *Msg)`, located at `legacy/Code/TMSrv/ProcessDBMessage.cpp:39`, that receives a raw inbound message buffer read from the DBSrv socket connection and dispatches it to the appropriate handler based on the message `Type` field and the `ID` (connection index) it targets.

Its role in the system is the **inbound half of the TMSrv <-> DBSrv protocol**. While the game's DBSrv performs persistent database reads/writes, the TMSrv must apply the results of those operations back onto its live, in-memory game state (user slots, mob slots, inventories, guilds, server configuration, notices). `ProcessDBMessage` is the choke point where every DB2GAME-framed packet is interpreted and applied.

The function is organized into two broad regions:

1. **Server-wide (broadcast/system) messages** — handled when `conn == 0`. These set server configuration (e.g. `_MSG_DBSetIndex`), trigger global events (e.g. `_MSG_War`, `_MSG_GuildAlly`), push notices to all clients (`_MSG_DBNotice`, `_MSG_NPNotice`, `_MSG_MagicTrumpet`), and apply anti-multiboxing enforcement (`_MSG_DBCheckPrimaryAccount`).
2. **Per-user messages** — handled when `conn > 0`, addressing a specific connection slot. These are the account/character lifecycle confirmations (`_MSG_DBCNFAccountLogin`, `_MSG_DBCNFCharacterLogin`, `_MSG_DBCNFNewCharacter`, `_MSG_DBCNFDeleteCharacter`), login failure paths, item-delivery results (`_MSG_DBSendItem`, `_MSG_DBSendDonate`), capsule results, logout confirmations, and miscellaneous client signals.

**Key findings:**

- **Monolithic dispatch function:** The entire component is one 1330-line `switch`-based dispatcher with 40+ message-type handlers. There is no data abstraction, table-driven dispatch, or handler decomposition.
- **Header/implementation prototype mismatch:** `ProcessDBMessage.h:42` declares `void ProcessDBMessage(int conn, char *pMsg);` (two parameters), while the implementation at `ProcessDBMessage.cpp:39` and the real caller contract at `Server.h:99` use `void ProcessDBMessage(char *Msg)` (one parameter). The two-parameter prototype in `ProcessDBMessage.h` is stale and never matches the definition; the file that defines the function (`ProcessDBMessage.cpp`) does not even include `ProcessDBMessage.h`.
- **Heavy reliance on global mutable state:** The function reads and mutates dozens of module-level globals (`pUser[]`, `pMob[]`, `pMac[]`, `pMobGrid[]`, `GuildInfo[]`, `ChargedGuildList[]`, `ServerGroup`, `TransperCharacter`, `evDelete`, etc.), making it tightly coupled to the `Server.cpp` global environment and effectively non-unit-testable in isolation.
- **No test coverage exists** anywhere in the legacy tree for this component or the TMSrv/DBSrv servers in general.
- **Multiple correctness/safety concerns:** unchecked bounds access on `conn` in several handlers, use of `memcmp`/`strncmp` for equality checks, a `break` vs `return` ambiguity flagged by TODO comments in the source, and the largest handler (`_MSG_DBCNFCharacterLogin`, the character-enter-world path) is an extremely long, imperative block with several mid-function `break` statements and potential `pUser[0]`/`pMob[0]` access.

---

## 2. Data Flow Analysis

The component is not an entry point in the network sense; it is a downstream consumer invoked from the TMSrv main message loop. Its data flow is:

```
1. WSAAsyncSelect reports WSA_READ on the DBSrv socket
   -> Server.cpp MainWndProc message loop (case WSA_READDB)
2. DBServerSocket.Receive() buffers raw bytes from the DBSrv TCP connection
   -> Server.cpp:3850
3. DBServerSocket.ReadMessage(&Error, &ErrorCode) extracts one framed message
   -> Server.cpp:3889
4. Error/EOF validation (Error == 1 || Error == 2) -> log and bail
   -> Server.cpp:3907
5. Dispatch: ProcessDBMessage(Msg)  (the analyzed component)
   -> Server.cpp:3914
6. Inside ProcessDBMessage:
   a. Cast Msg to MSG_STANDARD* and validate (Type & FLAG_DB2GAME, 0 <= ID < MAX_USER)
      -> ProcessDBMessage.cpp:41-52
   b. Extract conn = std->ID
   c. If conn == 0 -> server-wide/system switch (config, notices, war, guilds)
   d. If conn in [1, MAX_USER) -> per-user switch (lifecycle, items, capsule, logout)
7. Each case mutates game state and/or sends responses:
   - to the client socket  pUser[conn].cSock.SendOneMessage / AddMessage / SendMessageA
   - back to DBSrv        DBServerSocket.SendOneMessage
   - to the broadcast grid GridMulticast / SyncMulticast
   - to the log           Log() / CrackLog()
8. Control returns to the DBSrv read loop for the next message
```

The primary data objects flowing through the component:

- **Inbound:** a raw `char* Msg` buffer interpreted as `MSG_STANDARD` (header: `Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`) and then re-cast to the concrete message struct (e.g. `MSG_DBCNFAccountLogin`, `MSG_CNFCharacterLogin`, `MSG_DBSendItem`, `MSG_DBSavingQuit`).
- **Outbound to client:** transformed/confirmed packets (e.g. `MSG_CNFClientCharacterLogin`, `MSG_CNFNewCharacter`, `MSG_MessageBoxOk`, notice strings).
- **Outbound to DBSrv:** result acknowledgements (e.g. `MSG_DBSendItem` with `Result = 0` or `3`, `MSG_DBSendDonate` with `Result = 0`, `MSG_DBNoNeedSave`).
- **State mutated:** `pUser[conn].Mode`, `pUser[conn].Cargo[]`, `pMob[conn].MOB`, `pMob[conn].extra`, `pMobGrid[][]`, `GuildInfo[]`, `ChargedGuildList[]`, `TransperCharacter`, `ServerGroup`/`ServerIndex`/`Sapphire`.

---

## 3. Business Rules & Logic

The component encodes the complete set of behaviors that TMSrv must exhibit when DBSrv completes (or fails) a database operation. The rules below were extracted from the switch cases; because the original game's business rules are implicit in the code (no spec document exists in the tree), several rules are documented with confidence indicators based on the code behavior.

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Reject any packet that is not DB2GAME-framed or has out-of-range ID | ProcessDBMessage.cpp:43-52 |
| Business Logic | `_MSG_DBSetIndex`: configure server group/index/sapphire from DBSrv | ProcessDBMessage.cpp:69-81 |
| Business Logic | `_MSG_War` / `_MSG_GuildAlly`: trigger guild war/alliance changes | ProcessDBMessage.cpp:83-97 |
| Business Logic | `_MSG_GuildInfo` / `_MSG_GuildReport`: cache guild info and charged guild list | ProcessDBMessage.cpp:99-112 |
| Business Logic | `_MSG_NPNotice` / `_MSG_DBNotice`: broadcast a notice to clients | ProcessDBMessage.cpp:114-121, 141-146, 341-351 |
| Business Logic | `_MSG_MessageDBImple`: execute an in-game command string from DB | ProcessDBMessage.cpp:123-134 |
| Business Logic | `_MSG_MagicTrumpet`: relay a broadcast announcement to all clients | ProcessDBMessage.cpp:136-139 |
| Security | `_MSG_DBCheckPrimaryAccount`: enforce one-account-per-MAC (multibox) | ProcessDBMessage.cpp:148-182 |
| Business Logic | `_MSG_TransperCharacter` / `_MSG_ReqTransper`: character transfer mode | ProcessDBMessage.cpp:61-67, 213-226 |
| Business Logic | `_MSG_DBSendItem`: deposit a web-delivered item into user cargo | ProcessDBMessage.cpp:229-338 |
| Security | `_MSG_DBCNFAccountLogin`: confirm account login; enforce MAC block and account match | ProcessDBMessage.cpp:354-472 |
| Error Handling | Login failure paths (account/pass/block/disable/only-once-per-day) | ProcessDBMessage.cpp:475-576 |
| Business Logic | `_MSG_DBCNFCharacterLogin`: fully materialize the player into the world | ProcessDBMessage.cpp:635-1081 |
| Business Logic | `_MSG_DBCNFNewCharacter` / `_MSG_DBCNFDeleteCharacter`: confirm creation/deletion | ProcessDBMessage.cpp:579-607 |
| Business Logic | `_MSG_DBSavingQuit`: force logout / account takeover (saving quit) | ProcessDBMessage.cpp:1137-1169 |
| Business Logic | `_MSG_DBCNFAccountLogOut`: finalize character logout | ProcessDBMessage.cpp:1172-1183 |
| Business Logic | `_MSG_DBSendDonate`: credit donated cash (donate points) | ProcessDBMessage.cpp:1217-1251 |
| Business Logic | Capsule character transfer results (success / fail / fail2) | ProcessDBMessage.cpp:1254-1282 |
| Business Logic | `_MSG_GrindRankingData`: forward EXP ranking update to the player | ProcessDBMessage.cpp:1285-1296 |
| Business Logic | `_MSG_DBClientMessage`: relay a client-facing message string | ProcessDBMessage.cpp:1299-1304 |
| Business Logic | `_MSG_DBServerSend1` / `_MSG_DBCNFServerChange`: server switch confirmations | ProcessDBMessage.cpp:1307-1324 |

### Detailed breakdown of the business rules

---

### Business Rule: DB2GAME packet validation gate

**Overview:** Every inbound packet must be framed with the `FLAG_DB2GAME` bit (0x0400) set in its `Type`, and its `ID` (the target connection index) must be within the valid range `[0, MAX_USER)`. Messages that fail this gate are logged and discarded without any state mutation.

**Detailed description:** The first action of the dispatcher is a defensive sanity check on the raw buffer. It interprets the leading bytes as a `MSG_STANDARD` header and verifies the packet is a legitimate DBSrv-to-game message by testing `std->Type & FLAG_DB2GAME`. It also bounds-checks the `ID` field, which is later used as the `conn` index into the `pUser[]` and `pMob[]` arrays. Because the component is invoked from a non-blocking socket read loop (`Server.cpp:3887-3915`), the buffer is trusted to at least have a valid `MSG_STANDARD` prefix; this gate is the only validation performed before the huge switch that follows. A failed packet produces a diagnostic line via `Log()` (`ProcessDBMessage.cpp:47-49`) and an immediate `return`. This rule is the critical safety boundary: if it were weakened, out-of-range `ID` values could reach the per-user handlers and index out of bounds. Note the check uses `std->ID < 0 || std->ID >= MAX_USER`, which rejects any ID equal to or above `MAX_USER` (1000) and any negative ID; `ID == 0` is allowed and routed to the server-wide region.

**Rule workflow:**
1. Cast inbound buffer to `MSG_STANDARD*` (`std`).
2. If `!(std->Type & FLAG_DB2GAME)` OR `std->ID < 0` OR `std->ID >= MAX_USER`:
   - Format an error string with Type/ID/Size/KeyWord.
   - Write to log under `"-system"` tag.
   - `return` (no processing).
3. Otherwise extract `conn = std->ID` and proceed to the dispatch switch.

---

### Business Rule: Server configuration via `_MSG_DBSetIndex`

**Overview:** When `conn == 0`, DBSrv may send `_MSG_DBSetIndex` (`MSG_STANDARDPARM3`) to tell the TMSrv which logical server group/index it belongs to, and the Sapphire (server-linking) token.

**Detailed description:** This handler runs at boot/connection time. `MSG_STANDARDPARM3` carries three parameters. `Parm1` is the server group id: if it is not `-1`, the TMSrv adopts `ServerGroup = Parm1` and `ServerIndex = Parm3`. `Parm2` is always assigned to `Sapphire`. The conditional `if (m->Parm1 != -1)` implies that a value of `-1` means "do not change the group/index", while Sapphire is updated unconditionally. This rule affects identity-sensitive features later in the code, such as which guild zone a player spawns into (`ServerGroup` used at `ProcessDBMessage.cpp:917`) and Kefra/guild naming. A source TODO comment at line 81 flags uncertainty about a `break` vs `return` semantics.

**Rule workflow:**
1. Cast to `MSG_STANDARDPARM3`.
2. If `Parm1 != -1`: set `ServerGroup = Parm1`, `ServerIndex = Parm3`.
3. Set `Sapphire = Parm2` unconditionally.
4. `break` out of the switch (fall through to function end).

---

### Business Rule: Global guild war and alliance triggers (`_MSG_War`, `_MSG_GuildAlly`)

**Overview:** DBSrv-initiated `_MSG_War` and `_MSG_GuildAlly` messages (both `MSG_STANDARDPARM2`) request the game to start a guild war or forge a guild alliance between two guilds.

**Detailed description:** These are broadcast/system messages (handled in the `conn == 0` region). `_MSG_War` calls `DoWar(Parm1, Parm2)` and `_MSG_GuildAlly` calls `DoAlly(Parm1, Parm2)`, delegating the actual guild relationship mutation to functions implemented in `Server.cpp`. The `Parm1`/`Parm2` values are the two guild identifiers involved. Because these handlers simply forward to shared server functions, the business semantics (war declaration rules, alliance prerequisites) live outside this component; this component is the transport adaptor that translates the DB message into the internal guild API.

**Rule workflow:**
1. Verify `conn == 0`.
2. `_MSG_War`: cast to `MSG_STANDARDPARM2`, call `DoWar(Parm1, Parm2)`.
3. `_MSG_GuildAlly`: cast to `MSG_STANDARDPARM2`, call `DoAlly(Parm1, Parm2)`.
4. `break`.

---

### Business Rule: Guild data caching (`_MSG_GuildInfo`, `_MSG_GuildReport`)

**Overview:** DBSrv synchronizes guild metadata and the "charged guild" (guild that controls a zone) list into the game server's memory so that the live server can answer guild queries without hitting the DB.

**Detailed description:** `_MSG_GuildInfo` carries a `MSG_GuildInfo` payload with a `Guild` index and a `STRUCT_GUILDINFO`. The handler copies `m->GuildInfo` into the global `GuildInfo[m->Guild]` slot, keeping an in-memory cache of guild details (including the `Citizen` field later used at `ProcessDBMessage.cpp:745` when a player of that guild logs in). `_MSG_GuildReport` carries a `ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]` matrix, which is bulk-copied into the global `ChargedGuildList` — this drives which guild "charges" each guild zone (a castle/war mechanic). Both are pure cache population operations with no client I/O. The correctness of this cache is assumed by downstream login logic, so a stale or incomplete cache would produce incorrect zone-owner behavior.

**Rule workflow:**
1. `_MSG_GuildInfo`: copy `m->GuildInfo` into `GuildInfo[m->Guild]`.
2. `_MSG_GuildReport`: `memcpy(ChargedGuildList, m->ChargedGuildList, sizeof(ChargedGuildList))`.
3. `break`.

---

### Business Rule: Notice broadcast (`_MSG_NPNotice`, `_MSG_DBNotice`, `_MSG_MagicTrumpet`)

**Overview:** Notices are pushed from DBSrv to the game for display either to all clients (server-wide) or to a single player.

**Detailed description:** There are three related notice flows. In the server-wide region, `_MSG_NPNotice` with `Parm1 == 1` broadcasts `SendNotice(m->String)` to every client, while `_MSG_DBNotice` always broadcasts `SendNotice(m->String)`. `_MSG_MagicTrumpet` relays a `MSG_MagicTrumpet` payload through `SyncMulticast(0, (MSG_STANDARD*)Msg, 0)` — a grid/broadcast fan-out that lets all nearby (or all) clients see the "magic trumpet" announcement. In the per-user region, `_MSG_NPNotice` with `Parm1 == 0` targets a single connected player: if that player's mode is `USER_PLAY`, `SendClientMessage(conn, m->String)` delivers the text directly to that client. The `Parm1` discriminator (1 = global notice, 0 = per-user) is the core branching rule.

**Rule workflow:**
1. Server-wide `_MSG_NPNotice`: if `Parm1 == 1` -> `SendNotice(String)`.
2. Server-wide `_MSG_DBNotice`: `SendNotice(String)`.
3. Server-wide `_MSG_MagicTrumpet`: `SyncMulticast(0, Msg, 0)`.
4. Per-user `_MSG_NPNotice`: if `Parm1 == 0` and `pUser[conn].Mode == USER_PLAY` -> `SendClientMessage(conn, String)`.

---

### Business Rule: In-game command execution from DB (`_MSG_MessageDBImple`)

**Overview:** DBSrv can inject an administrator command string into the game server's command interpreter (`ProcessImple`), effectively allowing DB-driven remote administration.

**Detailed description:** `MSG_MessageDBImple` carries a `Level` and a `String` of length `MESSAGE_LENGTH` (96). The handler force-terminates the string at index `MESSAGE_LENGTH - 1` and `MESSAGE_LENGTH - 2` (setting the last two bytes to zero, a defensive guard against an unterminated buffer), logs the raw string under the `"-system"` tag, then calls `ProcessImple(0, m->Level, m->String)`. `ProcessImple` (declared `Server.h:190`) interprets command strings (e.g. GM commands, server directives) — note the `imple.cpp` file also has command parsing. This is a privileged remote-execution channel from the database server into the game, so its correctness and authorization model are security-relevant. A source TODO comment at line 134 again flags the `break` vs `return` ambiguity.

**Rule workflow:**
1. Cast to `MSG_MessageDBImple`.
2. Null-terminate `String` at `[MESSAGE_LENGTH-1]` and `[MESSAGE_LENGTH-2]`.
3. `Log(String, "-system", 0)`.
4. `ProcessImple(0, m->Level, m->String)`.
5. `break`.

---

### Business Rule: Primary-account (anti-multiboxing) enforcement (`_MSG_DBCheckPrimaryAccount`)

**Overview:** When a user logs into a primary account via DBSrv, the game enforces that no other character on the same MAC address is playing; offending secondary characters are flagged `OnlyTrade = 1` and, if in a PK-unsafe village area, are recalled to a safe village.

**Detailed description:** This server-wide handler iterates over every non-empty user slot `i` in `[1, MAX_USER)`. For each slot with a live socket, it compares the slot's MAC (`pUser[i].Mac`) against the primary account's MAC (`m->Mac`) using `memcmp`. If the MACs do not match, the slot is skipped (different machine, unaffected). If they match, the account name is compared via `strncmp` against `m->AccountName`. A name match means this slot IS the primary account, so it is allowed to keep `OnlyTrade = 0` (full play privileges). A name mismatch means the slot is a secondary/multiboxed character on the same machine: it is set to `OnlyTrade = 1` (trade-only restriction), removed from any party (`RemoveParty(i)`), and if the character is actively playing (`pMob[i].Mode == USER_PLAY`) and located outside a village (`BASE_GetVillage` returns < 0 or >= 5) and not on a protected map attribute (`(mapAttribute & 0x80) == 0`), the game sends the "only in village" message and forces a recall (`DoRecall(i)`) back to a safe area. This is the game's defense against a single machine running multiple accounts for economic/PK advantage.

**Rule workflow:**
1. For `i` in `1..MAX_USER-1`:
   - Skip if `pUser[i].Mode == USER_EMPTY` or `pUser[i].cSock.Sock == 0`.
   - Skip if `memcmp(pUser[i].Mac, m->Mac, sizeof(m->Mac)) != 0`.
   - If `strncmp(pUser[i].AccountName, m->AccountName, ACCOUNTNAME_LENGTH) == 0`: set `pUser[i].OnlyTrade = 0`; `continue`.
   - Else (secondary account): set `pUser[i].OnlyTrade = 1`; `RemoveParty(i)`.
   - If `pMob[i].Mode == USER_PLAY` and not in a village and not on a protected attribute: send "only in village" message and `DoRecall(i)`.

---

### Business Rule: Character transfer mode (`_MSG_TransperCharacter`, `_MSG_ReqTransper`)

**Overview:** When the server is in "TransperCharacter" mode, requests to transfer a character to another server are honored by routing the request to the field scene.

**Detailed description:** `_MSG_TransperCharacter` (server-wide) simply sets the global flag `TransperCharacter = 1` and logs `"TransperCharacter mode"`. This flag gates the per-user `_MSG_ReqTransper` handler: if `TransperCharacter == 0`, the request is silently ignored (`return`). Otherwise, the handler rewrites the request `m->ID` to `ESCENE_FIELD + 1` (i.e. "sent to the field scene server"), sends it back to the client via `pUser[conn].cSock.SendOneMessage`, and moves the user's mode to `USER_SELCHAR`. This implements a controlled character-transfer handshake where the game acknowledges the transfer request and returns the user to the character-selection screen.

**Rule workflow:**
1. `_MSG_TransperCharacter`: `TransperCharacter = 1`; log.
2. `_MSG_ReqTransper` (per-user): if `TransperCharacter == 0` -> `return`.
3. Cast to `MSG_ReqTransper`; set `m->ID = ESCENE_FIELD + 1`.
4. `SendOneMessage` to client; set `pUser[conn].Mode = USER_SELCHAR`.

---

### Business Rule: Web-delivered item deposit (`_MSG_DBSendItem`)

**Overview:** Items purchased/granted through a web interface are delivered to the player's cargo (warehouse). The handler validates the target user, finds a free cargo slot, deposits the item, and acknowledges the result back to DBSrv.

**Detailed description:** This is one of the most intricate per-user handlers. It performs a two-phase slot search. Before any deposit, it enforces two preconditions: the user's mode must be at least `USER_SELCHAR` (else the attempt is logged under `_fail_play_` and dropped), and the account name embedded in the message must exactly match `pUser[conn].AccountName` (else logged under `_fail_name_` and dropped) — preventing item delivery to the wrong/unauthorized account.

Phase 1 searches `Cargo` from index `0` to `MAX_CARGO - 2` (i.e. `[0, 127)`), using `BASE_CanCargo(&m->Item, pUser[conn].Cargo, i % 9, i / 9)` to check whether the item fits at the given x/y layout position; if it fits, the item is placed at `Cargo[i]`, a `SendItem(conn, ITEM_PLACE_CARGO, i, ...)` is emitted to the client, the arrival message (`_NN_Item_Arrived` + item name) is sent, `m->Result = 0` (success) is written back to DBSrv via `DBServerSocket.SendOneMessage`, the transfer is logged under `_web1_`, and — importantly — if the user is still in `USER_SELCHAR` mode, `SaveUser(conn, 0)` persists the change immediately. The handler then `return`s (short-circuit).

Phase 2 is a fallback that scans `Cargo` from index `127` down to `0` looking for any zeroed `sIndex` slot (a simple empty-slot scan) and deposits there similarly (logged `_web2_`). If neither phase finds a slot, `m->Result = 3` (failure/full), the failure is acknowledged to DBSrv, `SaveUser(conn, 1)` is forced, and the event is logged under `_fail_empty_`. The slot limit of `MAX_CARGO - 2` in phase 1 reserves the last two cargo slots for other uses.

**Rule workflow:**
1. If `pUser[conn].Mode < USER_SELCHAR`: log `_fail_play_`; `break`.
2. If account mismatch: log `_fail_name_`; `break`.
3. Phase 1: for `i` in `0..MAX_CARGO-2`: if `BASE_CanCargo(...)` fits -> deposit at `Cargo[i]`, `SendItem`, notify client, `Result = 0`, ack to DBSrv, log `_web1_`, `SaveUser(conn,0)` if in `USER_SELCHAR`; `return`.
4. Phase 2: for `i` in `127..0`: if `Cargo[i].sIndex == 0` -> deposit, notify, `Result = 0`, ack, log `_web2_`, `SaveUser(conn,0)` if `USER_SELCHAR`; `return`.
5. If no slot: `Result = 3`, ack to DBSrv, `SaveUser(conn, 1)`, log `_fail_empty_`.

---

### Business Rule: Account login confirmation and MAC blocking (`_MSG_DBCNFAccountLogin`)

**Overview:** When DBSrv confirms an account login, the game validates account identity and MAC-block status, then populates the user's selection screen (cargo, coins, selected character) and applies item/effect sanitization.

**Detailed description:** The handler first verifies the account in the message matches the connection's account; a mismatch triggers a "try reconnect" message, flush of the send buffer (`SendMessageA`), and `CloseUser(conn)`. It then iterates the global MAC-block table `pMac[MAX_MAC]`; if the connection's MAC matches a blocked entry, the user is told `_NN_MAC_Block`, flushed, and closed. On success, all per-user session flags are reset (billing connect, whisper/guild/party/king chat, admin, chatting, and several `Unk_*` fields), and `OnlyTrade = 1` is set. The message is re-targeted to the client (`m->ID = ESCENE_FIELD + 2`, `m->Type = _MSG_CNFAccountLogin`), the user mode becomes `USER_SELCHAR`, and the inbound `Cargo[]` array is sanitized: for items whose `nPos` is 64 or 192 (weapon-type positions), any `EF_DAMAGEADD`/`EF_DAMAGE2` effect on slots 0-2 is normalized to `EF_DAMAGE`. If the event flag `evDelete != 0`, cargo items in the `[470, 500]` sIndex range are removed (event-item cleanup). The confirmation packet is sent to the client, and cargo/coins/sel are copied into `pUser`. If billing is enabled (`BILLING > 0`) and the selected character is free (`IsFree(&m->sel) != 0`), a billing request is sent (`SendBilling` with mode depending on `CHARSELBILL`). If `TransperCharacter` mode is active, an additional transfer packet is sent.

**Rule workflow:**
1. If account mismatch: message + `SendMessageA()` + `CloseUser(conn)`; `return`.
2. Scan `pMac[]`; if MAC blocked: message + `SendMessageA()` + `CloseUser(conn)`; `break`.
3. Reset session flags; set `OnlyTrade = 1`; retarget message to client; `Mode = USER_SELCHAR`.
4. Sanitize `Cargo[]` effects (weapon nPos 64/192: `EF_DAMAGEADD`/`EF_DAMAGE2` -> `EF_DAMAGE`).
5. If `evDelete`: remove cargo items with `sIndex` in `[470, 500]`.
6. Send confirmation to client; copy cargo/coin/sel into `pUser`.
7. If `BILLING > 0` and `IsFree(sel) != 0`: `SendBilling(...)` (mode 8 or 1 by `CHARSELBILL`).
8. If `TransperCharacter`: send `_MSG_TransperCharacter` packet to client.

---

### Business Rule: Character enter-world materialization (`_MSG_DBCNFCharacterLogin`)

**Overview:** The largest and most consequential handler: it converts a confirmed character-login message from DBSrv into a fully spawned, playable player in the game world, applying class-mastery item logic, inventory/equipment sanitization, spawn position selection, guild-zone/clan rules, EXP-segment classification, and broadcasting the new mob to the grid.

**Detailed description:** The handler copies the inbound `MSG_CNFCharacterLogin` into a `MSG_CNFClientCharacterLogin` (`sm`) and performs extensive normalization and setup. It sanitizes equipment (`Equip[0..MAX_EQUIP)`) and carry (`Carry[0..MAX_CARRY)`) weapon items' damage effects (nPos 64/192 `EF_DAMAGE2`/`EF_DAMAGEADD` -> `EF_DAMAGE`), and applies `evDelete` event-item removal to `Carry` items in `[470, 500]`. It then applies **class-mastery transformations to `Equip[0]`**: for `ClassMaster == MORTAL` or `ARCH`, effect slots 1-2 are rewritten (effect 98 value 0, effect 106 bound to the equipped weapon sIndex); for `CELESTIAL`/`SCELESTIAL`/`CELESTIALCS`, the value is fixed at 3. The user's `Donate` is set. If the stored HP is `<= 0`, it is clamped to 2 (prevents a dead-on-arrival player).

The full `STRUCT_MOB` is copied into `pMob[conn].MOB`, session timers are reset, `pMob[conn].ProcessorCounter = 1`, the `STRUCT_MOBEXTRA` is copied, and guild citizenship is inherited from the cached `GuildInfo[guild].Citizen`. Short skills and affects are copied. `MaxCarry` defaults to 30 but is raised by 15 for each of `Carry[60]`/`Carry[61]` holding item `3467` (a carry-capacity augment). The ARCH `MortalLevel` is derived from the selected mortal slot's level minus 299 (or defaulted to 99). HP/MP and bonus skill/score points are recomputed via `BASE_GetHpMp`, `BASE_GetBonusSkillPoint`, `BASE_GetBonusScorePoint`, and `GetCurrentScore`.

**Spawn selection** is intricate: the initial city spawn uses `g_pGuildZone[CityID].CitySpawnX/Y` plus a random `%15` offset (CityID derived from `Merchant` bits `0xC0`). If the mob's guild owns a guild zone (`MobGuild == g_pGuildZone[n].ChargeGuild`), the spawn is the guild-zone spawn. If the player is a low-level mortal (`Level < FREEEXP`, i.e. < 35) and `ClassMaster == MORTAL`, they are placed at a fixed newbie spawn (`2112, 2042` with small random jitter). The final coordinates must clear `GetEmptyMobGrid`, else the user is logged and closed. The mob is then registered into `pMobGrid[y][x] = conn`, broadcast to the grid via `GridMulticast` (CreateType 2), and the player is sent PK info, grid mobs, war info, castle state, etc.

**EXP-segment classification** splits the current level's EXP curve into 4 segments; based on where `Exp` falls, `pMob[conn].Segment` is set to 3/2/1/0. Level-999 characters are granted `Admin = 1`. Guild mantle (cloak, `Equip[15]`) is normalized when the character's clan does not match the guild's cached clan: for clan 7 (blue) the mantle is remapped to blue-range items (543/545/3191, or 3197 for celestial), and for clan 8 (red) to red-range items (544/546/3192/3195, or 3198 for celestial), based on which mantle range the player currently wears. Finally, notices (Kefra end, RvR bonus clan), `SendEtc`, `SendScore`, and a login stat log line are emitted.

**Rule workflow (condensed):**
1. Copy message to `sm`; if `conn == 0` -> `CrackLog` + `CloseUser` + `break`.
2. Sanitize `Equip[]` and `Carry[]` weapon effects; apply `evDelete` carry removal.
3. Apply class-mastery `Equip[0]` transformations (MORTAL/ARCH vs CELESTIAL variants).
4. Copy mob/extra; inherit guild citizenship; copy skills/affects; recompute HP/MP/score.
5. Determine `MaxCarry` (30 + 15 per augment item); compute ARCH MortalLevel.
6. Select spawn (city/guild-zone/newbie); require `GetEmptyMobGrid` success else close.
7. Register in `pMobGrid`; broadcast `GridMulticast` CreateMob (type 2).
8. Apply EXP-segment classification; grant admin at level >= 999.
9. Normalize guild mantle by clan; send war info / castle state / PK info / grid / score / etc.
10. Emit login stat log.

---

### Business Rule: Character creation/deletion confirmations (`_MSG_DBCNFNewCharacter`, `_MSG_DBCNFDeleteCharacter`)

**Overview:** After DBSrv successfully creates or deletes a character, the game forwards a confirmation to the client and returns the user to the character-selection screen.

**Detailed description:** `_MSG_DBCNFNewCharacter` retypes the message to `_MSG_CNFNewCharacter` (client-facing), sets `ID = ESCENE_FIELD + 1`, sends it to the client, and sets the user mode back to `USER_SELCHAR`. `_MSG_DBCNFDeleteCharacter` does the same with `_MSG_CNFDeleteCharacter`. In both cases the user remains in the character-selection flow, ready to pick or create another character. Failure counterparts (`_MSG_DBNewCharacterFail`, `_MSG_DBDeleteCharacterFail`) instead send a `SendClientSignal` with the failure signal (`_MSG_NewCharacterFail` / `_MSG_DeleteCharacterFail`) and reset mode to `USER_SELCHAR` without transitioning.

**Rule workflow:**
1. `_MSG_DBCNFNewCharacter`: set Type/ID, send to client, `Mode = USER_SELCHAR`.
2. `_MSG_DBCNFDeleteCharacter`: set Type/ID, send to client, `Mode = USER_SELCHAR`.
3. Failure variants: `SendClientSignal(conn, 0, <fail signal>)`, `Mode = USER_SELCHAR`.

---

### Business Rule: Saving-quit and account takeover (`_MSG_DBSavingQuit`)

**Overview:** When a player saves-and-quits (or is force-quit by a second login), the game acknowledges, notifies the affected client, and closes the connection.

**Detailed description:** `MSG_DBSavingQuit` carries the target `ID` and a `Mode`. First, the target `ID` is range-checked (`<= 0 || >= MAX_USER` -> log error and `break`). If the target user is neither `USER_PLAY` nor `USER_SAVING4QUIT`, the game sends `_MSG_DBNoNeedSave` back to DBSrv (indicating no save is necessary). If the target is `USER_PLAY` or `USER_SELCHAR`, a client message is sent: `Mode == 0` -> "your account is being used from another location", `Mode == 1` -> "account disabled"; the send buffer is flushed (`SendMessage`). Finally `CloseUser(m->ID)` tears down the target connection. This implements the "account logged in elsewhere" takeover and forced-disconnect flows.

**Rule workflow:**
1. Range-check `m->ID`; on failure log and `break`.
2. If target not `USER_PLAY`/`USER_SAVING4QUIT`: send `_MSG_DBNoNeedSave` to DBSrv.
3. If target `USER_PLAY` or `USER_SELCHAR`: send mode-specific message; flush send buffer.
4. `CloseUser(m->ID)`.

---

### Business Rule: Character logout finalization (`_MSG_DBCNFAccountLogOut`)

**Overview:** Confirms a character logout requested by DBSrv, clears the mob/user slots, and closes the connection.

**Detailed description:** The handler logs the logout event (connection index and mob name), sets `pMob[conn].Mode = MOB_EMPTY` and `pUser[conn].Mode = USER_ACCEPT` to free the slots, and calls `CloseUser(conn)` to release the socket and slot resources. This is the clean-teardown path for a DB-confirmed logout.

**Rule workflow:**
1. Log `"etc,charlogout conn:%d name:%s"`.
2. `pMob[conn].Mode = MOB_EMPTY`; `pUser[conn].Mode = USER_ACCEPT`.
3. `CloseUser(conn)`.

---

### Business Rule: Web donate credit (`_MSG_DBSendDonate`)

**Overview:** Donate/cash points purchased via the web are credited to the user's account balance in real time.

**Detailed description:** Mirroring the `_MSG_DBSendItem` preconditions, this handler rejects the credit if the user is not at least in `USER_SELCHAR` mode (logged `_fail_play_`) or if the account name does not match (logged `_fail_name_`). On success it adds `m->Donate` to `pUser[conn].Donate`, sends the client the `_NN_Cash_ChargeOk` message, sets `m->Result = 0`, acknowledges to DBSrv, and logs the donation under `_web1_`. This keeps the in-game donate balance synchronized with the DB/web purchase immediately, without a restart.

**Rule workflow:**
1. If mode < `USER_SELCHAR`: log `_fail_play_`; `break`.
2. If account mismatch: log `_fail_name_`; `break`.
3. `pUser[conn].Donate += m->Donate`; send charge-OK message.
4. `m->Result = 0`; ack to DBSrv; log donation.

---

### Business Rule: Capsule character transfer results (`_MSG_DBCNFCapsule*`, `_MSG_CNFDBCapsuleInfo`)

**Overview:** Handles the outcomes of capsule-based character/item storage operations, refreshing the client's carry view and reporting success or failure.

**Detailed description:** `_MSG_CNFDBCapsuleInfo` simply forwards the capsule info packet to the client. `_MSG_DBCNFCapsuleCharacterFail` refreshes the user's carry (`SendCarry(conn)`) and reports "no empty slot". `_MSG_DBCNFCapsuleCharacterFail2` does the same but with a "cannot use ID" message. `_MSG_DBCNFCapsuleSucess` clears the mob and extra structures and forces a `SaveUser(conn, 1)`, committing the capsule operation. These handlers keep the client's inventory view consistent with the DB result.

**Rule workflow:**
1. `_MSG_CNFDBCapsuleInfo`: forward to client.
2. Fail variants: `SendCarry(conn)` + failure message.
3. `_MSG_DBCNFCapsuleSucess`: clear mob/extra; `SaveUser(conn, 1)`.

---

### Business Rule: EXP ranking notification (`_MSG_GrindRankingData`)

**Overview:** When DBSrv detects a ranking change, the game forwards the updated EXP ranking packet to the affected player.

**Detailed description:** The handler forwards `MSG_SendExpRanking` to the client only when the user is actively playing (`USER_PLAY`) and the ranking record's name matches the player's mob name (and the name is non-empty). This guards against sending ranking data to a client still on the character-selection screen or to the wrong player (the comment notes the selection-screen mode lacks player data, so the name check is skipped there). This keeps the player's "grind ranking" display fresh.

**Rule workflow:**
1. If `pUser[conn].Mode == USER_PLAY` and `m->PlayerRank.Name[0] != '\0'` and name matches mob name -> `SendOneMessage` to client.

---

### Business Rule: Server switch confirmations (`_MSG_DBServerSend1`, `_MSG_DBCNFServerChange`)

**Overview:** Handles server-change handshakes where a player is told to move to another server.

**Detailed description:** `_MSG_DBServerSend1` retargets the message ID to `ESCENE_FIELD`, queues it via `AddMessage`, and flushes with `SendMessageA`. `_MSG_DBCNFServerChange` sends the full server-change packet to the client and then `CloseUser(conn)`, ending the session on this server so the client can connect to the target server. Together these implement the cross-server migration flow.

**Rule workflow:**
1. `_MSG_DBServerSend1`: `m->ID = ESCENE_FIELD`; `AddMessage`; `SendMessageA`.
2. `_MSG_DBCNFServerChange`: `SendOneMessage`; `CloseUser(conn)`.

---

### Business Rule: Account secure and login-failure signal routing

**Overview:** Various DB-side outcomes (account security prompts, wrong password, blocked/disabled account, already playing, etc.) are translated into client-facing signals or messages, often followed by disconnection.

**Detailed description:** `_MSG_AccountSecure` / `_MSG_AccountSecureFail` forward `SendClientSignal` to the client (ID `ESCENE_FIELD`). `_MSG_DBAccountLoginFail_Account`/`_MSG_DBAccountLoginFail_Disable`/`_MSG_DBAccountLoginFail_Pass` send a localized message, flush the send buffer, and `CloseUser`. `_MSG_DBAccountLoginFail_Block` maps a `Parm` code (0/1/2/3 -> "X"/"A"/"B"/"C") to a blocked-account message before closing. `_MSG_DBOnlyOncePerDay` sends the "only once per day" message and flushes (no close). `_MSG_DBAlreadyPlaying` / `_MSG_DBStillPlaying` signal the existing session and close. `_MSG_DBMessageBoxOk` forwards a message-box acknowledgment. `_MSG_DBClientMessage` relays a generic client message string. `_MSG_DBNewAccountFail` sends a signal and closes.

**Rule workflow:** Each failure/signal type: translate to a `SendClientMessage` / `SendClientSignal` / `SendClientSignalParm`, optionally flush, and `CloseUser(conn)` for terminal failures.

---

## 4. Component Structure

The component spans two files, but the entirety of its behavior lives in a single translation unit.

```
legacy/Code/TMSrv/
├── ProcessDBMessage.cpp      # The complete dispatcher implementation (1330 lines)
│   ├── includes: Windows.h, Basedef.h, CPSock.h, ItemEffect.h, Language.h,
│   │            CItem.h, Server.h, ProcessClientMessage.h, GetFunc.h, SendFunc.h
│   └── void ProcessDBMessage(char *Msg)   # lines 39-1330
│       ├── [Gate] packet validation                       (43-52)
│       ├── [conn == 0] server-wide/system switch          (56-185)
│       │   ├── _MSG_TransperCharacter                     (61-67)
│       │   ├── _MSG_DBSetIndex                            (69-81)
│       │   ├── _MSG_War                                   (83-89)
│       │   ├── _MSG_GuildAlly                             (91-97)
│       │   ├── _MSG_GuildInfo                             (99-105)
│       │   ├── _MSG_GuildReport                           (107-112)
│       │   ├── _MSG_NPNotice (global)                     (114-121)
│       │   ├── _MSG_MessageDBImple                        (123-134)
│       │   ├── _MSG_MagicTrumpet                          (136-139)
│       │   ├── _MSG_DBNotice                              (141-146)
│       │   └── _MSG_DBCheckPrimaryAccount                 (148-182)
│       └── [conn > 0] per-user switch                     (188-1329)
│           ├── empty-slot / DBNoNeedSave handling         (190-208)
│           ├── _MSG_ReqTransper                           (213-226)
│           ├── _MSG_DBSendItem                            (229-338)
│           ├── _MSG_NPNotice (per-user)                   (341-351)
│           ├── _MSG_DBCNFAccountLogin                     (354-472)
│           ├── _MSG_DBNewAccountFail                      (475-484)
│           ├── _MSG_DBAccountLoginFail_Account            (487-498)
│           ├── _MSG_DBAccountLoginFail_Block              (501-524)
│           ├── _MSG_DBAccountLoginFail_Disable            (527-538)
│           ├── _MSG_AccountSecure / _MSG_AccountSecureFail(541-550)
│           ├── _MSG_DBOnlyOncePerDay                      (553-562)
│           ├── _MSG_DBAccountLoginFail_Pass               (565-576)
│           ├── _MSG_DBCNFNewCharacter                     (579-592)
│           ├── _MSG_DBCNFDeleteCharacter                  (595-606)
│           ├── _MSG_DBDeleteCharacterFail                 (609-619)
│           ├── _MSG_DBNewCharacterFail                    (622-632)
│           ├── _MSG_DBCNFCharacterLogin                   (635-1081)
│           ├── _MSG_DBCharacterLoginFail                  (1084-1096)
│           ├── _MSG_DBMessageBoxOk                        (1099-1108)
│           ├── _MSG_DBAlreadyPlaying / _MSG_DBStillPlaying(1111-1134)
│           ├── _MSG_DBSavingQuit                          (1137-1169)
│           ├── _MSG_DBCNFAccountLogOut                    (1172-1183)
│           ├── _MSG_DBCNFArchCharacterSucess / _Fail      (1186-1214)
│           ├── _MSG_DBSendDonate                          (1217-1251)
│           ├── _MSG_CNFDBCapsuleInfo / capsule results    (1254-1282)
│           ├── _MSG_GrindRankingData                      (1285-1296)
│           ├── _MSG_DBClientMessage                       (1299-1304)
│           ├── _MSG_DBServerSend1                         (1307-1315)
│           └── _MSG_DBCNFServerChange                     (1318-1324)
│
└── ProcessDBMessage.h        # Header (42 lines) - declares a STALE two-parameter
                              # prototype `void ProcessDBMessage(int conn, char *pMsg)`
                              # that does NOT match the implementation
```

The component is entirely a **procedural dispatch function**; there are no classes, structs, or helper functions defined within `ProcessDBMessage.cpp`. All state lives in external globals, and all behavior is delegated to shared server functions.

---

## 5. Dependency Analysis

### Internal Dependencies

The component depends on the following TMSrv-internal modules (resolved via includes and symbol references):

| Dependency (module) | Symbols used | Purpose |
|---------------------|--------------|---------|
| `Server.cpp` / `Server.h` | `CloseUser`, `SaveUser`, `DoRecall`, `RemoveParty`, `DoWar`, `DoAlly`, `MountProcess`, `ClearCrown`, `CharLogOut`, `CrackLog`, `SendBilling`, `IsFree`, `Log`, `ProcessImple`; globals `ServerGroup`, `ServerIndex`, `Sapphire`, `TransperCharacter`, `evDelete`, `BILLING`, `CHARSELBILL`, `NewbieEventServer`, `CastleState`, `KefraLive`, `RvRBonus`, `CurrentWeather`, `SecCounter`, `CurrentTime` | Session lifecycle, guild ops, config, logging, billing |
| `SendFunc.cpp` / `SendFunc.h` | `SendClientMessage`, `SendNotice`, `SendClientSignal`, `SendClientSignalParm`, `SyncMulticast`, `SendItem`, `SendScore`, `SendEtc`, `SendWarInfo`, `SendCarry`, `SendPKInfo`, `SendGridMob`, `GridMulticast` | All client-bound output |
| `GetFunc.cpp` / `GetFunc.h` | `GetAttribute`, `GetGuild`, `GetEmptyMobGrid`, `GetCreateMob` | World queries and mob creation |
| `Basedef.cpp` / `Basedef.h` | `BASE_GetVillage`, `BASE_GetItemCode`, `BASE_CanCargo`, `BASE_GetHpMp`, `BASE_GetBonusSkillPoint`, `BASE_GetBonusScorePoint`, `BASE_GetGuildName`; all `_MSG_*` and `MSG_*` definitions | Item/cargo math, stat recomputation, message contracts |
| `CMob.cpp` / `CMob.h` | `pMob[conn].GetCurrentScore(conn)`; `STRUCT_MOB`, `MOB_*` modes | Player/mob object manipulation |
| `CUser.cpp` / `CUser.h` | `pUser[]`, `USER_*` modes, `cSock` | Per-user connection state |
| `CPSock.cpp` / `CPSock.h` | `CPSock` (client socket), `DBServerSocket`, `SendOneMessage`, `AddMessage`, `SendMessageA`, `SendMessage` | Network I/O |
| `CItem.cpp` / `CItem.h` | `STRUCT_ITEM`, `g_pItemList` | Item data |
| `ItemEffect.h` | `EF_DAMAGE`, `EF_DAMAGEADD`, `EF_DAMAGE2`, `EF_CURKILL`, `EF_LTOTKILL`, `EF_HTOTKILL` | Effect codes |
| `Language.h` | `g_pMessageStringTable`, `_NN_*`, `_SN_*` string IDs | Localized messages |

Dependency chain (representative):

```
Server.cpp (socket loop)
  -> ProcessDBMessage(char *Msg)
       -> SendFunc (SendClientMessage, SendNotice, GridMulticast, ...)
       -> GetFunc (GetAttribute, GetGuild, GetEmptyMobGrid, ...)
       -> Basedef (BASE_CanCargo, BASE_GetHpMp, ...)
       -> CMob (GetCurrentScore)
       -> CPSock (DBServerSocket / cSock network I/O)
       -> Server.cpp (CloseUser, SaveUser, DoWar, DoAlly, ...)
       -> CItem / ItemEffect / Language (data + messages)
```

### External Dependencies

| Dependency | Type | Purpose | Notes |
|------------|------|---------|-------|
| DBSrv (sibling executable, `legacy/Code/DBSrv/`) | Internal sibling service | Origin of all DB2GAME messages | Message protocol defined in `Basedef.h`; no hard interface, uses shared structs |
| WinSock2 / Windows Sockets | Platform API | TCP transport for DB and client sockets | Via `CPSock` |
| Windows API (`windows.h`, `windowsx.h`) | Platform API | Types, threading primitives | Include-level |
| C runtime (`stdio.h`, `time.h`, `math.h`, `io.h`, `errno.h`, `fcntl.h`) | Standard library | Logging, time, I/O | Include-level |

No third-party libraries (e.g. no STL containers, no external SDKs) are used by this component; it relies on C-style arrays, pointers, and raw casts.

---

## 6. Afferent and Efferent Coupling

The component is a single free function. Coupling is measured at the function level.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| `ProcessDBMessage` | 1 (single call site: `Server.cpp:3914`) | High (~30+ distinct functions across 6+ modules) | High |
| `_MSG_DBCNFCharacterLogin` handler | 1 (dispatch) | Very high (20+ functions + 30+ globals) | High |
| `_MSG_DBSendItem` handler | 1 (dispatch) | Moderate (SendItem, BASE_CanCargo, SaveUser, BASE_GetItemCode, Log) | Medium |
| `_MSG_DBCheckPrimaryAccount` handler | 1 (dispatch) | Moderate (RemoveParty, DoRecall, BASE_GetVillage, GetAttribute, SendClientMessage) | Medium |

**Afferent coupling analysis:** Only one caller in the entire codebase invokes `ProcessDBMessage` — the DBSrv read loop at `Server.cpp:3914`. This makes it a leaf-ish consumer: it has essentially no fan-in, so changes to its signature only affect `Server.cpp`. However, the `ProcessDBMessage.h` header declares a mismatched two-parameter prototype that is never used; this header is included by several other translation units (`MobKilled.cpp:37`, `ProcessSecMinTimer.cpp:37`, `CWarTower.cpp:37`, `imple.cpp:30`, `CCastleZakum.cpp:37`, `Server.cpp:37`), meaning those files see a stale declaration that does not match the definition — a latent coupling hazard (they only compile today because none of them call the function directly).

**Efferent coupling analysis:** Efferent coupling is very high. The function reaches into `Server.cpp`, `SendFunc.cpp`, `GetFunc.cpp`, `Basedef.cpp`, `CMob.cpp`, `CUser.cpp`, `CPSock.cpp`, `CItem.cpp`, `Language.h`, and `ItemEffect.h`, and mutates dozens of module-level globals. This makes the component a broad, high-fan-out dispatcher that is highly coupled to the global server state and difficult to isolate. The complexity is concentrated: the `_MSG_DBCNFCharacterLogin` case alone (lines 635-1081) touches nearly every server subsystem.

---

## 7. Endpoints

This component **does not expose network/API endpoints**. It is an internal message handler invoked from the TMSrv main loop. Its "protocol surface" is the set of inbound `FLAG_DB2GAME` message types it consumes from the DBSrv TCP connection (as defined in `legacy/Code/Basedef.h`). The transport is a raw TCP socket with a length/type-framed binary protocol managed by `CPSock`. The message-type handlers it implements are enumerated in Sections 3 and 4. Since the component provides no REST/GraphQL/gRPC endpoints, this section is limited to this note.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv socket (`DBServerSocket`) | Sibling service (inbound) | Receive DB responses/commands | TCP, custom framed binary (`CPSock`) | `MSG_*` structs from `Basedef.h` | Validation gate at `ProcessDBMessage.cpp:43`; `ReadMessage` error codes handled in `Server.cpp:3907` |
| DBSrv socket (`DBServerSocket`) | Sibling service (outbound) | Acknowledgements (`_MSG_DBSendItem`/`_MSG_DBSendDonate` results, `_MSG_DBNoNeedSave`) | TCP, framed binary | `MSG_DBSendItem`, `MSG_DBSendDonate`, `MSG_STANDARD` | Direct `SendOneMessage`; no retry/backoff in this component |
| Client socket (`pUser[conn].cSock`) | Client (outbound) | Character/account confirmations, items, notices, signals | TCP, framed binary | `MSG_CNFClientCharacterLogin`, `MSG_CNFNewCharacter`, `MSG_MessageBoxOk`, etc. | Direct send; `CloseUser` on terminal failures |
| Client socket (`pUser[conn].cSock`) | Client (inbound flush) | `SendMessageA`/`SendMessage` flush then disconnect | TCP | n/a | `CloseUser(conn)` after flush |
| Game world grid | Internal | Spawn/despawn broadcast, notices | In-memory grid (`pMobGrid`) | `MSG_CreateMob`, `MSG_STANDARD` | `GridMulticast`/`SyncMulticast`; best-effort |
| `Log()` / `CrackLog()` | Logging | Audit and error logging | File/console | Formatted strings | Non-fatal; logged via `-system`/account/IP tags |
| Billing service | External (conditional, `BILLING > 0`) | Free-account billing requests | `SendBilling` | Internal | Guarded by `BILLING`/`CHARSELBILL` config |

**Error handling patterns:** The component's error handling is limited to (a) the packet validation gate, (b) mode/account precondition checks in `_MSG_DBSendItem`/`_MSG_DBSendDonate` (log-and-drop), (c) range checks before `pUser[conn]` access in a few handlers (e.g. `_MSG_DBSavingQuit`), and (d) `CloseUser` on terminal failures. There is no retry/backoff, no transactionality, and many handlers index `pUser[conn]`/`pMob[conn]` directly after only the top-level gate, which is noted as a risk.

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Dispatch / Command pattern | `switch (std->Type)` over message types | ProcessDBMessage.cpp:58, 210 | Route each DB message to its handler case |
| Parameter-block pattern | Raw pointer cast `(MSG_STANDARD*)Msg` then re-cast to concrete `MSG_*` struct | Throughout | Overlay typed views onto the flat byte buffer |
| Global singleton / service locator | `DBServerSocket`, `pUser[]`, `pMob[]`, `GuildInfo[]` accessed directly | Throughout | Shared server state access (no DI) |
| Facade (at server level) | `ProcessDBMessage` as a single entry point for all DB->game traffic | Server.cpp:3914 | Centralize inbound DB message handling |
| Guard clauses | Top-level validation gate + per-handler precondition checks | ProcessDBMessage.cpp:43, 233, 245, 1141 | Reject invalid/unauthorized input |

**Architectural decisions observed:**

- **Single-dispatch-function design:** All 40+ message types funnel through one C function with a `switch`. This keeps the protocol surface in one place but yields a very large, low-cohesion function.
- **Shared binary struct contracts:** The TMSrv and DBSrv share message struct definitions from `Basedef.h`; there is no IDL or versioned schema — protocol evolution requires coordinated edits.
- **Global-state-driven configuration:** Server identity (`ServerGroup`/`ServerIndex`/`Sapphire`), feature flags (`BILLING`, `CHARSELBILL`, `evDelete`, `NewbieEventServer`, `TransperCharacter`), and runtime state (`CastleState`, `KefraLive`, `RvRBonus`) are plain globals read/written by this component.
- **No abstraction layer** between the dispatcher and business logic: handlers call concrete server functions and mutate globals directly.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Per-user handlers | `conn`/`ID` bounds not re-validated per-case; many handlers index `pUser[conn]`/`pMob[conn]` directly after only the top-level gate (e.g. lines 222, 234, 358, 481, 581, 645...) | Potential out-of-bounds memory access if a malformed but gate-passing ID reaches these paths |
| High | `ProcessDBMessage.h` | Stale two-parameter prototype `(int conn, char *pMsg)` does not match implementation/declaration (`Server.h:99`); definition file doesn't even include its own header | Misleading contract; latent link/compile hazard if a translation unit ever calls the two-arg form |
| High | `_MSG_DBCNFCharacterLogin` (635-1081) | Extremely long imperative handler with mid-function `break` vs `return` TODO ambiguities (lines 649, 865, 895) and `pUser[0]`/`pMob[0]` indexing (lines 1004-1005) | Difficult to verify correctness; ambiguous control flow; possible wrong-slot access |
| High | `_MSG_DBSendItem` | Two-phase slot search logic (0..125 then 127..0) with early `return`; if both fail, `Result = 3`; boundary semantics of `MAX_CARGO - 2` implicit | Complex, easy to mis-maintain; reserved-slot policy undocumented |
| Medium | Global state | Heavily couples to dozens of module globals; no encapsulation | Non-unit-testable; fragile to reordering; hard to reason about concurrent/single-threaded assumptions |
| Medium | Security | `memcmp`/`strncmp` used for identity equality (MAC, account) rather than constant-time compare | Timing side-channel (minor in this context); string comparisons not length-bounded in all paths |
| Medium | `_MSG_DBCheckPrimaryAccount` | MAC-based anti-multibox relies on `pMac[]` block list and MAC matching; only `memcmp` full-width match | Bypass if MAC spoofing; logic assumes DBSrv is authoritative |
| Medium | Item sanitization | Hardcoded sIndex ranges `[470,500]` for `evDelete`, weapon nPos `64/192`, effect codes `98`/`106`, mantle ranges `543-546`/`548-549`/`3191-3196` | Magic numbers scattered; breaks on item-table changes |
| Medium | Reconnect | Socket reconnect/retry logic lives in `Server.cpp:3858-3868` (2 attempts, 200ms), not in this component | Recovery behavior is tightly coupled to the loop, not resilient |
| Low | Logging | Uses shared global `temp` buffer for formatting throughout | Not thread-safe; single global scratch buffer reused across call sites |
| Low | Comment hygiene | Several TODO markers (lines 81, 134, 649, 865, 895) flag unresolved control-flow questions | Uncertainty about intended semantics |

---

## 11. Test Coverage Analysis

A search of the entire `legacy` tree found **no test files** (no files named `*test*`/`*spec*`, no Google Test / Catch2 / assert-based test suites) for the TMSrv or DBSrv C/C++ servers. The only test-related artifacts in the repository are inside `.opencode/node_modules` (third-party JavaScript dependencies), which are excluded per the `ignore-folders` parameter and are unrelated to this component.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `ProcessDBMessage` | 0 | 0 | 0% (no automated tests) | N/A — no test suite exists |
| `_MSG_DBCNFCharacterLogin` handler | 0 | 0 | 0% | N/A |
| `_MSG_DBSendItem` handler | 0 | 0 | 0% | N/A |

**Observations:**

- The component is entirely untested. Given its role as the single dispatcher for all DB responses, this represents a significant regression risk: any change to message structs (shared with DBSrv in `Basedef.h`) or to handler logic cannot be validated automatically.
- No test scaffolding, mock layer, or harness exists to drive `ProcessDBMessage` with synthetic `MSG_*` buffers. The heavy dependence on global state (`pUser[]`, `pMob[]`, `DBServerSocket`) would require a substantial harness (global-state reset + a fake `CPSock`) to test in isolation.
- Test file locations for any future coverage would conventionally live alongside the source (e.g. under `legacy/Code/TMSrv/` or a sibling `legacy/test/`), but none currently exist.

---

## 12. Report Summary

`ProcessDBMessage` is the DBSrv-response dispatcher at the heart of the TMSrv's database integration. It is a single, large, procedural `switch`-based function that applies every database-confirmed outcome to the live game state. The analysis documents 40+ message handlers, ~19 distinct business rules (packet validation, server config, guild sync, notices, anti-multibox, character transfer, web item delivery, account login, character enter-world, creation/deletion, saving-quit, logout, donate credit, capsule transfer, ranking, and server switching), a high-efferent/low-afferent coupling profile, and a complete absence of automated test coverage. Key risks include the stale header prototype, unchecked per-case index access, and an extremely long, partially-ambiguous enter-world handler.

---

*Report generated by the Component Deep Analyzer. This is an analysis-only deliverable; no project source files were modified.*
