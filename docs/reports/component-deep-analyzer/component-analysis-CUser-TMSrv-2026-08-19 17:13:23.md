# Component Deep Analysis Report: CUser (TMSrv)

## 1. Executive Summary

**Component:** `CUser` (TMSrv) — per-player session state in the W2PP game server.
**Location:** `legacy/Code/TMSrv/CUser.h` (124 lines), `legacy/Code/TMSrv/CUser.cpp` (92 lines).

`CUser` is the authoritative in-memory representation of a single player's session within the `TMSrv` (the game/TM server) of the legacy W2PP codebase. It is a **plain data-holder class** (struct-like, all members `public`) with minimal behavior: a constructor, a destructor, `AcceptUser()` (accept a TCP connection), and `CloseUser()` (tear down a session). It holds no gameplay logic of its own; instead it is the **shared session context** that virtually every game subsystem reads and writes.

The class is instantiated as a fixed global array `CUser pUser[MAX_USER]` (`legacy/Code/TMSrv/Server.cpp:269`) where `MAX_USER = 1000` (`legacy/Code/Basedef.h:56`). The array index (`conn`) doubles as the player's connection/slot ID and is used to index the parallel `CMob pMob[MAX_MOB]` array that holds the actual character/mob gameplay data. This 1:1 coupling between `pUser[conn]` (session/transport state) and `pMob[conn]` (character state) is a central architectural convention.

**Key findings:**
- `CUser` is the **highest-fan-in component** in the TMSrv codebase: 63 source files reference `pUser` (1227 total references), spanning every message handler, the DB message processor, the periodic timer, and the network loop.
- It encodes the **player session state machine** through its `Mode` field with well-defined constants (`USER_EMPTY`, `USER_ACCEPT`, `USER_LOGIN`, `USER_SELCHAR`, `USER_CHARWAIT`, `USER_WAITDB`, `USER_PLAY`, `USER_SAVING4QUIT`).
- It carries a large number of **implicit/undocumented fields** (e.g. `Unk2[400]`, `Unk3`, `Unk_1498`, `Unk_1816`, `Unk5[36]`, `Unk_2628`, `Unk_2632`, `Unk_2648[24]`, `Unk_2688`, `Unk_2708`, `Unk_2728`, `Unk_2732`, `Unk_2736`, `Unk9[400]`) — commented as unknown by the original authors, representing significant reverse-engineering uncertainty.
- The component is central to **anti-cheat/anti-exploit** controls: anti-speedhack timestamps (`LastAttackTick`, `LastIllusionTick`, `LastActionTick`), an accumulated error/crack counter (`NumError`), a MAC-address-based anti-multi-login mechanism (`Mac[4]` → `OnlyTrade` mode), and idle-timeout tracking (`LastReceiveTime`).
- **No automated tests exist** anywhere in the project for this component or any other. This is a critical risk for a 46k-line server codebase.
- A distinct `CUser` exists in `DBSrv` (`legacy/Code/DBSrv/CUser.h/.cpp`) with a different, smaller field set (IP, Mode, cSock, Count, Level, Encode1/2, Name, DisableID, Year, YearDay) — the two are separate implementations sharing only the name and the `USER_EMPTY`/`USER_ACCEPT` mode convention.

---

## 2. Data Flow Analysis

The following traces how data moves through the `CUser` session object from connection establishment to session teardown.

```
1. Client opens TCP connection to TMSrv listen socket
2. WSA_ACCEPT event → GetEmptyUser() finds first pUser[i].Mode == USER_EMPTY
   (Server.cpp:4011-4020)
3. pUser[User].AcceptUser(ListenSocket.Sock) accepts socket, records peer IP,
   sets Mode = USER_ACCEPT, initializes CPSock buffers
   (CUser.cpp:51-79)
4. Client sends MSG_AccountLogin → checked against Mode == USER_ACCEPT,
   MAC copied to pUser[conn].Mac, AccountName normalized, forwarded to DBSrv,
   Mode = USER_LOGIN
   (_MSG_AccountLogin.cpp:56-95)
5. DBSrv responds → ProcessDBMessage sends STRUCT_SELCHAR → Mode = USER_SELCHAR
   (ProcessDBMessage.cpp:224, 400, 588, 604, 617, 630)
6. Client selects character → MSG_CharacterLogin → Mode = USER_CHARWAIT
   (_MSG_CharacterLogin.cpp:76)
7. DBSrv confirms char login → character loaded into pMob[conn], session fields
   initialized (ReqHp/ReqMp/cProgress/Trade.OpponentID), Mode = USER_PLAY
   (ProcessDBMessage.cpp:906-909, 979-981)
8. In-game: every client message dispatched through ProcessClientMessage is
   gated on pUser[conn].Mode == USER_PLAY (guard pattern throughout all _MSG_*.cpp)
9. Server loop keeps LastReceiveTime fresh on each packet
   (ProcessClientMessage.cpp:57)
10. Periodic ProcessSecMinTimer reads session state (billing, idle, Regen)
11. On logout / timeout / error → CharLogOut / SaveAndQuit:
    session persisted to DBSrv via MSG_SavingQuit, Mode = USER_SAVING4QUIT,
    then CloseUser() → socket closed, Mode = USER_EMPTY, slot freed
    (Server.cpp:6834-6926, CUser.cpp:81-92)
```

