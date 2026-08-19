# Component Deep Analysis Report

## Component: _MSG_* Handlers (W2PP Legacy TMSrv)

**Project scope:** `legacy/` (W2PP C/C++ codebase at `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy`)
**Folders ignored:** `.git`, `.opencode`
**Date:** 2026-08-19 17:13:23
**Analyzer:** Senior Software Architect / Component Analysis Expert (analysis and reporting only — no code modified)

---

## 1. Executive Summary

The `_MSG_* Handlers` component is the **client-to-game-server message processing layer** of the W2PP legacy game server (TMSrv — "Telepathy/Game Server"). It consists of **58 handler functions**, each implemented in a dedicated `_MSG_<name>.cpp` source file located in `legacy/Code/TMSrv/`, with a single shared entry dispatcher and a single shared header declaring all handlers.

Each handler (`Exec_MSG_*`) implements the complete per-message business logic for one category of client action: account/character lifecycle, chat and whispers, movement and actions, combat, item usage and inventory management, merchant buying/selling, item combination (crafting), player trading, party management, player-vs-player duels and ranking, and guild/war/ally management. The handlers operate directly on the shared in-memory world state (`pUser[]`, `pMob[]`, `pItem[]`, `pItemGrid[]`, `pMobGrid[]`, `g_pGuildZone[]`, etc.) and communicate with the external database server (`DBSrv`) via a message-passing socket (`DBServerSocket`).

**Role in the system:** The component is the single point of entry for every game action a connected client performs while in-game. It receives raw binary protocol packets (identified by a `_MSG_*` type constant) through `ProcessClientMessage(int conn, char *pMsg, BOOL isServer)`, which acts as a large `switch` dispatcher, routing each packet to its corresponding `Exec_MSG_*` handler. Because all 58 handlers are reached through this one dispatcher and share a common set of helper libraries, the component is the backbone of the game server's gameplay logic.

**Key findings:**
- **Centralized dispatch, decentralized logic:** All routing flows through the `switch` in `ProcessClientMessage.cpp:66-311`, but each business rule lives in its own isolated handler file. This gives high cohesion per handler but creates a very wide fan-in at the dispatcher.
- **Heavy anti-cheat/validation emphasis:** Nearly every handler begins with a state-machine check (`pUser[conn].Mode != USER_PLAY`) and HP check, and most call `AddCrackError`/`CrackLog` to penalize or log suspected packet tampering (timestamp validation, movement speed caps, route/tick checks).
- **Custom binary protocol, not REST:** There are no HTTP/GraphQL/gRPC endpoints; the "endpoints" are binary packet message types over a raw TCP socket (see section 7).
- **In-memory world state with deferred persistence:** Handlers mutate global in-memory structures directly; persistence is deferred and coordinated with the external `DBSrv` via forwarded `_MSG_DB*` message types.
- **No automated tests exist** anywhere in the `legacy/` tree (confirmed by exhaustive search). This is a significant quality risk for a large, mutation-heavy logic layer.
- **Currency/inventory integrity is the dominant risk theme:** A large fraction of validation code is dedicated to preventing coin overflow (the 2,000,000,000 cap), ensuring item pointers are in range, and verifying that client-supplied item snapshots match server-held state (`memcmp` against `pMob[conn].MOB.Carry[]` / `pUser[conn].Cargo[]`).

---

## 2. Data Flow Analysis

Data flows from the client socket, through the shared dispatcher, into a handler, which mutates world state and either replies to the client, broadcasts to nearby players, or forwards a derived message to the database server.

```
1. Packet received from client socket via recv()
   -> Server.cpp:3975  pUser[User].cSock.ReadMessage(&Error, &ErrorCode)

2. Entry validation in Server.cpp:4001
   -> ProcessClientMessage(User, Msg, FALSE)

3. Dispatcher (ProcessClientMessage.cpp:38)
   -> MSG_STANDARD *std = (MSG_STANDARD*)pMsg
   -> Bounds check: 0 <= std->ID < MAX_USER
   -> ServerDown >= 120 guard; Ping short-circuit; ClientTick/SKIPCHECKTICK guard
   -> switch(std->Type) routes to Exec_MSG_<Type>

4. Handler validation (per-handler, representative _MSG_Trade.cpp:24)
   -> pMob[conn].MOB.CurrentScore.Hp == 0 || pUser[conn].Mode != USER_PLAY
   -> SendHpMode(conn); AddCrackError(conn, 5, 18); RemoveTrade(conn); return;

5. Business logic (e.g. _MSG_Buy.cpp:104-313)
   -> Read/write shared state: pMob[conn].MOB.Coin, pMob[TargetID].MOB.Carry[]
   -> Item/coin integrity checks (memcmp, range checks, 2G overflow guard)

6. World broadcast to nearby players
   -> GridMulticast(pMob[conn].TargetX, pMob[conn].TargetY, (MSG_STANDARD*)pMsg)

7. Client response formatting
   -> SendClientMessage / SendItem / SendScore / SendEtc / pUser[conn].cSock.AddMessage

8. External DB persistence (only for certain handlers)
   -> DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_*))
   -> e.g. _MSG_AccountLogin.cpp:92 (MSG_DBAccountLogin), _MSG_CapsuleInfo.cpp:30
```

Handlers fall into three data-flow archetypes:
- **Pure local state mutation** (most in-game actions): e.g. `_MSG_ApplyBonus`, `_MSG_Deposit`, `_MSG_DropItem` — mutate `pMob[conn].MOB.*` and reply/broadcast locally.
- **DB-forwarding proxies** (account/character lifecycle): e.g. `_MSG_AccountLogin`, `_MSG_AccountSecure`, `_MSG_CreateCharacter`, `_MSG_DeleteCharacter`, `_MSG_CharacterLogin`, `_MSG_CapsuleInfo`, `_MSG_GuildAlly`, `_MSG_War`, `_MSG_PutoutSeal` — rewrite `m->Type` to a `_MSG_DB*` variant and forward to `DBServerSocket`, transitioning the user's mode state (`USER_SELCHAR -> USER_WAITDB`, etc.).
- **Two-player coordination** (trading/parties/duels): e.g. `_MSG_Trade`, `_MSG_AcceptParty`, `_MSG_ReqRanking` — validate both parties, update both `pUser[]`/`pMob[]` records, and send messages to both sockets.

---

## 3. Business Rules & Logic

The component encodes the core gameplay business rules of the W2PP game server. Rules are implicit in code (no separate spec was found); the following extraction documents them with their implementation locations. Confidence is high where the rule is explicitly enforced by guards; where behavior is ambiguous (e.g. magic constants without comments) it is noted.

