# Component Deep Analysis Report — Whisper and GM Chat Handler

**Component name:** Whisper and GM Chat Handler
**Primary location:** `Code/TMSrv/_MSG_MessageWhisper.cpp` (35,006 bytes, 1,378 lines)
**Analysis scope:** entire project root `E:\wyd\test-setup\w2pp` (486 files inspected outside ignored folders)
**Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Project:** W2PP — decompiled C/C++ Win32 (MSVC) MMORPG server emulator; `TMSrv` is the game/world server
**Analysis date:** 2026-11-15
**Architectural role (from architectural report):** Command Handler (privileged entry); afferent 2, efferent 14; "35 KB whisper/party/guild chat plus the `/gm` command that forwards to the admin interpreter (`imple.cpp` `ProcessImple`)"

**Note on the Endpoints section:** this component exposes no REST/GraphQL/gRPC/HTTP endpoints. Its entire external surface is one client→server packet type (`_MSG_MessageWhisper`) plus a text command vocabulary carried inside that packet. Per the analysis guidelines the *Endpoints* section is therefore intentionally omitted; the equivalent information (packet entry point, command routing table) is documented in Sections 2 and 3.

---

## 1. Executive Summary

`Exec_MSG_MessageWhisper` is a single 1,359-line function (declared at `Code/TMSrv/_MSG_MessageWhisper.cpp:20`, terminated at `:1378`) that handles **one** client packet type: `_MSG_MessageWhisper` (`Code/Basedef.h:1447`, struct at `Code/Basedef.h:1448-1453`, 128 bytes on the wire). Despite the name, the packet is not a whisper-only channel. It is the server's **multi-purpose text command and chat multiplexer**: the same 128-byte frame carries 29 slash-command style operations, six chat channels (guild, guild-ally, party, kingdom, citizen/global, private), and the delegated GM command interpreter entry point.

The component's role in the system is best understood as two stacked responsibilities:

1. **A social/chat router.** The sender's `MobName` field is used as a *device selector*: an empty name means "channel message" (the first character of `String` then selects guild `-`, party `=`, kingdom `@@`, or citizen `@`), while a non-empty name means "private message to that player name" (or a slash-command keyword). Delivery is performed by direct per-recipient `CPSock::AddMessage` calls or by the broadcast helpers `SyncMulticast` (`Code/TMSrv/SendFunc.cpp:260`) and `SyncKingdomMulticast` (`Code/TMSrv/SendFunc.cpp:274`).
2. **A privileged command surface.** When `MobName` equals `"gm"`, the handler computes a privilege level as `pMob[conn].MOB.CurrentScore.Level - 1000` and forwards the raw `String` to `ProcessImple` (`Code/TMSrv/imple.cpp:46`) — the 1,846-line admin interpreter that mutates items, scores, timers, weather, billing flags, mobs, and can kick players. **No authorization check occurs inside this component**; the only gate is the arithmetic delegation of the game level, which is enforced downstream in `imple.cpp` at `:89` (level ≥ 8), `:569` (≥ 3), `:647` (≥ 2), `:1119` and `:1164` (≥ 1).

Beyond chat and GM delegation, the component also embeds a substantial **guild lifecycle subsystem** (`/create`, `/subcreate`, `/abandonar`, `/handover`, `/fimguerra`, `/fimirma`, `/expulsar`, `/gfame`), a **movement/teleport subsystem** (`/summon`, `/relo`, `/summonguild`, `/kingdom`, `/king`), a **premium broadcast path** that consumes an inventory item (`/spk`), and a set of **account/security commands** (`/pin`, `/contaprincipal`, `/srv`).

Key findings (each detailed later in this report):

- **No packet-size validation exists.** The per-message-type validator `BASE_CheckPacket` (`Code/Basedef.cpp:6475`) has its entire body commented out (`:6476-6594`) and unconditionally returns `FALSE` (`:6595`). `_PACKET_DEBUG`, which would enable the call site, is defined only for the `Debug|Win32` configuration (`Code/TMSrv/TMSrv.vcxproj:61`). The handler therefore relies on fixed offsets (`MobName[16]`, `String[100]`) regardless of the client-declared `Size`, and it **writes** terminator bytes at `:26-27` into the receive buffer.
- **Privilege is derived exclusively from character level** (`level - 1000`); the `CUser::Admin` field exists (`Code/TMSrv/CUser.h`) but is never consulted in TMSrv (only initialised in `CUser.cpp:34` and cleared in `Server.cpp:6843`). `/not` (`:1088`) is gated by the same rule and broadcasts a server-wide notice through DBSrv.
- **Guild creation is non-atomic.** `/create` deducts 100,000,000 coins and assigns the guild id at `:210-213` **before** opening the persistence file at `:222`; a failed `fopen` returns after the mutation with no rollback (`:224-228`).
- **`/pin` rate limiting is inert.** `pMob[conn].extra.DonateInfo.Count` is read at `:1059` but never incremented, and the DBSrv handler for `_MSG_DBActivatePinCode` is an empty stub (`Code/DBSrv/CFileDB.cpp:2060-2066`).
- **Silent-failure design dominates.** A large share of the command branches return without any client feedback on validation failure (e.g. `:59`, `:62`, `:65`, `:84`, `:123`, `:181`, `:190`, `:276`, `:290`, `:299`), which is indistinguishable from a dropped packet on the client.
- **No tests of any kind exist** for this component or for the project (Section 10 documents the evidence).
- **Two logic defects are provably dead guards**: `conn <= 0 && conn >= MAX_USER` (`:91` inside `imple.cpp`, `:1832`) and the `myleader == 0` sub-expressions at `:706` and `:847`, which collapse the "party or guild" summons rule into "same party only".
- **A mask bug** at `:954` and `:1011` (`myguild & MAX_GUILD` instead of `MAX_GUILD - 1`) makes `/fimguerra` and `/fimirma` validate the guild *name* of guild index 0 rather than the calling guild's name.

---

## 2. Data Flow Analysis

### 2.1 Entry point contract

The packet is delivered by the Win32 message pump → `CPSock::Receive` (`Code/CPSock.cpp:336`) → `CPSock::ReadMessage` (`Code/CPSock.cpp:353`), which validates only framing: message init code (`INITCODE`, `CPSock.h:40`), `Size` between `sizeof(HEADER)` (12) and `MAX_MESSAGE_SIZE` (8192, `CPSock.h:38`), and a rolling checksum that is *not* enforced (a mismatched checksum still returns the packet, `CPSock.cpp:457-464`). `ProcessClientMessage` (`Code/TMSrv/ProcessClientMessage.cpp:38`) then applies three global guards — `std->ID` range `[0, MAX_USER)`, `ServerDown < 120`, and the anti-replay sentinel `std->ClientTick == SKIPCHECKTICK` — and dispatches `case _MSG_MessageWhisper:` at `:164-166`.

```
1. Client sends MSG_MessageWhisper {Size, KeyWord, CheckSum, Type=52, ID, ClientTick, MobName[16], String[100]}
2. CPSock::ReadMessage       -> framing + checksum (checksum mismatch tolerated)          [CPSock.cpp:353]
3. ProcessClientMessage      -> ID range / ServerDown / ClientTick gate, switch(Type)    [ProcessClientMessage.cpp:42-66,164]
4. Exec_MSG_MessageWhisper   -> force NUL at MobName[15] and String[99]                   [_MSG_MessageWhisper.cpp:26-27]
5. Session gate              -> pUser[conn].Mode != USER_PLAY ? silent return             [:29-30]
6. Command chain A           -> /cp /getout /srv /gfame /spk /qst /create /subcreate      [:33-508]
                                /abandonar /handover /nt /nig
7. Command chain B           -> /tab                                                      [:510-559]
8. Command chain C           -> /snd                                                      [:562-571]
9. Command chain D           -> /day                                                      [:574-579]
10. Command chain E          -> /kingdom /king /summonguild /summon /time /gm /relo        [:582-908]
                                /expulsar /fimguerra /fimirma /pin /not /wp /contaprincipal
11. Global mute gate         -> pUser[conn].MuteChat == 1 ? refuse all remaining chat      [:1129-1133]
12. Channel/PM demultiplex    ->  MobName[0] == 0 ? channels : private message            [:1136, :1284]
13. Channel branch            -> guild '-' | party '=' | kingdom '@@' | citizen '@'        [:1139,1185,1229,1256]
14. Private branch            -> resolve /r alias -> GetUserByName -> privacy -> relay     [:1291-1374]
15. Audit                    -> Log() for commands, ChatLog() for chat, ChatLog for PM     [:variable]
```

### 2.2 Flow: GM command delegation (`/gm`)

```
1. Client: MobName="gm", String="/<command> <args>"                     (the leading '/' is part of String)
2. Match at :787 (accepts "gm" and "GM")
3. level = pMob[conn].MOB.CurrentScore.Level - 1000                     [:789]
4. ProcessImple(conn, level, m->String)                                 [:791]
5. ProcessImple: Log("adm <string>", account, IP)                        [imple.cpp:80-87]
6. ProcessImple: sscanf(str + 1, "%s %s %s %s %s %s %s %s %s", ...)      [imple.cpp:68]   <- first char discarded
7. Tiered execution: level>=8 [imple.cpp:89] -> >=3 [:569] -> >=2 [:647] -> >=1 [:1119], [:1164]
8. Effects: direct mutation of pMob/pUser arrays, GridMulticast, SendClientMessage, DBServerSocket.SendOneMessage
9. Response: SendClientMessage(conn, "set level"/"SAVEALL"/...) — zero responses when the level tier does not match
```
Second caller (not reachable from the client package path): `ProcessDBMessage` `case _MSG_MessageDBImple:` (`Code/TMSrv/ProcessDBMessage.cpp:123-134`) invokes `ProcessImple(0, m->Level, m->String)` with `conn = 0`, i.e. privileged commands issued through the DB/admin channel.

### 2.3 Flow: guild chat (`String[0] == '-'`)

```
1. MobName is empty; sender's name copied into MobName                        [:1141]
2. m->String[96] = 3 (client-side channel marker byte)                        [:1143]
3. Guild == 0 ? _NN_Only_Guild_Member_Can and stop                            [:1145-1151]
4. Loop i = 1..MAX_USER-1                                                     [:1155]
     skip not USER_PLAY | skip self | skip Guildchat != 0
     same-guild OR (second char == '-' AND recipient guild == g_pGuildAlly[guild] != 0)
5. m->ID = conn; pUser[i].cSock.AddMessage(m, sizeof(MSG_MessageWhisper))      [:1172-1173]
6. BASE_GetGuildName(ServerGroup, guild, name); ChatLog("chat_guild, ...")     [:1176-1180]
```
Recipients are queued, not flushed: `AddMessage` only appends to each socket's send buffer (`Code/CPSock.cpp:513-591`); transmission happens on the recipient's own send cycle.

### 2.4 Flow: party chat (`String[0] == '='`)

```
1. MobName := sender name                                                     [:1187]
2. myleader = pMob[conn].Leader, or conn when <= 0                             [:1189-1192]
3. Reject when leader out of range or not USER_PLAY                            [:1194-1198]
4. Deliver to leader first (when leader != conn) — no PartyChat filter         [:1200-1201]
5. Loop PartyList[0..MAX_PARTY-1]: skip <=0, >=MAX_USER, self, not USER_PLAY,
   and pUser[partyconn].PartyChat != 0                                         [:1203-1217]
6. m->ID = conn; AddMessage to each; ChatLog("chat_party, ...")                [:1219-1224]
```

### 2.5 Flow: kingdom chat (`String[0..1] == "@@"`)

```
1. Rate check against pUser[conn].Message (GetTickCount, 3000 ms), stamp       [:1231-1243]
2. MobName := sender name                                                      [:1245]
3. SyncKingdomMulticast(conn, pMob[conn].MOB.Clan, m, 0)                       [:1247]
     -> all USER_PLAY with MOB.Clan == sender Clan, except sender,
        excluding pUser[i].KingChat != 0                                       [SendFunc.cpp:274-286]
4. ChatLog("chat_kingdom, %s : %s reino:%d")                                   [:1249-1250]
```
The fan-out is **local to the current TMSrv process**; no DB message is emitted, so kingdom chat does not cross game servers.

### 2.6 Flow: citizen/global chat (`String[0] == '@'`)

```
1. Same shared 3-second rate bucket                                            [:1258-1270]
2. SyncMulticast(conn, m, 0) -> every USER_PLAY except sender                  [:1274] [SendFunc.cpp:260-272]
3. ChatLog("chat_cidadao, %s : %s")                                            [:1276-1277]
```

### 2.7 Flow: private message

```
1. MobName non-empty                                                            [:1284]
2. Force NUL at MobName[15] and MobName[14]                                     [:1286-1287]
3. Reply alias test (/r, language _NN_Reply, "ñº", "¦õ")                        [:1291-1304]
     LastChat empty -> _NN_No_One_To_Reply; else MobName := pUser[conn].LastChat
4. target = GetUserByName(MobName)   ("+" prefix = account-name lookup)         [:1306] [Server.cpp:6793]
5. target == 0 -> _NN_Not_Connected;  target not USER_PLAY -> _NN_Not_Connected  [:1308-1318]
6. pUser[target].Whisper != 0 AND sender Level < 1000 -> _NN_Deny_Whisper       [:1320-1324]
7. m->ID = target; pUser[conn].LastChat := target name                          [:1325-1327]
8. String empty -> profile card (citizen, fame, guild name, away message)       [:1329-1360]
9. String[0] in { '-', '=' } -> replaced with ' '                               [:1362-1363]
10. String[96] = 0; MobName := sender name; pUser[target].LastChat := sender     [:1365-1368]
11. pUser[target].cSock.AddMessage(m, sizeof(MSG_MessageWhisper))               [:1370]
12. ChatLog("chat_sms,<sender> <recipient> : <text>")                           [:1372-1373]
```

### 2.8 Flow: magic trumpet (`/spk`)

```
1. MuteChat check                                                               [:100-104]
2. Scan Carry[0..MaxCarry-1] (MaxCarry = 30) for sIndex == 3330                  [:106-110] [ProcessDBMessage.cpp:751]
3. Consume 1 (decrement amount, or clear slot); SendItem refresh                [:113-118]
4. Not found -> silent return                                                   [:122-123]
5. "[MobName]> text" built in temp[4096], strncpy into String[96], marker String[94]=1  [:132-136]
6. MSG_MagicTrumpet (ID=0) -> DBServerSocket.SendOneMessage                     [:138]
7. DBSrv re-fans out to every connected TMSrv and every admin client            [CFileDB.cpp:1591-1610]
8. Each TMSrv receives _MSG_MagicTrumpet -> SyncMulticast(0,...) locally        [ProcessDBMessage.cpp:136-139]
9. ChatLog("chat_spk,...")                                                      [:140-141]
```

### 2.9 Flow: guild management commands (representative: `/create`)