**Entry points:** network receive path in `Server.cpp` `WSA_READ` handler; DB response path in `ProcessDBMessage.cpp`; timer path in `ProcessSecMinTimer.cpp`.

**Exit points:** `CloseUser()` (session teardown), `SaveAndQuit`/`CharLogOut` (persistence), and the various `Send*` functions that format outbound packets from session fields.

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Session/State | Session mode state machine gating all gameplay | CUser.h:26-37, CUser.cpp:27,75,87 |
| Capacity | Slot allocation from `USER_EMPTY` slots in `pUser[1..MAX_USER-1]` | Server.cpp:6743-6752 |
| Capacity | Reserve top 10 slots (`ADMIN_RESERV`) for admins | Basedef.h:57, Server.cpp:4022-4030 |
| Session/Timeout | Idle disconnect after ~720 seconds with no packets | Server.cpp:4304-4322 |
| Anti-cheat | Attack speed limit of 800ms between client ticks | _MSG_Attack.cpp:59-75 |
| Anti-cheat | Illusion/move speed limit of 900ms | _MSG_Action.cpp:92-133 |
| Anti-cheat | Accumulated crack/error counter; logout at 2,000,000,000 | Server.cpp:685-707 |
| Anti-cheat | MAC-based multi-login detection → `OnlyTrade` restricted mode | ProcessDBMessage.cpp:148-182 |
| Restriction | `OnlyTrade` blocks attack/action/party/whisper; trade-only | multiple _MSG_*.cpp |
| Combat/Stats | Authoritative ReqHp/ReqMp clamping and synchronization | Server.cpp:8659-8687 |
| Billing | `IsBillConnect` drives billing disconnect signaling | Server.cpp:6845-6846 |
| Chat | Per-channel chat toggles (Whisper/Guild/Party/King/Chatting) + MuteChat | _MSG_MessageChat.cpp:113-163 |
| Economy | Shared account Cargo (128 slots), Coin, Donate balance | CUser.h:50-51,105 |
| Trading | Trade session state and TradeMode gating | CUser.h:46,59, _MSG_Trade.cpp |
| Trading | AutoTrade (player shop) session state | CUser.h:65, _MSG_SendAutoTrade.cpp:105 |
| Duel/Ranking | Duel/ranking request target and type | _MSG_ReqRanking.cpp:40-41 |
| Rate-limit | UseItemTime/AttackTime/PotionTime/LastClientTick throttles | CUser.h:109-113 |
| Server control | Reject new connects when server shutting down / at capacity | Server.cpp:4032-4039 |

---

## Detailed breakdown of the business rules

### Business Rule: Session Mode State Machine

**Overview:**
`CUser.Mode` is the single most important field in the component. It encodes the current stage of a player's session lifecycle and is used as a universal gate: message handlers, the DB processor, the timer, and the network loop all inspect `pUser[conn].Mode` to decide whether an operation is legal for the connection. There are three logical families of modes.

**Detailed description:**
The state machine has three families. The *connecting* family contains `USER_EMPTY (0)` meaning no user occupies the slot, `USER_ACCEPT (1)` meaning a TCP socket has just been accepted but no account login has been processed, and `USER_LOGIN (2)` meaning the account login request has been forwarded to the DB server and the server is awaiting the account response. The *character-select* family contains `USER_SELCHAR (11)` — waiting for the DB to send the `STRUCT_SELCHAR` character-selection data, `USER_CHARWAIT (12)` — waiting for DB confirmation of character login, and `USER_WAITDB (13)` — waiting for a DB response to confirm (used by character creation/deletion). The *ingame* family contains `USER_PLAY (22)` — the player is actively playing a character, and `USER_SAVING4QUIT (24)` — the game is saving right before quitting. A commented-out constant `USER_CREWAIT (14)` indicates a former character-creation waiting state that was removed.

The state machine is enforced at every handler boundary. For example `_MSG_AccountLogin.cpp:56` rejects any account login unless `Mode == USER_ACCEPT`; `_MSG_CreateCharacter.cpp:27` requires `Mode == USER_SELCHAR`; and the near-universal `pUser[conn].Mode != USER_PLAY` guard at the top of in-game handlers (e.g. `_MSG_UseItem.cpp:45`, `_MSG_Attack.cpp`, `_MSG_Trade.cpp:24`) prevents a client from issuing gameplay commands before login completes. This is the primary anti-sequence-bypass control in the server.

**Rule workflow:**
1. Constructor sets `Mode = USER_EMPTY` (CUser.cpp:27).
2. `AcceptUser` sets `Mode = USER_ACCEPT` after accepting the socket (CUser.cpp:75).
3. Account login sets `Mode = USER_LOGIN` (AccountLogin.cpp:94).
4. DB `SELCHAR` response sets `Mode = USER_SELCHAR` (ProcessDBMessage.cpp:224 et al.).
5. Character login sets `Mode = USER_CHARWAIT` (CharacterLogin.cpp:76).
6. DB character confirmation sets `Mode = USER_PLAY` (ProcessDBMessage.cpp:906).
7. Logout sets `Mode = USER_SAVING4QUIT` (Server.cpp:6917).
8. `CloseUser` sets `Mode = USER_EMPTY`, freeing the slot (CUser.cpp:87).

