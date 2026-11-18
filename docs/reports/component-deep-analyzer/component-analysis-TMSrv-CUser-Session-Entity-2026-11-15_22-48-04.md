# Component Deep Analysis Report

**Component:** TMSrv CUser Session Entity
**Primary location:** `Code/TMSrv/CUser.h`, `Code/TMSrv/CUser.cpp`
**Project:** W2PP — decompiled C/C++ (Win32, MSVC) W2 MMORPG server emulator
**Scope analysed:** entire project root `E:\wyd\test-setup\w2pp`, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis date:** 2026-11-15

> Note: `Code/DBSrv/CUser.cpp` / `Code/DBSrv/CUser.h` are a **different** component (DBSrv session entity) and were explicitly excluded from this analysis. They were read only to confirm that they are a distinct declaration of the same class name in a different project.

---

## 1. Executive Summary

`CUser` (TMSrv) is the **per-connection session record of the game/world server**. One static array, `CUser pUser[MAX_USER]` (`Code/TMSrv/Server.cpp:269`, `MAX_USER = 1000` in `Code/Basedef.h:56`), holds every client session. The array index is the connection identity: `conn`, `User`, `bb`, `SaveCount`, and `m->ID` all mean "index into `pUser`", and the same index addresses `pMob[index]` for the in-world avatar. The class therefore acts simultaneously as:

* a **transport endpoint wrapper** (`CPSock cSock` plus receive/send/proc positions),
* an **identity record** (`AccountName[16]`, `IP`, `Mac[4]`, `Slot`, `Admin`, `OnlyTrade`, `MuteChat`),
* a **state machine** (`Mode` with seven live values),
* an **inventory/money shadow** for out-of-world storage (`Cargo[128]`, `Coin`, `Donate`),
* a **transaction staging area** for player-to-player trade and offline/vendor shops (`Trade`, `AutoTrade`, `TradeMode`),
* an **anti-cheat scoreboard and rate-limit store** (`NumError`, `LastAttackTick`, `LastIllusionTick`, `LastMove`, `Message`, `PotionTime`, `cProgress`, `ReqHp`, `ReqMp`).

### Key findings

1. **The class is a data-only entity with three methods.** `CUser()` (`Code/TMSrv/CUser.cpp:25`), `~CUser()` (`:46`, empty), `AcceptUser(int)` (`:51`), `CloseUser()` (`:81`). All business behaviour lives in *consumers*: 61 of the 63 TMSrv translation units reference `pUser[...]` (1,630 accesses), and 41 files read or write `Mode` (218 accesses). The entity itself is the most afferently coupled type in TMSrv.

2. **Session lifetime is governed by a hand-rolled integer state machine** (`USER_EMPTY=0`, `USER_ACCEPT=1`, `USER_LOGIN=2`, `USER_SELCHAR=11`, `USER_CHARWAIT=12`, `USER_WAITDB=13`, `USER_PLAY=22`, `USER_SAVING4QUIT=24`; `Code/TMSrv/CUser.h:25-37`). Transitions are scattered across 15 write sites in 5 files; there is no single transition function and no assertion of legality. `USER_PLAY` is by far the dominant gate: 119 negative and 51 positive comparisons in TMSrv.

3. **`CloseUser()` is a deliberately shallow reset** (`Code/TMSrv/CUser.cpp:81-92`): it closes the socket, zeroes `cSock.Sock`, `IsBillConnect`, sets `Mode = USER_EMPTY` and `AccountName[0] = 0`. It does **not** clear `Cargo`, `Coin`, `Donate`, `Slot`, `Trade`, `AutoTrade`, `TradeMode`, `PKMode`, `Mac`, `Admin`, `MuteChat`, `NumError` or any timer. Full re-initialisation is deferred to the next account/character confirmation (`Code/TMSrv/ProcessDBMessage.cpp:386-457` and `:728-982`), so correctness depends on those two paths always running. This is the single largest correctness risk of the component (see section 11).

4. **Two confirmed defects inside the component boundary**: (a) `pUser[n].Admin = 1` at `Code/TMSrv/ProcessDBMessage.cpp:1005` uses the loop variable `n` left over from the spawn-zone search rather than the session index `conn`; (b) the `Admin` field is written at three sites and **read at none**, so the "admin flag" advertised in the architecture report is currently inert — GM authority is actually derived from `pMob[conn].MOB.CurrentScore.Level - 1000` (`Code/TMSrv/_MSG_MessageWhisper.cpp:789`) and `IsImple()` always returns 0 (`Code/TMSrv/Server.cpp:6829-6832`).

5. **Zero automated tests exist** for this component or for the project. There is no test project in `W2PP Code Project.sln`, only two `.vcxproj` files (DBSrv, TMSrv), no file matching `*test*`, `*spec*`, `*mock*`, and no assertion/test framework reference in either project file. Verification is entirely manual/runtime.

6. **Coupling is extreme and intentional**: afferent coupling = 61 translation units / 1,630 accesses; efferent coupling = 3 (CPSock, the POD structs of `Basedef.h`, and the Win32/Winsock plus `Server.h` globals). Instability `I = Ce/(Ca+Ce) ≈ 3/64 ≈ 0.05` — the entity is a stable, depended-upon foundation and therefore maximally sensitive to any field change.

---

## 2. Data Flow Analysis

### 2.1 Session creation (accept path)

```
1. Winsock accept notification: MainWndProc case WSA_ACCEPT (Server.cpp:4006)
2. GetEmptyUser() scans pUser[1..999] for Mode == USER_EMPTY (Server.cpp:6743)
3. pUser[User].AcceptUser(ListenSocket.Sock) (Server.cpp:4020)
   3a. accept() -> tSock; failure -> FALSE, session stays EMPTY (CUser.cpp:56-59)
   3b. WSAAsyncSelect(tSock, hWndMain, WSA_READ, FD_READ|FD_CLOSE) (CUser.cpp:61)
       failure -> closesocket, FALSE
   3c. cSock.Sock / nRecvPosition / nProcPosition / nSendPosition := 0 (CUser.cpp:68-71)
   3d. IP := acc_sin.sin_addr.S_un.S_addr (network order) (CUser.cpp:74)
   3e. Mode := USER_ACCEPT, Unk3 := 0 (CUser.cpp:75-76)
4. Reserved-slot check: User >= MAX_USER - ADMIN_RESERV (990) -> message + CloseUser (Server.cpp:4022-4030)
5. Shutdown check: ServerDown != -1000 -> message + CloseUser (Server.cpp:4032-4039)
```

### 2.2 Inbound client message path

```
1. Win32 message WSA_READ (Server.cpp:3919)
2. GetUserFromSocket(wParam) linear scan on cSock.Sock (Server.cpp:6729)
3. FD_CLOSE or anything != FD_READ -> CloseUser(User) (Server.cpp:3929-3936)
4. pUser[User].cSock.Receive() with one retry (Server.cpp:3938-3968)
5. loop: pUser[User].cSock.ReadMessage(&Error,&ErrorCode) (Server.cpp:3975)
6. ProcessClientMessage(User, Msg, FALSE) (Server.cpp:4001)
   6a. header sanity: 0 <= std->ID < MAX_USER else drop (ProcessClientMessage.cpp:42-51)
   6b. ServerDown >= 120 -> drop (ProcessClientMessage.cpp:53-54)
   6c. pUser[conn].LastReceiveTime := SecCounter (ProcessClientMessage.cpp:57)
   6d. _MSG_Ping -> return; client packet with ClientTick == SKIPCHECKTICK -> drop (59-64)
   6e. switch(std->Type) -> Exec_MSG_* handler (66-311)
```

### 2.3 Authentication / character selection path (state-bearing)

```
1. Exec_MSG_AccountLogin  (Mode must be USER_ACCEPT)         _MSG_AccountLogin.cpp:21
   -> AccountName := _strupr(sscanf token); Mac[] capture   :68-78
   -> Mode := USER_LOGIN                                    :94
   -> m->Type := _MSG_DBAccountLogin -> DBSrv               :92
2. ProcessDBMessage(_MSG_DBCNFAccountLogin)                 ProcessDBMessage.cpp:354
   -> AccountName echo check, MAC block list                :358-383
   -> chat/admin/billing flags reset, OnlyTrade := 1        :386-395
   -> Mode := USER_SELCHAR                                  :400
   -> Cargo := DB Cargo, Coin := DB Coin, SelChar := DB sel :436-440
3. Exec_MSG_CharacterLogin (Mode must be USER_SELCHAR)      _MSG_CharacterLogin.cpp:21
   -> billing/level gates                                   :31-107
   -> Mode := USER_CHARWAIT; pMob[conn].Mode := MOB_USER    :76-77
4. ProcessDBMessage(_MSG_DBCNFCharacterLogin)               ProcessDBMessage.cpp:635
   -> pMob[conn].MOB := DB mob; Donate; CharShortSkill      :728-747
   -> session reset: Slot, NumError, LastMove/LastAction*,
      LastAttack*, Ranking*, CastleStatus, Trade sentinels,
      AutoTrade sentinels, TradeMode, PKMode, cProgress,
      ReqHp, ReqMp, Unk9 := 0xFF                            :819-846, 979-982
   -> spawn resolution + pMobGrid write                     :848-904
   -> Mode := USER_PLAY                                     :906
   -> downstream projection (SendScore/SendEtc/SendGridMob) :992-1076
```

### 2.4 Outbound path

```
1. Handler/SendFunc builds a POD struct into pUser[conn].cSock via AddMessage()
2. ProcessSecTimer, every second: if Mode != 0 and nSendPosition != 0
   -> cSock.SendMessageA(); failure -> CloseUser (ProcessSecMinTimer.cpp:201-221)
3. SendFunc helpers silently return when pUser[conn].Mode != USER_PLAY (e.g. SendFunc.cpp:1195-1201)
4. AddMessage overflow in a SendFunc also closes the session (e.g. SendFunc.cpp:556-557)
```

### 2.5 Session teardown paths

```
A. Socket close / FD_CLOSE / recv error        Server.cpp:3934, 3952, 3965
B. Idle timeout (720 s without any packet)     Server.cpp:4304-4322 via ProcessSecMinTimer.cpp:1181
C. Server shutdown drain (ServerDown == 120)   ProcessSecMinTimer.cpp:55-94
D. Login/protocol rejection                    _MSG_AccountLogin.cpp:37,52; ProcessDBMessage.cpp:364..1323
E. Outbound failure                            SendFunc.cpp (14 sites), ProcessSecMinTimer.cpp:218
F. GM kick                                     imple.cpp:1827
G. Anti-cheat threshold                        AddCrackError -> CharLogOut (Server.cpp:684-707)
-> all converge on CloseUser(conn) -> branch by Mode -> pUser[conn].CloseUser() -> Mode = USER_EMPTY
```

### 2.6 Persistence path