```
1. String non-empty                                                             [:170]
2. Coin >= 100,000,000 (else _DN_D_Cost)                                        [:173-178]
3. MOB.Guild == 0                                                               [:180]
4. Clan in {7,8} (else _NN_GUILDCREATECLAN)                                     [:183-187]
5. GuildCounter < 4096 (MAX_GUILD)                                              [:189]
6. extra.Citizen != 0 (else _DN_NO_TOWNSPEOPLE)                                 [:192-196]
7. ServerIndex != -1 ; GuildCounter != 0                                        [:198-208]
8. MOB.Guild = ServerIndex * 4096 + GuildCounter ; GuildLevel = 9               [:210-211]
9. MOB.Coin -= 100,000,000 ; SendEtc                                            [:213-214]
10. Append "Group ServerIndex GuildCounter Name" to ../../Common/Guilds.txt      [:222-232]
11. GuildInfo[guild] = {Clan, Fame=0, Citizen}; MSG_GuildInfo -> DB              [:247-254]
12. Log(sys,guild medal value...); GuildCounter++; WriteGuild(); InitializeGuildName(); success message  [:256-268]
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation | Only sessions in `USER_PLAY` may issue any command or chat; otherwise silent return | `_MSG_MessageWhisper.cpp:29-30` |
| BR-02 | Validation | `MobName` truncated to 15 bytes, `String` to 99 bytes (forced NUL), independent of declared packet size | `:26-27` |
| BR-03 | Protocol | `MobName` is a device selector: empty = channel, non-empty = command keyword or PM recipient | `:1136`, `:1284` |
| BR-04 | Business Logic | `/cp` reports PK points as `KILL_MARK effect - 75` | `:33-38` |
| BR-05 | Business Logic | `/getout` sets `extra.Citizen = 0` (irreversible renunciation of townsperson status) | `:41-49` |
| BR-06 | Business Logic | `/srv` requests a cross-server character transfer via DBSrv | `:52-78` |
| BR-07 | Authorization | `/gfame` requires a guild and `GuildLevel == 9` | `:81-93` |
| BR-08 | Business Logic | `/spk` global broadcast consumes exactly one item 3330 from inventory | `:96-143` |
| BR-09 | Business Logic | `/qst` maps Arch quest crystal stage 0..4 to a fixed contract name | `:146-165` |
| BR-10 | Business Logic | Guild creation: 100,000,000 coins, clan 7/8, citizenship, free guild counter, name from payload | `:168-270` |
| BR-11 | Business Logic | Sub-leader appointment costs 100,000,000 coins and fills Sub1/Sub2/Sub3 in order (levels 6/7/8) | `:273-365` |
| BR-12 | Business Logic | `/abandonar` clears guild membership; sub-leaders release their Sub slot | `:368-414` |
| BR-13 | Authorization | `/handover` requires `GuildLevel == 9` and a same-guild connected target | `:417-481` |
| BR-14 | Business Logic | `/nt`, `/nig`, `/time`, `/day` return counters/clocks to the caller only | `:484-507`, `:770-784`, `:574-579` |
| BR-15 | Validation | `/tab` requires level ≥ 69 or non-MORTAL class; stores 26-byte personal tab and refreshes perception/PK state | `:510-559` |
| BR-16 | Business Logic | `/snd` stores a 96-byte away message shown to whisperers | `:562-571` |
| BR-17 | Business Logic | `/kingdom` and `/king` teleport by clan (7/8) to fixed coordinates | `:582-608` |
| BR-18 | Business Logic | `/summonguild` requires `GuildLevel > 0`, a valid village tile, and a non-blocked map attribute | `:611-645` |
| BR-19 | Business Logic | `/summon` enforces liveness, party relation, ARCH class, skill bit `0x40000`, map attribute, level cap `level+special+30`, trade state | `:648-767` |
| BR-20 | Business Logic | `/relo` teleports the caster to the target using medal-based or skill-based summon mode and a level cap | `:796-908` |
| BR-21 | Business Logic | `/expulsar` delegates guild expulsion to `DoDeprivate` (rank-based authority) | `:911-927`, `Server.cpp:7755` |
| BR-22 | Business Logic | `/fimguerra` and `/fimirma` end war/alliance only for named guilds holding a war item | `:930-1041` |
| BR-23 | Security | `/pin` accepts only 36-character codes and applies a 2-hour window with a 3-attempt cap | `:1044-1085` |
| BR-24 | Authorization | `/not` server-wide notice requires character level ≥ 1000 (asserted inside the branch condition) | `:1088-1101` |
| BR-25 | Business Logic | `/wp` reports donation balance; `/contaprincipal` promotes the account to primary | `:1104-1127` |
| BR-26 | Authorization | `/gm` derives privilege as `Level - 1000` and delegates to `ProcessImple` without a local check | `:787-793` |
| BR-27 | Validation | `MuteChat == 1` blocks guild/party/kingdom/citizen/PM but not the command chain above it | `:1129-1133` |
| BR-28 | Routing | Guild chat reaches same-guild `USER_PLAY` members who have not disabled guild chat | `:1139-1182` |
| BR-29 | Routing | Guild chat prefixed `--` also reaches the registered ally guild | `:1163-1164` |
| BR-30 | Routing | Party chat reaches the party leader first, then the `PartyList`, honouring `PartyChat` opt-out (leader exempt) | `:1185-1226` |
| BR-31 | Rate Limit | `@@` kingdom chat is limited to one message per 3,000 ms per session | `:1229-1253` |
| BR-32 | Rate Limit | `@` citizen chat shares the same 3,000 ms bucket and reaches all `USER_PLAY` locally | `:1256-1279` |
| BR-33 | Business Logic | PM target resolution by character name or `+ACCOUNTNAME`; `/r` and localized aliases reply to `LastChat` | `:1291-1331` |
| BR-34 | Privacy | Recipient `Whisper` flag blocks PMs unless the sender is level ≥ 1000 | `:1320-1324` |
| BR-35 | Business Logic | Empty PM body is a profile query (citizen, fame, guild name, away message) and does not deliver a message | `:1329-1360` |
| BR-36 | Validation | PM body with leading `-`/`=` is rewritten to a space; body truncated to 96 bytes; sender name stamped | `:1362-1368` |
| BR-37 | Audit | Every accepted command and chat message is written to the log/chat-log sinks | `:46-47`, `:140`, `:259`, `:344`, `:1179`, `:1223`, `:1249`, `:1276`, `:1372` |
| BR-38 | Configuration | Channel opt-out flags are owned by a different component (`Exec_MSG_MessageChat`) | `_MSG_MessageChat.cpp:111-169` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Session-state gate

**Overview:**
Every code path in the component is reachable only for a connection whose `pUser[conn].Mode` equals `USER_PLAY` (22). The check happens after two field truncations and immediately after reading `pMob[conn].MOB.Clan` into a local.

**Detailed description:**
The gate at `_MSG_MessageWhisper.cpp:29-30` is the component's coarse-grained admission control. Because the whisper packet can be sent by a client that is still on the character-selection screen (`USER_SELCHAR`, `USER_CHARWAIT`, `USER_LOGIN`), the check prevents the command vocabulary from being exercised against a `pMob` slot that has not yet been populated by `_MSG_CNFCharacterLogin`. In practice this is the only structural defense separating character-selection state from the chat and GM surface, since the packet dispatcher (`ProcessClientMessage.cpp:66`) performs no per-type state validation.

The check is *silent*: it returns without a message, and without an audit record. A client whose session is degraded therefore receives no diagnostic, which is a deliberate (and repeated) pattern throughout this file — most branches behave the same way. Because the check occurs at the top of the function, all 38 rules below inherit it; no later rule can be reached from a non-playing session.

One nuance matters for assessment: `int Clan = pMob[conn].MOB.Clan;` is read at `:24`, i.e. *before* the session gate. For a connection in `USER_EMPTY`/`USER_ACCEPT` the `pMob` slot exists (it is a fixed global array `CMob pMob[MAX_MOB]`), so the read is memory-safe, but it does mean state from a previous session in the same slot can be captured into `Clan` and later used by `/kingdom` (`:584`) and `/king` (`:599`) before any validation of currency of that state.

**Rule workflow:**
```
receive packet -> truncate fields -> read MOB.Clan
  -> Mode != USER_PLAY -> return (no reply, no log)
  -> Mode == USER_PLAY -> continue to command chain
```

---

### Business Rule: BR-02 — Fixed-width input truncation

**Overview:**
Two unconditional writes terminate the two payload fields: `m->MobName[NAME_LENGTH - 1] = 0` and `m->String[MESSAGEWHISPER_LENGTH - 1] = 0` (`:26-27`), i.e. byte 15 and byte 99 of the message struct.

**Detailed description:**
The rule exists to guarantee C-string safety for the subsequent `strcmp`/`strlen`/`%s` usage. `NAME_LENGTH` is 16 and `MESSAGEWHISPER_LENGTH` is 100 (`Code/Basedef.h:132`, `:130`), matching the wire struct declared at `Code/Basedef.h:1448-1453`. The writes are performed on the pointer returned by `CPSock::ReadMessage`, which points *into* the receiving socket's 128 KB buffer (`CPSock.h:35`), not into a private copy. The handler therefore mutates the shared receive buffer in place.

Because `BASE_CheckPacket` is a stub (`Code/Basedef.cpp:6475-6595`) and the dispatcher never compares `std->Size` with `sizeof(MSG_MessageWhisper)`, a client may declare `Size = 12` and still reach this code. In that case bytes 15 and 99 lie beyond the logical message but inside `pRecvBuffer`; the writes then corrupt the beginning of the *next* queued packet(s) in the same socket buffer. No heap boundary is crossed (the buffer is 128 KB), so the consequence is message-stream corruption and mis-parsing rather than an out-of-bounds heap write.

Secondary truncation rules are applied inconsistently later in the file: several name-bearing commands re-zero `String[NAME_LENGTH - 1]` and `String[NAME_LENGTH - 2]` (`:425-426`, `:664-665`, `:819-820`, `:916-917`, `:1286-1287`), i.e. they apply a 16-byte domain limit to a 100-byte buffer. For `/summon` and `/relo` this conveniently limits target names to 14 characters (matching the client's name width) but it also discards 85 usable bytes of the payload, and the choice of constant does not express the actual intent.

**Rule workflow:**
```
ReadMessage -> pointer into pRecvBuffer (Size may be 12..8192)
  -> MobName[15] = 0 ; String[99] = 0   (write into receive buffer)
  -> later per-command truncations may lower String to index 14
```

---

### Business Rule: BR-03 — Command vocabulary and dispatch precedence

**Overview:**
The component implements a 29-entry command table as a hand-written `if / else if` chain split across five independent chains, evaluated in source order, first match wins.

**Detailed description:**
Chains and their contents: chain A (`:33-508`) covers `cp`, `getout`, `srv`, `gfame`, `spk`, `qst`, `create`, `subcreate`, `abandonar`, `handover`, `nt`, `nig`; chain B (`:510`) `tab`; chain C (`:562`) `snd`; chain D (`:574`) `day`; chain E (`:582-1127`) `kingdom`, `king`, `summonguild`, `summon`, `time`, `gm`/`GM`, `relo`, `expulsar`, `fimguerra`, `fimirma`, `pin`, `not`, `wp`, `contaprincipal`. Chains B–E are separate `if` statements rather than `else if`, which is benign only because every branch in the preceding chain ends in `return`.

Because the match is on `MobName` and nothing else, the vocabulary is not namespaced: a character literally named `cp`, `srv`, `time`, `gm`, `not`, … becomes permanently unreachable by private message, and conversely a player cannot whisper a player whose name collides with a command word. `GetUserByName` (`Server.cpp:6793`) resolves by exact `strcmp` on `MOB.MobName`; there is no escape syntax in this handler.

The keyword comparisons are partly hard-coded and partly localized: `/kingdom`, `/king`, `/summonguild`, `/summon`, `/relo`, and the `/r` alias compare against `g_pMessageStringTable[_NN_…]` entries (`:582`, `:597`, `:611`, `:648`, `:796`, `:1291`) loaded from the 2,000 × 128-byte catalog (`Code/Basedef.cpp:742`). A client built with a different message table therefore loses those aliases while retaining the English ones. `/kingdom` additionally accepts the literal `"kingdom"`, `/king` accepts `"king"`, `/summon` accepts `"summon"`, `/relo` accepts `"relo"`, `/summonguild` accepts `"summonguild"`.

**Rule workflow:**
```
if MobName == "cp" ... else if ... (chain A, all branches return)
if MobName == "tab" ...        (chain B)
if MobName == "snd" ...        (chain C)
if MobName == "day" ...        (chain D)
if MobName == <kingdom aliases> ... else if ... (chain E, all branches return)
mute gate -> channel demux (MobName empty) or PM resolution (MobName non-empty)
```

---

### Business Rule: BR-04 — PK point report (`/cp`)

**Overview:**
`/cp` returns the caller's current PK penalty counter, displayed as `GetPKPoint(conn) - 75`, formatted with message id `_DN_Show_Chao` (363).

**Detailed description:**
`GetPKPoint` (`Code/TMSrv/GetFunc.cpp:2105-2112`) reads `pMob[conn].MOB.Carry[KILL_MARK].stEffect[0].cEffect` where `KILL_MARK` is 63 (`Code/Basedef.h:166`). The stored byte is 0..255 and the display subtracts a fixed 75-point offset, so the reported value is negative for characters with fewer than 75 points. There is no clamping in this component; whether the negative value is meaningful depends on the message-table format string and the client.

The branch has no level, guild, map, combat, or cost precondition, and it is evaluated *before* the mute gate at `:1129`. A muted, dead, trading, or recently-teleported character can query it freely, and there is no rate limit, so the command is a cheap, unlimited read primitive on a game-state field.

This is the first of the "diagnostic query" family (`/cp`, `/nt`, `/nig`, `/time`, `/wp`, `/qst`, `/gfame`) that mutate nothing and reply only to the caller via `SendClientMessage` (`Code/TMSrv/SendFunc.cpp:27`), which itself truncates to 96 bytes and forces the last two bytes to zero (`:39-42`).

**Rule workflow:**
```
MobName == "cp" -> GetPKPoint(conn) -> temp = format(363, value-75) -> SendClientMessage(conn, temp) -> return
```

---

### Business Rule: BR-05 — Citizenship renunciation (`/getout`)

**Overview:**
`/getout` sets `pMob[conn].extra.Citizen = 0`, tells the player they are no longer a townsperson (`_DN_NO_TOWNSPEOPLE`, 513), and writes an audit line `etc,getout <value>`.

**Detailed description:**
`extra.Citizen` is a `char` inside `STRUCT_MOBEXTRA` (`Code/Basedef.h:486`) and is persisted through the DB character record; it is set from `GuildInfo[mob.Guild].Citizen` on login when the character is guilded (`ProcessDBMessage.cpp:744-745`). Setting it to zero here has immediate downstream consequences in the same file: `/create` refuses to run with `Citizen == 0` (`:192-196`), so a player who runs `/getout` can no longer found a guild until the flag is re-acquired elsewhere.

The command performs no confirmation, no cooldown, no cost, no health check, and no "are you sure" handshake; one packet is sufficient. It is placed at the very top of the command chain, before the mute gate, so even a muted player can invoke it. Logging is by `Log()` with account name and IP, preserving accountability, but there is no reverse operation in this component.

Note the message chosen for the reply is the *failure* string of the guild-creation flow (`_DN_NO_TOWNSPEOPLE`), reused as a confirmation. The semantic overlap suggests the string was recycled rather than authored for this path, and it means the client shows a "you are not a townsperson" style message as the success acknowledgement.

**Rule workflow:**
```
MobName == "getout" -> extra.Citizen = 0 -> SendClientMessage(513)
  -> Log("etc,getout 0", account, ip) -> return
```

---

### Business Rule: BR-06 — Cross-server transfer (`/srv`)

**Overview:**
`/srv <n>` asks DBSrv to move the character to game server index `n`, subject to four range rules.

**Detailed description:**
The parsing is `sscanf(m->String, "%d", &srv)` (`:56`); a non-numeric payload leaves `srv = 0` and is rejected by the first rule. The accepted domain is `1 <= srv < MAX_SERVER` (10, `Basedef.h:48`), `srv != ServerIndex + 1` (must not be the current server, since `ServerIndex` is 0-based while `srv` is 1-based), and `srv <= NumServerInGroup` (`Server.cpp:353`, clamped to 1..10 at `:3575-3582`). On acceptance the handler builds `MSG_DBServerChange{Size, Type, ID = conn, NewServerID, Slot}` and hands it to `DBServerSocket` (`:67-77`).

DBSrv re-validates the range (`Code/DBSrv/CFileDB.cpp:1942-1947`) and then performs the credential hand-off: it reads the stored encrypted password (`GetEncPassword`), and either reports the failure code (`_MSG_DBServerSend1`, `:1956-1966`) or writes the destination server into the password blob, stores a temp key, and returns `_MSG_DBCNFServerChange` with a `"*srv enc0..enc7 slot"` string the client can use to re-login on the target server (`:1968-1988`).

The security-relevant property is that **any** playing character can initiate a transfer, with no level, guild, combat, or immunity precondition and no cost; the only barrier is the server-group topology. The `m->Slot` value is taken from `pUser[conn].Slot`, i.e. server-side state, so the client cannot forge it. Because no acknowledgement is awaited by TMSrv, a failed persistence silently strands the account key at the destination; recovery relies on the client re-login path.

**Rule workflow:**
```
parse int -> 0/out-of-range/current/> group size  -> silent return
  -> MSG_DBServerChange to DBSrv -> DB re-validates range
     -> error: _MSG_DBServerSend1 (error code) | ok: _MSG_DBCNFServerChange (*srv enc... slot)