---

### Business Rule: Slot Allocation and Capacity Enforcement

**Overview:**
The server supports a hard cap of `MAX_USER = 1000` concurrent connections via the fixed array `CUser pUser[MAX_USER]`. Slots are allocated by scanning for the first free slot whose `Mode == USER_EMPTY`.

**Detailed description:**
`GetEmptyUser()` (Server.cpp:6743-6752) iterates `i = 1 .. MAX_USER-1` and returns the first index whose `pUser[i].Mode == USER_EMPTY`, or 0 if the server is full. Slot 0 is reserved and never allocated (iterations begin at 1), matching the convention that `conn == 0` means "no user". The returned index becomes the `conn` handle used for the life of the session. When no slot is free, `GetEmptyUser` returns 0 and the accept path logs `"err,accept fail - no empty"` and drops the connection (Server.cpp:4013-4018). This bounds server memory and CPU per player and is the capacity limit of the game server. Because the same `conn` indexes both `pUser` and `pMob`, capacity is shared with the mob array's user region (`MAX_USER` is also "the starting index of npcs and mobs", Basedef.h:56).

**Rule workflow:**
1. On `WSA_ACCEPT`, call `GetEmptyUser()`.
2. If 0, log failure and reject the connection.
3. Otherwise call `pUser[User].AcceptUser(...)` to bind the slot.
4. On session close, `CloseUser()` resets the slot to `USER_EMPTY`, making it reusable.

---

### Business Rule: Admin Reserved Slots

**Overview:**
The top `ADMIN_RESERV = 10` slots (indices `MAX_USER - 10` through `MAX_USER - 1`, i.e. 990-999) are reserved so that administrators can always connect even when the server is full.

**Detailed description:**
When a new user is accepted, the server checks `if (User >= MAX_USER - ADMIN_RESERV)` (Server.cpp:4022). If the allocated slot falls in the reserved admin range, the server sends the client a "Reconnect" message, flushes it, and immediately closes the user (`CloseUser`). This logic appears intended to reserve the high slots for admin connections; however, the code as written rejects *any* connection that lands in the reserved range rather than routing only admins there, so the reservation behavior is only partially realized. The `Admin` field on `CUser` (CUser.h:98) is set to 1 when a character reaches level 999 (ProcessDBMessage.cpp:1004-1005) and cleared to 0 on close (Server.cpp:6843) and on login (ProcessDBMessage.cpp:391). Admin IPs are separately tracked in `pAdminIP[MAX_ADMIN]` loaded from `Admin.txt` (CReadFiles.cpp:640-672).

**Rule workflow:**
1. Accept a connection into some slot.
2. If slot index `>= MAX_USER - ADMIN_RESERV`, notify client to reconnect and close the session.
3. Normal connections proceed in the non-reserved range.

---

### Business Rule: Idle Timeout / Disconnect

**Overview:**
Connections that stop sending packets for an extended period are automatically disconnected to reclaim server resources and prevent zombie/half-open sessions.

**Detailed description:**
`CheckIdle(int conn)` (Server.cpp:4304-4322) compares `pUser[conn].LastReceiveTime` (refreshed on every incoming packet at ProcessClientMessage.cpp:57) against the current `SecCounter`. If `LastReceiveTime` is older than 720 seconds, the server logs a `"sys,disconnect"` message and calls `CloseUser(conn)`. The function also normalizes the timestamp if it is in the future (`lst > ser`) or older than 1440 seconds, resetting it to the current counter to guard against clock-skew anomalies. The 720-second threshold (12 minutes) represents the server's idle-session policy.

**Rule workflow:**
1. Every packet updates `pUser[conn].LastReceiveTime`.
2. `CheckIdle` is invoked (periodically) per connection.
3. If the connection is idle for more than 720s, `CloseUser` is called and the slot is reclaimed.

---

### Business Rule: Anti-Speedhack Attack/Illusion Cooldowns

**Overview:**
The server validates client-supplied action timestamps to detect and reject speedhacking. Two hard limits are enforced: 800ms between attacks and 900ms between illusion/movement actions.

**Detailed description:**
In `_MSG_Attack.cpp:59-75`, the server stores the client tick in `pUser[conn].LastAttackTick`. If the incoming `ClientTick` is earlier than the previous attack tick plus 800ms (and neither is the sentinel `SKIPCHECKTICK = 235543242`, Basedef.h:172), the server logs `"err,attack ... 800ms limit"` and rejects the attack. Similarly, in `_MSG_Action.cpp:92-133`, movement/illusion actions are limited to one per 900ms via `LastIllusionTick`, logging `"err,illusion ... 900ms limit"`. The sentinel `SKIPCHECKTICK` bypasses these checks for server-generated or special messages. These timestamps are initialized to `SKIPCHECKTICK` at character login (ProcessDBMessage.cpp:825-828) so that the first action after login is not spuriously rejected. These controls constitute the primary server-side speedhack mitigation.