### 3.1 Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Connection index must be within `1..MAX_USER-1` (admin-reserved top range excluded) | ProcessClientMessage.cpp:42, _MSG_AccountLogin.cpp:30 |
| Validation | Player must be in `USER_PLAY` mode and alive (HP > 0) to perform most actions | e.g. _MSG_Action.cpp:25, _MSG_Trade.cpp:24, _MSG_Motion.cpp:24 |
| Validation | Packet client tick must not be stale/future; anti-speed-hack windows (900ms / 15000ms limits) | _MSG_Action.cpp:63-145 |
| Validation | Movement target must be within view grid; speed capped to `AttackRun & 0xF` | _MSG_Action.cpp:147-175 |
| Business Logic | Failed account logins tracked; 3+ failures blocks login (`CheckFailAccount >= 3`) | _MSG_AccountLogin.cpp:82-90 |
| Business Logic | Character create/delete require `USER_SELCHAR` mode and a valid name string | _MSG_CreateCharacter.cpp:27, _MSG_DeleteCharacter.cpp:26 |
| Business Logic | Party join level-gated by `PARTY_DIF` window unless high-level/class-equality bypass | _MSG_AcceptParty.cpp:93-149, _MSG_SendReqParty.cpp:68-99 |
| Business Logic | Trade requires both parties' PK mode off and whisper not denied; 2G coin cap enforced | _MSG_Trade.cpp:120-138, 251-299 |
| Business Logic | Non-tradeable/restricted items (guild medals, certain indices) cannot be traded/dropped | _MSG_Trade.cpp:147-190, _MSG_DropItem.cpp:109-110 |
| Business Logic | Item combining uses a `rand()%115` success roll vs a per-combination threshold | _MSG_CombineItem.cpp:80-84 and all `_MSG_CombineItem*` variants |
| Business Logic | Auto-trade stalls must be in a village; item/coin/price must match seller's posted offer | _MSG_SendAutoTrade.cpp:54-96, _MSG_ReqBuy.cpp:61-127 |
| Validation | Deposit/withdraw capped at 2,000,000,000 coins in either balance | _MSG_Deposit.cpp:33, _MSG_Withdraw.cpp:33 |
| Business Logic | City tax (0-30%) applies to buy/sell prices; guild-discount and skill-discount modifiers | _MSG_Buy.cpp:106-145, _MSG_Sell.cpp:182-213 |
| Business Logic | Guild leadership (GuildLevel 9) required for wars, allies, invites, tax setting | _MSG_GuildAlly.cpp:33, _MSG_War.cpp:33, _MSG_InviteGuild.cpp:44-49 |
| Business Logic | Skill learning gated by class match, skill points, level/stat requirements, coin cost for master skills | _MSG_ApplyBonus.cpp:129-249 |
| Business Logic | Gate opening requires a matching key item (`EF_KEYID`) when gate state transitions to/from 3 | _MSG_UpdateItem.cpp:57-90 |
| Security | Suspected packet tampering increments crack counters / logs via `AddCrackError`/`CrackLog` | throughout; e.g. _MSG_Action.cpp:77, _MSG_AcceptParty.cpp:45 |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: Connection and Session State Guarding

**Overview:**
Every handler assumes the caller is a valid, connected user in a well-defined session state. The dispatcher enforces the connection index range once, and each handler independently re-verifies the user's session mode and liveness.

**Detailed description:**
The dispatcher `ProcessClientMessage` first validates that `std->ID` is in `[0, MAX_USER)` (ProcessClientMessage.cpp:42) and that the server is not shutting down (`ServerDown >= 120`, line 53). It records the last receive time and rejects packets where the client-supplied tick equals the sentinel `SKIPCHECKTICK` unless the packet originated from the server itself (`isServer == TRUE`). Handlers then perform their own mode gate. The dominant pattern is the `pUser[conn].Mode != USER_PLAY` check, which restricts gameplay actions to the playing state and otherwise returns a corrective `SendHpMode(conn)`. Liveness is enforced with `pMob[conn].MOB.CurrentScore.Hp == 0`, which triggers `SendHpMode` plus an `AddCrackError` penalty — the assumption being that a dead or non-playing client should not be issuing gameplay commands. These guards protect the shared in-memory world state from inconsistent mutation and are the first line of anti-cheat defense.

**Rule workflow:**
1. Packet enters dispatcher; `std->ID` range-validated.
2. Server-down and ping/tick guards applied.
3. Handler checks session mode (`USER_PLAY`) and HP.
4. On violation, `SendHpMode`/`SendClientMessage` issued and `AddCrackError` incremented.
5. Otherwise handler proceeds to business logic.

---

### Business Rule: Movement, Action, and Anti-Speed-Hack Validation

**Overview:**
`Exec_MSG_Action` (and `_MSG_Motion`) govern character movement and animation. They enforce timing limits to defeat speed hacks and movement teleport exploits, and restrict movement into prohibited zones.

**Detailed description:**
The action handler distinguishes three packet variants: `_MSG_Action` (walk/run), `_MSG_Action2` (stop), and `_MSG_Action3` (the Illusion class skill teleport). For the Illusion skill, the handler verifies the character class (Class 3) and that the `Illusion` learned-skill bit is set; otherwise it increments a crack counter. It deducts the spell's mana cost before validating the client timestamp. Timing rules require that the client tick is not older than `CurrentTime - 120000` and not more than `CurrentTime + 15000`, and that consecutive action packets are at least 900ms apart (`LastIllusionTick`), else `AddCrackError` is triggered. Movement speed is clamped to the character's `AttackRun & 0xF` value if the client reports a faster speed. Target coordinates must lie within the view grid (a `VIEWGRIDX * 2` teleport window triggers a forced teleport-back via `GetAction`) and within the world bounds `(0,4096)`. Special zone restrictions then apply: newbie-zone attribute (`0x80`), guild-zone membership (`0x20`), Pista (racing track) leader-only teleports, BattleRoyale (`BrState`/`BRItem`), and `Colo150Limit` level-based exclusions. Trade is forcibly cancelled if the user attempts to move while trading.

**Rule workflow:**
1. Validate mode/HP; cancel any active trade.
2. Class/zone-specific gate (Illusion, Celestial, Pista, newbie, guild zones).
3. Compute timing window; reject stale/future or too-frequent packets via `AddCrackError`.
4. Clamp reported speed; validate target within view grid and world bounds.
5. Apply per-zone movement restrictions (recall on violation).
6. Update position/route in `pMob[conn]`, multicast movement to neighbors via `GridMulticast`.

---

### Business Rule: Account and Character Lifecycle Management

**Overview:**
Handlers `_MSG_AccountLogin`, `_MSG_AccountSecure`, `_MSG_CharacterLogin`, `_MSG_CharacterLogout`, `_MSG_CreateCharacter`, and `_MSG_DeleteCharacter` manage session establishment and character selection. These are DB-forwarding handlers that hand off persistence to `DBSrv`.

**Detailed description:**
`Exec_MSG_AccountLogin` validates the connection slot (excluding the admin-reserved range), checks that the client version matches `APP_VERSION` (or size constraints under `_PACKET_DEBUG`), verifies the user is in `USER_ACCEPT` mode, captures the MAC address, normalizes the account name to uppercase, and calls `CheckFailAccount` to enforce the "3 failed login attempts" lockout. It then rewrites the message type to `_MSG_DBAccountLogin`, sets `m->ID = conn`, forwards to `DBServerSocket`, and transitions the user to `USER_LOGIN` mode. `Exec_MSG_CharacterLogin` enforces that the requested slot is valid, then runs a billing/level gating branch: free accounts are restricted to characters below `FREEEXP` level, and under `BILLING` modes the server checks the user's billing state (`Unk_1816`) before dispatching `_MSG_DBCharacterLogin`. `Exec_MSG_CreateCharacter` and `Exec_MSG_DeleteCharacter` both require the user to be in `USER_SELCHAR` mode, validate the character name with `BASE_CheckValidString`, set `USER_WAITDB`, and forward `_MSG_DBCreateCharacter`/`_MSG_DBDeleteCharacter`. `Exec_MSG_CharacterLogout` removes the user from any party and calls `CharLogOut(conn)`.

**Rule workflow:**
1. Validate connection slot and version.
2. For login: enforce mode and failed-attempt lockout; normalize account name.
3. Rewrite packet type to `_MSG_DB*`; set `ID = conn`; forward via `DBServerSocket`.
4. Transition user mode (`USER_ACCEPT -> USER_LOGIN`, `USER_SELCHAR -> USER_WAITDB`, etc.).
5. For logout: `RemoveParty(conn)` then `CharLogOut(conn)`.