```

---

### Business Rule: BR-07 — Guild fame query (`/gfame`)

**Overview:**
`/gfame` returns `GuildInfo[guild].Fame` as plain text, but only to a guild leader (`Guild != 0 && GuildLevel == 9`).

**Detailed description:**
`STRUCT_GUILDINFO` (`Code/Basedef.h`, definition preceding `STRUCT_RANKING`) holds `short Fame`, `char Clan`, `char Citizen`, and the three sub-leader flags `Sub1..Sub3`. The record array is `GuildInfo[65536]` (`ProcessClientMessage.h` extern) and is refreshed wholesale by DBSrv through `_MSG_GuildInfo` (`ProcessDBMessage.cpp:100-105`).

The authorization rule is exact: a guild member with `GuildLevel` 6, 7, or 8 (sub-leader) and ordinary members with 0 are silently rejected, as are unguilded characters. The rejection is silent rather than explanatory, consistent with the "no feedback" pattern, so a sub-leader cannot tell whether the command exists.

Two implementation details reduce the rule's value: the local `Server` variable computed at `:87` (`MOB.Guild / MAX_GUILD`) is never used, and the reply is written with `sprintf(temp, "%d", ...)` into the shared 4 KB global `temp` (`Server.h:382`, `Server.cpp:277`) rather than a local buffer, which is the shared-mutable-state pattern used throughout the file.

**Rule workflow:**
```
MobName == "gfame" -> Guild == 0 or GuildLevel != 9 -> silent return
  -> temp = decimal(GuildInfo[guild].Fame) -> SendClientMessage -> return
```

---

### Business Rule: BR-08 — Magic trumpet global broadcast (`/spk`)

**Overview:**
`/spk <text>` consumes exactly one inventory item with index 3330 and broadcasts `"[Name]> text"` to every player on every game server in the group, plus every connected admin client.

**Detailed description:**
The cost model is inventory-based: the handler scans `MOB.Carry[0 .. MaxCarry-1]` (`MaxCarry` is fixed at 30 across the codebase, e.g. `ProcessDBMessage.cpp:751`) for `sIndex == 3330`, and consumes one unit — decrementing via `BASE_SetItemAmount` when the stack is > 1, otherwise `BASE_ClearItem` — then re-sends the slot with `SendItem(conn, ITEM_PLACE_CARRY, i, …)` (`:106-119`). If no such item exists, the branch returns silently at `:122-123`; the player receives no "you need a trumpet" message.

Before the item scan, `MuteChat == 1` is rejected with `_NN_No_Speak` (353) (`:100-104`), making `/spk` the only command with its own mute check — necessary because it sits above the global mute gate at `:1129`. The broadcast text is composed in the global `temp[4096]` as `"[%s]> %s"`, copied with `strncpy(…, 96)`, and given a marker byte `String[94] = 1` (`:132-136`); note that `String` is only 96 bytes (`MESSAGE_LENGTH`), so the marker overwrites text position 94 when the composed string is longer, i.e. long names plus long text silently lose their last two characters.

Transport is store-and-forward through the DB: TMSrv sends `_MSG_MagicTrumpet` with `ID = 0` to `DBServerSocket` (`:138`), DBSrv re-broadcasts the same struct to all `MAX_SERVER` game servers and all `MAX_ADMIN` admin clients (`CFileDB.cpp:1591-1610`), and each TMSrv converts it into a local `SyncMulticast(0, …)` (`ProcessDBMessage.cpp:136-139`), which reaches every `USER_PLAY` session including the original sender. The memset at `:126` uses `sizeof(MSG_STANDARDPARM)` (16) against an `MSG_MagicTrumpet` (108) — a size mismatch that is harmless only because the subsequent `strncpy` with `n = 96` pads the whole `String` array.

**Rule workflow:**
```
MuteChat -> refuse(353)
scan Carry for 3330 -> not found: silent return
  consume 1 / clear stack -> SendItem refresh
  temp = "[Name]> text" -> strncpy String[96] -> String[94]=1
  MSG_MagicTrumpet(ID=0) -> DBSrv -> fan-out all TMSrv + admin clients
  each TMSrv -> SyncMulticast(0) -> all USER_PLAY
  ChatLog("chat_spk,...")
```

---

### Business Rule: BR-09 — Quest crystal report (`/qst`)

**Overview:**
`/qst` maps `extra.QuestInfo.Arch.Cristal` (0..4) to either "no quest" (`_NN_NoHave_Quest`, 384) or "playing quest" (`_DN_Play_Quest`, 383) with a fixed contract name.

**Detailed description:**
The mapping is hard-coded Portuguese content: 1 = "Contrato de Elime", 2 = "Contrato de Sylphed", 3 = "Contrato de Thelion", 4 = "Contrato de Noas". The underlying field is `char Cristal` inside the Arch quest sub-record (`Code/Basedef.h:514`) and is advanced by the Arch quest line elsewhere in the project.

The branch is an `if / else if` ladder with **no terminal `else`** (`:148-162`) while still calling `SendClientMessage(conn, temp)` unconditionally at `:163`. For `Cristal` values of 5 or greater the global `temp` retains whatever the previous request in the same process left there, so the client receives stale text from an unrelated earlier call (for example another player's chat line, another player's guild name, or an admin command echo). The field is a `char` with values written by quest code, so out-of-domain values require corrupted or third-party-modified character data — the defect is latent but real.

Like the other read-only queries, the rule has no cost, cooldown, or level precondition, and because it precedes the mute gate it is available to muted characters.

**Rule workflow:**
```
MobName == "qst" -> switch Cristal 0 -> 384 ; 1..4 -> 383 + contract name
  (no default) -> SendClientMessage(temp) -> return
```

---

### Business Rule: BR-10 — Guild creation (`/create`)

**Overview:**
`/create <name>` founds a guild for 100,000,000 coins, requires a kingdom clan (7 or 8), citizenship, no existing guild, and a free guild-counter slot, and persists both the name record and the counter.

**Detailed description:**
The eight preconditions are applied in a fixed order (`:170-208`) with three different failure behaviours: `_DN_D_Cost` for insufficient coin (`:175`), `_NN_GUILDCREATECLAN` (535) for a wrong clan (`:185`), `_DN_NO_TOWNSPEOPLE` (513) for a non-citizen (`:194`), and *silence* for an empty name, an existing guild, a full guild table, an undefined `ServerIndex`, and a zero guild counter. The zero-counter case is reachable in practice: `CReadFiles::ReadGuild` sets `GuildCounter = 0` before opening the file and only logs an error when it stays zero (`CReadFiles.cpp:774`, `:823-824`), while the process-level initialiser is 1 (`Server.cpp:53`).

The guild identity is the packed value `ServerIndex * MAX_GUILD + GuildCounter` (`:210`) with `GuildLevel = 9`, i.e. server index in the high bits and a per-server 12-bit counter in the low bits; `BASE_GetGuildName` (`Basedef.cpp:1314-1353`) decomposes it the same way. `GuildInfo[guild]` is initialised with the founder's clan, `Fame = 0` and `Citizen`, and pushed to DBSrv with `_MSG_GuildInfo` (`:247-254`).

The persistence step is a **two-file, non-atomic** operation with the mutation ordered before it: coins are deducted at `:213`, the guild id is assigned at `:210`, and only then is `../../Common/Guilds.txt` opened for append (`:222`). If `fopen` fails, the function logs and returns (`:224-228`) leaving the character guilded, 100 M coins poorer, and `GuildCounter` un-incremented — so the next founder receives the same guild id. On success the counter is incremented, `CReadFiles::WriteGuild()` rewrites `../../Common/Guild_<group>_<server>.txt` (`CReadFiles.cpp:827`, path template at `:42`), and `BASE_InitializeGuildName()` (`Basedef.cpp:6153`) re-derives the in-memory name table. The guild name is truncated to `GUILDNAME_LENGTH` (12) at `:220`.

**Rule workflow:**
```
parse name -> empty: return
  coin < 100M -> 204 ; Guild != 0 -> return ; Clan not in {7,8} -> 535
  GuildCounter >= 4096 -> return ; Citizen == 0 -> 513
  ServerIndex == -1 -> log+return ; GuildCounter == 0 -> log+return
  Guild = ServerIndex*4096 + GuildCounter ; GuildLevel = 9 ; Coin -= 100M ; SendEtc
  append Guilds.txt -> fopen fail: log+return (state already mutated)
  GuildInfo init -> MSG_GuildInfo to DB -> log -> GuildCounter++ -> WriteGuild -> InitializeGuildName -> 421
```

---

### Business Rule: BR-11 — Sub-leader appointment (`/subcreate`)

**Overview:**
`/subcreate <member> <logname>` promotes a guild member to one of three sub-leader ranks (levels 6, 7, 8) for 100,000,000 coins, filling `Sub1`, `Sub2`, `Sub3` in order.

**Detailed description:**
The rule set is the densest authorization cluster in the file: sender must be guilded (`:275`), the named target must resolve and must not be the sender (`:287-290`), the target must be connected (`_NN_Not_Connected` if not, `:292-296`), the target must be in the same guild (silent, `:298`), the sender must be `GuildLevel == 9` (`_NN_Only_Guild_Master_can`, 24, `:301-305`), and the target must currently be `GuildLevel == 0` (`_NN_Haven_Guild_Medal`, 147, `:307-311`). A fourth slot is impossible: if all three flags are set the command returns silently (`:315-316`).

On success the cost is deducted (`:326`) and the promoter assigns level 6, 7, or 8 based on the first free flag, setting the corresponding `GuildInfo[guild].SubN = 1` (`:328-342`). The result is pushed to DBSrv as `_MSG_GuildInfo` and both players are re-rendered via `GetCreateMob` + `GridMulticast` (`:347-363`) so the new guild rank badge appears without relogging. The audit line is `etc,subcreate <logname> <guild>`.

Implementation weaknesses worth recording: the parse uses `sscanf(m->String, "%s %s", SubMobName, SubName)` into 256-byte stacks (`:278-281`) fed by a 99-byte payload (safe), but `strncpy(sub, SubMobName, 16)` at `:285` does not guarantee termination for a 16+ character token, and `GetUserByName` then `strcmp`s that buffer. The second token `SubName` is never validated or used except in the log line, so the client controls an arbitrary audit string.

**Rule workflow:**
```
Guild == 0 -> return
parse two tokens -> target = GetUserByName(token1)
  target == self -> return ; target == 0 -> 91 ; guild mismatch -> return
  GuildLevel != 9 -> 24 ; target GuildLevel != 0 -> 147 ; all Sub slots used -> return
  coin < 100M -> 204 ; Coin -= 100M
  assign level 6/7/8 + SubN = 1 -> log -> MSG_GuildInfo -> CreateMob multicast x2
```

---

### Business Rule: BR-12 — Guild departure (`/abandonar`)

**Overview:**
`/abandonar` clears the caller's guild membership (`Guild = 0`, `GuildLevel = 0`) and, when the caller is a sub-leader (6..8), releases their `SubN` slot and notifies DBSrv.

**Detailed description:**
The branch requires a guild (`:370`) and logs the pre-state (`etc,abandonar <guild> <level>`, `:373`). For levels 6–8 the code clears the first non-zero `Sub` flag it finds (`:378-386`) and sends the updated `_MSG_GuildInfo` to DBSrv so the persistent record follows (`:392-402`). For level 9 (leader) that block is skipped entirely, so a leader leaving the guild produces **no** DBSrv notification, no fame reset, and no clearing of the sub-leader flags.

After the conditional block the membership is unconditionally cleared and both players in view are updated via `GetCreateMob`/`GridMulticast` (`:405-412`). Consequently a leader who abandons leaves behind a guild record with `Sub1..Sub3` possibly occupied, a fame value that will be stale until the next `_MSG_GuildInfo` refresh, and no leader — with the only in-component path to repair the missing leadership being `/handover` before departing.

The command has no cost, cooldown, confirmation, or penalty, and it is reachable by any guilded player above the mute gate. Because it precedes the chat demultiplexer it is unaffected by `MuteChat`.

**Rule workflow:**
```
Guild == 0 -> return
log etc,abandonar
  level in 6..8 -> clear first Sub flag -> MSG_GuildInfo to DB -> log etc,subdelete
  Guild = 0 ; GuildLevel = 0 -> GetCreateMob -> GridMulticast -> return
```

---

### Business Rule: BR-13 — Guild leadership transfer (`/handover`)

**Overview:**
`/handover <name>` transfers guild leadership to a connected same-guild character, demoting the caller to member level 0 and resetting guild fame.

**Detailed description:**
Preconditions: caller must be guilded (`:419`), must hold `GuildLevel == 9` (`:422`, silent), the payload is truncated to 14 bytes (`:425-426`), the target must resolve (`:428`), must exist (`_NN_Not_Connected`, 91), must be `USER_PLAY` (`:436-440`), and must be in the same guild (`:442-443`, silent). Only then is the swap performed: `pMob[target].GuildLevel = 9` and `pMob[conn].GuildLevel = 0` (`:451-452`), both characters are re-rendered (`:454-463`), `GuildInfo[guild].Fame = 0` (`:474`) and the record is pushed to DBSrv (`:465-478`).

The audit line records guild id, resolved guild name, old leader, and new leader (`etc,handover guild:%d guildname:%s Lider:%s -> NovoLider:%s`, `:447`), which is the most complete audit record in the file and the only one that captures both parties of a state change.

Three gaps are visible. The target's existing `GuildLevel` is not examined, so a sub-leader (6/7/8) can be promoted to 9 while still owning their `SubN` flag — the guild then reports a leader who also occupies a sub slot, and a subsequent `/subcreate` will see the flags exhausted. The target's `OnlyTrade`/combat state is not checked. And the operation is not transactional across the two `GetCreateMob` multicasts: if the second lookup fails, the state change has already been committed in memory and sent to the DB.

**Rule workflow:**
```
Guild == 0 -> return ; GuildLevel != 9 -> return
truncate 14 bytes -> target = GetUserByName
  target == 0 -> 91 ; not USER_PLAY -> 91 ; guild mismatch -> return
  log handover (guild, name, old leader, new leader)
  target.GuildLevel = 9 ; sender.GuildLevel = 0
  CreateMob multicast sender + target -> GuildInfo.Fame = 0 -> MSG_GuildInfo to DB