**Rule workflow:**
1. On login, initialize `LastAttackTick`/`LastIllusionTick`/`LastActionTick` to `SKIPCHECKTICK`.
2. On attack, if `ClientTick < LastAttackTick + 800` and tick is not sentinel, reject.
3. Otherwise update `LastAttackTick` and process.
4. On illusion/move, apply the same pattern with a 900ms threshold.

---

### Business Rule: Crack / Error Accumulation and Forced Logout

**Overview:**
The server maintains a cumulative "error" score per user, `NumError`, which is incremented when the client sends malformed or prohibited packets; crossing a hard threshold forces the player offline.

**Detailed description:**
`AddCrackError`-style accumulation is implemented in `Server.cpp:685-707`: for certain error types it adds `val` to `pUser[conn].NumError`. When `NumError >= 2,000,000,000`, the server sends the client a "Bad Network Packets" message, forces `CharLogOut(conn)`, and logs `"cra char logout type: %d"`. The counter is reset to 0 at various legitimate points (character login at ProcessDBMessage.cpp:822, on restart at _MSG_Restart.cpp:28, on successful gameplay events in _MSG_Attack.cpp:1039/1249/1259, and periodically at Server.cpp:8706). The very high threshold indicates it is a "last resort" tripwire rather than a strict policy, and the reset points make it a rolling rather than permanent accumulator.

**Rule workflow:**
1. A protocol violation increments `pUser[conn].NumError`.
2. Legitimate events reset the counter.
3. If the counter reaches 2,000,000,000, the player is logged out.

---

### Business Rule: MAC-Based Multi-Login Detection and OnlyTrade Restriction

**Overview:**
To prevent a single machine from running multiple accounts simultaneously, the server tracks each user's MAC address and, when it detects the same MAC on multiple accounts, demotes the offending accounts to `OnlyTrade` mode (restricted to trading only).

**Detailed description:**
`_MSG_DBCheckPrimaryAccount` (ProcessDBMessage.cpp:148-182) iterates all non-empty users and compares `pUser[i].Mac` with the incoming MAC. If the same MAC belongs to the same account name, `pUser[i].OnlyTrade = 0` (the legitimate account is unrestricted). Otherwise, the secondary account is set `OnlyTrade = 1`, removed from its party, and, if it is in play outside a safe village, recalled to the village with a "OnlyVillage" message. `Mac[4]` is populated at account login from the client's adapter name (AccountLogin.cpp:65-70). The `OnlyTrade` flag then blocks the restricted account from attacking (`_MSG_Attack.cpp:47`), acting (`_MSG_Action.cpp:198`), joining/leading parties (`_MSG_AcceptParty.cpp:71,77`, `_MSG_SendReqParty.cpp:56`), and whispering (`_MSG_MessageWhisper.cpp:744,810`).

**Rule workflow:**
1. Client MAC captured at account login into `pUser[conn].Mac`.
2. DB primary-account check compares MACs across sessions.
3. Matching-MAC secondary accounts are set `OnlyTrade = 1` and recalled to a village.
4. `OnlyTrade` blocks combat, action, party, and whisper messages.

---

### Business Rule: Authoritative ReqHp / ReqMp HP-MP Management

**Overview:**
The server maintains its own authoritative HP/MP values (`ReqHp`/`ReqMp`) distinct from the client's view, to reconcile damage, healing, and stat changes and prevent client-side tampering.

**Detailed description:**
`ReqHp`/`ReqMp` (CUser.h:90-91) are the server's requested/authoritative hit points and mana. They are initialized from the character's current HP/MP at login (ProcessDBMessage.cpp:980-981). `SetReqHp`/`SetReqMp` (Server.cpp:8659-8687) clamp the value to non-negative and enforce that it is never below the actual `pMob[conn].MOB.CurrentScore` HP/MP, and clamp the underlying score to its max. Damage subtracts from `ReqHp`/`ReqMp` (e.g. _MSG_Attack.cpp:1624, ProcessSecMinTimer.cpp:1859-1930), healing caps at `MaxHp`/`MaxMp` (_MSG_UseItem.cpp:116-134), and the synchronized values are sent to the client via `SendScore`/`SendHpMode` (SendFunc.cpp:1611-1615). This dual-bookkeeping ensures the client's reported HP/MP cannot diverge arbitrarily from server truth.

**Rule workflow:**
1. Initialize `ReqHp`/`ReqMp` from character HP/MP at login.
2. On damage/heal/stat change, update and clamp via `SetReqHp`/`SetReqMp`.
3. Send the authoritative values to the client.

---

### Business Rule: Billing Connection Handling

**Overview:**
`IsBillConnect` records whether the session is participating in the external billing system, so that on disconnect the server can notify the billing service.