---

### Business Rule: Chat and Whisper Command Handling

**Overview:**
`Exec_MSG_MessageChat` handles general chat and slash-commands, while `Exec_MSG_MessageWhisper` (1,378 lines) implements a large set of GM/admin and guild commands (the single largest non-item handler).

**Detailed description:**
`Exec_MSG_MessageChat` first parses the message for a leading command token. It implements client-side preference toggles (`guildon`/`guildoff`, `whisper`, `partychat`, `kingdomchat`, `guildchat`, `chatting`) that flip boolean fields on `pUser[conn]`. It also implements the guild tax command: only a guild master (`GuildLevel == 9`) may set a city tax in `0..30` (percent), at most once per day (`TaxChanged[i]`), persisting via `CReadFiles::WriteGuild()`. During BattleRoyale it censors chat within specific map regions. Muted users (`MuteChat == 1`) are refused. Valid playing users' messages are broadcast to the grid (via the party leader for party chat). `Exec_MSG_MessageWhisper` dispatches on the recipient name (e.g. `"cp"` for cross-server, `"tab"`, guild commands, GM commands) and implements a large set of administrative and guild-management actions: server-group checks, mute handling, guild creation (with coin costs and item `3330` currency checks), crystal/quest items, citizen checks, guild-sub-commander setup (`GuildInfo[Guild].Sub1/2/3`), guild member promotion, and war/ally management.

**Rule workflow:**
1. Parse command token from message string.
2. Handle preference toggles and guild-tax (guild master only, 0-30%, once/day).
3. Apply region chat censoring (BattleRoyale).
4. Refuse muted users; verify `USER_PLAY` mode.
5. Broadcast via `GridMulticast` (party-leader scoping for party chat) and write chat log.
6. Whisper handler routes by recipient token to the relevant GM/guild sub-routine.

---

### Business Rule: Party Management

**Overview:**
`_MSG_SendReqParty`, `_MSG_AcceptParty`, and `_MSG_RemoveParty` implement party creation, invitation, acceptance, and removal with strict integrity and level-gating checks.

**Detailed description:**
`Exec_MSG_SendReqParty` validates that the requester is their own party leader (`partyID == conn`), not already in a party, that the target is a connected playing user not in a party, and that neither is in "OnlyTrade" mode. It then applies the party level-gap rule: the target's effective level (adjusted for class master: `MORTAL`/`ARCH` use raw level, others add `MAX_CLEVEL`) must be within `PARTY_DIF` of the requester's, unless either is level 1000+ or they share the same class master — mirroring the acceptance rule. On success it sets `pMob[targetID].LastReqParty = conn` and sends the request. `Exec_MSG_AcceptParty` verifies the leader is in range, that the invitation actually exists (`leaderID == pMob[myindex].LastReqParty` — a crack/`PARTYHACK` check), that the leader isn't already leading a party, and re-applies the same level-gap/class rule. It then scans for a free slot in `pMob[leaderID].PartyList[]` (refusing if full), links both parties, and broadcasts the updated roster via `SendAddParty` to all members. `Exec_MSG_RemoveParty` resolves the target (self or a listed member) and calls `RemoveParty`.

**Rule workflow:**
1. Validate requester/target indices and party membership state.
2. Enforce `PARTY_DIF` level window (or high-level / same-class exemption).
3. Set `LastReqParty` for acceptance correlation.
4. On accept: verify pending request, find free slot, link `Leader`/`PartyList`.
5. Broadcast roster updates to all affected members.

---

### Business Rule: Player-to-Player Trading

**Overview:**
`_MSG_Trade` implements the full two-player trade protocol: offer exchange, mutual check confirmation, item/money integrity verification, and atomic exchange. Supporting handlers `_MSG_TradingItem` (inventory swap), `_MSG_QuitTrade` (cancellation), `_MSG_ReqTradeList`, `_MSG_SendAutoTrade`, and `_MSG_ReqBuy` handle the related trade-adjacent flows (item rearrangement and automated merchant stalls).

**Detailed description:**
`Exec_MSG_Trade` validates liveness, opponent index, and that the offered `TradeMoney` does not exceed the player's coin. It verifies that every offered item's `InvenPos` is in range and that the client's snapshot (`m->Item[i]`) matches the server's `pMob[conn].MOB.Carry[InvenPos]` via `memcmp`; any mismatch cancels the trade. Trading is prohibited when either party is in PK mode or has whispers denied, and restricted items (guild medals `508/522/526-537/446`, non-tradeable `EF_NOTRADE`) are rejected unless both are guild masters of the relevant guild. When both players have confirmed (`MyCheck == 1`), the handler validates the full trade structure, caps both parties' resulting coin at 2,000,000,000, ensures neither `TradeMoney` exceeds the holder's coin, checks carry capacity on both sides with `BASE_CanTrade`, then performs the atomic exchange by copying the precomputed destination arrays and updating coin balances. It logs the exchange (`ItemLog`) and persists both users (`SaveUser`). `_MSG_TradingItem` handles inventory/equip/cargo swapping with equip-compatibility checks (`BASE_CanEquip`), stack-merging rules for stackable items (indices `412/413/419/420/2390-2419`, max stack 120), date-expiry handling for timed items, and cargo auto-arrangement. `_MSG_QuitTrade` cancels and broadcasts updated PK info.

**Rule workflow:**
1. Validate liveness, opponent, trade-money bounds, item snapshot integrity.
2. Apply PK/whisper/restricted-item guards.
3. Relay offers between parties; on mutual check, re-validate structure.
4. Verify 2G coin cap and carry capacity for both parties.
5. Atomically swap carried items and coin; log and persist; clear trade state.

---

### Business Rule: Item Combination (Crafting)

**Overview:**
Ten handlers (`_MSG_CombineItem`, `_MSG_CombineItemEhre`, `_MSG_CombineItemTiny`, `_MSG_CombineItemShany`, `_MSG_CombineItemAilyn`, `_MSG_CombineItemAgatha`, `_MSG_CombineItemOdin`, `_MSG_CombineItemLindy`, `_MSG_CombineItemAlquimia`, `_MSG_CombineItemExtracao`) implement NPC-specific crafting recipes with a shared probabilistic success/failure model.

**Detailed description:**
Each handler follows the same skeleton: (1) verify each offered ingredient's inventory position is valid and that the client snapshot matches the server state; (2) call a NPC-specific matcher (`GetMatchCombine*`) to determine the recipe ID (`combine`) and its success threshold; (3) enforce recipe-specific preconditions; (4) consume the ingredients; (5) roll `_rand = rand() % 115` (with `_rand >= 100` reduced by 15), succeeding when `_rand <= combine` (or `LOCALSERVER`); (6) on success, place the crafted item and announce; on failure, send a failure signal. Recipe-specific gates include: Odin — elemental-stone completeness, sanctification limits (`REF_15`), quest/level/class conditions, and celestial-item crafting; Ehre — level/class/exp gates and coin costs (1,000,000) for certain recipes; Alquimia — restricted to class 3 (alchemist) with a grade-derived chance; Lindy — a special potion recipe checked via specific item indices and amounts. The base `_MSG_CombineItem` applies an "ancient item + jewel" recipe where the jewel index (`m->Item[1].sIndex - 2441`) selects a variant (`joia + extra`) and sanctification is set.

