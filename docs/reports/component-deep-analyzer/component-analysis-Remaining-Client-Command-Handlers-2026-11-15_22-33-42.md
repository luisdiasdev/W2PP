# Component Deep Analysis Report — Remaining Client Command Handlers

- **Component name:** Remaining Client Command Handlers (family)
- **Primary location:** `Code/TMSrv/_MSG_*.cpp` — the 40 handlers listed in scope
- **Project:** W2PP — decompiled C/C++ (Win32, MSVC) W2 MMORPG server emulator
- **Subsystem:** TMSrv (game / world server)
- **Analysis date:** 2026-11-15 22:33:42
- **Excluded from analysis:** `.agents`, `.codegraph`, `.opencode`, `openspec`
- **Handlers in scope (40):** AcceptParty, AccountSecure, CapsuleInfo, Challange, ChallangeConfirm, ChangeCity, CharacterLogout, CombineItem, CombineItemAgatha, CombineItemAilyn, CombineItemAlquimia, CombineItemExtracao, CombineItemLindy, CombineItemShany, CombineItemTiny, CreateCharacter, DeleteCharacter, DeleteItem, Deposit, Deprivate, GuildAlly, InviteGuild, Motion, NoViewMob, PKMode, PutoutSeal, QuitTrade, RemoveParty, ReqBuy, ReqRanking, REQShopList, ReqTeleport, ReqTradeList, Restart, SendAutoTrade, SendReqParty, SetShortSkill, SplitItem, War, Withdraw
- **Out of scope (already covered by dedicated reports):** `_MSG_UseItem`, `_MSG_Quest`, `_MSG_Attack`, `_MSG_MessageWhisper`, `_MSG_MessageChat`, `_MSG_CombineItemOdin`, `_MSG_CombineItemEhre`, `_MSG_TradingItem`, `_MSG_Trade`, `_MSG_Action`, `_MSG_Buy`, `_MSG_Sell`, `_MSG_GetItem`, `_MSG_DropItem`, `_MSG_ApplyBonus`, `_MSG_AccountLogin`, `_MSG_CharacterLogin`, `_MSG_UpdateItem`

---

## 1. Executive Summary

### 1.1 Component purpose

The "Remaining Client Command Handlers" is a **flat family of 40 free functions** (`void Exec_MSG_<Name>(int conn, char *pMsg)`), one per inbound client message family. Every file contains exactly one function, a single `#include "ProcessClientMessage.h"`, no classes, no state of its own, and no header of its own. All state is reached through the process-wide globals declared in `ProcessClientMessage.h`: `pUser[MAX_USER]` (per-connection session), `pMob[MAX_MOB]` (the unified player/NPC/mob entity array, where indices `0..999` are players and `1000..24999` are NPCs/mobs), the grids, guild tables (`GuildInfo[65536]`, `g_pGuildZone[5]`, `GuildImpozoID[5]`) and the DB socket `DBServerSocket`.

Structurally the family is a **uniform inbound-adapter layer**: `ProcessClientMessage.cpp:38` performs the only global guards (valid `ID` range, `ServerDown < 120`, `_MSG_Ping` drop, `ClientTick == SKIPCHECKTICK` spoof rejection) and then switches on `std->Type` to one `Exec_MSG_*`. Every handler then re-validates its own preconditions from `pUser[conn].Mode`, `pMob[conn].MOB.CurrentScore.Hp` and its own packet fields.

### 1.2 Role in the system

These 40 handlers cover nine functional domains at the *edge* of the server:

| Domain | Handlers | Role |
|---|---|---|
| Party lifecycle | AcceptParty, SendReqParty, RemoveParty | Group formation, invitation correlation, dissolution/inheritance |
| Guild | GuildAlly, InviteGuild, War, Deprivate | Alliance/war submission to DBSrv, recruitment with fee, rank discipline |
| Guild siege / town tax | Challange, ChallangeConfirm | Siege NPC dialogue, weekly state machine, town-tax collection |
| NPC shop | REQShopList | Shop catalog emission (merchant-type mapping) |
| Player shop (auto-trade) | SendAutoTrade, ReqTradeList, ReqBuy | Listing creation, listing visibility, atomic purchase settlement |
| Ranking / duel | ReqRanking | Duel challenge handshake and duel start |
| Movement / world | ReqTeleport, ChangeCity, Restart, Motion, NoViewMob, SetShortSkill | Gate travel with fees, city binding, respawn, animation broadcast, visibility reconciliation |
| Trade lifecycle | QuitTrade (+ cancellation preconditions in 6 other handlers) | Abort escrow, re-broadcast PK state |
| Character session | CreateCharacter, DeleteCharacter, CharacterLogout, AccountSecure, CapsuleInfo | Pre-game session transitions and DB forwarding |
| Inventory / economy | DeleteItem, SplitItem, Deposit, Withdraw, PutoutSeal | Item destruction, stack splitting, warehouse, capsule creation |
| Crafting | CombineItem, CombineItemAgatha, CombineItemAilyn, CombineItemAlquimia, CombineItemExtracao, CombineItemLindy, CombineItemShany, CombineItemTiny | Eight distinct crafting rulesets with divergent loss/consumption semantics |
| PvP flag | PKMode | Kill-mode toggle and state broadcast |

### 1.3 Key findings

1. **The family is a thin layer over `Server.cpp`/`GetFunc.cpp`/`SendFunc.cpp`.** 12 of the 40 handlers are effectively forwarders (AccountSecure 10 lines of logic, CapsuleInfo 10, Deprivate 4, War 20, GuildAlly 20, ChallangeConfirm 6, CharacterLogout 6, RemoveParty 26, SetShortSkill 4, ReqTradeList 15, ReqTeleport 43, ReqRanking 50). The real business rules for those live in `Server.cpp` (`RemoveParty`, `DoDeprivate`, `Challange`, `DoRanking`, `DoRecall`, `DoTeleport`, `CharLogOut`, `RemoveTrade`), `GetFunc.cpp` (`GetMatchCombine*`, `GetTeleportPosition`) and `BASE_GetVillage`/`BASE_CheckValidString` in `Basedef.cpp`.
2. **Validation is inconsistent across the family — 4 distinct guard idioms coexist.** (a) full guard `HP == 0 || Mode != USER_PLAY` + `SendHpMode` + `AddCrackError` (Deposit, Withdraw, Motion, REQShopList, ReqBuy, ReqTradeList, QuitTrade, SendAutoTrade, PutoutSeal); (b) HP-only or Mode-only (DeleteItem, SplitItem, CombineItem family — mode only); (c) **no guard at all** (ChangeCity, PKMode, ReqTeleport, SetShortSkill, AccountSecure, CapsuleInfo, Challange, GuildAlly, War, Deprivate, CharacterLogout, CreateCharacter/DeleteCharacter use a *different* mode gate `USER_SELCHAR`); (d) guard placed *after* field validation (PutoutSeal validates `SourType`/grid only after the HP check but before pointer resolution).
3. **Eight crafting handlers implement eight different "what is lost on failure" policies.** `CombineItem` and `CombineItemAlquimia` and `CombineItemShany` consume all inputs before the roll; `CombineItemAgatha` and `CombineItemTiny` deliberately **keep** input index 1 (the effect donor) while destroying index 0; `CombineItemAilyn` charges 50,000,000 on the attempt and **another** 50,000,000 on success (`_MSG_CombineItemAilyn.cpp:73` and `:124`); `CombineItemExtracao` consumes the 1774 stone unconditionally but destroys the target item only on failure; `CombineItemLindy` is deterministic (no roll).
4. **`rand() % 115` with `if (_rand >= 100) _rand -= 15` is the family-wide probability idiom** (7 handlers + `GetTeleportPosition`-adjacent code). It yields a uniform 0..99 distribution, but it is built on `rand()` seeded once at startup — no per-call entropy, no server-authority proof, and it is **bypassed entirely when `LOCALSERVER` is 1** (`Server.cpp:3588`, set when `DBServerAddress == "192.168.0.50"`), which makes every craft guaranteed on that host configuration.
5. **Three money-capacity defects and one slot-bounds defect were identified** (see §11): `Deposit`/`Withdraw` compute `int tcoin = coin + pUser[conn].Coin` where both operands may be ~2e9 (signed overflow before the range test); `CombineItemAilyn` double-charges 50M; `ReqBuy`'s free-slot scan `for (i = 0; i < MAX_CARRY-4; i++)` is followed by `if (i == MAX_CARRY)` — unreachable — so a full 60-slot inventory writes into `Carry[60]` (equipment region) at `_MSG_ReqBuy.cpp:106-128`.
6. **Anti-replay/anti-spoof protection is concentrated in exactly one handler**: `AcceptParty` correlates the invite through `pMob[].LastReqParty` and calls `CrackLog(conn, "PARTYHACK")` on mismatch (`_MSG_AcceptParty.cpp:42-47`). No other handler in the family binds a client request to prior server state, apart from `ReqRanking`'s `RankingTarget` echo check and `ReqBuy`'s listing/cargo double-match.
7. **Zero automated tests exist in the repository** — no test project in `W2PP Code Project.sln`, no test framework reference in `Code/TMSrv/TMSrv.vcxproj`, and no file matching `*test*`, `*Test*`, `*.spec.*`, `*_test.*` anywhere outside the ignored folders (see §12 for evidence).

---

## 2. Data Flow Analysis

### 2.1 Common inbound pipeline

```
1. Client sends opcode-addressed packet (Size, KeyWord, CheckSum, Type, ID, ClientTick)     Basedef.h:925-930
2. Server.cpp:~4001 receives from CPSock and calls ProcessClientMessage(User, Msg, FALSE)
3. ProcessClientMessage.cpp:38-62  -> validates std->ID in [1, MAX_USER), ServerDown < 120,
                                      refreshes pUser[conn].LastReceiveTime, drops _MSG_Ping,
                                      rejects ClientTick == SKIPCHECKTICK when isServer == FALSE
4. ProcessClientMessage.cpp:65-311 -> switch(std->Type) dispatches to Exec_MSG_<Family>
5. Exec_MSG_<Family> casts pMsg to its packet struct (no size validation)
6. Preconditions: pUser[conn].Mode, pMob[conn].MOB.CurrentScore.Hp, Trade/OpponentID, target ranges
7. Domain logic: either inline (crafting/inventory/shop) or delegated to
   Server.cpp / GetFunc.cpp / SendFunc.cpp helpers
8. Mutations: pMob[].MOB.Carry / Equip, pUser[].Cargo / Coin, pUser[].AutoTrade, pUser[].Trade,
   MCB guild tables (GuildInfo, g_pGuildZone), NPC treasury (pMob[GuildImpostoID[z]].MOB.Exp)
9. Outbound: cSock.AddMessage / SendOneMessage (client), DBServerSocket.SendOneMessage (DBSrv),
   GridMulticast (neighbourhood), ItemLog/Log (file audit)
```

### 2.2 Party formation (SendReqParty → AcceptParty)

```
1. Client (would-be leader, conn) sends _MSG_SendReqParty{PartyID, MobName, unk=targetID}   SendReqParty.cpp:22
2. Validate: partyID == conn (leader self-identity)                                       :29
3. Validate: leader not already a member (pMob[conn].Leader == 0)                          :34
4. Validate: target in [1,MAX_USER), USER_PLAY, partyless (Leader==0 and PartyList empty)   :39-54, :87-97
5. Validate: leader not in OnlyTrade                                                      :56
6. Level window: |lvl(target) - lvl(conn)| within PARTY_DIF or exemption                    :68-71
7. Battle Royale guard: block if inside BR arena rectangle                                 :73-74
8. Record pMob[targetID].LastReqParty = conn; SendReqParty(targetID, partyID, partyID)      :76-77
9. Target client answers _MSG_AcceptParty{LeaderID, MobName}                               AcceptParty.cpp:23
10. Validate leader range, MobName == pMob[leaderID].MOB.MobName                            :30-40
11. Validate leaderID == pMob[myindex].LastReqParty  (anti-spoof, CrackLog "PARTYHACK")     :42-47
12. Validate leader is a leader (Leader==0), USER_PLAY; requester partyless, not OnlyTrade  :54-91
13. Level window re-check (asymmetric computation, see BR-PTY-04)                           :93-96
14. Find free slot in leader's PartyList[0..11]; reject if full                            :101-115
15. Mutate: PartyList[i] = requester; requester.Leader = leader                             :117-118
16. Notify: SendAddParty for leader/self/leader-to-member/all existing members             :122-142
17. Audit: "accept_party,<member> aceitou party de <leader>"                                :144-145
```

### 2.3 Auto-trade listing and purchase

```
SendAutoTrade (seller)                                     ReqBuy (buyer)
1. guard HP/Mode (crack 10/88)                              1. guard HP/Mode (crack 10/86)
2. cancel existing trade / reject if TradeMode              2. reject if TradeMode or Trade.OpponentID
3. require NewbieEventServer != 0                           3. validate targetID range
4. reject blacklist rectangle, require village 0..4         4. proximity test vs VIEWGRID
5. per slot: price 0..1,999,999,999; item<->price;           5. itemPos < MAX_AUTOTRADE;
   blacklist 508/3993/747/509/522/526..531/446;                CarryPos < MAX_CARGO
   CarryPos < MAX_CARGO; EF_NOTRADE == 0;                     6. itemTax == seller.AutoTrade.Tax
   cargo slot == advertised item                             7. itemPrice == seller.AutoTrade.Coin[pos]
6. Tax = g_pGuildZone[village].CityTax                      8. packet item == listing item
7. Title sanitized (last 2 bytes); copy into                 9. seller Cargo[StorageSlot] == listing item
   pUser[conn].AutoTrade; TradeMode = 1                     10. buyer.MOB.Coin >= itemPrice
8. SendAutoTrade echo; GridMulticast CreateMobTrade        11. seller.Coin + price <= 2,000,000,000
   with Con score zeroed (shop mob has no CON)             12. buyer free carry slot  (see RISK-04)
                                                           13. seller in village 0..4
                                                           14. move item to buyer carry
                                                           15. commission = price/100 * tax when price >= 100,000
                                                           16. clear listing + cargo, SendItem to seller
                                                           17. buyer.Coin -= price; seller.Coin += net
                                                           18. treasury credit (zone and zone 4), cap 200,000,000,000
                                                           19. GridMulticast _MSG_ItemSold + seller message
                                                           20. SaveUser(seller, 1)
```

### 2.4 Crafting (common skeleton, divergent semantics)

```
1. For i in 0..MAX_COMBINE-1: if m->Item[i].sIndex != 0
      a. invPos = m->InvenPos[i]; if invPos outside [0, MaxCarry) -> RemoveTrade(conn); abort
      b. if memcmp(&Carry[invPos], &m->Item[i], sizeof(STRUCT_ITEM)) != 0
            -> ItemLog "item remove or changed"; send _MSG_CombineComplete = 0; abort
2. rate = GetMatchCombine<Recipe>(m->Item)   // 0 or -1 means invalid recipe
3. Consume inputs (policies differ per handler — see §3.10)
4. _rand = rand() % 115; if (_rand >= 100) _rand -= 15        // uniform 0..99
5. if (_rand <= rate || LOCALSERVER) -> success transform + _MSG_CombineComplete = 1
   else                                -> failure        + _MSG_CombineComplete = 2
6. SendItem for every mutated slot (client resync)  [putout: not always — see RISK-11]
```

### 2.5 Character session (pre-game)

```
CreateCharacter: USER_SELCHAR gate -> name policy (BASE_CheckValidString) -> Mode=USER_WAITDB
                 -> Type rewritten to _MSG_DBCreateCharacter -> DBServerSocket
DeleteCharacter: USER_SELCHAR gate -> Mode=USER_WAITDB -> Type=_MSG_DBDeleteCharacter -> DBServerSocket
CharacterLogout: RemoveParty(conn) -> CharLogOut(conn)  [save, DeleteMob, Mode=USER_SELCHAR, _MSG_CNFCharacterLogout]
                          -> cSock.SendMessageA() flush
AccountSecure:   conn range -> m->ID = conn -> DBServerSocket (same struct, same type)
CapsuleInfo:     m->ID = conn, m->Type = _MSG_DBCapsuleInfo -> DBServerSocket (MSG_STANDARDPARM, Parm = index)
```

### 2.6 Guild / siege

```
GuildAlly / War : validate guild id + GuildLevel==9 -> rebuild MSG_STANDARDPARM2 -> DBSrv
InviteGuild     : eligibility matrix -> weekday window -> coin fee -> mutate target MOB.Guild/GuildLevel
                  -> DBSrv guild-name lookup -> join message -> GridMulticast CreateMob(target)
Deprivate       : DoDeprivate() -> rank comparison -> same-guild -> sub-slot cleanup (Sub1/Sub2/Sub3)
                  -> MSG_GuildInfo to DBSrv -> clear target guild fields -> GridMulticast CreateMob(target)
Challange       : NPC.BaseScore.Level = zone -> WeekMode state machine -> town-tax payout (PutItem 4011 x N)
                  or SendClientSignal(_MSG_ReqChallange)
ChallangeConfirm: zone = NPC.Merchant - 6 -> Challange() -> fame>=100 gate, -100 fame, MSG_GuildInfo -> DBSrv
```

---

## 3. Business Rules & Logic

## 3.0 Overview of the business rules