**Detailed description:**
`IsBillConnect` (CUser.h:79) is initialized to 0 in the constructor (CUser.cpp:29). When a user is closed, if `pUser[conn].IsBillConnect` is true, the server sends a billing message via `SendBilling(conn, pUser[conn].AccountName, 2, 0)` (Server.cpp:6845-6846) before closing the socket. This couples session teardown to the external billing server, ensuring paid-session accounting is notified of disconnection. The related fields `Unk_2728`/`Unk_2732` are commented "Related to BILLING" (CUser.h:99-100) and are checked in billing-gated login logic (e.g. CharacterLogin.cpp:69, ProcessSecMinTimer.cpp:178) where `BILLING == 2` enables billing-restricted free-play windows.

**Rule workflow:**
1. Billing-enabled sessions set `IsBillConnect`.
2. On session close, `SendBilling` notifies the billing server.
3. Billing fields gate free-play eligibility checks.

---

### Business Rule: Chat Channel Toggles and Muting

**Overview:**
Each player has independent on/off toggles for chat channels (whisper, guild, party, king, and general) plus a server-side mute flag that silences the player entirely.

**Detailed description:**
`_MSG_MessageChat.cpp:113-163` toggles `pUser[conn].Whisper`, `PartyChat`, `KingChat`, `Guildchat`, and `Chatting` by flipping each boolean (`x = x == 0 ? 1 : 0`) and echoing the resulting state to the player. These toggles are consulted during message routing: e.g. `SendFunc.cpp:278` skips players whose `KingChat` is enabled, `SendFunc.cpp:1016/1042` skip party members with `PartyChat` set, `_MSG_MessageWhisper.cpp:1169` skips users with `Guildchat` set, and `_MSG_MessageWhisper.cpp:1216` skips `PartyChat` users. `MuteChat` (CUser.h:106) is a server-imposed mute (set via admin commands in imple.cpp:1527-1557) that blocks chat output (`_MSG_MessageChat.cpp:181`, `_MSG_MessageWhisper.cpp:100,1129`). All toggles are reset to 0 at character login (ProcessDBMessage.cpp:387-392).

**Rule workflow:**
1. Player issues a channel toggle command.
2. The corresponding boolean is flipped and echoed back.
3. Message-broadcast routines consult these flags to filter recipients.
4. `MuteChat` overrides and blocks all chat when set by an admin.

---

### Business Rule: Shared Account Cargo, Coin, and Donate Balances

**Overview:**
Session state carries the account-level shared storage (`Cargo`, 128 slots), the coin balance, and the donate currency, used across trading, banking, and the donation shop.

**Detailed description:**
`Cargo[MAX_CARGO]` (CUser.h:50, `MAX_CARGO = 128`, Basedef.h:77) is the account-shared warehouse (storage), distinct from the character's `Carry`/`Equip` which live on `pMob`. It is zero-initialized in the constructor (CUser.cpp:32) and used by storage deposit/withdraw (`_MSG_Deposit.cpp`, `_MSG_Withdraw.cpp`), trading (`_MSG_TradingItem.cpp:76-397` uses `Cargo[MAX_CARGO-2]`/`Cargo[MAX_CARGO-1]` as special storage slots), and the item-swap grid (`BASE_CanCargo`, Basedef.cpp). `Coin` (CUser.h:51) is the player's money, persisted on save (Server.cpp:6908). `Donate` (CUser.h:105) is a premium/donation currency spent in the donation shop (`_MSG_Buy.cpp:77-98` checks `Donate` balance before deducting) and credited from the DB (ProcessDBMessage.cpp:728,1240). These account-level balances are saved to the DB on logout (`MSG_SavingQuit`, Server.cpp:6908-6909).

**Rule workflow:**
1. On login, `Cargo`/`Coin`/`Donate` are available as session state.
2. Storage/deposit/withdraw and trade mutate `Cargo` and `Coin`.
3. Donation shop spends `Donate` after a balance check.
4. On logout/save, balances are serialized into `MSG_SavingQuit` and persisted.

---

### Business Rule: Trade Session State and TradeMode Gating

**Overview:**
Player-to-player trading is driven by the `Trade` struct and the `TradeMode`/`Trade.OpponentID` fields held on each session.

**Detailed description:**
`MSG_Trade Trade` (CUser.h:59) and `int TradeMode` (CUser.h:46) hold the trade negotiation state (items offered, `TradeMoney`, `MyCheck`, `OpponentID`). `Trade.OpponentID` identifies the peer and is the anchor for `RemoveTrade`/`RemoveParty` cleanup. Many handlers reject actions while a trade is active: `_MSG_UseItem.cpp:58`, `_MSG_Action.cpp:46`, `_MSG_TradingItem.cpp:38`, `_MSG_GetItem.cpp:38`, `_MSG_Sell.cpp:38`, `_MSG_ReqBuy.cpp:32`, `_MSG_SendAutoTrade.cpp:39` all gate on `pUser[conn].TradeMode`. `_MSG_PKMode.cpp:26-33` forbids toggling PK mode mid-trade and forcibly removes the trade. On login the trade state is reset (ProcessDBMessage.cpp:839-842, Server.cpp:7324-7326). This ensures a player cannot perform other inventory/combat actions while locked into a trade negotiation.