**Rule workflow:**
1. Validate ingredient positions and snapshot integrity.
2. Resolve recipe via NPC matcher; reject unknown (`combine == 0`) recipes.
3. Enforce recipe-specific preconditions (class, level, coin, stones, sanctification, quest flags).
4. Consume ingredients from carry.
5. Roll success against recipe threshold; craft item or signal failure; log.

---

### Business Rule: Merchant Buying and Selling

**Overview:**
`_MSG_ReqShopList`, `_MSG_Buy`, and `_MSG_Sell` implement shop browsing and NPC-commerce, including donation purchases, city taxation, guild discounts, and merchant-stock management.

**Detailed description:**
`Exec_MSG_ReqShopList` verifies the target is a merchant NPC in view and dispatches the appropriate shop list (merchant type 1 → standard shop, type 19 → special shop). `Exec_MSG_Buy` validates the target merchant and line-of-sight, and computes the price: base price plus city tax (0-30%) with a per-item split going to the city treasury and the tax-collector guild; a 30% guild-member discount when the buyer's guild holds the village; and a merchant-skill (class 3) discount derived from the character's dexterity special. Donation items (`EF_DONATE`) are purchased against the user's `Donate` balance instead of coin. Guild items (medals, flags) require guild membership/level and are personalized with guild effects; the guild-medal counter (`GuildCounter`) and server index are encoded into the item. Special merchant mobs have per-slot stock (`pMobMerc[].Stock`) decremented on purchase. `Exec_MSG_Sell` prices items at base/4 with progressive reductions, applies the same city tax in reverse (tax deducted from the seller's proceeds), and credits the guild treasury. Potions (`EF_VOLATILE == 1`) and certain items (`3193`, `3194`, `747`) cannot be sold.

**Rule workflow:**
1. Verify merchant NPC, in-view, and correct shop type.
2. Compute price (base + tax, guild/skill discounts, donation path).
3. Validate buyer funds/capacity; verify and decrement special-merchant stock.
4. Personalize guild items; increment guild counter where applicable.
5. Exchange coin/item; credit city and tax-collector guild treasuries; log and persist.

---

### Business Rule: Bank Deposit and Withdrawal

**Overview:**
`_MSG_Deposit` and `_MSG_Withdraw` move coins between the character's carry (`pMob[conn].MOB.Coin`) and their bank/cargo balance (`pUser[conn].Coin`), with a strict 2,000,000,000 cap.

**Detailed description:**
Both handlers require the user to be alive and in `USER_PLAY` mode. `Exec_MSG_Deposit` checks that the requested amount is non-negative, does not exceed the character's current coins, and that the resulting bank balance stays within `[0, 2000000000]`; if all hold, it debits carry and credits the bank, then echoes the message and refreshes the cargo-coin display (`SendCargoCoin`). `Exec_MSG_Withdraw` is the mirror: it verifies the amount against the bank balance and the resulting carry total against the cap, then credits carry and debits the bank. Both log the transaction.

**Rule workflow:**
1. Verify liveness/mode.
2. Validate amount bounds and resulting-balance cap (2G).
3. Apply debit/credit between carry and bank.
4. Echo message and refresh client coin displays.

---

### Business Rule: Skill and Attribute Point Allocation

**Overview:**
`_MSG_ApplyBonus` handles the three bonus types: attribute points (`BonusType 0`), special/mastery points (`BonusType 1`), and skill learning (`BonusType 2`).

**Detailed description:**
For attribute points, the handler spends `ScoreBonus` (1 point normally, or 100-point chunks when `ScoreBonus >= 300`) into Str/Int/Dex/Con, with Int also raising MaxMp and Con raising MaxHp. For special points, it enforces a level-derived cap (`3 * (level + 1)`, plus celestial bonus) and a hard cap of 200 (255 when a master-skill bit is learned), refusing over-allocation. For skill learning, it validates the skill index (`5000..5095`), that the character's class matches the skill class, that enough `SkillBonus` points exist, that prerequisite skills are learned, that the level and stat requirements (`ReqLvl`, `ReqInt/Dex/Con`) are met, and that a 50,000,000 coin cost is paid for master skills (positions 7/15/23). Only one master skill per tree may be learned.

**Rule workflow:**
1. Validate mode/HP.
2. For attributes/specials: check available points and caps; apply increment; recompute scores.
3. For skills: validate index, class, prerequisites, requirements, and cost.
4. Deduct points/coin, set the learned-skill bit, recompute and refresh scores.

---

### Business Rule: Gate and Door Opening (Keyed Items)

**Overview:**
`_MSG_UpdateItem` opens gates/doors in the world, requiring a matching key item when the gate enters/leaves the locked (`state 3`) state.

**Detailed description:**
The handler validates the item/gate index range and that the user is alive and playing. It first attempts a castle-gate open via `CCastleZakum::OpenCastleGate`. Otherwise it determines whether a key is required: only when the current or target gate state is `3`. If the gate has a `EF_KEYID` set, the handler scans the player's carry for an item with a matching `EF_KEYID`; if none is found it refuses (with a "no key" message unless the gate is index `773`). If found, the key item is consumed (cleared) and the gate is opened via `UpdateItem(gateid, STATE_OPEN, &heigth)`, broadcasting the state change to the grid.

**Rule workflow:**
1. Validate mode/HP, gate state, and gate index.
2. Attempt castle-gate path.
3. If state transitions to/from 3 and gate has a key ID, find matching key in carry.
4. Refuse or consume the key; open the gate; broadcast update.

---

### Business Rule: Guild, War, and Alliance Management

**Overview:**
`_MSG_GuildAlly`, `_MSG_War`, `_MSG_InviteGuild`, `_MSG_Challange`, and `_MSG_ChallangeConfirm` implement guild diplomacy, wars, alliances, member invitation, and city-challenge management.

**Detailed description:**
`Exec_MSG_GuildAlly` and `Exec_MSG_War` both require the sender to be a guild master (`pMob[conn].MOB.GuildLevel == 9`) of the stated guild, validate guild indices (`1..65535`), and forward the request to the DB server. `Exec_MSG_InviteGuild` requires the sender to belong to a guild, the target to be guild-less and of the same clan, the sender to have guild rank (and guild-master rank `9` for non-basic invite types), and blocks invitations on Sundays. It charges a cost (4,000,000 basic, 100,000,000 for higher types), sets the target's guild, and broadcasts the membership change. `Exec_MSG_Challange` handles city tax collection (the tax-collector guild master can withdraw the stored town tax as coins or gold-ticket items `4011`, capped at 2G) and reports challenger/champion status, while `Exec_MSG_ChallangeConfirm` triggers the actual challenge for a valid zone.

**Rule workflow:**
1. Verify guild-master rank and guild index bounds.
2. Validate clan/rank/day restrictions and collect the invite cost.
3. Apply guild membership change; broadcast to grid.
4. For war/ally: forward request to DB server.
5. For challenges: handle tax withdrawal or initiate/confirm the challenge.

---

### Business Rule: Duel, Ranking, and PK Mode

**Overview:**
`_MSG_ReqRanking`, `_MSG_Challange`, `_MSG_PKMode`, and `_MSG_QuitTrade` handle player-vs-player state and ranked duels.

**Detailed description:**
`Exec_MSG_ReqRanking` lets a player challenge another to a ranking duel: it validates the target, denies if the target has whispers off, records `RankingTarget`/`RankingType` on the requester, and forwards the challenge. On confirmation (`DuelParm == 4`), it verifies the target reciprocated (`pUser[tDuel].RankingTarget == conn`), that no battle is in progress (`RankingProgress`), and then starts the duel via `DoRanking`. `Exec_MSG_PKMode` toggles the player's PK mode, cancels any active trade if either party toggles it, and broadcasts updated PK info; the PK state shown to others is derived from `GetGuilty`, PK mode, and regional war/castle state. `Exec_MSG_QuitTrade` (a trade-cancel path) similarly recomputes and broadcasts PK info after cancelling the trade.