| Rule ID | Domain | Rule Type | Rule Description | Location |
|---|---|---|---|---|
| BR-PTY-01 | Party | Authorization | Accepted invite must be correlated with `pMob[requester].LastReqParty`; mismatch triggers `CrackLog("PARTYHACK")` | `_MSG_AcceptParty.cpp:42-47` |
| BR-PTY-02 | Party | Validation | Accepted invite must name the current leader (`MobName` equality) | `_MSG_AcceptParty.cpp:36-40` |
| BR-PTY-03 | Party | Validation | Leader/requester ID range `[1, MAX_USER)`, identity `myindex == conn` | `_MSG_AcceptParty.cpp:30-34,49-53` |
| BR-PTY-04 | Party | Validation | Leader must hold leader flag (`pMob[].Leader == 0`) and be `USER_PLAY` | `_MSG_AcceptParty.cpp:54-64` |
| BR-PTY-05 | Party | Validation | Requester must have no party (`Leader == 0` and all `PartyList` empty) | `_MSG_AcceptParty.cpp:65-69,84-91` |
| BR-PTY-06 | Party | Validation | Neither party may be in `OnlyTrade` (shop) mode | `_MSG_AcceptParty.cpp:71-81` |
| BR-PTY-07 | Party | Business Logic | Level window `PARTY_DIF` with `>=1000`, same-`ClassMaster` exemptions; `PARTY_DIF` default 150, configurable | `_MSG_AcceptParty.cpp:93-96,149`; `Server.cpp:44`; `imple.cpp:1740` |
| BR-PTY-08 | Party | Constraint | Battle Royale arena rectangle blocks party formation/invitation | `_MSG_AcceptParty.cpp:98-99`; `_MSG_SendReqParty.cpp:73-74` |
| BR-PTY-09 | Party | Business Logic | Party capacity `MAX_PARTY` = 12; leader occupies position 0, members 1..12 | `_MSG_AcceptParty.cpp:101-142` |
| BR-PTY-10 | Party | Validation | Inviter must be the party leader itself (`PartyID == conn`) and not a member | `_MSG_SendReqParty.cpp:29-38` |
| BR-PTY-11 | Party | Validation | Invite target must be `USER_PLAY` and completely partyless | `_MSG_SendReqParty.cpp:39-54,87-97` |
| BR-PTY-12 | Party | Business Logic | Invitation is recorded server-side (`LastReqParty`) and forwarded to target | `_MSG_SendReqParty.cpp:76-77` |
| BR-PTY-13 | Party | Business Logic | Removal target resolution: invalid/non-member target collapses to self | `_MSG_RemoveParty.cpp:26-40` |
| BR-PTY-14 | Party | Business Logic | Members leave themselves; leaders kick/expel or dissolve | `_MSG_RemoveParty.cpp:42-46` |
| BR-PTY-15 | Party | Business Logic | Leadership inheritance, `Pista` registry rewrite, summon cleanup on removal | `Server.cpp:7354-7470` |
| BR-PTY-16 | Party | Business Logic | Character logout always removes the player from the party first | `_MSG_CharacterLogout.cpp:23` |
| BR-GLD-01 | Guild | Authorization | `_MSG_GuildAlly` requires `MOB.Guild == Guild` and `GuildLevel == 9` | `_MSG_GuildAlly.cpp:27-34` |
| BR-GLD-02 | Guild | Business Logic | Alliance request rebuilt and forwarded to DBSrv (no local write) | `_MSG_GuildAlly.cpp:36-46` |
| BR-GLD-03 | Guild | Authorization | `_MSG_War` requires guild ownership + `GuildLevel == 9` | `_MSG_War.cpp:27-34` |
| BR-GLD-04 | Guild | Business Logic | War declaration forwarded to DBSrv | `_MSG_War.cpp:36-46` |
| BR-GLD-05 | Guild | Validation | `InviteGuild`: target `[1,MAX_USER)`, invite type `[0,4)` | `_MSG_InviteGuild.cpp:27-31` |
| BR-GLD-06 | Guild | Validation | Inviter in guild, target guildless, same `Clan` | `_MSG_InviteGuild.cpp:33-41` |
| BR-GLD-07 | Guild | Authorization | Inviter rank `>= 1`; invite type `!= 0` requires rank 9 | `_MSG_InviteGuild.cpp:44-49` |
| BR-GLD-08 | Guild | Constraint | Recruitment blocked on Sunday (`tm_wday == 0`) with a "Saturday" message | `_MSG_InviteGuild.cpp:52-62` |
| BR-GLD-09 | Guild | Business Logic | Recruitment fee 4,000,000 (type 0) / 100,000,000 (type != 0) | `_MSG_InviteGuild.cpp:64-73` |
| BR-GLD-10 | Guild | Business Logic | Target joins at `GuildLevel = 0`, gets join message and mob re-broadcast | `_MSG_InviteGuild.cpp:75-97` |
| BR-GLD-11 | Guild | Authorization | `Deprivate` authority matrix (ranks 6-8 vs <6, rank 9 vs all) | `Server.cpp:7775-7791` |
| BR-GLD-12 | Guild | Business Logic | Sub-leader slots `Sub1/Sub2/Sub3` cleared in order and pushed to DBSrv | `Server.cpp:7795-7820` |
| BR-GLD-13 | Guild | Business Logic | Deprivation resets target guild fields and re-broadcasts mob/score | `Server.cpp:7822-7844` |
| BR-CHL-01 | Siege | Validation | Challenge NPC encodes zone in `MOB.BaseScore.Level`, must be `< ValidGuild` and `!= 5` | `_MSG_Challange.cpp:27-35` |
| BR-CHL-02 | Siege | Business Logic | `WeekMode == 4`: town-tax collection with 2G ceiling and 1e9 coin-item conversion | `_MSG_Challange.cpp:46-114` |
| BR-CHL-03 | Siege | Business Logic | `WeekMode == 5`: challenge window requested via `_MSG_ReqChallange` signal | `_MSG_Challange.cpp:115-116` |
| BR-CHL-04 | Siege | Business Logic | Other week modes: announce challenger price / champion / "no challenger" | `_MSG_Challange.cpp:119-133` |
| BR-CHL-05 | Siege | Validation | `ChallangeConfirm` derives zone from `Merchant - 6`, excludes zone 4 | `_MSG_ChallangeConfirm.cpp:30-33` |
| BR-CHL-06 | Siege | Business Logic | Challenge submission: rank 9, citizenship, guild name, fame >= 100, fee 100 fame | `Server.cpp:2479-2570` |
| BR-SHP-01 | Shop | Validation | Shop list requires alive/`USER_PLAY`, NPC target, merchant flag, visibility | `_MSG_REQShopList.cpp:24-48` |
| BR-SHP-02 | Shop | Business Logic | Merchant `1` -> catalog 1; merchant `19` -> catalog 3; all others silent | `_MSG_REQShopList.cpp:50-54` |
| BR-SHP-03 | Shop | Validation | Auto-trade requires alive/in-game, no trade, no listing, `NewbieEventServer != 0` | `_MSG_SendAutoTrade.cpp:24-49` |
| BR-SHP-04 | Shop | Validation | Auto-trade location: blacklist rectangle and village-only requirement | `_MSG_SendAutoTrade.cpp:56-68` |
| BR-SHP-05 | Shop | Validation | Per-slot price bounds `[0,1999999999]`, item/price consistency | `_MSG_SendAutoTrade.cpp:75-76` |
| BR-SHP-06 | Shop | Constraint | Auto-trade item blacklist (508, 3993, 747, 509, 522, 526-531, 446) + `CarryPos` bounds | `_MSG_SendAutoTrade.cpp:80-82` |
| BR-SHP-07 | Shop | Constraint | `EF_NOTRADE` items and non-matching cargo entries are rejected | `_MSG_SendAutoTrade.cpp:84-94` |
| BR-SHP-08 | Shop | Business Logic | Listing registration: `CityTax`, title sanitisation, `TradeMode = 1`, shop mob broadcast with `Con = 0` | `_MSG_SendAutoTrade.cpp:98-119` |
| BR-SHP-09 | Shop | Validation | Listing visibility requires seller `TradeMode != 0` and `VIEWGRID` proximity | `_MSG_ReqTradeList.cpp:26-44` |
| BR-SHP-10 | Shop | Validation | Purchase: target range, proximity, listing/cargo/price/tax quadruple match | `_MSG_ReqBuy.cpp:49-87` |
| BR-SHP-11 | Shop | Validation | Purchase: buyer funds, seller 2G ceiling, buyer inventory space, seller village | `_MSG_ReqBuy.cpp:90-126` |
| BR-SHP-12 | Shop | Business Logic | Settlement: item move, commission `(price/100)*tax` when `price >= 100,000`, net credit, `SaveUser` | `_MSG_ReqBuy.cpp:128-163` |
| BR-SHP-13 | Shop | Business Logic | Town tax credited to zone treasury and always additionally to zone 4 (cap 200,000,000,000) | `_MSG_ReqBuy.cpp:165-180` |
| BR-SHP-14 | Shop | Business Logic | Sale broadcast `_MSG_ItemSold` + seller notification | `_MSG_ReqBuy.cpp:182-192` |
| BR-RNK-01 | Ranking | Validation | Type `[0,4]`, target `[1,MAX_USER)`; whisper-blocked targets refused | `_MSG_ReqRanking.cpp:27-34` |
| BR-RNK-02 | Ranking | Authorization | Team duels (types 1/2) refused when both players hold guild rank | `_MSG_ReqRanking.cpp:36-49` |
| BR-RNK-03 | Ranking | Validation | Acceptance requires `RankingTarget == conn`, target `USER_PLAY`, no duel in progress | `_MSG_ReqRanking.cpp:53-64` |
| BR-RNK-04 | Ranking | Business Logic | 1v1 duel: teleport to arenas, 121 s timer, level titles; team duels: 301 s, guild names | `Server.cpp:8052-8110` |
| BR-TEL-01 | Travel | Constraint | Teleport blocked from water zone when `x/4 == 491 && y/4 == 443` | `_MSG_ReqTeleport.cpp:25-29` |
| BR-TEL-02 | Travel | Business Logic | Destination/fee resolved by `GetTeleportPosition` table (city gates 700) | `GetFunc.cpp:658-899` |
| BR-TEL-03 | Travel | Business Logic | Members of the zone-4 owning clan travel free | `_MSG_ReqTeleport.cpp:33-36` |
| BR-TEL-04 | Travel | Business Logic | Fee debited, half credited to zone-4 treasury capped 2,000,000,000,000 | `_MSG_ReqTeleport.cpp:38-51` |
| BR-TEL-05 | Travel | Business Logic | Teleport applied only if destination differs; Kefra ticket consumption | `_MSG_ReqTeleport.cpp:54-60`; `GetFunc.cpp:868-878` |
| BR-TEL-06 | World | Business Logic | `ChangeCity` encodes village into `Merchant` bits 6-7 (`0x3F` mask, `city << 6`) | `_MSG_ChangeCity.cpp:23-32` |
| BR-TEL-07 | World | Business Logic | `Restart` sets HP=2, resets `NumError`, applies faction spawn or recall | `_MSG_Restart.cpp:24-45` |
| BR-TRD-01 | Trade | Validation | `QuitTrade` requires alive/in-game; failure adds crack point 10/17 | `_MSG_QuitTrade.cpp:22-27` |
| BR-TRD-02 | Trade | Business Logic | Trade teardown is mutual (both endpoints) | `_MSG_QuitTrade.cpp:29-30` |
| BR-TRD-03 | Trade | Business Logic | Trade exit re-broadcasts PK state | `_MSG_QuitTrade.cpp:32-53` |
| BR-TRD-04 | Trade | Business Logic | Six other handlers abort an active trade as a side effect | `_MSG_SplitItem.cpp:36-40`; `_MSG_DeleteItem.cpp:36-41`; `_MSG_CombineItem.cpp:34`; `_MSG_PutoutSeal.cpp:30-34`; `_MSG_ReqBuy.cpp:32-42`; `_MSG_SendAutoTrade.cpp:32-37` |
| BR-CHR-01 | Session | Validation | `CreateCharacter` requires `USER_SELCHAR` | `_MSG_CreateCharacter.cpp:27-34` |
| BR-CHR-02 | Session | Validation | Character name policy: 4..15 chars, `[A-Za-z0-9-]`, blacklist of GM words | `Basedef.cpp:2522-2551` |
| BR-CHR-03 | Session | Business Logic | Successful create: `Mode = USER_WAITDB`, type rewritten to `_MSG_DBCreateCharacter` | `_MSG_CreateCharacter.cpp:36-46` |
| BR-CHR-04 | Session | Validation | `DeleteCharacter` requires `USER_SELCHAR`, else English "wait a moment" message | `_MSG_DeleteCharacter.cpp:26-44` |
| BR-CHR-05 | Session | Business Logic | `DeleteCharacter` forwards to DBSrv without local name validation | `_MSG_DeleteCharacter.cpp:28-33` |
| BR-CHR-06 | Session | Business Logic | Logout sequence: party removal, save, mob deletion, mode reset, flush | `_MSG_CharacterLogout.cpp:23-26`; `Server.cpp:7085-7125` |
| BR-CHR-07 | Session | Business Logic | `AccountSecure` rewrites `ID` and forwards the raw struct to DBSrv | `_MSG_AccountSecure.cpp:25-30` |
| BR-CHR-08 | Session | Business Logic | `CapsuleInfo` rewrites type/ID and forwards capsule index as `MSG_STANDARDPARM` | `_MSG_CapsuleInfo.cpp:25-30` |
| BR-INV-01 | Inventory | Validation | `DeleteItem`: slot `[0, MAX_CARRY-4)`, index `[1, MAX_ITEMLIST)` | `_MSG_DeleteItem.cpp:24-28` |
| BR-INV-02 | Inventory | Validation | `DeleteItem`: alive/in-game guard; trade aborts deletion | `_MSG_DeleteItem.cpp:30-41` |
| BR-INV-03 | Inventory | Business Logic | `DeleteItem`: audit log with item code, then slot clear (no client echo) | `_MSG_DeleteItem.cpp:42-48` |
| BR-INV-04 | Inventory | Validation | `SplitItem`: slot and `Num` bounds (`Num` 1..119) | `_MSG_SplitItem.cpp:24-28,54-58` |
| BR-INV-05 | Inventory | Constraint | `SplitItem`: stackable whitelist 413/412/419/420/2390-2419 | `_MSG_SplitItem.cpp:44-50` |
| BR-INV-06 | Inventory | Constraint | `SplitItem`: at least one free slot; cannot split a whole stack | `_MSG_SplitItem.cpp:63-73` |
| BR-INV-07 | Inventory | Business Logic | `SplitItem`: remainder kept, new stack created, audit, resync | `_MSG_SplitItem.cpp:75-93` |
| BR-INV-08 | Inventory | Validation | `Deposit`: alive/in-game, amount ownership, 2G ceiling both sides | `_MSG_Deposit.cpp:24-55` |
| BR-INV-09 | Inventory | Business Logic | `Deposit`: echo with `ID = ESCENE_FIELD`, cargo coin resync | `_MSG_Deposit.cpp:42-49` |
| BR-INV-10 | Inventory | Validation | `Withdraw`: mirror rules with crack point 10/2 | `_MSG_Withdraw.cpp:24-54` |
| BR-INV-11 | Inventory | Business Logic | `Withdraw`: echo with `ID = 30000`, cargo coin resync | `_MSG_Withdraw.cpp:42-48` |
| BR-INV-12 | Inventory | Validation | `PutoutSeal`: alive/in-game, no trade, dead-player item resync | `_MSG_PutoutSeal.cpp:24-51` |
| BR-INV-13 | Inventory | Validation | `PutoutSeal`: grid bounds, resolvable source pointer | `_MSG_PutoutSeal.cpp:53-66` |
| BR-INV-14 | Inventory | Constraint | `PutoutSeal`: item must be 3443 with a non-zero first effect | `_MSG_PutoutSeal.cpp:68-78` |
| BR-INV-15 | Inventory | Business Logic | `PutoutSeal`: name policy, immediate logout, DB capsule creation packet | `_MSG_PutoutSeal.cpp:80-111` |
| BR-CRF-01 | Crafting | Validation | All combine handlers verify each declared input against `Carry[InvenPos]` byte-for-byte | `_MSG_CombineItem.cpp:25-44` (same block in 8 handlers) |
| BR-CRF-02 | Crafting | Constraint | Item 747 is a global crafting poison pill in every recipe matcher | `GetFunc.cpp:37-41,169-173,219-223,263-267` |
| BR-CRF-03 | Crafting | Business Logic | Ancient combine rate = base 1 + `g_pAnctChance[0..2]` per stone sanctification 7/8/9 | `GetFunc.cpp:33-104` |
| BR-CRF-04 | Crafting | Business Logic | Ancient combine consumes all inputs before the roll; success rewrites index to `Extra + jewelOffset`, anct 7 | `_MSG_CombineItem.cpp:55-143` |
| BR-CRF-05 | Crafting | Business Logic | Agatha: donor (index 1) survives, base (index 0) is destroyed either way | `_MSG_CombineItemAgatha.cpp:60-129` |
| BR-CRF-06 | Crafting | Constraint | Agatha recipe: Arch base, donor item level >= 4, matching `nPos`, sanc >= 9 on both, 4x item 3140 | `GetFunc.cpp:377-425` |
| BR-CRF-07 | Crafting | Business Logic | Ailyn charges 50,000,000 at attempt and again on success (100M total) | `_MSG_CombineItemAilyn.cpp:73,124` |
| BR-CRF-08 | Crafting | Constraint | Ailyn recipe: two identical items of grade 5-8, one 1774, jewels 2441-2444 by grade | `GetFunc.cpp:261-375` |
| BR-CRF-09 | Crafting | Business Logic | Ailyn success sets sanctification 10 with jewel index as success counter | `_MSG_CombineItemAilyn.cpp:112-119` |
| BR-CRF-10 | Crafting | Constraint | Alquimia requires `MOB.Class == 3` and one of 10 fixed recipes | `_MSG_CombineItemAlquimia.cpp:47`; `GetFunc.cpp:498-559` |
| BR-CRF-11 | Crafting | Business Logic | Alquimia chance = `(Special[2]+1)/6`; success yields item `3200 + recipeId` with cleared effects | `_MSG_CombineItemAlquimia.cpp:82-104` |
| BR-CRF-12 | Crafting | Constraint | Extracao: item level < 5, sanc >= 9, `nPos` in {2,4,8,16,32}, `EF_MOBTYPE == 0` | `_MSG_CombineItemExtracao.cpp:34-43` |
| BR-CRF-13 | Crafting | Business Logic | Extracao always consumes one item 1774 (first match anywhere in inventory) | `_MSG_CombineItemExtracao.cpp:45-58` |
| BR-CRF-14 | Crafting | Business Logic | Extracao success preserves item level, adds static damage, rewrites index `3021 + slotOffset`; failure destroys the item | `_MSG_CombineItemExtracao.cpp:67-107` |
| BR-CRF-15 | Crafting | Business Logic | Lindy is deterministic: quest-state gated, no roll | `_MSG_CombineItemLindy.cpp:45-121` |
| BR-CRF-16 | Crafting | Business Logic | Lindy recipe: 2x (413 amount 10) + 4127 + 4x 413 | `_MSG_CombineItemLindy.cpp:77-78` |
| BR-CRF-17 | Crafting | Business Logic | Lindy rewards: level 354 -> `Level355`, clan-specific `Equip[15]` 3191/3192/3193; level 369 -> `Level370`, -1 fame | `_MSG_CombineItemLindy.cpp:90-112` |
| BR-CRF-18 | Crafting | Constraint | Shany class gate uses `MORTAL || ARCH && level < 355` (precedence-dependent) | `_MSG_CombineItemShany.cpp:45-50` |
| BR-CRF-19 | Crafting | Constraint | Shany recipe: 2x (540/541 sanc>=9) + one of 540/541/633 + 4x 413 | `GetFunc.cpp:246-256` |
| BR-CRF-20 | Crafting | Business Logic | Shany roll compares against `g_pShanyBase`; success grants item 633 | `_MSG_CombineItemShany.cpp:85-104` |
| BR-CRF-21 | Crafting | Business Logic | Tiny gates on 100,000,000 and charges it only on success | `_MSG_CombineItemTiny.cpp:45-50,107-108` |
| BR-CRF-22 | Crafting | Constraint | Tiny recipe: Arch grade 5-8, `EF_ITEMTYPE` 4/5, `nPos` 64/192, three items sanc >= 9 | `GetFunc.cpp:167-215` |
| BR-CRF-23 | Crafting | Business Logic | Tiny rate = `g_pTinyBase + itemLevel*5`; failure destroys base, keeps donor | `GetFunc.cpp:214`; `_MSG_CombineItemTiny.cpp:128-139` |
| BR-PVP-01 | PvP | Business Logic | `PKMode` is normalised to 0/1 from any non-zero input | `_MSG_PKMode.cpp:24` |
| BR-PVP-02 | PvP | Business Logic | Enabling PK mode aborts a mutual trade with notification to both | `_MSG_PKMode.cpp:26-36` |
| BR-PVP-03 | PvP | Business Logic | PK state broadcast = guilty || PKMode || inside RvR/Castle/GTorre rectangles; newbie event forces 1 | `_MSG_PKMode.cpp:45-60` |
| BR-MSC-01 | Misc | Validation | `Motion` requires alive/in-game (`AddCrackError(4,6)`), then broadcasts | `_MSG_Motion.cpp:24-36` |
| BR-MSC-02 | Misc | Business Logic | `NoViewMob` reconciles visibility: empty -> remove, in view -> create+PKInfo, else remove | `_MSG_NoViewMob.cpp:26-49` |
| BR-MSC-03 | Misc | Business Logic | `SetShortSkill` persists 4-byte skill bar and 16-byte shortcut array without validation | `_MSG_SetShortSkill.cpp:24-25` |

---

## 3.1 Detailed breakdown — Party domain

### Business Rule: BR-PTY-01 Invitation correlation (anti-spoof nonce)

**Overview**
An accepted party invitation is only honoured when the leader named in the packet is the same index the server recorded in `pMob[requester].LastReqParty` when the invitation was issued. A mismatch is treated as an exploit attempt and logged with `CrackLog(conn, "PARTYHACK")`.

**Detailed description**
`_MSG_SendReqParty.cpp:76` writes `pMob[targetID].LastReqParty = conn` immediately before the invitation packet is emitted, so the server holds a single pending inviter per player. `_MSG_AcceptParty.cpp:42` then compares the client-supplied `m->LeaderID` against that stored value; because `LeaderID` is also range-checked against `[1, MAX_USER)` at `:30` it cannot be abused as an arbitrary array index before the comparison.

This is the only per-request correlation token in the whole 40-handler family: no other handler binds a confirmation to a previously issued server token, which means the party subsystem is the only one where a replayed client packet is structurally detectable. The consequence for the component is a strong guarantee on party joins but a weak one everywhere else (see RISK-01).

Because the token is a single slot, a second invitation issued before the first is answered silently overwrites the first. That produces a legitimate-but-confusing failure mode: the earlier invitee receives `_NN_Not_Connected` (`:38`) even though the leader is online, and no `CrackLog` entry is produced because the mismatch is detected by the *name* check rather than the token check.

**Rule workflow**
1. Invitation issued: `pMob[target].LastReqParty = conn` (`_MSG_SendReqParty.cpp:76`).
2. Client answers `_MSG_AcceptParty{LeaderID, MobName}`.
3. Validate `LeaderID` in `[1, MAX_USER)`; otherwise log `err,CNFParty leader out of range` and drop.
4. If `strcmp(pMob[leaderID].MOB.MobName, m->MobName)` != 0 -> `_NN_Not_Connected`, drop.
5. If `leaderID != pMob[myindex].LastReqParty` -> log `err,CNFParty leader no send reqparty`, `CrackLog("PARTYHACK")`, drop.

---

### Business Rule: BR-PTY-02 … BR-PTY-03 Identity and name binding

**Overview**
The accept packet must reference an existing online entity by both index and name, and the accepting connection must be its own identity (`m`, `conn`, and `myindex` must agree).

**Detailed description**
The name check at `_MSG_AcceptParty.cpp:36` is a defence against a client that guesses a low `LeaderID` (indices are dense and predictable). Even if the token check at `:42` were bypassed, an attacker would still have to know the exact 16-byte `MobName` of the target slot. `m->MobName[NAME_LENGTH - 1] = 0` at `:28` guarantees the string is terminated before `strcmp`, so a crafted packet cannot read past the field.

The identity check at `:49` (`myindex <= 0 || myindex >= MAX_USER || myindex != conn`) is a tautology in practice because `myindex` is assigned from `conn` at `:25`; it therefore documents intent rather than enforcing a constraint. This is characteristic of the family: several handlers re-check values they themselves derived, which inflates perceived rigour without adding a control.

The practical effect for the component is that the party join is the only place where the server proves "you were actually invited by this specific character". Downstream party operations (BR-PTY-15 leadership inheritance) rely on that proof, so weakening this rule would allow arbitrary party hijacking.

**Rule workflow**
1. `myindex = conn`, `leaderID = m->LeaderID`.
2. Terminate `m->MobName` at `NAME_LENGTH-1`.
3. Range-check `leaderID`; range-check `myindex` and its equality with `conn`.
4. Compare `m->MobName` to `pMob[leaderID].MOB.MobName`.
5. Any failure: log (or message `_NN_Not_Connected`) and return without mutation.

---

### Business Rule: BR-PTY-04 Leader and requester eligibility

**Overview**
The accepting side must not already be a party member, the leader must currently be a leader (`pMob[].Leader == 0`), the leader must be `USER_PLAY`, and neither side may be in `OnlyTrade` (player-shop) mode.

**Detailed description**
`pMob[leaderID].Leader` is 0 for a player who leads (or stands alone) and non-zero for a member; the check at `_MSG_AcceptParty.cpp:54` therefore rejects invitations issued by someone who has since joined another party. The `USER_PLAY` test at `:60` rejects leaders who are mid-login, mid-character-select, or already logging out, which prevents a party from being formed against a session that cannot receive the notification packets.

The `OnlyTrade` test appears twice, at `:71` (requester) and `:77` (leader), and uses different message targets: the requester is told via `SendClientMessage(myindex, _NN_ONLYTRADE)` while the leader — who may not even be the sender of this packet — receives the same text. `OnlyTrade` is set by other subsystems (shop/auto-trade mode) and is not cleared by the party subsystem, so a player in shop mode can never be pulled into a group.

The double membership test at `:65` and the `PartyList` scan at `:84-91` overlap: `Leader` catches members whose leader array entry exists, while the `PartyList` scan catches the leader's own non-empty roster. Together they enforce "no partial party state", which matters because `RemoveParty` (BR-PTY-15) can leave a leader with `Leader == 0` but stale `PartyList` entries if interrupted.

**Rule workflow**
1. Reject if `pMob[leaderID].Leader != 0` -> `_NN_Other_Partys_Member`.
2. Reject if `pUser[leaderID].Mode != USER_PLAY` -> `_NN_Not_Connected`.
3. Reject if `pMob[myindex].Leader != 0` -> `_NN_Dropped_Current_Party_First`.
4. Reject if `pUser[myindex].OnlyTrade` -> `_NN_ONLYTRADE`.
5. Reject if `pUser[leaderID].OnlyTrade` -> `_NN_ONLYTRADE` (sent to leader).
6. Reject if any `pMob[myindex].PartyList[i] != 0` -> `_NN_Dropped_Current_Party_First`.

---

### Business Rule: BR-PTY-05 … BR-PTY-07 Level window (PARTY_DIF)

**Overview**
Party formation is restricted to characters whose effective level difference is below `PARTY_DIF` (default 150), unless at least one side is level 1000 or above, or both sides share the same `ClassMaster` tier.

**Detailed description**
Effective level is *not* the raw level. `_MSG_AcceptParty.cpp:93-94` computes `leaderLV` and `myLV` by adding `MAX_CLEVEL` (199) or `MAX_CLEVEL + 1` to the base level for non-`MORTAL`/non-`ARCH` classes, i.e. Celestial tiers are offset above the mortal ladder so that a level 5 Celestial cannot group with a level 5 Mortal. The comparison at `:96` is `myLV >= leaderLV - PARTY_DIF && myLV < leaderLV + PARTY_DIF` — note the asymmetric strictness (`>=` below, `<` above), which combined with the extra `+1` in `leaderLV` produces a window that is one unit wider downwards.

`PARTY_DIF` is not a compile-time constant: it defaults to 150 (`Server.cpp:44`), is loaded from `gameconfig.txt` line 13 (`Server.cpp:1105`) and is mutable at runtime through the admin command `partydif` (`imple.cpp:1738-1745`). This is a live-tunable business rule, which means the same packet can be accepted or rejected depending on a config value changed hours earlier.

The exemption chain (`leaderLV >= 1000 || myLV >= 1000 || ClassMaster equality`) effectively disables the gate for the highest tier and for same-class players, which is what makes high-level grouping practical. The same window is re-implemented independently inside `_MSG_SendReqParty.cpp:68-71`, where the level computation omits the `+1`; the two copies can therefore disagree at the boundary by one level. In `SendReqParty` the failure also produces a message (`_NN_Party_Level_Limit`), whereas in `AcceptParty` the failure is the *else* branch of the whole success block, so a level failure and a battle-royale failure are indistinguishable to the client.

**Rule workflow**
1. Compute `leaderLV = level + (ClassMaster in {MORTAL, ARCH} ? 0 : MAX_CLEVEL + 1)`.
2. Compute `myLV    = level + (ClassMaster in {MORTAL, ARCH} ? 0 : MAX_CLEVEL)`.
3. Pass if `myLV >= leaderLV - PARTY_DIF` and `myLV < leaderLV + PARTY_DIF`.
4. Pass unconditionally if `leaderLV >= 1000` or `myLV >= 1000`.
5. Pass unconditionally if `ClassMaster` values are equal.
6. Otherwise send `_NN_Party_Level_Limit` (accept path) / same message from `SendReqParty` at `:83`.

---

### Business Rule: BR-PTY-08 Battle Royale area exclusion

**Overview**
While a Battle Royale event is running and the player stands inside the arena rectangle, party formation and invitation are refused outright — no message is sent.

**Detailed description**
`_MSG_AcceptParty.cpp:98-99` and `_MSG_SendReqParty.cpp:73-74` both evaluate `!BrState || conn >= MAX_USER || BRItem <= 0 || TargetX < 2604 || TargetY < 1708 || TargetX > 2648 || TargetY > 1744`. The rule fires only when `BrState` is non-zero, `BRItem > 0`, and the *inviter* is inside the rectangle `[2604..2648] x [1708..1744]`. Because the negation is used as a *success* condition, players outside the arena (the common case) satisfy it trivially.

The interesting consequence is silence: in the accept path the whole mutation block is nested under this negated condition, so a legitimate level-compatible join inside the arena does nothing at all — no message, no log, no crack point. The client is left with an open invitation window. `BrState` is a three-state machine (`Server.cpp:6310-6382`) so the block is time-bounded, but nothing on the server tells the players why.

The rule uses the *inviter's* coordinates (`pMob[conn]`, where `conn` is the accepting player in the accept path and the leader in the request path), not the party leader's. A leader standing outside the arena can therefore invite a player inside it — the request path checks the leader, the accept path checks the acceptor — which yields an inconsistent outcome for the same pair of players.

**Rule workflow**
1. Evaluate `BrState != 0 && BRItem > 0 && TargetX in [2604,2648] && TargetY in [1708,1744]` for the acting connection.
2. If true, skip the entire party mutation block (accept) or skip `SendReqParty` (request).
3. If false, continue to capacity/level handling.

---

### Business Rule: BR-PTY-09 Party capacity and slot semantics

**Overview**
A party holds at most 12 members (`MAX_PARTY`, `Basedef.h:168`). The leader is position 0 in the protocol; members occupy positions 1..12 in `PartyList[0..11]`.

**Detailed description**
`_MSG_AcceptParty.cpp:101-106` first counts occupied slots (variable `slot`), then `:108-109` finds the first free index. The two loops are not equivalent: the count is only used to decide whether the leader was previously alone (`if (!slot)`), while the actual insertion uses the first free index — so a gap left by a departing member is reused. If no gap exists, `i == MAX_PARTY` triggers `_NN_Partys_Full` (`:111-115`).

Position semantics are 1-based on the wire but 0-based in memory. After inserting at index `i`, the code increments `i` (`:120`) and then sends `SendAddParty(leaderID, myindex, i)`, i.e. the new member appears at wire position `i+1`. The leader is always emitted with position 0 (`:123`, `:128`). `SendAddParty` also special-cases `PartyID == 0` to mean "this is the leader" and `30000` otherwise (`SendFunc.cpp:1490-1493`), and halves HP/MP above 32,000 before serialising (`SendFunc.cpp:1496-1497`) because the party frame field is a 16-bit value.

The notification fan-out at `:131-142` is quadratic in party size: for each existing member it sends one packet to the newcomer and one to that member, on top of the four explicit sends. For a full 12-member party the join therefore costs on the order of 30 small writes, all through `cSock.SendOneMessage`, with each `SendAddParty` calling `CloseUser` on failure.

**Rule workflow**
1. Count non-zero entries in `pMob[leaderID].PartyList[0..MAX_PARTY-1]` into `slot`.
2. Find first index `i` with `PartyList[i] == 0`.
3. If `i == MAX_PARTY` -> `_NN_Partys_Full`, abort.
4. Set `PartyList[i] = requester`; set `pMob[requester].Leader = leaderID`.
5. If `slot == 0`: emit leader-to-self (position 0) and newcomer-to-self (position `i+1`).
6. Emit newcomer-to-leader (0) and leader-to-newcomer (`i+1`).
7. For each existing member `pt`: emit newcomer-to-`pt` and `pt`-to-newcomer.

---

### Business Rule: BR-PTY-10 … BR-PTY-12 Invitation request rules

**Overview**
Only a party leader may invite, only to a partyless in-game player, and the invitation is recorded server-side as the single pending nonce described in BR-PTY-01.

**Detailed description**
`_MSG_SendReqParty.cpp:29` requires `partyID == conn`, meaning the client must declare itself as the leader in the very same packet; `partyID` is then only used for the outbound `SendReqParty(targetID, partyID, partyID)` call. The check at `:34` rejects leaders who are already members of another party, which is the mirror of BR-PTY-04.

The target scan at `:87-97` iterates the *target's* `PartyList` rather than checking a single flag, which catches a target that is a leader with members (correct) but also means the candidate's own `Leader` field is checked separately at `:50`. Both are needed because a leader has `Leader == 0` while a member has `PartyList` entries of its own only if data is inconsistent.

Notably, `m->MobName` is terminated (`:27`) but never compared to anything, and `m->Target` / `m->Class` / `m->Level` / `m->MaxHp` / `m->Hp` / `m->PartyPos` fields of `MSG_SendReqParty` (`Basedef.h:1879-1896`) are entirely ignored — the server derives everything from `pMob[targetID]`. That is a sound choice, but it also means a client can send a well-formed request whose name field is nonsense and still get a valid invitation, so the name field must not be treated as an authorisation input anywhere downstream.

**Rule workflow**
1. `partyID = m->PartyID`; reject unless `partyID == conn`. Log `err,REQParty leader out of range`.
2. Reject if `pMob[partyID].Leader != 0` -> `_NN_Dropped_Current_Party_First`.
3. `targetID = m->unk`; reject unless in `[1, MAX_USER)`. Log `err,REQParty target out of range`.
4. Reject if `pUser[targetID].Mode != USER_PLAY` -> `_NN_Not_Connected`.
5. Reject if `pMob[targetID].Leader != 0` -> `_NN_Other_Partys_Member`.
6. Reject if `pUser[conn].OnlyTrade` -> `_NN_ONLYTRADE`.
7. Scan target's `PartyList`; any non-zero entry -> `_NN_Have_Own_Party_Already`.
8. Level window (BR-PTY-07) and BR guard (BR-PTY-08).
9. Set `LastReqParty`, emit `SendReqParty`.

---

### Business Rule: BR-PTY-13 … BR-PTY-14 Removal target resolution

**Overview**
`_MSG_RemoveParty` accepts an arbitrary target index but silently collapses it to "self" unless that target is a current member of the caller's party; whether the call then means "leave" or "kick/dissolve" depends on whether the caller is itself a member.

**Detailed description**
`_MSG_RemoveParty.cpp:26-27` first clamps invalid targets to `conn`. Then `:29-40` only accepts the packet-supplied target if it appears in `pMob[conn].PartyList[0..11]`; otherwise it is replaced by `conn`. This means a leader cannot kick a player who is not in its party (no error, silently becomes a self-removal) and a non-leader cannot name anybody else — the fallback makes the operation always "safe" but also silent on invalid input.

The dispatch at `:42-46` inspects `pMob[conn].Leader`: a non-zero, non-`-1` value means the caller is a member, so `RemoveParty(conn)` runs (leave, leaving the party otherwise intact); otherwise the caller is a leader and `RemoveParty(target)` runs (kick the resolved target, which defaults to the caller = dissolve). Note the condition excludes both `0` and `-1`, so a leader with `Leader == -1` takes the kick path — relevant because other code treats `-1` as "never set".