**Rule workflow:**
1. Player initiates trade → `TradeMode` set, `Trade.OpponentID` recorded.
2. During trade, conflicting actions are rejected by `TradeMode` guards.
3. On completion/cancel, `RemoveTrade` clears state and `TradeMode`.
4. On logout, trade is torn down (Server.cpp:6873-6878).

---

### Business Rule: AutoTrade (Player Shop) Session State

**Overview:**
`AutoTrade` holds the configuration of a player-run shop (title, up to 12 items with prices and a tax) that other players can browse and buy from.

**Detailed description:**
`MSG_SendAutoTrade AutoTrade` (CUser.h:65) stores the shop's `Title` (24 chars), up to `MAX_AUTOTRADE = 12` `STRUCT_ITEM`s with per-item `Coin` prices, `CarryPos` positions, and a `Tax`. `_MSG_SendAutoTrade.cpp` validates and copies the client's shop config into `pUser[conn].AutoTrade` (line 103-107) and sets `TradeMode = 1` (line 105) while the shop is active. Other players browse via `_MSG_ReqTradeList.cpp` and buy via `_MSG_ReqBuy.cpp`, which reads `AutoTrade` (GetFunc.cpp:1167). The shop state is reset on login (ProcessDBMessage.cpp:842, Server.cpp:7329). This enables the player-vendor economy.

**Rule workflow:**
1. Player configures a shop and sets `AutoTrade` + `TradeMode`.
2. Buyers request the trade list and purchase items with price/tax validation.
3. Shop state is reset at login.

---

### Business Rule: Duel / Ranking Request State

**Overview:**
`RankingTarget` and `RankingType` record a pending duel/ranking request between two players so the challenged party can confirm against the challenger's identity.

**Detailed description:**
`_MSG_ReqRanking.cpp` uses `pUser[conn].RankingTarget` and `pUser[conn].RankingType` to stage a ranking/duel challenge. The requester records the target and type (lines 40-41) and notifies the target. When the target responds, the server validates that `pUser[tDuel].RankingTarget == conn` (line 56) and that the stored `RankingType` is valid (line 66) before executing `DoRanking`. This confirms both parties agreed on the same challenge parameters and prevents a third party from spoofing a challenge. `pUser[tDuel].Whisper` is also consulted (line 30) as a signal of availability.

**Rule workflow:**
1. Challenger sets `RankingTarget`/`RankingType` and notifies target.
2. Target confirms.
3. Server validates the recorded target/type against the confirm sender.
4. Executes the duel/ranking.

---

### Business Rule: Per-Session Rate Limiting Timers

**Overview:**
Several unsigned timestamp fields throttle high-frequency in-game actions (item use, potions, attacks) and track client tick health.

**Detailed description:**
`UseItemTime`, `Message`, `AttackTime`, `LastClientTick`, and `PotionTime` (CUser.h:109-113) are initialized to 0 in the constructor (CUser.cpp:37-41). They are used to space out actions and detect abnormal client tick sequences. `LastClientTick` tracks the last client-supplied tick and, combined with the anti-speedhack fields, feeds the tick-validity checks. These timers implement the server's action-rate governance in conjunction with the fixed 800ms/900ms limits described earlier. The exact threshold semantics for several of these fields are implicit (the original code comments are `?`), so this rule is documented with medium confidence.

**Rule workflow:**
1. Timers initialized at construction.
2. Actions update the relevant timestamp and are rejected if they occur too soon.
3. Client tick health feeds anti-speedhack checks.

---

### Business Rule: Server Shutdown / Capacity Rejection

**Overview:**
When the server is shutting down (`ServerDown != -1000`) it refuses new connections after acceptance.

**Detailed description:**
After `AcceptUser`, the accept path checks `if (ServerDown != -1000)` (Server.cpp:4032). If the server is not in the normal running state, it sends the client a `"ServerReboot_Cant_Connect"` message and immediately closes the connection. This graceful-drain behavior prevents players from joining during a shutdown window. The same accept block also gates on the admin-reserved slot range before this check.

**Rule workflow:**
1. Accept a socket into a free slot.
2. If slot is admin-reserved, notify and close.
3. If `ServerDown != -1000`, notify "server rebooting" and close.
4. Otherwise the connection proceeds to the login sequence.

---

## 4. Component Structure