**Rule workflow:**
1. Validate target and mutual consent; check for in-progress battle.
2. Record challenge state; forward confirmation.
3. On confirm, start ranked duel via `DoRanking`.
4. For PK mode: toggle flag, cancel trade if needed, broadcast PK info.

---

### Business Rule: Item Usage (Potion/Enhancement/Scroll Logic)

**Overview:**
`_MSG_UseItem` (5,726 lines, the largest handler) implements the full item-use engine: potions, enhancement stones, and consumables, including timed use-cooldowns and enhancement probability rolls.

**Detailed description:**
The handler requires playing/live state and refuses use during trade/auto-trade. It classifies items by `EF_VOLATILE` value: `Vol == 1` potions heal HP/MP subject to a `PotionDelay` cooldown (`PotionTime`); `Vol == 4/5` are enhancement items (PO/PL) applied to a destination item in carry/equip, with a `UseItemTime` cooldown (1000ms) and a probabilistic enhancement roll (`_rd % 100` vs `_chance`). Enhancement is gated by item type and sanctification level: only certain item ranges are enhanceable, and enhancement is blocked beyond sanctification thresholds (e.g. `sanc >= 6` for `Vol == 4` unless the item has special effect criteria). Timed items (`_MSG_UseItem.cpp:20` `HuntingScrolls[6][10][2]`) map scroll types to effects. The handler also validates `SourType`/`SourPos` against carry/cargo/equip, consumes one unit when stackable, and refreshes the item display via `SendItem`. Hunting scrolls grant buffs tied to the user's level band.

**Rule workflow:**
1. Validate mode/HP; cancel trade/auto-trade state.
2. Resolve source item by place/position.
3. For potions: apply cooldown, heal HP/MP up to max, consume a unit.
4. For enhancement: verify dest item validity/type/sanctification, apply cooldown, roll success, apply effects or consume.
5. For scrolls: map scroll to effect, grant buff, consume.
6. Refresh client item/score state and log usage.

---

### Business Rule: Quest and NPC Interaction Dispatch

**Overview:**
`_MSG_Quest` (2,712 lines) dispatches on the targeted NPC's merchant/grade attributes to route into dozens of quest-specific sub-flows.

**Detailed description:**
The handler validates the NPC index range and cancels trade/auto-trade. It then maps the NPC's `Merchant` value and equipped grade (`EF_GRADE0`) to a quest-mode constant (e.g. `QUEST_COVEIRO`, `QUEST_JARDINEIRO`, `QUEST_KAIZEN`, `QUEST_HIDRA`, `PERZEN`, `UXMAL`, etc.) and branches into the corresponding quest logic. Each branch implements that NPC's dialogue, accept/reject, item-hand-in, reward, and progression rules, mutating the character's `pMob[conn].extra.QuestInfo` fields and granting rewards. This is the largest single source of quest content and is tightly coupled to the `CNPCGene` NPC generator for locating the NPC.

**Rule workflow:**
1. Validate NPC index; cancel trade/auto-trade.
2. Resolve `npcMode` from NPC merchant/grade.
3. Branch to the specific quest handler for that NPC.
4. Execute dialogue/accept/reward logic, mutating quest-state and inventory.
5. Reply to client and refresh state.

---

### Business Rule: Combat and Skill Execution

**Overview:**
`_MSG_Attack` (1,789 lines) implements the combat engine for normal and skill attacks, including attack-speed throttling, mana costs, skill validation, and special-case skills.

**Detailed description:**
The handler validates playing state and liveness (allowing the `skillIndex == 99` death exception), and cancels trade. It enforces an attack-rate limit via `LastAttackTick` (minimum 800ms between attacks) and a 15-second anti-hack window on the client tick. Skills are validated against the learned-skill bitmask, the character class (`skillnum / 24 == Class`), and skill-type restrictions; mana (`ManaSpent`) must be available. Passive and mastery skills are handled specially, and certain skills (`skillnum == 85`) require coins. It computes the attack against the target from `m->TargetX/Y`, deducts mana, applies the damage/effect logic, and broadcasts the combat result. Special skills (e.g. Illusion teleport, summoning via item `746`) have dedicated branches.

**Rule workflow:**
1. Validate mode/liveness; cancel trade.
2. Throttle attack rate (800ms) and validate client tick window.
3. Validate skill index, class match, learned bit, skill type, and mana.
4. Deduct mana/coins; compute and apply damage/effect to target.
5. Broadcast combat state and update scores.

---

## 4. Component Structure

The component is spread across 58 source files plus 2 shared files (dispatcher + header). All files live in `legacy/Code/TMSrv/`.

```
legacy/Code/TMSrv/
├── ProcessClientMessage.cpp   # Central switch dispatcher (313 lines)
├── ProcessClientMessage.h     # Declares all 58 Exec_MSG_* handlers + dispatcher (132 lines)
├── _MSG_AccountLogin.cpp      # Account login (DB-forwarding)
├── _MSG_AccountSecure.cpp     # Account security question (DB-forwarding)
├── _MSG_CharacterLogin.cpp    # Character login with billing/level gating (DB-forwarding)
├── _MSG_CharacterLogout.cpp   # Character logout / party removal
├── _MSG_CreateCharacter.cpp   # Character creation (DB-forwarding)
├── _MSG_DeleteCharacter.cpp   # Character deletion (DB-forwarding)
├── _MSG_MessageChat.cpp       # Public chat + slash commands
├── _MSG_MessageWhisper.cpp    # Whisper + GM/guild commands (1,378 lines)
├── _MSG_Action.cpp            # Movement/action + anti-speed-hack (340 lines)
├── _MSG_Motion.cpp            # Animation/motion broadcast
├── _MSG_NoViewMob.cpp         # Mob visibility sync
├── _MSG_Restart.cpp           # Death restart / recall
├── _MSG_Deprivate.cpp         # Disguise removal
├── _MSG_ChangeCity.cpp        # City allegiance tracking
├── _MSG_ReqTeleport.cpp       # Paid teleport request
├── _MSG_Attack.cpp            # Combat/skill engine (1,789 lines)
├── _MSG_UseItem.cpp           # Item use engine (5,726 lines)
├── _MSG_Buy.cpp               # NPC purchase (313 lines)
├── _MSG_Sell.cpp              # NPC sale (236 lines)
├── _MSG_ReqShopList.cpp       # Shop list request
├── _MSG_ReqBuy.cpp            # Auto-trade purchase (200 lines)
├── _MSG_SendAutoTrade.cpp     # Auto-trade stall setup (120 lines)
├── _MSG_ReqTradeList.cpp      # Auto-trade browse
├── _MSG_Trade.cpp             # Player trade protocol (425 lines)
├── _MSG_TradingItem.cpp       # Inventory swap/stack (430 lines)
├── _MSG_QuitTrade.cpp         # Trade cancel
├── _MSG_DropItem.cpp          # Item drop
├── _MSG_GetItem.cpp           # Item pickup (252 lines)
├── _MSG_SplitItem.cpp         # Stack splitting
├── _MSG_DeleteItem.cpp        # Item deletion
├── _MSG_UpdateItem.cpp        # Gate/door opening
├── _MSG_ApplyBonus.cpp        # Attribute/special/skill allocation (250 lines)
├── _MSG_SetShortSkill.cpp     # Skill bar persistence
├── _MSG_AcceptParty.cpp       # Party accept (150 lines)
├── _MSG_RemoveParty.cpp       # Party removal
├── _MSG_SendReqParty.cpp      # Party invitation (100 lines)
├── _MSG_Challange.cpp         # City challenge/tax (135 lines)
├── _MSG_ChallangeConfirm.cpp  # Challenge confirmation
├── _MSG_ReqRanking.cpp        # Ranked duel (77 lines)
├── _MSG_PKMode.cpp            # PK mode toggle
├── _MSG_GuildAlly.cpp         # Guild alliance (DB-forwarding)
├── _MSG_War.cpp               # Guild war (DB-forwarding)
├── _MSG_InviteGuild.cpp       # Guild member invitation
├── _MSG_Deposit.cpp           # Bank deposit
├── _MSG_Withdraw.cpp          # Bank withdrawal
├── _MSG_CapsuleInfo.cpp       # Capsule/instance info (DB-forwarding)
├── _MSG_PutoutSeal.cpp        # Capsule/character seal extraction (DB-forwarding)
├── _MSG_ReqRanking.cpp        # Ranked duel
├── _MSG_CombineItem.cpp       # Ancient-item combining (144 lines)
├── _MSG_CombineItemEhre.cpp   # Ehre recipes (395 lines)
├── _MSG_CombineItemTiny.cpp   # Tiny recipes (141 lines)
├── _MSG_CombineItemShany.cpp  # Shany recipes
├── _MSG_CombineItemAilyn.cpp  # Ailyn recipes (152 lines)
├── _MSG_CombineItemAgatha.cpp # Agatha recipes (130 lines)
├── _MSG_CombineItemOdin.cpp   # Odin recipes (580 lines)
├── _MSG_CombineItemLindy.cpp  # Lindy recipes (122 lines)
├── _MSG_CombineItemAlquimia.cpp # Alchemy recipes
├── _MSG_CombineItemExtracao.cpp # Extraction recipes
└── _MSG_Quest.cpp             # Quest/NPC dispatch (2,712 lines)
```