There is no rate limiting, no confirmation, and no check that the party actually exists. Because `RemoveParty(conn)` re-reads `pMob[conn].Leader`, a caller can also trigger a leave for a player who is not in a party, in which case `Server.cpp:7356-7362` logs `err,RemoveParty but worng leader` and returns — a normal-looking client action producing an error log entry.

**Rule workflow**
1. `target = m->Parm`; if outside `[1, MAX_USER)` -> `target = conn`.
2. If `target != conn`, scan `pMob[conn].PartyList`; if not found -> `target = conn`.
3. If `pMob[conn].Leader != -1 && pMob[conn].Leader != 0` -> `RemoveParty(conn)`.
4. Else -> `RemoveParty(target)`.

---

### Business Rule: BR-PTY-15 Dissolution, leadership inheritance and summon cleanup

**Overview**
`RemoveParty` (in `Server.cpp`) is the authoritative party mutation: it notifies the leaving member and the leader, removes the entry from the leader's roster, promotes a successor when the leader leaves, rewrites the persistent `Pista` party registry, deletes summons bound to the leaving player, and re-issues invitations so the party re-forms.

**Detailed description**
The function first validates the leader index and detects the "leader offline" case (`Server.cpp:7364-7370`), which forcibly clears `pMob[conn].Leader` instead of notifying anyone. In the normal member-leave path (`:7376-7400`) it sends `SendRemoveParty(conn, 0)` to the leaver, `SendRemoveParty(leader, conn)` to the leader, clears `pMob[conn].Leader`, then walks the roster clearing the entry and calling `DeleteMob(partyconn, 3)` for summons whose `Summoner` is the leaver, and for NPC-side summons owned by a user leader. Every remaining human member receives a `SendRemoveParty` echo.

When the leader leaves (`:7402-7467`) the code picks the first human member as the new leader (`novolider`), rewrites `Pista[s].Party[t].LeaderID/LeaderName` for all 7 parties x 3 slots, clears every roster entry and every member's `Leader` pointer, deletes or detaches summons, and then — critically — **synthesises an `_MSG_AcceptParty` packet for each remaining member** and feeds it back through `ProcessClientMessage(partyconn, (char*)&sm, TRUE)` at `:7451-7464`. That is the only place in the family where a handler is invoked by the server itself rather than by a client, and it is why the new leader must satisfy all of BR-PTY-01..09: the code sets `pMob[partyconn].LastReqParty = novolider` at `:7462` precisely to satisfy BR-PTY-01 for this synthetic invite.

The consequence for the component is that `Exec_MSG_AcceptParty` has afferent coupling 2 (dispatcher + `RemoveParty`), unlike every other handler's 1. It also means party re-formation after a leader logout is subject to the same level window and battle-royale checks as a normal join; if the successor fails them, the party silently dissolves.

**Rule workflow**
1. Read `leader = pMob[conn].Leader`; if outside `[0, MAX_MOB)` -> log and return.
2. If `leader` is a user not in `USER_PLAY` -> clear `pMob[conn].Leader`, return.
3. Notify leaver (`SendRemoveParty(conn, 0)`).
4. If `leader != 0`: notify leader, clear leaver's `Leader`, walk roster removing the leaver, delete summons, notify remaining members.
5. If `leader == 0` (leader leaving): pick `novolider`, rewrite `Pista` registry, clear roster and all members' `Leader`, delete summons, then for each remaining member set `LastReqParty = novolider` and re-dispatch a synthetic `_MSG_AcceptParty`.

---

### Business Rule: BR-PTY-16 Logout forces party removal

**Overview**
`_MSG_CharacterLogout` always calls `RemoveParty(conn)` before `CharLogOut(conn)`, regardless of whether the player is actually in a party.

**Detailed description**
`_MSG_CharacterLogout.cpp:23-24` is a two-line sequence with no guards. Calling `RemoveParty` for an unaffiliated player produces the error log described in BR-PTY-13, so every logout of a solo player writes an `err,RemoveParty but worng leader` line. This is noise, not a failure: `RemoveParty` returns without mutating anything.

The ordering matters: the party is dismantled *before* `CharLogOut` saves the character (`Server.cpp:7117`) and deletes the mob (`:7118`). If the order were reversed, `RemoveParty` would read a deleted mob slot. Because `CharLogOut` sets `Mode = USER_SELCHAR` only after `SaveUser`, the party notifications emitted by `RemoveParty` still reach the logging-out client, which is what allows the client's party frame to clear before the character-select screen appears.

The handler then calls `pUser[conn].cSock.SendMessageA()` (`:26`) to flush the outbound queue — the only handler in the family that explicitly flushes. Without it, the `_MSG_RemoveParty` and `_MSG_CNFCharacterLogout` packets could be lost when the socket is reset.

**Rule workflow**
1. `RemoveParty(conn)` — full BR-PTY-13/15 sequence.
2. `CharLogOut(conn)` — billing notify, trade teardown, `Mode = USER_SELCHAR`, `SaveUser(conn,1)`, `DeleteMob(conn,2)`, reset position to `SPX/SPY`, `SendClientSignal(_MSG_CNFCharacterLogout)`.
3. `cSock.SendMessageA()` — flush outbound buffer.

---

## 3.2 Detailed breakdown — Guild domain

### Business Rule: BR-GLD-01 … BR-GLD-02 Alliance requests

**Overview**
An alliance request is accepted only from a guild master acting for its own guild, and is forwarded verbatim to DBSrv where the alliance graph is actually stored.

**Detailed description**
`_MSG_GuildAlly.cpp:27-31` range-checks both `Guild` and `Ally` against `[1, 65536)`. The upper bound matches the `GuildInfo[65536]` array in `ProcessClientMessage.h` and the `MAX_GUILD * 16` name table (`Basedef.h:2460`), so the check is really "is this a valid guild identifier space". `:33` then requires both that the packet names the caller's own guild and that the caller holds `GuildLevel == 9` — the rank used everywhere in this codebase for "guild master".

The handler does **not** check whether the ally exists, whether it is already an ally, or whether the two guilds are at war. It builds a local `MSG_STANDARDPARM2` (`:36-44`), zeroes it, sets `Type = _MSG_GuildAlly`, `ID = conn`, `Parm1 = Guild`, `Parm2 = Ally`, and sends it to `DBServerSocket`. The response path is out of scope (it returns through `ProcessDBMessage.cpp`), which means the TMSrv-side rule is "authorise and delegate", and all semantic validation (duplicate ally, ally of ally, war conflict) is a DBSrv responsibility that is not visible from this component.

The practical implication for coupling is that the handler has neither local state nor rollback: if DBSrv rejects the alliance, nothing in TMSrv needs to be undone because nothing was written. Contrast with BR-GLD-10 (InviteGuild), which mutates TMSrv state *before* informing the DB.

**Rule workflow**
1. Read `Guild`, `Ally`; reject if either outside `[1, 65536)`.
2. Reject if `pMob[conn].MOB.Guild != Guild` or `GuildLevel != 9`.
3. Build zeroed `MSG_STANDARDPARM2`; set `Size`, `Type = _MSG_GuildAlly`, `ID = conn`, `Parm1 = Guild`, `Parm2 = Ally`.
4. `DBServerSocket.SendOneMessage(...)`.

---

### Business Rule: BR-GLD-03 … BR-GLD-04 War declaration

**Overview**
Identical in shape to BR-GLD-01/02, using `Parm1 = Guild`, `Parm2 = Enemy` and `Type = _MSG_War`.

**Detailed description**
`_MSG_War.cpp:27-34` repeats the four-line authorisation block from `GuildAlly` verbatim, including the `[1, 65536)` bounds and the `GuildLevel != 9` rejection. The handler then rebuilds the packet with `Type = _MSG_War` (`:40`) and forwards it. Both handlers construct a *fresh* struct rather than forwarding the inbound buffer, which means client-supplied extra bytes are discarded — a deliberate narrowing of the surface.

Because both handlers are pure forwarders, they are the cheapest place in the family to audit: there is no mutation, no logging (`_MSG_War` writes no log line at all, unlike `GuildAlly` which also writes none), and no error message on rejection. A rejection here produces a silent client-side stall, which materially complicates operational diagnosis: a guild master pressing "declare war" and getting nothing has no server-side trace.

The declaration also never validates that `Enemy != Guild`. Self-war is prevented only if DBSrv checks it. Likewise there is no check that a war is not already running between the pair.

**Rule workflow**
1. Read `Guild = Parm1`, `Enemy = Parm2`; reject if either outside `[1, 65536)`.
2. Reject unless `pMob[conn].MOB.Guild == Guild` and `GuildLevel == 9`.
3. Build `MSG_STANDARDPARM2` with `Type = _MSG_War`, `ID = conn`, `Parm1 = Guild`, `Parm2 = Enemy`.
4. Forward to `DBServerSocket`.

---

### Business Rule: BR-GLD-05 … BR-GLD-10 Guild recruitment

**Overview**
Recruitment is a paid, rank-gated, clan-restricted, weekday-limited operation that writes the target's guild membership directly on TMSrv and then broadcasts the changed appearance to the neighbourhood.

**Detailed description**
The eligibility matrix is evaluated in a strict order (`_MSG_InviteGuild.cpp:27-49`): index bounds, then invite-type bounds `[0,4)`, then "inviter is in a guild", then "target is guildless", then "same clan", then "inviter has any rank", then "type != 0 implies rank 9". Each rejection is silent — no message, no log. Only the Sunday block (`:58-62`) and the insufficient-funds case (`:69-70`, also silent) are distinguishable by the *absence* of further processing.

Pricing is two-tiered: 4,000,000 to invite a plain member, 100,000,000 for any non-zero invite type (which presumably denotes officer ranks or a higher-tier invitation). The fee is taken from the *inviter's* wallet (`:72`) and there is no refund path: if the subsequent DB guild-name lookup fails, the money is already gone but the target's membership was already written at `:75-76`, so the operation is effectively non-atomic with its own notification. There is also no distance check between inviter and target, no target consent, and no verification that the target is `USER_PLAY` — a mid-login `CUser` slot with a valid `MOB.Guild == 0` can be inducted.

The Sunday check deserves attention: `if (timeinfo->tm_wday == 0)` blocks Sunday but the message used is `_NN_NotEquip_Saturday` (`Language.h:409`). Day-of-week business rules therefore exist for recruitment, but the text is mislabelled, and Saturday itself is *allowed*. The rule is evaluated with `localtime` on the server host, so the effective window depends on server timezone.

**Rule workflow**
1. `TargetID = Parm1`, `InviteType = Parm2`.
2. Reject if `TargetID` outside `[1, MAX_USER)` or `InviteType` outside `[0,4)`.
3. Reject if `pMob[conn].MOB.Guild == 0`.
4. Reject if `pMob[TargetID].MOB.Guild != 0`.
5. Reject if `pMob[conn].MOB.Clan != pMob[TargetID].MOB.Clan`.
6. Reject if `pMob[conn].MOB.GuildLevel == 0`.
7. Reject if `InviteType != 0 && GuildLevel != 9`.
8. Reject if weekday == 0 -> `_NN_NotEquip_Saturday`.
9. `custo = InviteType == 0 ? 4,000,000 : 100,000,000`; reject if `Coin < custo`.
10. `Coin -= custo`; `SendEtc`.
11. `pMob[TargetID].MOB.Guild = pMob[conn].MOB.Guild`; `GuildLevel = 0`.
12. Log `etc,inviteguild guild:%d target_name:%s`.
13. `BASE_GetGuildName` -> `_SN_JOINGUILD` message to target.
14. `GetCreateMob(TargetID, &sm)`; `GridMulticast` so bystanders see the guild tag.

---

### Business Rule: BR-GLD-11 … BR-GLD-13 Guild deprivation (rank discipline)

**Overview**
A guild officer can strip a lower-ranked member of guild membership, but only inside a specific authority matrix; sub-leader ranks additionally require freeing one of three guild `Sub` slots, and the operation is pushed to DBSrv and visually re-broadcast.

**Detailed description**
`_MSG_Deprivate.cpp` is a four-line forwarder: it takes `m->Parm` as the target and calls `DoDeprivate(conn, m->Parm)` (`Server.cpp:7755`). All rules live in `Server.cpp`. The first pair of checks (`:7768`, `:7775`) establish that the target is a live player and that the acting rank is strictly greater than the target's rank; the third (`:7788`) requires the same guild. A guild master (9) therefore outranks everyone; a rank-6 officer cannot touch another rank-6.

The disqualification matrix at `:7791` is `(sub >= 6 && sub <= 8 && target < 6) || sub == 9`. That is: ranks 6-8 may remove only plain members (rank < 6), and rank 9 may remove anyone. Ranks 1-5 have no authority at all even though the earlier `sub > target` test would let them pass for lower ranks — they reach `Disqualified == FALSE` and get the `_NN_Deprivate` "you cannot" message at `:7836`. This two-stage design (comparison, then matrix) is the only place in the family where a permission model is expressed as data rather than as an inline condition.

Sub-leader bookkeeping (`:7795-7820`) is first-fit: if the target held rank 6-8, the code clears `GuildInfo[guild].Sub1`, else `Sub2`, else `Sub3` — note the `else if` chain, so only **one** slot is cleared per deprivation and it is always the lowest-numbered non-zero slot, not necessarily the slot belonging to that member. A `MSG_GuildInfo` is then pushed to DBSrv with the updated guild record. After the guild fields are cleared (`:7822-7823`) the mob is re-created and multicast (`:7825-7830`) so that the guild tag disappears for nearby clients. Only on the disqualification path does the target receive `_NN_Disqualified_By_Guild_Master` plus a score refresh (`:7841-7844`).

**Rule workflow**
1. Reject if `conn` outside `(0, MAX_USER]` (log `err,dodeprivate - conn`).
2. Reject if `target` outside `(0, MAX_USER]` or `Mode != USER_PLAY` -> `_NN_Not_Connected`.
3. Reject if `GuildLevel(conn) <= GuildLevel(target)` -> `_NN_Only_Guild_Master_Can`.
4. Reject silently if `pMob[conn].MOB.Guild != pMob[target].MOB.Guild`.
5. Compute `Disqualified = (sub in 6..8 && targetRank < 6) || sub == 9`.
6. If disqualified and target rank in 6..8: clear `Sub1` (else `Sub2`, else `Sub3`), log `etc,subdelete`, push `MSG_GuildInfo` to DBSrv.
7. If disqualified: `Guild = 0`, `GuildLevel = 0`, re-create and multicast mob.
8. If not disqualified: `_NN_Deprivate` to actor and return.
9. If disqualified: `SendScore(target)`, notify target and actor.

---

## 3.3 Detailed breakdown — Guild siege and town tax

### Business Rule: BR-CHL-01 Challenge NPC zone encoding

**Overview**
The challenge NPC carries its guild-zone identifier in `MOB.BaseScore.Level`; any value outside `[0, ValidGuild)` or equal to `5` makes the handler a no-op.

**Detailed description**
`_MSG_Challange.cpp:27-28` rejects targets outside `[1, MAX_MOB)` — note this allows player indices (`< MAX_USER`) as "target", which then read a player's level as a zone. `:30` reads `zone = pMob[target].MOB.BaseScore.Level`, and `:32-33` requires `0 <= zone < ValidGuild`. `ValidGuild` defaults to `MAX_GUILDZONE` = 5 (`Server.cpp:55`, `Basedef.h:148`).

`zone != 5` at `:35` is dead code given the previous bound (`zone < 5` already), which suggests the rule was originally `zone < 6` and a sixth "no zone" sentinel existed. The remainder of the function is therefore reachable only for zones 0..4, i.e. the five capturable towns.

The consequence is that the NPC dialogue system is driven entirely by a field (`BaseScore.Level`) that is normally a combat statistic. Any NPC spawned with a level in 0..4 becomes a challenge NPC; any challenge NPC misconfigured with level 5 or above becomes inert with no diagnostic. There is no verification that the NPC is actually a challenge NPC (no `Merchant` check, no mob-type check).

**Rule workflow**
1. `target = m->Parm`; reject unless `0 < target < MAX_MOB`.
2. `zone = pMob[target].MOB.BaseScore.Level`; reject unless `0 <= zone < ValidGuild`.
3. Reject if `zone == 5`.
4. Branch on `WeekMode` (BR-CHL-02..04).

---

### Business Rule: BR-CHL-02 WeekMode 4 — town-tax collection

**Overview**
During week mode 4 the challenge NPC becomes a tax counter: it reports the accumulated town tax, and the owning guild master can collect it either as coin (capped at 2,000,000,000) or as 1,000,000,000-coin items when the balance exceeds the coin cap.

**Detailed description**
The tax balance is stored in `pMob[GuildImpostoID[zone]].MOB.Exp`, i.e. the *experience* field of a dedicated treasury NPC (`Server.cpp:332` declares `GuildImpostoID[MAX_GUILDZONE]`). `_MSG_Challange.cpp:52` computes `Coin = (TaxExp + 1) / 1,000,000,000`, so one "coin item" (index 4011) is produced per full billion. `:54-59` reports the raw tax when it is zero or negative via `_I64D_TOWN_TAX`.

When `Coin == 0` the whole balance is paid as money: `:64-85` computes `fGold = tax + pMob[conn].MOB.Coin` and only proceeds if the result is `<= 2,000,000,000`; on success the treasury is zeroed, the wallet is set to the sum (a *cast* of a `long long` to `int` at `:73`), `SendEtc` refreshes the client and an `etc,imposto recolhido(1)` line is logged. When `Coin > 0`, the loop at `:97-106` calls `PutItem(conn, &Item)` once per billion with `Item.sIndex = 4011`, decrementing the treasury by exactly 1e9 per inserted item. `PutItem` returns 0 only when the inventory is full (`Server.cpp:731-735`), and on that condition the loop `return`s **without** the success message — leaving the treasury decremented only for the items actually inserted (safe) but with no user feedback.

Two asymmetries are worth recording. First, the `Coin == 0` branch transfers the *entire* balance including a potentially large remainder below 1e9, while the `Coin > 0` branch discards the remainder (it only removes exact billions). Second, the collection is authorised by `pMob[conn].MOB.Guild == g_pGuildZone[zone].ChargeGuild && GuildLevel == 9` (`:50`) — the guild master of the *owning* guild, not of the challenger guild. If the owning guild has no master online, the tax is uncollectable during that mode window.

**Rule workflow**
1. Require `WeekMode == 4`.
2. Require `MOB.Guild == g_pGuildZone[zone].ChargeGuild` and `GuildLevel == 9`; otherwise fall through to the tax report (`:112-113`).
3. `Coin = (treasuryExp + 1) / 1e9`.
4. If `treasuryExp <= 0` -> report `_I64D_TOWN_TAX`, return.
5. If `Coin == 0`: require `treasuryExp + wallet <= 2e9`; else `_NN_Cant_get_more_than_2G`.
6. If `Coin == 0` and allowed: `treasuryExp = 0`; `wallet = (int)(treasuryExp + wallet)`; `SendEtc`; log `imposto recolhido(1)`.
7. If `Coin > 0`: loop `Coin` times: `PutItem(4011)`; on failure return; `treasuryExp -= 1e9`; log `imposto recolhido(2)`.
8. Emit `_NN_GIVE_TOWN_TAX`.

---

### Business Rule: BR-CHL-03 … BR-CHL-04 Week mode state machine

**Overview**
`WeekMode` selects the NPC dialogue: 4 = tax collection, 5 = emit the challenge request window, 1/2/3 (and 0) = announce the current challenger and champion.

**Detailed description**
The condition at `_MSG_Challange.cpp:46` is `WeekMode && WeekMode != 1 && WeekMode != 2 && WeekMode != 3`, so modes 4 and any value above 5 take the special branch, while 0 (no mode) falls into the *announcement* branch along with 1/2/3. Inside the special branch, only 4 and 5 are implemented; any other value (6, 7, …) produces nothing at all.

The announcement branch (`:119-133`) reads `g_pGuildZone[zone].ChallangeGuild` and, if set, emits two lines: the challenger's bid (`_DS_S_Challanged` with `pChallangerMoney[zone]` and the challenger guild name) and the champion/challenger pair (`_SS_Champion_And_Challanger`). If no challenger is registered it emits `_SN_No_Challanger` with the champion name. Guild names come from `BASE_GetGuildName(ServerGroup, id, buffer)` (`:43-44`), i.e. cross-server guild identity is resolved through the group index.

`WeekMode == 5` (`:115-116`) does not start a challenge; it merely signals the client with `SendClientSignal(conn, 0, _MSG_ReqChallange)` so the client opens the dialogue that will eventually send `_MSG_ChallangeConfirm`. All three modes write nothing except in the tax branch, so `_MSG_Challange` is an information query except in week mode 4.

**Rule workflow**
1. If `WeekMode` is non-zero and not 1/2/3:
   - `WeekMode == 4` -> tax collection (BR-CHL-02); fall through to report on refusal.
   - `WeekMode == 5` -> `SendClientSignal(conn, 0, _MSG_ReqChallange)`.
   - other -> nothing.
2. Else (0/1/2/3):
   - If `g_pGuildZone[zone].ChallangeGuild != 0`: say challenger bid + champion/challenger names.
   - Else: say `_SN_No_Challanger` with champion name.

---

### Business Rule: BR-CHL-05 … BR-CHL-06 Challenge submission

**Overview**
`_MSG_ChallangeConfirm` maps the dialogue NPC to a zone via `Merchant - 6`, excludes zone 4, and delegates to `Challange()`, which enforces guild-master rank, town citizenship, an existing guild name, at least 100 guild fame, and a 100-fame fee per attempt.

**Detailed description**
`_MSG_ChallangeConfirm.cpp:30` derives the zone as `pMob[target].MOB.Merchant - 6`, so dialogue NPCs 6..10 correspond to zones 0..4. `:32` then requires `0 <= zone < ValidGuild && zone != 4`, formally excluding the capital zone from being challenged through this path even though `Challange()` itself also excludes zone 4 (`Server.cpp:2490-2491`). The double exclusion means zone 4 is unreachable through either entry point.