```
legacy/Code/TMSrv/
├── CUser.h                  # Class definition, mode constants, all session fields (124 lines)
├── CUser.cpp                # Constructor, destructor, AcceptUser, CloseUser (92 lines)
│
│   # Primary consumers (depend on CUser via global pUser[MAX_USER]):
├── Server.cpp               # Network loop, accept/close, idle, shutdown, GetEmptyUser (9449 lines)
├── ProcessClientMessage.cpp # Inbound client message dispatcher, mode gating (313 lines)
├── ProcessDBMessage.cpp     # DB response handling, session init/login/onlytrade (1330 lines)
├── ProcessSecMinTimer.cpp   # Periodic session maintenance, regen, billing (2314 lines)
├── GetFunc.cpp              # Lookup/query helpers over pUser/pMob (2160 lines)
├── SendFunc.cpp             # Outbound packet builders reading session state (1817 lines)
├── MobKilled.cpp            # Combat rewards, party/exp distribution (2469 lines)
├── imple.cpp                # Admin/console commands mutating session state (1846 lines)
├── CItem.cpp / CMob.cpp / CNPCGene.cpp / CCastleZakum.cpp / CWarTower.cpp
├── _MSG_*.cpp (58 files)    # Per-message handlers, nearly all gate on pUser[conn].Mode
│
└── Shared dependencies:
    legacy/Code/Basedef.h / Basedef.cpp   # STRUCT_ITEM, STRUCT_SELCHAR, MSG_Trade,
                                          #   MSG_SendAutoTrade, constants (MAX_USER, etc.)
    legacy/Code/CPSock.h / CPSock.cpp     # cSock network transport
    legacy/Code/ItemEffect.h              # item effect handling
```