**Shared supporting files (dependencies, not part of the component):**
```
legacy/Code/
├── Basedef.h                  # Message type constants (_MSG_*), limits, structs
├── CPSock.h / CPSock.cpp      # Socket abstraction (pUser[].cSock)
├── ItemEffect.h               # Item ability constants (EF_*)
└── TMSrv/
    ├── Server.h / Server.cpp  # World state, DBServerSocket, entry loop
    ├── CUser.h / CUser.cpp    # pUser[] user state
    ├── CMob.h / CMob.cpp      # pMob[] mob/player world state
    ├── CItem.h / CItem.cpp    # pItem[] item world state
    ├── GetFunc.h / GetFunc.cpp# Shared helper logic (DoTeleport, RemoveTrade, etc.)
    ├── SendFunc.h / SendFunc.cpp # Client message senders (SendScore, SendItem, ...)
    ├── CCastleZakum.*, CWarTower.*, CNPCGene.*, CReadFiles.*  # Domain subsystems
    └── ProcessDBMessage.cpp   # Handles DB->game messages (response side)
```

---

## 5. Dependency Analysis

### Internal Dependencies

```
ProcessClientMessage (dispatcher)
   ├── Exec_MSG_* (all 58 handlers)   -- via switch (ProcessClientMessage.cpp:66-311)
   └── ProcessClientMessage.h         -- declarations, includes shared headers

Each handler depends on:
   ├── ProcessClientMessage.h  (shared include: CItem, Server, GetFunc, SendFunc, CMob, CUser, ...)
   ├── pUser[] / pMob[] / pItem[] / grid arrays  (global world state in Server)
   ├── Helper layer (GetFunc.cpp / SendFunc.cpp):
   │     SendClientMessage (445 uses), SendItem (337), SendSay (95), RemoveTrade (87),
   │     SendClientSignalParm (80), SendScore (72), BASE_GetItemAbility (63), SendEtc (44),
   │     DoTeleport (42), AddCrackError (42), GridMulticast (31), SendHpMode (27), ...
   ├── CCastleZakum (gates), CReadFiles (guild persistence), CNPCGene (NPC lookup)
   └── DBServerSocket (DBSrv integration) -- 12 handlers forward _MSG_DB* messages
```

Representative chains:
```
_MSG_AccountLogin → CheckFailAccount → DBServerSocket.SendOneMessage → pUser[conn].Mode = USER_LOGIN
_MSG_Trade → RemoveTrade / BASE_CanTrade → memcpy(pMob[conn].MOB.Carry) → SaveUser
_MSG_AcceptParty → SendAddParty → pMob[leaderID].PartyList / pMob[myindex].Leader
_MSG_UpdateItem → CCastleZakum::OpenCastleGate → UpdateItem → GridMulticast
```

### External Dependencies
- **DBSrv (database server)** — same codebase (`legacy/Code/DBSrv/`), communicated via `DBServerSocket.SendOneMessage` over the internal socket; message contract is the `MSG_*` structs and `_MSG_DB*` type constants. Handled asynchronously; responses arrive via `ProcessDBMessage`.
- **Windows API** — all files include `<Windows.h>` and use WinSock (`CPSock`), the Win32 message pump in `Server.cpp`, and `rand()`/`time()` from the CRT. The project is a Windows-only Visual Studio build (`TMSrv.vcxproj`).
- **No third-party libraries** — no external game/network/DB SDKs; all logic is hand-rolled on raw sockets and in-memory structures.

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the function level (the unit of this C++ codebase). Afferent coupling (CA) = number of distinct callers/entry points reaching a handler; efferent coupling (CE) = number of distinct external helper functions/subsystems a handler calls. The dispatcher has the highest fan-out.

| Component (Handler) | Afferent Coupling | Efferent Coupling | Critical |
|---------------------|-------------------|-------------------|----------|
| ProcessClientMessage (dispatcher) | 1 (Server.cpp:4001) | 58 (all handlers) | High |
| Exec_MSG_UseItem | 1 (dispatcher) | ~30 (helpers, spells, items) | High |
| Exec_MSG_Quest | 1 (dispatcher) | ~25 (NPC/quest subsystems) | High |
| Exec_MSG_Attack | 1 (dispatcher) | ~28 (combat, spells, items) | High |
| Exec_MSG_MessageWhisper | 1 (dispatcher) | ~25 (GM/guild subsystems) | High |
| Exec_MSG_Trade | 1 (dispatcher) | ~20 | High |
| Exec_MSG_TradingItem | 1 (dispatcher) | ~18 | High |
| Exec_MSG_Action | 1 (dispatcher) | ~15 | High |
| Exec_MSG_Buy | 1 (dispatcher) | ~15 | High |
| Exec_MSG_ApplyBonus | 1 (dispatcher) | ~12 | Medium |
| Exec_MSG_CombineItemOdin | 1 (dispatcher) | ~12 | Medium |
| Exec_MSG_GetItem | 1 (dispatcher) | ~12 | Medium |
| Exec_MSG_Sell | 1 (dispatcher) | ~14 | Medium |
| Exec_MSG_CharacterLogin | 1 (dispatcher) | ~8 | Medium |
| Exec_MSG_AccountLogin | 1 (dispatcher) | ~7 | Medium |
| Exec_MSG_MessageChat | 1 (dispatcher) | ~9 | Medium |
| Exec_MSG_SendAutoTrade | 1 (dispatcher) | ~8 | Medium |
| Exec_MSG_ReqBuy | 1 (dispatcher) | ~9 | Medium |
| Exec_MSG_AcceptParty | 1 (dispatcher) | ~7 | Medium |
| Exec_MSG_CombineItem (base) | 1 (dispatcher) | ~7 | Medium |
| Exec_MSG_SetShortSkill | 1 (dispatcher) | 1 | Low |
| Exec_MSG_Deprivate | 1 (dispatcher) | 1 | Low |
| Exec_MSG_ChangeCity | 1 (dispatcher) | 2 | Low |
| Exec_MSG_CharacterLogout | 1 (dispatcher) | 2 | Low |
| Exec_MSG_AccountSecure | 1 (dispatcher) | 2 | Low |
| Exec_MSG_CapsuleInfo | 1 (dispatcher) | 2 | Low |