`Challange()` (`Server.cpp:2479-2570`) is the authoritative rule set: target must be an NPC (`>= MAX_USER`), zone must be valid and not 4, the actor must be `GuildLevel == 9`, must not already be the owning guild, must have `extra.Citizen == ServerIndex + 1` (citizenship of *this* server's town), must have a non-empty guild name in `g_pGuildName[Group][Server][usGuild]`, and must have `GuildInfo[Guild].Fame >= 100`. Only after all of that does it deduct 100 fame (`:2549`) and submit `MSG_GuildInfo` to DBSrv (`:2553`).

The bid logic at `:2560-2567` is the actual contested state: if the actor's remaining fame (`coin` is reassigned to the *pre-deduction* fame at `:2547`, then the bid value is snapshotted into `coin`) exceeds the current `pChallangerMoney[zone]`, the zone's challenger guild and money are overwritten and both `WriteChallanger()` and `WriteGuild()` persist the change to disk. Note that the fee is deducted *before* the comparison, and that the challenger is replaced only on a strictly higher bid — so a losing bid still burns 100 fame. The mutated fame is also pushed to the DB *before* the bid comparison, which means the fame deduction is committed even when the bid loses.

**Rule workflow**
1. `target = Parm1`; reject unless `0 < target < MAX_MOB`.
2. `zone = Merchant - 6`; reject unless `0 <= zone < ValidGuild && zone != 4`.
3. Call `Challange(conn, target, 0)`.
4. Inside: reject if `target < MAX_USER`; `ZoneChall = pMob[target].MOB.BaseScore.Level`; reject if invalid or `== 4`.
5. Reject if `GuildLevel != 9` -> `_NN_Only_Guild_Master_can`.
6. Reject if `ChargeGuild == Guild` -> `_NN_Champions_Cant_Challange`.
7. Reject if `extra.Citizen != ServerIndex + 1` -> `_DN_ANOTHER_TOWNSPEOPLE`.
8. Reject if guild name empty -> `_NN_Only_Named_Guild`.
9. Reject if `GuildInfo[Guild].Fame < 100` -> `_DN_Need_1000000_For_Challange`.
10. `coin = Fame`; `Fame -= 100`; push `MSG_GuildInfo` to DBSrv; `_NN_Challanged`.
11. If `coin > pChallangerMoney[zone]`: set `ChallangeGuild`/`pChallangerMoney`, persist via `CReadFiles::WriteChallanger()` and `WriteGuild()`.

---

## 3.4 Detailed breakdown — NPC shop domain

### Business Rule: BR-SHP-01 … BR-SHP-02 Shop catalog emission

**Overview**
A shop list is emitted only for a live, in-game player looking at an NPC merchant inside `VIEWGRID`, and only for merchant types `1` (catalog 1) and `19` (catalog 3); all other merchant types produce no response.

**Detailed description**
`_MSG_REQShopList.cpp:24-29` applies the standard alive/in-game guard with crack point 10/22. `:33` rejects any target below `MAX_USER` (so a player cannot be used to open a shop) and any target at or above `MAX_MOB`; the log line is `err,MSG_ReqShopList target range`. `:39-43` requires `pMob[target].MOB.Merchant != 0` and otherwise sends `_NN_He_Is_Not_Merchant`, which is the only user-visible rejection in the handler.

The visibility test at `:45-48` calls `GetInView(conn, target)` (`GetFunc.cpp:640-647`), which compares `TargetX/TargetY` against `VIEWGRIDX/VIEWGRIDY` = 33 (`Basedef.h:95-96`). A player who moved away between opening the dialogue and requesting the list gets a silent drop rather than an error. The mapping at `:50-54` is an `if/else if` with no final `else`: merchants whose `Merchant` value is 2..18 or 20..255 get nothing, which suggests the catalog set is intentionally narrow but is undocumented.

`SendShopList` (`SendFunc.cpp:1369-1414`) reads the NPC's `Carry` array with a 9-column layout (`invpos = (i % 9) + ((i / 9) * MAX_SHOPLIST)`) and stamps `Tax` from `g_pGuildZone[village].CityTax` when the NPC is inside a village, else 0. The tax stamped here is the same value later validated by BR-SHP-10 in `ReqBuy`, which is what makes shop prices self-consistent.

**Rule workflow**
1. Reject if `Hp == 0 || Mode != USER_PLAY` (crack 10/22).
2. `target = m->Target`; reject unless `MAX_USER <= target < MAX_MOB`.
3. Reject if `MOB.Merchant == 0` -> `_NN_He_Is_Not_Merchant`.
4. Reject silently if `GetInView(conn, target) == 0`.
5. If `Merchant == 1` -> `SendShopList(conn, target, 1)`.
6. Else if `Merchant == 19` -> `SendShopList(conn, target, 3)`.
7. Else nothing.

---

### Business Rule: BR-SHP-03 … BR-SHP-08 Player shop registration

**Overview**
Opening a player shop requires a live in-game player with no active trade or listing, on a server where `NewbieEventServer != 0`, standing inside one of the five villages but outside a specific blocked rectangle, with every listed slot satisfying price, blacklist, flag and inventory-consistency rules.

**Detailed description**
The precondition chain is `_MSG_SendAutoTrade.cpp:24-49`. The state guard (crack 10/88) is followed by three trade-state rejections: an existing `Trade.OpponentID` cancels *both* sides and returns (`:32-37`), `TradeMode` produces `_NN_CantWhenAutoTrade` (`:39-43`), and `NewbieEventServer == 0` produces `_NN_NEWBIEEVENTSERVER` (`:45-49`). That last check is counter-intuitive: it means player shops are **disabled on normal servers** and enabled only on the newbie-event server. It is the single most consequential gating rule in the shop domain and it reads as an inverted flag (the message name suggests the opposite intent), so this rule is documented here as observed behaviour with medium confidence about intent.

Location rules (`:51-68`) reject the rectangle `x in [2123,2148] && y in [2139,2157]` (a plaza area) with `_NN_OnlyVillage`, then require `BASE_GetVillage(x,y)` to return 0..4 (`Basedef.cpp:4258-4267`, which returns 5 for "outside any city limit"). Both failures also call `RemoveTrade(conn)` — which is what clears the *listing* state, not just an escrow trade, because `RemoveTrade` also zeroes `AutoTrade` and resets `CarryPos` to -1 (`Server.cpp:7329-7332`).

Per-slot validation (`:73-96`) iterates `MAX_AUTOTRADE` = 12 slots (`Basedef.h:80`): negative or >1,999,999,999 prices are rejected outright (`return`, no message); an item without a price, or a price without an item, is rejected; seven specific item indices plus the 526..531 range are blacklisted (508, 3993, 747, 509, 522, 446 and 526-531 — 747 is the same "poison pill" item used by every crafting matcher); `CarryPos` must be a valid `MAX_CARGO` index; `EF_NOTRADE` items are rejected with `_NN_Cant_MoveItem`; and finally the advertised `STRUCT_ITEM` must byte-match `pUser[conn].Cargo[CarryPos]`, which is the anti-forgery control (a client cannot advertise a different item from the one in its warehouse).

Registration (`:98-119`) stamps `Tax = g_pGuildZone[Village].CityTax`, terminates the title at `MAX_AUTOTRADETITLE-2`, copies the title and the whole `MSG_SendAutoTrade` into `pUser[conn].AutoTrade`, sets `TradeMode = 1`, echoes the listing to the seller via `SendAutoTrade(conn, conn)`, and multicasts a `MSG_CreateMobTrade` with `Score.Con = 0` (so the shop NPC renders with zero constitution and cannot be mistaken for a combatant).

**Rule workflow**
1. Reject if `Hp <= 0 || Mode != USER_PLAY` (crack 10/88, `RemoveTrade`).
2. If `Trade.OpponentID` -> `RemoveTrade` both, return.
3. If `TradeMode` -> `_NN_CantWhenAutoTrade`.
4. If `NewbieEventServer == 0` -> `_NN_NEWBIEEVENTSERVER`.
5. If inside blocked rectangle -> `_NN_OnlyVillage`.
6. If `BASE_GetVillage` outside 0..4 -> `_NN_OnlyVillage`.
7. For each slot 0..11: bounds/price/item consistency; blacklist; `CarryPos` bounds; `EF_NOTRADE`; cargo match.
8. `Tax = CityTax(village)`; sanitise title; copy into `AutoTrade`; `TradeMode = 1`.
9. `SendAutoTrade(conn, conn)`; `GetCreateMobTrade`; `Con = 0`; `GridMulticast`.

---

### Business Rule: BR-SHP-09 Listing visibility

**Overview**
A buyer obtains a seller's listing only when the seller is still in auto-trade mode and both are within `VIEWGRID` of each other.

**Detailed description**
`_MSG_ReqTradeList.cpp:26-31` applies the alive/in-game guard with crack 10/87. `:33-34` validates the seller index, and `:36-37` returns silently when `TradeMode == 0` — a seller who just closed the shop produces a silent drop. The proximity test at `:39-41` is written inline rather than through `GetInView`, but is equivalent (`VIEWGRIDX/Y` on both axes).

On success it calls `SendAutoTrade(conn, autoID)` (`SendFunc.cpp:527-558`), which re-validates both modes, copies `pUser[seller].AutoTrade` into a fresh packet with `ID = ESCENE_FIELD` and `Index = seller`, and pushes it with a **size mismatch**: `AddMessage((char*)&sm, sizeof(MSG_UpdateCarry))` uses a different struct's size (`:556`). Whether this truncates or over-reads depends on the relative sizes of `MSG_SendAutoTrade` and `MSG_UpdateCarry`; this is recorded as a defect risk (RISK-06) rather than a business rule, because the business intent (send the listing) is clear.

The failure path logs `err,too far from autotrade - _MSG_ReqTradeList`. There is no rate limit: a client within range can poll listings as fast as it can send packets.

**Rule workflow**
1. Reject if `Hp == 0 || Mode != USER_PLAY` (crack 10/87).
2. `autoID = m->Parm`; reject unless `0 < autoID < MAX_USER`.
3. Reject if `pUser[autoID].TradeMode == 0`.
4. If within `VIEWGRIDX/Y` -> `SendAutoTrade(conn, autoID)`.
5. Else log `err,too far from autotrade`.

---

### Business Rule: BR-SHP-10 … BR-SHP-14 Purchase and settlement

**Overview**
A purchase is accepted only when the buyer's packet reproduces the seller's listing exactly (position, price, tax, item) *and* the seller's warehouse still holds that item; settlement then moves the item, applies a commission of `(price / 100) * tax` for prices at or above 100,000, credits the seller the net amount, routes the commission to the town treasuries, persists the seller, and notifies both parties.

**Detailed description**
The guard block (`_MSG_ReqBuy.cpp:24-42`) rejects dead/out-of-game buyers with crack 10/86 plus a side-effect `RemoveTrade`, rejects when the buyer is itself auto-trading, and rejects when the buyer is in an escrow trade. The quadruple match is the authorisation core: `itemTax == pUser[target].AutoTrade.Tax` (`:71`), `itemPrice == pUser[target].AutoTrade.Coin[itemPos]` (`:74`), the packet item equals the advertised item (`:77`), and the seller's actual `Cargo[StorageSlot]` equals the advertised item (`:83`). Requiring both the listing and the warehouse to match closes the TOCTOU window where a seller could swap the warehoused item after advertising.

Funds checks are asymmetric: the buyer must hold `itemPrice` (`:90`), and the seller's *warehouse* coin plus the price must not exceed 2,000,000,000 (`:96-102`), but the seller's *pocket* coin is credited with only the net amount and is guarded separately by a silent `if (pUser[target].Coin < 2000000000)` at `:158-159` — meaning a seller already at the cap loses the proceeds silently. The buyer's free-slot search at `:106-118` uses `MAX_CARRY-4` as the bound but compares the resulting index against `MAX_CARRY`, so a completely full inventory is not detected (RISK-04).

Commission applies only for prices at or above 100,000 (`:140-144`): `imposto = (itemPrice / 100) * itemTax`, `price_end = itemPrice - imposto`. Because `itemTax` is a per-village `CityTax` (likely a small percentage), the formula is "percent of price, applied per 100 units" and can exceed the price if a village tax were misconfigured above 100. Below 100,000 the seller receives the full price and no tax is routed.

Tax routing (`:165-180`) credits `pMob[GuildImpostoID[target_village]].MOB.Exp` when that NPC is alive and belongs to the zone's charge guild, capped at 200,000,000,000, **and then independently credits zone 4's treasury** under the same conditions. A purchase in any village therefore pays the local guild and the capital guild. Whether that double-credit is intended is not determinable from the code; it is recorded as an ambiguity with medium confidence that it is a copy/paste artefact.

**Rule workflow**
1. Reject if `Hp == 0 || Mode != USER_PLAY` (crack 10/86, `RemoveTrade`).
2. Reject (with `RemoveTrade`) if `TradeMode` or `Trade.OpponentID`.
3. `targetID/price/tax/pos` from packet; reject if `targetID` out of `[1,MAX_USER)`.
4. Reject if buyer or seller positions are outside `VIEWGRIDX/Y`.
5. Reject if `pos` outside `[0, MAX_AUTOTRADE)`; `StorageSlot = seller.AutoTrade.CarryPos[pos]` outside `[0, MAX_CARGO)`.
6. Reject unless `tax == seller.AutoTrade.Tax` and `price == seller.AutoTrade.Coin[pos]`.
7. Reject (with `RemoveTrade`) unless packet item == listing item and seller cargo == listing item.
8. Reject if `buyer.Coin < price` -> `_NN_Not_Enough_Money`.
9. Reject if `seller.Coin + price > 2e9` -> `_NN_Cant_get_more_than_2G`.
10. Find free carry slot; reject if `i == MAX_CARRY` -> `_NN_You_Have_No_Space_To_Trade`.
11. Reject if seller village outside `[0,5)` -> `_NN_OnlyVillage`.
12. Copy item to buyer carry; `SendItem`; log `autotrade_buy,...`.
13. If `price >= 100,000`: `imposto = (price/100)*tax`; `price_end = price - imposto`; else `imposto = 0`, `price_end = price`.
14. Clear seller listing (`CarryPos = -1`, item zeroed, `Coin[pos] = 0`), clear seller cargo, `SendItem`.
15. `buyer.Coin -= price`; if `seller.Coin < 2e9` then `seller.Coin += price_end`.
16. `SendEtc(buyer)`, `SendCargoCoin(seller)`, `SaveUser(seller, 1)`.
17. Credit `GuildImpostoID[village]` and `GuildImpostoID[4]` treasuries with `imposto`, capped at 200,000,000,000.
18. `GridMulticast(_MSG_ItemSold, Parm1 = seller, Parm2 = pos)`; `SendClientMessage(seller, _NN_ItemSold)`.

---

## 3.5 Detailed breakdown — Ranking domain

### Business Rule: BR-RNK-01 Whisper-block and parameter validation

**Overview**
Duel requests require a valid target and type, and are refused when the target has disabled whispers.

**Detailed description**
`_MSG_ReqRanking.cpp:27-28` bounds the type to `[0,4]` and the target to `[1, MAX_USER)`. `:30-34` rejects targets whose `pUser[tDuel].Whisper` flag is set, using `_NN_Deny_Whisper` — the same flag other subsystems use for whisper privacy is thus the de-facto "do not disturb" flag for duels. There is no reciprocal check on the *requesting* player's own whisper setting.

The type space is documented in `Basedef.h:2135` as `0:1vs1 1:5vs5 2:10vs10 3:Ok`, and the handler treats `4` as the acceptance type. That means the constant comment and the handler disagree (4 is not in the comment), so the protocol mapping must be derived from the handler, not the header — an ambiguity recorded for downstream consumers.

No distance check exists in this handler: `DoRanking` teleports both combatants (`Server.cpp:8067-8068`), so proximity is irrelevant to the duel itself, only to the challenge handshake UX.

**Rule workflow**
1. `tDuel = Parm1`, `DuelParm = Parm2`.
2. Reject if `DuelParm` outside `[0,4]` or `tDuel` outside `[1, MAX_USER)`.
3. Reject if `pUser[tDuel].Whisper != 0` -> `_NN_Deny_Whisper`.
4. Branch on `DuelParm == 4` (accept) vs otherwise (request).

---

### Business Rule: BR-RNK-02 Challenge request eligibility

**Overview**
A duel request is relayed to the target only when the type is 1 or 2, or when at least one of the two players holds a guild rank; the server stores `RankingTarget`/`RankingType` on the requester and forwards the request.

**Detailed description**
The condition at `_MSG_ReqRanking.cpp:38` is `if (DuelParm != 1 && DuelParm != 2 || pMob[conn].MOB.GuildLevel && pMob[tDuel].MOB.GuildLevel)`. With `&&` binding tighter than `||`, this reads: relay if (type is not 1 and not 2) **or** (both players have a non-zero `GuildLevel`). So a 1v1 (type 0) and type 3 always relay, while team duels (1, 2) relay only when both players are guild-ranked. The likely intent (from the `Basedef.h` comment about 5vs5/10vs10) is the opposite: team duels should require guild membership. Documented here as observed behaviour; the intent is ambiguous and this is flagged in RISK-09.

The successful path (`:40-47`) stores `pUser[conn].RankingTarget = tDuel` and `RankingType = DuelParm`, then rewrites the packet in place: `Parm1 = conn` (the requester), `ID = tDuel` (the recipient), and pushes it to `pUser[tDuel].cSock`. Rewriting `ID` to the *recipient* is unusual — for server-generated packets `ID` conventionally identifies the subject — but it is what makes the target's client display the requester's challenge. Nothing is sent to the requester itself, so the requester's UI depends on its own optimistic state.

Note that the request is not rate-limited and `RankingTarget` is single-valued: a player challenging two targets in sequence silently invalidates the first challenge, and the first target's eventual acceptance fails BR-RNK-03.

**Rule workflow**
1. If `DuelParm != 4`:
   - If `DuelParm` is 1 or 2 and both players have `GuildLevel != 0`, skip (no relay).
   - Else set requester `RankingTarget`/`RankingType`; rewrite `Parm1 = conn`, `ID = tDuel`; push to target; return.
2. Fall through to acceptance branch otherwise.

---

### Business Rule: BR-RNK-03 … BR-RNK-04 Acceptance and duel start

**Overview**
Acceptance is a second packet of type 4 that verifies the target is in-game, that it had previously targeted the accepter, and that no duel is running; it then starts the duel with fixed durations and arena teleports.

**Detailed description**
`_MSG_ReqRanking.cpp:53-64` requires `pUser[tDuel].Mode == USER_PLAY` and `pUser[tDuel].RankingTarget == conn` — the reciprocity check that proves the two packets belong to the same negotiation. `RankingProgress` must be 0, otherwise both sides receive `_NN_Battle_In_Progress`. `:66-75` additionally requires the target's stored `RankingType` to be in `[0,3]`, refreshes both clients with `SendEtc`, calls `DoRanking(type, conn, tDuel)`, and logs `etc,reqranking <a> vs <b> type:<n>`.

`DoRanking` (`Server.cpp:8052-8110`) has two modes. For `tDuel == 0` (1v1) it records both names and levels as titles, teleports the two players to `Ranking2X/Y` and `Ranking1X/Y` respectively, announces `_NN_Battle_Started`, sends `_MSG_StartTime` with `2 * 121 - 2` (a 240-second visual countdown for a 121-second clock), sets `RankingProgress = 1`, `Ranking1/2`, and `RankingTime = 121`. For team duels it requires both players to be in a guild, requires the guilds to differ, sets `RankingProgress = tDuel + 1`, resolves guild names as titles, and uses a 301-second clock. Team duels therefore start with a `RankingProgress` of 2..4, which is the value the periodic `ProcessRanking` uses to know which bracket is running.

Because the duel state lives in globals (`RankingProgress`, `Ranking1/2`, `nRanking1/2`, `RankingTime`), only one duel can exist per TMSrv process at a time, regardless of server population. That is a hard concurrency ceiling imposed by the design, not by any validation rule.

**Rule workflow**
1. Require `DuelParm == 4`.
2. Reject if `pUser[tDuel].Mode != USER_PLAY`.
3. Reject if `pUser[tDuel].RankingTarget != conn`.
4. Reject (both notified) if `RankingProgress != 0` -> `_NN_Battle_In_Progress`.
5. Require `pUser[tDuel].RankingType` in `[0,3]`.
6. `SendEtc(conn)`, `SendEtc(tDuel)`, `DoRanking(type, conn, tDuel)`, log.

---

## 3.6 Detailed breakdown — Travel and world domain

### Business Rule: BR-TEL-01 Water-zone teleport block

**Overview**
Teleporting is refused when the player stands at grid block `(1964, 1772)` — i.e. `x/4 == 491 && y/4 == 443` — with the message "only by water scroll".

**Detailed description**
`_MSG_ReqTeleport.cpp:25` compares integer-divided coordinates rather than a rectangle, so an entire 4x4 tile block is blocked. The message `_NN_Only_By_Water_Scroll` implies the area is a lake crossing that has its own item-based travel mechanic handled elsewhere (presumably in `_MSG_UseItem`, out of scope).

The handler performs **no** `USER_PLAY` or HP check before this rule, so a dead player or a mid-login session can reach the teleport logic. Only after the water check does it call `GetTeleportPosition` and then compare coin. Dead players teleporting is prevented only indirectly: `DoTeleport` calls `GetEmptyMobGrid`, which may fail for a dead mob, and any failure silently skips the teleport.

Because the coordinate test precedes destination resolution, the block applies even to teleports that would not cross the lake — e.g. `GetTeleportPosition` at that location returns no mapping and the destination stays unchanged, so no teleport would happen anyway (BR-TEL-05). The check is therefore mostly a UX message rather than a real gate.

**Rule workflow**
1. `posx = TargetX`, `posy = TargetY`.
2. If `posx/4 == 491 && posy/4 == 443` -> `_NN_Only_By_Water_Scroll`, return.
3. Continue to BR-TEL-02.

---

### Business Rule: BR-TEL-02 … BR-TEL-05 Fee-based gate travel

**Overview**
Gate travel resolves a hard-coded source-block-to-destination mapping, charges 700 coin for the three capital gates (free for the clan that owns zone 4), credits half the fee to the zone-4 guild treasury, and applies the teleport only if the destination actually changed.

**Detailed description**
`GetTeleportPosition` (`GetFunc.cpp:658-899`) is a 60-branch table keyed on `(x & 0xFFFC, y & 0xFFFC)` — i.e. the 4x4 aligned block. Each branch sets a destination with `rand() % 2` or `% 3` jitter so arrivals are spread. Only three branches set `Charge = 700`: Armia→Noatum (`2116,2100`), Azran→Noatum (`2480,1716`), Erion→Noatum (`2456,2016`). Every other branch leaves `Charge = 0`, so inter-city, dungeon and field transitions are free. Special branches consume state: the Kefra gate (`2364,3892`) requires `extra.KefraTicket > 0`, decrements it, and reports the remaining count (`:868-878`), and the reverse Kefra gate requires `KefraLive != 0` (`:881`).

The clan exemption at `_MSG_ReqTeleport.cpp:33-36` sends execution to the `label_tel` label when `g_pGuildZone[4].Clan == pMob[conn].MOB.Clan`, skipping both the coin check and the treasury credit. This is a positional `goto` across a declaration, which is legal here only because no object with a non-trivial destructor is declared between the jump target and the label — but it is the only `goto` in the 40-handler family and makes the fee logic hard to read.

Fee settlement (`:38-51`) deducts only when `reqcoin > 0`, and credits `pMob[GuildImpostoID[4]].MOB.Exp` with `reqcoin / 2` when that treasury NPC is alive, belongs to the zone-4 charge guild, and its balance is below 2,000,000,000,000. The remaining half is destroyed. Teleport application (`:54-60`) is conditional on the destination differing from the current position, which is why an unmapped source produces no teleport and no charge (the `reqcoin` is 0 there anyway). The log line records the *post-teleport* coordinates (`pMob[conn].TargetX/Y` after `DoTeleport`), so it shows the destination rather than the origin.

**Rule workflow**
1. Resolve `reqcoin = GetTeleportPosition(conn, &posx, &posy)`.
2. If `g_pGuildZone[4].Clan == pMob[conn].MOB.Clan` -> jump to step 6.
3. If `reqcoin > pMob[conn].MOB.Coin` -> `_NN_Not_Enough_Money`, return.
4. If `reqcoin > 0`: `Coin -= reqcoin`; `SendEtc`; credit zone-4 treasury with `reqcoin / 2` (cap 2e12).
5. Fall through to teleport step.
6. If `(posx, posy) != (TargetX, TargetY)`: `DoTeleport(conn, posx, posy)`; log `etc,reqteleport ...`.

---

### Business Rule: BR-TEL-06 City binding (`ChangeCity`)

**Overview**
The handler recomputes the player's bound city from their current coordinates and stores it in bits 6-7 of `MOB.Merchant`, preserving the low 6 bits.

**Detailed description**
`_MSG_ChangeCity.cpp:23-32` reads `TargetX/TargetY`, asks `BASE_GetVillage(x, y)` (which returns the index of the first `g_pGuildZone` whose `CityLimitX1..2 / Y1..2` rectangle contains the point, else 5), and if the result is 0..4 rewrites `Merchant` as `(Merchant & 0x3F) | (city << 6)`. The low six bits are the actual merchant/NPC identity, so a player who is currently serving as a shop NPC preserves that identity while changing city.

There is no guard at all: no `USER_PLAY`, no HP check, no verification that the packet came from the client rather than the server, and no check that the city actually changed. The handler therefore trusts the player's own coordinates, which are authoritative on the server side (`pMob[].TargetX/Y` are server-held, not client-supplied) so this is safe, but it is the only handler that mutates persistent character state with zero validation.

The city value is what `DoRecall` later uses to pick a respawn point (`Server.cpp:7562-7565`: `CityID = (Merchant & 0xC0) >> 6`, then `g_pGuildZone[CityID].CitySpawnX/Y` plus jitter), so a stale or out-of-range city would index the array unchecked there. This handler is the reason that index is normally in range; the coupling is implicit and undocumented.

**Rule workflow**
1. `tx = TargetX`, `ty = TargetY`, `city = BASE_GetVillage(tx, ty)`.
2. If `0 <= city <= 4`: `Merchant = (Merchant & 0x3F) | (city << 6)`.
3. Log `etc,changecity city:%d X:%d Y:%d` (IP field logged as 0).

---

### Business Rule: BR-TEL-07 Respawn (`Restart`)

**Overview**
A restart revives the character at exactly 2 HP, clears the accumulated crack-point counter, refreshes score/HP and snaps the player to a faction spawn point inside the Armia field, or to the recall point elsewhere.

**Detailed description**
`_MSG_Restart.cpp:24` requires `Mode == USER_PLAY`, else `SendHpMode` and return. `:26` sets `MOB.CurrentScore.Hp = 2` — a deliberate "revive at the brink" value rather than a percentage, so the player is immediately vulnerable. `:28` resets `NumError = 0`, which is the counter incremented by `AddCrackError` and used to force a logout at 2,000,000,000 (`Server.cpp:692-703`); a restart therefore **clears the anti-cheat penalty counter**, which is a resilience trade-off worth noting (RISK-10).

The spawn logic at `:33-40` is a two-branch faction snap inside the Armia field rectangle `[1017..1290] x [1911..2183]`: clan 7 (Armia) is moved to `(1061..1063, 2129..2131)`, clan 8 (Azran) to `(1237..1239, 1966..1968)`, everyone else to `DoRecall(conn)`. `DoRecall` (`Server.cpp:7558-7600+`) computes the city spawn from `Merchant` bits 6-7 with `rand() % 15` jitter, overrides it with the guild spawn if the player's guild owns a zone, and finally overrides everything with `(2112±3, 2042±3)` for mortal characters below `FREEEXP` (the newbie zone).

Order of operations matters: `SendScore` and `SendSetHpMp` are emitted (`:30-31`) **before** the teleport, so the client receives the new HP/MP in the old position and then a separate action packet for the move. The final `SendEtc(conn)` (`:45`) resynchronises the wallet/score block.

**Rule workflow**
1. Reject if `Mode != USER_PLAY` -> `SendHpMode`.
2. `Hp = 2`; `NumError = 0`.
3. `SendScore(conn)`; `SendSetHpMp(conn)`.
4. If inside Armia field and `Clan == 7` -> `DoTeleport(1061+r%, 2129+r%)`.
5. Else if inside Armia field and `Clan == 8` -> `DoTeleport(1237+r%, 1966+r%)`.
6. Else `DoRecall(conn)`.
7. Log `etc,restart char:%s hp:%d`; `SendEtc(conn)`.

---

## 3.7 Detailed breakdown — Trade lifecycle domain

### Business Rule: BR-TRD-01 … BR-TRD-03 Trade abort

**Overview**
`QuitTrade` requires a live in-game player, tears down both sides of the escrow, and re-broadcasts the player's PK state to the neighbourhood.

**Detailed description**
`_MSG_QuitTrade.cpp:22-27` applies the alive/in-game guard with crack point 10/17; the HP test is `<= 0` here whereas most siblings use `== 0`, a cosmetic difference that matters only for negative HP values. `:29-30` then calls `RemoveTrade(pUser[conn].Trade.OpponentID)` and `RemoveTrade(conn)` — note the first call passes whatever `OpponentID` held, and `RemoveTrade` silently returns for out-of-range indices (`Server.cpp:7321-7322`), so a player with no opponent just performs a self-clean.

`RemoveTrade` is heavier than its name: it zeroes `Trade`, resets all `InvenPos` to -1, zeroes `AutoTrade` and resets `CarryPos` to -1, sends signal 900 (`Server.cpp:7337`), and if `TradeMode` was set it multicasts a fresh `CreateMob` so the shop NPC disappears from the world (`:7339-7351`). A "quit trade" therefore also closes a player shop — a cross-domain side effect that is not obvious from either handler's name.

The tail (`:32-53`) is a byte-for-byte duplicate of the PK-state broadcast in `_MSG_PKMode.cpp:38-60`: build `MSG_STANDARDPARM`, `Type = _MSG_PKInfo`, `ID = conn`, compute `state = guilty || PKMode || (RvR rect) || (Castle rect) || (GTorre rect)`, `GridMulticast`. Two copies of the same rectangle arithmetic means the PK-eligibility zones are defined twice and must be kept in sync manually.

**Rule workflow**
1. Reject if `Hp <= 0 || Mode != USER_PLAY` (crack 10/17).
2. `RemoveTrade(OpponentID)`; `RemoveTrade(conn)`.
3. Build `_MSG_PKInfo`; compute `state` (guilty || PKMode || inside RvR/Castle/GTorre rectangles); `GridMulticast`.

---

### Business Rule: BR-TRD-04 Trade cancellation as a precondition

**Overview**
Six handlers cancel any active trade (often both sides) instead of proceeding, whenever the requested inventory operation would race with an escrow.

**Detailed description**
The pattern appears with three different shapes. `_MSG_SplitItem.cpp:36-40` cancels only the caller (`RemoveTrade(conn)`) and returns. `_MSG_DeleteItem.cpp:36-41` cancels *both* endpoints before returning. `_MSG_CombineItem.cpp:30-36` cancels only the caller but only when an `InvenPos` is out of range. `_MSG_PutoutSeal.cpp:30-34` cancels only the caller. `_MSG_ReqBuy.cpp:38-42` cancels only the caller, while `_MSG_SendAutoTrade.cpp:32-37` cancels both.

The inconsistency is a real hazard: an escrow is a two-sided structure (`pUser[a].Trade.OpponentID == b` and vice versa). Cancelling one side leaves the other with a dangling `OpponentID`, which is later cleaned up only because `RemoveTrade` on the peer is also called from `CharLogOut` (`Server.cpp:7104-7110`) and from `RemoveTrade(seller)` in other paths. In the interim, the peer's `Trade.OpponentID` points at a player who no longer considers itself trading, so BR-PVP-02's mutual check (`pUser[opponent].Trade.OpponentID == conn`) fails and the peer's PK-mode toggle does not cancel the (already dead) trade.

Because none of these cancellations send a message, the client that initiated the split/delete/combine learns about the cancellation only from `RemoveTrade`'s signal 900. There is no log line either, so an operator cannot reconstruct why a trade ended.

**Rule workflow**
1. On entry to the inventory operation, test `pUser[conn].Trade.OpponentID`.
2. Cancel per handler policy (self only, or both).
3. Return without performing the operation.
4. Peer state converges later via `RemoveTrade`, `CharLogOut`, or timeout.

---

## 3.8 Detailed breakdown — Character session domain

### Business Rule: BR-CHR-01 … BR-CHR-03 Character creation

**Overview**
Character creation is only accepted from a connection sitting on the character-select screen, only with a name matching the 4-15 character alphanumeric policy, and it hands off to DBSrv with the session moved to `USER_WAITDB`.

**Detailed description**
`_MSG_CreateCharacter.cpp:24-25` zeroes the last two bytes of `MobName` before anything else, which both guarantees termination and truncates any 15-character name by one byte (`NAME_LENGTH` is 16). `:27-34` requires `pUser[conn].Mode == USER_SELCHAR`; failure logs `err,createchar not user_selchar <conn> <mode>` and sends `_MSG_NewCharacterFail` with an explicit `SendClientSignal`.

`BASE_CheckValidString` (`Basedef.cpp:2522-2551`) enforces length `[4, 16)` and allows `ISALPHA`, `ISDIGIT`, `'-'` and any byte with the high bit set (multi-byte characters are skipped two bytes at a time, `:2538-2542`), while rejecting 15 specific reserved words (`Reino`, `subcreate`, `create`, `gritar`, `king`, `kingdom`, `getout`, `gfame`, `expulsar`, `summonguild`, `summon`, `time`, `relo`, `stopally`, `stopwar`). Those are the admin command names handled by `imple.cpp`, so the rule is an anti-impersonation control for command names rather than a general profanity filter. The list is case-sensitive and matches the commands exactly.

On success the handler rewrites `Type = _MSG_DBCreateCharacter`, sets `ID = conn`, moves the session to `USER_WAITDB` **before** the DB send (`:41-43`) so that a duplicate packet arriving in the same tick cannot start a second creation, and logs `etc,createchar name:%s %d %d`. The DB response is handled in `ProcessDBMessage.cpp` (out of scope), which is also where slot conflicts and per-account character limits are actually enforced — none of those rules exist here.

**Rule workflow**
1. Terminate `MobName` at `NAME_LENGTH-1` and `-2`.
2. Reject unless `Mode == USER_SELCHAR` -> log + `_MSG_NewCharacterFail`.
3. Reject unless `BASE_CheckValidString(MobName)` -> `_MSG_NewCharacterFail`.
4. `Type = _MSG_DBCreateCharacter`; `ID = conn`; `Mode = USER_WAITDB`.
5. `DBServerSocket.SendOneMessage(sizeof(MSG_CreateCharacter))`.
6. Log `etc,createchar name:%s %d %d`.

---

### Business Rule: BR-CHR-04 … BR-CHR-05 Character deletion

**Overview**
Deletion requires the character-select screen and is forwarded to DBSrv with the session parked in `USER_WAITDB`; unlike creation, the name is not validated locally.

**Detailed description**
`_MSG_DeleteCharacter.cpp:24` terminates `MobName` at `NAME_LENGTH-1`, `:26` requires `Mode == USER_SELCHAR`, and `:28-33` rewrites `Type = _MSG_DBDeleteCharacter`, sets `ID`, parks the session in `USER_WAITDB` and forwards the struct including its `Slot` and `Password[16]` fields (`Basedef.h:1269-1278`).

The failure branch (`:38-44`) is the only place in the family that emits a hard-coded English string instead of a `g_pMessageStringTable` entry: `SendClientMessage(conn, "Deleting Character. wait a moment.")`. That message is shown when the player is *not* on the select screen, which is precisely when the message is misleading. It also writes `err,delchar not user_selchar <conn> <mode>`.

Notably the log line uses `pMob[conn].MOB.MobName` rather than the packet's `MobName`, so at character-select time the logged name is whatever the mob slot last held — for a fresh connection that is empty. Deleting a character therefore produces an audit line that does not identify the deleted character; the DB side must be consulted for that.

**Rule workflow**
1. Terminate `MobName` at `NAME_LENGTH-1`.
2. If `Mode == USER_SELCHAR`: rewrite type/ID, park `USER_WAITDB`, forward `sizeof(MSG_DeleteCharacter)`, log `etc,delchar`.
3. Else: `SendClientMessage("Deleting Character. wait a moment.")`, log `err,delchar not user_selchar`.

---

### Business Rule: BR-CHR-06 … BR-CHR-08 Logout, secure-code and capsule forwarders

**Overview**
Three session handlers are pure orchestration: logout (save + notify), account-secure (numeric token change) and capsule info (capsule listing) — each ends in a DB round-trip or a client confirmation.

**Detailed description**
`_MSG_CharacterLogout.cpp` is analysed under BR-PTY-16; its session half is `CharLogOut` (`Server.cpp:7085-7125`), which rejects non-`USER_PLAY` sessions with `_MSG_CNFCharacterLogout` instead of an error, notifies billing when `IsBillConnect && CHARSELBILL == 0`, tears down the peer's trade if it points back at this connection, writes `SelChar.Score[Slot].Level` from the live score, calls `SaveUser(conn, 1)`, `DeleteMob(conn, 2)`, resets position to the stored `SPX/SPY`, clears `Mode`, and signals `_MSG_CNFCharacterLogout`.

`_MSG_AccountSecure.cpp:25-30` validates `conn` and rewrites `m->ID = conn` in place before forwarding the *same buffer* to `DBServerSocket` with `Type` untouched (`_MSG_AccountSecure`, which is flagged for both directions, `Basedef.h:1248`). The `ChangeNumeric` and `NumericToken[6]` fields (`Basedef.h:1254-1256`) are forwarded verbatim; there is no local validation of the token format, so an empty or partially filled token reaches the DB.

`_MSG_CapsuleInfo.cpp:21-30` casts the packet to `MSG_STANDARDPARM`, reads `Parm` into a local `Index` that is then **never used**, sets `ID = conn` and `Type = _MSG_DBCapsuleInfo`, and forwards `sizeof(MSG_STANDARDPARM)`. The dead local is a code smell, not a rule: the ellipsis is that the capsule index is passed through `Parm` untouched, so the DB is authoritative for capsule ownership and index bounds — no TMSrv-side rule exists.

**Rule workflow (logout)**
1. `RemoveParty(conn)` (BR-PTY-16).
2. `CharLogOut(conn)`: mode check -> billing -> peer trade teardown -> `SelChar` level sync -> `SaveUser` -> `DeleteMob` -> position reset -> `Mode = USER_SELCHAR` -> `_MSG_CNFCharacterLogout`.
3. `cSock.SendMessageA()` flush.

**Rule workflow (AccountSecure)**
1. Reject if `conn` outside `(0, MAX_USER)`.
2. `m->ID = conn`.
3. `DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_AccountSecure))`.

**Rule workflow (CapsuleInfo)**
1. `Index = m->Parm` (unused).
2. `m->ID = conn`; `m->Type = _MSG_DBCapsuleInfo`.
3. `DBServerSocket.SendOneMessage(..., sizeof(MSG_STANDARDPARM))`.

---

## 3.9 Detailed breakdown — Inventory and economy domain

### Business Rule: BR-INV-01 … BR-INV-03 Item deletion

**Overview**
`DeleteItem` removes an inventory item, but the index supplied by the client is only range-checked, never compared to the item actually present in the slot.

**Detailed description**
`_MSG_DeleteItem.cpp:24-28` bounds `Slot` to `[0, MAX_CARRY-4)` = `[0,60)` and `sIndex` to `[1, MAX_ITEMLIST)`. The `sIndex` value is then never read again: `:43` reads the *actual* item code from `pMob[conn].MOB.Carry[m->Slot]` for the audit log, and `:48` memsets that slot. So a client that sends the correct slot with a stale or arbitrary (but in-range) index still destroys whatever is there. This is permissive by design (the slot is the identity) but it means the `sIndex` field provides no safety: a desynchronised client deleting slot 5 will destroy a different item than the one the user saw.

The `MAX_CARRY-4` bound is the significant protection: indices 60..63 are the equipment region inside the same `Carry` array (and `KILL_MARK` is index 63, `Basedef.h:166`), so the bound prevents deleting equipped gear or the PK marker. Any future change to the equipment layout would silently widen this hole.

The guard order (`:30-34` mode, then `:36-41` trade) means a player in a trade who tries to delete gets the trade cancelled instead of the item deleted — a protective interlock, but also a way to cancel a trade by attempting a deletion. Failure to *send* an item update after clearing means the client is expected to have already removed the item optimistically; if the packet is lost, client and server disagree until the next inventory sync.

**Rule workflow**
1. Reject if `Slot` outside `[0, MAX_CARRY-4)`.
2. Reject if `sIndex` outside `[1, MAX_ITEMLIST)`.
3. Reject if `Mode != USER_PLAY` -> `SendHpMode`.
4. If `Trade.OpponentID`: `RemoveTrade(opponent)`, `RemoveTrade(conn)`, return.
5. `BASE_GetItemCode(&Carry[Slot], buf)`; `ItemLog("deleteitem, <code>")`.
6. `memset(&Carry[Slot], 0, sizeof(STRUCT_ITEM))`.

---

### Business Rule: BR-INV-04 … BR-INV-07 Stack splitting

**Overview**
Only specific stackable item classes may be split, the split amount must be strictly less than the stack (so a whole stack cannot be "split"), at least one free inventory slot is required, and the remainder stays in the origin slot.

**Detailed description**
The whitelist at `_MSG_SplitItem.cpp:44-46` accepts indices 413, 412, 419, 420 and the range 2390..2419. These are the consumable/ammunition classes used elsewhere (`413` appears in the Lindy and Shany recipes, `4127` in Alchemy), so the rule is "only multi-count items can be split" expressed as an item-index list rather than as "the item has an `EF_AMOUNT` effect". The redundant duplicate bound at `:54-58` (`Num < 0 || Num > 120`) re-tests what `:27-28` already rejected, and neither bound allows `Num == 120`, so a 120-stack cannot be split into 120+0 (consistent with the "strictly less" rule).

The free-slot scan at `:63-67` counts empty slots across `pMob[conn].MaxCarry` and aborts below 1 with no message. Because `PutItem` itself checks for space (`Server.cpp:725-735`), this pre-check is advisory only — and it scans to `MaxCarry` while `PutItem` scans to `min(MaxCarry, MAX_CARRY)`, so the two agree only when `MaxCarry <= MAX_CARRY` (which is the normal case).

Execution (`:75-93`) is careful about auditing before mutation and about the amount arithmetic: `BASE_SetItemAmount(&Carry[slot], amount - m->Num)` when `amount > 1`, and a memset when `amount == 1` — the second branch is unreachable because `:72` already rejected `amount <= 1`. The new item is built from `sIndex` and `Num` only, deliberately dropping `stEffect` beyond the amount, and finally both the origin slot and the new slot are sent to the client.

**Rule workflow**
1. Reject if `Slot` outside `[0, MAX_CARRY-4)`.
2. Reject if `Num` outside `(0, 120)`.
3. Reject if `Mode != USER_PLAY` -> `SendHpMode`.
4. If `Trade.OpponentID` -> `RemoveTrade(conn)` and return.
5. Reject unless `Carry[Slot].sIndex` in {413, 412, 419, 420} ∪ [2390, 2419].
6. `amount = BASE_GetItemAmount(&Carry[Slot])`.
7. Reject if `amount == 0 || amount == 1 || amount <= Num`.
8. Count free slots in `[0, MaxCarry)`; reject if `< 1`.
9. Log `splititem, itemindex:%d amount:%d num:%d`.
10. Set origin amount to `amount - Num`; build new item with `sIndex` and `Num`; `PutItem`; `SendItem` origin.

---

### Business Rule: BR-INV-08 … BR-INV-11 Warehouse deposit and withdrawal

**Overview**
Coin moves between the wallet (`pMob[].MOB.Coin`) and the warehouse (`pUser[].Coin`) in both directions under a 2,000,000,000 ceiling on the destination, with the client echoed a rewrite of its own request.

**Detailed description**
Both handlers apply the same guard: `Hp == 0 || Mode != USER_PLAY` -> `SendHpMode` plus a crack point (`10/1` for deposit, `10/2` for withdraw). The deposit rule (`_MSG_Deposit.cpp:33`) requires the wallet to cover the amount *and* the amount to be in `[0, 2e9]`; the withdraw rule (`_MSG_Withdraw.cpp:33`) mirrors it on the warehouse balance. The destination ceiling is checked as `tcoin = coin + destination` in `[0, 2e9]` (`:35-37` and `:35-37` respectively).

Both then write the *request packet* back to the client with a rewritten `ID`: `ESCENE_FIELD` (30000) for deposit (`:42`) and the literal `30000` for withdraw (`:42`) — the same value expressed two ways, a consistency smell. The echo is deliberate protocol design: the client uses the returned `Parm` as the acknowledged movement amount. `SendCargoCoin(conn)` (`SendFunc.cpp:1233`) then pushes the authoritative warehouse coin block.

Two weaknesses are structural. First, `int tcoin = coin + pUser[conn].Coin` uses signed 32-bit arithmetic where both operands can approach 2^31-1; the sum overflows to a negative value which the `>= 0` test usually catches, but a wrap that lands back in `[0, 2e9]` would pass (RISK-03). Second, there is no rate limit and no per-tick budget: 1-coin deposits can be issued as fast as the client can send, each triggering a full `SendCargoCoin` and a `Log` line, which is a cheap server-side amplification vector.

**Rule workflow (deposit)**
1. Reject if `Hp == 0 || Mode != USER_PLAY` -> `SendHpMode`, `AddCrackError(conn, 10, 1)`.
2. `coin = m->Parm`.
3. Require `pMob[conn].MOB.Coin >= coin && coin >= 0 && coin <= 2,000,000,000`.
4. `tcoin = coin + pUser[conn].Coin`; require `0 <= tcoin <= 2,000,000,000` -> else `_NN_Cant_get_more_than_2G`.
5. `MOB.Coin -= coin`; `pUser[conn].Coin = tcoin`.
6. Echo `MSG_STANDARDPARM` with `ID = ESCENE_FIELD`; `SendCargoCoin`; log `etc,deposito no bau`.
7. If step 3 fails -> `_NN_Cant_Deposit_That_Much`.

**Rule workflow (withdraw)**
1. Reject if `Hp == 0 || Mode != USER_PLAY` -> `SendHpMode`, `AddCrackError(conn, 10, 2)`.
2. Require `pUser[conn].Coin >= coin && 0 <= coin <= 2e9`.
3. `tcoin = coin + MOB.Coin`; require `0 <= tcoin <= 2e9` -> else `_NN_Cant_get_more_than_2G`.
4. `MOB.Coin = tcoin`; `pUser[conn].Coin -= coin`.
5. Echo with `ID = 30000`; `SendCargoCoin`; log `etc,retirada do bau`.
6. If step 2 fails -> `_NN_Cant_Withdraw_That_Much`.

---

### Business Rule: BR-INV-12 … BR-INV-15 Seal capsule creation (`PutoutSeal`)

**Overview**
A "seal capsule" (item 3443) with a bound name can be used to spawn a persisted capsule entity: the handler validates the source pointer, the item identity and the effect, validates the new name, forcibly logs the character out, and forwards a `MSG_DBOutCapsule` to DBSrv.

**Detailed description**
The guard sequence (`_MSG_PutoutSeal.cpp:24-51`) is the strictest in the family: mode first, then trade cancellation, then a dead-player branch that both reports `SendHpMode`, records crack point `1/15`, *and* resends the source item to resynchronise the client for any of the three item stores (`ITEM_PLACE_CARRY`, `ITEM_PLACE_CARGO`, else `ITEM_PLACE_EQUIP`). That resend is the "dead character resend item" block the architecture report identified as copy-pasted across handlers.

Grid validation (`:53-57`) checks `GridX/GridY` against `MAX_GRIDX/MAX_GRIDY` (4096) and uses `CrackLog` rather than a message, so an out-of-range destination is treated as an integrity violation. Item resolution (`:59-66`) delegates to `GetItemPointer` (`Basedef.cpp:2340-2370`), which validates both the container type and the position and additionally rejects items whose `sIndex` is outside `[0, MAX_ITEMLIST)` — so the handler does get real bounds enforcement through that helper.

The identity gates are `item->sIndex == 3443` (`:68`) and `stEffect[0].cEffect != 0` (`:74`), both `CrackLog`-guarded; the effect presumably carries the capsule's payload or bound owner. The name is then truncated twice and validated with `BASE_CheckValidString` (`:83`), and on failure the item is simply resent. On success the order is: `CharLogOut(conn)` **first** (`:88`), then build and send `MSG_DBOutCapsule` with `Slot`, source, destination, grid, `WarpID` and a 12-byte `strncpy` of the name (`:90-111`). Logging the character out before the DB write means the capsule creation is committed by DBSrv against an already-saved character — if the DB write fails, the item has been consumed (it was never re-sent) and the character is already logged out.

**Rule workflow**
1. Reject if `Mode != USER_PLAY` -> `SendHpMode`.
2. If `Trade.OpponentID` -> `RemoveTrade(conn)`, return.
3. If `Hp == 0`: `SendHpMode`, `AddCrackError(1,15)`, resend source item by type, return.
4. Reject if `GridX >= MAX_GRIDX || GridY >= MAX_GRIDY` -> `CrackLog(" Drop grid, out of range")`.
5. `item = GetItemPointer(...)`; reject if NULL with `err,use item fail source`.
6. Reject if `item->sIndex != 3443` -> `CrackLog("outseal no 3443")`.
7. Reject if `stEffect[0].cEffect == 0` -> `CrackLog("outseal no effect")`.
8. Truncate `MobName`; reject if `!BASE_CheckValidString` (resend item).
9. `CharLogOut(conn)`.
10. Build `MSG_DBOutCapsule`; `strncpy(..., 12)`; log `etc,outcapsule`; forward to DBSrv.

---

## 3.10 Detailed breakdown — Crafting domain

### Business Rule: BR-CRF-01 … BR-CRF-02 Inventory identity proof and the 747 poison pill

**Overview**
Every combine handler re-reads the declared inventory slots and requires a byte-for-byte match with the client-declared items; item index 747 anywhere in the input set invalidates every recipe.

**Detailed description**
The identity block is literally duplicated in all eight combine handlers: loop `i` over `MAX_COMBINE` (8), skip zero-index entries, bounds-check `m->InvenPos[i]` against `pMob[conn].MaxCarry`, and `memcmp` the `STRUCT_ITEM` at that slot against `m->Item[i]`. Any mismatch logs `err,msg_Combine<Recipe> - item remove or changed`, sends `_MSG_CombineComplete` with result 0, and aborts. A bad `InvenPos` instead calls `RemoveTrade(conn)` and aborts with no message. This block is the family's only general anti-forgery control for crafting: the client cannot claim to be combining items it does not have.

Note what it does **not** verify: that the same slot is not declared twice. Two `InvenPos` entries pointing at one item both pass `memcmp` if the item matches both declared entries, and the consumption loop then memsets the same slot twice — destroying one item while the recipe believed it had two. Recipes mitigate this only indirectly (e.g. Ailyn's `GetMatchCombineAilyn` requires two identical items, which a duplicate slot can satisfy). This is recorded as RISK-05.

The `747` check (`GetFunc.cpp:37-41, 169-173, 219-223, 263-267, 379-383, 429-...`, `498-504`) returns 0/-1 from every matcher if any input slot holds item 747, blocking the recipe even when all other conditions pass. 747 also appears in the auto-trade blacklist (BR-SHP-06), so it is a globally "non-interactive" item; its presence in recipe inputs is treated as an exploit signature rather than a legitimate ingredient.

**Rule workflow**
1. For each `i` in `[0, MAX_COMBINE)`:
   - Skip if `m->Item[i].sIndex == 0`.
   - Reject (with `RemoveTrade`) if `m->InvenPos[i]` outside `[0, MaxCarry)`.
   - Reject (with `_MSG_CombineComplete = 0`) if `memcmp(&Carry[InvenPos], &m->Item[i], sizeof(STRUCT_ITEM)) != 0`.
2. Call the recipe matcher; reject if it returns 0 (or -1 for Alquimia).
3. Proceed to consumption per handler.

---

### Business Rule: BR-CRF-03 … BR-CRF-04 Ancient combination (`CombineItem`)

**Overview**
An Arch-grade item (unique 41..49) plus a sanctification stone and up to six jewels produces the "ancient" variant whose identity is `itemList[base].Extra + jewelOffset`, at a success rate of 1 plus a configured bonus per stone sanctification level.

**Detailed description**
`GetMatchCombine` (`GetFunc.cpp:33-104`) validates: no 747 anywhere; base index in `[1, MAX_ITEMLIST)`; stone index in `[1, MAX_ITEMLIST)`; base `nUnique` in `[41, 49]`; base `Extra > 0`; base `EF_MOBTYPE != 3`; then for each input from index 2 to 7 with a valid index, `EF_POS != 0`, the base item level must be `<=` the additive item level, and the additive must have sanctification exactly 7, 8 or 9, each adding `g_pAnctChance[0]`, `[1]` or `[2]` respectively (loaded from config at `CReadFiles.cpp:578-584`). The returned rate is `1 + Σ bonuses`, so with six stones at the highest tier the rate can be large; the roll is against 0..99.

`_MSG_CombineItem.cpp:55-62` deletes **all** declared inputs before the roll, then logs the combination, then rolls `rand() % 115` normalised to 0..99 (`:80-83`). On success (`:84-119`) it reads `extra = g_pItemList[item[0].sIndex].Extra`, requires `0 < extra < MAX_ITEMLIST`, computes `joia = m->Item[1].sIndex - 2441` and requires `0..3`, then writes the result into `Carry[InvenPos[0]]`: `memcpy(&Carry[ipos].sIndex, &m->Item[0], sizeof(STRUCT_ITEM))`, `sIndex = joia + extra`, `BASE_SetItemSanc(..., 7, 0)` (ancient level 7, zero success counter), then `SendItem` and `_MSG_CombineComplete = 1`. The `memcpy` targets `&Carry[ipos].sIndex` (a `short*`) with a full-struct length — it works because `sIndex` is the first field, but it is a type-punned copy that a static analyser flags.

On failure (`:134-143`) the inputs are already gone, the client gets `_MSG_CombineComplete = 2` and a message containing the roll and the target rate (`_rand/combine`). Leaking the exact roll values to the client is deliberate (the same pattern appears in Tiny/Shany/Ailyn) and lets players verify the RNG, which is unusual but consistent across the crafting suite.

**Rule workflow**
1. BR-CRF-01 identity proof.
2. `combine = GetMatchCombine(m->Item)`; reject if 0 -> `_NN_Wrong_Combination` + complete 0.
3. Delete every declared input; `SendItem` each.
4. Log `*** Item combine anct. ***` + per-item codes.
5. `_rand = rand()%115`; if `>= 100` subtract 15.
6. If `_rand <= combine || LOCALSERVER`: validate `extra`, `joia`; write result; `BASE_SetItemSanc(7,0)`; message with `(_rand/combine)`; complete 1; `SendItem`.
7. Else: message with `(_rand/combine)`; complete 2; log failure.

---

### Business Rule: BR-CRF-05 … BR-CRF-06 Agatha effect transfer

**Overview**
Agatha moves the three effect pairs from a donor item into an Arch base item of the same slot and item level >= 4, both at sanctification >= 9, consuming four units of item 3140; the donor survives and the base is destroyed on failure.

**Detailed description**
`GetMatchCombineAgatha` (`GetFunc.cpp:377-425`) requires six non-empty inputs (0..5), base `EF_MOBTYPE == ARCH`, donor `EF_ITEMTYPE` in `{MORTAL, 0}`, donor `EF_ITEMLEVEL >= 4`, `nPos` equality between base and donor, sanctification >= 9 on both, and `Item[2..5] == 3140`. The rate is `g_pAgathaBase + Grade(donor) * 5 + (donor level == 5 ? 30 : 1)`, i.e. strongly dependent on the donor's grade and level.

Consumption (`_MSG_CombineItemAgatha.cpp:55-65`) skips index 1 — the donor is preserved through the roll — and memsets indices 0, 2, 3, 4, 5. On success (`:90-118`) it writes `Carry[InvenPos[0]].sIndex = m->Item[0].sIndex` (restoring what was just deleted), copies `m->Item[1].stEffect[0..2]` into the base's effect slots, applies `BASE_SetItemSanc(..., 7, 0)`, clears `InvenPos[1]` and `SendItem`s both slots, then completes with 1 and `_NN_Processing_Complete`. On failure (`:120-129`) indices 0, 2..5 are already gone, the donor at index 1 remains, and the complete code is 2 with `_NN_CombineFailed`.

The asymmetry is the defining property of this recipe: failure costs the base item and the four 3140 catalysts but leaves the player with the donor (which still holds the effects). Since the donor's effects are the valuable part, a failed Agatha is recoverable — the player can retry with a new base. This is materially gentler than Ancient (BR-CRF-04), which loses everything, and materially different from Tiny (BR-CRF-23), which keeps the donor but charges money.

**Rule workflow**
1. BR-CRF-01 identity proof.
2. `combine = GetMatchCombineAgatha(...)`; reject if 0.
3. Delete inputs 0, 2, 3, 4, 5 (keep 1); `SendItem` each.
4. Log `*** Item combine agatha. ***` + codes.
5. Roll `rand()%115` normalised.
6. Success: restore base index; copy donor effects 0..2; `BASE_SetItemSanc(7,0)`; clear donor slot; `SendItem` both; `_NN_Processing_Complete`; complete 1.
7. Failure: `_NN_CombineFailed`; complete 2.

---

### Business Rule: BR-CRF-07 … BR-CRF-09 Ailyn attribute merge

**Overview**
Ailyn merges two identical grade-5..8 items plus a 1774 catalyst and up to four class-matched jewels; it costs 50,000,000 per attempt and another 50,000,000 on success, and produces sanctification level 10 whose success counter encodes the jewel index.

**Detailed description**
`_MSG_CombineItemAilyn.cpp:45-52` checks the wallet for 50,000,000 and reports `_DN_D_Cost` if short, then `GetMatchCombineAilyn` (`GetFunc.cpp:261-375`) requires seven non-empty inputs: two identical `sIndex` values with equal `Grade`, `Item[2] == 1774`, base `nPos` in {2,4,8,16,32,64,128,192}, and jewels in 3..6 that must be 2441 (grade 5), 2442 (grade 6), 2443 (grade 7), 2444 (grade 8), or — for grades outside 5..8 — all equal to `Item[3]`. Each accepted jewel adds `g_pAilynBase` to the rate.

Consumption (`:64-74`) deletes inputs 2..7 (keeping both base items for the roll), then unconditionally deducts 50,000,000 and calls `SendEtc` (`:73-74`). The roll is the standard normalised `rand()`; on success (`:99-141`) the branch `if (rand() % 1 == 0)` is **always true**, so `ipos1` is always `InvenPos[0]` and `ipos2` always `InvenPos[1]` — the apparent randomisation is dead code. Effects 0..2 are copied from `ipos2` to `ipos1`, `BASE_SetItemSanc(&Carry[ipos1], 10, m->Item[3].sIndex - 2441)` sets ancient level 10 with the jewel ordinal as the success counter, `ipos2` is cleared, and then the code deducts **another** 50,000,000 (`:124`) and calls `SendEtc` again.

The total cost on success is therefore 100,000,000 while the displayed/gated price is 50,000,000. On failure the cost is 50,000,000 and only the catalysts (2..7) are lost; both base items survive because the deletion loop starts at index 2. This makes Ailyn the only recipe where a failure preserves *both* primary inputs — an economically significant asymmetry.

**Rule workflow**
1. BR-CRF-01 identity proof.
2. Reject if `MOB.Coin < 50,000,000` -> `_DN_D_Cost`, complete 0.
3. `combine = GetMatchCombineAilyn(...)`; reject if 0.
4. Delete inputs 2..7; `Coin -= 50,000,000`; `SendEtc`; log.
5. Roll normalised `_rand`.
6. Success: `ipos1 = InvenPos[0]`, `ipos2 = InvenPos[1]`; copy effects; `BASE_SetItemSanc(10, jewelIdx)`; clear `ipos2`; `Coin -= 50,000,000`; `SendEtc`; `_NN_Processing_Complete`; complete 1; `SendItem`.
7. Failure: `_NN_CombineFailed`; complete 2.

---

### Business Rule: BR-CRF-10 … BR-CRF-11 Alchemy (`CombineItemAlquimia`)

**Overview**
Alchemy is restricted to class 3, matches one of ten fixed reagent recipes, and yields a skill item `3200 + recipeId` at a rate derived from `CurrentScore.Special[2]`.

**Detailed description**
`_MSG_CombineItemAlquimia.cpp:47` rejects when `GetMatchCombineAlquimia` returns -1 **or** `pMob[conn].MOB.Class != 3`. The class test is checked after the recipe, so an invalid recipe and a wrong class are indistinguishable to the client (both produce only `_MSG_CombineComplete = 0`, with no message). `GetMatchCombineAlquimia` (`GetFunc.cpp:498-559`) validates eight inputs, rejects 747, and then matches ten exact reagent triples/quads (413+2441+2442 = Sagacidade, 413+2443+2442 = Resistencia, 3x4127 = Revelacao, 4127+4127+697 = Recuperacao, 412+2441+2444 = Absorcao, 412+2444+2443 = Protecao, 612 sanc>=9 +2441+2442 = Poder, 612+613+614+615 = Armazenagem, 614 sanc>=9 +2443+2444 = Precisao, 615 sanc>=9 +3x697 = Magia). Inputs 3..7 are allowed to be empty for the triples.

Consumption (`:53-60`) deletes all declared inputs before the roll, then `:82` computes `chance = (pMob[conn].MOB.CurrentScore.Special[2] + 1) / 6`. `Special[]` is a character stat; index 2 is therefore a luck-like attribute that linearly improves alchemy up to `(max+1)/6`. Whether that can exceed 99 depends on the stat cap, which is not defined in this component — with the standard W2 cap of ~100 the chance tops out around 16, making alchemy the least likely craft in the suite.

On success (`:84-104`) the result is written into `InvenPos[0]` with `sIndex = 3200 + combine` and all three effect slots zeroed, then sent with `_MSG_CombineComplete = 1` and `_NN_Processing_Complete`. Failure keeps the inputs destroyed and uses `Log` (not `ItemLog`) for the failure record — the only crafting handler whose failure path is not audited through `ItemLog`, which is a forensic gap.

**Rule workflow**
1. BR-CRF-01 identity proof.
2. `combine = GetMatchCombineAlquimia(...)`; reject if `-1` or `MOB.Class != 3` -> complete 0.
3. Delete all declared inputs; `SendItem` each; log.
4. `chance = (Special[2] + 1) / 6`.
5. Roll normalised `_rand`.
6. Success: `Carry[InvenPos[0]].sIndex = 3200 + combine`; zero effects; `SendItem`; complete 1.
7. Failure: `_NN_CombineFailed`; complete 2; `Log` (not ItemLog).

---

### Business Rule: BR-CRF-12 … BR-CRF-14 Extraction (`CombineItemExtracao`)

**Overview**
Extraction consumes one item 1774 and either promotes a weapon/armour piece (item level < 5, sanctification >= 9, single-slot `nPos`) into an extraction-tier item while preserving its level and adding static damage, or destroys the item outright.

**Detailed description**
This handler is the only one in the family that uses `MSG_STANDARDPARM2` instead of `MSG_CombineItem`, taking the target slot from `Parm2`. The prerequisites (`_MSG_CombineItemExtracao.cpp:26-43`) are: slot in `[0, MaxCarry)`, item index in `(0, MAX_ITEMLIST)`, `EF_ITEMLEVEL < 5`, `BASE_GetItemSanc >= 9`, `nPos` in {2, 4, 8, 16, 32} (single equipment slots), and `EF_MOBTYPE == 0`. Every failure is a silent `return` — no message, no log — which makes the handler opaque to players who do not meet the (undocumented in-game) prerequisites.

The catalyst is found by a linear scan for `sIndex == 1774` anywhere in `[0, MaxCarry)` (`:47-55`) and cleared with `BASE_ClearItem` + `SendItem`, then `:57-58` aborts if the scan ran off the end. Critically, the stone is consumed **before** the roll and is not restored on failure, so a failed extraction costs both the stone and the item.

Success (`:67-99`) requires `_rnd < rate` (strictly less than, unlike the `<=` used by the other recipes) with `rate = (Special[2] + 1) / 6`. It writes `EF_ITEMLEVEL` with the old level into effect slot 0, adds `BASE_GetStaticItemAbility(EF_DAMAGE)` to any effect slot that already holds `EF_DAMAGE`, sets `sIndex = 3021 + offset` where offset is 0/1/2/3/4 for `nPos` 2/4/8/16/32, and resyncs. Failure (`:101-107`) clears the item entirely. Both outcomes are audited with `ItemLog` using a mixed-encoding literal (`extra��o`) that indicates a source-encoding mismatch in the file (Latin-1 bytes in a UTF-8 tree).

**Rule workflow**
1. `ItemSlot = Parm2`; reject if outside `[0, MaxCarry)`.
2. `item = Carry[ItemSlot].sIndex`; reject if outside `(0, MAX_ITEMLIST)`.
3. Reject if `EF_ITEMLEVEL >= 5`.
4. Reject if `BASE_GetItemSanc(...) < 9`.
5. Reject if `nPos` not in {2,4,8,16,32} or `EF_MOBTYPE != 0`.
6. Scan inventory for item 1774; clear and `SendItem`; reject if not found.
7. `_rnd = rand()%115`; if `> 100` subtract 15. `rate = (Special[2]+1)/6`.
8. If `_rnd < rate`: write level effect, add damage, set `sIndex = 3021 + offset`, `SendItem`, `ItemLog` success.
9. Else: `BASE_ClearItem(Carry[ItemSlot])`, `SendItem`, `ItemLog` failure.

---

### Business Rule: BR-CRF-15 … BR-CRF-17 Lindy quest unlock (deterministic)

**Overview**
Lindy is not a probability craft: it is the Arch-class quest unlock, gated by level (354 or 369), quest flags and fame, consuming an exact seven-item set, and granting either a clan-specific `Equip[15]` item (level 354) or the level-370 unlock plus a fame decrement (level 369).

**Detailed description**
`_MSG_CombineItemLindy.cpp:45-58` is a four-gate filter: `ClassMaster == ARCH`, level is exactly 354 or 369, `QuestInfo.Arch.Level355 == 1` blocks a second level-354 unlock, `Level370 == 0 && level == 369 && Fame <= 0` blocks the level-369 unlock without fame, and `Level370 == 1 && level == 369` blocks a repeat. Every failure is a silent return, so a player with the wrong level gets no feedback at all.

The recipe check at `:77-78` is a single conjunction: `Item[0] == 413 && amount == 10`, `Item[1] == 413 && amount == 10`, `Item[2] == 4127`, and `Item[3..6] == 413`. The amount is read from `m->Item[]` (the client's declaration) rather than from the inventory, but BR-CRF-01 has already proven byte equality with the actual slots, so the amount is trustworthy. The remaining slots (index 7) are not inspected.

Only after the recipe matches are the inputs memset (`:81-88`). The level-354 branch (`:90-106`) sets `QuestInfo.Arch.Level355 = 1`, clears `Equip[15]`, and grants 3191 for clan 7, 3192 for clan 8, 3193 otherwise, then `SendItem` on the equip slot. The level-369 branch (`:108-112`) sets `Level370 = 1` and decrements `Fame`. Both end with `_MSG_CombineComplete = 1`, `SendEmotion(conn, 14, 3)` and `_NN_Processing_Complete`. There is no failure branch that would fire after the recipe matches, so a matching recipe always succeeds — the only risk is the inputs being consumed by the `memset` and the state write failing silently.

**Rule workflow**
1. BR-CRF-01 identity proof.
2. Reject unless `ClassMaster == ARCH`.
3. Reject unless level is 354 or 369; apply the three quest/fame gates.
4. Require the exact seven-item recipe (2x 413 amount 10, 4127, 4x 413).
5. Clear all declared inputs; `SendItem` each.
6. If level 354: `Level355 = 1`; clear `Equip[15]`; grant 3191/3192/3193 by clan; `SendItem(EQUIP, 15)`.
7. If level 369: `Level370 = 1`; `Fame -= 1`.
8. Log `lindy,unlock lv%d`; complete 1; `SendEmotion(14,3)`; `_NN_Processing_Complete`.

---

### Business Rule: BR-CRF-18 … BR-CRF-20 Shany catalyst crafting

**Overview**
Shany combines two sanctified 540/541 items plus one 540/541/633 catalyst and four 413 units into a single item 633, at a rate taken directly from `g_pShanyBase`; the class gate uses an expression whose meaning depends on operator precedence.

**Detailed description**
`_MSG_CombineItemShany.cpp:45` reads `if (pMob[conn].extra.ClassMaster == MORTAL || pMob[conn].extra.ClassMaster == ARCH && pMob[conn].MOB.CurrentScore.Level < 355)`. Because `&&` binds tighter than `||`, this is `MORTAL || (ARCH && level < 355)`, so: Mortals are always rejected; Archs below 355 are rejected; Archs at or above 355 pass; Celestial tiers pass unconditionally. If the intent was "Mortals and Archs below 355", the code is correct; if the intent was "anything below 355", it is not. Recorded as observed behaviour with an explicit precedence note.

`GetMatchCombineShany` (`GetFunc.cpp:217-259`) requires seven non-empty inputs: `Item[0]` and `Item[1]` in {540, 541} with sanctification >= 9, `Item[2]` in {540, 541, 633}, and `Item[3..6]` all equal to 413. The returned rate is the constant `1`, so the effective probability is *not* derived from the recipe — unlike every other recipe except Ancient. `:89` therefore compares against `g_pShanyBase` (a configured value), and the recipe's `combine` return value is discarded.

Consumption (`:60-67`) deletes all declared inputs before the roll. Success (`:90-104`) creates a fresh `STRUCT_ITEM` with `sIndex = 633` and inserts it with `PutItem`; if `PutItem` fails (full inventory) the function continues to send `_NN_Processing_Complete` and `complete = 1`, so the item is silently lost — the only crafting path where a full inventory destroys the result without notice. Failure (`:106-113`) does not consume the 413s? It does: they were deleted at `:60-67`, so failure costs everything.

**Rule workflow**
1. BR-CRF-01 identity proof.
2. Reject if `ClassMaster == MORTAL || (ClassMaster == ARCH && level < 355)` -> `_NN_Wrong_Combination`, complete 0.
3. `combine = GetMatchCombineShany(...)`; reject if 0.
4. Delete all declared inputs; `SendItem` each; log.
5. `_rand` normalised.
6. If `_rand <= g_pShanyBase || LOCALSERVER`: `PutItem(633)`; `_NN_Processing_Complete`; complete 1.
7. Else `_NN_CombineFailed`; complete 2.

---

### Business Rule: BR-CRF-21 … BR-CRF-23 Tiny merge

**Overview**
Tiny merges an Arch weapon/armour base with a donor of the same slot, both sanctified, plus a third matching item, for 100,000,000 coin charged only on success; failure destroys the base and keeps the donor.

**Detailed description**
`_MSG_CombineItemTiny.cpp:45-50` gates on `MOB.Coin >= 100,000,000` but reports `_NN_Wrong_Combination` rather than a cost message, so a player who is merely poor is told the combination is wrong. `GetMatchCombineTiny` (`GetFunc.cpp:167-215`) requires `Item[0..2]` non-empty, base `EF_MOBTYPE == ARCH`, base `Grade` in 5..8, `Item[1]` and `Item[2]` with `EF_ITEMTYPE == 4 || 5` (the `||` inside each test makes the check always true, a defect — see RISK-07), `Item[2]` grade 5..8, `nPos` equality between `Item[0]` and `Item[1]`, base `nPos` in {64, 192}, and sanctification >= 9 on all three. The rate is `g_pTinyBase + EF_ITEMLEVEL(Item[1]) * 5`.

Consumption (`:61-68`) deletes inputs 2..7 (keeping 0 and 1 for the roll), which is the mirror of Agatha's policy. On success (`:93-126`) effects 0..2 are copied from `m->Item[1]` into `Carry[InvenPos[0]]`, the donor slot is cleared, `BASE_SetItemSanc(...,7,0)` is applied, then — and only here — `Coin -= 100,000,000` and `SendEtc`. The success message is built with a format string that the table entry apparently does not expect (`sprintf(tt, g_pMessageStringTable[_NN_Processing_Complete], MobName, itemName)` where `_NN_Processing_Complete` is used elsewhere with no arguments), which is a format-string risk (RISK-08).

On failure (`:128-139`) the **base** item slot is cleared and the donor is kept, and no coin is deducted. So Tiny's failure is cheap in money but expensive in equipment, while Ailyn's failure is expensive in money but free in equipment — the two recipes are economical opposites despite sharing the same structural shape.

**Rule workflow**
1. BR-CRF-01 identity proof.
2. Reject if `Coin < 100,000,000` -> `_NN_Wrong_Combination`, complete 0.
3. `combine = GetMatchCombineTiny(...)`; reject if 0.
4. Delete inputs 2..7; `SendItem` each; log.
5. `_rand` normalised.
6. Success: copy donor effects to base; clear donor; `BASE_SetItemSanc(7,0)`; `Coin -= 100,000,000`; `SendEtc`; format message; complete 1; `SendItem`.
7. Failure: clear base slot; `SendItem`; `_NN_CombineFailed`; complete 2; `ItemLog` failure.

---

## 3.11 Detailed breakdown — PvP and miscellaneous domain

### Business Rule: BR-PVP-01 … BR-PVP-03 PK mode toggle

**Overview**
`PKMode` normalises any non-zero request to 1, aborts a mutual trade with notifications, and multicasts a PK-state flag computed from guilt, the new mode and three event rectangles; on a newbie-event server the flag is forced to 1.

**Detailed description**
`_MSG_PKMode.cpp:24` writes `pUser[conn].PKMode = m->Parm != 0 ? 1 : 0` directly to persistent session state with no mode or HP guard — the only "state-changing" handler besides `ChangeCity` and `SetShortSkill` without any guard. The trade interlock (`:26-36`) is mutual-aware: it only fires when the opponent is `USER_PLAY` **and** its `Trade.OpponentID` points back at `conn`, then messages both players with `_NN_Cant_trade_pkmode` and calls `RemoveTrade` on both. This is the correct two-sided tear-down that BR-TRD-04's one-sided variants lack.

The broadcast computation (`:45-57`) is a long single expression: `state = 1` when `guilty` (from `GetGuilty`, which reads `MOB.Carry[KILL_MARK].stEffect[1].cEffect` and self-heals values above 50, `GetFunc.cpp:2114-2127`), or `PKMode`, or the player is inside the RvR rectangle `[1023..1280]x[1919..2179]`, or the castle rectangle `[1024..1153]x[1664..1793]`, or the GTorre rectangle `[2430..2560]x[1825..1925]`, each gated by its respective global state flag. When `NewbieEventServer != 0` the whole computation is replaced by `state = 1`.

The expression relies on `&&`/`||` precedence without parentheses across seven terms, which is exactly the class of code where an added term silently changes meaning. Note also that this per-zone logic differs from `SendPKInfo` (`SendFunc.cpp:1752-1762`), which sets state 1 if *any* event is active anywhere — so two different PK-visibility semantics coexist: local-rectangle (this handler) and global-flag (`SendPKInfo`, used by `NoViewMob`).

**Rule workflow**
1. `PKMode = (Parm != 0)`.
2. If `Trade.OpponentID` and opponent is `USER_PLAY` and reciprocal: message both, `RemoveTrade` both.
3. Build `MSG_STANDARDPARM` `_MSG_PKInfo`, `ID = conn`.
4. If `NewbieEventServer == 0`: `state = guilty || PKMode || RvR-rect || Castle-rect || GTorre-rect`; else `state = 1`.
5. `GridMulticast`; log `etc,pkmode %d`.

---

### Business Rule: BR-MSC-01 Motion broadcast

**Overview**
Emotes and motion animations require a live in-game player, are always audited, and are then broadcast to the surrounding grid.

**Detailed description**
`_MSG_Motion.cpp:24-31` applies the alive/in-game guard with `AddCrackError(conn, 4, 6)` — note `4` rather than the `10` used by the inventory handlers, so motion abuse escalates the crack counter at a different rate. `:33-34` writes an `etc,motion motion:%d parm:%d notused:%d` line for every motion including the ignored `NotUsed` field, which means the log is high-volume and mostly noise on a busy server.

`:36` then `GridMulticast(pMob[conn].TargetX, TargetY, (MSG_STANDARD*)m, 0)` forwards the client's packet **unmodified**, including refCount/lifetime bytes: the handler performs no validation of `Motion` or `Parm`. Any 16-bit value is therefore legal and will be rendered by every client within `VIEWGRID`. Because the packet is forwarded verbatim, the client's `ID` field becomes the broadcast `ID` — it is not rewritten to `conn`, so a client could in principle advertise a different animating entity. `NoViewMob` (BR-MSC-02) is the reciprocal, server-authoritative path; `Motion` is not.

**Rule workflow**
1. Reject if `Hp == 0 || Mode != USER_PLAY` -> `SendHpMode`, `AddCrackError(4,6)`.
2. Log `etc,motion motion:%d parm:%d notused:%d`.
3. `GridMulticast(TargetX, TargetY, (MSG_STANDARD*)m, 0)` with the unmodified packet.

---

### Business Rule: BR-MSC-02 Visibility reconciliation (`NoViewMob`)

**Overview**
When a client reports that it can no longer see an entity, the server decides authoritatively: empty slot -> removal notice; live entity in view -> re-create it plus PK info; live entity out of view -> removal notice.

**Detailed description**
`_MSG_NoViewMob.cpp:26-30` validates `MobID` in `[1, MAX_MOB)` and logs `err,reqmobbyid MobId Out of range` otherwise. `:32-36` handles the `MOB_EMPTY` case with `SendRemoveMob(conn, MobID, 0, 0)`, which is the client's hint accepted. The branch at `:38` (`MobID >= MAX_USER || pUser[MobID].Mode == USER_PLAY`) is effectively "is this a player or any mob", i.e. everything except a user slot not in play; within it, `GetInView(conn, MobID)` decides between `SendCreateMob(conn, MobID, 1)` + `SendPKInfo(conn, MobID)` (re-create and refresh PK flag) and a removal notice. The final `else` (`:48-49`) sends a removal notice for user slots that are not in `USER_PLAY`.

The handler is therefore a self-healing mechanism for client-side entity desync: a client that loses an entity asks, and the server either re-sends it or confirms it is gone. Because the re-create path includes `SendPKInfo`, a desynced client also gets its PK colouring corrected. The `bSend = 1` argument to `SendCreateMob` forces an immediate send rather than queueing.

The one hole is that the `MobID >= MAX_USER` condition means the handler will happily re-create a *mob* for a client that never should see it (no visibility-filtering beyond `GetInView`), which is fine for grid-based visibility but means `NoViewMob` cannot be used to hide entities.

**Rule workflow**
1. Reject if `MobID` outside `[1, MAX_MOB)` (log).
2. If `pMob[MobID].Mode == MOB_EMPTY` -> `SendRemoveMob`, return.
3. If `MobID >= MAX_USER || pUser[MobID].Mode == USER_PLAY`:
   - If `GetInView(conn, MobID)` -> `SendCreateMob(conn, MobID, 1)`, `SendPKInfo(conn, MobID)`.
   - Else -> `SendRemoveMob`.
4. Else -> `SendRemoveMob`.

---

### Business Rule: BR-MSC-03 Shortcut persistence (`SetShortSkill`)

**Overview**
The client may write its own 4-byte skill bar and 16-byte shortcut array into persistent character state with no validation whatsoever.

**Detailed description**
`_MSG_SetShortSkill.cpp:24-25` is two `memcpy` calls: 4 bytes from `m->Skill1` into `pMob[conn].MOB.SkillBar` and 16 bytes from `m->Skill2` into `pUser[conn].CharShortSkill`. There is no mode check, no HP check, no value validation, no logging, and it does not even send a confirmation.

The two destinations differ in kind: `MOB.SkillBar` is part of the character record that is persisted through `SaveUser`, while `CharShortSkill` is a `CUser` field that is not part of the mob save (it is per-session). The rule therefore has split persistence semantics — one array survives logout, the other does not — which is a behavioural detail invisible from the handler.

Because arbitrary bytes are accepted, a client can store skill ids it does not own; the enforcement point must be the client (which is not a control) or the use-site (out of scope). This is the least-defended handler in the family and is recorded as RISK-02.

**Rule workflow**
1. `memcpy(pMob[conn].MOB.SkillBar, m->Skill1, 4)`.
2. `memcpy(pUser[conn].CharShortSkill, m->Skill2, 16)`.
3. No response, no log, no validation.

---

## 4. Component Structure

```
Code/TMSrv/
├── ProcessClientMessage.cpp        # Dispatcher: global guards + switch(Type) -> Exec_MSG_*  (38-313)
├── ProcessClientMessage.h          # Declares all 61 Exec_MSG_* entry points, aggregates every TMSrv header
│
├── _MSG_AcceptParty.cpp            # 129 lines - party join, invite correlation (BR-PTY-01..09)
├── _MSG_SendReqParty.cpp           #  93 lines - party invite issuance (BR-PTY-10..12)
├── _MSG_RemoveParty.cpp            #  41 lines - thin wrapper over RemoveParty() (BR-PTY-13..14)
├── _MSG_CharacterLogout.cpp        #  25 lines - RemoveParty + CharLogOut + socket flush
│
├── _MSG_GuildAlly.cpp              #  38 lines - alliance forwarder (BR-GLD-01..02)
├── _MSG_War.cpp                    #  38 lines - war forwarder (BR-GLD-03..04)
├── _MSG_InviteGuild.cpp            #  74 lines - paid recruitment (BR-GLD-05..10)
├── _MSG_Deprivate.cpp              #  23 lines - wrapper over DoDeprivate() (BR-GLD-11..13)
├── _MSG_Challange.cpp              # 110 lines - siege dialogue + town tax (BR-CHL-01..04)
├── _MSG_ChallangeConfirm.cpp       #  29 lines - wrapper over Challange() (BR-CHL-05..06)
│
├── _MSG_REQShopList.cpp            #  46 lines - NPC shop catalog (BR-SHP-01..02)
├── _MSG_SendAutoTrade.cpp          #  99 lines - player shop registration (BR-SHP-03..08)
├── _MSG_ReqTradeList.cpp           #  38 lines - player shop visibility (BR-SHP-09)
├── _MSG_ReqBuy.cpp                 # 164 lines - player shop purchase + settlement (BR-SHP-10..14)
├── _MSG_ReqRanking.cpp             #  67 lines - duel handshake (BR-RNK-01..04)
│
├── _MSG_ReqTeleport.cpp            #  57 lines - gate travel with fees (BR-TEL-01..05)
├── _MSG_ChangeCity.cpp             #  32 lines - city binding into Merchant bits 6-7 (BR-TEL-06)
├── _MSG_Restart.cpp                #  42 lines - respawn / faction snap (BR-TEL-07)
├── _MSG_Motion.cpp                 #  31 lines - animation broadcast (BR-MSC-01)
├── _MSG_NoViewMob.cpp              #  45 lines - visibility reconciliation (BR-MSC-02)
├── _MSG_SetShortSkill.cpp          #  24 lines - skill bar persistence (BR-MSC-03)
├── _MSG_PKMode.cpp                 #  51 lines - PK toggle + state broadcast (BR-PVP-01..03)
├── _MSG_QuitTrade.cpp              #  46 lines - trade abort + PK broadcast (BR-TRD-01..04)
│
├── _MSG_CreateCharacter.cpp        #  43 lines - name policy + DB handoff (BR-CHR-01..03)
├── _MSG_DeleteCharacter.cpp        #  38 lines - DB handoff (BR-CHR-04..05)
├── _MSG_AccountSecure.cpp          #  27 lines - token change forwarder (BR-CHR-07)
├── _MSG_CapsuleInfo.cpp            #  27 lines - capsule list forwarder (BR-CHR-08)
│
├── _MSG_DeleteItem.cpp             #  42 lines - item destruction (BR-INV-01..03)
├── _MSG_SplitItem.cpp              #  76 lines - stack splitting (BR-INV-04..07)
├── _MSG_Deposit.cpp                #  47 lines - wallet -> warehouse (BR-INV-08..09)
├── _MSG_Withdraw.cpp               #  47 lines - warehouse -> wallet (BR-INV-10..11)
├── _MSG_PutoutSeal.cpp             #  91 lines - seal capsule creation (BR-INV-12..15)
│
├── _MSG_CombineItem.cpp            # 124 lines - ancient combination (BR-CRF-03..04)
├── _MSG_CombineItemAgatha.cpp      # 104 lines - effect transfer (BR-CRF-05..06)
├── _MSG_CombineItemAilyn.cpp       # 124 lines - attribute merge, 100M (BR-CRF-07..09)
├── _MSG_CombineItemAlquimia.cpp    #  93 lines - alchemy skills (BR-CRF-10..11)
├── _MSG_CombineItemExtracao.cpp    #  81 lines - extraction (BR-CRF-12..14)
├── _MSG_CombineItemLindy.cpp       #  98 lines - Arch quest unlock (BR-CRF-15..17)
├── _MSG_CombineItemShany.cpp       #  93 lines - catalyst craft (BR-CRF-18..20)
└── _MSG_CombineItemTiny.cpp        # 112 lines - merge, 100M on success (BR-CRF-21..23)

Supporting code outside the component boundary (referenced, not owned):
Code/TMSrv/Server.cpp              # RemoveParty, DoDeprivate, Challange, DoRanking, DoRecall,
                                   # DoTeleport, CharLogOut, RemoveTrade, PutItem, AddCrackError, CrackLog
Code/TMSrv/GetFunc.cpp             # GetMatchCombine{,Tiny,Shany,Ailyn,Agatha,Alquimia}, GetTeleportPosition,
                                   # GetInView, GetGuilty, GetCreateMob, GetCreateMobTrade
Code/TMSrv/SendFunc.cpp            # SendClientMessage, SendItem, SendEtc, SendCargoCoin, SendShopList,
                                   # SendAutoTrade, SendAddParty, SendReqParty, SendRemoveParty, SendPKInfo,
                                   # SendCreateMob, SendRemoveMob, SendHpMode, GridMulticast, SendSay
Code/Basedef.cpp                   # BASE_GetVillage, BASE_CheckValidString, BASE_Get/SetItemSanc,
                                   # BASE_Get/SetItemAmount, BASE_ClearItem, GetItemPointer, BASE_GetItemCode
Code/Basedef.h                     # All packet structs, opcode constants, MAX_* limits
Code/TMSrv/Language.h              # _NN_* / _DN_* / _I64D_* message table indices
```

### 4.1 Structural facts

| Property | Value | Evidence |
|---|---|---|
| Files in family | 40 | directory listing of `Code/TMSrv/_MSG_*.cpp` |
| Functions per file | exactly 1 | e.g. `_MSG_AccountSecure.cpp:21` is the only definition |
| Common header | `#include "ProcessClientMessage.h"` | line 19 of every file |
| Shared aggregate include depth | 14 internal components | `ProcessClientMessage.h` includes Basedef, CPSock, ItemEffect, Language, CItem, Server, GetFunc, SendFunc, CMob, CUser, CNPCGene, CReadFiles, CCastleZakum, CWarTower |
| Total lines in family | 2,486 | sum of `Measure-Object -Line` over the 40 files |
| Largest handler | `_MSG_ReqBuy.cpp` (164 lines) | — |
| Smallest handler | `_MSG_Deprivate.cpp` (23 lines) | — |
| Own class / state | none | all state via `pUser[]`/`pMob[]` globals |
| Own header | none | declared centrally in `ProcessClientMessage.h` |
| Packet-size validation | none in any handler | every handler casts `char*` to its struct without checking `Size` |

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains (per domain)

```
Party
  AcceptParty   -> SendAddParty, SendClientMessage, CrackLog, Log            (+ pMob.LastReqParty)
  SendReqParty  -> SendReqParty(SendFunc), SendClientMessage, Log
  RemoveParty   -> RemoveParty(Server) -> SendRemoveParty, DeleteMob, Pista[], ProcessClientMessage(AcceptParty)

Guild
  GuildAlly     -> DBServerSocket
  War           -> DBServerSocket
  InviteGuild   -> BASE_GetGuildName, GetCreateMob, GridMulticast, SendEtc, SendClientMessage, Log
  Deprivate     -> DoDeprivate(Server) -> MSG_GuildInfo->DBSrv, GetCreateMob, GridMulticast, SendScore
  Challange     -> BASE_GetGuildName, PutItem, SendSay, SendEtc, SendClientSignal, Log
  ChallangeConfirm -> Challange(Server)

Shop
  REQShopList   -> GetInView, SendShopList(SendFunc) -> g_pGuildZone[].CityTax
  SendAutoTrade -> BASE_GetVillage, BASE_GetItemAbility, GetCreateMobTrade, SendAutoTrade, GridMulticast, RemoveTrade
  ReqTradeList  -> SendAutoTrade(SendFunc)
  ReqBuy        -> BASE_GetVillage, BASE_GetItemCode, SendItem, SendEtc, SendCargoCoin, SaveUser,
                   GridMulticast, RemoveTrade, AddCrackError, ItemLog

Ranking
  ReqRanking    -> DoRanking(Server) -> DoTeleport, BASE_GetGuildName, SendClientSignalParm, SendClientMessage

Travel / world
  ReqTeleport   -> GetTeleportPosition, DoTeleport, SendEtc, SendClientMessage, Log
  ChangeCity    -> BASE_GetVillage, Log
  Restart       -> DoTeleport, DoRecall, SendScore, SendSetHpMp, SendEtc, SendHpMode, Log
  Motion        -> GridMulticast, SendHpMode, AddCrackError, Log
  NoViewMob     -> GetInView, SendCreateMob, SendPKInfo, SendRemoveMob, Log
  SetShortSkill -> (no calls; writes globals)

Trade / PvP
  QuitTrade     -> RemoveTrade, GridMulticast, GetGuilty, SendHpMode, AddCrackError
  PKMode        -> RemoveTrade, GetGuilty, GridMulticast, SendClientMessage, Log

Session
  CreateCharacter -> BASE_CheckValidString, SendClientSignal, DBServerSocket, Log
  DeleteCharacter -> SendClientMessage, DBServerSocket, Log
  CharacterLogout -> RemoveParty, CharLogOut, cSock.SendMessageA
  AccountSecure   -> DBServerSocket
  CapsuleInfo     -> DBServerSocket

Inventory
  DeleteItem    -> BASE_GetItemCode, ItemLog, RemoveTrade, SendHpMode
  SplitItem     -> BASE_Get/SetItemAmount, PutItem, SendItem, RemoveTrade, ItemLog, SendHpMode
  Deposit       -> SendCargoCoin, SendHpMode, AddCrackError, SendClientMessage, Log
  Withdraw      -> SendCargoCoin, SendHpMode, AddCrackError, SendClientMessage, Log
  PutoutSeal    -> GetItemPointer, BASE_CheckValidString, CharLogOut, SendItem, RemoveTrade,
                   AddCrackError, CrackLog, SendHpMode, DBServerSocket

Crafting
  CombineItem*  -> GetMatchCombine*, SendItem, SendClientSignalParm, SendEtc, RemoveTrade,
                   BASE_GetItemCode, BASE_SetItemSanc, BASE_GetItemAmount, PutItem, ItemLog, Log
```

### 5.2 External dependencies

| Dependency | Type | Used by | Notes |
|---|---|---|---|
| DBSrv (`DBServerSocket`) | Internal process (TCP) | AccountSecure, CapsuleInfo, CreateCharacter, DeleteCharacter, GuildAlly, War, PutoutSeal, Challange (via `MSG_GuildInfo`) | Fire-and-forget; no ack tracking in TMSrv handlers |
| Client sockets (`CPSock::AddMessage` / `SendOneMessage` / `SendMessageA`) | Network | all sending handlers | `AddMessage` returns false -> `CloseUser` in the `Send*` helpers |
| Win32 CRT | System | all | `rand()`, `memset`, `memcpy`, `memcmp`, `strcmp`, `sprintf`, `strcat`, `strncpy`, `time`, `localtime` |
| `gameconfig.txt` | Config file | ReqTeleport (indirectly), party (PARTY_DIF), crafting bases via CReadFiles | Line 13 holds `PARTY_DIF` (`Server.cpp:1105`) |
| `g_pItemList[MAX_ITEMLIST]` | Loaded item database | SplitItem, all crafting, ReqBuy, NoViewMob, Extracao | Indexed without local re-validation in several places after a range check |
| `g_pGuildZone[5]` | Loaded zone table | ChangeCity, ReqTeleport, ReqBuy, SendAutoTrade, Challange, Restart | `CityTax`, `CityLimit*`, `CitySpawn*`, `ChargeGuild`, `Clan` |
| `GuildInfo[65536]` | In-memory guild DB mirror | Challange, Deprivate | `Fame`, `Sub1..3` |
| `GuildImpostoID[5]` | Tax-treasury NPC index | Challange, ReqTeleport, ReqBuy | Treasury balances stored in `MOB.Exp` |
| `pChallangerMoney[6]` | Siege bid state | Challange | Persisted via `CReadFiles::WriteChallanger()` |
| Logging (`Log`, `ItemLog`, `CrackLog`) | File I/O | 31 of 40 handlers | 9 handlers emit no audit trail at all |

### 5.3 Handlers with no persistent audit trail

`AccountSecure`, `CapsuleInfo`, `GuildAlly`, `War`, `Deprivate`, `CharacterLogout`, `RemoveParty`, `ChallangeConfirm`, `SetShortSkill`, `NoViewMob` (only an error log), `Motion` (fan-out only). For these, the only evidence of a rejected or accepted operation is the absence/presence of downstream effects.

---

## 6. Afferent and Efferent Coupling

Method: **afferent (Ca)** counts distinct translation units that call the `Exec_MSG_*` symbol; **efferent (Ce)** counts distinct internal API symbols the handler references (measured by identifier scan over the 40 files). The structural Ce reported by the architecture analysis is uniformly 14 because `ProcessClientMessage.h` aggregates 14 internal headers — that figure is a header artefact, so the symbol-level Ce below is the meaningful one.

| Component (handler) | Afferent (Ca) | Efferent (Ce, symbols) | Structural Ce (headers) | Critical | Rationale for criticality |
|---|---|---|---|---|---|
| ReqBuy | 1 | 13 | 14 | **High** | Moves items and coin between two players; writes `SaveUser`; routes taxes |
| PutoutSeal | 1 | 10 | 14 | **High** | Consumes an item, forces logout, creates a persisted entity |
| SendAutoTrade | 1 | 9 | 14 | **High** | Creates persistent listing state used by ReqBuy settlement |
| CombineItemAilyn | 1 | 9 | 14 | **High** | 100M currency sink with double-charge defect |
| CombineItemTiny | 1 | 9 | 14 | **High** | Destroys base equipment on failure, 100M gate |
| CombineItem | 1 | 8 | 14 | **High** | Destroys all inputs; result sfx/sanc mutation |
| CombineItemAgatha | 1 | 8 | 14 | **High** | Effect transfer can create anct 7 on arbitrary base |
| CombineItemLindy | 1 | 8 | 14 | **High** | Grants quest unlock + `Equip[15]` reward |
| CombineItemAlquimia | 1 | 8 | 14 | Medium | Skill item creation, class-gated |
| CombineItemShany | 1 | 8 | 14 | Medium | Item 633 creation |
| CombineItemExtracao | 1 | 7 | 14 | Medium | Item destruction + index rewrite |
| Challange | 1 | 7 | 14 | **High** | Town-tax payout from an NPC-based treasury |
| Restart | 1 | 7 | 14 | Medium | Resets the crack counter; teleports |
| SplitItem | 1 | 8 | 14 | Medium | Inventory duplication risk if arithmetic wrong |
| InviteGuild | 1 | 6 | 14 | Medium | Writes membership + charges 4M/100M |
| REQShopList | 1 | 6 | 14 | Low | Read-only catalogue |
| Deposit | 1 | 5 | 14 | Medium | Currency movement |
| Withdraw | 1 | 5 | 14 | Medium | Currency movement |
| PKMode | 1 | 5 | 14 | Medium | Persistent PvP flag + trade abort |
| QuitTrade | 1 | 5 | 14 | Medium | Trade teardown + broadcast |
| ReqTeleport | 1 | 5 | 14 | Medium | Currency sink + position change |
| NoViewMob | 1 | 5 | 14 | Low | Entity resync |
| DeleteItem | 1 | 4 | 14 | Medium | Unrecoverable item destruction |
| AcceptParty | **2** (dispatcher + `RemoveParty`, `Server.cpp:7464`) | 4 | 14 | Medium | Only handler invoked by the server itself |
| SendReqParty | 1 | 3 | 14 | Low | Invitation issuance |
| Motion | 1 | 4 | 14 | Low | Broadcast of unvalidated client packet |
| ReqRanking | 1 | 4 | 14 | Low | Handshake, state in globals |
| ReqTradeList | 1 | 4 | 14 | Low | Read-only listing fetch |
| CreateCharacter | 1 | 4 | 14 | Medium | Session transition + name policy |
| DeleteCharacter | 1 | 3 | 14 | Medium | Irreversible DB operation |
| CharacterLogout | 1 | 2 | 14 | Medium | Save + party teardown |
| ChallangeConfirm | 1 | 1 | 14 | Low | Wrapper |
| Deprivate | 1 | 1 | 14 | Medium | Rank discipline + sub-slot bookkeeping |
| ChangeCity | 1 | 2 | 14 | Low | Bit-field rewrite with no guard |
| AccountSecure | 1 | 1 | 14 | Medium | Credential change forwarder |
| CapsuleInfo | 1 | 1 | 14 | Low | Read-only forwarder |
| GuildAlly | 1 | 1 | 14 | Low | Forwarder |
| War | 1 | 1 | 14 | Low | Forwarder |
| RemoveParty | 1 | 1 | 14 | Medium | Party mutation via Server.cpp |
| SetShortSkill | 1 | **0** | 14 | Low | Writes globals only; no validation |

### 6.1 Cohesion assessment

- **Functional cohesion within each handler: high.** Each function does exactly one operation class (`AcceptParty` joins, `Deposit` moves coin, `SplitItem` splits). No handler multiplexes unrelated operations — with the two documented exceptions: `Challange` (siege dialogue *and* town-tax withdrawal) and `Restart` (respawn *and* crack-counter reset).
- **Cohesion of the family as a unit: low by design.** The 40 files are grouped only by "not covered by a dedicated report". There is no shared base, no shared validation helper, and the same conceptual guard is written three different ways. The family is a *set*, not a *module*.
- **Reuse of validation logic: near zero.** `Hp == 0 || Mode != USER_PLAY` appears in 9 handlers with 2 different spellings (`== 0` vs `<= 0`) and 9 different crack-point pairs (`10/1`, `10/2`, `4/6`, `10/22`, `10/86`, `10/87`, `10/88`, `10/17`, `1/15`). The `_MSG_CombineComplete` handshake block appears 8 times verbatim.
- **Coupling direction:** strictly downward (handler → `Server.cpp`/`GetFunc.cpp`/`SendFunc.cpp` → `Basedef.cpp` → globals). The one upward edge in the whole family is `RemoveParty` → `ProcessClientMessage` → `AcceptParty`.

---

## 7. Endpoints (Protocol Surface)

No HTTP/REST, GraphQL, gRPC or named-service endpoints exist in this component; the inbound contract is an **opcode-based binary protocol** carried over the client TCP socket and dispatched by `ProcessClientMessage.cpp`. The table below is the endpoint inventory in protocol-appropriate form. `Type` values include the direction flag bits (`FLAG_CLIENT2GAME = 0x0200`, `Basedef.h:933`).

| Message | Opcode (base \| flags) | Payload struct | Frame size | Handler | Purpose |
|---|---|---|---|---|---|
| `_MSG_AccountSecure` | 222 \| 0x0200 (+0x0800) | `MSG_AccountSecure` | 28 | Exec_MSG_AccountSecure | Submit numeric token change to DBSrv |
| `_MSG_CreateCharacter` | 15 \| 0x0200 | `MSG_CreateCharacter` | 28 | Exec_MSG_CreateCharacter | Create character |
| `_MSG_DeleteCharacter` | 17 \| 0x0200 | `MSG_DeleteCharacter` | 36 | Exec_MSG_DeleteCharacter | Delete character |
| `_MSG_CharacterLogout` | 21 \| 0x0200 | `MSG_STANDARD` | 12 | Exec_MSG_CharacterLogout | Log character out |
| `_MSG_CapsuleInfo` | 205 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_CapsuleInfo | Request capsule contents |
| `_MSG_AcceptParty` | 171 \| 0x0200 | `MSG_AcceptParty` | 26 | Exec_MSG_AcceptParty | Accept party invitation |
| `_MSG_SendReqParty` | 127 \| 0x0200 | `MSG_SendReqParty` | 40 | Exec_MSG_SendReqParty | Issue party invitation |
| `_MSG_RemoveParty` | 126 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_RemoveParty | Leave/kick/dissolve party |
| `_MSG_GuildAlly` | 18 \| 0x0200 | `MSG_GuildAlly` | 20 | Exec_MSG_GuildAlly | Request guild alliance |
| `_MSG_War` | 14 \| 0x0200 | `MSG_STANDARDPARM2` | 20 | Exec_MSG_War | Declare guild war |
| `_MSG_InviteGuild` | 213 \| 0x0200 | `MSG_STANDARDPARM2` | 20 | Exec_MSG_InviteGuild | Recruit into guild |
| `_MSG_Deprivate` | 140 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_Deprivate | Remove member from guild |
| `_MSG_Challange` | 142 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_Challange | Challenge NPC dialogue / tax collection |
| `_MSG_ChallangeConfirm` | 143 \| 0x0200 | `MSG_STANDARDPARM2` | 20 | Exec_MSG_ChallangeConfirm | Confirm siege challenge |
| `_MSG_REQShopList` | 123 \| 0x0200 | `MSG_REQShopList` | 16 | Exec_MSG_REQShopList | Open NPC shop |
| `_MSG_SendAutoTrade` | 151 \| 0x0200 | `MSG_SendAutoTrade` | 236 | Exec_MSG_SendAutoTrade | Open player shop |
| `_MSG_ReqTradeList` | 154 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_ReqTradeList | View player shop |
| `_MSG_ReqBuy` | 152 \| 0x0200 | `MSG_ReqBuy` | 40 | Exec_MSG_ReqBuy | Buy from player shop |
| `_MSG_ReqRanking` | 159 \| 0x0200 | `MSG_STANDARDPARM2` | 20 | Exec_MSG_ReqRanking | Request / accept duel |
| `_MSG_ReqTeleport` | 144 \| 0x0200 | `MSG_STANDARD` | 12 | Exec_MSG_ReqTeleport | Use a gate |
| `_MSG_ChangeCity` | 145 \| 0x0200 | `MSG_STANDARD` | 12 | Exec_MSG_ChangeCity | Rebind home city |
| `_MSG_Restart` | 137 \| 0x0200 | `MSG_STANDARD` | 12 | Exec_MSG_Restart | Respawn after death |
| `_MSG_Motion` | 106 \| 0x0200 | `MSG_Motion` | 20 | Exec_MSG_Motion | Broadcast animation |
| `_MSG_NoViewMob` | 105 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_NoViewMob | Report lost entity |
| `_MSG_PKMode` | 153 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_PKMode | Toggle kill mode |
| `_MSG_QuitTrade` | 132 \| 0x0200 | `MSG_STANDARD` | 12 | Exec_MSG_QuitTrade | Abort trade |
| `_MSG_SetShortSkill` | 120 \| 0x0200 | `MSG_SetShortSkill` | 32 | Exec_MSG_SetShortSkill | Persist skill bar |
| `_MSG_DeleteItem` | 228 \| 0x0200 | `MSG_DeleteItem` | 20 | Exec_MSG_DeleteItem | Destroy inventory item |
| `_MSG_SplitItem` | 229 \| 0x0200 | `MSG_SplitItem` | 24 | Exec_MSG_SplitItem | Split a stack |
| `_MSG_Deposit` | 136 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_Deposit | Move coin to warehouse |
| `_MSG_Withdraw` | 135 \| 0x0200 | `MSG_STANDARDPARM` | 16 | Exec_MSG_Withdraw | Move coin from warehouse |
| `_MSG_PutoutSeal` | 204 \| 0x0200 | `MSG_PutoutSeal` | 48 | Exec_MSG_PutoutSeal | Create seal capsule |
| `_MSG_CombineItem` | 166 \| 0x0200 | `MSG_CombineItem` | 168 | Exec_MSG_CombineItem | Ancient combination |
| `_MSG_CombineItemTiny` | 192 \| 0x0200 | `MSG_CombineItem` | 168 | Exec_MSG_CombineItemTiny | Tiny merge |
| `_MSG_CombineItemAilyn` | 181 \| 0x0200 | `MSG_CombineItem` | 168 | Exec_MSG_CombineItemAilyn | Ailyn merge |
| `_MSG_CombineItemAgatha` | 186 \| 0x0200 | `MSG_CombineItem` | 168 | Exec_MSG_CombineItemAgatha | Agatha effect transfer |
| `_MSG_CombineItemAlquimia` | 225 \| 0x0200 | `MSG_CombineItem` | 168 | Exec_MSG_CombineItemAlquimia | Alchemy |
| `_MSG_CombineItemShany` | 196 \| 0x0200 | `MSG_CombineItem` | 168 | Exec_MSG_CombineItemShany | Shany catalyst |
| `_MSG_CombineItemLindy` | 195 \| 0x0200 | `MSG_CombineItem` | 168 | Exec_MSG_CombineItemLindy | Arch quest unlock |
| `_MSG_CombineItemExtracao` | 212 \| 0x0200 | `MSG_STANDARDPARM2` | 20 | Exec_MSG_CombineItemExtracao | Extraction |

Corresponding server-to-client acks emitted by this family (not endpoints, but part of the same contract): `_MSG_CombineComplete` (167), `_MSG_PKInfo` (102), `_MSG_SN_ItemSold` / `_MSG_ItemSold` (155), `_MSG_ShopList`, `_MSG_CNFAddParty`, `_MSG_RemoveParty` (126), `_MSG_CreateMobTrade`, `_MSG_Item`, `_MSG_Etc`, `_MSG_CargoCoin`, `_MSG_SetHpMode` (146), `_MSG_NewCharacterFail` (26), `_MSG_CNFCharacterLogout` (22), `_MSG_ReqChallange` (141), `_MSG_StartTime` (161).

**Frame-size note:** no handler validates `std->Size` against its struct before casting. `Size` is present in every packet (`Basedef.h:925`) and is only used by the DB and `BASE_CheckPacket` under `_PACKET_DEBUG`. See RISK-01.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| Client TCP socket (`CPSock`) | Inbound/outbound | All 40 opcodes in, structured acks out | Binary frames over TCP | Packed structs (`_MSG` macro + payload) | Silent drop; `AddMessage` failure -> `CloseUser` in `Send*` helpers |
| DBSrv (`DBServerSocket`) | Internal service (outbound) | Character create/delete, account secure, capsule info, guild ally, war, capsule out, guild info (fame) | Binary frames over TCP | Same structs with `FLAG_GAME2DB` | Fire-and-forget; no timeout, retry or ack tracking in the handler |
| `gameconfig.txt` | Configuration file | `PARTY_DIF` (party window), crafting bases, event hours | Text (line 13 = PARTY_DIF) | Integers separated by spaces | `MessageBox` + abort load on invalid values (`Server.cpp:1096-1103`) |
| `g_pItemList` | In-memory catalogue | Item identity, `Extra`, `nPos`, `Grade`, `nUnique`, abilities | Struct array | `STRUCT_ITEMLIST` | Range-checked by index before use (mostly) |
| `g_pGuildZone[5]` | In-memory zone table | Village detection, city tax, spawn points, owner clan | Struct array | `STRUCT_GUILDZONE` | `BASE_GetVillage` returns 5 for "outside"; callers test `< 5` |
| `GuildInfo[65536]` | In-memory guild mirror | Fame for siege, Sub1..3 for ranks | Struct array | `STRUCT_GUILDINFO` | Bounds checked on entry (`< 65536`) |
| `GuildImpostoID[5]` -> `pMob[].MOB.Exp` | In-memory treasury | Town-tax accumulation (siege, teleport, player shop) | Direct field mutation | `long long` in an `int`-era struct | Caps applied (2e9 / 2e11 / 2e12 in different places) — inconsistent ceilings |
| `pChallangerMoney[6]` | In-memory + file | Siege bid state | `int[6]`, persisted | — | Persisted via `CReadFiles::WriteChallanger()` / `WriteGuild()` |
| Log files (`Log`, `ItemLog`, `CrackLog`) | File I/O | Audit | Text lines | Free-form | Fire-and-forget |
| Win32 `rand()` | Runtime | All probability rolls | LCG | — | Not seeded per-call; bypassed when `LOCALSERVER == 1` |
| `localtime` | Runtime | Recruitment weekday window | `tm` | — | Server-timezone dependent |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / effect |
|---|---|---|---|
| Command Handler (opcode dispatch) | One free function per message family, `switch(std->Type)` in the dispatcher | `ProcessClientMessage.cpp:65-311` | Uniform inbound adapter; enables one-file-per-command auditing |
| Transaction Script | Each handler is a linear script over globals; no domain objects | all 40 files | Simplest possible shape; no reuse, high duplication |
| Forwarder / Delegation | Handler validates ranges, then calls one `Server.cpp` function | AccountSecure, CapsuleInfo, Deprivate, RemoveParty, ChallangeConfirm, War, GuildAlly | Keeps authoritative logic in one place (e.g. `Challange`, `DoDeprivate`) |
| Token / Nonce correlation | `pMob[].LastReqParty` compared on accept | `_MSG_SendReqParty.cpp:76`, `_MSG_AcceptParty.cpp:42` | Anti-spoof for party joins; also satisfies the synthetic re-invite from `RemoveParty` |
| Commit-before-roll | Inputs destroyed before the RNG is evaluated | CombineItem, CombineItemAlquimia, CombineItemShany, Agatha (partial), Extracao (stone) | Server-authoritative consumption; makes rollback impossible |
| Keep-the-donor | Deliberate `if (i == 1) continue;` in the consumption loop | `_MSG_CombineItemAgatha.cpp:60-61`, `_MSG_CombineItemTiny.cpp:61` | Preserves the effect-bearing item on failure |
| Double-entry audit | `ItemLog` for items, `Log` for actions, `CrackLog` for integrity | 31 handlers | Forensic trail; 9 handlers have none |
| Crack counter (reputation scoring) | `AddCrackError(conn, weight, type)`; logout at 2e9 | `Server.cpp:684-707` | Soft anti-cheat; weights differ per handler; reset by `Restart` |
| Grid multicast (spatial broadcast) | `GridMulticast(x, y, msg, skip)` | Motion, PKMode, QuitTrade, InviteGuild, Deprivate, SendAutoTrade, ReqBuy | Area-of-interest publication without subscriber lists |
| Guard clause ladder | Sequential `if (bad) { message; return; }` | all handlers | Flat, readable, but duplicated per file |
| Two-strike handshake | Request packet stores `RankingTarget`; accept packet echoes it | `_MSG_ReqRanking.cpp:40-46,56` | Reciprocity proof for duels |
| Positional `goto` for fee bypass | `goto label_tel` | `_MSG_ReqTeleport.cpp:36,53` | Single instance in the family; skips payment for zone-4 clan |
| RNG normalisation idiom | `rand() % 115; if (>= 100) -= 15` | 7 crafting handlers | Uniform 0..99 from a 115-wide modulo |
| Debug bypass flag | `LOCALSERVER` in every craft success test | 7 crafting handlers | Guarantees success on one hard-coded DB address |

### 9.1 Architectural boundaries (as observed, not as intended)

- **Boundary "client input is untrusted" is enforced partially.** Field-level range checks exist for indices and amounts; packet `Size` is never validated; `Motion` is broadcast verbatim; `SetShortSkill` accepts arbitrary bytes.
- **Boundary "persistent state changes go through the DB" is not enforced.** `InviteGuild` mutates `MOB.Guild/GuildLevel` locally and notifies nobody except via a later save; `ChangeCity` rewrites a persisted bit-field with no DB interaction; `Challange` writes treasury balances held in `MOB.Exp` of an NPC.
- **Boundary "one handler, one concern" is broken twice** (`Challange` = dialogue + tax withdrawal; `Restart` = respawn + crack reset).
- **Layering is conventional only.** Handlers reach directly into `pUser[]`/`pMob[]` and into `Server.cpp` internals (`Pista`, `GuildImpostoID`, `pChallangerMoney`).

---

## 10. Technical Debt & Risks

| ID | Risk level | Component area | Issue | Impact |
|---|---|---|---|---|
| RISK-01 | **High** | All 40 handlers | Packet `Size` is never validated before casting `char*` to the target struct; a short frame makes every field read past the buffer | Out-of-bounds read → garbage indices → potential write to arbitrary `pMob[]`/`pUser[]` slot |
| RISK-02 | **High** | `SetShortSkill` | No mode/HP guard and no value validation; writes 20 client-controlled bytes into character state | Client can persist arbitrary skill ids; use-site enforcement unverified |
| RISK-03 | **Medium** | `Deposit`, `Withdraw` | `int tcoin = coin + pUser[conn].Coin` with both operands up to 2e9 — signed overflow before the range test (UB; `-fwrapv`-style behaviour in MSVC) | A crafted pair could wrap into the accepted range and mint coin |
| RISK-04 | **High** | `ReqBuy` | Free-slot loop bounded by `MAX_CARRY-4` then compared to `MAX_CARRY` (`_MSG_ReqBuy.cpp:106-118`) — the full-inventory branch is unreachable | Purchase writes into `Carry[60]` (equipment region) when all 60 slots are full |
| RISK-05 | **Medium** | 8 combine handlers | Duplicate `InvenPos` entries are not rejected; the same slot can satisfy two recipe inputs | Duplication of results (e.g. Ancient/Ailyn base) from a single item |
| RISK-06 | **Medium** | `SendAutoTrade` helper | `AddMessage((char*)&sm, sizeof(MSG_UpdateCarry))` sends the wrong length for a `MSG_SendAutoTrade` | Truncated or over-read listing; client-side listing corruption |
| RISK-07 | **Low** | `GetMatchCombineTiny` | `BASE_GetItemAbility(...EF_ITEMTYPE) != 4 \|\| ... != 5` is always true — the item-type gate does nothing | Non-donor item types pass validation |
| RISK-08 | **Medium** | `CombineItemTiny` | `sprintf(tt, g_pMessageStringTable[_NN_Processing_Complete], MobName, itemName)` passes arguments to a format used without arguments elsewhere | Format-string read of stack; garbage message text |
| RISK-09 | **Medium** | `ReqRanking` | `DuelParm != 1 && DuelParm != 2 \|\| GuildLevel && GuildLevel` — precedence makes the guild-rank restriction apply to 1v1/type 3 instead of team duels | Unintended eligibility; unranked players blocked from 1v1 duels |
| RISK-10 | **Medium** | `Restart` | `pUser[conn].NumError = 0` clears the anti-cheat counter | Cheaters can reset accumulated crack points by dying/restarting |
| RISK-11 | **Medium** | `DeleteItem` | Server clears the slot but sends no item update (`:48`) | Permanent client/server desync if the client did not optimistically remove it |
| RISK-12 | **Medium** | `CombineItemAilyn` | 50,000,000 deducted at `:73` and again at `:124` | Double charge; 100M effective cost vs 50M advertised |
| RISK-13 | **Low** | `CombineItemAilyn` | `if (rand() % 1 == 0)` is always true | Dead randomisation; the "random which item wins" intent is not implemented |
| RISK-14 | **Medium** | `ReqBuy` tax routing | Commission credited to the village treasury **and** independently to zone 4 (`:165-180`) | Double taxation revenue; economy inflation |
| RISK-15 | **Low** | `ReqBuy` | Seller pocket credit silently skipped when already at 2e9 (`:158-159`) | Lost proceeds with no notification |
| RISK-16 | **Medium** | `Challange` | `PutItem` failure inside the tax loop returns without the completion message; below-1e9 remainder discarded in the multi-billion branch | Silent partial payout; unrecoverable remainder |
| RISK-17 | **Low** | `InviteGuild` | Membership written locally; no DB save triggered in the handler | Membership lost on crash before the next periodic save |
| RISK-18 | **Low** | `Challange` / `ReqTeleport` / `ReqBuy` | Treasury caps differ: 2e9, 2e11, 2e12 for the same conceptual balance | Inconsistent economic ceilings; overflow at the lowest cap |
| RISK-19 | **Medium** | Crafting suite | `LOCALSERVER == 1` (DB address `192.168.0.50`) forces every craft to succeed | A mis-set config turns production into an item faucet |
| RISK-20 | **Low** | `PutoutSeal` | `CharLogOut` precedes the DB capsule write; item is consumed and never restored on DB failure | Item loss with character already logged out |
| RISK-21 | **Low** | Code hygiene | Dead code: `zone != 5` after `zone < 5`; `Index` unused in CapsuleInfo; `int Server =` unused in InviteGuild; `if (i == MAX_PARTY)` after `< MAX_PARTY` loop | Confuses reviewers; hides real bugs |
| RISK-22 | **Low** | Code hygiene | Duplicated PK-rectangle logic in `PKMode` and `QuitTrade`; duplicated `SendHpMode || AddCrackError` idiom in 9 handlers; duplicated `CombineComplete` block in 8 | Rule drift whenever one copy is changed |
| RISK-23 | **Low** | `RemoveParty`, `CharacterLogout` | Every solo logout logs `err,RemoveParty but worng leader` | Log noise masks real errors |
| RISK-24 | **Low** | `DeleteItem` | `sIndex` range-checked but never compared to the actual slot content | Client desync destroys the wrong item |
| RISK-25 | **Low** | `Motion` | Client packet broadcast unmodified, `ID` not rewritten | Client can animate as another entity |

---

## 11. Test Coverage Analysis

### 11.1 Result: no automated tests exist anywhere in the repository

| Component | Unit tests | Integration tests | Coverage | Test quality |
|---|---|---|---|---|
| AcceptParty | 0 | 0 | 0% | Not tested |
| AccountSecure | 0 | 0 | 0% | Not tested |
| CapsuleInfo | 0 | 0 | 0% | Not tested |
| Challange | 0 | 0 | 0% | Not tested |
| ChallangeConfirm | 0 | 0 | 0% | Not tested |
| ChangeCity | 0 | 0 | 0% | Not tested |
| CharacterLogout | 0 | 0 | 0% | Not tested |
| CombineItem | 0 | 0 | 0% | Not tested |
| CombineItemAgatha | 0 | 0 | 0% | Not tested |
| CombineItemAilyn | 0 | 0 | 0% | Not tested |
| CombineItemAlquimia | 0 | 0 | 0% | Not tested |
| CombineItemExtracao | 0 | 0 | 0% | Not tested |
| CombineItemLindy | 0 | 0 | 0% | Not tested |
| CombineItemShany | 0 | 0 | 0% | Not tested |
| CombineItemTiny | 0 | 0 | 0% | Not tested |
| CreateCharacter | 0 | 0 | 0% | Not tested |
| DeleteCharacter | 0 | 0 | 0% | Not tested |
| DeleteItem | 0 | 0 | 0% | Not tested |
| Deposit | 0 | 0 | 0% | Not tested |
| Deprivate | 0 | 0 | 0% | Not tested |
| GuildAlly | 0 | 0 | 0% | Not tested |
| InviteGuild | 0 | 0 | 0% | Not tested |
| Motion | 0 | 0 | 0% | Not tested |
| NoViewMob | 0 | 0 | 0% | Not tested |
| PKMode | 0 | 0 | 0% | Not tested |
| PutoutSeal | 0 | 0 | 0% | Not tested |
| QuitTrade | 0 | 0 | 0% | Not tested |
| RemoveParty | 0 | 0 | 0% | Not tested |
| ReqBuy | 0 | 0 | 0% | Not tested |
| ReqRanking | 0 | 0 | 0% | Not tested |
| REQShopList | 0 | 0 | 0% | Not tested |
| ReqTeleport | 0 | 0 | 0% | Not tested |
| ReqTradeList | 0 | 0 | 0% | Not tested |
| Restart | 0 | 0 | 0% | Not tested |
| SendAutoTrade | 0 | 0 | 0% | Not tested |
| SendReqParty | 0 | 0 | 0% | Not tested |
| SetShortSkill | 0 | 0 | 0% | Not tested |
| SplitItem | 0 | 0 | 0% | Not tested |
| War | 0 | 0 | 0% | Not tested |
| Withdraw | 0 | 0 | 0% | Not tested |

### 11.2 Evidence for "no tests"

1. **Solution inventory.** `W2PP Code Project.sln` declares exactly two projects — `DBSrv` (`Code\DBSrv\DBSrv.vcxproj`) and `TMSrv` (`Code\TMSrv\TMSrv.vcxproj`) — plus a "Solution Items" folder. There is no test project, no `tests` folder, and no `*.Tests` assembly.
2. **Filename search.** A recursive search over the whole project root (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`) for `*test*`, `*Test*`, `*.spec.*`, `*_test.*` returned **zero** files. The only hits for the word "test" in source are `TESTSERVER` (`Server.cpp:3585`), the `User`-index comparisons in comments, and `pChallangeMoney`-style identifiers.
3. **Build-file search.** No `CMakeLists.txt`, `Makefile`, `*.yml`, `*.yaml`, or CI configuration exists anywhere in the tree.
4. **Framework search.** `Code/TMSrv/TMSrv.vcxproj` contains 75 `ClCompile` entries, all production translation units (`Basedef.cpp`, `CPSock.cpp`, 60+ `_MSG_*.cpp`, `Server.cpp`, …) and no reference to GoogleTest, Catch2, CppUnit, doctest, MS Test or `vstest`.
5. **Test hooks in production code.** The only test-like switch is `LOCALSERVER` (`Server.cpp:3588`, auto-set when `DBServerAddress == "192.168.0.50"`), which forces crafting success. It is a deployment convenience, not a test harness, and it has no assertion or reporting mechanism.

### 11.3 Testability assessment (structural, no tests written)

| Factor | Observation | Consequence for any future test |
|---|---|---|
| Global state | `pUser[MAX_USER]`, `pMob[MAX_MOB]`, `g_pItemList`, `g_pGuildZone` are process globals | A test must reset ~2 MB of state between cases and cannot run cases in parallel |
| Return values | Every handler returns `void`; outcomes are only observable through outbound packets and globals | Assertions must capture `cSock` writes or diff `pUser`/`pMob` |
| Hidden I/O | `Log`/`ItemLog`/`CrackLog` write files; `DBServerSocket.SendOneMessage` writes a socket | Requires fakes for the socket layer and a redirectable log sink |
| Time dependence | `InviteGuild` (weekday), `Challange` (WeekMode), crafting (RNG) | Needs clock and RNG injection; neither is abstracted |
| Non-determinism | `rand()` with no per-call seeding, plus `LOCALSERVER` bypass | Craft outcomes cannot be asserted unless `rand` is replaced |
| Coupled helpers | `PutItem`, `RemoveTrade`, `SendHpMode` called statically | Cannot be stubbed without link-time seams |
| Package boundary | Everything in one `.cpp` + aggregate header | A test must link the whole TMSrv object set or use `#include "the .cpp"` |

**Highest-value missing tests** (by risk, no implementation proposed): BR-INV-08/10 ceiling arithmetic (RISK-03), BR-SHP-11 inventory-space boundary (RISK-04), BR-CRF-07 cost accounting (RISK-12), BR-PTY-07 level-window boundary (the `+1` asymmetry), BR-CHL-02 payout arithmetic (RISK-16), BR-SHP-12 commission arithmetic (RISK-14).

---

## 12. Data Models and Validation Summary

### 12.1 Persistent/authoritative fields mutated by this family

| Field | Type | Mutated by | Validation before mutation |
|---|---|---|---|
| `pMob[].MOB.Carry[0..59]` | `STRUCT_ITEM[64]` | DeleteItem, SplitItem, all crafts, ReqBuy | Slot bounds; item identity (`memcmp`) in crafts; whitelist in SplitItem |
| `pMob[].MOB.Equip[15]` | `STRUCT_ITEM` | CombineItemLindy | Quest flags + level + clan only |
| `pUser[].Cargo[0..127]` | `STRUCT_ITEM[128]` | SendAutoTrade (read), ReqBuy (clear) | `CarryPos` bounds + byte match |
| `pMob[].MOB.Coin` | `int` | Deposit, Withdraw, ReqBuy, SendAutoTrade, InviteGuild, all crafts, Challange | Range and 2e9 ceiling (with RISK-03 overflow) |
| `pUser[].Coin` | `int` | Deposit, Withdraw, ReqBuy | Range and 2e9 ceiling |
| `pMob[].MOB.Guild` / `GuildLevel` | `int` / `unsigned char` | InviteGuild, Deprivate | Guild existence, clan equality, rank matrix |
| `pMob[].MOB.Merchant` | `unsigned char` | ChangeCity | None |
| `pMob[].MOB.SkillBar[4]` | `unsigned char[4]` | SetShortSkill | None |
| `pUser[].CharShortSkill[16]` | `unsigned char[16]` | SetShortSkill | None |
| `pUser[].PKMode` | `int` | PKMode | `!= 0` normalisation only |
| `pUser[].TradeMode` | `int` | SendAutoTrade (1), RemoveTrade (0) | Location + slot rules |
| `pUser[].AutoTrade` | `MSG_SendAutoTrade` | SendAutoTrade, ReqBuy, RemoveTrade | Full slot validation |
| `pUser[].Trade` | `MSG_Trade` | RemoveTrade (zeroed) | None (teardown) |
| `pMob[].Leader`, `PartyList[12]` | `int`, `short[12]` | AcceptParty, RemoveParty | Level window, capacity, correlation token |
| `pMob[].LastReqParty` | `int` | SendReqParty, RemoveParty | None (write-only token) |
| `pMob[].extra.QuestInfo.Arch.Level355/370` | bit flags | CombineItemLindy | Level + fame + quest gates |
| `pMob[].extra.Fame` | `int` | CombineItemLindy (-1) | `Fame > 0` for level 369 |
| `pMob[GuildImpostoID[z]].MOB.Exp` | `long long` (in `int`-era struct) | Challange, ReqTeleport, ReqBuy | Caps per call site (2e9 / 2e11 / 2e12) |
| `g_pGuildZone[z].ChallangeGuild`, `pChallangerMoney[z]` | `int` | Challange | Fame bid comparison |
| `GuildInfo[g].Fame`, `.Sub1..3` | `int` | Challange, Deprivate | `>= 100` gate; first-fit sub-slot |
| `pUser[].NumError` | `int` | AddCrackError (all guarded handlers), Restart (reset) | Logout at 2e9 |
| `pUser[].LastReceiveTime` | `int` | Dispatcher only (`ProcessClientMessage.cpp:58`) | — |

### 12.2 Validation coverage matrix (guard idioms)

| Handler | Mode guard | HP guard | Crack point | Target range | Amount/index bounds | Audit log |
|---|---|---|---|---|---|---|
| AcceptParty | implicit (leader `USER_PLAY`) | no | `CrackLog` on token mismatch | yes | n/a | Log |
| SendReqParty | implicit (target `USER_PLAY`) | no | no | yes | n/a | Log |
| RemoveParty | no | no | no | yes (clamped) | n/a | via Server.cpp |
| CharacterLogout | via `CharLogOut` | no | no | n/a | n/a | no |
| GuildAlly / War | no | no | no | yes (1..65535) | n/a | no |
| InviteGuild | implicit (target not checked) | no | no | yes | yes (coin) | Log |
| Deprivate | via `DoDeprivate` | no | no | yes | n/a | Log |
| Challange | no | no | no | yes | yes (fame) | Log |
| ChallangeConfirm | no | no | no | yes | n/a | via Challange |
| REQShopList | yes | yes | 10/22 | yes | n/a | Log (error only) |
| SendAutoTrade | yes | yes | 10/88 | yes (self) | yes (12 slots) | Log (none on rejection) |
| ReqTradeList | yes | yes | 10/87 | yes | n/a | Log (error only) |
| ReqBuy | yes | yes | 10/86 | yes | yes | ItemLog |
| ReqRanking | yes (target) | no | no | yes | yes | Log |
| ReqTeleport | **no** | **no** | no | via table | yes (coin) | Log |
| ChangeCity | **no** | **no** | no | n/a | city 0..4 | Log |
| Restart | yes | no | no (resets) | n/a | n/a | Log |
| Motion | yes | yes | 4/6 | n/a | **no** | Log |
| NoViewMob | no | no | no | yes | n/a | Log (error only) |
| PKMode | **no** | **no** | no | n/a | `!= 0` | Log |
| QuitTrade | yes | yes | 10/17 | n/a | n/a | no |
| CreateCharacter | `USER_SELCHAR` | no | no | n/a | name policy | Log |
| DeleteCharacter | `USER_SELCHAR` | no | no | n/a | **no name policy** | Log |
| AccountSecure | no | no | no | conn range | **no token validation** | no |
| CapsuleInfo | no | no | no | no | no | no |
| DeleteItem | yes | no | no | yes | yes (index range only) | ItemLog |
| SplitItem | yes | no | no | yes | yes + whitelist | ItemLog |
| Deposit | yes | yes | 10/1 | n/a | yes | Log |
| Withdraw | yes | yes | 10/2 | n/a | yes | Log |
| PutoutSeal | yes | yes | 1/15 | yes | item identity | Log |
| All 8 combines | via craft flow | no | no | yes (`InvenPos`) | recipe + identity | ItemLog |
| SetShortSkill | **no** | **no** | no | n/a | **no** | no |

### 12.3 Configuration and environment handling

| Setting | Source | Default | Effect in this family |
|---|---|---|---|
| `PARTY_DIF` | `gameconfig.txt` line 13 / admin `partydif` | 150 | Party level window (BR-PTY-07) |
| `ValidGuild` | `Server.cpp:55` | `MAX_GUILDZONE` = 5 | Challenge zone bounds (BR-CHL-01) |
| `g_pTinyBase`, `g_pShanyBase`, `g_pAilynBase`, `g_pAgathaBase`, `g_pAnctChance[3]` | `CReadFiles.cpp:446-584` | 0 (loaded) | Craft success rates (BR-CRF-03, 06, 08, 20, 22) |
| `NewbieEventServer` | `ProcessSecMinTimer.cpp:2041-2056` | 0 | Auto-trade enable (BR-SHP-03); PK flag override (BR-PVP-03) |
| `WeekMode` | event scheduler | 0 | Siege dialogue (BR-CHL-03/04) |
| `BrState`, `BRItem` | Battle Royale scheduler (`Server.cpp:6297-6382`) | 0 | Party exclusion (BR-PTY-08) |
| `LOCALSERVER` | `DBServerAddress == "192.168.0.50"` (`Server.cpp:3587`) | 0 | Forces craft success (RISK-19) |
| `g_pGuildZone[z].CityTax` | zone table | 0 | Shop tax, player-shop commission (BR-SHP-12) |
| `g_pGuildZone[4].Clan` | zone table | 0 | Free teleport (BR-TEL-03) |
| `ServerIndex`, `ServerGroup` | server config | — | Challenge citizenship (BR-CHL-06), guild names |
| `RvRState`, `CastleState`, `GTorreState` | event schedulers | 0 | PK rectangles (BR-PVP-03) |
| `timeinfo->tm_wday` | host clock | — | Recruitment window (BR-GLD-08) |

---

## 13. Ambiguities and Assumptions Register

| ID | Item | Observation | Confidence |
|---|---|---|---|
| AMB-01 | `zone != 5` in `_MSG_Challange.cpp:35` | Unreachable after the `zone < ValidGuild` (=5) check; suggests a former 6-zone model | High that it is dead code; medium on intent |
| AMB-02 | `WeekMode == 0` treated as "announce" | The guard `WeekMode && ...` excludes 0 from the special branch | High (direct read) |
| AMB-03 | `_MSG_ReqRanking` type 4 | `Basedef.h:2135` documents 0/1/2/3, but the handler treats 4 as acceptance | High that 4 = accept; medium on the header's accuracy |
| AMB-04 | Player-shop gating on `NewbieEventServer != 0` | Reads as an inverted condition relative to the message name | Medium |
| AMB-05 | Zone-4 double tax credit in `ReqBuy` | Both the local and zone-4 treasuries are credited in the same purchase | Medium that it is unintended |
| AMB-06 | `ChangeCity` has no guard | Could be intentional (client sends after every move) | Medium |
| AMB-07 | `InviteGuild` "Saturday" message on Sunday | Message index `_NN_NotEquip_Saturday` (390) with `tm_wday == 0` | High on the mismatch; low on which is wrong |
| AMB-08 | `RemoveParty` condition `Leader != -1 && Leader` | `-1` and `0` both take the "leader" path | High on behaviour |
| AMB-09 | Alchemy `Special[2]` semantics | Assumed to be a luck-like stat; the stat caps are defined outside this component | Low |
| AMB-10 | `MOB.Exp` used as a `long long` money field for treasuries | Type width in `STRUCT_MOB` was not resolved here; the arithmetic uses `long long` | Medium |
| AMB-11 | `Challange` bid uses pre-deduction fame | `coin = GuildInfo[Guild].Fame` **before** `Fame -= 100` | High (direct read) |
| AMB-12 | `CombineItemAilyn` `rand() % 1` | Could be a placeholder for a coin-flip | High that it is always true |
| AMB-13 | `SplitItem` whitelist 2390..2419 | Item family identity not resolved from `g_pItemList` | Medium that it is the ammo/consumable range |
| AMB-14 | `_MSG_SendAutoTrade` `Index` field | Echoed but never read by this family | High |
| AMB-15 | `MSG_Type` collision between `_MSG_War` (14\|…) and `_MSG_DeleteCharacter`/`_MSG_CreateCharacter` (17/15\|) | Direction flags disambiguate; no collision in practice | High |
| AMB-16 | `ReqBuy` uses an explicit rectangle instead of `GetInView` | Equivalent, but duplicated | High |
| AMB-17 | `CombineItemShany` class precedence | `MORTAL \|\| (ARCH && level < 355)` blocks Arch below 355, allows Celestial | High on behaviour; medium on intent |
| AMB-18 | `_MSG_PutoutSeal` item 3443 effect payload | The effect slot is required non-zero but its meaning is not read here | High |
| AMB-19 | `DeleteItem` ignores `sIndex` equality | Slot is treated as authoritative | High |
| AMB-20 | `Motion` broadcast with client `ID` | No rewrite to `conn` | High |

---

## 14. Quality Attributes Assessment

| Attribute | Rating | Assessment |
|---|---|---|
| **Maintainability** | Low | 40 files, one function each, heavy copy-paste (guard blocks, `CombineComplete` handshake, PK rectangles). No shared helpers means any rule change must be replicated manually. |
| **Understandability** | Medium | Each handler is short and linear (23-164 lines); readability is hurt only by deep nesting in `Challange` (5 levels) and the unparenthesised PK expression. |
| **Testability** | Very low | Global state, `void` returns, hidden I/O, unseeded `rand()`, static helper calls, no seams. |
| **Reliability** | Medium-low | Three identified arithmetic/bounds defects (RISK-03, 04, 12); several paths consume items before the outcome is known. |
| **Security** | Low-medium | No packet-size validation; one anti-spoof token for the whole family; item destruction is client-initiated with only a range check. |
| **Performance** | Adequate | Handlers are O(1) except `RemoveParty` (O(12·7·3)), `ReqBuy` (O(60)) and the crafting identity scan (O(8)). `Motion` logging on every packet is the main avoidable cost. |
| **Consistency** | Low | 4 guard idioms, 9 crack-point pairs, 2 spellings of the HP test, 3 different treasury caps, 2 different PK-state semantics. |
| **Auditability** | Medium | 31/40 handlers log; item mutations are well covered by `ItemLog`; 9 handlers (including all three DB forwarders for guild/war/account) are silent. |
| **Portability** | Very low | Win32, MSVC, `#pragma pack`, 32-bit `long`/`int` assumptions, direct struct serialisation over sockets. |

### 14.1 Code smells detected

| Smell | Instances | Location |
|---|---|---|
| Duplicated code block | 8x | Combine identity-check loop in all combine handlers |
| Duplicated code block | 2x | PK-state computation (`_MSG_PKMode.cpp:45-60`, `_MSG_QuitTrade.cpp:39-51`) |
| Duplicated code block | 9x | `Hp == 0 || Mode != USER_PLAY` + `SendHpMode` + `AddCrackError` |
| Dead code | 5 | `zone != 5`; `Index` in CapsuleInfo; `int Server` in InviteGuild; `if (i == MAX_PARTY)` in ReqBuy; `if (amount > 1) … else memset` in SplitItem |
| Long method | 3 | `ReqBuy` (164), `CombineItem` (124), `CombineItemAilyn` (124) — long for this codebase |
| Magic numbers | pervasive | 1e9, 5e7, 1e8, 7e2, 4000000, 100000000, 100000, 2000000000, 200000000000, 2000000000000, 2604/1708/2648/1744, 1023..1280, 2123..2148 |
| Type punning | 1 | `memcpy(&Carry[ipos].sIndex, &m->Item[0], sizeof(STRUCT_ITEM))` (`_MSG_CombineItem.cpp:96`) |
| Positional goto | 1 | `_MSG_ReqTeleport.cpp:36/53` |
| Comment-free complex logic | many | `Challange` tax loop, `ReqBuy` tax routing, `AcceptParty` level window |
| Inconsistent naming | 1 | `REQShopList` (file/function) vs `ReqBuy`/`ReqTeleport`/`ReqRanking`/`ReqTradeList` |
| Dead `if` after return-path change | 2 | `_MSG_SplitItem.cpp:54-58`, `_MSG_ReqBuy.cpp:114-118` |
| Mixed-language literals | 1 | `_MSG_CombineItemExtracao.cpp:99,106` (`extra��o`) |

---

## 15. Conclusion

The "Remaining Client Command Handlers" family is a **thin, uniform, highly duplicated inbound adapter layer** whose business substance lives mostly in `Server.cpp`, `GetFunc.cpp` and `SendFunc.cpp`. Its 40 handlers divide cleanly into three archetypes:

1. **Forwarders** (11 handlers) — range-check, rewrite the header, hand off to DBSrv; all semantic authority is remote.
2. **Orchestrators** (12 handlers) — validate session state, then call a single authoritative helper (`RemoveParty`, `DoDeprivate`, `Challange`, `DoRanking`, `DoRecall`, `DoTeleport`, `CharLogOut`, `RemoveTrade`, `Send*`).
3. **Transactional scripts** (17 handlers) — perform the whole operation inline: the eight crafting recipes, split/delete/deposit/withdraw, seal capsule creation, auto-trade registration and purchase, restart, PK toggle.

The family's strengths are uniform shape, short functions, and a consistent client-acknowledgement protocol (`_MSG_CombineComplete` codes, `SendHpMode`, `_MSG_PKInfo`). Its weaknesses are equally uniform: no packet-size validation anywhere, four incompatible guard idioms, nine distinct crack-point weightings, three different treasury caps for one conceptual balance, and eight different item-loss policies across eight crafting recipes that share one skeleton. The most consequential identified defects are the `ReqBuy` free-slot off-by-two (RISK-04), the `Deposit`/`Withdraw` signed-overflow window (RISK-03), the `CombineItemAilyn` double charge (RISK-12), and the complete absence of automated tests for code that destroys items and moves currency.

---

*End of report.*