```

---

### Business Rule: BR-14 — Counter and clock queries (`/nt`, `/nig`, `/time`, `/day`)

**Overview:**
Four read-only replies: `/nt` prints `extra.NT` with `_DN_CHANGE_COUNT` (420); `/nig` prints local time as `!!%H%M%S`; `/time` prints `%d-%m-%Y %H:%M:%S`; `/day` sends the fixed literal `"!#11  2"`.

**Detailed description:**
`extra.NT` is the "nightmare ticket"/change counter (`Basedef.h:570`, `time_t LastNT` at `:569`), also surfaced by the `_DN_CHANGE_COUNT` format used at `GetFunc.cpp:876` for the Kefra ticket, which suggests index reuse across features. `/nig` and `/time` call `time()`/`localtime()` and `strftime` into a 256-byte local (`:495-505`, `:772-783`), so the reply reflects the *server's* local timezone, not the client's — a deliberate or accidental information leak of server geography.

`/day` is opaque: it sends the protocol-looking literal `"!#11  2"` through `SendClientMessage`, i.e. into a `MSG_MessagePanel` text field (`SendFunc.cpp:32-44`). Nothing in this component or in the language table explains it; the most plausible reading is a client-side UI command embedded in chat text (the `"!!%H%M%S"` prefix of `/nig` follows the same convention). Confidence: low; recorded as an ambiguity.

None of the four has a precondition, a cost, or a rate limit, and all four precede the mute gate.

**Rule workflow:**
```
/nt   -> format(420, extra.NT) -> SendClientMessage
/nig  -> localtime -> "!!HHMMSS" -> SendClientMessage
/time -> localtime -> "dd-mm-yyyy HH:MM:SS" -> SendClientMessage
/day  -> SendClientMessage("!#11  2")
```

---

### Business Rule: BR-15 — Personal tab and perception refresh (`/tab`)

**Overview:**
`/tab <text>` stores a 26-byte personal tab string on the mob and re-broadcasts the character (and PK state) to nearby clients; it requires level ≥ 69 unless the character is non-MORTAL.

**Detailed description:**
The level rule is `Level < 69 && ClassMaster == MORTAL` → refuse with `_DN_Level_Limit` (373) parameterised with 70 (`:512-517`). `MORTAL` is 2 (`Basedef.h:178`), so Arch (1), Celestial (3), CelestialCS (4), and SubCelestial (5) characters bypass the level requirement entirely — the gate is really an "endgame or ascended class" gate.

The write is `strncpy(pMob[conn].Tab, m->String, 26)` (`:519`) into `char Tab[36]` (`CMob.h:86`). `pMob.Tab` is *not* part of `STRUCT_MOB`, so it is not persisted to the DB character record; the tab disappears on relog and is re-sent only by the create-mob path (`GetFunc.cpp:1015`, `:1166` copy 26 bytes into `MSG_CreateMob.Tab[26]`). Because the source is a 99-byte string and the copy length equals the destination message width, an over-length tab is broadcast truncated without termination, relying on the pre-zeroed remainder of the 36-byte field.

The second half of the branch (`:521-558`) is a **perception refresh**, not a chat feature: it re-broadcasts either `MSG_CreateMob` or `MSG_CreateMobTrade` depending on `TradeMode`, then multicasts a `MSG_PKInfo` whose `Parm` is 1 when the character is guilty, in PK mode, or standing inside one of the RvR/Castle/GTorre rectangles, and is forced to 1 when `NewbieEventServer != 0`. That composite condition at `:549` is a single 340-character expression with no parentheses, mixing `&&` and `||` in a way that depends entirely on C precedence.

**Rule workflow:**
```
Level < 69 && ClassMaster == MORTAL -> 373(70)
Tab := first 26 bytes of payload
TradeMode == 0 ? CreateMob multicast : CreateMobTrade multicast
PKInfo.Parm = (NewbieEventServer ? 1 : guilty|PKMode|RvR-area|Castle-area|GTorre-area)
```

---

### Business Rule: BR-16 — Away message (`/snd`)

**Overview:**
`/snd <text>` stores a 96-byte "away"/status message on the mob and echoes it back prefixed with `_NN_SND_MESSAGE` (385).

**Detailed description:**
The storage is `strncpy(pMob[conn].Snd, m->String, 96)` (`:564`) into `char Snd[96]` (`CMob.h:87`). Because the copy length equals the array size and the source may be 99 bytes, an exactly-96-byte payload yields a non-terminated `Snd`; the value is later printed with `%s` at `:566` and at `:1355`, so the read can continue into the following `CMob` members (the next field is `int ForceMobDamage` at `CMob.h:89`). In practice the risk is a few stray bytes appended to a status line, but it is a genuine unterminated-string defect.

The away message is consumed by the private-message flow: when a whisper with an empty body is sent to a player whose `Snd[0] != 0`, the sender receives the away text (`:1353-1358`). That is the only consumer in the project — no room-entry, trade, or party flow reads `Snd`. Like `Tab`, `Snd` lives on `CMob` and is not persisted, so it is lost on logout.

There is no length policy beyond the fixed copy, no profanity or character filtering, and no rate limit; each invocation overwrites the previous value.

**Rule workflow:**
```
MobName == "snd" -> Snd := first 96 bytes of payload
  -> temp = "<385 message> <Snd>" -> SendClientMessage -> return
```

---

### Business Rule: BR-17 — Kingdom and king teleport (`/kingdom`, `/king`)

**Overview:**
Two fixed-coordinate teleports selected by clan: `/kingdom` sends clan 7 to (1690,1618), clan 8 to (1690,1842), everyone else to (1702,1726); `/king` sends clan 7 to (1748,1574) and clan 8 to (1748,1880).

**Detailed description:**
`Clan` is captured at function entry (`:24`) from `MOB.Clan` (`Basedef.h:441`). The aliases are the localized `_NN_Kingdom` (184) / `_NN_King` (185) or the English literals (`:582`, `:597`). The teleport itself is `DoTeleport` (`Server.cpp:8212`), which resolves an empty grid cell (`GetEmptyMobGrid`), builds a `MSG_Action` with `Effect = 1`, echoes it to the mover, and multicasts it to the new area.

Unlike `/summon` and `/relo`, these branches perform **no** liveness check (`Hp > 0`), no `OnlyTrade` check, no map-attribute check, and no level check. A dead or trading character is teleported by `/kingdom`; `/relo` and `/summon` explicitly refuse those states (`:804`, `:810`, `:655`, `:744`). The inconsistency means the "free movement" commands form two different safety classes.

`/king` differs from `/kingdom` in one more respect: when the clan is neither 7 nor 8, no teleport occurs but the branch still writes the log line and returns (`:599-607`). A neutral player therefore gets a "command executed" audit entry for an action that did nothing. Both commands log `etc,kingdom command <clan>` / `etc,king command <clan>`.

**Rule workflow:**
```
/kingdom -> Clan 7 ? (1690,1618) : Clan 8 ? (1690,1842) : (1702,1726) -> DoTeleport -> log
/king   -> Clan 7 ? (1748,1574) : Clan 8 ? (1748,1880) : no teleport -> log
```

---

### Business Rule: BR-18 — Guild mass summon (`/summonguild`)

**Overview:**
`/summonguild` teleports guild members to the caster's tile, subject to the caster having any guild rank (`GuildLevel > 0`), standing inside one of the five city rectangles, and standing on a tile without map-attribute bits `0x04` or `0x40`.

**Detailed description:**
The rank rule is only `GuildLevel <= 0` → `_NN_Only_Guild_Master_Can` (144) (`:623-627`), so despite the message text a level-6/7/8 sub-leader may summon the whole guild, not just the master. The positional rules use `BASE_GetVillage(x, y)` (`Basedef.cpp:4258-4267`), which returns 0..4 for the five configured city rectangles and 5 when outside; `village < 0 || village >= 5` therefore means "not in a city" (`:631-635`). The attribute test `mapatt & 4 || mapatt & 0x40` blocks tiles flagged as summon-restricted or PK-restricted (`:637-638`).

The delivery call is `SummonGuild2(usGuild, xx, yy, 350, sub)` (`:640`) with `sub = BASE_GetSubGuild(MOB.Equip[12].sIndex)` (`:629`). `BASE_GetSubGuild` (`Basedef.cpp:4237-4245`) maps item indexes 526..537 to 1..3, and 0 otherwise. Inside `SummonGuild2` (`Server.cpp:5641-5681`) the filter is `village < 1 || village > 3 || BASE_GetVillage(member) == village` — so a caster **with no sub-guild medal (`sub = 0`) summons every guild member regardless of city**, while a caster with a medal only pulls members standing in the matching city index. `maxsummon` is 350, with position offsets applied at 30 and 45 summons (`Server.cpp:5665-5675`) and a hard `break` at the cap.

Guild members in `OnlyTrade` are skipped inside `SummonGuild2`, not here. There is no liveness check on the caster or on the members, and no per-member notification; each member simply receives a teleport action.

**Rule workflow:**
```
GuildLevel <= 0 -> 144
village(caster) outside 0..4 -> 96
mapatt(caster) & (0x04|0x40) -> 96 ; else SummonGuild2(guild, x, y, 350, sub)
  member loop: same guild, USER_PLAY, !OnlyTrade, (sub==0 OR same city) -> DoTeleport
log etc,summonguild guild:X:Y
```

---

### Business Rule: BR-19 — Player summon (`/summon`)

**Overview:**
`/summon <name>` pulls another player to the caster's tile under nine independent conditions, all of which are bypassed when the caster's level is ≥ 1000.

**Detailed description:**
The precondition chain is: caster alive (`:655-659`, `_NN_Cant_Use_Killed` 90); payload non-empty (`:661`); payload truncated to 14 bytes (`:664-665`); target resolves (`:667`, `_NN_Not_Connected` 91); target is not the caster (`:675-679`, `_NN_Cant_Use_To_Yourself` 92); target is `USER_PLAY` (`:681-685`); target alive (`:700-704`, `_NN_Cant_Use_To_Killed` 93); relation check (`:706-710`, `_NN_Only_To_Party_Or_Guild` 94); caster class is ARCH (`:712-716`, `_NN_Didnt_Learn_Summon_Skill` 95); learned-skill bit `0x40000` set (`:718-724`, same message); caster tile lacks attribute `0x04` (`:728-734`, `_NN_Cant_Use_That_Here` 96); target level ≤ `caster.Level + Special[3] + 30` (`:736-742`, `_NN_Too_High_Level` 97); target not `OnlyTrade` (`:744-748`, `_NN_ONLYTRADE` 526).

The relation rule is materially narrower than its own message claims. Because `myleader` and `targetleader` are normalised at `:692-698` to `conn`/`target` when `Leader <= 0`, the sub-expressions `myleader == 0` and `myguild == 0 && myleader == 0` at `:706` can never be true; the surviving predicate is `myleader != targetleader`, i.e. **same party only**. Two guild mates who are not in a party are rejected with "only to party or guild".

The positional gate at `:750` is `pMob[target].TargetX & 0xFF00 || pMob[target].TargetY & 0xFF00`, which is true only when at least one coordinate is ≥ 256; the effect is that the summon succeeds inside the main coordinate space and fails with `_NN_Cant_Use_That_Here` for low-coordinate regions. On success `DoSummon` (`Server.cpp:8232`) moves the target, the target is told `_SN_Summoned_By_S` (52), and `etc,summon <name> X:Y` is logged. The `admin` bypass at level ≥ 1000 disables the relation, class, skill, attribute, and level checks but not the liveness, connectivity, or trade checks.

**Rule workflow:**
```
admin = Level >= 1000
caster dead -> 90 ; empty payload -> return
target = GetUserByName ; 0 -> 91 ; self -> 92 ; not PLAY -> 91
target dead && !admin -> 93 ; myleader != targetleader && !admin -> 94
Class != ARCH && !admin -> 95 ; !(LearnedSkill & 0x40000) && !admin -> 95
map(caster) & 0x04 && !admin -> 96
target.Level > caster.Level + Special[3] + 30 && !admin -> 97
target.OnlyTrade -> 526
coords have high byte -> DoSummon + notify + log ; else -> 96
```

---

### Business Rule: BR-20 — Relocation (`/relo`)

**Overview:**
`/relo <name>` teleports the caster *to* the target, gated by a medal-based or skill-based "summon mode" and by a level cap that depends on the mode.

**Detailed description:**
Preconditions mirror `/summon` but with different subjects: caster alive (`:804-808`), caster not `OnlyTrade` (`:810-814`), payload non-empty and truncated to 14 bytes (`:816-820`), target resolves/exists/is playing (`:822-834`), then the same relation predicate whose `myleader == 0` sub-terms are dead (`:847`), then the mode determination.

Mode 1 is item-based: `MOB.Equip[12].sIndex` in {522, 509, 529..531, 535..537} (`:854-857`). Mode 2 is skill-based: `Class == ARCH` **and** `LearnedSkill & 0x40000` (`:859-865`). Mode 0 → `_NN_Didnt_Learn_Summon_Skill` (95) unless admin. The level cap is `dif = Level`, plus 30 in mode 1, plus `Special[3] + 30` in mode 2 (`:881-888`), and the target must not exceed it unless admin.

Unlike `/summon`, the map-attribute test here is applied to the **target's** tile (`:873-879`) while `/summon` tests the caster's. That asymmetry allows one direction to succeed where the other fails, and neither is documentable from the code alone. On success the caster is teleported (`DoTeleport(conn, target.X, target.Y)`), the action is logged (`etc,relo <name> X:Y`), and — only for non-admin casters — the target is informed (`_SN_S_Relocate_To_You`, 100) so that a non-consensual relocation is at least visible. An admin relocation is silent to the target.

**Rule workflow:**
```
admin = Level >= 1000 ; caster dead -> 90 ; caster OnlyTrade -> 526
target = GetUserByName ; 0/not PLAY -> 91 ; relation && !admin -> 94
mode = medal-based(1) or ARCH+skill(2) ; mode == 0 && !admin -> 95
map(target) & 0x04 && !admin -> 96
dif = Level (+30 mode1 | +Special[3]+30 mode2) ; target.Level > dif && !admin -> 97
DoTeleport(conn, target.X, target.Y) -> log -> optional notify target
```

---

### Business Rule: BR-21 — Member expulsion (`/expulsar`)

**Overview:**
`/expulsar <name>` resolves a target and calls `DoDeprivate`, which removes the target from the guild according to a rank comparison.

**Detailed description:**
The component's own contribution is minimal: payload non-empty, truncate to 14 bytes, resolve with `GetUserByName`, and either delegate or reply `_NN_Not_Connected` (91) (`:911-927`). All authority logic lives in `DoDeprivate` (`Server.cpp:7755-7845`): the caller must outrank the target (`GuildLevel <= target.GuildLevel` → `_NN_Only_Guild_Master_Can`, 144), target and caller must share a guild, and the disqualification window is `(caller 6..8 && target < 6) || caller == 9`.

Consequences: a master (9) can expel anyone in the guild including sub-leaders; a sub-leader (6..8) can expel only ordinary members; an ordinary member gets `_NN_Deprivate` feedback. Expelling a sub-leader clears the first non-zero `Sub` flag and pushes `_MSG_GuildInfo` to DBSrv (`Server.cpp:7795-7819`), then clears the target's `Guild`/`GuildLevel` and re-renders them (`:7822-7830`).

Note that this component performs no `Mode == USER_PLAY` verification before delegating; `DoDeprivate` supplies it (`Server.cpp:7768`) along with the `_NN_Not_Connected` reply, so the protection exists but is two components away from the entry point.

**Rule workflow:**
```
empty -> return ; target = GetUserByName
  found -> DoDeprivate(conn, target) : not found -> 91
DoDeprivate: rank/guild checks -> clear Sub flag + MSG_GuildInfo -> Guild=0, GuildLevel=0 -> CreateMob
```

---

### Business Rule: BR-22 — Guild war and alliance termination (`/fimguerra`, `/fimirma`)

**Overview:**
The two commands ask DBSrv to clear the caller guild's war (`_MSG_War`, `Parm2 = 0`) or alliance (`_MSG_GuildAlly`, `Ally = 0`), after verifying rank, a war item, and that the guild has a registered name.

**Detailed description:**
Both branches share an identical eight-step guard (`:932-970`, `:989-1027`): `Guild > 0`, `Guild < 65536`, an immediately redundant `checkguild != myguild` comparison (both are read from the same field), `GuildLevel != 0`, `Equip[15].sIndex != 0` (the war item slot), and a name-existence test against `g_pGuildName[Group][Server][Guild]` producing `_NN_Only_Named_Guild` (173) on failure.

That name test contains a masking defect. `Server` is computed as `(myguild / MAX_GUILD) & 15` and `Guild` as `myguild & MAX_GUILD` (`:953-954`, `:1010-1011`) — but `MAX_GUILD` is 4096, so the mask should be `MAX_GUILD - 1` (or a modulo). For any legal guild id the expression yields 0, therefore the check always reads `g_pGuildName[Group][Server][0]`, i.e. the name of guild index 0 on that server. A guild can consequently be denied (or allowed) based on another guild's name registration. `BASE_GetGuildName` (`Basedef.cpp:1314-1318`) performs the decomposition correctly with subtraction.

On success the branches emit `MSG_STANDARDPARM2` `Type = _MSG_War`, `Parm1 = guild`, `Parm2 = 0` (`:972-982`) or `MSG_GuildAlly` `Guild = guild`, `Ally = 0` (`:1029-1039`). Nothing is logged in either branch, which makes these two of the few state-changing commands without an audit trail.

**Rule workflow:**
```
Guild <= 0 or >= 65536 -> return ; GuildLevel == 0 -> return ; Equip[15] == 0 -> return
name = g_pGuildName[Group][myguild/4096][myguild & 4096 (=0)] -> empty -> 173
/fimguerra -> MSG_War(Parm1=guild, Parm2=0) ; /fimirma -> MSG_GuildAlly(Guild, Ally=0)
```

---

### Business Rule: BR-23 — PIN code activation (`/pin`)

**Overview:**
`/pin <36-char code>` performs syntax validation and a sliding-window attempt check, then forwards the code with account, character, MAC, and IP to DBSrv.

**Detailed description:**
The syntax rule is exact: `strlen(m->String) != 36` or a failed `sscanf("%s")` produces `_NN_PINCODE_INVALID` (529) (`:1049-1053`). Because `String` was already NUL-forced at index 99 (`:27`), a 36-character code is representable; shorter or longer codes are refused. A copy of the code is made into `sm.PinCode` whose last byte (`MAX_PIN_LENGTH - 1` = 36) is then zeroed (`:1075`), and identity fields (`AccountName`, `MobName`, `Mac[4]`, `IP`) are attached (`:1077-1080`).

The rate rule is `if (LastTime + 7200 >= now)` → `if (Count + 1 > 3)` → `_NN_PINCODE_3TIMES` (530); otherwise the window is reset (`LastTime = now; Count = 0`) (`:1057-1069`). The logic is broken in a way that is visible in the source: `Count` is never incremented, so `Count + 1 > 3` can never hold and the 3-attempt cap is unreachable. Every attempt also refreshes nothing, so `LastTime` only advances when a window has expired — the intended semantics (max 3 attempts per 2 hours) collapse to "unlimited attempts".

The downstream half is inert as well: DBSrv's `_MSG_DBActivatePinCode` handler consists of a single commented-out call (`// CActivePinCode::ProcessMessage(Msg, conn);`) at `CFileDB.cpp:2064`, so the message is consumed and discarded. The command therefore logs nothing, changes nothing, and returns no client feedback on success.