**Observations:**
- Afferent coupling is uniformly 1 for every handler (only the dispatcher calls them), which keeps handlers cohesive and independently replaceable.
- Efferent coupling varies widely: the large gameplay handlers (UseItem, Quest, Attack, Whisper) have high fan-out to shared helpers and domain subsystems, making them the most change-sensitive.
- The dispatcher is a high-fan-out bottleneck: any new message type or handler requires a new `case` in `ProcessClientMessage.cpp` plus a declaration in `ProcessClientMessage.h`.

---

## 7. Endpoints

This component does **not** expose REST/GraphQL/gRPC endpoints. It operates over a **custom binary protocol** on a raw TCP socket. Each "endpoint" is a message type (`_MSG_*`) dispatched by `ProcessClientMessage.cpp`. The 61 message types routed to the 58 handlers are listed below with their dispatch location and purpose.

| Message Type | Dispatcher Case | Handler | Purpose |
|--------------|-----------------|---------|---------|
| _MSG_AccountLogin | :68 | Exec_MSG_AccountLogin | Account login (DB-forward) |
| _MSG_CharacterLogin | :72 | Exec_MSG_CharacterLogin | Character login (DB-forward) |
| _MSG_CharacterLogout | :76 | Exec_MSG_CharacterLogout | Character logout |
| _MSG_DeleteCharacter | :80 | Exec_MSG_DeleteCharacter | Delete character (DB-forward) |
| _MSG_CreateCharacter | :84 | Exec_MSG_CreateCharacter | Create character (DB-forward) |
| _MSG_AccountSecure | :88 | Exec_MSG_AccountSecure | Account security (DB-forward) |
| _MSG_MessageChat | :92 | Exec_MSG_MessageChat | Public chat / commands |
| _MSG_Action / _MSG_Action2 / _MSG_Action3 | :96-99 | Exec_MSG_Action | Movement / stop / illusion |
| _MSG_Motion | :102 | Exec_MSG_Motion | Animation |
| _MSG_NoViewMob | :112 | Exec_MSG_NoViewMob | Mob visibility sync |
| _MSG_Restart | :116 | Exec_MSG_Restart | Death restart |
| _MSG_Deprivate | :120 | Exec_MSG_Deprivate | Remove disguise |
| _MSG_Challange | :124 | Exec_MSG_Challange | City challenge / tax |
| _MSG_ChallangeConfirm | :128 | Exec_MSG_ChallangeConfirm | Confirm challenge |
| _MSG_ReqTeleport | :132 | Exec_MSG_ReqTeleport | Paid teleport |
| _MSG_REQShopList | :136 | Exec_MSG_REQShopList | Shop list |
| _MSG_Deposit | :140 | Exec_MSG_Deposit | Bank deposit |
| _MSG_Withdraw | :144 | Exec_MSG_Withdraw | Bank withdraw |
| _MSG_RemoveParty | :148 | Exec_MSG_RemoveParty | Party removal |
| _MSG_SendReqParty | :152 | Exec_MSG_SendReqParty | Party invite |
| _MSG_AcceptParty | :156 | Exec_MSG_AcceptParty | Party accept |
| _MSG_TradingItem | :160 | Exec_MSG_TradingItem | Inventory swap |
| _MSG_MessageWhisper | :164 | Exec_MSG_MessageWhisper | Whisper / GM commands |
| _MSG_ChangeCity | :168 | Exec_MSG_ChangeCity | City allegiance |
| _MSG_PKMode | :172 | Exec_MSG_PKMode | PK mode toggle |
| _MSG_ReqTradeList | :176 | Exec_MSG_ReqTradeList | Auto-trade browse |
| _MSG_UpdateItem | :180 | Exec_MSG_UpdateItem | Gate/door open |
| _MSG_Quest | :184 | Exec_MSG_Quest | Quest / NPC interaction |
| _MSG_SetShortSkill | :188 | Exec_MSG_SetShortSkill | Skill bar |
| _MSG_Attack / _MSG_AttackOne / _MSG_AttackTwo | :192-195 | Exec_MSG_Attack | Combat / skills |
| _MSG_DropItem | :198 | Exec_MSG_DropItem | Drop item |
| _MSG_GetItem | :202 | Exec_MSG_GetItem | Pick up item |
| _MSG_QuitTrade | :206 | Exec_MSG_QuitTrade | Cancel trade |
| _MSG_UseItem | :210 | Exec_MSG_UseItem | Use item |
| _MSG_ApplyBonus | :214 | Exec_MSG_ApplyBonus | Allocate points/skills |
| _MSG_SendAutoTrade | :218 | Exec_MSG_SendAutoTrade | Open auto-trade stall |
| _MSG_ReqBuy | :222 | Exec_MSG_ReqBuy | Auto-trade purchase |
| _MSG_Buy | :226 | Exec_MSG_Buy | NPC purchase |
| _MSG_Sell | :230 | Exec_MSG_Sell | NPC sale |
| _MSG_Trade | :234 | Exec_MSG_Trade | Player trade |
| _MSG_CombineItem | :238 | Exec_MSG_CombineItem | Ancient combine |
| _MSG_ReqRanking | :242 | Exec_MSG_ReqRanking | Ranked duel |
| _MSG_CombineItemEhre | :246 | Exec_MSG_CombineItemEhre | Ehre recipes |
| _MSG_CombineItemTiny | :250 | Exec_MSG_CombineItemTiny | Tiny recipes |
| _MSG_CombineItemShany | :254 | Exec_MSG_CombineItemShany | Shany recipes |
| _MSG_CombineItemAilyn | :258 | Exec_MSG_CombineItemAilyn | Ailyn recipes |
| _MSG_CombineItemAgatha | :262 | Exec_MSG_CombineItemAgatha | Agatha recipes |
| _MSG_CombineItemOdin / _MSG_CombineItemOdin2 | :266-268 | Exec_MSG_CombineItemOdin | Odin recipes |
| _MSG_DeleteItem | :271 | Exec_MSG_DeleteItem | Delete item |
| _MSG_InviteGuild | :275 | Exec_MSG_InviteGuild | Invite to guild |
| _MSG_SplitItem | :279 | Exec_MSG_SplitItem | Split stack |
| _MSG_CombineItemLindy | :283 | Exec_MSG_CombineItemLindy | Lindy recipes |
| _MSG_CombineItemAlquimia | :287 | Exec_MSG_CombineItemAlquimia | Alchemy recipes |
| _MSG_CombineItemExtracao | :291 | Exec_MSG_CombineItemExtracao | Extraction recipes |
| _MSG_GuildAlly | :295 | Exec_MSG_GuildAlly | Guild alliance (DB-forward) |
| _MSG_War | :299 | Exec_MSG_War | Guild war (DB-forward) |
| _MSG_CapsuleInfo | :303 | Exec_MSG_CapsuleInfo | Capsule info (DB-forward) |
| _MSG_PutoutSeal | :307 | Exec_MSG_PutoutSeal | Seal extraction (DB-forward) |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv (database server) | Internal Service (same codebase) | Account/character persistence, guild/war records, capsule data | Custom binary socket (`DBServerSocket.SendOneMessage`) | `MSG_*` structs / `_MSG_DB*` types | Asynchronous; user mode transitioned to wait state; responses via ProcessDBMessage |
| Client (game) | External peer | All gameplay input/output | Raw TCP (WinSock via CPSock) | Binary `MSG_*` packets | Bounds/tick/version checks; `AddCrackError` penalties; `CloseUser` on hard failures |
| File system (CReadFiles) | Internal | Guild/tax persistence | File I/O | Custom text/guild records | `WriteGuild()` on tax change |
| World state (in-memory) | Internal | Shared gameplay state | Direct memory access | `pUser[]`, `pMob[]`, `pItem[]`, grids | Guarded by mode/HP/index checks |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Front Controller / Central Dispatcher | `ProcessClientMessage` switch | ProcessClientMessage.cpp:38-313 | Single entry point routing all client packets to handlers |
| Command Handler (one function per message type) | 58 `Exec_MSG_*` functions, one per `_MSG_*` type | `_MSG_*.cpp` | Isolates each business rule in its own unit |
| Message/Struct over shared memory (no DTO) | Handlers cast raw buffer to `MSG_*` struct and mutate globals | throughout | Zero-copy parsing against shared world state |
| Facade over socket | `pUser[conn].cSock` (CPSock) abstracts send/recv | CPSock.h | Decouples handlers from raw socket I/O |
| Helper library (procedural) | GetFunc / SendFunc shared functions | GetFunc.cpp, SendFunc.cpp | Centralizes reusable logic (teleport, trade removal, score send) |
| External system integration (data-forward proxy) | Handlers rewrite type to `_MSG_DB*` and forward | e.g. _MSG_AccountLogin.cpp:73-92 | Deferred persistence to DBSrv |
| Defensive validation / fail-fast guards | Mode/HP/index/tick checks at handler entry | throughout | Anti-cheat and state-integrity protection |