```
1. SaveUser(conn, Export)  -> MSG_DBSaveMob {MOB, Cargo, Coin, Slot, Donate,
                            AccountName, ShortSkill, affect, extra}   Server.cpp:7056
2. CloseUser(conn) while PLAY -> MSG_SavingQuit (same payload) + Mode := USER_SAVING4QUIT  Server.cpp:6880-6919
3. DBSrv replies _MSG_DBSavingQuit -> CloseUser(m->ID)   ProcessDBMessage.cpp:1137-1169
4. Round-robin save every 8 s (SaveCount)                ProcessSecMinTimer.cpp:169-199
5. CharLogOut: SaveUser(conn,1) + DeleteMob + Mode := USER_SELCHAR   Server.cpp:7085-7125
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Identity / allocation | Session identity is the array index; index 0 is never allocated (`GetEmptyUser`/`GetUserFromSocket` start at 1) | `Server.cpp:6729-6752` |
| BR-02 | Admission control | Slots >= `MAX_USER - ADMIN_RESERV` (990) are reserved and refused with a reconnect message | `Server.cpp:4022-4030`; `Basedef.h:56-57` |
| BR-03 | Admission control | New connections are refused unless `ServerDown == -1000` (normal running state) | `Server.cpp:4032-4039` |
| BR-04 | Initialization | `AcceptUser` sets only socket handles/positions, `IP`, `Mode=USER_ACCEPT`, `Unk3=0`; failure leaves the slot EMPTY | `CUser.cpp:51-79` |
| BR-05 | State machine | `Mode` may only move along the transitions in §3.2; handlers gate on exact values | `CUser.h:25-37`, 15 write sites |
| BR-06 | Dispatch guard | Packets with `ID` outside `[0, MAX_USER)` are dropped; all packets are dropped when `ServerDown >= 120` | `ProcessClientMessage.cpp:42-54` |
| BR-07 | Liveness | Every accepted packet refreshes `LastReceiveTime = SecCounter` | `ProcessClientMessage.cpp:56-57` |
| BR-08 | Anti-replay | Client packets whose `ClientTick == SKIPCHECKTICK` are discarded before dispatch | `ProcessClientMessage.cpp:63-64` |
| BR-09 | Authentication | `_MSG_AccountLogin` is only accepted in `USER_ACCEPT`; otherwise a crack log is written and nothing changes | `_MSG_AccountLogin.cpp:56-63` |
| BR-10 | Protocol compatibility | Account login requires `Size >= sizeof(MSG_AccountLogin)` **and** `ClientVersion == APP_VERSION` | `_MSG_AccountLogin.cpp:41-54` |
| BR-11 | Normalization | Account name is tokenised, upper-cased (`_strupr`) and NUL-terminated at `NAME_LENGTH-1` | `_MSG_AccountLogin.cpp:28, 76-80` |
| BR-12 | Brute-force throttle | Accounts with `CheckFailAccount(...) >= 3` are refused before the DB round-trip | `_MSG_AccountLogin.cpp:82-90` |
| BR-13 | Client fingerprint | `Mac[4]` is copied from the packet, or set to `0xFF..` when the packet is short | `_MSG_AccountLogin.cpp:65-70` |
| BR-14 | Ban enforcement | A MAC present in `pMac[]` (loaded from `../../DBSRV/Run/Mac.txt`, refreshed every 120 s) terminates the session at account confirmation | `ProcessDBMessage.cpp:369-383`; `CReadFiles.cpp:678-715`; `ProcessSecMinTimer.cpp:127-128` |
| BR-15 | Session/DB consistency | `_MSG_DBCNFAccountLogin` must echo the stored `AccountName` exactly, otherwise the session is closed | `ProcessDBMessage.cpp:358-367` |
| BR-16 | Field reset | At account confirmation the volatile flags are reset: `IsBillConnect, Whisper, Guildchat, PartyChat, KingChat, Admin, Chatting, Unk_2732, Unk_2728 = 0`, `OnlyTrade = 1` | `ProcessDBMessage.cpp:386-395` |
| BR-17 | Character slot bounds | Character selection slot must satisfy `0 <= Slot < MOB_PER_ACCOUNT` (4) | `_MSG_CharacterLogin.cpp:25-29`; `Basedef.h:71` |
| BR-18 | Billing gate | Character login is refused or deferred depending on `BILLING`, `CHARSELBILL`, `Unk_1816`, `Unk_2728`, level vs `FREEEXP`, and the `g_Hour` window | `_MSG_CharacterLogin.cpp:31-107` |
| BR-19 | Character creation/deletion | Only legal in `USER_SELCHAR`; transitions to `USER_WAITDB` and back to `USER_SELCHAR` on any DB result | `_MSG_CreateCharacter.cpp:27-47`; `_MSG_DeleteCharacter.cpp:26-37`; `ProcessDBMessage.cpp:588,604,617,630` |
| BR-20 | Full session init | `USER_PLAY` entry re-initialises `Slot, NumError, LastMove/LastAction/LastActionTick/LastAttack/LastAttackTick/LastIllusionTick, RankingTarget/Type, CastleStatus, Trade, AutoTrade, TradeMode, PKMode, cProgress, ReqHp/ReqMp, Unk9` | `ProcessDBMessage.cpp:819-846, 979-982, 906` |
| BR-21 | Login validation | Invalid class or an unobtainable spawn cell closes the session | `ProcessDBMessage.cpp:859-896` |
| BR-22 | Login repair | Non-positive HP at login is forced to 2 | `ProcessDBMessage.cpp:730-731` |
| BR-23 | Logout precondition | `CharLogOut` only proceeds when `Mode == USER_PLAY`; otherwise it logs an error and emits `_MSG_CNFCharacterLogout` without changing state | `Server.cpp:7085-7099` |
| BR-24 | Close semantics | `CloseUser` branches on `Mode`: never-played -> `_MSG_DBNoNeedSave`; PLAY/SAVING4QUIT -> save-quit; 0/ACCEPT -> plain close | `Server.cpp:6834-6926` |
| BR-25 | Save payload | Save/quit payload carries `MOB, Cargo[128], ShortSkill[16], affect, extra, AccountName, Coin, Donate, Slot` | `Server.cpp:6899-6915`, `7056-7083` |
| BR-26 | Save-ack handling | `_MSG_DBSavingQuit` replies `_MSG_DBNoNeedSave` for non-playing sessions and always closes | `ProcessDBMessage.cpp:1137-1169` |
| BR-27 | Reset scope | Member `CloseUser()` clears only `cSock`, `IsBillConnect`, `Mode`, `AccountName[0]` | `CUser.cpp:81-92` |
| BR-28 | Shutdown drain | With `ServerDown == 120`, exactly one live slot per second is closed, in slot order, until all are EMPTY | `ProcessSecMinTimer.cpp:55-94` |
| BR-29 | Idle timeout | 720 s of silence closes the session; a 1440 s clock-skew guard re-seeds the timestamp; checked every 32 s; `USER_SAVING4QUIT` is exempt | `Server.cpp:4304-4322`; `ProcessSecMinTimer.cpp:1181` |
| BR-30 | Periodic persistence | One session is saved per 8 s in round-robin (`SaveCount`), only when `Mode == USER_PLAY && pMob.Mode != MOB_EMPTY` | `ProcessSecMinTimer.cpp:169-199` |
| BR-31 | Billing eviction | In the save loop, `BILLING==2 && Unk_2728==1 && level >= FREEEXP && (hour <= 12 \|\| hour >= 19)` forces logout | `ProcessSecMinTimer.cpp:178-186` |
| BR-32 | Flush supervision | Each second, sessions with pending output are flushed; a failed send closes the session | `ProcessSecMinTimer.cpp:201-221` |
| BR-33 | Projection gating | All `SendFunc` projections return silently unless the session is `USER_PLAY` (and socket != 0 where checked) | `SendFunc.cpp:1195-1204` and 40+ peers |
| BR-34 | Storage model | `Cargo[128]` is the account warehouse; contents come only from the DB at account confirmation and are zeroed by the constructor | `CUser.cpp:32`; `ProcessDBMessage.cpp:436` |
| BR-35 | Deposit rule | Deposit requires live playing session, `0 <= amount <= 2e9`, `mob.Coin >= amount`, and `warehouse + amount <= 2e9` | `_MSG_Deposit.cpp:24-56` |
| BR-36 | Withdraw rule | Withdraw requires the symmetric conditions on `Cargo` coin | `_MSG_Withdraw.cpp:24-55` |
| BR-37 | Trade preconditions | Target in `[1,999]`, target `USER_PLAY`, money `0..mob.Coin`, items owned and unchanged, no `PKMode` on either side, no `Whisper` opt-out on either side | `_MSG_Trade.cpp:34-138` |
| BR-38 | Trade ownership invariants | Each traded slot must satisfy `-1 <= InvenPos < MAX_CARRY-4`, no duplicate positions, and `memcmp` identity with `MOB.Carry[pos]` | `_MSG_Trade.cpp:56-85, 192-223` |
| BR-39 | Trade exclusivity | A session in `TradeMode` (shop open) cannot attack, move, use items, buy or trade | `_MSG_Attack.cpp:24-31`; `_MSG_Action.cpp:46-50`; `_MSG_UseItem.cpp:58-62`; `_MSG_ReqBuy.cpp:32-42` |
| BR-40 | Trade confirmation | Both parties must set `MyCheck`; the stored `MSG_Trade` (including header tick/keyword) must match the incoming one byte-for-byte | `_MSG_Trade.cpp:225-238, 387-399` |
| BR-41 | Money ceiling | Individual trade amounts and both resulting balances must stay within `0..2,000,000,000` | `_MSG_Trade.cpp:251-299` |
| BR-42 | Trade settlement | Both inventories are recomputed by `BASE_CanTrade`, capacities checked, then items/coin swapped and both sessions saved | `_MSG_Trade.cpp:301-384` |
| BR-43 | Trade reset | `RemoveTrade` zeroes `Trade` and `AutoTrade`, re-arms both `-1` sentinels, and drops `TradeMode` with a mob refresh when playing | `Server.cpp:7319-7352` |
| BR-44 | Shop listing | `_MSG_SendAutoTrade` validates 12 slots: coin `0..1,999,999,999`, coin only with item, item blacklist, `EF_NOTRADE`, `CarryPos` in `[0,128)`, cargo identity, village location, `NewbieEventServer != 0` | `_MSG_SendAutoTrade.cpp:32-107` |
| BR-45 | Shop purchase | Buyer must be in view, price/tax/item/cargo must match the listing, buyer solvent, seller balance <= 2e9, buyer carry space, seller in a village | `_MSG_ReqBuy.cpp:20-160` |
| BR-46 | Anti-cheat score | `AddCrackError` accumulates into `NumError`; reaching 2,000,000,000 forces `CharLogOut`; score resets at login, at death-restart and on area clear | `Server.cpp:684-707, 8706`; `_MSG_Restart.cpp:24-30`; `ProcessDBMessage.cpp:822` |
| BR-47 | Attack rate limit | 800 ms minimum between attacks, 15 s future/past tick window, and 700 ms `isTime` floor per skill cast | `_MSG_Attack.cpp:59-97, 124-145` |
| BR-48 | Action rate limit | 900 ms between `_MSG_Action`/`_MSG_Action3` and 15 s tick window; repeated `_MSG_Action2` (stop) rejected | `_MSG_Action.cpp:92-145` |
| BR-49 | Potion cooldown | `PotionTime` enforces `PotionDelay` on volume-1 consumables | `_MSG_UseItem.cpp:98-112` |
| BR-50 | Broadcast cooldown | Kingdom/citizen broadcast chats are throttled to one per 3,000 ms using `Message` (`GetTickCount`) | `_MSG_MessageWhisper.cpp:1231-1270` |
| BR-51 | Reply memory | `LastChat` stores the last whisper correspondent for `/r`; the recipient's `LastChat` is seeded with the sender name | `_MSG_MessageWhisper.cpp:1295-1370` |
| BR-52 | Critical-progress plumbing | `cProgress` is handed to `BASE_GetDoubleCritical` as the client-reported critical accumulator | `_MSG_Attack.cpp:440`; `Basedef.cpp:6475+` |
| BR-53 | Primary-account restriction | `OnlyTrade = 1` marks a session as non-primary (same MAC, different account) and blocks combat, party, whisper, action | `ProcessDBMessage.cpp:148-182`; `_MSG_Attack.cpp:47-51` |
| BR-54 | Moderation mute | `MuteChat == 1` blocks local, whisper, kingdom and citizen chat | `_MSG_MessageChat.cpp:181-185`; `_MSG_MessageWhisper.cpp:100, 1129` |
| BR-55 | Channel opt-outs | `Whisper/Guildchat/PartyChat/KingChat` act as "off" switches; `Chatting` is write-only | `_MSG_MessageChat.cpp:111-169`; `SendFunc.cpp:278, 1016, 1042` |
| BR-56 | Deferred vitals | `ReqHp`/`ReqMp` are target values; `ApplyHp/ApplyMp` converge toward them in steps of at most 2,000, clamped to `MaxHp/MaxMp` and 2e9 | `Server.cpp:8551-8687`; `ProcessSecMinTimer.cpp:601-619` |
| BR-57 | Attack range | `Range` is a per-session copy of the mob's range, used as `dis > pUser[conn].Range \|\| dis > 23` | `CMob.cpp:657-663`; `_MSG_Attack.cpp:426` |
| BR-58 | Castle siege | `CastleStatus` counts seconds of valid stand on the castle pad; `> 22` transfers the castle | `Server.cpp:4362-4412` |
| BR-59 | Long-period counter | `Unk_2736` increments per regen pass and every 450 counts applies PK-point and mount incubation/feeding | `Server.cpp:4415-4480` |
| BR-60 | Cash ledger | `Donate` is loaded from DB at character login, incremented by web donations, and written on save | `ProcessDBMessage.cpp:728, 1217-1251`; `Server.cpp:6909, 7073` |
| BR-61 | Billing linkage flag | `IsBillConnect` is tested at close/logout but never set to 1 in TMSrv (`SendBilling` is a stub returning TRUE) | `CUser.cpp:29, 85`; `Server.cpp:1377-1380, 6845, 7101` |
| BR-62 | Origin address | `IP` stores the raw `sin_addr.S_un.S_addr` (network byte order) and is used only for log/ban bookkeeping | `CUser.cpp:73-74`; `Server.cpp:6701-6727` |

### 3.2 Mode state machine (referenced by BR-05)

Legal transitions observed in code (source of each transition in parentheses):

| From | To | Trigger | Evidence |
|------|----|---------|----------|
| — | `USER_EMPTY` (0) | static construction of all 1000 slots | `CUser.cpp:27` |
| `USER_EMPTY` | `USER_ACCEPT` (1) | successful `accept()` + `WSAAsyncSelect` | `CUser.cpp:75`, `Server.cpp:4020` |
| `USER_ACCEPT` | `USER_EMPTY` | reserved slot, shutdown, or accepted-then-rejected | `Server.cpp:4027, 4036` |
| `USER_ACCEPT` | `USER_LOGIN` (2) | valid account-login request | `_MSG_AccountLogin.cpp:94` |
| `USER_LOGIN` | `USER_SELCHAR` (11) | `_MSG_DBCNFAccountLogin` | `ProcessDBMessage.cpp:400` |
| `USER_LOGIN` | `USER_ACCEPT` then `USER_EMPTY` | `_MSG_DBCNFAccountLogOut` (assign then immediate close) | `ProcessDBMessage.cpp:1180-1182` |
| `USER_SELCHAR` | `USER_CHARWAIT` (12) | `_MSG_CharacterLogin` accepted | `_MSG_CharacterLogin.cpp:76` |
| `USER_SELCHAR` | `USER_WAITDB` (13) | create/delete character request | `_MSG_CreateCharacter.cpp:41`, `_MSG_DeleteCharacter.cpp:31` |
| `USER_WAITDB` | `USER_SELCHAR` | any DB create/delete result | `ProcessDBMessage.cpp:588, 604, 617, 630` |
| `USER_WAITDB` | `USER_SELCHAR` | `_MSG_ReqTransper` (server transfer) | `ProcessDBMessage.cpp:224` |
| `USER_CHARWAIT` | `USER_PLAY` (22) | `_MSG_DBCNFCharacterLogin` | `ProcessDBMessage.cpp:906` |
| `USER_CHARWAIT` | `USER_SELCHAR` | `_MSG_DBCharacterLoginFail` | `ProcessDBMessage.cpp:1092` |
| `USER_PLAY` | `USER_SELCHAR` | `CharLogOut` (client logout, arch/bless flow, GM save-all) | `Server.cpp:7113` |
| `USER_PLAY` | `USER_SAVING4QUIT` (24) | socket closed while playing | `Server.cpp:6917` |
| `USER_PLAY`/`USER_SAVING4QUIT` | `USER_EMPTY` | save acknowledged / close | `ProcessDBMessage.cpp:1168`, `Server.cpp:6925` |
| `USER_SELCHAR`/`USER_WAITDB`/`USER_CHARWAIT` | `USER_EMPTY` | `CloseUser` non-playing branch (DB `_MSG_DBNoNeedSave` sent) | `Server.cpp:6852-6867` |

Dead / unused states: `USER_CREWAIT` (14) is commented out (`CUser.h:33`); `USER_WAITDB` has no timeout of its own and relies on the generic 720 s idle rule.

### 3.3 Detailed breakdown of the business rules

---

### Business Rule: BR-01 Session slot identity and allocation

**Overview**:
The session object is addressed by its array index; the index *is* the connection ID and is shared with `pMob[]`.

**Detailed description**:
TMSrv never stores a session pointer anywhere. A socket handle is mapped back to an index by linearly scanning `pUser[1..999]` for a matching `cSock.Sock` (`Code/TMSrv/Server.cpp:6729-6741`), and a free slot is found by scanning for `Mode == USER_EMPTY` (`:6743-6752`). Because `pMob[conn]` is the avatar for the same index, the invariant "session index == avatar index" is assumed everywhere; `DeleteMob`, `SendGridMob`, `GetGuild`, `MobKilled` and the party system all rely on it. Slot 0 is deliberately excluded from both scans even though `CUser pUser[1000]` exists, so index 0 acts as a sentinel "no user" value returned by lookups (`GetUserByName` returns 0, `GetUserFromSocket` returns 0).

The consequence of this design is that a session's identity is mutable state, not an immutable object reference: while `Mode == USER_EMPTY`, the same index can be handed to a completely different TCP client. Any code that caches an index across a `CloseUser` boundary is looking at a different person afterwards. No generation counter or "session epoch" exists to detect this, and there is no check that `pMob[i].Mode` matches `pUser[i].Mode` before using cached indices.

Slot reuse is first-fit, lowest index first, which means the server tends to reuse low indices and never compacts or rebalances. This makes the lowest indices the busiest and explains why many loops start at 1 and why `GetEmptyUser()` returns 0 on exhaustion (which the accept handler treats as "accept fail - no empty" and simply logs).

**Rule workflow**:
`WSA_ACCEPT -> GetEmptyUser() -> pUser[User].AcceptUser() -> ... -> CloseUser(User) -> Mode=USER_EMPTY -> slot becomes reusable by GetEmptyUser()`.

---

### Business Rule: BR-02 Reserved slots for administrators

**Overview**:
The last 10 slots of the 1000-slot table are reserved for administrative/billing connections and are refused to normal clients.

**Detailed description**:
`MAX_USER` is 1000 and `ADMIN_RESERV` is 10 (`Code/Basedef.h:56-57`), so indices 990-999 form the reserved band. The check is applied *after* `AcceptUser` has already accepted the socket and installed the async handler (`Code/TMSrv/Server.cpp:4020-4030`), which means the TCP handshake completes and the client is told to reconnect (`_NN_Reconnect`) before being disconnected. The account-login handler repeats the same bound (`Code/TMSrv/_MSG_AccountLogin.cpp:30-39`), so a client that somehow reached `USER_ACCEPT` on a reserved slot is dropped there too.

No code path ever assigns a normal login to the reserved band: `GetEmptyUser` scans from 1 upward and would only return 990+ after 989 live sessions. The intent is clearly to keep headroom for administrative tooling, and the same band is excluded from account login rather than being validated as "admin only". There is no check of `pAdminIP[]` at this point — the IP allow-list is read (`Code/TMSrv/CReadFiles.cpp:640-676`) and only written from a GM command (`Code/TMSrv/imple.cpp:1668-1700`).

Because the rejection happens after accept, the reserved band still consumes one `pUser` entry momentarily; the entry is returned to `EMPTY` by `CloseUser`, so no permanent leak occurs.

**Rule workflow**:
`accept success -> GetEmptyUser() -> User >= MAX_USER - ADMIN_RESERV ? -> send _NN_Reconnect + CloseUser -> return TRUE (no session)`.

---

### Business Rule: BR-03 Shutdown admission gate

**Overview**:
While the server is shutting down, no new session may become established.

**Detailed description**:
`ServerDown` is a global phase counter. The value `-1000` means "running normally"; any other value means a shutdown is being sequenced (`Code/TMSrv/ProcessSecMinTimer.cpp:42-119`). The accept path tests `ServerDown != -1000` and immediately answers with `_NN_ServerReboot_Cant_Connect` and closes the slot (`Code/TMSrv/Server.cpp:4032-4039`). The dispatcher separately drops *all* packets once `ServerDown >= 120` (`Code/TMSrv/ProcessClientMessage.cpp:53-54`), which is the point where the one-slot-per-second drain begins.

The two gates serve different purposes: the accept gate prevents new sessions from appearing during the announcement window, while the dispatch gate freezes existing sessions so their state stops changing before the final save. Note that the accept gate is evaluated *after* `AcceptUser`, so the socket is still fully initialised and then torn down rather than never created.

Because `CloseUser` is used, the shutdown rejection also runs the `Mode`-based branch; since the session is `USER_ACCEPT` it takes the plain-close path and no DB message is emitted.

**Rule workflow**:
`WSA_ACCEPT -> AcceptUser -> ServerDown != -1000 ? -> message + CloseUser (plain close) -> slot EMPTY`.

---

### Business Rule: BR-04 Accept initialisation contract

**Overview**:
`AcceptUser` is the only place that binds an OS socket to a session record, and it initialises exactly the transport fields plus the origin address and the initial state.

**Detailed description**:
`AcceptUser(int ListenSocket)` (`Code/TMSrv/CUser.cpp:51-79`) calls `accept()`, then `WSAAsyncSelect(tSock, hWndMain, WSA_READ, FD_READ|FD_CLOSE)`, then stores `cSock.Sock` and resets the three buffer cursors (`nRecvPosition`, `nProcPosition`, `nSendPosition`). It records `IP = acc_sin.sin_addr.S_un.S_addr` in network byte order and sets `Mode = USER_ACCEPT`, `Unk3 = 0`. Every failure path (`tSock < 0`, `WSAAsyncSelect(...) > 0`) returns `FALSE` *without* touching `Mode`, so a failed accept leaves the slot `USER_EMPTY` and therefore reusable — a deliberate and correct choice for slot hygiene. Note that `WSAAsyncSelect` returns `SOCKET_ERROR` (-1) on failure and `0` on success, so the `> 0` test is technically the wrong polarity check (see section 11).

The function does **not** clear buffers; `pSendBuffer`/`pRecvBuffer` are owned by `CPSock` and are allocated in its constructor (`Code/CPSock.cpp`). Since `pUser` is a global array, all 1000 `CPSock` members are constructed at process start, so no per-session allocation occurs here. This makes accept cheap but means buffer memory is permanently resident (2 × 128 KiB per slot at the `CPSock` level → the dominant static memory cost of the server).

The unused `char *tmp = inet_ntoa(...)` on line 73 shows a leftover conversion that is discarded; `IP` ends up in network byte order while the log formatter later reinterprets the same integer byte-wise (`Code/TMSrv/Server.cpp:6710-6716`), which is why logs print the correct dotted address. The caller ignores the boolean return value entirely (`Server.cpp:4020`), so a failed `WSAAsyncSelect` is silent apart from a closed socket.

**Rule workflow**:
`accept() -> (fail) return FALSE, mode unchanged | (ok) WSAAsyncSelect -> (fail) closesocket, return FALSE | (ok) set Sock/positions/IP -> Mode=USER_ACCEPT -> return TRUE`.

---

### Business Rule: BR-05 Mode state machine legality

**Overview**:
`Mode` is the authoritative session phase; every handler gates on an exact expected value and every transition is an ad-hoc assignment.

**Detailed description**:
The values are grouping macros rather than an enum: connection modes 0/1/2, selection-screen modes 11/12/13, in-game modes 22/24 (`Code/TMSrv/CUser.h:25-37`). The gaps (3-10, 15-21, 23) are leftovers from the original binary and are not used, which means an out-of-range `Mode` cannot be distinguished from a valid one by arithmetic — the code therefore compares with `==`, `< USER_SELCHAR`, or truthiness rather than relying on ordering. `Mode < USER_SELCHAR` is used as "not yet at the character screen" in three places (`ProcessDBMessage.cpp:233, 1221`; `imple.cpp:1538, 1554`), which works only because all selection values are >= 11.

There are exactly fifteen transition write sites, spread over `CUser.cpp`, `Server.cpp`, `ProcessDBMessage.cpp`, `_MSG_AccountLogin.cpp`, `_MSG_CharacterLogin.cpp`, `_MSG_CreateCharacter.cpp` and `_MSG_DeleteCharacter.cpp`. No mutex, guard function or transition table enforces legality; legality is enforced by the precondition each handler performs *before* writing. This is workable because the server is single-threaded (Win32 message pump), so no interleaving can occur between the check and the assignment.

The most error-prone transition is `USER_LOGIN -> USER_ACCEPT -> USER_EMPTY` in `_MSG_DBCNFAccountLogOut` (`ProcessDBMessage.cpp:1179-1182`): `Mode` is set to `USER_ACCEPT` and then `CloseUser` immediately sets it to `USER_EMPTY`, so the intermediate value is unobservable. The intent was presumably to return the socket to a re-logon-capable state, but `CloseUser` destroys the socket, so the effect is a plain disconnect.

**Rule workflow**:
`handler precondition (Mode == expected) -> state-specific work -> Mode := next` and teardown via `CloseUser` which always forces `USER_EMPTY`.

---

### Business Rule: BR-06 Dispatcher admission control

**Overview**:
Every inbound packet must carry a session index in range, and the server must be in an accepting phase.

**Detailed description**:
`ProcessClientMessage` first casts the raw buffer to `MSG_STANDARD` (`HEADER`: `Size, KeyWord, CheckSum, Type, ID, ClientTick`; `Code/CPSock.h:42-50`) and validates `0 <= std->ID < MAX_USER`, logging and returning on violation (`Code/TMSrv/ProcessClientMessage.cpp:40-51`). It then refuses everything while `ServerDown >= 120` (`:53-54`). The `conn` argument, not `std->ID`, is used for all `pUser[...]` access, so a spoofed `ID` cannot by itself redirect state changes — but many handlers echo `m->ID = conn` back into the packet, so the field is normalised quickly.

The dispatcher is a flat `switch` over `std->Type` with 50+ cases, no default branch, and no size validation per type. It relies on each handler to validate `m->Size` (only `_MSG_AccountLogin` does so explicitly). A packet with a valid type but a short body therefore reaches struct casts whose fields may be uninitialised stack/read past the received bytes; `CPSock::ReadMessage` bounds the framing but not the per-type body contract.

Two early exits are special: `_MSG_Ping` is answered by doing nothing (`:59-60`), and client packets with `ClientTick == SKIPCHECKTICK` are dropped (`:63-64`) before the `switch`, which blocks the sentinel that legitimate internal server-originated packets would use.

**Rule workflow**:
`ReadMessage -> std->ID range check -> ServerDown check -> LastReceiveTime := SecCounter -> ping? -> ClientTick sentinel? -> switch(Type) -> Exec_*(conn, msg)`.

---

### Business Rule: BR-07 Idle timeout and liveness clock

**Overview**:
A session with no inbound traffic for 720 seconds is closed; the clock is refreshed by any packet and self-heals against clock skew.

**Detailed description**:
`ProcessClientMessage` sets `pUser[conn].LastReceiveTime = SecCounter` on every accepted call, before the ping/tick filters (`Code/TMSrv/ProcessClientMessage.cpp:56-57`). `SecCounter` is a global incremented once per second by `ProcessSecTimer`, so the unit is seconds of server uptime rather than wall-clock time, which immunises the rule against system clock changes. `CheckIdle` (`Code/TMSrv/Server.cpp:4304-4322`) closes the session when `lst < ser - 720` (12 minutes) and re-seeds the timestamp when `lst > ser` (counter wrapped/restarted) or when `lst < ser - 1440` (the session was somehow frozen for more than 24 minutes, treated as a measurement artefact rather than an offence).

The check is distributed across slots: `ProcessSecMinTimer` only calls it for slot `i` when `i % 32 == SecCounter % 32` (`Code/TMSrv/ProcessSecMinTimer.cpp:1179-1182`), giving each live slot a check every 32 seconds. That granularity means the effective timeout is 720-752 seconds. Sessions in `USER_SAVING4QUIT` are excluded, because a slow database acknowledgement must not race the idle timer and re-trigger a save.

Because the timestamp is refreshed before the `SKIPCHECKTICK` rejection, a client spamming rejected packets keeps its session alive even though no gameplay packet is processed. The rule is liveness-only, not activity-only.

**Rule workflow**:
`per-second: some slot i -> CheckIdle(i) -> skew fixes -> lst < ser-720 ? -> log + CloseUser(i) -> (save path if PLAY)`.

---

### Business Rule: BR-08 Socket-close dispatch by mode

**Overview**:
`CloseUser(conn)` decides how much state must be persisted based purely on `Mode`, and is the single choke point for teardown.

**Detailed description**:
The global `CloseUser(int conn)` (`Code/TMSrv/Server.cpp:6834-6926`) begins by clearing the grid cell when the session was playing and its coordinates are in `[0,4096)`, resets `Admin = 0`, sends a billing release if `IsBillConnect`, and closes the socket. It then branches on the captured `Mode`. For `Mode == 0` or `Mode == USER_ACCEPT` it takes the trivial path (`pUser[conn].CloseUser()`); for any other mode that is neither `USER_PLAY` nor `USER_SAVING4QUIT` it sends `_MSG_DBNoNeedSave` to DBSrv, forces `pMob[conn].Mode = USER_EMPTY` and then closes; for `USER_PLAY`/`USER_SAVING4QUIT` it removes the party and trade counterpart, and emits `MSG_SavingQuit` with the full persistence payload before setting `Mode = USER_SAVING4QUIT` and `DeleteMob(conn, 2)`.

The playing branch validates two invariants before saving: `0 <= conn <= MAX_MOB` and `0 <= Slot < MAX_CLASS`, and on violation writes a crack log and returns **without** closing the session or resetting `Mode`. This is an early-return leak: the slot remains in `USER_SAVING4QUIT`/whatever mode with a closed socket, which the idle timer will eventually reap (and `USER_SAVING4QUIT` is exempt from idle checks, so such a slot can permanently occupy a session and keep `pMob` state).

Because `USER_SAVING4QUIT` is re-entered through the same playing branch, calling `CloseUser` twice on a saving session re-sends the save payload rather than short-circuiting. `_MSG_DBSavingQuit` then calls `CloseUser(m->ID)` (`ProcessDBMessage.cpp:1168`), which re-enters this function in `USER_SAVING4QUIT` mode; the second save is harmless only because DBSrv receives a redundant, identical `MSG_SavingQuit`.

**Rule workflow**:
`CloseUser(conn) -> grid clear + Admin=0 + billing + socket close -> Mode==0/ACCEPT ? plain : (PLAY/SAVING ? save-quit : DBNoNeedSave) -> Mode=USER_EMPTY + AccountName[0]=0`.

---

### Business Rule: BR-09 Account name normalisation and brute-force throttle

**Overview**:
Account names are canonicalised to upper case, length-limited, and logins are blocked after three recorded failures.

**Detailed description**:
`Exec_MSG_AccountLogin` first writes `pUser[conn].AccountName[NAME_LENGTH - 1] = 0` before doing anything else (`Code/TMSrv/_MSG_AccountLogin.cpp:28`), guaranteeing termination even if the previous session left a full 16-byte name. It then tokenises with `sscanf(m->AccountName, "%s", ...)`, applies `_strupr`, and copies the canonical form back into the outgoing packet with `strncpy(..., NAME_LENGTH)` (`:76-80`). Upper casing in the server is what allows `GetUserByName("+name")` to compare `_strupr`'d input against `AccountName` (`Code/TMSrv/Server.cpp:6793-6813`).

Before forwarding, `CheckFailAccount(m->AccountName)` is consulted; a value of 3 or more short-circuits with `_NN_3_Tims_Wrong_Pass` and leaves `Mode` at `USER_ACCEPT` so the client may retry (`:82-90`). The failure table itself is a 16×16 global (`Code/TMSrv/Server.h:315`) populated by `AddFailAccount`, and it is not cleared by this component — the throttle therefore survives session churn for the same account name.

Because the canonical name is what is stored and sent to DBSrv, and because `_MSG_DBCNFAccountLogin` later verifies the echo with `strcmp` (BR-15), any case or truncation mismatch makes the login fail closed (session closed) rather than continuing with a partially initialised record.

**Rule workflow**:
`AccountName[15]=0 -> tokenise -> strupr -> write back -> CheckFailAccount >= 3 ? reject : send _MSG_DBAccountLogin -> Mode=USER_LOGIN, pMob.Mode=MOB_EMPTY`.

---

### Business Rule: BR-10 Protocol version and size gate

**Overview**:
Account login requires both the exact build version and a body at least as large as the login struct.

**Detailed description**:
The handler compares `m->Size` against `sizeof(MSG_AccountLogin)` and, outside `_PACKET_DEBUG` builds, also requires `m->ClientVersion == APP_VERSION` (`Code/TMSrv/_MSG_AccountLogin.cpp:41-54`). Failure produces a formatted `_NN_Version_Not_Match_Rerun` message containing `APP_VERSION`, flushes the send buffer, and closes the session. This is the only per-message size validation in the entire dispatcher.

The gate is intentionally fail-closed: the session is destroyed rather than left in `USER_ACCEPT`, so a mismatched client must reconnect from scratch. In `_PACKET_DEBUG` builds the version check is compiled out, which means a debug build accepts any client version as long as the body is large enough — a configuration-dependent behaviour worth noting for analysis of test/debug deployments.

Because `m->Size` comes from the client header, it is attacker-controlled; the comparison protects against short bodies for this one message but does not constrain the declared size against the actual received length (framing is handled by `CPSock::ReadMessage`).

**Rule workflow**:
`Size < sizeof(MSG_AccountLogin) || ClientVersion != APP_VERSION ? -> message + SendMessageA + CloseUser : continue`.

---

### Business Rule: BR-11 MAC fingerprint capture and block list

**Overview**:
The client-reported adapter name (4×int) is captured on login, stored in the session, and compared against a ban list and a duplicate-account list.

**Detailed description**:
If the login packet is shorter than the full struct, `pUser[conn].Mac` is filled with `0xFF` bytes; otherwise the adapter name is copied (`Code/TMSrv/_MSG_AccountLogin.cpp:65-70`). The `0xFF` sentinel marks "unknown MAC" and deliberately does not equal any parsed ban entry (ban entries come from a text file of four decimal octets). At account confirmation, the server scans `pMac[0..199]` and closes the session if all four components match (`Code/TMSrv/ProcessDBMessage.cpp:369-383`). `pMac` is reloaded from `../../DBSRV/Run/Mac.txt` every 120 seconds (`Code/TMSrv/CReadFiles.cpp:678-715`; reload gate `ProcessSecMinTimer.cpp:127-128`), so a ban added while the client is connected takes effect on the next login attempt, not immediately.

The same `Mac` value is also used as the "primary account" discriminator: DBSrv sends `_MSG_DBCheckPrimaryAccount` with a MAC and account name, and the server walks all live sessions comparing MACs, setting `OnlyTrade = 0` for the matching account and `1` for every other account on that MAC (`ProcessDBMessage.cpp:148-182`). This is what implements "one primary character per machine".

`Mac` is never cleared by `CloseUser`, so a stale value persists in the slot until the next login overwrites it. All lookups are guarded by `Mode == USER_EMPTY || cSock.Sock == 0 -> continue`, which prevents matching an empty slot's leftover MAC.

**Rule workflow**:
`login -> Mac = packet.AdapterName or 0xFF x4 -> DB CNF -> scan pMac[] -> match ? message + CloseUser : continue -> compare with pUser[i].Mac for OnlyTrade`.

---

### Business Rule: BR-12 Session/DB identity verification

**Overview**:
The account confirmation from DBSrv must name exactly the account the session believes it is.

**Detailed description**:
`_MSG_DBCNFAccountLogin` compares `m->AccountName` with `pUser[conn].AccountName` using `strcmp` and, on mismatch, sends `_NN_Try_Reconnect`, flushes and closes (`Code/TMSrv/ProcessDBMessage.cpp:358-367`). This is a defence against DB/session desynchronisation and against an attacker who can inject DB-server messages: the session cannot be re-pointed at a different account because the name already stored locally is authoritative.

The check is also the last point at which the session can be safely destroyed, because nothing user-visible has been sent yet except possibly the login acknowledgement. After the check the handler performs an irreversible sequence: it rewrites `Cargo`, `Coin`, `SelChar`, sends the confirmation packet to the client, and sets `Mode = USER_SELCHAR`.

The comparison is case-sensitive and length-bounded by `strcmp` on two NUL-terminated 16-byte buffers; because the login handler up-cased the local copy and DBSrv is expected to echo that same buffer, any DB normalisation difference breaks login. The asymmetric risk is intentional: a false negative costs a reconnect, a false positive would attach the wrong inventory.

**Rule workflow**:
`DB CNF -> strcmp(AccountName) -> mismatch ? message + SendMessageA + CloseUser : MAC/flag/Cargo/Coin/SelChar init -> Mode=USER_SELCHAR`.

---

### Business Rule: BR-13 Volatile flag reset at account confirmation

**Overview**:
Nine session flags are unconditionally reset when the account is confirmed, and the primary-account restriction is armed.

**Detailed description**:
`ProcessDBMessage.cpp:386-395` resets `IsBillConnect`, `Whisper`, `Guildchat`, `PartyChat`, `KingChat`, `Admin`, `Chatting`, `Unk_2732`, `Unk_2728` and sets `OnlyTrade = 1`. The chat flags are "off switches" (1 = suppressed), so resetting to 0 re-enables all channels for the new session. `OnlyTrade = 1` initially forbids everything except trade; it is relaxed to 0 only when DBSrv confirms this account is the primary one for its MAC (BR-11). This ordering is deliberate: the server starts restrictive and relaxes on evidence.

`Admin` is reset here as well, which means any administrative elevation cannot survive a re-login — but since `Admin` is never read (see BR-61/technical debt), the reset has no observable effect. `Unk_2728` is a billing "child account" marker consulted in the character-login gate and the save loop; `Unk_2732` is a "billing check sent at" timestamp compared against `SecCounter - 10`.

Because these resets live in the account-confirmation handler rather than in `CloseUser`, a session that never reaches confirmation keeps whatever flags it had. In practice such a session is `USER_ACCEPT` or `USER_LOGIN` and cannot act, but the invariant "flags are clean before use" is enforced at use-time, not at allocation-time.

**Rule workflow**:
`DB CNF (name verified) -> reset 9 flags, OnlyTrade=1 -> Mode=USER_SELCHAR -> (later) DBCheckPrimaryAccount may set OnlyTrade=0`.

---

### Business Rule: BR-14 Character slot bounds and selection state

**Overview**:
A character selection is only valid for slots 0..3, and creation/deletion are only valid from `USER_SELCHAR`.

**Detailed description**:
`_MSG_CharacterLogin` rejects `m->Slot < 0 || m->Slot >= MOB_PER_ACCOUNT` with `_NN_SelectCharacter` (`Code/TMSrv/_MSG_CharacterLogin.cpp:25-29`), where `MOB_PER_ACCOUNT` is 4 and matches `STRUCT_SELCHAR::Score[4]`, `Name[4][16]`, `Equip[4][16]`, `Guild[4]`, `Coin[4]`, `Exp[4]` (`Code/Basedef.h:765-778`). The chosen slot is persisted in `pUser[conn].Slot` only after a successful character login (`ProcessDBMessage.cpp:821`), so during selection the field still holds the previous session's value — which is why the logout path re-validates `Slot` before saving (BR-24).

Character creation and deletion both require `Mode == USER_SELCHAR`, then move the session to `USER_WAITDB` and forward to DBSrv (`_MSG_CreateCharacter.cpp:27-47`, `_MSG_DeleteCharacter.cpp:26-37`). Creation additionally validates the name with `BASE_CheckValidString` before consuming the state; deletion does not validate anything beyond the mode, so a malformed delete request still burns a DB round-trip. Every DB outcome — success, failure, delete-failure — returns the session to `USER_SELCHAR`, so `USER_WAITDB` is a strictly transient state.

`USER_WAITDB` has no dedicated timeout: a lost DB reply strands the session until the 720-second idle rule fires, during which the client receives no feedback. Because `USER_WAITDB` is "not yet at the character screen" (`Mode < USER_SELCHAR` is false for 13), the login-time checks that use that predicate treat it as an established session.

**Rule workflow**:
`SELCHAR -> (create|delete) -> WAITDB -> DB result (any) -> SELCHAR`; `SELCHAR -> CharacterLogin(slot in [0,4)) -> CHARWAIT`.

---

### Business Rule: BR-15 Billing and level gate for character login

**Overview**:
Entering the world is conditional on billing mode, level versus `FREEEXP`, account flags, and the daily hour window.

**Detailed description**:
The gate is a nested decision tree (`Code/TMSrv/_MSG_CharacterLogin.cpp:31-107`). The first branch applies when `BILLING == 2`, the chosen slot exists in `SelChar`, its level is `>= FREEEXP` and `< 999`; inside it, `Unk_1816` distinguishes "checking" (<=1), "not allowed" (3) and "other server group" (4). `Unk_1816 == 3` produces a formatted `_DN_Not_Allowed_Account` message with `FREEEXP`; `Unk_1816 == 4` produces `_NN_Using_Other_Server_Group`; `Unk_1816 <= 1` produces `_NN_Wait_Checking_Billing` and a `SendBilling(..., 1, 1)` request.

The final admission test is `BILLING != 2 || pUser[conn].Unk_2728 != 1 || level < FREEEXP || (g_Hour > 7 && g_Hour < 23)`. In words: the session may proceed unless it is a child/limited account (`Unk_2728 == 1`) at or above the free-experience threshold outside the permitted play window — the "child pay" restriction. When the test fails the client receives `_NN_Child_Pay`. The sibling branch for `Unk_1816 <= 1` also implements a 10-second grace on the billing timestamp: if `Unk_2732` is set and older than `SecCounter - 10`, the check is considered complete and `Unk_1816 = 5`; otherwise the billing server is asked again.

Because `SendBilling` is currently a stub that returns `TRUE` without contacting anything (`Code/TMSrv/Server.cpp:1377-1380`), all billing-driven branches are effectively inert in this build: `Unk_1816` is only ever set to 0 at account confirmation and 5 in the branch above, and `IsBillConnect` is never set. The gate therefore behaves as "Billing != 2 → allow", i.e. permissive.

**Rule workflow**:
`CharacterLogin -> slot bounds -> (BILLING==2 path: Unk_1816 / Unk_2728 / level / g_Hour) -> allowed ? Mode=CHARWAIT + _MSG_DBCharacterLogin : message only`.

---

### Business Rule: BR-16 Full session initialisation on entering the world

**Overview**:
`USER_PLAY` may only be entered after a fixed reset of every volatile, transactional and anti-cheat field in `CUser`.

**Detailed description**:
The character-confirmation handler writes `Donate`, copies `MOB`, `CharShortSkill`, `affect`, `extra`, then performs the session reset block (`Code/TMSrv/ProcessDBMessage.cpp:728-751, 819-846, 979-982`). The reset covers `Unk_2708 = 0`, `LastChat[0] = 0`, `Slot = m->Slot`, `NumError = 0`, `LastMove = 0`, `LastAction = _MSG_Action`, `LastActionTick = SKIPCHECKTICK`, `LastAttack = 0`, `LastAttackTick = SKIPCHECKTICK`, `LastIllusionTick = SKIPCHECKTICK`, `RankingTarget = 0`, `RankingType = 0`, `CastleStatus = 0`, `Unk9` all `0xFF`, `Trade` zeroed with `InvenPos[0..14] = -1`, `AutoTrade` with `CarryPos[0..11] = -1`, `TradeMode = 0`, `PKMode = 0`, `cProgress = 0`, `ReqHp = CurrentScore.Hp`, `ReqMp = CurrentScore.Mp`, `Unk_2688 = 0`, `Unk_2736 = 0`, `Message = 0`, `UseItemTime = 0`, `PotionTime = 0`.

The `SKIPCHECKTICK` sentinel is essential: it tells the attack/action validators "this session has no valid client tick baseline yet, do not apply the delta checks". Writing `0` instead would make the first real packet appear to violate the 800 ms rule. Similarly, the `-1` sentinels in `InvenPos`/`CarryPos` mean "no item in this trade slot", which the trade validator treats as empty (`iPos == -1 -> memset(item)`).

This block is the *de facto* constructor for gameplay state: the real constructor (`Code/TMSrv/CUser.cpp:25-44`) initialises only `Mode`, `Unk3`, `IsBillConnect`, `LastReceiveTime`, `Cargo`, `Admin`, `CastleStatus`, `MuteChat`, `UseItemTime`, `Message`, `AttackTime`, `LastClientTick`, `PotionTime`, `OnlyTrade`. Everything else relies on static zero-initialisation of the global array plus this block.

**Rule workflow**:
`DB CNFCharacterLogin -> mob/extra/affect/shortskill load -> sentinel + counter reset -> spawn resolution -> Mode=USER_PLAY -> projections`.

---

### Business Rule: BR-17 Login validation and repair

**Overview**:
Two hard validations (class range, free spawn cell) close the session, while a dead character is silently revived with 2 HP.

**Detailed description**:
After the mob is copied, `pMob[conn].MaxCarry` is derived from `Carry[60]`/`Carry[61] == 3467` (30, optionally +15 each), the mob's current score is recalculated, guild info is attached, and `pMob[conn].Mode = MOB_USER` (`Code/TMSrv/ProcessDBMessage.cpp:751-782`). Then the spawn position is chosen: the city of the merchant flag (`(Merchant & 0xC0) >> 6`), a small random jitter, an override for the guild that currently charges the zone, and a fixed newbie square for `MORTAL` characters below `FREEEXP` (`:848-885`).

If `MOB.Class` is outside `[0, MAX_CLASS)` the session is closed and the handler breaks (`:859-866`); if `GetEmptyMobGrid` cannot find a free cell the session is also closed (`:887-896`). Both are fail-closed because the alternative — entering `USER_PLAY` at an invalid location — would corrupt the grid. Note that these `break` statements exit the `switch` case only, so the surrounding dispatcher continues normally; the session is already closed at that point.

The repair rule is `if (m->mob.CurrentScore.Hp <= 0) m->mob.CurrentScore.Hp = 2` (`:730-731`), applied before any score computation. It converts a corrupted "dead on login" record into a minimal live state so that the client is not stuck in a death loop it cannot resolve.

**Rule workflow**:
`mob copy -> MaxCarry -> scores -> spawn selection -> Class out of range ? CloseUser : GetEmptyMobGrid fail ? CloseUser : grid write -> Mode=USER_PLAY`.

---

### Business Rule: BR-18 Character logout

**Overview**:
Logout requires a live playing session; it settles trades, saves, removes the avatar, and returns the session to character selection.

**Detailed description**:
`CharLogOut(int conn)` validates the index, then requires `Mode == USER_PLAY`; otherwise it logs `err,char logout - not user_play` and still emits `_MSG_CNFCharacterLogout` without changing state (`Code/TMSrv/Server.cpp:7085-7099`). This asymmetry is intentional: the client must always receive an answer, but the server refuses to persist a session that was not in the world.

For a valid session it optionally releases billing, breaks a mutual trade by inspecting the opponent's `Trade.OpponentID` (symmetric check), sets `Mode = USER_SELCHAR`, refreshes `SelChar.Score[Slot].Level` from the live mob so the selection screen shows the current level, calls `SaveUser(conn, 1)`, `DeleteMob(conn, 2)`, restores `pMob[conn].TargetX/Y` to the saved spawn point, sets `pMob[conn].Mode = 0`, and finally signals the client. The order matters: `SaveUser` must run before `DeleteMob` because `DeleteMob` zeroes HP and the grid cell.

`SaveAll()` (`Code/TMSrv/imple.cpp:33-44`) simply iterates all slots and calls `CharLogOut` on every `USER_PLAY` session, so a server-side "save all" is really a mass logout to the character screen. The same function is the anti-cheat fallback: `AddCrackError` calls `CharLogOut` when the score saturates.

**Rule workflow**:
`index check -> Mode != PLAY ? signal only : billing -> trade break -> Mode=SELCHAR -> SelChar level sync -> SaveUser -> DeleteMob -> mob pos reset -> signal`.

---

### Business Rule: BR-19 Persistence payload and save-quit protocol

**Overview**:
Every save transmits the avatar plus the session-owned payload, and a socket close during play becomes an asynchronous save-then-quit handshake.

**Detailed description**:
`SaveUser(conn, Export)` builds `MSG_DBSaveMob` with `MOB`, `Cargo[128]`, `Coin`, `ID`, `Export`, `Slot`, `Donate`, `AccountName`, `ShortSkill[16]`, `affect`, `extra` (`Code/TMSrv/Server.cpp:7056-7083`). `Export` distinguishes a routine/periodic save (0) from a forced export (1) used at logout, arch upgrade and capsule completion. The payload is the complete list of fields that live in `CUser` rather than `CMob`: cargo, warehouse coin, donation balance, slot and the 16-byte shortcut bar.

The socket-close path builds the same payload into `MSG_SavingQuit` and sets `Mode = USER_SAVING4QUIT` (`:6880-6919`). It does **not** wait synchronously; the session stays allocated and `DeleteMob(conn, 2)` removes the avatar immediately from the world so other players stop seeing it. DBSrv then answers `_MSG_DBSavingQuit`, and the handler validates the ID range, sends `_MSG_DBNoNeedSave` if the session is no longer playing/saving, emits `_NN_Your_Account_From_Others` or `_NN_Disabled_Account` depending on `m->Mode`, flushes, and calls `CloseUser(m->ID)` (`ProcessDBMessage.cpp:1137-1169`).

Because `USER_SAVING4QUIT` is excluded from the idle sweep, an unanswered `_MSG_SavingQuit` leaves the slot permanently occupied. The only recovery is a later matching DB message or a server restart; there is no save timeout or retry.

**Rule workflow**:
`CloseUser(PLAY) -> payload -> send _MSG_SavingQuit -> Mode=SAVING4QUIT -> DeleteMob -> DB ack -> CloseUser -> Mode=EMPTY`.

---

### Business Rule: BR-20 Close reset scope and its invariant dependency

**Overview**:
The member `CloseUser()` deliberately leaves most fields intact; correctness depends on the next login rewriting them.

**Detailed description**:
`CUser::CloseUser()` (`Code/TMSrv/CUser.cpp:81-92`) performs only: `cSock.CloseSocket()`, `cSock.Sock = 0`, `IsBillConnect = 0`, `Mode = USER_EMPTY`, `AccountName[0] = 0`. It returns `TRUE` unconditionally and cannot fail. The socket close destroys the buffers' contents via `CPSock::CloseSocket`, but the buffer pointers themselves are re-created only at process start.

Fields that survive a close: `Slot`, `IP`, `Mac`, `Coin`, `Donate`, `Cargo`, `SelChar` (all 4 characters plus equipment), `Trade`, `AutoTrade`, `TradeMode`, `PKMode`, `Admin`, `MuteChat`, `OnlyTrade`, `KingChat`, `Whisper`, `Guildchat`, `PartyChat`, `Chatting`, `NumError`, `Range`, `CastleStatus`, all tick fields, `LastChat`, `Unk9`. Most are re-written at account confirmation or character confirmation, but several are not: `MuteChat`, `OnlyTrade` (reset at CNF login), `NumError` (reset at CNF character login), `Range` (reset only when the mob score is recalculated), `LastChat` (reset at CNF character login) and `Unk_1816` (reset at CNF login).

The practical risk is a **data leak across sessions on the same slot**: a new client briefly sees the previous player's `SelChar`, `Cargo` and `Coin` values in any code that runs between `USER_ACCEPT` and `USER_SELCHAR`. Two concrete exposures exist: `GetUserByName` and the MAC scan skip empty slots by mode, but the `_MSG_DBCheckPrimaryAccount` loop reads `pUser[i].AccountName` for any slot whose mode is not EMPTY — and `AccountName` is truncated to length 0 by this function, which happens to make the comparison safe. Meanwhile `Cargo` is only rewritten at account confirmation, so a failed login leaves the previous player's warehouse in the slot; because the session never reaches `USER_PLAY`, it is not addressable by the client, but it is still resident and visible in crash dumps.

**Rule workflow**:
`CloseSocket -> Sock=0 -> IsBillConnect=0 -> Mode=EMPTY -> AccountName[0]=0 -> return TRUE` and all other fields unchanged.

---

### Business Rule: BR-21 Shutdown drain ordering

**Overview**:
During shutdown, sessions are closed one per second in ascending index order until the table is empty, then the process quits.

**Detailed description**:
When `ServerDown` reaches 120, `ProcessSecTimer` enters a loop that skips `USER_EMPTY` slots from `UserCount` upward, closes the first live one, and increments `UserCount` (`Code/TMSrv/ProcessSecMinTimer.cpp:55-94`). When `UserCount >= MAX_USER` the server writes "server down complete", deletes the font and posts `WM_QUIT`. The timer period is lengthened to 200 ms at the 120 mark (`:115`) but the drain itself is driven by the once-per-second `TIMER_SEC` handler, so the wall-clock rate is one session per second.

The ordering by index means the drain is deterministic and independent of which sessions are playing, which simplifies reasoning but is unfair to clients with low indices (they are disconnected first). Because each close goes through `CloseUser`, playing sessions enter the asynchronous `USER_SAVING4QUIT` handshake, so the process may reach `PostQuitMessage` before DBSrv has acknowledged every save — the acknowledgement is not awaited anywhere in the drain loop.

`UserCount` is reused as the drain cursor and is not otherwise meaningful; after a drain it is left at 1000.

**Rule workflow**:
`ServerDown==120 -> while: UserCount>=MAX_USER ? quit : Mode==EMPTY ? UserCount++ : CloseUser(UserCount), UserCount++ -> return`.

---

### Business Rule: BR-22 Periodic save and billing eviction

**Overview**:
One session is saved every eight seconds in round-robin, and limited accounts are evicted during the nightly window.

**Detailed description**:
Every `SecCounter % 8 == 0` the loop checks `pUser[SaveCount]` (`Code/TMSrv/ProcessSecMinTimer.cpp:169-199`). `SaveCount` wraps to 1 at `MAX_USER`, so it cycles through 999 slots taking eight seconds per step — a full pass takes roughly 2.2 hours. A slot is only saved when `Mode == USER_PLAY && pMob[SaveCount].Mode != MOB_EMPTY`, and the loop `break`s after handling exactly one such slot, so the cadence is strictly one save per eight seconds regardless of population.

The eviction branch precedes the save: if `BILLING == 2 && pUser[SaveCount].Unk_2728 == 1 && pMob[SaveCount].MOB.CurrentScore.Level >= FREEEXP && (g_Hour <= 12 || g_Hour >= 19)` the client is told `_NN_Child_Pay` and `CharLogOut` is called instead of a save. The hour test is inverted relative to the login gate (`g_Hour > 7 && g_Hour < 23`), so a child account is allowed to *enter* during 8..22 but is evicted outside 13..18 — the two conditions are not complements, which is a behavioural inconsistency in the billing policy rather than a code defect.

Because `SaveCount` is a single global cursor shared with nothing else, the rule is cheap and lock-free; its weakness is that a session that disconnects between visits is never reached by the periodic path and relies entirely on the close path.

**Rule workflow**:
`SecCounter % 8 == 0 -> SaveCount wrap -> USER_PLAY && mob alive ? (child-pay window ? message + CharLogOut : SaveUser(0)) -> SaveCount++ -> break`.

---

### Business Rule: BR-23 Warehouse (Cargo) and warehouse coin semantics

**Overview**:
`Cargo[128]` plus `Coin` form the account-wide warehouse, transferred wholesale from/to DBSrv and mutated only through deposit/withdraw with 2e9 caps.

**Detailed description**:
The constructor zeroes `Cargo` (`Code/TMSrv/CUser.cpp:32`), the account-confirmation handler replaces it with the DB copy (`ProcessDBMessage.cpp:436`), and both `SaveUser` and `MSG_SavingQuit` send it back (`Server.cpp:6900, 7065`). During play the only sanctioned mutations are deposit/withdraw of coin, and item-level mutations from the DB (`_MSG_DBSendItem` inserting at the first `BASE_CanCargo`-approved cell, `ProcessDBMessage.cpp:229-278`), autotrade sale proceeds (`_MSG_ReqBuy.cpp:152`), withdrawal of a listed item, and crown removal (`Server.cpp:670-681`).

Deposit requires a live playing session (`Mode == USER_PLAY` and `Hp != 0`), `0 <= amount <= 2e9`, `mob.Coin >= amount`, and `warehouse + amount <= 2e9`; the arithmetic is done in `int` so the sum can overflow before the range check (`Code/TMSrv/_MSG_Deposit.cpp:33-40`). Withdraw mirrors it (`_MSG_Withdraw.cpp:33-41`). Both acknowledge by echoing the request with a fixed scene ID (30000/field) and then push `SendCargoCoin`.

The warehouse is not part of `SelChar`, so it is shared by all four characters of the account — the field belongs to the account, not the avatar. Autotrade listings reference warehouse cells (`AutoTrade.CarryPos[i]` indexes `Cargo`), which is why a listing is invalidated whenever the underlying cargo cell changes (BR-45).

**Rule workflow**:
`Deposit/Withdraw -> PLAY + HP + bounds + overflow checks -> mutate Coin & mob.Coin -> SendCargoCoin + log`; `save -> Cargo[128] + Coin` to DBSrv.

---

### Business Rule: BR-24 Player trade preconditions and ownership invariants

**Overview**:
A trade invitation is only valid between two live, non-PK, non-whisper-blocked sessions, over items the sender actually owns, without duplicates, and within carry bounds.

**Detailed description**:
`Exec_MSG_Trade` begins with the standard live-session guard (`HP != 0 && Mode == USER_PLAY`) and then validates the opponent index, the opponent's mode, and `0 <= TradeMoney <= mob.Coin` (`Code/TMSrv/_MSG_Trade.cpp:24-54`). It then loops over all 15 trade slots and, for every non-empty item, verifies `0 <= InvenPos < MAX_CARRY-4` and that `memcmp(MOB.Carry[pos], Item)` is zero — i.e. the item is still exactly where and what the client claims (`:56-85`). The same identity check is applied to the *opponent's* stored trade state, so both sides are continuously validated.

Additional gates: if the opponent's `Trade.OpponentID` is set and not equal to the sender, the trade is refused as "already trading"; `PKMode` on either side blocks; `Whisper` (the opt-out flag) on either side blocks; guild-bound items (747, 3993, 3994, guild cloaks 508/522/526-537/446) require the appropriate guild master; and any item with `EF_NOTRADE` blocks (`:120-190`). Duplicate `InvenPos` values are rejected later when the opponent has already accepted (`:211-219`).

The `MAX_CARRY-4` bound reserves the last four carry cells, matching the autotrade purchase scan (`_MSG_ReqBuy.cpp:106`) and the `BASE_CanTrade` recomputation. Because `InvenPos` is a `char[15]`, `-1` is a legitimate sentinel and the check `iPos < -1` is the only negative rejection.

**Rule workflow**:
`live? -> opponent valid/playing? -> money <= own? -> per-slot ownership + duplicate + NOTRADE/guild checks -> pkmode/whisper gates -> proceed to check protocol`.

---

### Business Rule: BR-25 Trade confirmation, money ceiling and settlement

**Overview**:
The trade commits only when both parties press check with identical state; money is capped at 2e9 on both sides and both inventories are recomputed before the swap.

**Detailed description**:
When `m->MyCheck == 1` the server copies `MyCheck`, `KeyWord`, `CheckSum` and `ClientTick` into the stored `Trade`, then `memcmp`s the entire stored struct against the incoming packet (`Code/TMSrv/_MSG_Trade.cpp:225-238`). Any difference aborts both sides and logs `err,msg_trade - Click check button but diffrent data`. If the opponent has not checked yet, the packet is forwarded with `ID`/`OpponentID` swapped and no commit happens. When both are checked, the server validates each side's `TradeMoney` against `[0, 2e9]`, computes both resulting balances (`opponentMoney + ownCoin - ownMoney`, and the symmetric expression), rejects if either exceeds 2e9 or goes negative, and re-checks solvency.

Only then are the inventories recomputed by `BASE_CanTrade(mydest, MOB.Carry, Trade.InvenPos, opponent.Trade.Item, MaxCarry)` for both parties (`:301-329`). The function returns 0 when the receiving side has no space; both-zero produces a shared "no space" message, single-zero produces the asymmetric messages. The committed state is the two returned inventory arrays plus the two computed coin balances, and the server saves both sessions (`SaveUser(conn,1)`, `SaveUser(opponent,1)`) before `RemoveTrade` on both.

`memcmp` over `MSG_Trade` includes the header fields (`Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`), which is why the header values are stored alongside the items; but it also includes any struct padding, whose bytes come from the network buffer. If the client does not send a fully initialised struct, the comparison can fail spuriously and abort a legitimate trade.

**Rule workflow**:
`MyCheck -> store check fields -> memcmp stored vs incoming -> equal? -> opponent checked? -> money ceilings -> BASE_CanTrade both -> swap carry/coin -> SendCarry both -> ItemLog -> SaveUser both -> RemoveTrade both`.

---

### Business Rule: BR-26 Trade/AutoTrade reset semantics

**Overview**:
`RemoveTrade` is the single reset for both transaction buffers and also closes an open shop, but only for playing sessions.

**Detailed description**:
`RemoveTrade(int conn)` validates the index, zeroes `Trade`, sets `Trade.InvenPos[0..14] = -1`, zeroes `AutoTrade`, sets `AutoTrade.CarryPos[0..11] = -1`, and returns early if `Mode != USER_PLAY` (`Code/TMSrv/Server.cpp:7319-7352`). The early return means the buffers are always cleared, but the client-facing signal and the `TradeMode` teardown only happen for live sessions — which is correct, because a non-playing session has no client to notify.

For a playing session it sends signal `900` (the client's "trade window closed" signal), and if `TradeMode` was set it re-broadcasts the mob via `GetCreateMob` so other players see the shop disappear, then clears `TradeMode`. Note that it does **not** clear `TradeMode` when `Mode != USER_PLAY`, so a session that closes its shop and then leaves the world keeps `TradeMode == 1` until `CloseUser`... except `CloseUser` does not clear it either; the next character confirmation sets it to 0.

Every caller relies on this function being idempotent, and it is: repeated calls simply re-zero the same memory and, for playing sessions, re-send signal 900. The one non-idempotent effect is the `GridMulticast` of `GetCreateMob`, which will be seen by nearby clients each time.

**Rule workflow**:
`index check -> clear Trade + InvenPos=-1 -> clear AutoTrade + CarryPos=-1 -> Mode != PLAY ? return : signal 900 + (TradeMode ? rebroadcast mob + TradeMode=0)`.

---

### Business Rule: BR-27 Autotrade listing validation

**Overview**:
An offline-style shop listing is only created from a village, outside the newbie event server, with 12 well-formed slots referencing real cargo items.

**Detailed description**:
`Exec_MSG_SendAutoTrade` requires a live playing session, no active trade (`Trade.OpponentID == 0`), no open shop (`TradeMode == 0`), and `NewbieEventServer != 0` (`Code/TMSrv/_MSG_SendAutoTrade.cpp:24-49`). It refuses the coordinates of the newbie-event exclusion box (2123-2148 × 2139-2157) and any position whose `BASE_GetVillage` is outside `[0,5)`. The village also determines the tax that is snapshotted into the listing (`m->Tax = g_pGuildZone[Village].CityTax`, `:98`).

For each of the 12 slots it enforces: `0 <= Coin[i] <= 1,999,999,999`; coin without an item is invalid; an item requires a positive coin; the item index must not be one of the trade-restricted ids (508, 3993, 747, 509, 522, 526-531, 446); `CarryPos[i]` must be in `[0, MAX_CARGO)`; `EF_NOTRADE` must be absent; and `memcmp(Cargo[CarryPos[i]], Item[i])` must be zero. Any violation returns silently — there is no error message and no negative acknowledgement, so a malformed request is indistinguishable from packet loss.

On success it copies the title (truncated to `MAX_AUTOTRADETITLE-2`), sets `TradeMode = 1`, copies the whole struct into `AutoTrade`, and broadcasts a `MSG_CreateMobTrade` so the shop becomes visible in the world. The three parallel arrays (`Item[]`, `CarryPos[]`, `Coin[]`) are the listing's entire state and must stay mutually consistent; `CarryPos[i] == -1` is the "slot sold/empty" marker.

**Rule workflow**:
`live + no trade + no shop + event server -> coordinates/village -> per-slot coin/item/cargo/NOTRADE checks -> Tax snapshot -> TradeMode=1 -> AutoTrade copy -> SendAutoTrade + mob broadcast`.

---

### Business Rule: BR-28 Autotrade purchase

**Overview**:
A purchase must match the listing exactly, be affordable, leave the seller under the 2e9 cap, and free a carry slot; the server then atomically moves the item and the money.

**Detailed description**:
`Exec_MSG_ReqBuy` requires a live session with no active trade and no open shop, a valid seller index, and proximity within one view grid in both axes (`Code/TMSrv/_MSG_ReqBuy.cpp:20-61`). It reads `StorageSlot = pUser[seller].AutoTrade.CarryPos[itemPos]`, validates `0 <= StorageSlot < MAX_CARGO`, then verifies three identities: the tax in the request equals `AutoTrade.Tax`, the price equals `AutoTrade.Coin[itemPos]`, the item equals `AutoTrade.Item[itemPos]`, and the item also equals `Cargo[StorageSlot]`. Each failure either silently returns or calls `RemoveTrade`.

Money handling is done before delivery: the buyer must have `mob.Coin >= price`, and `seller.Coin + price <= 2e9`. Delivery requires a free carry cell in `[0, MAX_CARRY-4)`; the seller must be located in a valid village (checked *after* the carry scan). The commit copies the cargo item into the buyer's carry, computes `imposto = (price/100) * itemTax` for prices >= 100,000, credits `price - imposto` to the seller's warehouse coin, clears the listing slot (`CarryPos = -1`, item zeroed, coin zeroed), zeroes the cargo cell and pushes it to the seller, and debits the buyer.

The tax rule creates a discontinuity at exactly 100,000: below it the seller receives the full price and `Tax` is only informational; at or above it the guild tax is withheld. Because `itemTax` comes from the snapshot, a later tax change does not retroactively affect existing listings.

**Rule workflow**:
`live/no-trade/no-shop -> seller valid -> in view -> storage slot -> tax/price/item/cargo identity -> buyer funds -> seller cap -> buyer space -> village -> transfer item/coin -> clear listing -> logs`.

---

### Business Rule: BR-29 Anti-cheat error budget

**Overview**:
Every protocol or physics violation adds points to `NumError`; saturation forces a character logout.

**Detailed description**:
`AddCrackError(conn, val, Type)` logs most error types (silently ignoring types 3, 8 and 15, which are treated as noise), adds `val` to `pUser[conn].NumError`, and when the value reaches 2,000,000,000 sends `_NN_Bad_Network_Packets`, calls `CharLogOut(conn)` and returns `TRUE` so the caller can abort (`Code/TMSrv/Server.cpp:684-707`). Callers either ignore the return (accumulating errors) or use it to stop processing immediately.

Points vary by violation: 1 for minor protocol anomalies, 2, 4, 5 or 8 for movement/attack oddities, 10 for "action while not playing" in most handlers, 50 for item-update violations, 208 for one attack path (a value chosen to near-saturate). Because the budget is essentially 2e9 and typical penalties are 1-10, a cheater can issue hundreds of millions of violations before eviction; the mechanism is a corruption counter for detecting pathological loops, not a practical rate limiter.

The counter is reset at character login (`ProcessDBMessage.cpp:822`), on death-restart (`_MSG_Restart.cpp:28`), when clearing dead players from an area (`Server.cpp:8706`), and partially in attack paths (`_MSG_Attack.cpp:1039, 1249, 1259`). Note the reset on restart means an attacker can deliberately die and restart to clear the budget.

**Rule workflow**:
`violation -> AddCrackError(conn,val,type) -> NumError += val -> >= 2e9 ? message + CharLogOut + return TRUE : return FALSE`.

---

### Business Rule: BR-30 Attack and action rate limiting

**Overview**:
Client-supplied tick values are validated against both a minimum interval and a 15-second window around server time.

**Detailed description**:
`Exec_MSG_Attack` rejects the packet if `ClientTick != SKIPCHECKTICK && LastAttackTick != SKIPCHECKTICK && ClientTick < LastAttackTick + 800`, and separately penalises a backwards jump of more than 100 (`Code/TMSrv/_MSG_Attack.cpp:59-77`). It then requires `ClientTick <= CurrentTime + 15000` and `ClientTick >= CurrentTime - 120000` (the `CurrentTime <= 0x1D4C0` guard covers the first two minutes after boot when no baseline exists), penalising with type 107 (`:79-97`). Cast-level delay is enforced separately: the delta from the previous tick must be at least `g_pSpell[skillnum].Delay * 1000` with a 700 ms hard floor, reduced by one second when `RSV_CAST` is set (`:124-145`).

`Exec_MSG_Action` uses the same 15-second window but a 900 ms interval, tracked in `LastIllusionTick` for `_MSG_Action`/`_MSG_Action3`, plus a duplicate-`_MSG_Action2` detector using `LastMove` (`Code/TMSrv/_MSG_Action.cpp:92-145`). The illusion branch additionally requires class 3 and learned skill bit 2 before allowing `_MSG_Action3` (`:71-79`).

`SKIPCHECKTICK` (235543242, `Code/Basedef.h:172`) is the sentinel that disables these checks; it is set for all three tick fields at character login so the first packet is never rejected, and any client packet carrying it is dropped by the dispatcher (BR-06) so a client cannot use the sentinel to bypass the limits.

**Rule workflow**:
`ClientTick == SKIPCHECKTICK ? skip : (interval check -> window check -> skill delay check) -> store LastAttackTick/LastIllusionTick`.

---

### Business Rule: BR-31 Consumable and broadcast cooldowns

**Overview**:
Potions are rate-limited by `PotionTime`/`PotionDelay`, and kingdom/citizen broadcasts by `Message` with a fixed three-second window.

**Detailed description**:
For volume-1 consumables, `Exec_MSG_UseItem` compares `GetTickCount() - pUser[conn].PotionTime` against the global `PotionDelay` and, if too soon, re-sends the item to the client and returns without consuming it (`Code/TMSrv/_MSG_UseItem.cpp:98-112`). `PotionTime` is then set to the current tick, so the cooldown is global to the session rather than per potion type. `UseItemTime` exists in the same role but its only enforcement block is commented out (`:207-224`); it is still written for item 4146 (`:3214`), making it a write-only field in practice.

For chat, the kingdom (`@@`) and citizen (`@`) broadcast branches compute `isTime = GetTickCount() - pUser[conn].Message` and refuse with "Aguarde 3 segundos." when the interval is under 3,000 ms, then set `Message = GetTickCount()` unconditionally on the accepted path (`Code/TMSrv/_MSG_MessageWhisper.cpp:1231-1270`). `Message` is reset to 0 at character login and at character confirmation, so the first broadcast is never throttled.

Both rules use wall-clock `GetTickCount()` (32-bit, wraps at ~49.7 days) rather than the server `SecCounter`, which makes them vulnerable to the wrap: a wrap makes `isTime` negative and therefore "too soon" indefinitely until the next accepted broadcast resets the baseline. `PotionTime` has the same property.

**Rule workflow**:
`PotionTime != 0 && now - PotionTime < PotionDelay ? resend item + return : PotionTime = now`; `Message != 0 && now - Message < 3000 ? message + return : Message = now`.

---

### Business Rule: BR-32 Chat channel flags and moderation

**Overview**:
Five toggles control chat visibility, one of them (`Chatting`) is inert, and `MuteChat` overrides all outgoing chat.

**Detailed description**:
The `/whisper`, `/partychat`, `/kingdomchat`, `/guildchat` and `/chatting` commands each flip a session flag and echo the new state (`Code/TMSrv/_MSG_MessageChat.cpp:111-169`). The semantics are "1 = off": `pUser[i].Guildchat` skips the recipient in guild broadcast (`_MSG_MessageWhisper.cpp:1169`), `pUser[partyconn].PartyChat` skips party recipients (`:1216`, `SendFunc.cpp:1016, 1042`), `pUser[i].KingChat == 0` is required for kingdom chat (`SendFunc.cpp:278`), and `pUser[target].Whisper` blocks whispers unless the sender is level >= 1000 (`_MSG_MessageWhisper.cpp:1320`). `Chatting` is written at three sites and read at none — it is a vestigial flag.

`MuteChat == 1` short-circuits local chat, whisper, `/spk` and both broadcast branches before any routing (`_MSG_MessageChat.cpp:181-185`; `_MSG_MessageWhisper.cpp:100, 1129`). It is toggled by the GM command `/gm mute <name>` and force-set/cleared by `muteall` and `desmuteall` (`Code/TMSrv/imple.cpp:1514-1562`), which set the flag for every session with `Mode >= USER_SELCHAR`. `MuteChat` is not cleared by `CloseUser` and is not reset at login, so a mute survives re-login on the same slot until the next account confirmation... which also does not reset it. Only explicit GM action changes it.

`LastChat` complements the flags: it stores the last whisper correspondent (and is back-filled on the recipient) so `/r` can reply without a name (`_MSG_MessageWhisper.cpp:1295-1370`), and it is cleared at character confirmation.

**Rule workflow**:
`chat command -> toggle flag -> echo state`; `mute set -> all chat paths return _NN_No_Speak`; `broadcast -> throttled -> routed to recipients whose opt-out flag is 0`.

---

### Business Rule: BR-33 OnlyTrade (primary-account) restriction

**Overview**:
A session flagged `OnlyTrade` may only trade, not fight, move, group or whisper.

**Detailed description**:
DBSrv sends `_MSG_DBCheckPrimaryAccount` with a MAC and an account name; the server marks the matching account `OnlyTrade = 0` and every other account sharing that MAC `OnlyTrade = 1` (`Code/TMSrv/ProcessDBMessage.cpp:148-182`). For the restricted sessions the handler also removes the party and, if the player is in the world outside a village and outside a PvP-marked attribute, sends `_NN_OnlyVillage` and recalls them to town.

Enforcement is by explicit check in the combat and social handlers: attack (`_MSG_Attack.cpp:47-51`), action/movement (`_MSG_Action.cpp:198`), whisper send and summon (`_MSG_MessageWhisper.cpp:744, 810`), party invite/accept (`_MSG_SendReqParty.cpp:56`, `_MSG_AcceptParty.cpp:71, 77`), and level-based party bonus eligibility (`_MSG_UseItem.cpp:2303, 2343`). The check is deliberately repeated per handler rather than centralised, matching the codebase's style.

The flag is re-armed to 1 at account confirmation and relaxed only by the DB check, so a session that never receives the check remains restricted. Because `CloseUser` does not clear it, a reused slot starts restricted, which is the safe default.

**Rule workflow**:
`DB MAC/account compare -> matching account OnlyTrade=0, others OnlyTrade=1 + RemoveParty + recall -> handlers block restricted sessions`.

---

### Business Rule: BR-34 Deferred HP/MP accounting

**Overview**:
The session stores target HP/MP (`ReqHp`/`ReqMp`) which are converged in bounded steps, replacing instantaneous client-reported vitals.

**Detailed description**:
At character login `ReqHp`/`ReqMp` are seeded from the mob's current score (`ProcessDBMessage.cpp:980-981`), and at character confirmation they are zeroed again in the same block. `ApplyHp`/`ApplyMp` clamp the target to `MaxHp`/`MaxMp`, compute the difference, cap a single step at 2,000, add it to the current value, clamp the total at 2e9, and then clamp back to the target (`Code/TMSrv/Server.cpp:8551-8657`). They return TRUE only when a change occurred, which the caller uses to decide between `SendScore` and `SendSetHpMp`.

`SetReqHp`/`SetReqMp` normalise the target: negative becomes 0, the mob's current value is clamped to its maximum, and the target is raised to at least the current value (`:8659-8687`). This makes the target monotone with respect to observed damage/healing events and prevents a target below the real value from draining the character.

The convergence runs on the two-second tick (`ProcessSecMinTimer.cpp:601-619`) for live, non-dead players, and `Unk_2688` is cleared there each pass. Because the step is capped at 2,000 per pass and the pass runs every two seconds, very large deltas take multiple passes — a deliberate smoothing used by the client's display.

**Rule workflow**:
`SetReqHp/Mp -> clamp -> ReqHp/ReqMp`; `every 2 s for PLAY & HP>0 -> Unk_2688=0 -> ApplyHp -> ApplyMp -> SendScore/SendSetHpMp`.

---

### Business Rule: BR-35 Attack range snapshot

**Overview**:
`Range` shadows the attacker's mob range for the distance check, with a hard ceiling of 23.

**Detailed description**:
When a mob score is recalculated, `CMob::GetCurrentScore` writes `pUser[idx].Range = BASE_GetMobAbility(&MOB, EF_RANGE)` and then immediately overwrites it with `23` for every `idx < MAX_USER` (`Code/TMSrv/CMob.cpp:657-663`). The first assignment is dead code, so the value is always 23 regardless of equipment or class. The consumer is `Exec_MSG_Attack`, which rejects when `dis > pUser[conn].Range || dis > 23` (`Code/TMSrv/_MSG_Attack.cpp:426`), making the two conditions identical.

Storing the value in the session rather than reading it from the mob on demand is a deliberate micro-optimisation typical of the decompiled source; it also means `Range` can go stale if the mob's range ever becomes dynamic. Since `Range` is not reset by `CloseUser`, it carries over between sessions on the same slot until the next score recalculation (which happens at every character login).

The rule's practical effect is a fixed 23-unit melee/ranged ceiling, which is the W2 caliper for normal attacks; skills use their own per-spell ranges elsewhere.

**Rule workflow**:
`GetCurrentScore -> Range = 23`; `Attack -> GetLength(dx,dy) -> dis > Range || dis > 23 ? reject`.

---

### Business Rule: BR-36 Castle charge accumulator

**Overview**:
Standing on the castle pad accumulates `CastleStatus` once per regen pass; exceeding 22 captures the castle.

**Detailed description**:
`RegenMob(int conn)` resets `CastleStatus = 0` unless the character is on the castle pad `(1046,1690)` during an active siege (`CastleState > 1`), belongs to a valid guild whose guild level is 9, and the guild is not already the charging guild (`Code/TMSrv/Server.cpp:4362-4382`). When the conditions hold, the first pass broadcasts the "charging" state and every pass increments the counter, with `> 22` triggering `_SN_S_charge_castle`, transferring `g_pGuildZone[4].ChargeGuild`/`Clan`, `FinishCastleWar()` and clearing crowns for all players.

The counter therefore encodes roughly 23 seconds of uninterrupted presence, since `RegenMob` runs every 16 seconds per slot (`ProcessSecMinTimer.cpp:1184`). Any interruption — moving off the pad, dying, leaving the guild, a siege ending — resets it to 0 through the guard clause. Because the reset is computed from the *current* position each pass, there is no separate timer state to leak.

`CastleStatus` is initialised to 0 in the constructor and at character confirmation, and is never persisted; a re-login during a siege restarts the charge.

**Rule workflow**:
`RegenMob -> conditions false ? CastleStatus=0 : (first pass -> broadcast, CastleStatus++ -> >22 ? capture + FinishCastleWar + ClearCrown all)`.

---

### Business Rule: BR-37 Long-period session counter

**Overview**:
`Unk_2736` counts regen passes and drives two slow effects every 450 passes.

**Detailed description**:
`RegenMob` increments `pUser[conn].Unk_2736` each pass and, when `Unk_2736 % 450 == 0`, applies PK-point decay/increase and mount incubation/feeding (`Code/TMSrv/Server.cpp:4415-4500`). With a 16-second cadence per slot, 450 passes is two hours of in-world time, so the field is a coarse "online duration" counter used to schedule slow effects without a dedicated timer.

It is reset at character confirmation (`Unk_2736 = 0`) and when the exp segment is computed for low levels (`ProcessDBMessage.cpp:1046`). The reset at login means the two-hour cadence restarts per session, so a player who relogs frequently never reaches the trigger — an observable behavioural quirk rather than a defect in the field itself.

Because the field is a plain `int` incremented once per pass, it will overflow only after ~2^31 passes (over 1,000 years), so no wrap handling is needed.

**Rule workflow**:
`RegenMob -> Unk_2736++ -> (Unk_2736 % 450 == 0) ? PK-point/mount effects : nothing`.

---

### Business Rule: BR-38 Cash ledger (`Donate`) and billing linkage

**Overview**:
`Donate` is the premium-currency balance owned by the session, while `IsBillConnect` is a linkage flag that is never raised in this build.

**Detailed description**:
`Donate` is loaded from the DB at character login (`pUser[conn].Donate = m->Donate`, `ProcessDBMessage.cpp:728`), incremented by `_MSG_DBSendDonate` after verifying the account name matches and the session is at least `USER_SELCHAR` (`:1217-1251`), spent by shop code (`_MSG_Buy.cpp`, `_MSG_UseItem.cpp`), and persisted in both `SaveUser` and `MSG_SavingQuit` (`Server.cpp:6909, 7073`). Because it is stored in `CUser`, it is account-level, like `Cargo` and `Coin`.

`IsBillConnect` is initialised to 0, cleared at account confirmation and at close, and tested at close and logout to decide whether to send a billing release (`CUser.cpp:29, 85`; `Server.cpp:6845, 7101`). No TMSrv code sets it to 1: `SendBilling` is a stub returning TRUE (`Server.cpp:1377-1380`), so the billing subsystem is disconnected in this build. The flag is therefore a dormant integration point: when billing is re-enabled, both tests will start firing and the release messages will be emitted.

The distinction matters for analysis because the architecture description of "billing flag" implies live behaviour; the observed behaviour is inert. Any reconstruction must preserve the field and its reset points to keep the persistence contract intact.

**Rule workflow**:
`DB login -> Donate load`; `DB donate msg -> account check -> Donate += amount -> ack`; `save -> Donate`; `close/logout -> IsBillConnect ? SendBilling(release) : nothing`.

---

### Business Rule: BR-39 Origin address bookkeeping

**Overview**:
`IP` is the raw accept-time address, used only for logging and administrative bookkeeping.

**Detailed description**:
`AcceptUser` stores `acc_sin.sin_addr.S_un.S_addr` (`CUser.cpp:74`), i.e. the 32-bit address in network byte order. It is never compared, validated or used for authorisation. Its consumers are the log helpers: `Log(str1, str2, ip)` reinterprets the integer byte-wise so the dotted form prints correctly (`Server.cpp:6710-6716`), and `imple.cpp:1687` copies it into `pAdminIP[]` when a GM registers an administrative IP.

Because the value is not cleared on close, the previous session's address is visible to any code that reads it before `AcceptUser` runs. In practice all readers run after accept, so the value is always the current client's. There is no proxy-header or X-Forwarded-For handling; the address is the TCP peer.

The field is signed `unsigned int` and stored in the session, not in `CMob`, reinforcing that it is connection identity rather than character identity.

**Rule workflow**:
`accept -> IP = peer address -> all subsequent logging uses it -> not reset on close`.

---

## 4. Component Structure

```
Code/TMSrv/                                  # component boundary (consumers included for context)
├── CUser.h                    (124 lines)   # class definition, 8 mode macros, 55 data members, 3 methods
├── CUser.cpp                   (92 lines)   # ctor, dtor, AcceptUser, CloseUser  <- PRIMARY IMPLEMENTATION
├── Server.h                   (418 lines)   # extern CUser pUser[MAX_USER]; hWndMain; function contracts
├── Server.cpp               (214 KB)        # pUser[] definition (:269), accept/read pump (:3919-4045),
│                                            # GetEmptyUser/GetUserFromSocket/GetUserByName (:6729-6827),
│                                            # CloseUser (:6834-6926), SaveUser (:7056), CharLogOut (:7085),
│                                            # CheckIdle (:4304), RemoveTrade (:7319), AddCrackError (:684)
├── ProcessClientMessage.cpp   (6.4 KB)      # dispatcher: Mode-driven routing, LastReceiveTime refresh
├── ProcessClientMessage.h     (4.9 KB)      # includes CUser.h
├── ProcessDBMessage.cpp      (40 KB)        # 15 Mode transitions; full session init; save-quit ack
├── ProcessDBMessage.h         (1.2 KB)      # includes CUser.h
├── ProcessSecMinTimer.cpp    (63.9 KB)      # shutdown drain, 8 s save loop, flush, idle, ReqHp/ReqMp tick
├── SendFunc.cpp              (39.9 KB)      # 40+ "Mode != USER_PLAY -> return" projections
├── imple.cpp                 (46.6 KB)      # GM commands: mute/muteall/desmuteall, kick, admin IP
├── _MSG_AccountLogin.cpp                    # ACCEPT -> LOGIN, name normalisation, MAC capture
├── _MSG_CharacterLogin.cpp                  # SELCHAR -> CHARWAIT, billing/level gate
├── _MSG_CreateCharacter.cpp                 # SELCHAR -> WAITDB
├── _MSG_DeleteCharacter.cpp                 # SELCHAR -> WAITDB
├── _MSG_Trade.cpp / _MSG_QuitTrade.cpp      # Trade buffer ownership, check protocol, settlement
├── _MSG_SendAutoTrade.cpp / _MSG_ReqBuy.cpp # AutoTrade listing and purchase
├── _MSG_Deposit.cpp / _MSG_Withdraw.cpp     # Cargo coin rules
├── _MSG_Attack.cpp / _MSG_Action.cpp        # tick rate limits, Range, cProgress
├── _MSG_UseItem.cpp                         # PotionTime, TradeMode guard, Cargo access
├── _MSG_MessageChat.cpp / _MSG_MessageWhisper.cpp  # channel flags, MuteChat, Message cooldown, LastChat
└── CMob.cpp                                 # writes pUser[idx].Range (:657-663); paired entity
```

The component has **no sub-modules**: the header declares 55 public data members and 3 methods in a single flat class (`Code/TMSrv/CUser.h:39-121`). Every member is `public`, so any translation unit can read or write any field; there is no encapsulation boundary beyond the file.

Member groups as declared (offsets are the original decompiler's comments and are **unreliable** — see section 11):

| Group | Members | Declared offset comment |
|-------|---------|------------------------|
| Identity | `AccountName[16]`, `Slot`, `IP`, `Mode`, `TradeMode`, `cSock` | 0, 16, 20, 24, 28, 32 |
| Opaque/unmapped | `Unk2[400]`, `Unk3` | 64, 464 |
| Storage | `Cargo[128]`, `Coin` | 468, 1492 |
| Progress/trade | `cProgress`, `Unk_1498`, `Trade`, `AutoTrade` | 1496, 1498, 1500, *(no comment)* |
| Tick state | `LastAttack`, `LastAttackTick`, `LastMove`, `LastAction`, `LastActionTick`, `LastIllusionTick`, `NumError`, `Unk_1816` | 1788-1816 |
| Selection screen | `SelChar` | 1820 |
| Social | `LastChat[16]`, `Whisper`, `Guildchat`, `PartyChat`, `Chatting`, `MuteChat`, `KingChat` | 2564-2652, 3061-... |
| Billing | `IsBillConnect`, `Unk_2728`, `Unk_2732`, `Unk_2736` | 2580, 2728, 2732, 2736 |
| Combat bookkeeping | `CharShortSkill[16]`, `PKMode`, `ReqHp`, `ReqMp`, `Range`, `cProgress`, `AttackTime` | 2620-2740 |
| Fingerprint/ranking | `Mac[4]`, `RankingTarget`, `RankingType`, `LastReceiveTime`, `Admin` | 2692-2724 |
| Castle/cash | `CastleStatus`, `Unk9[400]`, `Donate` | 2744, 2748 |
| Cooldowns | `UseItemTime`, `Message`, `AttackTime`, `LastClientTick`, `PotionTime` | *(no comments)* |
| Restriction | `OnlyTrade` | *(no comment)* |

---

## 5. Dependency Analysis

### 5.1 Efferent dependencies (what CUser depends on)

```
CUser.cpp
 ├── <Windows.h>                     (Win32: HWND, BOOL, memset, closesocket, WSAAsyncSelect)
 ├── "CUser.h"
 │    ├── "..\Basedef.h"             STRUCT_ITEM, MSG_Trade, MSG_SendAutoTrade, STRUCT_SELCHAR,
 │    │                              ACCOUNTNAME_LENGTH, MAX_CARGO
 │    └── "..\CPSock.h"              CPSock member by value (Sock, buffers, positions), WSA_READ
 └── "Server.h"                      extern HWND hWndMain (the only reason for this include)