**Rule workflow:**
```
strlen != 36 -> 529
LastTime + 7200 >= now ? (Count + 1 > 3 ? 530 : continue) : reset window
fill MSG_DBActivatePinCode {PinCode, AccountName, MobName, Mac, IP} -> DBSrv
DBSrv: handler body is commented out -> message dropped
```

---

### Business Rule: BR-24 — Global admin notice (`/not`)

**Overview:**
`/not <text>` broadcasts a server-wide notice through DBSrv, gated only by `MobName == "not" && MOB.CurrentScore.Level >= 1000`.

**Detailed description:**
The privilege test is embedded in the branch condition itself rather than in the block body (`:1088`), which means a low-level character sending `/not` falls through to the chat demultiplexer instead of being refused; if the payload is not a channel prefix the packet is silently discarded at `:1281`. There is no sub-tier check equivalent to `ProcessImple`'s level ≥ 1, so a character that reaches exactly level 1000 (GM level 9) has the same notice power as level 2000.

The payload is copied with `strncpy(sm_dbn.String, m->String, MESSAGE_LENGTH - 1)` into a 96-byte field (`:1097`), i.e. 95 bytes plus the pre-zeroed tail, and sent as `_MSG_DBNotice` with `ID = 0` (`:1091-1099`). DBSrv re-broadcasts the raw struct to every connected game server (`CFileDB.cpp:1612-1622`); each TMSrv's `ProcessDBMessage` converts it to `SendNotice` over all local players (`ProcessDBMessage.cpp:141-146`).

Notably this branch sits **above** the mute gate, so a muted GM can still broadcast. It also performs no audit logging of its own — the only record is whatever `SendNotice` writes (`SendFunc.cpp:52`) and the `Log` calls on the DB side.

**Rule workflow:**
```
MobName == "not" && Level >= 1000 -> MSG_DBNotice(ID=0, String=payload) -> DBSrv
  -> fan-out to all TMSrv -> SendNotice -> all USER_PLAY
(otherwise falls through to chat demux)
```

---

### Business Rule: BR-25 — Donation and primary-account queries (`/wp`, `/contaprincipal`)

**Overview:**
`/wp` prints `pUser[conn].Donate` using `_DN_REMAINDONATE` (533); `/contaprincipal` sends the account's MAC and IP to DBSrv, which marks the account as primary and announces the change to all game servers.

**Detailed description:**
`/wp` is a pure read of the session field `CUser::Donate` (declared at the end of `CUser.h`), which is populated from the DB character payload (`ProcessDBMessage.cpp:728`). It has no precondition or rate limit and precedes the mute gate.

`/contaprincipal` (`:1112-1127`) builds `MSG_DBPrimaryAccount` with `ID = conn`, `Mac` copied from `pUser[conn].Mac`, and `IP = pUser[conn].IP`, then forwards it. DBSrv resolves the account index from `m->ID`, builds `_MSG_DBCheckPrimaryAccount` with that account name and MAC, and sends it to **every** connected game server (`CFileDB.cpp:2068-2100`), finally confirming to the caller with the Portuguese string "Sua conta agora é a primária.". Each TMSrv then compares the MAC against every local session (`ProcessDBMessage.cpp:148-159`) — the cross-server MAC-collision detection path.

Two properties stand out. The command has no authorization of any kind: any playing character can declare its own account primary, which by design is a self-service operation, but the absence of any confirmation step means a single packet changes account-level state group-wide. And the identity material is taken from server-side session fields rather than the payload, so MAC spoofing through this message is not possible.

**Rule workflow:**
```
/wp -> SendClientMessage(format(533, Donate))
/contaprincipal -> MSG_DBPrimaryAccount{ID, Mac, IP} -> DBSrv
  -> _MSG_DBCheckPrimaryAccount to all MAX_SERVER game servers -> per-server MAC compare
  -> confirmation text to caller
```

---

### Business Rule: BR-26 — GM interpreter delegation (`/gm`)

**Overview:**
`/gm <command>` computes `level = MOB.CurrentScore.Level - 1000` and forwards `String` verbatim to `ProcessImple(conn, level, String)`; the component itself performs no privilege check.

**Detailed description:**
The branch is two statements and a return (`:787-793`), accepting both `"gm"` and `"GM"`. `ProcessImple` (`imple.cpp:46`) immediately logs `"adm <string>"` with the account name and IP (`:80-87`), then parses with `sscanf(str + 1, ...)`, deliberately skipping the first character of `String` — therefore the client must place a leading `/` in the payload, and a payload without it loses its first character. All 67 command comparisons inside `ProcessImple` are guarded by four cumulative level tiers: ≥ 8 (`:89`, item/score/name/level mutation), ≥ 3 (`:569`, timers, save-all, billing/drop configuration), ≥ 2 (`:647`, reboot, delayed reboot, mob generation), and ≥ 1 (`:1119`, `:1164`, weather, map attribute dump, billing connect, tax reset, teleport, kick, gsummon, allsummon). Level 0 (`Level == 1000` exactly) receives no commands and no feedback.

Because the only privilege input is a mutable game field, the actual security boundary of the whole admin surface is the integrity of `MOB.CurrentScore.Level` (and by extension `BaseScore.Level`, which `set level` can raise to 2010 at `imple.cpp:125-129`). `CUser::Admin` exists but is never consulted in TMSrv; `IsImple` is a stub returning 0 (`Server.cpp:6829-6832`). The `conn` validation inside `ProcessImple` (`:91`, `:1832`) is written as `conn <= 0 && conn >= MAX_USER`, an unsatisfiable conjunction, so the intended guard never fires.

Parsing robustness: `cmd` and `sval1..sval8` are uninitialised stack buffers (`:48-56`) filled by `sscanf` without a return-value check (`:68`). A payload of `"/gm"` (empty remainder) or `"/gm "` leaves them uninitialised, and the subsequent `strcmp(cmd, ...)` and `strcmp(sval1, ...)` chains then read indeterminate memory — undefined behaviour on every such packet, reachable by any player.

**Rule workflow:**
```
MobName in {"gm","GM"} -> level = Level - 1000 -> ProcessImple(conn, level, String)
ProcessImple: log "adm ..." -> sscanf(str+1) -> tier checks (>=8, >=3, >=2, >=1)
  -> mutate state / send client message / send DB message ; level 0 -> nothing
```

---

### Business Rule: BR-27 — Global mute enforcement

**Overview:**
At `:1129-1133` a single check refuses all remaining chat when `pUser[conn].MuteChat == 1`, replying `_NN_No_Speak` (353).

**Detailed description:**
`MuteChat` is an `int` on `CUser` and is the moderation flag applied by administrators (it survives normal play because it is part of the session object rather than the character record). The gate is positioned after the entire command chain and before the chat demultiplexer, which yields a precise but non-obvious policy: **muted players retain every slash command** (`/create`, `/summon`, `/relo`, `/srv`, `/pin`, `/contaprincipal`, `/gm`, `/not`, …) and lose only guild/party/kingdom/citizen/PM. `/spk` is the sole command that re-checks the flag itself (`:100`).

The message is sent with `SendClientMessage`, so the client receives a message-panel frame rather than a channel-specific error; there is no indication of remaining mute duration or reason. No log entry is written when a muted message is rejected, so moderation has no record of attempted evasion.