**Architectural notes:**
- The design is a **procedural, global-state, single-threaded** model typical of early-2000s MMORPG servers. There is no object-orientation for the message handlers; logic is free functions operating on global arrays.
- **High cohesion** per handler (each handles one message type completely), but the shared global mutable state creates **tight coupling across handlers** (e.g. trade and movement both mutate `pUser[conn].Trade` and `pMob[conn]`).
- **No abstraction layer** over the game domain; business rules are encoded directly in each handler with minimal reuse beyond the helper libraries. This is the primary source of the component's technical debt.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | All handlers | No automated tests anywhere in `legacy/` | Business-rule regressions undetected; high refactor/change risk |
| High | UseItem / Quest / Attack / Whisper | Extremely large monolithic functions (5,726 / 2,712 / 1,789 / 1,378 lines) | Hard to maintain, reason about, and test; high cognitive load |
| High | Trade / TradingItem / Buy / Sell | Magic item indices (508, 522, 747, 3993, 2390-2419, 4150-4188, etc.) hardcoded inline with no named constants | Brittle; item renumbering breaks rules silently |
| High | All coin operations | 2,000,000,000 cap enforced piecemeal via repeated `if (x > 2000000000)` guards | Inconsistent overflow protection; subtle money-dup/overflow exploits if one path is missed |
| Medium | ProcessClientMessage | Central switch must be edited for every new message/handler | Merge conflicts and high blast radius on changes |
| Medium | State machine | User modes (`USER_*`) and `pUser[conn].Unk_*`/`pMob[conn].extra` fields are numeric/untyped | Obscure logic; unreadable flags (e.g. `Unk_1816`, `Unk_2728`) |
| Medium | Anti-cheat | `AddCrackError`/`CrackLog` used with magic reason codes (e.g. `AddCrackError(conn, 10, 28)`) | Enforcement actions are opaque and inconsistent |
| Medium | Persistence | Many mutations (inventory, coin) are applied to memory and persisted only on `SaveUser`/DB-forward at specific points | Crash between mutation and save loses state |
| Medium | Guild/War/Challenge | Complex conditional logic (WeekMode, BillingMode) with many branches | Hard to verify correctness; likely contains unreachable paths |
| Low | Combine handlers | Ten near-duplicate handlers with slightly different matchers | Code duplication; fixes must be applied to each variant |
| Low | SetShortSkill / Deprivate / ChangeCity | Trivial handlers | None significant |

---

## 11. Test Coverage Analysis

**No automated tests exist for this component, or anywhere in the `legacy/` project tree.**

An exhaustive search of the repository (excluding `.git` and `.opencode`) for test files (`*test*`, `*spec*`, `.go`/`.py`/`.js`/`.ts` test sources) returned **zero results** in the `legacy/` codebase. The only matches found were inside `legacy/../.opencode/node_modules/` (an unrelated Node toolchain), which is excluded by the `ignore-folders` parameter.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| _MSG_AccountLogin | 0 | 0 | 0% (none) | N/A — no tests present |
| _MSG_CharacterLogin | 0 | 0 | 0% (none) | N/A |
| _MSG_MessageChat | 0 | 0 | 0% (none) | N/A |
| _MSG_MessageWhisper | 0 | 0 | 0% (none) | N/A |
| _MSG_Action | 0 | 0 | 0% (none) | N/A |
| _MSG_Attack | 0 | 0 | 0% (none) | N/A |
| _MSG_UseItem | 0 | 0 | 0% (none) | N/A |
| _MSG_Buy / _MSG_Sell | 0 | 0 | 0% (none) | N/A |
| _MSG_Trade / _MSG_TradingItem | 0 | 0 | 0% (none) | N/A |
| _MSG_AcceptParty / _MSG_SendReqParty | 0 | 0 | 0% (none) | N/A |
| _MSG_CombineItem* (10 handlers) | 0 | 0 | 0% (none) | N/A |
| _MSG_Quest | 0 | 0 | 0% (none) | N/A |
| _MSG_ApplyBonus | 0 | 0 | 0% (none) | N/A |
| _MSG_Deposit / _MSG_Withdraw | 0 | 0 | 0% (none) | N/A |
| _MSG_GuildAlly / _MSG_War / _MSG_InviteGuild | 0 | 0 | 0% (none) | N/A |
| All other handlers (29 remaining) | 0 | 0 | 0% (none) | N/A |

**Test locations:** None found. The only directories in the project are `legacy/Code/TMSrv`, `legacy/Code/DBSrv`, and the shared `legacy/Code/*` headers — no test directories, test projects, or test sources exist. The Windows-only nature (`TMSrv.vcxproj`, WinSock, `<Windows.h>`) and the tight coupling to global mutable state and the Win32 message pump make the handlers effectively untestable in isolation without substantial refactoring. **This is the most significant risk to the component.**

---

## 12. Report Metadata

- **Component analyzed:** _MSG_* Handlers (58 per-message business logic handlers in `legacy/Code/TMSrv/`)
- **Files analyzed:** 58 `_MSG_*.cpp` handler files + `ProcessClientMessage.cpp` (dispatcher) + `ProcessClientMessage.h` (declarations)
- **Total handler source lines:** 18,625
- **Code modified:** None (analysis and reporting only)
- **Report saved to:** `docs/reports/component-deep-analyzer/component-analysis-MSG-Handlers-2026-08-19 17:13:23.md`

---

*End of Component Deep Analysis Report*