```

Declared member types pull the dependency graph:

| Member | Type | Defined in |
|--------|------|------------|
| `cSock` | `CPSock` (by value, embedded) | `Code/CPSock.h:53-86` |
| `Cargo[MAX_CARGO]` | `STRUCT_ITEM[128]` | `Code/Basedef.h` |
| `Trade` | `MSG_Trade` | `Code/Basedef.h:2047-2057` |
| `AutoTrade` | `MSG_SendAutoTrade` | `Code/Basedef.h:1909-1923` |
| `SelChar` | `STRUCT_SELCHAR` | `Code/Basedef.h:765-778` |

**Efferent coupling count: 3** (CPSock, Basedef POD contracts, Win32/Winsock + `Server.h` globals) — matching the architecture report's `efferent 3`.

### 5.2 Afferent dependencies (who depends on CUser)

Transitive include path: `CUser.h` is included by `Server.h:39`, `ProcessClientMessage.h:39`, `ProcessDBMessage.h:38`; nearly every TMSrv `.cpp` includes one of those, which is why 61 of 63 translation units touch `pUser`.

Measured afferent accesses per field (occurrences of `pUser[...].<field>` in `Code/TMSrv/*.cpp`):

```
AccountName 399 | IP 365 | Mode 218 | Trade 89 | Cargo 62 | ReqHp 32 | ReqMp 29
TradeMode 20 | PKMode 17 | Mac 16 | AutoTrade 15 | OnlyTrade 14 | Slot 12 | Coin 12
LastAttackTick 10 | Donate 10 | SelChar 9 | NumError 9 | Message 9 | Unk_1816 8 | Whisper 8
LastIllusionTick 7 | PartyChat 7 | MuteChat 7 | CharShortSkill 6 | Guildchat 5
RankingType 5 | CastleStatus 5 | KingChat 5 | LastReceiveTime 5 | LastChat 5
UseItemTime 5 | Unk_2732 5 | LastActionTick 3 | LastMove 3 | Unk_2736 3 | IsBillConnect 3
Admin 3 | Range 3 | Unk_2688 3 | Unk_2708 1 | Unk5 1 | LastClientTick 1 | Unk_2628 1
Unk_2632 1 | LastAction 1 | cProgress 2 | Unk9 2 | Unk3 0 | Unk2 0 | AttackTime 0
```

Largest consumers by raw access count: `Server.cpp` (221), `ProcessDBMessage.cpp` (148), `SendFunc.cpp` (107), `_MSG_MessageWhisper.cpp` (81), `_MSG_Trade.cpp` (78), `_MSG_Attack.cpp` (73), `ProcessSecMinTimer.cpp` (49), `_MSG_CombineItemOdin.cpp` (41), `_MSG_TradingItem.cpp` (38), `_MSG_Action.cpp` (37).

### 5.3 Runtime dependency chains

```
WSA_ACCEPT -> GetEmptyUser -> CUser::AcceptUser -> CPSock::Sock/WSAAsyncSelect
WSA_READ   -> GetUserFromSocket -> CPSock::Receive -> CPSock::ReadMessage
           -> ProcessClientMessage -> Exec_MSG_* -> (pUser[conn] fields + pMob[conn])
Exec_MSG_AccountLogin -> CheckFailAccount -> DBServerSocket -> DBSrv
ProcessDBMessage -> pUser[conn].* (identity, storage, flags) -> Mode transition
CloseUser -> CUser::CloseUser -> CPSock::CloseSocket
         -> SendBilling (stub) | DBServerSocket::SendOneMessage(_MSG_DBNoNeedSave | _MSG_SavingQuit)
```

---

## 6. Afferent and Efferent Coupling

C++ class-level metrics (components = classes / translation units, since the unit of work here is the class):

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Instability I = Ce/(Ca+Ce) | Critical |
|-----------|------------------------|------------------------|----------------------------|----------|
| `CUser` (whole entity) | 61 translation units, 1,630 field accesses | 3 (CPSock, Basedef PODs, Win32/globals) | 0.047 | Critical |
| `CUser::Mode` (field as a logical component) | 41 files, 218 accesses | 8 mode constants | 0.163 | Critical |
| `CUser::AccountName` | 52 files, 399 accesses | 1 (`ACCOUNTNAME_LENGTH`) | 0.019 | Critical |
| `CUser::IP` | 52 files, 365 accesses | 0 | 0.000 | High |
| `CUser::Cargo` | 9 files, 62 accesses | 2 (`STRUCT_ITEM`, `MAX_CARGO`) | 0.182 | High |
| `CUser::Trade` | 18 files, 89 accesses | 1 (`MSG_Trade`) | 0.053 | High |
| `CUser::AutoTrade` | 7 files, 15 accesses | 1 (`MSG_SendAutoTrade`) | 0.125 | Medium |
| `CUser::Coin` | 6 files, 12 accesses | 0 | 0.000 | Medium |
| `CUser::SelChar` | 3 files, 9 accesses | 1 (`STRUCT_SELCHAR`) | 0.250 | Medium |
| `CUser::NumError` | 5 files, 9 accesses | 1 (`AddCrackError`) | 0.167 | Medium |
| `CUser` timer fields (attack/action/potion/message) | 8 files, 40 accesses | 2 (`CurrentTime`, `GetTickCount`) | 0.200 | Medium |
| `CUser` chat flags (Whisper/Guildchat/PartyChat/KingChat/Chatting/MuteChat) | 7 files, 38 accesses | 0 | 0.000 | Medium |
| `CUser` billing/unknown fields (`IsBillConnect`, `Unk_2728`, `Unk_2732`, `Unk_2736`, `Unk_2688`, `Unk_2708`, `Unk_2628/2632`, `Unk5`, `Unk9`, `Unk2`, `Unk3`) | 5 files, 18 accesses (12 of them writes only) | 0 | 0.000 | Low (dead) |
| `CUser::Admin` | 2 files, 3 accesses, **0 reads** | 0 | 0.000 | Low (dead) |

Notes on the metrics:

* `Mode` has the highest *semantic* fan-out because it is used as a gate in almost every handler, but `AccountName`/`IP` dominate raw access because they are parameters of every log call.
* `CUser` has the lowest instability of any type in TMSrv (only `CMob` comes close), meaning it is a **stable dependency**: changes to it propagate to all 61 units while changes in consumers never reach it.
* Cohesion is low by construction: the class mixes transport, identity, economy, social preferences, transaction staging and anti-cheat counters. There is no member function operating on the data, so LCOM (lack of cohesion of methods) cannot be computed — the data/method ratio is 55:3.
* The constructor does not cover the whole state surface (14 of 55 fields explicitly initialised), so "initialised" is a property of the global-array zeroing plus the two login blocks, not of the class.

---

## 7. (No service endpoints)

`CUser` exposes no REST/GraphQL/gRPC endpoint and no public protocol surface of its own; it is an in-process record. Its externally reachable behaviour is the TMSrv client socket (`GAME_PORT 8281`, `Code/Basedef.h:44`) and the DBSrv link, both owned by `Server.cpp`. This section is therefore omitted per the analysis guidelines; the protocol entry points are described under section 8.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Client (game client, port 8281) | External client | Inbound commands / outbound projections | TCP + `WSAAsyncSelect(FD_READ\|FD_CLOSE)`, 12-byte `HEADER` (`Size,KeyWord,CheckSum,Type,ID,ClientTick`), max 8 KB message | Packed C structs (`#pragma pack` PODs from `Basedef.h`) | Drop-on-invalid-ID; silent reject on `Mode != USER_PLAY`; `CloseUser` on framing/recv/send failure; `AddCrackError` on protocol violations |
| DBSrv (`DBServerSocket`) | Internal service | Account auth, character load/save, cargo, donate, char create/delete, server transfer | Custom length-prefixed messages over TCP (`CPSock::ConnectServer`, `WSA_READDB`) | `MSG_DB*` structs | Throw-away `_MSG_DBNoNeedSave` when session is `Mode == 0`; `CloseUser` on identity mismatch; early-return on out-of-range ID; no retry/queue |
| Billing server (`BillServerSocket`) | External service (stubbed) | Premium/child-account authorisation, connect/disconnect notifications | `CPSock::ConnectBillServer`, `WSA_READBILL`, `_AUTH_GAME` blob | Opaque 196-byte struct | `SendBilling` returns TRUE without doing anything (`Server.cpp:1377`); reconnect logic exists in `ProcessSecMinTimer.cpp:130-167` but the link is disabled (`BILLING = 0` at `Server.cpp:3608`) |
| `Mac.txt` (`../../DBSRV/Run/Mac.txt`) | File | MAC ban list → `pMac[200]` | Text, 5 integers per line (`idx a b c d`) | Plain text | Missing file silently ignored; reloaded every 120 s |
| `Admin.txt` (`../../DBSRV/Run/Admin.txt`) | File | Administrative IP allow-list → `pAdminIP[10]` | Text | Plain text | Missing file silently ignored; `pAdminIP` is never consulted by `CUser` logic |
| `gameconfig.txt` | File | `BILLING`, `FREEEXP`, `CHARSELBILL`, `POTIONCOUNT`, `PARTYBONUS`, `GUILDBOARD`, events | Text with strict header lines | Plain text | MessageBox + abort of the parse on header mismatch; partially parsed values retained |
| Log files (`Log/`, chat log, item log) | File sink | Audit; `IP`/`AccountName` are the join keys | `fprintf` | Text | `fLogFile` NULL-checked; no rotation except on date change (`StartLog`) |
| In-process `CMob pMob[MAX_MOB]` | Paired memory entity | Avatar state for the same index; `pUser` holds the session-owned remainder | Direct memory | Struct | Index equality assumed; no validation that `pMob[i].Mode` matches `pUser[i].Mode` |

**Protocol entry points that mutate the session** (not service endpoints, but the reachable surface of the entity):

| Message type | Handler | Session effect |
|--------------|---------|----------------|
| `_MSG_AccountLogin` | `_MSG_AccountLogin.cpp:21` | `AccountName`, `Mac`, `Mode=USER_LOGIN` |
| `_MSG_CharacterLogin` | `_MSG_CharacterLogin.cpp:21` | `Mode=USER_CHARWAIT` |
| `_MSG_CharacterLogout` | `_MSG_CharacterLogout.cpp:21` | `RemoveParty` + `CharLogOut` → `Mode=USER_SELCHAR` |
| `_MSG_CreateCharacter` / `_MSG_DeleteCharacter` | `:20` | `Mode=USER_WAITDB` |
| `_MSG_Trade` / `_MSG_TradingItem` / `_MSG_QuitTrade` | `_MSG_Trade.cpp:20` etc. | `Trade`, `TradeMode` |
| `_MSG_SendAutoTrade` / `_MSG_ReqBuy` | `_MSG_SendAutoTrade.cpp:20`, `_MSG_ReqBuy.cpp:20` | `AutoTrade`, `TradeMode`, `Cargo`, `Coin` |
| `_MSG_Deposit` / `_MSG_Withdraw` | `:20` | `Coin` |
| `_MSG_Attack` / `_MSG_Action` / `_MSG_Motion` / `_MSG_Restart` | `:20` | tick fields, `NumError` |
| `_MSG_UseItem` | `_MSG_UseItem.cpp:20` | `PotionTime`, `UseItemTime`, `Cargo`, `CharShortSkill` |
| `_MSG_MessageChat` / `_MSG_MessageWhisper` | `:20` | chat flags, `Message`, `LastChat` |
| `_MSG_SetShortSkill` | `_MSG_SetShortSkill.cpp:20` | `CharShortSkill[16]` |
| `_MSG_PKMode` | `_MSG_PKMode.cpp:20` | `PKMode` |
| `_MSG_ReqRanking` | `_MSG_ReqRanking.cpp:20` | `RankingTarget`, `RankingType` |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Session table / object pool | `CUser pUser[MAX_USER]`, index-as-handle, first-fit allocation | `Server.cpp:269`, `6743-6752` | O(1) access by connection id, index also addresses `pMob` |
| Explicit state machine | `int Mode` + 8 macros + 15 assignment sites | `CUser.h:25-37` | Phase gating of every handler |
| Active Object / message pump | `WSAAsyncSelect` + `MainWndProc` dispatch + `ProcessClientMessage` | `Server.cpp:3919-4045` | All session work serialised on the UI thread (no locks) |
| Data Transfer Object mirroring | `MSG_Trade`/`MSG_SendAutoTrade` copied verbatim into the session | `_MSG_Trade.cpp:389, 411`; `_MSG_SendAutoTrade.cpp:107` | Server-side staging of in-flight transactions so the client cannot rewrite intent |
| Guard-clause validation (pervasive) | ~119 `Mode != USER_PLAY` returns + index bounds at every entry | `SendFunc.cpp`, all `_MSG_*` | Defensive programming compensating for missing encapsulation |
| Sentinel-value protocol | `-1` in `Trade.InvenPos[15]` / `AutoTrade.CarryPos[12]`; `SKIPCHECKTICK` in tick fields; `0xFF` in `Mac` | `ProcessDBMessage.cpp:839-843`; `Basedef.h:172` | Encode "empty"/"no baseline" without extra flags |
| Shadow/paired entity | `pUser[i]` (account/session) + `pMob[i]` (avatar) share the index | `Server.cpp:269-270` | Split persistent-account from world state |
| Layered architecture (nominal) | Socket → dispatcher → handler → domain (`CMob`, `BASE_*`) → persistence (`DBSrv`) | whole project | Separation exists but the session entity is a shared global that crosses all layers |
| Proxy/Delegation to DB | `MSG_DB*` request/response with type rewriting | `_MSG_AccountLogin.cpp:73-92` | Authoritative account data lives in DBSrv |
| Write-behind persistence | Periodic 8 s round-robin save + close-time save | `ProcessSecMinTimer.cpp:169-199` | Bounds loss window without per-action writes |
| Anti-cheat scoreboard | `NumError` accumulator + tick-window validators | `Server.cpp:684-707`; `_MSG_Attack.cpp:59-97` | Client-trust minimisation |

Architectural decisions observable in the component:

* **No encapsulation by design.** All 55 fields are `public` and there are no accessors; consumers write directly. This is characteristic of decompiled output and is the root cause of most of the coupling metrics.
* **Index-based identity over pointer identity**, which avoids dangling pointers on close but creates ABA-style slot-reuse hazards.
* **Single-threaded concurrency model**, which is why none of the direct field writes need synchronisation.
* **Fail-closed teardown** for identity mismatches (account name echo, MAC ban), versus **fail-silent** for protocol/action violations (handlers simply `return`).
* **Watcher-style supervision**: the second timer loop checks send progress, idle time and billing windows for every slot, so a session that stops behaving is closed by the supervisor rather than by the peer.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | `CUser::CloseUser` (`CUser.cpp:81-92`) | Resets only 4 of 55 fields; `Cargo`, `Coin`, `Donate`, `Slot`, `SelChar`, `Trade`, flags persist across slot reuse | Cross-session data exposure on a reused slot; correctness depends on two login paths always running |
| High | `ProcessDBMessage.cpp:1005` | `pUser[n].Admin = 1` uses the leftover loop variable `n` instead of `conn` | Wrong session is marked admin; the intended session never is (latent privilege/logging bug) |
| High | `CUser::Admin` | Written at 3 sites, **read at none**; GM authority actually derives from `MOB.CurrentScore.Level - 1000` | Field is dead weight; any reconstruction assuming it gates GM commands is wrong |
| High | `AcceptUser` (`CUser.cpp:61`) | `if (WSAAsyncSelect(...) > 0)` — the API returns `0` on success and `SOCKET_ERROR (-1)` on failure, so the branch is effectively dead and failure is not detected | A failed `WSAAsyncSelect` leaves `Mode` unchanged but the socket half-configured; the caller ignores the return value (`Server.cpp:4020`) |
| High | `CloseUser(conn)` (`Server.cpp:6885-6897`) | Early `return` after `CrackLog` when `conn`/`Slot` are out of range, *after* the socket was closed and before `Mode` is reset | Slot can be stranded in `USER_PLAY`/`USER_SAVING4QUIT` with a dead socket; `USER_SAVING4QUIT` is exempt from the idle sweep, so it can leak permanently |
| High | `ProcessDBMessage.cpp:1179-1182` | `Mode = USER_ACCEPT` immediately followed by `CloseUser` (which sets `USER_EMPTY`) | Intended "return to re-logon" behaviour is unobservable; the intermediate state is dead code |
| High | Trade commit (`_MSG_Trade.cpp:232`) | `memcmp` over `MSG_Trade` includes header fields **and struct padding** taken from the network buffer | Spurious trade aborts; a client that does not zero padding cannot complete a legitimate trade |
| Medium | `_MSG_Deposit.cpp:35` / `_MSG_Withdraw.cpp:35` | `int tcoin = coin + warehouseCoin` is computed before range checks; signed overflow is UB | A crafted amount can bypass the `<= 2e9` gate on compilers that optimise on UB assumptions |
| Medium | `CheckIdle` / `LastReceiveTime` | Timestamp refreshed before `SKIPCHECKTICK` rejection and before ID validation succeeds | A client sending only rejected packets stays "alive" indefinitely |
| Medium | `Unk_2736` cadence (`Server.cpp:4415-4417`) | Counter reset at login means the `% 450` trigger is unreachable for frequently relogging players | Slow PK-point/mount effects silently never fire for some players |
| Medium | `AddCrackError` (`Server.cpp:694`) | Threshold of 2,000,000,000 with typical penalties of 1-10, and resets on death-restart | Anti-cheat budget is practically unreachable and resettable by the attacker |
| Medium | `GetTickCount()` cooldowns (`Message`, `PotionTime`) | 32-bit wrap (~49.7 days) makes the delta negative → treated as "too soon" indefinitely | Chat/potion cooldown can become permanently locked until the next successful action |
| Medium | `_MSG_SendAutoTrade.cpp` | All validation failures `return` with no client feedback | Malformed listing looks like packet loss; no diagnosis path |
| Medium | `ProcessClientMessage.cpp:66-311` | Only `_MSG_AccountLogin` validates `m->Size`; no default case; no per-type body length check | Handlers cast short packets and read uninitialised/over-read memory |
| Medium | `USER_WAITDB` (13) | No dedicated timeout; only the general 720 s idle rule | Stranded character-selection sessions after a lost DB reply |
| Medium | `CUser.h:42-115` | Original decompiler offsets are stale: `LastAttack` is annotated 1788, which equals `Trade` (1500) + 288 and ignores `AutoTrade` entirely | Any binary-layout reconstruction based on these comments will be wrong |
| Medium | `CMob.cpp:661-662` | `Range` assigned from `EF_RANGE` then immediately overwritten with `23` | Dead code; range is always 23 regardless of equipment/skill configuration |
| Medium | `_MSG_UseItem.cpp:207-224, 3214` | `UseItemTime` cooldown block is commented out but the field is still written | Misleading half-implemented anti-spam |
| Medium | `_MSG_MessageChat.cpp:159-169` | `Chatting` toggled and never read | Dead flag; the client may believe it disabled chat |
| Low | `CUser.cpp:73` | `inet_ntoa` result stored in unused `tmp` | Dead code / warning |
| Low | `Unk2[400]`, `Unk3`, `Unk5[36]`, `Unk_2628`, `Unk_2632`, `Unk_2688`, `Unk_2708`, `Unk_2736`, `Unk9[400]`, `AttackTime`, `LastClientTick` | Unmapped or write-only fields (~1.3 KB per session) | Memory bloat (~1.3 MB across 1000 slots) and reconstruction uncertainty |
| Low | `CUser.h:33` | `USER_CREWAIT` (14) commented out | Historical state removed; no live references |
| Low | `CUser::CloseUser` return type `int` returning `TRUE` | No caller uses the result | Noise in the API |
| Low | `SendBilling` (`Server.cpp:1377`) | Stub returns TRUE, so `IsBillConnect` logic is inert | Billing-related branches are untestable/unreachable in this build |
| Low | `ProcessSecMinTimer.cpp:178` vs `_MSG_CharacterLogin.cpp:69` | Child-pay hour windows are not complementary (`<=12 \|\| >=19` vs `>7 && <23`) | Inconsistent billing policy between admission and eviction |

Static memory footprint of the component: `sizeof(CUser)` is large (the annotated layout reaches past 3,000 bytes, dominated by `SelChar` at 744 bytes and three 400/1024-byte blocks), and `pUser[1000]` plus the embedded `CPSock` buffers (128 KiB tx + 128 KiB rx each) makes this entity the single largest static allocation in TMSrv.

---

## 11. Test Coverage Analysis

### 11.1 Search method and evidence

The whole project tree was searched (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`) for test artefacts:

| Search | Result |
|--------|--------|
| Files matching `(?i)(test\|spec\|mock\|fixture\|gtest\|catch2\|assert)` | **0 matches** |
| Test framework references (`gtest`, `catch`, `test` in `.vcxproj`) | **0 matches** in `Code/TMSrv/TMSrv.vcxproj` and `Code/DBSrv/DBSrv.vcxproj` |
| Projects in `W2PP Code Project.sln` | 2 (`DBSrv`, `TMSrv`) — no test/CI project |
| Test/CI configuration (`.github`, `azure-pipelines`, `*_test.*`) | none present |
| `README.md` project status | "Things to do": `Fix all bugs`, `Create an documentation`, `Re-create the project` — no test item |

**Conclusion: there are no unit, integration, contract or end-to-end tests for `CUser`, its methods, or any of its consumers.** The component is verified only by running the server against a game client.

### 11.2 Coverage table

| Component / behaviour | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------------------|------------|-------------------|----------|--------------|
| `CUser::CUser` (14-field init) | 0 | 0 | 0% | No test; initial state is observable only via a live server |
| `CUser::~CUser` | 0 | 0 | 0% (empty body) | N/A |
| `CUser::AcceptUser` (success/failure/socket setup) | 0 | 0 | 0% | Requires a live listen socket and `hWndMain`; no harness exists |
| `CUser::CloseUser` (field reset scope) | 0 | 0 | 0% | The central risk of the component is untested |
| `Mode` state machine (16 transitions) | 0 | 0 | 0% | No transition table test, no illegal-transition test |
| Login chain (account → select → play) | 0 | 0 | 0% | Depends on a real DBSrv and client |
| Save/quit protocol (`MSG_SavingQuit` ↔ `_MSG_DBSavingQuit`) | 0 | 0 | 0% | No fake DB server harness |
| Trade settlement (`_MSG_Trade`) | 0 | 0 | 0% | Highest-value untested business logic (money/item duplication) |
| Autotrade listing/purchase | 0 | 0 | 0% | Same |
| Cargo/Coin deposit & withdraw | 0 | 0 | 0% | No boundary/overflow tests |
| Anti-cheat rate limits (`LastAttackTick`, `LastIllusionTick`, `Message`, `PotionTime`) | 0 | 0 | 0% | No clock injection; uses global `CurrentTime`/`GetTickCount` |
| Idle timeout / shutdown drain | 0 | 0 | 0% | Only reachable by waiting in a live process |
| MAC ban / OnlyTrade / MuteChat | 0 | 0 | 0% | File-driven, no fixtures |

### 11.3 Testability assessment

The component is difficult to test as written, for structural reasons rather than absence of effort:

* **Global state**: `pUser` is a file-scope global defined in `Server.cpp:269`; there is no injection point, so a test must link the whole server.
* **Win32 coupling**: `AcceptUser` requires a real `HWND` (`hWndMain`) and `WSAAsyncSelect`; `CloseUser` requires a live `DBServerSocket`. Neither is virtualised.
* **Time coupling**: rate limits read the globals `CurrentTime`, `SecCounter` and call `GetTickCount()` directly.
* **No seams**: 3 methods, all `public`, all side-effecting; no pure functions, no interfaces, no dependency inversion.
* **Hidden inputs**: `Mac.txt`, `Admin.txt`, `gameconfig.txt`, `Macro`/`Language` string tables are read from relative paths (`../../DBSRV/Run/...`), so even a linked test would fail without a directory layout.

The only observable-behaviour verification currently available is manual: connect a client, exercise login/select/play, and read the text logs (`Log`, `CrackLog`, `ChatLog`, `ItemLog`) which record `Mode`, `AccountName`, `IP` and the specific violation string.

---

## 12. Report Metadata

* **Files read for this analysis:** `Code/TMSrv/CUser.h`, `CUser.cpp`, `Server.h`, `Server.cpp`, `ProcessClientMessage.h/.cpp`, `ProcessDBMessage.h/.cpp`, `ProcessSecMinTimer.cpp`, `SendFunc.cpp`, `imple.cpp`, `CReadFiles.cpp`, `CMob.cpp`, `_MSG_AccountLogin.cpp`, `_MSG_CharacterLogin.cpp`, `_MSG_CharacterLogout.cpp`, `_MSG_CreateCharacter.cpp`, `_MSG_DeleteCharacter.cpp`, `_MSG_Trade.cpp`, `_MSG_QuitTrade.cpp`, `_MSG_SendAutoTrade.cpp`, `_MSG_ReqBuy.cpp`, `_MSG_Deposit.cpp`, `_MSG_Withdraw.cpp`, `_MSG_Attack.cpp`, `_MSG_Action.cpp`, `_MSG_UseItem.cpp`, `_MSG_MessageChat.cpp`, `_MSG_MessageWhisper.cpp`, `_MSG_Restart.cpp`, `_MSG_SetShortSkill.cpp`; `Code/CPSock.h`; `Code/Basedef.h`, `Basedef.cpp`.
* **Declaration/definition counts:** 1 class, 3 methods, 8 mode macros, 55 data members, 15 `Mode` assignment sites, 38 `CloseUser` call sites, 1,630 `pUser[...]` accesses in 61 translation units.
* **Excluded by instruction:** `Code/DBSrv/CUser.cpp` and `Code/DBSrv/CUser.h` (separate component). `.agents`, `.codegraph`, `.opencode`, `openspec` excluded from all searches.
* **No project files were modified.** This report is analysis only.