Because the gate is a single `if` with `return`, it cannot be partially bypassed from within this file; all six chat paths are downstream of it. The gap is structural rather than logical: the GM commands above it were deliberately placed there (the file's `#pragma region` layout groups them as "commands" before the "chat" regions).

**Rule workflow:**
```
(command chain fell through) -> MuteChat == 1 ? SendClientMessage(353) + return
  -> MobName empty ? channels : private message
```

---

### Business Rule: BR-28 — Guild chat channel

**Overview:**
With an empty `MobName` and `String[0] == '-'`, the message is delivered to all same-guild `USER_PLAY` sessions that have not disabled guild chat, and the sender's name replaces `MobName`.

**Detailed description:**
Routing conditions are: recipient `Mode == USER_PLAY` (`:1157`), guild equality (`:1160`), recipient not the sender (`:1166`), and `pUser[i].Guildchat == 0` (`:1169`). The recipient set is built by iterating the full `pUser[1..999]` array — an O(1000) scan per message — and enqueuing a copy of the modified 128-byte struct into each socket (`:1172-1173`). The first character `-` is *not* stripped from `String`, so every recipient receives the payload with its channel prefix intact.

Before the loop the handler writes `m->String[MESSAGE_LENGTH] = 3` (`:1143`), i.e. byte 96 of the 100-byte `String`, while byte 99 was already zeroed at `:27`. If the client sends 96–99 characters, those bytes are replaced by `0x03` followed by up to three NULs; otherwise the marker sits harmlessly after the terminating NUL. The value appears to be a client-side channel discriminator (guild chat has a marker, party chat does not), but no server-side consumer reads it. Confidence in that interpretation: medium.

Unguilded characters get `_NN_Only_Guild_Member_Can` (104) and are stopped before any fan-out (`:1147-1151`). The audit line is `chat_guild, <name> : <text> guild:<name>` with the guild name resolved through `BASE_GetGuildName`.

**Rule workflow:**
```
MobName empty && String[0]=='-' -> MobName := sender ; String[96] = 3
Guild == 0 -> 104
loop 1..999: USER_PLAY && same guild && !self && !Guildchat -> ID=conn ; AddMessage
ChatLog("chat_guild, name : text guild:name")
```

---

### Business Rule: BR-29 — Guild ally extension

**Overview:**
If the guild-chat payload's second character is also `-` (i.e. the text begins `--`), delivery is extended to the guild registered as this guild's ally, provided an ally is registered.

**Detailed description:**
The condition is at `:1163`: a recipient outside the sender's guild is allowed only when `m->String[1] == '-'` **and** the recipient's guild equals `g_pGuildAlly[guild]` **and** that ally id is non-zero. `g_pGuildAlly` is a 65,536-entry `unsigned short` map (`ProcessClientMessage.h` extern; `Server.h:52`), i.e. a single ally per guild. If no ally is registered, the clause degenerates to "same guild only" and the `--` prefix merely appears in the text.

This is the only place in the component where a chat channel crosses the guild boundary, and the only cross-guild delivery that does not require leader authority. It has no consent mechanism on the receiving side beyond the per-recipient `Guildchat` flag, which the ally's members can toggle independently. Because the ally relation is set elsewhere (alliance formation), the sender cannot unilaterally widen the audience beyond the currently registered ally.

Two behavioural consequences are worth recording: ally messages are indistinguishable from guild messages on the wire (both carry `String[0] == '-'`), so a receiving client cannot style them differently without inspecting `String[1]`; and the log line does not distinguish ally traffic from guild traffic, so the audit trail cannot reconstruct which audience a message reached.

**Rule workflow:**
```
String[0]=='-' && String[1]=='-' -> audience = own guild + g_pGuildAlly[guild] (if != 0)
  recipient guild in audience && USER_PLAY && !self && !Guildchat -> AddMessage
```

---

### Business Rule: BR-30 — Party chat channel

**Overview:**
With `String[0] == '='`, delivery goes first to the party leader and then to each `PartyList` member, honouring the recipient `PartyChat` opt-out — except for the leader.

**Detailed description:**
The leader is resolved as `pMob[conn].Leader`, defaulting to `conn` when the value is ≤ 0 (`:1189-1192`), and is validated as in-range and `USER_PLAY` (`:1194-1198`); if the leader is invalid the whole message is dropped silently. The leader is enqueued at `:1200-1201` before the loop and **without** the `PartyChat` check, so a leader who has disabled party chat still receives party messages, and a solo player's own `Leader == 0` normalisation means a solo sender delivers to nobody (leader == conn is skipped by the `myleader != conn` condition).

The loop over `PartyList[0..MAX_PARTY-1]` (`MAX_PARTY` = 12, `Basedef.h:168`) skips out-of-range slots, the sender, non-`USER_PLAY` slots, and recipients with `PartyChat != 0` (`:1205-1217`). If the leader also appears in `PartyList` — which the party-formation code permits — the leader receives the message twice, since the first delivery at `:1201` is unconditional on list membership. No de-duplication exists.

`String[0] == '='` is not stripped and no marker byte is set, so party chat is the only channel without a client-side discriminator. The audit line is `chat_party, <name> : <text>`.

**Rule workflow:**
```
MobName empty && String[0]=='=' -> MobName := sender
leader = Leader>0 ? Leader : conn ; leader invalid -> return
leader != conn -> AddMessage(leader)                       (no PartyChat filter)
for i in 0..11: PartyList[i] valid && != conn && USER_PLAY && !PartyChat -> AddMessage
ChatLog("chat_party, name : text")
```

---

### Business Rule: BR-31 — Kingdom chat and its rate limit

**Overview:**
`@@<text>` broadcasts to all `USER_PLAY` characters of the same `MOB.Clan` on the current server, excluding the sender and excluding sessions with `KingChat != 0`, subject to one message per 3,000 ms.

**Detailed description:**
The rate limiter is `pUser[conn].Message`, an `unsigned int` on `CUser`, compared against `GetTickCount()`: if the stored stamp is non-zero, `isTime = GetTickCount() - Message` and the message is refused with the hardcoded Portuguese text `"Aguarde 3 segundos."` when `isTime < 3000` (`:1231-1242`). The stamp is written **before** the comparison (`:1235`) and again after it (`:1243`), so the declared intent is "timestamp every attempt", which is a sliding window of 3 s per attempt rather than a fixed 3 s bucket. `Message` is reset to 0 on character login (`ProcessDBMessage.cpp:735`) and nowhere else, so the limiter survives character state changes within a session.

Delivery uses `SyncKingdomMulticast(conn, Clan, m, 0)` (`SendFunc.cpp:274-286`), which iterates all sessions, requires `USER_PLAY`, a different connection, `MOB.Clan == Kingdom`, and `pUser[i].KingChat == 0`. Because `Clan` is a `char` with no validation, clan value 0 (neutral) forms a channel of its own, and every neutral character on the server receives those messages. The fan-out is local to the process — no DB message, so kingdom chat does not cross game servers even though "kingdom" implies a group-wide audience.

The sender's name is copied into `MobName` and the `@@` prefix remains in the text. The audit line records the realm: `chat_kingdom, <name> : <text> reino:<clan>`.

**Rule workflow:**
```
MobName empty && String[0..1]=="@@"
Message != 0 && now-Message < 3000 -> "Aguarde 3 segundos." ; Message = now
MobName := sender -> SyncKingdomMulticast(conn, Clan, m, 0)
  recipients: USER_PLAY && != conn && Clan match && KingChat == 0
ChatLog("chat_kingdom, ... reino:clan")
```

---

### Business Rule: BR-32 — Citizen (global) chat and its rate limit

**Overview:**
`@<text>` broadcasts to every `USER_PLAY` session on the server except the sender, using the same 3,000 ms bucket as kingdom chat, with no per-recipient opt-out.

**Detailed description:**
The limiter block is duplicated verbatim from BR-31 (`:1258-1270`), including the "stamp before compare" ordering and the hardcoded refusal text. Because both channels share `pUser[conn].Message`, a kingdom message and a citizen message compete for the same quota: sending `@@` then `@` within 3 s is refused, even though they target different audiences.

Delivery is `SyncMulticast(conn, m, 0)` (`SendFunc.cpp:260-272`), a loop over all `MAX_USER` sessions requiring `USER_PLAY` and `i != conn`. There is **no** recipient flag test, so the `Chatting` toggle that exists on `CUser` and is maintained by `Exec_MSG_MessageChat` (`_MSG_MessageChat.cpp:159-169`) is ignored here — the only global channel a player cannot opt out of. This is the most intrusive routing rule in the component: one packet reaches up to 999 sessions and enqueues 999 copies of a 128-byte struct.

The audience is server-local, and the text keeps the `@` prefix. Audit: `chat_cidadao, <name> : <text>`.

**Rule workflow:**
```
MobName empty && String[0]=="@" && String[1]!="@" 
shared Message bucket -> maybe "Aguarde 3 segundos."
MobName := sender -> SyncMulticast(conn, m, 0)  (all USER_PLAY except sender, no opt-out)
ChatLog("chat_cidadao, ...")
```

---

### Business Rule: BR-33 — Private message target resolution and reply aliasing

**Overview:**
A non-empty `MobName` resolves a recipient by character name (or `+ACCOUNTNAME` for account lookup); `/r` and three localized aliases substitute the last conversation partner, and the partner of every PM is recorded on both sides.

**Detailed description:**
Resolution uses `GetUserByName` (`Server.cpp:6793-6827`), which scans `pMob[1..999]`, requires `pMob[i].Mode != MOB_EMPTY` and `pUser[i].Mode == USER_PLAY`, and compares either `pUser[i].AccountName` (upper-cased, when the query starts with `+`) or `pMob[i].MOB.MobName` exactly. Lookup is therefore case-sensitive for character names and case-normalising for account names; the `+` prefix is the only namespace escape and it consumes one of the 15 usable name bytes.

The reply alias set is `g_pMessageStringTable[_NN_Reply]` (101), `"r"`, `"ñº"`, and `"¦õ"` (`:1291-1293`) — one English alias, one ASCII alias, and two byte sequences that are almost certainly Portuguese "não" and Korean text mangled by code-page conversion. Substitution requires `pUser[conn].LastChat[0] != 0`, otherwise `_NN_No_One_To_Reply` (102) is returned. `LastChat` is a 16-byte field on `CUser` (declared after `SelChar`) and is written **twice** per successful PM: with the recipient's name at `:1327` (so the sender can reply) and with the sender's name at `:1368` (so the recipient can reply). The write at `:1327` happens before the empty-body check, so even a profile query re-points the sender's reply target.

Failure replies are `_NN_Not_Connected` (91) for both "no such character" and "character not in play" — an intentional ambiguity that avoids disclosing whether a name exists but is offline. Self-targeting is permitted: `target == conn` satisfies every check, so a character can whisper itself, which sets `LastChat` to its own name and delivers the frame back to the sender.

**Rule workflow:**
```
MobName non-empty -> truncate 15/14
  alias in {_NN_Reply,"r","ñº","¦õ"} -> LastChat empty ? 102 : MobName := LastChat
target = GetUserByName (exact name | +ACCOUNT)
  0 or not USER_PLAY -> 91
m->ID = target ; LastChat := target name
body empty -> profile card (BR-35)
else -> sanitize, MobName := sender, target.LastChat := sender, relay
```

---

### Business Rule: BR-34 — Whisper privacy / ignore rule

**Overview:**
If the recipient has `pUser[target].Whisper != 0`, the private message is refused with `_NN_Deny_Whisper` (165) unless the sender's character level is ≥ 1000.

**Detailed description:**
`Whisper` is an `int` on `CUser`, toggled by the `/whisper` command handled in the *sibling* component `_MSG_MessageChat.cpp:111-121` (a boolean flip reported to the client as "Whisper : Off/On"). It is a per-session, global ignore that applies to **all** senders without any per-player exception list, and it does not persist across logout because it lives on the session object rather than the character record.

The GM exemption is the only bypass: `pMob[conn].MOB.CurrentScore.Level < 1000` is required for the block to apply (`:1320`). This is the same threshold used for admin notice (`:1088`), `admin` bypasses in `/summon` and `/relo` (`:652`, `:801`), and the base of the `/gm` privilege computation (`:789`), so a single level value simultaneously controls four unrelated capabilities with no separate role model.

The refusal is the *sender's* message only; the recipient is never informed that someone tried to whisper them. Note the rule is evaluated **before** the payload is examined, so even the profile-query form (empty body) is blocked by a whisper-disabled recipient — a player cannot look up citizen/fame/guild of someone who has whispers off, which is the only place where the privacy flag also hides public character attributes.

**Rule workflow:**
```
target resolved && USER_PLAY
pUser[target].Whisper != 0 && sender.Level < 1000 -> 165 (stop)
otherwise continue to profile query or delivery
```

---

### Business Rule: BR-35 — Whisper profile query (empty body)

**Overview:**
A private message with an empty `String` does not deliver text; it returns the target's citizen flag, fame, guild name (if any), and away message to the sender.

**Detailed description:**
The branch is at `:1329-1360`. For an unguilded target the reply is `<MobName> <_NN_Check_User_Info(435) formatted with citizen, fame)>`; for a guilded target the guild name is appended in brackets (`[name]`) after resolution via `BASE_GetGuildName`. If `pMob[target].Snd[0] != 0`, a second message-panel frame is sent with `_NN_SND_MESSAGE` (385) and the away text.

Because `m->ID` and both `LastChat` values were already set before this branch, a profile query is also a "start conversation" event: `/r` afterwards replies to the queried player. That coupling is invisible to the player and is the most likely source of accidental misdirected whispers in the whole flow.

The information disclosed (citizen flag, fame, guild, away text) is otherwise visible in the world through the client's inspection UI, so the rule is a convenience rather than a disclosure escalation — with one nuance: it bypasses `Whisper` privacy only in the sense that a whisper-disabled recipient blocks it entirely (BR-34), so the query is strictly *more* restricted than the entity inspection path.

**Rule workflow:**
```
String empty -> guild? "<name> <citizen,fame> [guild]" : "<name> <citizen,fame>"
  SendClientMessage(sender)
  if target.Snd set -> SendClientMessage(sender, "<385> <Snd>")
  return (no delivery, no chat log)
```

---

### Business Rule: BR-36 — Private message sanitization and relay

**Overview:**
A non-empty PM body has leading `-`/`=` rewritten to a space, is truncated to 96 bytes, has the sender's name stamped into `MobName`, and is enqueued to the recipient with `ID = target`.

**Detailed description:**
The marker rewrite at `:1362-1363` prevents a private message from being mistaken for a channel message if the recipient's client re-broadcasts or re-parses the field, and it prevents the `--` ally-chat prefix from surviving into a private conversation. Only the *first* character is rewritten, and only for `-`/`=`, not for `@`/`@@`; a PM body beginning with `@` therefore reaches the recipient unchanged, which is harmless because the recipient's client has no channel-dispatch role.

The truncation at `:1365` writes `String[96] = 0`. Combined with the entry-time `String[99] = 0` (`:27`) this means a 97–99 byte body leaves bytes 96–99 partially non-zero in the struct that is relayed; because `String[96]` is zeroed, `%s` consumers stop there, and the client is documented (by the party/guild paths) to read up to 96 bytes, so the practical effect is a 96-byte message limit.

The relay itself is `pUser[target].cSock.AddMessage((char*)m, sizeof(MSG_MessageWhisper))` (`:1370`); `AddMessage` overwrites the `Size`, `KeyWord`, `CheckSum`, and `ClientTick` fields of the shared struct (`CPSock.cpp:538-541`) and validates that the target socket is open and the send buffer has room (`:518-533`), logging `err,add buffer invalid` if not. If the send buffer is full the message is silently dropped after the log — the sender is never told. The audit line `chat_sms,<sender> <recipient> : <text>` duplicates the sender name in both the prefix and the stamped field (`:1372`).

**Rule workflow:**
```
String[0] in {'-','='} -> ' '
String[96] = 0 ; MobName := sender ; target.LastChat := sender
AddMessage(target, sizeof(MSG_MessageWhisper))
  buffer full or socket closed -> log only (message lost)
ChatLog("chat_sms,sender recipient : text")
```

---

### Business Rule: BR-37 — Audit logging

**Overview:**
Accepted commands and chat are written to two file sinks: `Log()` (`Server.cpp:6701`, general `TM_*.txt` log plus the window status line) and `ChatLog()` (`Server.cpp:9225`, `chat_*.txt`).

**Detailed description:**
Command audit entries are emitted for `/getout` (`:46`), `/gfame` (none), `/spk` (`chat_spk`, `:140`), `/create` (`sys,guild medal value`, `:258`), `/subcreate` (`etc,subcreate`, `:344`), `/abandonar` (`etc,abandonar`, `:373`, plus `etc,subdelete`, `:389`), `/handover` (`:447`), `/kingdom` (`:591`), `/king` (`:605`), `/summonguild` (`:642`), `/summon` (`:757`), `/relo` (`:898`), and `/gm` (indirectly, via `ProcessImple`'s `adm <string>`, `imple.cpp:81-87`). Commands with **no** audit entry include `/cp`, `/getout`'s inverse (none exists), `/srv`, `/qst`, `/nt`, `/nig`, `/tab`, `/snd`, `/day`, `/time`, `/expulsar`, `/fimguerra`, `/fimirma`, `/pin`, `/wp`, and `/contaprincipal`.

Chat audit entries exist for guild (`:1179`), party (`:1223`), kingdom (`:1249`), citizen (`:1276`), and private (`:1372`) messages, plus `chat_spk` for trumpet. Every one of them embeds raw player-controlled text, and `Log`/`ChatLog` pass their composed buffer as the `fprintf` *format string* (`Server.cpp:6720-6723`, `:9241-9244`; the `Log` implementation even reads `LogTemp` as its own `%s` argument). A payload containing `%n` or `%s` sequences is therefore interpreted by `fprintf`, which at minimum corrupts the log line and at worst permits arbitrary memory writes into the process's address space on MSVC's legacy `printf` family. This is the highest-severity defect in the audit path and it is reachable from every chat message, not just from GM text.

No log rotation is driven from this component: rotation is handled by `StartChatLog`/`StartLog` day checks in `Server.cpp` (`:9220`, `:9257`). Log writes are unbuffered `fprintf` calls on a process-global `FILE*`, and the component never flushes explicitly.

**Rule workflow:**
```
command branch -> Log("<tag>,<data>", account, ip)  -> fLogFile
chat branch    -> ChatLog("<tag>,name : text", account, ip) -> fChatLogFile
```

---

### Business Rule: BR-38 — Channel opt-out flags and their ownership

**Overview:**
The four per-channel `int` flags consulted by this component (`Whisper`, `Guildchat`, `PartyChat`, `KingChat`) and the unused `Chatting` flag are written exclusively by `Exec_MSG_MessageChat` in `_MSG_MessageChat.cpp`.

**Detailed description:**
Each flag is a boolean on `CUser`, documented only by comments in `CUser.h` ("`int Whisper; // 2636 // Whisper ?`"). They are toggled by text commands sent on the *local chat* packet (`_MSG_MessageChat`), not on the whisper packet: `whisper` (`:111-121`), `partychat` (`:123-133`), `kingdomchat` (`:135-145`), `guildchat` (`:147-157`), `chatting` (`:159-169`). This component consumes four of the five and ignores `Chatting`, so the flag set is only partially wired.

The toggles report the *new* state as "Off" when the resulting value is 1 and "On" when 0 (`_MSG_MessageChat.cpp:113-118`), i.e. the flag is "blocking enabled". There is no persistence: flags live on the session object, so a relog resets every channel to the default (receiving). There is no per-sender ignore, no keyword filter, and no cross-server propagation of the flags.

The ownership split matters for analysis because it means this component's privacy behaviour cannot be assessed from its own source: three of the six channels are filterable (whisper, guild, party), one is filterable but not for the leader (party), and two are not filterable at all (`KingChat` filters kingdom; citizen has no filter despite the unused `Chatting` flag).

**Rule workflow:**
```
MessageChat("whisper"/"guildchat"/"partychat"/"kingdomchat"/"chatting") -> flag flip
Whisper component: reads Whisper (PM), Guildchat (guild), PartyChat (party), KingChat (kingdom)
Citizen channel: no flag read (Chatting never consulted)
```

---

## 4. Component Structure

```
Code/
├── TMSrv/
│   ├── _MSG_MessageWhisper.cpp        # THE COMPONENT — 1,378 lines, one function, 35,006 bytes
│   │   ├── :20-30      entry, field truncation, session gate
│   │   ├── :32-508     command chain A (12 commands)
│   │   ├── :509-579    command chains B/C/D (/tab, /snd, /day)
│   │   ├── :581-1127   command chain E (15 commands incl. /gm, /summon, /relo)
│   │   ├── :1129-1133  global mute gate
│   │   ├── :1135-1282  channel demultiplexer (guild, party, kingdom, citizen)
│   │   └── :1283-1377  private message (resolution, privacy, profile query, relay)
│   ├── ProcessClientMessage.cpp       # dispatcher: case _MSG_MessageWhisper at :164-166
│   ├── ProcessClientMessage.h         # declaration at :76; externs for pMob/pUser/GuildInfo/g_pGuildAlly/temp
│   ├── ProcessDBMessage.cpp           # DB→game: _MSG_MessageDBImple -> ProcessImple(0, level, str) :123-134
│   ├── imple.cpp                      # ProcessImple :46 (efferent target for /gm)
│   ├── SendFunc.cpp                   # SendClientMessage :27, SyncMulticast :260, SyncKingdomMulticast :274, SendItem :1055
│   ├── Server.cpp                     # Log :6701, GetUserByName :6793, DoDeprivate :7755, DoTeleport :8212, DoSummon :8232, SummonGuild2 :5641, ChatLog :9225, globals :53,:264,:277,:353
│   ├── Server.h                       # externs: temp[4096] :382, DBServerSocket :224, pMob :230, NumServerInGroup :300, NewbieEventServer :267, RvRState :298, CastleState :337, GTorreState :407
│   ├── GetFunc.cpp                    # GetCreateMobTrade :1072, GetAttribute :1257, GetPKPoint :2105, GetGuilty :2114
│   ├── CUser.h                        # Whisper/Guildchat/PartyChat/Chatting/Message/MuteChat/OnlyTrade/Mac/IP/Donate/Slot
│   ├── CMob.h                         # Leader :43, PartyList :70, extra :84, Tab :86, Snd :87, MaxCarry :96
│   ├── CReadFiles.cpp                 # GuildCounter read/write :772-870, GUILD_PATH :42
│   └── TMSrv.vcxproj                  # ClCompile entry :141; _PACKET_DEBUG only in Debug|Win32 :61
├── Basedef.h                          # _MSG macro :925-930, MSG_MessageWhisper :1447-1453, MSG_MagicTrumpet :1400-1405, constants :48-172
├── Basedef.cpp                        # g_pMessageStringTable :742, BASE_GetGuildName :1314, BASE_GetSubGuild :4237, BASE_GetVillage :4258, BASE_InitializeGuildName :6153, BASE_CheckPacket stub :6475-6595
├── CPSock.h / CPSock.cpp              # framing :353-467, AddMessage :513-591, MAX_MESSAGE_SIZE :38
└── DBSrv/
    ├── CFileDB.cpp                    # _MSG_MagicTrumpet fan-out :1591, _MSG_DBNotice :1612, _MSG_DBServerChange :1929, _MSG_DBActivatePinCode no-op :2060, _MSG_DBPrimaryAccount :2068
    └── Server.cpp                     # DB-side Log :2115
```

Structural observations:

- The component is **one function** spanning `:20`-`:1378` (1,359 lines of a 1,378-line file) with a cyclomatic complexity dominated by a 29-branch dispatch chain. There are no helper functions, no local types, and no header of its own; it depends entirely on `ProcessClientMessage.h` for its contract.
- `#pragma region` markers divide the function into 29 labelled regions (e.g. `/cp`, `/getout`, `GUILD_CHAT`, `PARTY_CHAT`, `KINGDOM_CHAT`, `CITZEN_CHAT`, `PM, /r`), providing the only internal structure. Several region labels preserve the original Portuguese naming (`/expulsar`, `Cidadao`, `Troca de Server`).
- All I/O is through process-global state (`pUser`, `pMob`, `GuildInfo`, `g_pMessageStringTable`, `temp`, `DBServerSocket`); the function has no parameters other than the connection index and a raw message pointer.
- The `/spk` item index (`3330`) and the `KILL_MARK` slot (`63`) are magic numbers with no symbolic constant in this file; the `0x40000` summon-skill bit, the `0x04`/`0x40` map-attribute bits, and the `1000` admin level offset are likewise inline literals.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (call graph, efferent)

```
Exec_MSG_MessageWhisper
├── ProcessImple                        [imple.cpp:46]        /gm only; conn, level, str
│   └── (67 command comparisons, level tiers 8/3/2/1)
├── SendClientMessage                   [SendFunc.cpp:27]     -> pUser[conn].cSock.AddMessage
├── SyncMulticast                       [SendFunc.cpp:260]    citizen chat
├── SyncKingdomMulticast                [SendFunc.cpp:274]    kingdom chat
├── GridMulticast (2 overloads)         [SendFunc.cpp:620,:843]
├── SendItem                            [SendFunc.cpp:1055]   /spk consumption
├── SendEtc                             [SendFunc.cpp:1195]   /create coin update
├── GetCreateMob / GetCreateMobTrade    [GetFunc.cpp:947,:1072]
├── GetAttribute                        [GetFunc.cpp:1257]    /tab, /summon, /relo, /summonguild
├── GetPKPoint                          [GetFunc.cpp:2105]    /cp
├── GetUserByName                       [Server.cpp:6793]     /subcreate, /handover, /summon, /relo, /expulsar, PM
├── DoTeleport                          [Server.cpp:8212]     /kingdom, /king, /relo
├── DoSummon                            [Server.cpp:8232]     /summon
├── DoDeprivate                         [Server.cpp:7755]     /expulsar
├── SummonGuild2                        [Server.cpp:5641]     /summonguild
├── Log                                 [Server.cpp:6701]     command audit
├── ChatLog                             [Server.cpp:9225]     chat audit
├── BASE_GetItemAmount / BASE_SetItemAmount / BASE_ClearItem   [Basedef.cpp]  /spk
├── BASE_GetVillage                     [Basedef.cpp:4258]    /summonguild
├── BASE_GetSubGuild                    [Basedef.cpp:4237]    /summonguild medal
├── BASE_GetGuildName                   [Basedef.cpp:1314]    guild/ally chat, /handover, PM card
├── BASE_InitializeGuildName            [Basedef.cpp:6153]    /create
├── CReadFiles::WriteGuild              [CReadFiles.cpp:827]  /create
├── pUser[i].cSock.AddMessage           [CPSock.cpp:513]      direct channel/PM delivery
└── DBServerSocket.SendOneMessage       [CPSock.cpp]          8 message types (below)
```

### 5.2 Internal dependencies (afferent — who depends on this component)

```
ProcessClientMessage.cpp:164-166   -> Exec_MSG_MessageWhisper(conn, pMsg)     (only runtime caller)
ProcessClientMessage.h:76          -> declaration consumed by the dispatcher and any TU including the header
TMSrv.vcxproj:141 / .filters:207   -> build-system inclusion
```

No other translation unit calls `Exec_MSG_MessageWhisper`; a project-wide search for `_MSG_MessageWhisper` yields only the dispatcher, the header, the struct definition (`Basedef.h:1447`), the disabled size table entry (`Basedef.cpp:6502`), and this file.

### 5.3 External dependencies (outbound, message level)

| Message | Destination | Emitted from | Symmetric handler |
|---|---|---|---|
| `_MSG_DBServerChange` | DBSrv | `:67-76` | `CFileDB.cpp:1929-1988` |
| `_MSG_MagicTrumpet` | DBSrv (fan-out) | `:125-138` | `CFileDB.cpp:1591-1610`, `ProcessDBMessage.cpp:136-139` |
| `_MSG_GuildInfo` | DBSrv | `:238-254`, `:347-357`, `:392-402`, `:465-478` | `ProcessDBMessage.cpp:100-105` |
| `_MSG_War` (`MSG_STANDARDPARM2`) | DBSrv | `:972-982` | guild war subsystem |
| `_MSG_GuildAlly` | DBSrv | `:1029-1039` | `_MSG_GuildAlly` handler |
| `_MSG_DBActivatePinCode` | DBSrv | `:1046-1082` | `CFileDB.cpp:2060-2066` (no-op) |
| `_MSG_DBNotice` | DBSrv (fan-out) | `:1090-1099` | `CFileDB.cpp:1612-1622` |
| `_MSG_DBPrimaryAccount` | DBSrv | `:1114-1124` | `CFileDB.cpp:2068-2100` |
| `MSG_MessagePanel` | client(s) | via `SendClientMessage` | client UI |
| `MSG_MessageWhisper` | peer client(s) | `:1173`, `:1201`, `:1220`, `:1370` | client chat UI |
| `MSG_CreateMob` / `MSG_CreateMobTrade` | nearby clients | `:363`, `:412`, `:458-463`, `:526`, `:533` | client entity view |
| `MSG_PKInfo` | nearby clients | `:536-557` | client name-colour UI |
| `MSG_Action` | mover + nearby | via `DoTeleport`/`DoSummon` | client movement |
| `MSG_SendItem` | caller | `:118` via `SendItem` | client inventory |

### 5.4 File / OS level dependencies

- Win32 API: `GetTickCount`, `time`, `localtime`, `strftime`, `fopen`, `fprintf`, `fclose`.
- CRT: `sprintf`, `sscanf`, `strncpy`, `strcmp`, `strlen`, `memcpy`, `memset`, `_strupr` (inside `GetUserByName`).
- Persistent artifacts: `../../Common/Guilds.txt` (append, `:222`), `../../Common/Guild_<gg>_<ss>.txt` (rewrite via `CReadFiles::WriteGuild`, path template `CReadFiles.cpp:42`), `TM_*.txt` general log and `chat_*.txt` chat log (`Server.cpp:9220`, `:9257`).
- In-memory catalogs: `g_pMessageStringTable[2000][128]` (`Basedef.cpp:742`), `GuildInfo[65536]`, `g_pGuildName[group][server][guild]`, `g_pGuildAlly[65536]`, `g_pGuildWar[65536]`.

---

## 6. Afferent and Efferent Coupling

Coupling is measured in distinct external symbols/messages referenced (compile-time calls plus runtime globals), as required for a procedural C component where the unit of coupling is the function and the global symbol rather than a class.

### 6.1 Unit-level coupling

| Unit | Afferent Coupling | Efferent Coupling | Critical |
|---|---|---|---|
| `Exec_MSG_MessageWhisper` (whole function) | 2 (dispatcher, header/build) | 14 module groups (imple, SendFunc, GetFunc, Server.cpp helpers, Basedef BASE_*, CReadFiles, CPSock, DBSrv socket, 6 global state groups) | High |
| Command router + `/gm` delegation (`:20-30`, `:787-793`) | 2 | 6 (`ProcessImple`, `SendClientMessage`, `Log`, `pUser`, `pMob`, `g_pMessageStringTable`) | High |
| Guild lifecycle cluster (`/create`, `/subcreate`, `/abandonar`, `/handover`, `/gfame`, `/fimguerra`, `/fimirma`, `/expulsar`) | 1 (router) | 12 (`DBServerSocket`, `GuildInfo`, `g_pGuildName`, `GuildCounter`, `CReadFiles::WriteGuild`, `BASE_InitializeGuildName`, `BASE_GetGuildName`, `GetCreateMob`, `GridMulticast`, `BASE_GetSubGuild`, `DoDeprivate`, `Log`) | High |
| Summon / relocation cluster (`/summon`, `/relo`, `/summonguild`) | 1 | 8 (`DoSummon`, `DoTeleport`, `SummonGuild2`, `GetAttribute`, `BASE_GetVillage`, `BASE_GetSubGuild`, `GetUserByName`, `Log`) | High |
| Chat channel cluster (guild, ally, party, kingdom, citizen) | 1 | 6 (`SyncMulticast`, `SyncKingdomMulticast`, `pUser[].cSock.AddMessage`, `ChatLog`, `g_pGuildAlly`, `BASE_GetGuildName`) | Medium |
| Private message cluster (resolution, privacy, profile, relay) | 1 | 6 (`GetUserByName`, `SendClientMessage`, `BASE_GetGuildName`, `ChatLog`, `pUser[].LastChat`, `pUser[].Whisper`) | Medium |
| Utility/query cluster (`/cp`, `/getout`, `/srv`, `/qst`, `/nt`, `/nig`, `/time`, `/tab`, `/snd`, `/day`, `/kingdom`, `/king`, `/pin`, `/not`, `/wp`, `/contaprincipal`) | 1 | 10 (`GetPKPoint`, `GetGuilty`, `GetCreateMob(Trade)`, `GridMulticast`, `DBServerSocket`, `SendClientMessage`, `Log`, `pMob.extra`, `NewbieEventServer`, time/CRT) | Medium |
| Global mute gate (`:1129-1133`) | 1 | 2 (`pUser[].MuteChat`, `SendClientMessage`) | Low |

### 6.2 Reading of the coupling profile

The architectural report's afferent 2 / efferent 14 is confirmed exactly: the function is **maximally efferent and minimally afferent** — a leaf consumer of 14 collaborator groups that nothing else calls. This is the signature of a "god handler": high instability (I = Ce/(Ca+Ce) = 14/16 = 0.875) and no reuse. Every change to `SendFunc`, `GetFunc`, `Server.cpp` helpers, `Basedef` rules, `CReadFiles`, the `CUser`/`CMob` layouts, or eight DB message schemas propagates into this file; conversely, changes inside this file affect nothing else, which is why it can harbour 20+ dead guards and mask bugs without any compile-time signal.

Cohesion is temporal and lexical rather than functional: `/pin` (donation security) shares a function with `/create` (guild economy) and `@` (global chat) only because the client encodes all three in the same packet type. The 29 `#pragma region` labels are the de facto module boundaries and correspond one-to-one with the coupling clusters above.

Global-state coupling deserves separate mention: the function reads and writes six 65,536-entry arrays and two 1,000-entry object arrays without any accessor, and it uses the single process-global `char temp[4096]` (`Server.cpp:277`) as its formatting scratch space in 44 call sites. That buffer is shared with every other handler in the process, so any nested or interleaved formatting (for example a `Log` call writing `temp` while `temp` is an argument) can observe another path's content.

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| Client ↔ TMSrv (`_MSG_MessageWhisper`) | External client protocol | All commands, channels, PMs | Winsock TCP, custom framing + XOR-ish obfuscation + rolling checksum | 128-byte fixed struct (`_MSG` 12 + `MobName[16]` + `String[100]`) | Framing validated (`Size` 12..8192); checksum mismatch tolerated and packet still processed (`CPSock.cpp:457-464`); **no per-type size validation** (`BASE_CheckPacket` stub); all semantic failures are silent or message-panel only |
| TMSrv → peer clients | In-process socket fan-out | Channel/PM delivery, entity refresh | Same framing via `CPSock::AddMessage` | Reused `MSG_MessageWhisper`, `MSG_CreateMob`, `MSG_CreateMobTrade`, `MSG_PKInfo`, `MSG_Action`, `MSG_MessagePanel`, `MSG_SendItem` | `AddMessage` checks socket validity and send-buffer capacity, logs `err,add buffer invalid/full` and drops the frame (`CPSock.cpp:518-533`); sender is not notified |
| TMSrv → DBSrv | Internal service | Guild persistence, war/alliance, transfer, trumpet, notice, PIN, primary account | Same framing over `DBServerSocket` | 8 message types (Section 5.3) | Fire-and-forget: `SendOneMessage` returns no correlation; no timeout, retry, or acknowledgement; DB-side validation is partial (`CFileDB.cpp:1942`, `:2060`) |
| DBSrv → all TMSrv + admin clients | Internal broadcast | Global trumpet and notices | Same framing | `MSG_MagicTrumpet` (108 B), `MSG_DBNotice` (108 B) | Iterates `pUser[0..MAX_SERVER-1]` and `pAdmin[0..MAX_ADMIN-1]`, skipping empty sockets (`CFileDB.cpp:1593-1609`, `:1614-1621`); no delivery guarantee |
| `../../Common/Guilds.txt` | Local file | Append guild ownership record | `fopen("a+")` / `fprintf` | Text line `Group ServerIndex GuildCounter Name` | On `fopen` failure: log and return **after** state mutation (`:224-228`); no atomic write, no fsync, no lock |
| `../../Common/Guild_<gg>_<ss>.txt` | Local file | Guild counter + city/war/tax state | `fopen("wt")` / `fprintf` via `CReadFiles::WriteGuild` | Text tokens | On failure: log only (`CReadFiles.cpp:840-845`); truncating write can lose prior state |
| General log (`TM_*.txt`) | Local file | Command audit, errors | `fprintf(fLogFile, LogTemp)` | Free text with timestamp/account/IP | No failure handling on write; format string taken from data (`Server.cpp:6720-6723`) |
| Chat log (`chat_*.txt`) | Local file | Chat and PM audit | `fprintf(fChatLogFile, LogTemp)` | Free text | No failure handling; same format-string exposure (`Server.cpp:9241-9244`) |
| `g_pMessageStringTable[2000][128]` | In-memory catalog | All user-facing strings and some command aliases | Direct array index | Fixed 128-byte strings | No bounds check at the call sites; missing entries yield empty strings, breaking localized aliases |
| `g_pGuildName[group][server][guild]` | In-memory catalog | Guild name resolution and war/alliance eligibility | Direct array index | 12-byte names | Bounds checked in `BASE_GetGuildName` (`Basedef.cpp:1323-1330`) but **mis-indexed** by the caller's mask at `:954`/`:1011` |

There is no HTTP, gRPC, database driver, message broker, cache, or queue integration. All persistence is text files, and all inter-process communication is the proprietary `CPSock` framing over TCP to DBSrv.

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Command Object (string-keyed) | 29-branch `if/else if` chain on `MobName` | `:33-1127` | Dispatch text commands without a table or vtable |
| Chain of Responsibility | Ordered guard clauses; first matching branch consumes the packet | `:33-1133` | Precedence: commands → mute → channels → PM |
| Facade (privileged subsystem) | `/gm` forwards raw text to `ProcessImple` | `:787-793` → `imple.cpp:46` | Decouple the chat surface from 39 admin handlers |
| Mediator / Message Bus | DBSrv relays trumpet and notices to all game servers | `CFileDB.cpp:1591`, `:1612` | Cross-server broadcast without peer-to-peer links |
| Publish/Subscribe (implicit) | `SyncMulticast`, `SyncKingdomMulticast` fan-out | `SendFunc.cpp:260`, `:274` | Channel semantics implemented as audience enumeration |
| Guard Clauses / Fail-Silent | 29 early returns without feedback | `:30,59,62,65,84,123,171,181,190,276,290,299,316,371,420,423,663,676,694,702,709,713,721,731,739,747,1150,1191,1197` | Minimal branching cost; implicit "no such command" |
| Service Locator (global registries) | `pUser[1000]`, `pMob[MAX_MOB]`, `GuildInfo[65536]` accessed by index | throughout | No dependency injection; direct array access |
| Null-Object / sentinel | Index 0 reserved for "not found" (`GetUserByName` returns 0) | `Server.cpp:6812`, `:6826` | Avoid exceptions in C |
| Marker-byte protocol extension | `String[94] = 1` (trumpet), `String[96] = 3` (guild chat) | `:136`, `:1143` | Client-side channel styling without a new message type |
| Record/Field overloading | `MobName` = command word, recipient name, or empty channel selector; `ID` = sender or recipient | `:1136`, `:1284`, `:1325`, `:1172` | Fit many operations into one fixed 128-byte frame |
| Token-based parsing | `sscanf("%s %s")` / `sscanf("%d")` on the payload | `:56`, `:281`, `:1049`; `imple.cpp:68` | Argument extraction without a grammar |
| Two-tier delegation | Local validation, then DB-side re-validation | `:52-78` / `CFileDB.cpp:1929`; `:787` / `imple.cpp:89` | Defense in depth (partial) |
| Store-and-forward | `AddMessage` queues; transmission on the peer's send cycle | `CPSock.cpp:513` | Non-blocking fan-out |
| Template Method (implicit) | Identical guard sequences duplicated for `/fimguerra` and `/fimirma`, and for `@@`/`@` | `:930-1041`, `:1229-1279` | Copy-paste reuse |
| Layered regions | 29 `#pragma region` blocks as pseudo-modules | throughout | Compensates for the absence of functions |

Architectural decisions embedded in the component (observable, not documented elsewhere):

1. **Single-packet multiplexing.** All social and privileged text operations share message type 52. This preserves client compatibility with the original binary but couples 29 command schemas, 6 channel schemas, and the admin interpreter entry to one struct.
2. **Level-as-privilege.** `Level - 1000` is the only privilege derivation; there is no role table, no `Admin` flag use, no per-command ACL. Admin capability therefore inherits the integrity of the character level field.
3. **DB as broadcast bus.** Cross-server fan-out (trumpet, notice) is routed through DBSrv rather than peer links, while same-server chat bypasses the DB entirely. The two paths have different reliability characteristics.
4. **Silence as the default failure mode.** The component favours no-reply over error replies, except where a language-table string already existed. This keeps the client-facing surface small but makes operational diagnosis depend entirely on the log sinks (which are themselves incomplete — 16 commands have no audit entry).
5. **Session-scoped social state.** `Tab`, `Snd`, and all five chat flags live on `CUser`/`CMob`, not on `STRUCT_MOB`, so they are lost on logout and not shared across servers.
6. **File-based persistence for guild state, split across two files** (`Guilds.txt` append + `Guild_gg_ss.txt` rewrite) with no atomicity between them.

---

## 9. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|---|---|---|---|
| High | Packet validation | `BASE_CheckPacket` body commented out and returns `FALSE` (`Basedef.cpp:6476-6595`); `_PACKET_DEBUG` only in `Debug|Win32` (`TMSrv.vcxproj:61`) | No message-type size validation anywhere; the handler parses fixed offsets from client-declared lengths |
| High | Input bounds | Unconditional writes `MobName[15]=0`, `String[99]=0` into the shared receive buffer before any length check (`:26-27`) | With `Size = 12`, pending packets in the same 128 KB receive buffer are corrupted; parsing desynchronises |
| High | Authorization | GM privilege is `Level - 1000` with no local check (`:787-793`); `CUser::Admin` never consulted; `IsImple` stubbed (`Server.cpp:6829`) | Any level inflation or GM item becomes remote command execution over items, scores, timers, billing, mob spawns, kicks |
| High | Guild creation | Coins deducted (`:213`) and guild id assigned (`:210`) before `fopen` (`:222`); failure returns without rollback (`:224-228`) | Permanent currency loss and duplicate guild ids on persistence failure |
| High | Audit sink | `Log`/`ChatLog` pass data-composed buffers as `fprintf` format strings (`Server.cpp:6720-6723`, `:9241-9244`) | Player-controlled `%n`/`%s` in chat or command text is interpreted by `fprintf`; log corruption and potential memory writes |
| High | Test coverage | Zero tests, zero test project, zero CI (Section 10) | Every rule above is verified only by manual play; regressions are undetectable |
| Medium-High | `/pin` | `DonateInfo.Count` read but never incremented (`:1059`); DB handler is a commented-out stub (`CFileDB.cpp:2064`) | Intended 3-attempt/2-hour brute-force cap is unreachable; the feature is a silent no-op |
| Medium-High | `/srv` | Any player may initiate a cross-server transfer; only range checks exist (`:58-65`); DB re-validates range only (`CFileDB.cpp:1942`) | Unrestricted self-service transfers; failed persistence leaves the account key at the destination |
| Medium-High | Guild leadership | `/abandonar` as leader sends no `_MSG_GuildInfo` and clears no sub flags (`:378-403`); `/handover` promotes without checking the target's rank or clearing its sub flag (`:451-452`) | Orphaned guilds, leader occupying a sub slot, stale fame until the next DB refresh |
| Medium | `/fimguerra`, `/fimirma` | Name lookup masks with `MAX_GUILD` instead of `MAX_GUILD - 1` (`:954`, `:1011`) | Guild #0's name decides war/alliance eligibility for every guild on the server |
| Medium | Dead guards | `conn <= 0 && conn >= MAX_USER` (`imple.cpp:91`, `:1832`); `myleader == 0` / `myguild == 0 && myleader == 0` after normalisation (`:706`, `:847`) | Intended `conn` guard never fires; "party or guild" summon rule silently becomes "same party only" |
| Medium | Uninitialised stack | `cmd[128]`, `sval1..8[128]` uninitialised, `sscanf` return unchecked (`imple.cpp:48-68`) | `/gm` with an empty body executes `strcmp` chains on indeterminate data — undefined behaviour reachable by any player |
| Medium | `/qst` | `if/else if` ladder without a final `else` but unconditional `SendClientMessage(temp)` (`:148-163`) | `Cristal > 4` returns stale content of the global `temp` (previous request's text, possibly another player's data) |
| Medium | Unterminated strings | `strncpy(pMob.Snd, m->String, 96)` into `Snd[96]` (`:564`); `strncpy(sub, SubMobName, 16)` into `sub[16]` then `GetUserByName` (`:285-287`) | `%s` reads continue into adjacent `CMob` fields; `strcmp` over-reads a stack buffer |
| Medium | Global scratch buffer | 69 uses of the process-global `char temp[4096]` (`Server.cpp:277`) | Cross-handler content leakage; overlapping `sprintf` sources/destinations at `:132-140`, `:566` |
| Medium | No rate limiting on PM/commands | Only `@@`/`@` are limited (`:1231`, `:1258`); PM, guild, party, trumpet and all 29 commands are unlimited | PM flood, guild/party spam, inventory-consumption races on `/spk`, `/create` retry storms |
| Medium | `/not` gating | Level ≥ 1000 asserted inline in the branch condition (`:1088`), above the mute gate | Any level-1000 character can broadcast a group-wide notice; a muted GM can too |
| Medium | Mute semantics | Mute gate placed after the entire command chain (`:1129`) | Muted players retain `/create`, `/summon`, `/relo`, `/gm`, `/srv`, `/not`, etc. |
| Low-Medium | Convergence of `/kingdom`,`/king` | No liveness, trade, or map-attribute checks (`:582-608`) unlike `/summon`/`/relo` | Free movement from dead/trading state; inconsistent rule classes between similar commands |
| Low-Medium | `/summon` positional gate | `TargetX & 0xFF00 || TargetY & 0xFF00` required (`:750`) | Summon fails in all low-coordinate regions with the misleading "can't use here" message |
| Low-Medium | Localized aliases | `/kingdom`, `/king`, `/summon`, `/relo`, `/summonguild`, `/r` compare against `g_pMessageStringTable` (`:582`, `:597`, `:611`, `:648`, `:796`, `:1291`) | Aliases vanish with a different client message table; command surface becomes client-locale dependent |
| Low-Medium | `/cp` display | `GetPKPoint(conn) - 75` with no clamping (`:35`) | Negative values displayed for characters below 75 points |
| Low | `/spk` buffer sizing | `memset(&sm_mt, 0, sizeof(MSG_STANDARDPARM))` (16 B) for a 108-byte struct (`:126`); marker written at `String[94]` of a 96-byte field (`:136`) | Latent only (the following `strncpy` pads 96 bytes); long messages lose their trailing characters to the marker |
| Low | Inconsistent truncation constants | `String[NAME_LENGTH-1]` / `[NAME_LENGTH-2]` applied to a 100-byte field (`:425-426`, `:664-665`, `:819-820`, `:916-917`, `:1286-1287`) | 14-character name limit achieved accidentally; 85 payload bytes discarded without intent |
| Low | Redundant code | `group`/`Server` computed and unused (`:87`); `checkguild != myguild` compares a field to itself (`:941-944`, `:998-1001`); `PartyList` delivery can duplicate to the leader | Noise that obscures the real predicates |
| Low | Audit gaps | No audit for `/srv`, `/qst`, `/tab`, `/pin`, `/fimguerra`, `/fimirma`, `/expulsar`, `/wp`, `/contaprincipal` | State-changing operations without accountability |
| Low | Hardcoded content | Portuguese refusal text `"Aguarde 3 segundos."` (`:1239`, `:1266`), contract names (`:152-161`), magic item index 3330, magic level 1000, magic bits `0x40000`/`0x04`/`0x40` | Localization and configuration debt; behaviour cannot be tuned without recompilation |
| Low | Path coupling | `/create` writes a hardcoded `../../Common/Guilds.txt` (`:222`) while the counter uses `GUILD_PATH` (`CReadFiles.cpp:42`) | Two persistence locations for one aggregate; misconfigured working directory splits guild state |
| Low | `/day` | Sends the unexplained literal `"!#11  2"` (`:576`) | Client-protocol dependency with no server-side documentation |

No transaction, retry, circuit-breaker, timeout, or idempotency mechanism exists anywhere in the component. The only resilience primitives are early returns, `AddMessage` capacity checks, and DB-side re-validation of two of the eight outbound messages.

---

## 10. Test Coverage Analysis

### 10.1 Search methodology

1. Recursive enumeration of the project root excluding `.agents`, `.codegraph`, `.opencode`, `openspec` and `.git`: **486 files**.
2. Filename search for `test`, `spec`, `unittest`, `gtest`, `catch` (case-insensitive) across all files: **0 matches**.
3. Content search across all `.cpp`, `.h`, `.vcxproj` for test frameworks and assertion macros (`gtest`, `CppUnit`, `catch2`, `doctest`, `TEST(`, `assert(`): **0 matches**.
4. Build-system inspection: `W2PP Code Project.sln` declares exactly two projects (`DBSrv`, `TMSrv`) and no test project; no `CMakeLists.txt`, `Makefile`, `meson.build`, `conanfile`, or `vcpkg.json` exists at the root; no `.github/workflows`, `.gitlab-ci.yml`, `azure-pipelines.yml`, or `Jenkinsfile` exists.
5. `README.md` (2,594 bytes) contains no build or test instructions — the only process sentence is "We use the latest client build to connect this release."
6. `Code/TMSrv/TMSrv.vcxproj` lists 75 `ClCompile` entries, none of which is a test target; the `ItemDefinitionGroup` entries contain only `Debug|Win32` and `Release|Win32` compiler settings.
7. Codegraph blast-radius queries for `Exec_MSG_MessageWhisper`, `ProcessImple`, `ProcessClientMessage`, and `ProcessDBMessage` each report "no tests found within 3 caller hops".

### 10.2 Coverage table

| Component / Unit | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `Exec_MSG_MessageWhisper` (whole function) | 0 | 0 | 0% | No test project, no framework, no fixtures; verification is manual play only |
| Command router and dispatch precedence (`:33-1127`) | 0 | 0 | 0% | 29 commands across 5 chains unreachable by automated means |
| `/gm` delegation and level tiers (`:787-793`, `imple.cpp:89-1845`) | 0 | 0 | 0% | The most privilege-sensitive path has no negative test for level < 1001 |
| Guild lifecycle (`/create`, `/subcreate`, `/abandonar`, `/handover`) | 0 | 0 | 0% | No test for the non-atomic `fopen`-after-mutation path or the zero-counter path |
| Summon / relocation (`/summon`, `/relo`, `/summonguild`) | 0 | 0 | 0% | No test for the dead `myleader == 0` predicate or the coordinate gate |
| Chat channels (`-`, `--`, `=`, `@@`, `@`) | 0 | 0 | 0% | No test for audience enumeration, opt-out flags, or the shared rate bucket |
| Private message (resolution, privacy, profile, relay) | 0 | 0 | 0% | No test for `/r` aliasing, `+account` lookup, self-whisper, or whisper-block bypass |
| `/spk` trumpet consumption and fan-out | 0 | 0 | 0% | No test for zero-stock, stack > 1, or cross-server relay |
| `/pin` rate limiter | 0 | 0 | 0% | The defect (Count never incremented) would have been caught by a single unit test |
| `/fimguerra`, `/fimirma` name validation | 0 | 0 | 0% | The `& MAX_GUILD` mask bug would have been caught by one boundary test |
| Audit logging (`Log`, `ChatLog`) | 0 | 0 | 0% | No test for format-string payloads or per-command log presence |
| Packet-size validation (`BASE_CheckPacket`) | 0 | 0 | 0% | The stub's `return FALSE` makes every size-related test vacuous |
| DBSrv handlers used by this component (`_MSG_DBServerChange`, `_MSG_MagicTrumpet`, `_MSG_DBActivatePinCode`, `_MSG_DBPrimaryAccount`) | 0 | 0 | 0% | The PIN handler is a no-op; no test observes the dropped message |
| Regression / golden-master of the wire struct | 0 | 0 | 0% | `sizeof(MSG_MessageWhisper) == 128` is enforced only by the commented-out table at `Basedef.cpp:6502` |

### 10.3 Assessment

The project has **no automated verification of any kind** — no unit tests, no integration tests, no protocol conformance tests, no static-analysis configuration, and no CI. For this component the absence is materially worse than for a typical handler, because the majority of the 38 documented business rules are *deny-by-default guards* whose correctness can only be observed in the negative case (a summon that should be refused, a whisper that should be blocked, a guild creation that should fail). Manual play exercises almost exclusively the happy path.

The de facto verification mechanism is the runtime log: `Log()` writes `err,` lines for a handful of failure conditions (`:200`, `:206`, `:226`) and `adm <string>` for every GM command (`imple.cpp:81-87`), while `ChatLog()` records accepted chat. These are diagnostic traces, not tests: they record what happened, cannot assert what should have happened, and are themselves subject to the format-string defect described in BR-37. Several defects found in this analysis (the `/pin` counter, the `& MAX_GUILD` mask, the unreachable attempt cap, the missing `/qst` default) are exactly the class that a single table-driven unit test of the pure decision functions would expose; none can be detected from the logs because the failing paths write nothing.

Secondary risk: because `BASE_CheckPacket` is compiled out and its body is dead, even a future protocol test that sends a malformed `Size` would observe "accepted" behaviour identical to production, giving false confidence that the validation path exists.

---

## 11. Ambiguities and Confidence Notes

| Item | Statement | Confidence |
|---|---|---|
| `String[96] = 3` marker in guild chat (`:1143`) | Client-side channel discriminator; no server-side consumer exists in this repository | Medium (protocol evidence only; client source absent) |
| `/day` payload `"!#11  2"` (`:576`) | Client UI command smuggled through the message-panel text field, consistent with `/nig`'s `"!!"` prefix | Low |
| Map attribute bits `0x04` / `0x40` (`:637`, `:730`, `:875`) | "Summon/recall restricted" and "PK/safe" style flags; used consistently as blocking bits in this file but never defined symbolically | Medium |
| `0x40000` learned-skill bit (`:720`, `:863`) | The summon skill bit; corroborated by unrelated call sites using neighbouring bits for other skills | Medium-High |
| `Cristal` values > 4 (`:148-163`) | No writer emits > 4 in the visible codebase, so the stale-`temp` path requires externally modified character data | Medium-High |
| `DonateInfo.Count` (`:1059`) | Read-only in the entire codebase; no writer exists, so the 3-attempt cap cannot trigger | High |
| `/contaprincipal` intent | "Make this account the primary account for the MAC", confirmed by the DB reply text "Sua conta agora é a primária." | High |
| `/gfame` unused `Server` local (`:87`) | Vestigial from a per-server fame design; no effect | High |
| `NewbieEventServer` forcing `PKInfo.Parm = 1` (`:543-555`) | During a newbie event every character is rendered in the "hostile" name colour | Medium |
| Whether `Snd`/`Tab` are client-visible elsewhere | `Snd` is read only by the PM profile card; `Tab` only by `MSG_CreateMob`/`MSG_CreateMobTrade` | High (exhaustive project search) |
| Missing test evidence | No files, projects, CI definitions, or framework references exist anywhere in the 486-file tree | High |

---

*End of report.*