**Annotations on `CUser.h`:**
- Connection/identity: `AccountName[16]`, `IP`, `Mac[4]`, `Slot`.
- Transport: `CPSock cSock` (socket + buffers).
- Session state: `Mode` (state machine), `TradeMode`, `Admin`.
- Character-select data: `SelChar` (`STRUCT_SELCHAR`, 4 characters' summaries).
- Account storage/economy: `Cargo[128]`, `Coin`, `Donate`.
- Trading: `Trade`, `AutoTrade`.
- Combat/stat sync: `ReqHp`, `ReqMp`, `cProgress`, `LastAttack`, `LastAttackTick`.
- Anti-cheat/cooldown: `LastMove`, `LastAction`, `LastActionTick`, `LastIllusionTick`, `LastClientTick`, `NumError`, `UseItemTime`, `AttackTime`, `PotionTime`.
- Chat: `Whisper`, `Guildchat`, `PartyChat`, `Chatting`, `KingChat`, `MuteChat`, `LastChat[16]`.
- Billing: `IsBillConnect`, `Unk_2728`, `Unk_2732`.
- Misc: `RankingTarget`, `RankingType`, `CastleStatus`, `CharShortSkill[16]`, `OnlyTrade`, `Range`, and many `Unk*` reserved fields.

---

## 5. Dependency Analysis

```
Internal Dependencies:
CUser.h → Basedef.h          (STRUCT_ITEM, STRUCT_SELCHAR, MSG_Trade, MSG_SendAutoTrade,
                               MAX_CARGO, ACCOUNTNAME_LENGTH, MAX_USER)
CUser.h → CPSock.h           (CPSock cSock; WSA_* message constants)
CUser.cpp → Windows.h        (WinSock accept, SOCKADDR_IN, inet_ntoa)
CUser.cpp → Server.h         (hWndMain used by WSAAsyncSelect)

Consumers (compile + runtime):
pUser[conn] ← Server.cpp          (accept/close/idle/network loop)
pUser[conn] ← ProcessClientMessage.cpp + all _MSG_*.cpp   (message handlers)
pUser[conn] ← ProcessDBMessage.cpp  (DB responses, login, onlytrade)
pUser[conn] ← ProcessSecMinTimer.cpp (periodic maintenance)
pUser[conn] ← SendFunc.cpp / GetFunc.cpp / MobKilled.cpp / imple.cpp
pUser[conn] ← CMob.cpp / CCastleZakum.cpp  (parallel pMob indexing)

External Dependencies:
- WinSock2 / Windows.h  — TCP sockets for client and DB connections
- DBSrv (sibling project)  — account/character persistence via TCP messages
- Billing server  — external billing notifications (SendBilling)
- No third-party libraries; pure Win32 + standard C++ (legacy C-style)
```

**Note:** `CUser` is not instantiated dynamically; it exists only as the global `pUser[MAX_USER]` array. Its lifetime spans the entire server process.

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the class level (`CUser`) against the rest of the TMSrv translation units that reference the global `pUser` array.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| CUser (class) | 63 files / 1227 refs | 5 (CPSock, Basedef, Server globals, Win32, DBSrv) | High |
| CPSock (cSock) | ~63 | 3 (Win32, Server, Basedef) | High |
| STRUCT_SELCHAR | ~10 | 2 (STRUCT_ITEM, STRUCT_SCORE) | Medium |
| MSG_Trade | ~6 | 1 (STRUCT_ITEM) | Medium |
| MSG_SendAutoTrade | ~6 | 1 (STRUCT_ITEM) | Medium |

**Interpretation:** `CUser` exhibits **extremely high afferent coupling** (63 dependents) — nearly every subsystem reads or writes session state. Its **efferent coupling is low** (it depends only on `CPSock`, `Basedef` types, and a few `Server.h` globals). This makes `CUser` a classic "god data structure": a shared mutable context that is architecturally central but a potential bottleneck for change (any field layout change ripples across 60+ files, and the hand-written memory offsets documented in comments must be kept consistent).

---

## 7. Endpoints

`CUser` itself does not expose network endpoints. It is a per-player session data structure; endpoints belong to the `TMSrv` process and the individual `_MSG_*` handlers that read/write `pUser`. The component is therefore not listed with its own REST/GraphQL/gRPC endpoints. Its role in the protocol layer is as the session context that the TMSrv message endpoints operate upon (see section 8 for the integration picture).

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Client connections | External client | Gameplay sessions | TCP (WinSock, WSAAsyncSelect) | Binary `_MSG_*` packets | CloseUser on read error (Server.cpp:3940-3967) |
| DBSrv | Internal service | Account/character persistence, login, save | TCP (DBServerSocket) | Binary DB messages (MSG_AccountLogin→_MSG_DBAccountLogin, MSG_SavingQuit) | DBNoNeedSave sent when session gone (ProcessDBMessage.cpp:190-199) |
| Billing server | External service | Paid-session accounting | TCP (SendBilling) | Binary billing messages | SendBilling on disconnect if IsBillConnect (Server.cpp:6845-6846) |
| Admin console | Internal | Server administration | Command channel | Text/console commands | Mutates session state (imple.cpp:1524-1557) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Data Transfer / Plain Data Holder | `CUser` as a public-field struct-like class | CUser.h | Encapsulate per-session state without behavior |
| Global State / Registry | `CUser pUser[MAX_USER]` global array | Server.cpp:269 | Provide fixed-capacity indexed access to all sessions |
| State Machine | `Mode` field with documented constants | CUser.h:26-37 | Model session lifecycle (connecting→select→play→quit) |
| Parallel Arrays (struct of arrays) | `pUser[conn]` + `pMob[conn]` + `pItem[i]` | Server.cpp:269-279 | Indexed by a shared `conn` handle |
| Guard Clause / Mode Gating | `pUser[conn].Mode != USER_PLAY` checks | throughout `_MSG_*.cpp` | Reject operations in wrong session state |
| Façade (thin) | `AcceptUser`/`CloseUser` wrapping WinSock calls | CUser.cpp | Hide socket accept/close behind session API |
| Magic Sentinel | `SKIPCHECKTICK = 235543242` | Basedef.h:172 | Bypass tick/cooldown checks for special messages |

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Field layout | Hand-written memory offsets in comments (e.g. `CUser.h:42-104`) with many `Unk*` reserved fields | Any layout change risks desynchronization; unknown fields may be ABI-critical |
| High | No test coverage | Zero automated tests across the project | Regression risk on session/save/trade logic is very high |
| High | Global mutable array | `pUser` is a global shared across 63 files with no encapsulation | Concurrency/ordering bugs; hard to reason about invariants |
| Medium | Admin slot reservation | `Server.cpp:4022` rejects all connections landing in reserved range rather than admitting only admins | Admins may be unable to connect when full; behavior contradicts intent |
| Medium | Implicit business rules | Cooldowns (800ms/900ms), idle timeout (720s), crack threshold (2B) are magic numbers scattered in code | Tight coupling to tuning; not configurable or documented centrally |
| Medium | Security | `_MSG_UpdateScore` logged as crack attempt (ProcessClientMessage.cpp:106-110); client-supplied ticks validated against sentinels | Speedhack/cheat vectors depend on scattered manual checks |
| Medium | Error handling | `CloseUser` path has many branches; partial cleanup across party/trade/billing must be maintained | Stale party/trade/billing state possible if branches diverge |
| Low | Code duplication | Two separate `CUser` implementations (TMSrv vs DBSrv) share name but differ | Confusion; contract drift between server and DB |

---

## 11. Test Coverage Analysis

**No test files were found anywhere in the project** (searched for `*test*`/`*spec*` filenames and test directories across the repository, excluding `.git` and `.opencode`). The legacy codebase contains only production sources, `.vcxproj` project files, resource files, and the `.sln`.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CUser (TMSrv) | 0 | 0 | 0% | N/A — no tests exist |
| TMSrv (whole) | 0 | 0 | 0% | N/A — no tests exist |
| DBSrv | 0 | 0 | 0% | N/A — no tests exist |

**Risk assessment:** The absence of any automated tests for a 46k-line game server with a high-coupling session object is the most significant quality gap. The `CUser` session state machine, trade/save serialization, anti-cheat thresholds, and onlytrade logic are prime candidates for unit/integration coverage that does not currently exist. All behaviors documented in this report are derived solely from source inspection.

---

## 12. Notes and Assumptions

- **Component scope:** This analysis covers the `CUser` class as implemented in `legacy/Code/TMSrv/` (the TMSrv game server variant). A separate `CUser` in `legacy/Code/DBSrv/` was noted for contrast but is out of scope.
- **Implicit rules:** Business rules that were not explicitly documented in comments (cooldown thresholds, idle timeout, billing fields) are identified with their confidence level in the detailed breakdowns.
- **Unknown fields:** Numerous `Unk*`/`Unk[NN]` fields are reverse-engineering placeholders from the original authors; their semantics are undocumented and treated as ambiguity rather than fabricated.
- **Endpoints:** Section 7 was intentionally omitted as a table because `CUser` is a data structure and does not itself expose network endpoints.
- **Folders ignored per parameters:** `.git`, `.opencode`.
