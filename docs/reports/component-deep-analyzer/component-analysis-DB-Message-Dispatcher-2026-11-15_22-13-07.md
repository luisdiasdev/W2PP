# Component Deep Analysis Report - DB Message Dispatcher

| Attribute | Value |
|-----------|-------|
| Component | DB Message Dispatcher |
| Primary location | `Code/TMSrv/ProcessDBMessage.cpp` (1,330 lines), `Code/TMSrv/ProcessDBMessage.h` (42 lines) |
| Owning executable | TMSrv (`Code/TMSrv/TMSrv.vcxproj`), Win32 x86 GUI application |
| Architectural role | Inbound Adapter / Message Router for the TMSrv <-> DBSrv protocol |
| Protocol peer | DBSrv over TCP 7514, custom binary framing implemented by `Code/CPSock.cpp` |
| Analysis date | 2026-11-15 22:13:07 |
| Analyzed scope | Entire project root `E:\wyd\test-setup\w2pp`, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` |
| Method | Full read of the component, its header, all call sites, all message producers in DBSrv, the shared protocol contract `Code/Basedef.h`, transport `Code/CPSock.cpp`, and every TMSrv function the dispatcher invokes |

---

## 1. Executive Summary

### 1.1 Purpose

The DB Message Dispatcher is the single entry point through which TMSrv (the game/world server) consumes every message produced by DBSrv (the account/character store). It is a single free function, `void ProcessDBMessage(char *Msg)` (`Code/TMSrv/ProcessDBMessage.cpp:39`), invoked from exactly one place: the `WSA_READDB` branch of `MainWndProc` (`Code/TMSrv/Server.cpp:3914`). It receives an already de-framed, already de-obfuscated packet buffer from `CPSock::ReadMessage` and performs type-based dispatch over 45 message types, split into two disjoint namespaces by the `ID` header field.

### 1.2 Role in the system

The component is the *inbound half* of the game-server-to-database conversation. Outbound requests are emitted ad hoc from many TMSrv files (`_MSG_AccountLogin.cpp:92`, `_MSG_CharacterLogin.cpp:81`, `Code/TMSrv/Server.cpp:7082`, `Code/TMSrv/MobKilled.cpp:341`, and 40 other sites). All responses, and all unsolicited database-to-game directives (notices, guild reports, server index assignment, ranking updates, web-shop deliveries), converge here. It therefore acts as:

* the protocol terminator for `FLAG_DB2GAME` traffic;
* the authority that converts database state (account cargo, coin, character record, extra blob, affects, donate balance) into live session state (`pUser[]`, `pMob[]`, `pMobGrid`);
* the policy enforcement point for several cross-cutting rules that exist nowhere else (single-primary-account-per-MAC downgrade, MAC block, item-effect normalization, carry-capacity derivation, spawn-point selection, kill-mark injection);
* a relay that rewrites `Type`/`ID` and forwards packets verbatim to the game client (11 of the 45 branches are pure relays).

### 1.3 Key findings

1. **Two disjoint dispatch namespaces keyed by the `ID` field.** `ID == 0` means "server-scoped directive" (11 handlers, `ProcessDBMessage.cpp:56-185`); `ID` in `[1, MAX_USER)` means "player-scoped response" (34 handlers, `ProcessDBMessage.cpp:188-1328`). DBSrv deliberately hardcodes `ID = 0` for server-scoped messages (for example `Code/DBSrv/CFileDB.cpp:2190` for `_MSG_GuildInfo`, and `SendDBSignalParm3(i, 0, _MSG_DBSetIndex, ...)` at `Code/DBSrv/CFileDB.cpp:503`). There is no default branch in either switch, so any type outside these 45 is silently discarded.
2. **The component is the largest single-function state mutation site in TMSrv.** The `_MSG_DBCNFCharacterLogin` branch alone spans `ProcessDBMessage.cpp:635-1081` (447 lines) and mutates 30+ distinct fields on `pUser[conn]`, `pMob[conn]` and `pMobGrid` before emitting 9 client packets and 3 broadcast calls.
3. **No payload validation.** `BASE_CheckPacket` - the only per-type size validator in the codebase - has its entire body commented out (`Code/Basedef.cpp:6475-6476`) and is only reachable under `#ifdef _PACKET_DEBUG`. The dispatcher never verifies `std->Size` against the cast structure, and never validates payload indices before using them as array subscripts (`GuildInfo[m->Guild]` at line 103, `g_pItemList[m->Item.sIndex]` at lines 272 and 304).
4. **Several branches contain defects that are observable from code alone and are documented in section 10**, including a stack-content disclosure to the client (line 222), a missing `return` after a MAC-block close (lines 369-383), a session-slot leak on both the save-and-quit completion path (lines 1172-1183) and the MAC-block path, and a dead effect-normalization block whose results are overwritten 88 lines later (lines 654-696 vs line 784).
5. **Testability is effectively zero.** There are no test projects, no test framework references, no test files and no CI configuration anywhere in the 104 source files. The component binds directly to 27 global objects and 39 free functions, which makes any harness expensive. See section 11.
6. **The file has been functionally frozen since 2018.** `git log` shows the last change touching `ProcessDBMessage.cpp` is `e306ab8` ("Update ProcessDBMessage.cpp", 3 lines, 2018-05-22); the three other commits are the initial import, a license header change, and a warnings/build cleanup.

---

## 2. Data Flow Analysis

### 2.1 Transport path into the component

```
DBSrv.exe ──TCP 7514──> TMSrv.exe MainWndProc (Win32 message pump, single thread)
                              |
                              +-- case WSA_READDB  (Server.cpp:3812)
                                    |
                                    +-- WSAENUMEVENT != FD_READ -> close + reconnect (Server.cpp:3815-3848)
                                    +-- DBServerSocket.Receive()  (CPSock.cpp:336)  [recv into 128 KB ring]
                                    +-- loop:
                                          DBServerSocket.ReadMessage(&Error,&ErrorCode)  (CPSock.cpp:353)
                                            - INITCODE handshake 0x1F11F311 (CPSock.cpp:366-383)
                                            - Size range check 12 <= Size <= 8192 (CPSock.cpp:397)
                                            - in-place de-obfuscation of bytes [4..Size) (CPSock.cpp:430-453)
                                            - checksum mismatch => ErrorCode=1, packet still returned (CPSock.cpp:458-464)
                                          if (Msg == NULL) break;
                                          if (Error == 1 || Error == 2) { log; break; }   (Server.cpp:3907-3912)
                                          ProcessDBMessage(Msg);                          (Server.cpp:3914)
```

The dispatcher therefore never sees raw socket data: framing, length limiting, obfuscation and the first checksum pass already happened. It does, however, run *inside the Win32 window procedure*, on the same thread that services client sockets, timers and all gameplay. There is no locking anywhere in the codebase, and this is safe only because of that single-threaded design.

### 2.2 Internal flow of `ProcessDBMessage`

```
1.  Envelope gate                 (ProcessDBMessage.cpp:43-52)
    (Type & FLAG_DB2GAME) and 0 <= ID < MAX_USER, else log "err,packet ..." and return
2.  conn = std->ID                (line 54)
3.  if conn == 0  -> server-scope switch (11 cases, lines 56-185)
4.  else          -> player-scope prologue:
      4a. Mode == USER_EMPTY ?  -> send _MSG_DBNoNeedSave to DBSrv, return   (lines 190-200)
      4b. conn range guard (unreachable)                                     (lines 202-208)
      4c. switch over 34 cases                                               (lines 210-1327)
5.  Branch either:
      - mutates pUser[conn] / pMob[conn] / pMobGrid,
      - calls SendFunc/SendFunc-like helpers that append to pUser[conn].cSock,
      - optionally writes a reply back to DBSrv with DBServerSocket.SendOneMessage (6 sites),
      - optionally calls CloseUser / SaveUser / CharLogOut.
6.  Return to MainWndProc loop; queued client data is flushed later by
    ProcessSecTimer -> cSock.SendMessageA() (ProcessSecMinTimer.cpp:201-221, 500 ms timer).
```

### 2.3 Flow A - account login (highest fan-out of the inbound path)

```
Client --_MSG_AccountLogin--> ProcessClientMessage (ProcessClientMessage.cpp:68)
      -> Exec_MSG_AccountLogin (_MSG_AccountLogin.cpp:21): version check, Mode==USER_ACCEPT check,
         fail-count check (>=3 rejects), copy AdapterName into pUser[conn].Mac, Mode=USER_LOGIN
      -> DBSrvSocket _MSG_DBAccountLogin (ID=conn)
   DBSrv CFileDB::ProcessMessage case _MSG_DBAccountLogin (CFileDB.cpp:611):
         read/create account file, MAC capture, name-swap fix, DBGetSelChar
      -> _MSG_DBCNFAccountLogin (ID=conn, Cargo[], Coin, sel, AccountName)
         [optionally followed by _MSG_DBCheckPrimaryAccount with ID=0 to every TMSrv  (CFileDB.cpp:782-801)]
   TMSrv ProcessDBMessage:
         guard Mode != 0                                   (line 190)
         strcmp(pUser[conn].AccountName, m->AccountName)    (line 358) -> mismatch: kick
         pMac[] MAC-block scan                             (lines 369-383)
         reset 10 client-side flags, Mode = USER_SELCHAR   (lines 386-400)
         rewrite Type=_MSG_CNFAccountLogin ID=30002        (lines 397-398)
         normalize cargo effects for nPos 64/192           (lines 402-422)
         evDelete purge of sIndex 470..500                 (lines 424-431)
         forward packet to client                          (line 434)
         pUser[conn].Cargo = m->Cargo, Coin, SelChar       (lines 436-440)
         billing gate when BILLING>0 && IsFree(sel)        (lines 443-451)
         TransperCharacter? forward _MSG_TransperCharacter (lines 459-470)
```

### 2.4 Flow B - character login (state construction)

```
Client --_MSG_CharacterLogin--> Exec_MSG_CharacterLogin (_MSG_CharacterLogin.cpp:21)
      -> slot range, FREE/BILING gating, Mode must be USER_SELCHAR
      -> Mode=USER_CHARWAIT, pMob[conn].Mode=MOB_USER
      -> _MSG_DBCharacterLogin (ID=conn)
   DBSrv CFileDB.cpp:1028 -> reads char[Slot], mobExtra, ShortSkill, affect, Donate
      -> _MSG_DBCNFCharacterLogin (ID=conn)
   TMSrv ProcessDBMessage branch at 635:
      normalize equip/carry effects -> class-master equip[0] injection -> HP floor
      pMob[conn].MOB = m->mob ; extra copy ; Citizen from GuildInfo ; affects ; ShortSkill
      MaxCarry = 30 (+15 / +15) ; Merchant=0 ; Arch MortalLevel ; BASE_GetHpMp ; GetCurrentScore
      BASE_GetBonusSkillPoint ; BASE_GetBonusScorePoint ; Mode=MOB_USER
      build MSG_CNFClientCharacterLogin (ID=30000 or 30001 for newbie-event server)
      kill-mark item injection in Carry[63]
      spawn resolution: city spawn (Merchant bits 6-7) -> guild zone override -> newbie spot
                        -> GetEmptyMobGrid
      Mode=USER_PLAY ; GetCurrentScore ; guild mantle/clan correction
      send login packet ; GetCreateMob + pMobGrid + GridMulticast
      SendPKInfo ; SendGridMob ; MountProcess ; SendWarInfo ; castle state ; ClearCrown
      EXP segment (0..3) ; billing at FREEEXP ; Kefra notice ; RvR bonus notice
      SendEtc ; SendScore ; write "sta,Login char:..." log line
```

### 2.5 Flow C - web/item delivery with write-back to the database

```
Operator drops file in Common/ImportItem -> DBSrv CReadFiles::ImportItem (CReadFiles.cpp:200+)
   DBSrv computes a cargo position itself (CReadFiles.cpp:286-308) and writes the account file
   -> _MSG_DBSendItem (Result uninitialized, ID = player slot)  (CReadFiles.cpp:242-253)
TMSrv ProcessDBMessage (line 229):
   Re-runs the same placement algorithm on the live pUser[conn].Cargo:
     phase 1: first fit over i in [0,125] using BASE_CanCargo(item, Cargo, i%9, i/9)
     phase 2: fallback scan i = 127 down to 0 for any empty slot
   success -> place item, SendItem(ITEM_PLACE_CARGO, i), client text, m->Result = 0,
              reply _MSG_DBSendItem(m, m->Size) to DBSrv, log "tra,...", SaveUser(conn,0) if Mode==USER_SELCHAR
   failure -> m->Result = 3, reply to DBSrv, SaveUser(conn,1), log "tra,..._fail_empty_"
```

Note that DBSrv has no handler for the `_MSG_DBSendItem` reply (grep over `Code/DBSrv/*.cpp` finds the constant only in `CReadFiles.cpp` as a producer), so the acknowledgement is discarded on the return path. The same holds for `_MSG_DBSendDonate` (produced at `CReadFiles.cpp:821`).

### 2.6 Flow D - server bootstrap and runtime directives (conn == 0)

```
DBSrv accepts TMSrv connection (DBSrv/Server.cpp:1140-1170):
   -> _MSG_DBSetIndex(Parm1=ServerGroup, Parm2=Sapphire, Parm3=ServerIndex)  [line 1159]
   -> _MSG_GuildInfo for every guild with Citizen != 0 (up to 65,536 packets)  [line 1161-1164]
   -> _MSG_TransperCharacter if a redirect.txt is configured                  [line 1174]
   -> _MSG_GuildReport from CFileDB.cpp:371   -> ChargedGuildList[][]
   -> _MSG_War / _MSG_GuildAlly (guild war and alliance propagation)
   -> _MSG_NPNotice(Parm1=1) / _MSG_DBNotice / _MSG_MessageDBImple (admin-originated broadcasts)
   -> _MSG_MagicTrumpet (echo of a TMSrv-originated global trumpet, ID forced to 0)
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation / Routing | Packet accepted only if `Type & FLAG_DB2GAME` and `0 <= ID < MAX_USER` (1000); otherwise logged as `err,packet` and dropped | `ProcessDBMessage.cpp:43-52` |
| BR-02 | Routing | `ID == 0` selects the 11-case server-scope switch; `ID > 0` selects the 34-case player-scope switch | `ProcessDBMessage.cpp:54-56`, `188-210` |
| BR-03 | Resilience / State | A player-scoped packet whose slot is `USER_EMPTY` triggers `_MSG_DBNoNeedSave` back to DBSrv and is dropped | `ProcessDBMessage.cpp:190-200` |
| BR-04 | Validation (dead) | Second `conn` range guard `conn <= 0 || conn > MAX_USER` - unreachable because of BR-01 | `ProcessDBMessage.cpp:202-208` |
| BR-05 | Routing | No `default:` label in either switch: unknown or out-of-namespace types are silently ignored | `ProcessDBMessage.cpp:184`, `1327` |
| BR-06 | Validation | Payload size is never compared with the cast structure; `m->Size` is trusted when replying | `ProcessDBMessage.cpp:278`, `310`, `327`, `1246` |
| BR-07 | State / Feature flag | `_MSG_TransperCharacter` switches the server into character-transfer mode | `ProcessDBMessage.cpp:61-67` |
| BR-08 | Configuration | `_MSG_DBSetIndex` assigns `ServerGroup` / `ServerIndex` / `Sapphire`; `Parm1 == -1` means "Sapphire only" | `ProcessDBMessage.cpp:69-81` |
| BR-09 | Domain (guild war) | `_MSG_War` executes the war state machine `DoWar(Parm1, Parm2)` | `ProcessDBMessage.cpp:83-89` |
| BR-10 | Domain (guild alliance) | `_MSG_GuildAlly` executes the alliance state machine `DoAlly(Parm1, Parm2)` | `ProcessDBMessage.cpp:91-97` |
| BR-11 | Domain (guild) | `_MSG_GuildInfo` replaces the in-memory guild record `GuildInfo[Guild]` | `ProcessDBMessage.cpp:99-105` |
| BR-12 | Domain (crown guilds) | `_MSG_GuildReport` overwrites the 10x5 `ChargedGuildList` matrix | `ProcessDBMessage.cpp:107-112` |
| BR-13 | Notification | `_MSG_NPNotice` with `Parm1 == 1` broadcasts the text to every playing user | `ProcessDBMessage.cpp:114-121` |
| BR-14 | Authorization | `_MSG_MessageDBImple` truncates the string and executes it through the admin interpreter with `conn = 0` and the packet-supplied privilege level | `ProcessDBMessage.cpp:123-134` |
| BR-15 | Notification | `_MSG_MagicTrumpet` is relayed to all playing users through `SyncMulticast(0, Msg, 0)` (deferred flush) | `ProcessDBMessage.cpp:136-139` |
| BR-16 | Notification | `_MSG_DBNotice` broadcasts through `SendNotice` (all `USER_PLAY` slots) | `ProcessDBMessage.cpp:141-146` |
| BR-17 | Security / Policy | `_MSG_DBCheckPrimaryAccount` enforces one primary account per MAC: other accounts get `OnlyTrade = 1`, lose party, and are recalled to a village | `ProcessDBMessage.cpp:148-182` |
| BR-18 | Feature gate | `_MSG_ReqTransper` is ignored unless `TransperCharacter != 0`; otherwise it is forwarded to the client and the session returns to `USER_SELCHAR` | `ProcessDBMessage.cpp:213-226` |
| BR-19 | Persistence / Inventory | `_MSG_DBSendItem` places a delivered item in cargo with a two-phase fit algorithm and acknowledges the result (0 success, 3 full) | `ProcessDBMessage.cpp:229-338` |
| BR-20 | Notification | `_MSG_NPNotice` with `Parm1 == 0` delivers the text to the single addressed user, only while `USER_PLAY` | `ProcessDBMessage.cpp:341-351` |
| BR-21 | Session establishment | `_MSG_DBCNFAccountLogin` validates identity and MAC block, resets chat/privilege flags, loads cargo/coin/SelChar and forwards the confirmation | `ProcessDBMessage.cpp:354-472` |
| BR-22 | Session establishment | `_MSG_DBNewAccountFail` signals `_MSG_NewAccountFail` and closes the session | `ProcessDBMessage.cpp:475-484` |
| BR-23 | Session establishment | `_MSG_DBAccountLoginFail_Account` reports "no account with that name" and closes | `ProcessDBMessage.cpp:487-498` |
| BR-24 | Session establishment | `_MSG_DBAccountLoginFail_Block` formats one of four block tiers (X/A/B/C) into the blocked-account template and closes | `ProcessDBMessage.cpp:501-524` |
| BR-25 | Session establishment | `_MSG_DBAccountLoginFail_Disable` reports a disabled account and closes | `ProcessDBMessage.cpp:527-538` |
| BR-26 | Authentication | `_MSG_AccountSecure` relays the numeric-token success signal to the client with `ID = 30000` | `ProcessDBMessage.cpp:541-544` |
| BR-27 | Authentication | `_MSG_AccountSecureFail` relays the numeric-token failure signal to the client | `ProcessDBMessage.cpp:547-550` |
| BR-28 | Rate limit | `_MSG_DBOnlyOncePerDay` reports the once-per-day restriction and flushes without closing | `ProcessDBMessage.cpp:553-562` |
| BR-29 | Session establishment | `_MSG_DBAccountLoginFail_Pass` reports a wrong password and closes | `ProcessDBMessage.cpp:565-576` |
| BR-30 | Character management | `_MSG_DBCNFNewCharacter` forwards the refreshed SelChar list and returns the session to `USER_SELCHAR` | `ProcessDBMessage.cpp:579-592` |
| BR-31 | Character management | `_MSG_DBCNFDeleteCharacter` forwards the refreshed SelChar list and returns the session to `USER_SELCHAR` | `ProcessDBMessage.cpp:595-606` |
| BR-32 | Character management | `_MSG_DBDeleteCharacterFail` signals delete failure and keeps `USER_SELCHAR` | `ProcessDBMessage.cpp:609-619` |
| BR-33 | Character management | `_MSG_DBNewCharacterFail` signals creation failure and keeps `USER_SELCHAR` | `ProcessDBMessage.cpp:622-632` |
| BR-34 | Session establishment (primary) | `_MSG_DBCNFCharacterLogin` reconstructs the whole live character (18 sub-rules SL-01..SL-18) | `ProcessDBMessage.cpp:635-1081` |
| BR-35 | Session establishment | `_MSG_DBCharacterLoginFail` signals failure, restores `USER_SELCHAR`, logs a crack entry | `ProcessDBMessage.cpp:1084-1096` |
| BR-36 | Notification | `_MSG_DBMessageBoxOk` relays a modal message box to the client with `ID = 0` | `ProcessDBMessage.cpp:1099-1108` |
| BR-37 | Concurrency control | `_MSG_DBAlreadyPlaying` signals "already playing" and terminates the session | `ProcessDBMessage.cpp:1111-1121` |
| BR-38 | Concurrency control | `_MSG_DBStillPlaying` signals "still playing" and terminates the session | `ProcessDBMessage.cpp:1124-1134` |
| BR-39 | Concurrency control | `_MSG_DBSavingQuit` force-terminates the session, optionally warning the occupant (`Mode` 0 or 1) | `ProcessDBMessage.cpp:1137-1169` |
| BR-40 | Session teardown | `_MSG_DBCNFAccountLogOut` completes a save-and-quit, clears the mob and closes | `ProcessDBMessage.cpp:1172-1183` |
| BR-41 | Arch character | `_MSG_DBCNFArchCharacterSucess` wipes equip slots 10 and 11, persists, logs the character out and plays the arch effect | `ProcessDBMessage.cpp:1186-1206` |
| BR-42 | Arch character | `_MSG_DBCNFArchCharacterFail` restores `MOB_USER` and reports "no empty slot" | `ProcessDBMessage.cpp:1209-1214` |
| BR-43 | Commerce | `_MSG_DBSendDonate` credits the cash balance and acknowledges to DBSrv (Result 0) | `ProcessDBMessage.cpp:1217-1251` |
| BR-44 | Capsule | `_MSG_CNFDBCapsuleInfo` relays the capsule record to the client verbatim | `ProcessDBMessage.cpp:1254-1257` |
| BR-45 | Capsule | `_MSG_DBCNFCapsuleCharacterFail` resynchronizes carry and reports "no empty slot" | `ProcessDBMessage.cpp:1260-1265` |
| BR-46 | Capsule | `_MSG_DBCNFCapsuleCharacterFail2` resynchronizes carry and reports "cannot use ID" | `ProcessDBMessage.cpp:1268-1273` |
| BR-47 | Capsule | `_MSG_DBCNFCapsuleSucess` erases the live mob and extra blob and saves with export | `ProcessDBMessage.cpp:1276-1282` |
| BR-48 | Ranking | `_MSG_GrindRankingData` is forwarded only when the packet's `PlayerRank.Name` equals the logged-in mob name | `ProcessDBMessage.cpp:1285-1296` |
| BR-49 | Notification | `_MSG_DBClientMessage` relays a raw text line to the addressed client | `ProcessDBMessage.cpp:1299-1304` |
| BR-50 | Relay | `_MSG_DBServerSend1` rewrites `ID = 30000`, queues and flushes to the client | `ProcessDBMessage.cpp:1307-1315` |
| BR-51 | Relay | `_MSG_DBCNFServerChange` forwards the server-change blob and closes the session | `ProcessDBMessage.cpp:1318-1324` |

---

### 3.2 Detailed breakdown of the business rules

---

#### BR-01: Envelope validation gate

**Overview**: Every packet crossing the DBSrv-to-TMSrv boundary must carry the `FLAG_DB2GAME` bit (0x0400, defined at `Code/Basedef.h:935`) and a slot identifier in `[0, MAX_USER)`. Anything else is rejected before dispatch and written to the system log.

**Detailed description**:
The check is a single short-circuit expression evaluated on a cast of the raw buffer: `if (!(std->Type & FLAG_DB2GAME) || (std->ID < 0) || (std->ID >= MAX_USER))`. `Type` is a 16-bit field at offset 4 of the 12-byte `_MSG` header (`Code/Basedef.h:925-930`), and `MAX_USER` is 1000 (`Code/Basedef.h:56`). Because the test is a bitwise AND rather than equality, it accepts *any* message type that carries the DB bit, including types that also carry `FLAG_GAME2CLIENT`, `FLAG_GAME2DB`, `FLAG_NEW` or `FLAG_DB2NP`. This is intentional: the protocol reuses a single numeric type value across namespaces and disambiguates by flags, which is why several constants legitimately appear twice with different flag sets (for example `_MSG_SecretCode`-style duplicates such as `_MSG_DBSavingQuit = 10|FLAG_DB2GAME` at `Basedef.h:1002` versus `_MSG_DBUpdateSapphire = 10|FLAG_GAME2DB` at `Code/Basedef.h:1000`).

The rejection branch does not merely drop the packet: it formats `err,packet Type:%d ID:%d Size:%d KeyWord:%d` into the shared 4 KB `temp` buffer (`Code/TMSrv/Server.cpp:277`) and calls `Log(temp, "-system", 0)`. That log line is the only forensic artefact for malformed database traffic, and it is emitted for every rejected packet with no rate limiting, so a desynchronised peer can amplify log volume to disk unboundedly. The branch re-casts `Msg` into a second pointer `m` that is identical to `std` (line 45), a harmless decompilation artefact.

Because `ID` is declared `short` (signed), the `< 0` test is meaningful: a corrupted or hostile header can produce a negative slot. Slot 0 is accepted here and then interpreted as "server scope" by BR-02, so slot 0 is never a real player. In practice player slots live in `[1, MAX_USER - ADMIN_RESERV)` = `[1, 990)`; `ADMIN_RESERV` is 10 (`Code/Basedef.h:57`), and the upper reservation is enforced only on the accept path (`Code/TMSrv/Server.cpp:4022`), not here.

**Rule workflow**:
1. Cast the buffer to `MSG_STANDARD *`.
2. If `(Type & 0x0400) == 0` - reject.
3. If `ID < 0` - reject.
4. If `ID >= 1000` - reject.
5. On rejection: `sprintf` a four-field diagnostic into `temp`, `Log(..., "-system", 0)`, `return`.
6. On acceptance: `conn = std->ID` and continue to BR-02.

---

#### BR-02: Slot-scope routing (server vs player namespace)

**Overview**: The `ID` header field doubles as a routing key. `ID == 0` means the packet is a server-wide directive from DBSrv; `ID >= 1` means it is a response addressed to the player occupying that TMSrv slot.

**Detailed description**:
The dispatcher literally tests `if (conn == 0)` (line 56) and enters a switch containing only 11 message types, all of which are state changes to global world structures or broadcasts. The complementary branch (line 188) contains the 34 player-scoped types. The split is not defended anywhere in code: nothing prevents DBSrv from sending a player-scoped type with `ID = 0` (it would be dropped, because the server-scope switch has no matching case and no `default`), nor a server-scoped type with a non-zero `ID` (likewise dropped). The correctness of every login in the cluster therefore depends on the producer honouring an unwritten convention.

The convention is visible and consistently applied on the DBSrv side: `SendDBSignalParm3(i, 0, _MSG_DBSetIndex, ...)` at `Code/DBSrv/CFileDB.cpp:503`, `SendGuildInfo` which hardcodes `sm.ID = 0` at `Code/DBSrv/CFileDB.cpp:2190`, the guild report at `Code/DBSrv/CFileDB.cpp:371-375`, and the `_MSG_MagicTrumpet` echo at `Code/DBSrv/CFileDB.cpp:1593-1609` which re-sends the packet unchanged after TMSrv built it with `sm_mt.ID = 0` (`Code/TMSrv/_MSG_MessageWhisper.cpp:129`). Conversely every player-scoped reply is built with `SendDBSignal(conn, m->ID, ...)` where `m->ID` is the original player slot.

One consequence is architecturally significant: the same numeric type can be *both* scopes. `_MSG_NPNotice` (type 9) is handled in both switches with different semantics (BR-13 broadcast versus BR-20 unicast), selected purely by `ID` plus `Parm1`. The dispatcher is therefore not a pure type router; it is a type-plus-scope router, and any refactoring that assumed a single key would break the protocol.

**Rule workflow**:
1. `conn = std->ID`.
2. If `conn == 0` - enter server-scope switch (cases at lines 61, 69, 83, 91, 99, 107, 114, 123, 136, 141, 148).
3. Otherwise - enter player-scope prologue (BR-03, BR-04) and switch at line 210.
4. Unmatched type in either switch - fall through to the end of the enclosing block with no action and no log (BR-05).

---

#### BR-03: Dead-session guard (`_MSG_DBNoNeedSave` write-back)

**Overview**: If a player-scoped packet arrives for a slot with no session (`pUser[conn].Mode == USER_EMPTY`), TMSrv refuses the packet and explicitly tells DBSrv not to persist anything for that slot.

**Detailed description**:
The guard is `if ((conn > 0 && conn < MAX_USER) && pUser[conn].Mode == 0)` at line 190. `USER_EMPTY` is 0 (`Code/TMSrv/CUser.h:26`). The response is a 12-byte `MSG_STANDARD` with `Type = _MSG_DBNoNeedSave` (0x0805) and `ID = conn`, sent over the *outbound* database socket at line 197. `SafeTMSrv` therefore has a hard dependency on DBSrv's `case _MSG_DBNoNeedSave` (`Code/DBSrv/CFileDB.cpp:1096-1122`), which calls `RemoveAccountList(Idx)` and applies any pending admin disable for that account index.

This guard is the reason a late database reply cannot resurrect a closed session. It matters most in three races that the single-threaded design does not eliminate: (a) the player disconnects while a `_MSG_DBCNFCharacterLogin` is in flight; (b) an operator item delivery (`_MSG_DBSendItem`) targets a slot that has since emptied; (c) a duplicate-login kick has already reset the local session while the database is still answering. In all three cases the packet body is discarded unread, and the slot is released on the database side.

Two properties are worth recording. First, the reply is built from an *uninitialised* stack `MSG_STANDARD`; only `Type` and `ID` are assigned, but `CPSock::AddMessage` overwrites `Size`, `KeyWord`, `CheckSum` and `ClientTick` before copying (`Code/CPSock.cpp:535-542`), so no uninitialised bytes reach the wire. Second, the guard does not distinguish "session never existed" from "session already closed": it is a blunt `Mode == 0` test, and it will also fire for a slot that is mid-accept (`USER_ACCEPT` = 1 does *not* fire it, but a slot still at 0 after `CUser::CloseUser()` does).

**Rule workflow**:
1. Enter the player-scope branch (`conn != 0`).
2. If `pUser[conn].Mode != USER_EMPTY` - continue to the type switch.
3. Else: build `MSG_STANDARD{Type=_MSG_DBNoNeedSave, ID=conn}`.
4. `DBServerSocket.SendOneMessage(...)` - immediate flush (queue + `SendMessageA`).
5. `return` - the packet body is never inspected.

---

#### BR-04: Unreachable per-slot range guard

**Overview**: A second range check, `if (conn <= 0 || conn > MAX_USER)`, exists at line 202 but can never be true on any path that reaches it.

**Detailed description**:
The function reaches line 202 only from the `else` of `conn == 0`, so `conn >= 1` is already established. And BR-01 has already rejected `conn >= MAX_USER`. The predicate is therefore unsatisfiable. Its body - `sprintf("Unknown DB Message - conn:%d - type:%d")` plus `Log` plus `return` - is dead code that inflates the cyclomatic count without adding a defence.

The defect is the classic off-by-one from a decompiled `if (conn <= 0 || conn >= MAX_USER)`: the compiler-visible operand became `>` although the sibling check in `ProcessClientMessage.cpp:42` uses `>=`. Because the branch is unreachable, the mistake is presently harmless, but it means the only effective bounds enforcement for payload writes is BR-01. Any future change to allow an alternate entry point (for example a test harness or an admin socket) would silently permit `conn == MAX_USER`, which is the first index of the NPC half of `pMob[]` and outside `pUser[]`.

This rule is included in the inventory because a complete business-rule extraction must record that the *intended* invariant ("reject unknown connections") is declared but not enforced by this code path. The invariant is, in practice, enforced one level up.

**Rule workflow**:
1. Reachable only when `conn != 0` and `conn < MAX_USER` (guaranteed by BR-01).
2. Evaluate `conn <= 0 || conn > MAX_USER` - always false.
3. Continue to the type switch at line 210.

---

#### BR-05: Silent discard of unhandled types

**Overview**: Neither `switch` has a `default` label, so any `FLAG_DB2GAME` packet whose type is not one of the 45 implemented cases is dropped without logging.

**Detailed description**:
The codebase defines 191 message constants (`Code/Basedef.h`), of which a substantial number carry the `FLAG_DB2GAME` bit but are not handled here - for example `_MSG_DBMessagePanel` (1|0x400), `_MSG_DBCapsuleInfo` (60|0x400|0x800), `_MSG_DBOutCapsule` (62|...), `_MSG_DBPutInCapsule` (61|...), and `_MSG_NPAppeal` (16|0x400|0x800|0x1000). Those are game-to-database directions or admin-only types, so their absence is expected rather than erroneous. The risk is not the missing cases but the absence of diagnostics: a protocol drift between DBSrv and TMSrv (for example a DBSrv build that starts emitting a new reply) produces a login that hangs with no log entry, which is the hardest class of defect to diagnose in this architecture because the only other evidence is the client's own timeout.

The behaviour is measurably different from the sibling dispatcher: `ProcessClientMessage` also has no `default`, but it logs nothing either, so the two are consistent in style. From an architecture standpoint this makes the router "fail silent by default", and it obliges every operator to correlate TMSrv logs, DBSrv logs and client behaviour when a new message family is introduced.

The player-scope switch contains one more silent-drop hazard specific to this component: several types that DBSrv legitimately sends to a *server* are only handled in the server-scope switch. `_MSG_MagicTrumpet` is the clearest example (handled at line 136 only). Because TMSrv itself constructs the trumpet with `ID = 0` (`Code/TMSrv/_MSG_MessageWhisper.cpp:129`), the constraint holds today; any producer that used a real slot would produce a globally silent failure.

**Rule workflow**:
1. Evaluate `std->Type` against the active switch.
2. On no match: control reaches the closing brace of the switch (line 184 or 1327).
3. Nothing is logged, nothing is sent, nothing is mutated.
4. Control returns to the `WSA_READDB` loop, which continues with the next queued packet.

---

#### BR-06: Absent payload-size validation and `m->Size` trust

**Overview**: The component never verifies that the received length matches the structure it casts to, and it echoes the peer-supplied `Size` field back to DBSrv when acknowledging.

**Detailed description**:
For every branch the pattern is `MSG_X *m = (MSG_X*)Msg;` followed by immediate field access. `CPSock::ReadMessage` guarantees only that `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE (8192)` (`Code/CPSock.cpp:397`). A 12-byte packet claiming to be `_MSG_DBCNFCharacterLogin` will therefore be read as ~2.4 KB of adjacent buffer content. The intended defence, `BASE_CheckPacket`, is a per-type `Size != sizeof(...)` table with the entire body inside a comment block (`Code/Basedef.cpp:6475-6476`), returning an undefined value; its only call sites are inside `#ifdef _PACKET_DEBUG` blocks (`Code/TMSrv/Server.cpp:3895-3905`, `Code/CPSock.cpp:544-552`).

The practical exposure of this component is bounded by its trust in DBSrv, which is a co-located process on a private port. That trust boundary is explicit and should be recorded: TMSrv does not distinguish "malicious database" from "corrupt database". Several branches then use peer-supplied integers as array subscripts, which converts any corruption into an out-of-bounds write rather than a rejected packet (see BR-11 and BR-19 in section 10).

The second half of the rule is the echo: in `_MSG_DBSendItem` (lines 278, 310, 327) and `_MSG_DBSendDonate` (line 1246) the reply length is `m->Size`, taken from the inbound packet, not `sizeof(MSG_DBSendItem)`. If an inbound packet declares `Size = 8192`, the same 8 KB are pushed back out. `AddMessage` bounds this against the 128 KB send buffer (`Code/CPSock.cpp:518`) and returns FALSE on overflow, so the consequence is a refused acknowledgement plus a log line, not a buffer overrun.

**Rule workflow**:
1. Packet arrives with `Size` field `S` in `[12, 8192]`.
2. Dispatcher casts to the structure implied by `Type` without comparing `S`.
3. Field reads beyond `S` return whatever the 128 KB receive ring holds there.
4. On acknowledgement paths, `m->Size` is passed to `SendOneMessage`, so the reply is `S` bytes long.
5. No error is raised at any point.

---

#### BR-07: Character-transfer feature switch (`_MSG_TransperCharacter`)

**Overview**: A single bit of world state, `TransperCharacter`, is set from the database side and enables the "move a character between server groups" feature; the flag gates both the outbound request relay (BR-18) and an extra client notification during account login.

**Detailed description**:
The handler (lines 61-67) is three statements: set `TransperCharacter = 1`, log the literal `"TransperCharacter mode"`, break. `TransperCharacter` is a plain `int` defined at `Code/TMSrv/Server.cpp:382` with initial value 0 and declared at `Code/TMSrv/Server.h:327`. It is produced by DBSrv in two situations: when a `redirect.txt` exists at boot, the DBSrv sets its own flag and pushes `SendDBSignalParm2(User, 0, _MSG_TransperCharacter, 0, 0)` to each connected game server (`Code/DBSrv/Server.cpp:1174`, and the local flag at `Code/DBSrv/Server.cpp:609`), and it is implied by the presence of a redirection service.

The handler is not idempotent-safe in the sense that there is no reset path: no branch anywhere in TMSrv sets the flag back to 0 after `ProcessDBMessage.cpp:609`-equivalent assignment, other than process restart. The flag is therefore monotonic for the lifetime of the process, which matches its meaning ("this deployment supports character transfer") but means a database-side disable requires a restart of the game server.

Its two consumers both live in this component: `_MSG_ReqTransper` returns immediately when the flag is 0 (line 215), and `_MSG_DBCNFAccountLogin` appends a `MSG_STANDARDPARM2{Type=_MSG_TransperCharacter, ID=30001, Parm1=0, Parm2=0}` packet to the client after a successful account confirmation (lines 459-470). The client uses that notification to offer the transfer entry point.

**Rule workflow**:
1. DBSrv sends `_MSG_TransperCharacter` with `ID = 0`.
2. Set `TransperCharacter = 1`.
3. Log `"TransperCharacter mode"` with the `system` tag.
4. Every subsequent account login forwards the transfer notification to the client.
5. Every subsequent `_MSG_ReqTransper` is no longer suppressed.

---

#### BR-08: Server group/index and Sapphire assignment (`_MSG_DBSetIndex`)

**Overview**: The authoritative assignment of the game server's identity inside the cluster - `ServerGroup`, `ServerIndex` and the cash-shop multiplier `Sapphire` - arrives in a single `MSG_STANDARDPARM3` packet, with `Parm1 == -1` meaning "update Sapphire only".

**Detailed description**:
The handler reads three integers from `MSG_STANDARDPARM3` (`Code/Basedef.h:968-974`). When `Parm1 != -1`, `ServerGroup = Parm1` and `ServerIndex = Parm3`; `Sapphire = Parm2` is assigned unconditionally. DBSrv produces the identity form at `Code/DBSrv/Server.cpp:1159`: `SendDBSignalParm3(User, 0, _MSG_DBSetIndex, ServerIndex, Sapphire, User)`, where DBSrv's `ServerIndex` is the *server-group* index resolved from `serverlist.txt` and `User` is the TMSrv connection slot, which becomes the index within the group. It produces the Sapphire-only form at `Code/DBSrv/CFileDB.cpp:503` during a `_MSG_DBUpdateSapphire` broadcast: `SendDBSignalParm3(i, 0, _MSG_DBSetIndex, -1, Sapphire, i)`.

`ServerIndex` is not cosmetic. It is used as a *dimension* of `ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]` when resolving crown-guild privileges (`Code/TMSrv/Server.cpp:508`, `535`, and `Code/TMSrv/SendFunc.cpp:91`, `129`), it indexes the export path `S:/export/account%d/` on the database side (`Code/DBSrv/CFileDB.cpp:2488`), it selects the newbie-event server by `(mday - 1) % NumServerInGroup` (`Code/TMSrv/ProcessSecMinTimer.cpp:2038-2041`), and it derives the ranking-log prefix (`Code/TMSrv/Server.cpp:8034`). A wrong assignment therefore silently changes crown-guild behaviour, newbie-channel routing and backup layout rather than producing an error.

`Sapphire` is a cash-shop scale factor clamped by the producer between 1 and 64 (`Code/DBSrv/CFileDB.cpp:492-495`) for the doubling/halving path, but this handler does not clamp: a `Parm2` of 0 or a negative value is accepted verbatim. The `// TODO: Check if it isn't a break` comment at line 81 records the decompiler's uncertainty about whether the original fell through to the next case; the current code breaks, which is the safe interpretation.

**Rule workflow**:
1. Receive `MSG_STANDARDPARM3`.
2. If `Parm1 != -1`: `ServerGroup = Parm1`; `ServerIndex = Parm3`.
3. If `Parm1 == -1`: leave `ServerGroup` / `ServerIndex` untouched.
4. Always: `Sapphire = Parm2`.
5. No acknowledgement, no logging.

---

#### BR-09: Guild war state machine invocation (`_MSG_War`)

**Overview**: A two-integer directive (`Parm1` = my guild, `Parm2` = target guild, or 0 to cancel) is executed by `DoWar`, which owns the entire war/ceasefire decision table.

**Detailed description**:
`ProcessDBMessage.cpp:83-89` is a pure adapter: cast to `MSG_STANDARDPARM2` and call `DoWar(m->Parm1, m->Parm2)` (`Code/TMSrv/Server.cpp:7604`). All domain logic lives in `DoWar`, whose first act is to validate `0 < myguild < 65536` and `0 <= target < 65536` and to log `err,dowar` on violation. Because `g_pGuildWar` is declared `unsigned short[65536]` (`Code/TMSrv/Server.h:51`), a guild id above 65535 would wrap; the guard exists precisely to prevent that.

The war state machine distinguishes four transitions that matter to this component's contract: (a) target != 0, no existing war, target not already at war with me -> send `_SS_War_Declared` to both guilds and record `g_pGuildWar[myguild] = target`; (b) target != 0, symmetric confirmation (`g_pGuildWar[target] == myguild`) -> `_SS_War_Started` world notice and refresh of `SendWarInfo` for both guilds' online members; (c) target != 0 but a war already exists -> only an `err,dowar` log; (d) target == 0 -> cancellation, which requires `g_pGuildWar[myGuildWar] == myguild` (mutual consent) otherwise only my side is cleared with `_SS_War_declare_canceled`.

The dispatcher's role is to make the database the *initiator* of war state. TMSrv also produces `_MSG_War` upstream (client handler `_MSG_War.cpp:46`), so the same type flows in both directions; the DBSrv side re-broadcasts it (`CFileDB.cpp:388`) to keep every game server's `g_pGuildWar` consistent. Any server that misses the packet (for example because it reconnected to DBSrv mid-war) will disagree about war status until the next broadcast - there is no reconciliation at boot, because the war table is not part of `_MSG_GuildReport`.

**Rule workflow**:
1. Receive `MSG_STANDARDPARM2{Type=_MSG_War}` with `ID = 0`.
2. `DoWar(Parm1, Parm2)`.
3. `DoWar` validates both ids against `[0, 65536)`.
4. Resolve guild names through `BASE_GetGuildName(ServerGroup, guild, buf)`.
5. Branch to declare / confirm / reject / cancel; mutate `g_pGuildWar[]`, notify both guilds, refresh `SendWarInfo` for affected online users.

---

#### BR-10: Guild alliance state machine invocation (`_MSG_GuildAlly`)

**Overview**: The alliance counterpart of BR-09; `Parm1` is the requesting guild and `Parm2` the prospective ally, with 0 meaning "withdraw".

**Detailed description**:
The handler is again a two-line adapter (`ProcessDBMessage.cpp:91-97`) delegating to `DoAlly` (`Code/TMSrv/Server.cpp:7697`). `DoAlly` repeats the `[0, 65536)` validation used by `DoWar`, then operates on `g_pGuildAlly[65536]` (`Code/TMSrv/Server.h:52`). The three observable transitions are: propose (notify both guilds, record the unilateral intent), accept (both sides agree -> symmetric record), and withdraw (clear my side, optionally notify the ally when the reciprocal entry existed). The notices use `_SS_Ally_*` templates and `SendGuildNotice`, which iterates online members.

Architecturally, both BR-09 and BR-10 show the same pattern: the router contains no policy, the policy lives in `Server.cpp`, and the database is the sequencing authority. That is a reasonable separation for a single-threaded server, but it makes `Server.cpp` a mandatory dependency for a "message routing" component, which is the dominant contributor to the dispatcher's efferent coupling (section 6).

Alliance state is also not persisted in any file read by TMSrv at boot; it is reconstructed solely from database broadcasts after connect. A TMSrv that connects when no broadcast is pending starts with an empty alliance table, and the code contains no request-for-state primitive for war/alliance (unlike guild info, which is pushed per guild at connect, `Code/DBSrv/Server.cpp:1161-1164`).

**Rule workflow**:
1. Receive `MSG_STANDARDPARM2{Type=_MSG_GuildAlly}` with `ID = 0`.
2. `DoAlly(Parm1, Parm2)`.
3. Validate both ids in `[0, 65536)`.
4. Resolve guild names for the notice text.
5. Propose / accept / withdraw, mutating `g_pGuildAlly[]` and notifying members.

---

#### BR-11: Guild record replacement (`_MSG_GuildInfo`)

**Overview**: The database is the master copy of guild records; every `_MSG_GuildInfo` packet wholesale replaces `GuildInfo[Guild]`.

**Detailed description**:
The handler is `GuildInfo[m->Guild] = m->GuildInfo;` (line 103) on a `STRUCT_GUILDINFO` of eight bytes: `short Fame`, `char Clan`, `char Citizen`, `char Sub1..Sub3` (`Code/Basedef.h:796-806`). `GuildInfo` is declared `STRUCT_GUILDINFO GuildInfo[65536]` (`Code/TMSrv/Server.cpp:361`, `extern` at `Code/TMSrv/ProcessClientMessage.h:56`), i.e. a 512 KB table indexed by *global* guild id, mapped into the local namespace by `usGuild = Guild & (MAX_GUILD - 1)` = `guild & 4095` (`MAX_GUILD` = 4096, `Code/Basedef.h:147`) in the login path at lines 918-921.

This rule has two strong consumers inside the login flow. First, at account login the client's character list is not touched, but at character login `pMob[conn].extra.Citizen` is overwritten from `GuildInfo[m->mob.Guild].Citizen` when the character has a guild (lines 744-745) - the Citizen field decides which kingdom (clan 7/8) the player is flagged for, which in turn drives RvR bonus eligibility (lines 1064-1073) and PvP targeting. Second, the `Clan` field drives the mantle re-colouring logic (lines 921-975): when the stored character's `Clan` differs from `GuildInfo[usGuild].Clan`, the equipped mantle (`Equip[15]`) is remapped to the item id that encodes the new clan, and the client packet is patched to match.

No bounds check exists on `m->Guild`. A value of 65536 or more writes past the end of a 512 KB global with an 8-byte record. The producer (`Code/DBSrv/CFileDB.cpp:2185-2199`) iterates `0..65535`, so in a healthy cluster the invariant holds; it is a trust-boundary issue rather than a reachable bug, but it is the single highest-integrity write in the server-scope switch.

**Rule workflow**:
1. Receive `MSG_GuildInfo{ID=0}` carrying `Guild` and an 8-byte record.
2. Write `GuildInfo[Guild] = payload` with no range validation.
3. Subsequent reads: character-login Citizen resolution, mantle/clan correction, fame checks in guild chat, guild-board handlers, `SendGuildNotice`.

---

#### BR-12: Crown-guild matrix replacement (`_MSG_GuildReport`)

**Overview**: A 10x5 integer matrix identifying, for each of the 10 possible game servers, which guild currently holds each of the five guild zones (the "crown"), arrives as one packet and replaces `ChargedGuildList` entirely.

**Detailed description**:
`memcpy(ChargedGuildList, m->ChargedGuildList, sizeof(ChargedGuildList))` at line 111 copies 50 integers (`MAX_SERVER` = 10 at `Code/Basedef.h:48`, `MAX_GUILDZONE` = 5 at `Code/Basedef.h:148`). `ChargedGuildList[10][5]` is defined at `Code/Basedef.cpp:738`, zeroed at boot (`Code/Basedef.cpp:2566`), and produced by DBSrv after aggregating `_MSG_GuildZoneReport` from every game server (`Code/DBSrv/CFileDB.cpp:357-377`, send at `371-375`).

The matrix is consumed by three rules: (a) `ClearCrown(conn)` (`Code/TMSrv/Server.cpp:494-...`) uses column 4 - the castle zone - to decide whether the logging-in player's guild is the reigning guild and, if so, rewrites crown/dragon headgear item ids according to clan; (b) `SendNoticeChief` (`Code/TMSrv/SendFunc.cpp:64-130`) uses the whole matrix to decide which online guild leaders should receive a chief-only notice; (c) the status line printed every second (`Code/TMSrv/Server.cpp:148`) shows the five zone owners. The dispatcher calls `ClearCrown(conn)` on every character login (line 1013), so the freshness of this matrix directly affects crown appearance.

Because the copy is unconditional and there is no merge, a missing or stale `_MSG_GuildReport` (for example after a DBSrv restart that has not yet collected zone reports) erases crown knowledge, and `ClearCrown` will then treat every guild as non-reigning. No versioning or timestamp accompanies the packet.

**Rule workflow**:
1. Receive `MSG_GuildReport{ID=0}` with a 50-int array.
2. `memcpy` into `ChargedGuildList` (prefix-compatible with any smaller source, because the size used is the destination's).
3. Effect is immediate: next `ClearCrown`, `SendNoticeChief` and status print use the new values.

---

#### BR-13: Server-wide notice broadcast (`_MSG_NPNotice`, `Parm1 == 1`)

**Overview**: When the server-scoped `_MSG_NPNotice` carries `Parm1 == 1`, its text is broadcast to every player in the game world.

**Detailed description**:
Lines 114-121 cast to `MSG_NPNotice` (`Code/Basedef.h:2287-2294`: `int Parm1, Parm2; char AccountName[16]; char String[96]`) and call `SendNotice(m->String)` only when `Parm1 == 1`. `SendNotice` (`Code/TMSrv/SendFunc.cpp:47-62`) first logs `not <text>` with the `system` tag, then **early-returns without broadcasting** when the text begins with the two characters `'x` (the decompiled spelling of an escape sequence), then loops `0..MAX_USER` and calls `SendClientMessage(i, Message)` for each slot in `USER_PLAY`. The sentinel exists so operators can log a notice without showing it.

`SendClientMessage` (`Code/TMSrv/SendFunc.cpp:27-45`) refuses `conn <= 0`, copies exactly `MESSAGE_LENGTH` (96) bytes from the source into a `MSG_MessagePanel` with `Type = _MSG_MessagePanel` and `ID = 0`, forces terminators at offsets 94 and 95, and appends to the socket buffer without flushing. The message therefore becomes visible on the next 500 ms timer tick (`Code/TMSrv/ProcessSecMinTimer.cpp:201-221`).

The important contract here is that `MSG_NPNotice.String` is exactly 96 bytes (`MAX_NOTIFY_LENGTH` = 96, `Code/Basedef.h:128`), matching `MESSAGE_LENGTH`, so the fixed-length copy is in-bounds for this producer. DBSrv builds these via `SendAdminMessage` from the NP tool (`Code/DBSrv/Server.cpp:268-281`) with `strncpy(sm.String, message, MESSAGE_LENGTH)`, and re-broadcasts `_MSG_NPNotice` unchanged for other cases.

**Rule workflow**:
1. Receive `MSG_NPNotice` with `ID = 0`.
2. If `Parm1 != 1`: no action (unicast form is BR-20).
3. `SendNotice(String)`.
4. `SendNotice` logs `not <text>`; aborts if the text starts with `'x`.
5. For each slot in `USER_PLAY`: append `_MSG_MessagePanel` with the text.
6. Flush happens later on the second timer.

---

#### BR-14: Remote administrative command execution (`_MSG_MessageDBImple`)

**Overview**: A database-originated packet can execute an arbitrary administrator command on the game server, with the privilege level taken from the packet itself and the execution context hardcoded to slot 0.

**Detailed description**:
Lines 123-134 truncate `m->String[95]` and `m->String[94]` to zero (defensive termination of a 96-byte field), log the command text to the system log, and call `ProcessImple(0, m->Level, m->String)`. `MSG_MessageDBImple` is `{_MSG; int Level; char String[96];}` (`Code/Basedef.h:1181-1186`) and carries both flags `FLAG_GAME2DB | FLAG_DB2GAME`, so the same type travels in both directions. DBSrv receives it from the NP tool, forces `m->ID = 0`, truncates the string, and fans it out to every connected game server (`Code/DBSrv/CFileDB.cpp:592-609`).

`ProcessImple` (`Code/TMSrv/imple.cpp:46`) parses `str + 1` with nine `sscanf` conversions (`cmd` plus up to eight parameters) and then gates commands by numeric level thresholds: `level >= 8` for the `set` family (sanctuary, html build, quest/skill reset, level, hp, mp, ac, damage, exp, str/int/dex/con, coin, merchant, skill bonus, special points), `level >= 3`, `level >= 2`, and `level >= 1` for progressively wider families (`imple.cpp:89`, `569`, `647`, `1119`, `1164`). Because the level is supplied by the packet, the *only* access control on this path is DBSrv's decision to forward and the NP tool's credentials (`redirect.txt` / admin list on the DBSrv side).

The `conn = 0` argument is significant. `ProcessImple`'s own guard is `if (conn <= 0 && conn >= MAX_USER) return;` (`imple.cpp:91`) - an unsatisfiable conjunction, so slot 0 is *not* rejected. Commands then mutate `pMob[0]`/`pUser[0]` (the reserved slot) and call `SendClientMessage(0, ...)`, which refuses index 0. Server-wide branches (spawn generation, notices, guild info broadcasting, quest injection) work as intended; per-player `set` branches operate on the reserved slot and are effectively no-ops whose text feedback is discarded. This is a functional limitation of the remote-admin path that the dispatcher's fixed `conn = 0` argument imposes.

**Rule workflow**:
1. Receive `MSG_MessageDBImple` with `ID = 0`.
2. Force `String[95] = String[94] = 0`.
3. Log the raw command with the `system` tag.
4. `ProcessImple(0, Level, String)`.
5. `ProcessImple` re-logs as `adm <command>`, applies the `level >= N` gates, executes the matched command against global state.

---

#### BR-15: Global trumpet relay (`_MSG_MagicTrumpet`)

**Overview**: A purchased "magic trumpet" message is relayed to every playing user through the deferred multicast helper, exactly once per game server.

**Detailed description**:
`SyncMulticast(0, (MSG_STANDARD*)Msg, 0)` (line 138) iterates all `MAX_USER` slots, selects `Mode == USER_PLAY && conn != i`, and appends `m->Size` bytes to each socket buffer; the third argument `0` suppresses the immediate flush (`Code/TMSrv/SendFunc.cpp:260-272`). The packet is forwarded byte-for-byte, including its `ID = 0` and its `Type`.

The origin is TMSrv itself: `_MSG_MessageWhisper.cpp:125-138` builds `MSG_MagicTrumpet` with `ID = 0`, formats `[<mob name>]> <text>`, `strncpy` into a 96-byte field, sets `String[94] = 1`, and sends it to DBSrv. DBSrv then re-sends the identical packet to every connected game server **and** every NP admin session (`Code/DBSrv/CFileDB.cpp:1591-1610`), so each TMSrv sees one copy and broadcasts once. The `ID = 0` invariant is what makes the relay work: a non-zero id would route into the player-scope switch, where no case exists for this type (BR-05).

Two details are worth recording because they are protocol-critical. First, `String[94] = 1` is a non-printable marker that the client uses to distinguish a trumpet from a plain panel message; it is set by the producer, not by this handler. Second, the packet is *not* re-obfuscated per recipient by this code - `AddMessage` performs the obfuscation when appending (`Code/CPSock.cpp:556-590`), so the single shared buffer remains valid for all recipients.

**Rule workflow**:
1. Receive `MSG_MagicTrumpet` with `ID = 0`.
2. `SyncMulticast(0, Msg, 0)`.
3. For each slot in `USER_PLAY`: `AddMessage(Msg, m->Size)`.
4. No flush; delivery on the next 500 ms tick.

---

#### BR-16: Server notice via `_MSG_DBNotice`

**Overview**: `_MSG_DBNotice` is a second, simpler broadcast channel than BR-13: the text goes to all playing users with no flag and no unicast variant.

**Detailed description**:
Lines 141-146 call `SendNotice(m->String)` directly. `MSG_DBNotice` is `{_MSG; char String[96];}` (`Code/Basedef.h:1407-1412`) and carries `FLAG_DB2GAME | FLAG_GAME2DB | FLAG_GAME2CLIENT` (composite value 0x0D1E), which means the packet is also a valid game-to-client type; the client can therefore receive the same `Type` - but this handler does not forward the original type, it converts the text into a `_MSG_MessagePanel` line.

Unlike BR-13 there is no `ID`-based variant, no `Parm1` discrimination, and no `'x` suppression unless the text itself starts with `'x`. The producer set is: the NP tool through DBSrv, and TMSrv's own admin interpreter (`Code/TMSrv/imple.cpp:1198`, `_MSG_MessageWhisper.cpp:1099`) which sends `_MSG_DBNotice` to DBSrv so that the notice reaches every shard. The loop-back means a single `/notice` command issued on shard A is displayed by shard A through this handler even though shard A originated it - `SyncMulticast`'s `conn != i` exclusion is not applied, so the origin server also shows it.

**Rule workflow**:
1. Receive `MSG_DBNotice` with `ID = 0`.
2. `SendNotice(String)`.
3. `SendNotice` logs, applies the `'x` suppression, loops over `USER_PLAY`, appends a panel message.
4. Flush on the next timer tick.

---

#### BR-17: One-primary-account-per-machine policy (`_MSG_DBCheckPrimaryAccount`)

**Overview**: When an account logs in from a network adapter (MAC) that is already represented by another account, the other sessions are downgraded to "trade only": they cannot fight, party, or use most items, and if they are physically outside a village they are recalled into one.

**Detailed description**:
The packet is `{_MSG; char AccountName[16]; unsigned int Mac[4];}` (`Code/Basedef.h:1042-1049`), produced when the database decides an account is the "primary" one for its MAC address (`Code/DBSrv/CFileDB.cpp:2074-2090` for the explicit `_MSG_DBPrimaryAccount` path, and `Code/DBSrv/CFileDB.cpp:782-801` during account login when `GetAccountsByMac(...) <= 1`). It is broadcast to *every* connected game server, with `ID = 0`, precisely because the conflicting account may be playing on a different shard.

The loop (lines 152-180) scans `i = 1 .. MAX_USER-1` and skips (a) slots in `USER_EMPTY` or with `Sock == 0`, and (b) slots whose 16-byte `Mac` differs from the packet's (`memcmp`). For the remaining slots it compares `AccountName` case-sensitively with `strncmp(..., 16)`: an exact match means "this is the primary account itself" and clears `OnlyTrade = 0`; any other match means "another account behind the same adapter" and applies the downgrade: `OnlyTrade = 1`, `RemoveParty(i)`, and - only if the target is actually in play (`pMob[i].Mode == USER_PLAY`) - a geography test.

The geography test is `(Village < 0 || Village >= 5) && (mapAttribute & 0x80) == 0`, where `Village = BASE_GetVillage(TargetX, TargetY)` and `mapAttribute = GetAttribute(TargetX, TargetY)`. In other words: if the player is *not* inside one of the five guild/city zones and is *not* standing on a map cell flagged 0x80 (a safe-zone bit), the server sends `_NN_OnlyVillage` and calls `DoRecall(i)`. The combination means a downgraded player may remain at the city gates but is pulled in if they wander into the field. `OnlyTrade` is then honoured by the gameplay handlers: attack (`_MSG_Attack.cpp:47`), action/emote (`_MSG_Action.cpp:198`), party invitation and acceptance (`_MSG_AcceptParty.cpp:71,77`, `_MSG_SendReqParty.cpp:56`), whisper (`_MSG_MessageWhisper.cpp:744,810`) and two party-bonus item checks (`_MSG_UseItem.cpp:2303,2343`).

Two boundary notes complete the rule. `_MSG_DBCNFAccountLogin` sets `OnlyTrade = 1` for every fresh session *before* this packet can arrive (line 395), so a player is trade-only by default until proven primary; and `CloseUser` does not reset `OnlyTrade`, so the flag survives until `CUser` is reconstructed or another check clears it. Because the comparison is `strncmp` over 16 characters and account names are upper-cased by the login path (`_MSG_AccountLogin.cpp:78`), the comparison is effectively case-insensitive in practice - but only because both sides were normalised earlier, not because this handler normalises.

**Rule workflow**:
1. Receive `MSG_DBCheckPrimaryAccount{ID=0}` with `AccountName` and `Mac[4]`.
2. For `i` in `[1, MAX_USER)`:
3. Skip if `Mode == USER_EMPTY` or `Sock == 0`.
4. Skip if `memcmp(pUser[i].Mac, m->Mac, 16) != 0`.
5. If `strncmp(pUser[i].AccountName, m->AccountName, 16) == 0`: set `OnlyTrade = 0`, continue.
6. Else: set `OnlyTrade = 1`; `RemoveParty(i)`.
7. If `pMob[i].Mode == USER_PLAY` and outside village and not on a 0x80 cell: `SendClientMessage(_NN_OnlyVillage)`; `DoRecall(i)`.

---

#### BR-18: Character-transfer request relay (`_MSG_ReqTransper`)

**Overview**: When transfer mode is enabled, a database-issued transfer request is forwarded to the client and the session is rewound to character selection.

**Detailed description**:
The branch first checks the feature flag and returns silently if it is off (line 215-216). It then rewrites `m->ID = ESCENE_FIELD + 1` (30001, `Code/Basedef.h:170`), sends the packet to the owning client, and forces `pUser[conn].Mode = USER_SELCHAR`. The message itself is `MSG_ReqTransper{_MSG; int Result; int Slot; char OldName[16]; char NewName[16];}` (`Code/Basedef.h:2170-2177`) with a documented result vocabulary in the comment on line 2173 (`0 Success, 1 Same Name Exist, 2 No Space at Transper Server, 3 Unknown Error`, plus 4 used in practice by `Code/DBSrv/Server.cpp:680`).

The producer chain is unusual and worth recording: the request does not originate from the player. It is created in DBSrv's *admin client* handler when an NP tool answers a character-creation request (`Code/DBSrv/Server.cpp:642-705`): after enabling the account, deleting the moved character slot (`memset(&file.Char[slot], 0, ...)`, write at line 696), DBSrv builds `MSG_ReqTransper` with `Result = m->Result`, `Slot = slot`, `ID = id` and sends it to the game server that hosts the account. TMSrv's job is purely to inform the client so that it can reconnect to the destination server with the new name.

The critical defect in this branch is at line 222: `pUser[conn].cSock.SendOneMessage((char*)&m, sizeof(MSG_ReqTransper));` passes the *address of the local pointer variable* `m`, not the message. `sizeof(MSG_ReqTransper)` is 52 bytes, so 52 bytes starting at a stack slot holding a 4-byte pointer are transmitted. The client receives a well-framed 52-byte packet whose `Size`, `KeyWord` and `CheckSum` are rewritten by `AddMessage` but whose `Type`, `ID`, `Result`, `Slot` and both names are arbitrary stack content. Two consequences follow: the feature cannot work as coded, and 48 bytes of TMSrv stack are disclosed to the client on every invocation (see section 10, risk R-01).

**Rule workflow**:
1. Receive `MSG_ReqTransper` with a player `ID`.
2. If `TransperCharacter == 0`: return.
3. Set `m->ID = 30001`.
4. Send 52 bytes from `&m` (defect - pointer address, not payload).
5. Set `pUser[conn].Mode = USER_SELCHAR`.

---

#### BR-19: Web-shop item delivery into cargo (`_MSG_DBSendItem`)

**Overview**: An item purchased or granted out-of-band is inserted into the player's account cargo using a two-phase fit algorithm (grid-aware first fit over 126 usable slots, then a raw empty-slot scan from the top), reported to the client, acknowledged to DBSrv, and - if the player is still on the character-selection screen - persisted immediately.

**Detailed description**:
The packet is `MSG_DBSendItem{_MSG; int Result; char Account[16]; STRUCT_ITEM Item;}` (`Code/Basedef.h:1283-1289`); `STRUCT_ITEM` is 8 bytes (`short sIndex` plus three `{cEffect,cValue}` unions, `Code/Basedef.h:398-412`). The producer (`Code/DBSrv/CReadFiles.cpp:237-257`) validates `sIndex` against `[0, MAX_ITEMLIST)`, computes its own position in the on-disk cargo and writes the account file *before* sending, so any TMSrv failure means a permanent divergence between the file and the live session.

Phase 0 of the handler is authorisation. `pUser[conn].Mode < USER_SELCHAR` (i.e. slots that are accepting, logging in or waiting for the DB) are refused with log tag `_fail_play_` and no database reply at all; `strcmp(pUser[conn].AccountName, m->Account) != 0` is refused with tag `_fail_name_`. Both refusals `break` out of the case without setting `Result`, so DBSrv never learns the delivery failed - consistent with DBSrv having no handler for this reply, but it also means the only trace is the TMSrv log line.

Phase 1 (lines 257-291) scans `i = 0 .. MAX_CARGO-3` (0..125) and, for each candidate, calls `BASE_CanCargo(&m->Item, pUser[conn].Cargo, i % 9, i / 9)` (`Code/Basedef.cpp:4480`). That function rasterises the item's footprint from `g_pItemGrid[EF_GRID]` (up to 4 rows x 2 columns) and rejects the candidate if any covered cell is already occupied or out of the `CARGOGRIDX=9` x `CARGOGRIDY=14` grid. The bound `MAX_CARGO - 2` (126) matches the grid capacity, so phase 1 never tests the two overflow slots 126 and 127.

Phase 2 (lines 293-323) runs only when phase 1 found nothing: it scans `i = 127 .. 0` for the first `sIndex == 0` slot, ignoring footprints. That means a large item can be placed in a slot whose grid cell overlaps another item when the grid is fragmented. This mirrors, exactly, the fallback in the producer (`Code/DBSrv/CReadFiles.cpp:298-308`), so the two sides agree on *where* an over-capacity item lands; it does not make the result grid-legal.

Phase 3 is the success tail: assign the item, `SendItem(conn, ITEM_PLACE_CARGO, i, &m->Item)` (`Code/TMSrv/SendFunc.cpp:1055`, which itself requires `USER_PLAY`), send `"<Item_Arrived> [<item name>]"` via `SendClientMessage`, set `m->Result = 0`, echo the packet back to DBSrv with `m->Size`, write log tag `_web1_`/`_web2_` and - only when `Mode == USER_SELCHAR` - call `SaveUser(conn, 0)`. Phase 4 is the failure tail (lines 325-337): `Result = 3`, echo, `SaveUser(conn, 1)` (save *with* export, `Code/DBSrv/CFileDB.cpp:1186-1187`), log tag `_fail_empty_`.

Three behavioural asymmetries are documented here because they are part of the rule. (a) `SendItem` silently does nothing unless the player is `USER_PLAY`, so a delivery accepted at the character-selection screen is stored and persisted but never shown until the client requests cargo again. (b) The failure tail saves unconditionally while the success tail saves only at `USER_SELCHAR`; a successful delivery to an in-game player therefore lives only in memory until the next periodic save. (c) `g_pItemList[m->Item.sIndex].Name` is dereferenced at lines 272 and 304 with no `sIndex` validation, unlike every other item-name lookup in this file (lines 406 and 658 guard `0 < sIndex < MAX_ITEMLIST`).

**Rule workflow**:
1. Receive `MSG_DBSendItem` for slot `conn`.
2. If `Mode < USER_SELCHAR`: log `tra,<account><itemcode>` as `_fail_play_`, stop.
3. If `AccountName != m->Account`: log as `_fail_name_`, stop.
4. Phase 1: for `i` in `[0,126)`, first `i` with `BASE_CanCargo(...) == TRUE` wins.
5. Phase 2: if none, take the highest `i` in `[0,127]` with `Cargo[i].sIndex == 0`.
6. On success: place; `SendItem`; client text; `Result = 0`; echo to DBSrv; log `_web1_`/`_web2_`; `SaveUser(conn, 0)` if `USER_SELCHAR`.
7. On failure: `Result = 3`; echo; `SaveUser(conn, 1)`; log `_fail_empty_`.

---

#### BR-20: Unicast notice (`_MSG_NPNotice`, `Parm1 == 0`)

**Overview**: The player-scoped form of `_MSG_NPNotice` delivers a private message to one slot, and only while that slot is in play.

**Detailed description**:
Lines 341-351 cast to `MSG_NPNotice` and, when `Parm1 == 0` **and** `pUser[conn].Mode == USER_PLAY`, call `SendClientMessage(conn, m->String)`. The `Parm1` discrimination is what separates this from BR-13: the server-scope branch requires `Parm1 == 1`, the player-scope branch requires `Parm1 == 0`. A packet addressed to a player but carrying `Parm1 == 1` is dropped here (it would have been a broadcast if addressed to 0), and a packet addressed to `ID = 0` with `Parm1 == 0` is dropped there. The pair is therefore a two-dimensional switch and the only place in the component where two dimensions are combined.

The `Mode == USER_PLAY` gate means private notices are silently lost for players at the character-selection screen - a real behaviour that operators must know, because the same message issued one second before character login disappears with no trace other than `SendNotice`'s log (which this branch does not call, unlike BR-13).

`SendClientMessage` truncates at offsets 94/95 of the 96-byte panel string and appends to the socket buffer without flushing; delivery occurs on the 500 ms timer. `MSG_NPNotice.String` is `MAX_NOTIFY_LENGTH` = 96 bytes, exactly matching, so no over-read occurs despite the fixed-length copy inside `SendClientMessage`.

**Rule workflow**:
1. Receive `MSG_NPNotice` with `ID > 0`.
2. If `Parm1 != 0`: no action.
3. If `pUser[conn].Mode != USER_PLAY`: no action.
4. Append `_MSG_MessagePanel` with the text to that one socket.
5. Flush on the next timer tick.

---

#### BR-21: Account confirmation and session initialisation (`_MSG_DBCNFAccountLogin`)

**Overview**: This branch is the gate between "connected socket" and "authenticated account at character selection". It verifies that the database answered for the account this socket is actually logging in as, enforces the MAC block list, clears every chat/privilege flag, loads account cargo, coin and the character list, and forwards a client-shaped confirmation.

**Detailed description**:
The payload is `MSG_DBCNFAccountLogin{_MSG; char HashKeyTable[16]; int Unknow_28; STRUCT_SELCHAR sel; STRUCT_ITEM Cargo[128]; int Coin; char AccountName[16]; char Keys[12];}` (`Code/Basedef.h:1095-1109`). `STRUCT_SELCHAR` (`Code/Basedef.h:765-778`) is the character-selection record: 4 spawn points, 4 names, 4 `STRUCT_SCORE`, 4x16 equipment arrays, 4 guild ids, 4 coin values and 4 exp values.

Identity check comes first (line 358): `strcmp(m->AccountName, pUser[conn].AccountName)`. A mismatch - a database reply for a different account on this slot - produces `_NN_Try_Reconnect`, flushes, `CloseUser(conn)` and `return`. This is the only branch in the component that returns immediately after closing, and it is the correct shape; the MAC-block branch below it omits the `return` and is consequently defective.

The MAC block scan (lines 369-383) walks the 200-entry `pMac[]` table (`STRUCT_BLOCKMAC`, `Code/Basedef.h:916-919`; loaded from `../../DBSRV/Run/Mac.txt` by `CReadFiles::ReadMacblock`, `Code/TMSrv/CReadFiles.cpp:678-715`). Entries with all four octets zero are treated as empty and skipped. On a four-octet match with `pUser[conn].Mac` the branch sends `_NN_MAC_Block`, flushes and calls `CloseUser(conn)` - but only `break`s the loop, so execution continues into the state initialisation below with the socket already closed. The functional consequences are analysed as risk R-02 in section 10.

The initialisation block (lines 386-400) is a security-relevant reset list: `IsBillConnect`, `Whisper`, `Guildchat`, `PartyChat`, `KingChat`, `Admin`, `Chatting`, `Unk_2732`, `Unk_2728` are zeroed and `OnlyTrade` is set to 1. `Admin = 0` is the important one: administrative privilege is derived later, at character login, from character level (`>= 999` in this file, or `level-1000` in the whisper `/gm` path), never from the account record, so a re-login cannot inherit a stale flag.

The forwarding step rewrites the packet in place: `m->ID = ESCENE_FIELD + 2` (30002) and `m->Type = _MSG_CNFAccountLogin` (0x010A), then sends `sizeof(MSG_DBCNFAccountLogin)` bytes to the client (lines 397-434). Note that this sends the *database* structure, including `HashKeyTable[16]`, `Unknow_28` (set to `0xCCCCCCCC` by the producer, explicitly uninitialised marker) and `Keys[12]` - fields the client has no documented use for. `Code/Basedef.h:1095` carries a TODO acknowledging that the structure name and the client type do not match.

Cargo normalisation (lines 402-422) is a compatibility rewrite: for every cargo item with `sIndex` in `(0, MAX_ITEMLIST)`, if `g_pItemList[sIndex].nPos` is 64 or 192 (the two weapon/armour position classes), any effect of type `EF_DAMAGEADD` or `EF_DAMAGE2` is replaced by `EF_DAMAGE` in all three effect slots. This is applied to the packet buffer *before* forwarding and before copying into `pUser[conn].Cargo`, so client and server agree. The `evDelete` purge (lines 424-431) follows: when the global event flag is non-zero, any cargo item with `sIndex` in `[470, 500]` is replaced by `sIndex = 0` (an item wipe for an event-recall window), again on the packet buffer, so the wipe reaches both client and session state.

The tail (lines 436-470) copies `Cargo` (1,024 bytes), `Coin`, and `SelChar` (840 bytes) into the session, zeroes `Unk_1816` (the billing verification counter used by `_MSG_CharacterLogin.cpp`), sets `Unk5[0]` and `LastClientTick` to 0, logs `CNFAccountLogin Mac: a.b.c.d` under the account name and IP, and - when `BILLING > 0 && IsFree(sel)` - calls `SendBilling` and stamps `Unk_2732 = SecCounter` as a deadline marker. `IsFree` (`Code/TMSrv/Server.cpp:1363-1375`) returns 1 when `FREEEXP <= 0` or any of the four characters has `FREEEXP <= level < 999`. `SendBilling` is currently a stub that returns TRUE without contacting the billing server (`Code/TMSrv/Server.cpp:1377-1380`), so the whole billing sub-rule is inert in this build.

**Rule workflow**:
1. Receive `MSG_DBCNFAccountLogin` for `conn`.
2. If `strcmp(m->AccountName, pUser[conn].AccountName) != 0`: `_NN_Try_Reconnect`, flush, `CloseUser`, return.
3. For each `pMac[x]`: skip all-zero entries; on MAC match send `_NN_MAC_Block`, flush, `CloseUser(conn)`, break the loop (execution continues - defect).
4. Zero nine flags, set `OnlyTrade = 1`.
5. Rewrite `Type = _MSG_CNFAccountLogin`, `ID = 30002`; set `Mode = USER_SELCHAR`.
6. Normalise cargo effects for `nPos` 64/192; purge `sIndex` 470..500 when `evDelete`.
7. Send the packet to the client.
8. Copy `Cargo` -> `pUser[conn].Cargo`, `Coin`, `SelChar`.
9. If `BILLING > 0 && IsFree(sel)`: `SendBilling`, stamp `Unk_2732`.
10. Log the MAC; if `TransperCharacter`, send `_MSG_TransperCharacter(0,0)` to the client.

---

#### BR-22: New-account failure (`_MSG_DBNewAccountFail`)

**Overview**: A failed account-creation request signals `_MSG_NewAccountFail` to the client and closes the session.

**Detailed description**:
Lines 475-484 cast to `MSG_STANDARD`, overwrite `m->ID = ESCENE_FIELD + 3` (30003), call `SendClientSignal(conn, 0, _MSG_NewAccountFail)` and `CloseUser(conn)`. The `m->ID` assignment is vestigial: `m` is not transmitted afterwards, and `SendClientSignal` builds its own `MSG_STANDARD` with `ID = 0` (`Code/TMSrv/SendFunc.cpp:197-206`). The signal is queued, not flushed, and then `CloseUser` closes the socket - `CPSock::CloseSocket` zeroes the send buffer positions (`Code/CPSock.cpp:84-98`), so **the failure signal is never actually delivered**. The client only observes the disconnect.

That behaviour is uniform across the four `...Fail` branches that signal-then-close without a flush (BR-22, BR-23, BR-25, BR-29): they all lose their user-visible message. The two branches that do flush first (`_MSG_DBCNFAccountLogin`'s mismatch path at line 362 and `_MSG_DBAccountLoginFail_Block` at line 520) deliver their text. The producer for this branch is `Code/DBSrv/CFileDB.cpp:539`, reached when `DBWriteAccount` fails after creating a new account file.

**Rule workflow**:
1. Receive `_MSG_DBNewAccountFail` for `conn`.
2. Set `m->ID = 30003` (unused).
3. `SendClientSignal(conn, 0, _MSG_NewAccountFail)` - queued.
4. `CloseUser(conn)` - queue discarded, socket closed.

---

#### BR-23: Unknown account (`_MSG_DBAccountLoginFail_Account`)

**Overview**: The database reports that no account record exists for the requested name; the client is told so and disconnected.

**Detailed description**:
Lines 487-498 copy `m->ID` into a local `ID` that is never used, send `_NN_No_Account_With_That_Name` through `SendClientMessage`, call `SendMessageA` to force the flush, and then `CloseUser(conn)`. This is one of the two failure branches that *do* deliver their message, because the flush precedes the close.

The producer is `Code/DBSrv/CFileDB.cpp:622` (`SendDBSignal(conn, m->ID, _MSG_DBAccountLoginFail_Account)`), reached when `DBReadAccount` fails, and also from the reserved-name guard at `Code/DBSrv/CFileDB.cpp:617-625` which rejects login attempts for the `COM0..COM9` and `LPT0..LPT9` pseudo-account names. The dispatcher does not distinguish those two causes; the player sees the same text either way.

The unused local `int ID = m->ID;` is a decompilation remnant repeated in six consecutive branches (lines 491, 505, 531, 557, 569, 613) and has no effect.

**Rule workflow**:
1. Receive the type for `conn`.
2. `SendClientMessage(conn, _NN_No_Account_With_That_Name)`.
3. `pUser[conn].cSock.SendMessageA()` - flush.
4. `CloseUser(conn)`.

---

#### BR-24: Blocked account with tier (`_MSG_DBAccountLoginFail_Block`)

**Overview**: A blocked account is informed of its block tier, encoded as one of four single-letter grades (X, A, B, C) substituted into the `_SN_Blocked_Account` template.

**Detailed description**:
Lines 501-524 read `MSG_STANDARDPARM.Parm` and select the substitution character: 0 -> `"X"`, 1 -> `"A"`, 2 -> `"B"`, 3 -> `"C"`. The format call is `sprintf(temp, g_pMessageStringTable[_SN_Blocked_Account], "X")` etc., i.e. the template itself contains the `%s`. Note that when `Parm` is outside 0..3 no branch assigns `temp`, and the previous contents of the shared 4 KB `temp` buffer (whatever the last log line left there) are then sent to the client via `SendClientMessage(conn, temp)`. Because `temp` is a process-global (`Code/TMSrv/Server.cpp:277`), that is an information-leak path whose content is unpredictable but non-empty.

The branch flushes before closing, so unlike BR-22 the message is delivered. The producer is `Code/DBSrv/CFileDB.cpp`'s block check during `_MSG_DBAccountLogin`, which forwards the admin-chosen block class. The dispatcher performs no logging of its own here, so the tier is only visible to the player and in DBSrv's log.

**Rule workflow**:
1. Receive `MSG_STANDARDPARM{Type=_MSG_DBAccountLoginFail_Block}`.
2. Map `Parm` 0/1/2/3 to `"X"`/`"A"`/`"B"`/`"C"`.
3. `sprintf(temp, template, letter)` (undefined content when `Parm` is out of range).
4. `SendClientMessage(conn, temp)`; flush; `CloseUser(conn)`.

---

#### BR-25: Disabled account (`_MSG_DBAccountLoginFail_Disable`)

**Overview**: Reports that the account has been administratively disabled and closes the session.

**Detailed description**:
Lines 527-538 send `_NN_Disabled_Account`, flush, and close. As with BR-23 the flush precedes the close, so the text is delivered. The producer side distinguishes "disabled" from "blocked": `_MSG_DBAccountLoginFail_Disable` is emitted for a time-limited disable (`DisableAccount` with a year/year-day window on the DBSrv), whereas `_Block` carries the permanent grade. The dispatcher does not surface the disable expiry date to the player in this branch, even though `MSG_STANDARDPARM` has room for one parameter.

This branch is also the message used by BR-39's second mode (`m->Mode == 1` in `_MSG_DBSavingQuit`), so an account disabled while online receives the same text through a different path.

**Rule workflow**:
1. Receive the type for `conn`.
2. `SendClientMessage(conn, _NN_Disabled_Account)`.
3. Flush.
4. `CloseUser(conn)`.

---

#### BR-26: Numeric-token success relay (`_MSG_AccountSecure`)

**Overview**: The database's "PIN/numeric token verified" answer is relayed to the client as a `_MSG_AccountSecure` signal carrying `ID = 30000`.

**Detailed description**:
Lines 541-544 are a single call: `SendClientSignal(std->ID, ESCENE_FIELD, _MSG_AccountSecure)`. `std->ID` and `conn` are the same value. The client receives `MSG_STANDARD{Type=_MSG_AccountSecure, ID=30000}`, which tells it to proceed to the character list. The packet's own payload (`MSG_AccountSecure{NumericToken[6]; Unknown[10]; int ChangeNumeric;}`) is discarded - only the type is forwarded.

The database side (`Code/DBSrv/CFileDB.cpp:1357-1421`) implements the state machine that decides success versus failure: a first-time token is stored and confirmed, a matching token sets the internal `SecurePass = 1`, a change request rewrites the stored token, and anything else sets `SecurePass = -1` and answers `_MSG_AccountSecureFail`. TMSrv is a pure relay and holds no verification state of its own, so the security of the token comparison rests entirely with DBSrv's `strncmp(..., 6)`.

**Rule workflow**:
1. Receive `_MSG_AccountSecure` for `conn`.
2. `SendClientSignal(conn, 30000, _MSG_AccountSecure)`.
3. Flush on the next timer tick.

---

#### BR-27: Numeric-token failure relay (`_MSG_AccountSecureFail`)

**Overview**: The negative counterpart of BR-26; the client is signalled that the token did not match.

**Detailed description**:
Lines 547-550 relay the failure with `SendClientSignal(std->ID, ESCENE_FIELD, _MSG_AccountSecureFail)`. Unlike the login-failure branches, the session is **not** closed: the client is expected to re-prompt for the token, and the account remains in its pre-selection state on the server. That asymmetry is deliberate in the sense that the client owns the retry loop, but it also means a client that ignores the signal can remain connected indefinitely in the pre-login state, subject only to the generic idle handling (`LastReceiveTime` versus `SecCounter`, compared at `Code/TMSrv/Server.cpp:4306-4317`).

No attempt counter is maintained on the TMSrv side for token failures; the only throttling is the account-level wrong-password counter in `CheckFailAccount` (`_MSG_AccountLogin.cpp:82`), which counts password failures, not token failures.

**Rule workflow**:
1. Receive `_MSG_AccountSecureFail` for `conn`.
2. `SendClientSignal(conn, 30000, _MSG_AccountSecureFail)`.
3. Session stays open; client decides whether to retry.

---

#### BR-28: Once-per-day restriction notice (`_MSG_DBOnlyOncePerDay`)

**Overview**: Informs the player that a daily action has already been performed, and flushes without closing the session.

**Detailed description**:
Lines 553-562 send `_NN_Only_Once_Per_Day` and flush, leaving the session open. This is the only "informational failure" branch on the login side that neither closes nor changes state. It has **no producer in the repository**: `_MSG_DBOnlyOncePerDay` appears in `Code/Basedef.h:1126` and in this file only. The companion `BASE_CheckPacket` size table would have expected `sizeof(MSG_STANDARD)` (`Code/Basedef.cpp` line-range 6550-6570), confirming the intended shape, but no DBSrv code emits it. The branch is therefore dead in the current build and is retained here for completeness of the protocol inventory.

The name suggests a per-day reward gate (for example a daily quest or a daily donation bonus), which today must be enforced elsewhere or not at all.

**Rule workflow**:
1. Receive `_MSG_DBOnlyOncePerDay` for `conn`.
2. `SendClientMessage(conn, _NN_Only_Once_Per_Day)`.
3. Flush. Session remains in whatever mode it was.

---

#### BR-29: Wrong password (`_MSG_DBAccountLoginFail_Pass`)

**Overview**: Reports an invalid password and closes the session.

**Detailed description**:
Lines 565-576 send `_NN_Wrong_Password`, call `SendMessageA` and `CloseUser`. The flush precedes the close, so the message reaches the client. The producer is DBSrv's password comparison during `_MSG_DBAccountLogin`; the dispatcher does not increment or inspect any failure counter, so the three-strikes rule lives entirely in TMSrv's own `CheckFailAccount`/`FailAccount[]` array (`_MSG_AccountLogin.cpp:82-90`, `Code/TMSrv/Server.cpp:1340-1361`), which records failures *before* the database is consulted (it is populated on the client's local rejection path, not on this response).

The branch shares its shape with BR-22/23/25 and, like them, carries an unused `int ID = m->ID;` local.

**Rule workflow**:
1. Receive the type for `conn`.
2. `SendClientMessage(conn, _NN_Wrong_Password)`.
3. Flush; `CloseUser(conn)`.

---

#### BR-30: Character-creation confirmation (`_MSG_DBCNFNewCharacter`)

**Overview**: After a successful character creation, the refreshed character list is forwarded to the client and the session returns to character selection.

**Detailed description**:
Lines 579-592 rewrite `Type = _MSG_CNFNewCharacter` and `ID = 30001`, forward `sizeof(MSG_CNFNewCharacter)` bytes, and set `pUser[conn].Mode = USER_SELCHAR`. `MSG_CNFNewCharacter{_MSG; STRUCT_SELCHAR sel;}` (`Code/Basedef.h:1301-1306`) is exactly `MSG_DBCNFNewCharacter`'s payload, so the forward is a safe reinterpretation: the client receives the whole four-character selection record rebuilt by `DBGetSelChar` on the database side.

Note that the branch does **not** update `pUser[conn].SelChar` from the packet. The server-side selection record is therefore stale with respect to the newly created character until the next account login or until `_MSG_DBCNFDeleteCharacter` (BR-31) arrives, which also does not update it. In practice the client drives the next action (character login) using the slot number, and `pUser[conn].Slot` is only set at character login (line 821), so the staleness is not immediately harmful - but it does mean the dispatcher deliberately treats `SelChar` as write-once-per-account-session.

**Rule workflow**:
1. Receive `_MSG_DBCNFNewCharacter` for `conn`.
2. Rewrite `Type = _MSG_CNFNewCharacter`, `ID = 30001`.
3. Forward to client.
4. `pUser[conn].Mode = USER_SELCHAR`.

---

#### BR-31: Character-deletion confirmation (`_MSG_DBCNFDeleteCharacter`)

**Overview**: Same contract as BR-30 for the deletion path: forward the refreshed selection record, return to character selection.

**Detailed description**:
Lines 595-606 rewrite `Type = _MSG_CNFDeleteCharacter` and `ID = 30001`, forward, and set `USER_SELCHAR`. `MSG_CNFDeleteCharacter` is `{_MSG; STRUCT_SELCHAR sel;}` (`Code/Basedef.h:1309-1314`). The branch is also used as a *not* deletion confirmation in one place: the DBSrv capsule-put path sends a `MSG_CNFDeleteCharacter`-shaped packet whose `Type` was set to `_MSG_DBCNFDeleteCharacter` (`Code/DBSrv/CFileDB.cpp:1751-1759`) so that the client's character list is refreshed after a character was sealed into a capsule. The dispatcher cannot distinguish the two semantics, which is why `_MSG_DBCNFCapsuleSucess` (BR-47) exists alongside it.

As with BR-30, `pUser[conn].SelChar` is not refreshed, so a deleted character remains visible in the server-side copy until the next account login.

**Rule workflow**:
1. Receive `_MSG_DBCNFDeleteCharacter` for `conn`.
2. Rewrite `Type = _MSG_CNFDeleteCharacter`, `ID = 30001`.
3. Forward.
4. Mode = `USER_SELCHAR`.

---

#### BR-32: Character-deletion failure (`_MSG_DBDeleteCharacterFail`)

**Overview**: Signals delete failure and keeps the session at character selection.

**Detailed description**:
Lines 609-619 rewrite `m->ID = 30001` (again vestigial, since `SendClientSignal` builds its own header), call `SendClientSignal(conn, 0, _MSG_DeleteCharacterFail)` and set `USER_SELCHAR`. The signal is queued and *not* flushed before returning, but unlike BR-22 the session is not closed, so the 500 ms timer eventually delivers it. This is the pattern the "signal-only" branches rely on: queue and return, let `ProcessSecTimer` flush.

DBSrv produces this type from the deletion handler (`Code/DBSrv/CFileDB.cpp`, four occurrences) for causes such as a non-empty slot mismatch or a write failure. The dispatcher does not distinguish causes.

**Rule workflow**:
1. Receive the failure type.
2. `SendClientSignal(conn, 0, _MSG_DeleteCharacterFail)` - queued.
3. Mode = `USER_SELCHAR`.

---

#### BR-33: Character-creation failure (`_MSG_DBNewCharacterFail`)

**Overview**: Signals creation failure and keeps the session at character selection.

**Detailed description**:
Lines 622-632 mirror BR-32 with `_MSG_NewCharacterFail`. The type is heavily used by DBSrv (19 occurrences) - primarily name-already-exists, invalid class, invalid slot and file-write failures - all of which collapse into one client signal. Because the message carries no reason code and the branch does not log, diagnosing a creation failure requires the DBSrv log.

**Rule workflow**:
1. Receive `_MSG_DBNewCharacterFail`.
2. `SendClientSignal(conn, 0, _MSG_NewCharacterFail)`.
3. Mode = `USER_SELCHAR`.

---

#### BR-34: Character login - full live-character reconstruction (`_MSG_DBCNFCharacterLogin`)

**Overview**: The largest branch in the component (447 lines, `635-1081`) converts a persisted character record plus its extra blob, affects, short-skill bar and donate balance into a live, grid-placed, stat-recalculated player, broadcasts its appearance, resolves its spawn point, seeds event state and reports ranking progress. Eighteen distinct sub-rules compose it.

**Detailed description**:
The input is `MSG_CNFCharacterLogin` (`Code/Basedef.h:1345-1367`), which contains a `STRUCT_MOB` (812 bytes as laid out by the compiler), 208 bytes of `unk`, slot/client-id/weather, the 16-byte short-skill bar, 765 bytes of `Unk2`, 32 `STRUCT_AFFECT` records, a `STRUCT_MOBEXTRA` extra blob and the donate balance. The first action is a truncating `memcpy` into a *client* structure, `MSG_CNFClientCharacterLogin` (`Code/Basedef.h:1368-1386`), which is the same layout minus the trailing `affect`, `mobExtra` and `Donate` fields (line 641). Everything after that operates on either `sm` (the client copy) or `m` (the database packet), and the split is the source of the dead-code defect analysed as R-04.

The branch's centre of gravity is state construction on `pMob[conn]` and `pUser[conn]`, which between lines 733 and 913 executes: mob assignment, extra-blob copy, citizen resolution from the guild table, affect array copy, short-skill copy, carry-capacity derivation, merchant-field zeroing, arch mortal-level derivation, HP/MP recomputation, score recomputation, bonus-point recomputation, mob-mode promotion, and finally a full session-state reset table. After that it resolves a spawn coordinate through a four-stage precedence chain, writes the resulting grid occupancy, and only then emits the login packet to the player (line 984) followed by eight outbound broadcast/refresh calls (lines 990-1013) and three informational checks (lines 1015-1073).

The branch is also the only place where the server enforces two structural invariants on a character record: class validity (`0 <= Class <= MAX_CLASS-1` = 0..3, lines 859-866) and the existence of a free grid cell (`GetEmptyMobGrid`, lines 887-896). Both failures log and `CloseUser(conn)` and then `break` - and because `CloseUser` resets the session to `USER_EMPTY` (the mode at this point is `USER_CHARWAIT`, not `USER_ACCEPT`, so the reset branch runs), the subsequent fall-through does not corrupt anything permanently, unlike BR-21's MAC path.

**Sub-rule inventory**:

| Sub-rule | Description | Location |
|----------|-------------|----------|
| SL-01 | Payload re-interpretation into the client structure (`memcpy` truncation) | 641 |
| SL-02 | Equip effect normalisation, 16 slots, `nPos` 64/192, `EF_DAMAGE2`/`EF_DAMAGEADD` -> `EF_DAMAGE` (applied to `sm.mob.Equip`) | 654-674 |
| SL-03 | Carry effect normalisation, 63 slots (0..62), same mapping (applied to `sm.mob.Carry`) | 676-696 |
| SL-04 | `evDelete` purge of `sIndex` 470..500 from carry (applied to `sm.mob.Carry`) | 698-705 |
| SL-05 | ClassMaster equip[0] effect injection: MORTAL/ARCH -> `{98,0}` and `{106,Equip[0].sIndex}`; CELESTIAL/SCELESTIAL/CELESTIALCS -> `{98,3}` and `{106,Equip[0].sIndex}` | 706-726 |
| SL-06 | Donate balance adoption | 728 |
| SL-07 | HP floor: `CurrentScore.Hp <= 0` -> 2 (prevents logging in dead) | 730-731 |
| SL-08 | Mob adoption, extra-blob adoption, citizen from `GuildInfo`, short-skill and affect adoption | 733-749 |
| SL-09 | Carry capacity: base 30, +15 if `Carry[60].sIndex == 3467`, +15 if `Carry[61].sIndex == 3467` (maximum 60) | 751-757 |
| SL-10 | `Merchant` cleared on `BaseScore`; `Tab`/`Snd` cleared | 760-765 |
| SL-11 | Arch mortal-level derivation: `SelChar.Score[MortalSlot].Level - 299` when `ClassMaster == ARCH` and `0 <= MortalSlot < 3`, otherwise 99 | 767-771 |
| SL-12 | Statistic recomputation: `BASE_GetHpMp`, `GetCurrentScore`, `BASE_GetBonusSkillPoint`, `BASE_GetBonusScorePoint`; `pMob.Mode = MOB_USER` | 773-782 |
| SL-13 | Client packet finalisation: `Type=_MSG_CNFCharacterLogin`, `ID = 30001` when `NewbieEventServer == 1` else 30000, positions, `ClientID`, `Weather`, three zeroed reserved areas, account name at `Unk2[448]` | 784-803 |
| SL-14 | Kill-mark seed: if `Carry[63].sIndex == 0`, install item 547 with effects `EF_CURKILL`/`EF_LTOTKILL`/`EF_HTOTKILL` | 809-817 |
| SL-15 | Session reset table: 14 counters/flags, `Unk9` filled with -1 (400 bytes), `Trade` zeroed, `InvenPos[0..14] = -1`, `AutoTrade.CarryPos[0..11] = -1`, `TradeMode = 0`, `PKMode = 0` | 819-846 |
| SL-16 | Spawn resolution: city spawn from `Merchant` bits 6-7 plus `rand()%15`; guild-zone override on `ChargeGuild` match; newbie spot `(2112 +- 2, 2042 +- 2)` for `level < FREEEXP && MORTAL`; then `GetEmptyMobGrid` | 848-904 |
| SL-17 | Mode promotion to `USER_PLAY`, `GetCurrentScore`, `GuildDisable = 0`, guild mantle/clan correction | 906-975 |
| SL-18 | Presence broadcast and post-login notifications (CreateMob type 2, grid occupancy, `GridMulticast`, `SendPKInfo`, `SendGridMob`, admin flag, `MountProcess`, `SendWarInfo`, castle state, `ClearCrown`, EXP segment, billing, Kefra, RvR, `SendEtc`, `SendScore`, login log) | 984-1080 |

Detailed notes on the sub-rules with the highest behavioural impact follow.

**SL-02/SL-03/SL-04 (effect normalisation and event purge)**: The normalisation is a data-migration service. Older item data encodes "added damage" as `EF_DAMAGEADD` or `EF_DAMAGE2`; the current combat code expects `EF_DAMAGE` (`Code/ItemEffect.h`). Doing the rewrite at login guarantees that a stored legacy item cannot produce divergent damage calculations between client display and server computation. The event purge is a kill-switch for event-only items: setting `sIndex = 0` removes the item entirely rather than hiding it. Because both operations write to `sm.mob.*` and `sm` is later overwritten from `pMob[conn].MOB` (line 784), **neither the normalisation nor the purge reaches the client or the server state** on this path - the effective behaviour is "none". The account-login path (BR-21) performs the identical normalisation on `m->Cargo`, where it *does* survive. See risk R-04.

**SL-05 (class-master cosmetic injection)**: For every class-master tier the first equipment slot receives two synthetic effects: effect 98 (a "class" marker) with value 0 for mortal/arch and 3 for the three celestial tiers, plus effect 106 (a "face/skin override") whose value is the item index already worn in slot 0. This lets the client render the class-master skin without a distinct item. It is applied to `m->mob.Equip[0]` (not `sm`), so it *does* survive into `pMob[conn].MOB` and into `sm` after line 784. Note that `GetCurrentScore` zeroes `Equip[0].stEffect[0]` for players below level 2000 (`Code/TMSrv/CMob.cpp:622-623`), so the injection deliberately uses effect slots 1 and 2.

**SL-09 (carry capacity)**: `pMob[conn].MaxCarry` is recomputed from scratch at every login: 30 base, plus 15 for each of two specific "bag" items (index 3467) held in carry slots 60 and 61. Because the computation is absolute rather than incremental, it is idempotent and immune to stale values, which is the right shape for a derived attribute. `MaxCarry` is enforced by the client UI and by `_MSG_GetItem`-family handlers; the value 60 is therefore a hard cap on free inventory.

**SL-11 (arch mortal level)**: Arch characters (`ClassMaster == ARCH`) can hold one mortal character inside themselves. `QuestInfo.Arch.MortalSlot` records which of the three available slots is used; `MortalLevel` is derived as `SelChar.Score[slot].Level - 299`. The `-299` converts the absolute celestial level numbering (which starts at 300) back to a mortal level. The `else` arm sets 99 for every non-arch and for arch characters with an out-of-range slot, which is the "no sub-character" sentinel. The bound checks (`>= 0 && < 3`) are correct here, unlike many other index checks in the file.

**SL-12 (statistic recomputation)**: `BASE_GetHpMp` (`Code/Basedef.cpp:1174-1208`) recomputes `BaseScore.MaxHp/MaxMp` from base class stats, constitution/intelligence deltas and a level multiplier that adds `MAX_LEVEL` (399) to the level for celestial tiers, then clamps to `MAX_HP`/`MAX_MP` (2,000,000,000). It does not touch current HP/MP, which is why SL-07 must floor `Hp` separately. `GetCurrentScore` (`Code/TMSrv/CMob.cpp:598`) then applies equipment and affect bonuses, clamps resists at 100, zeroes `Clan` and `Rsv`, sets `Parry`, forces `pUser[idx].Range = 23`, and clears the per-mob bonus accumulators. `BASE_GetBonusSkillPoint`/`BASE_GetBonusScorePoint` (`Code/Basedef.cpp:858`) recompute the spendable points from class baselines and level and *claw back* over-allocated stats when the stored distribution exceeds what the level allows (the subtraction cascade at `Basedef.cpp:887-900`). Together these three calls make the login path the single place where a character's derived statistics are re-established from persisted primitives.

**SL-16 (spawn resolution)**: The precedence chain is: (1) city spawn derived from `MOB.Merchant` bits 6-7 (`(Merchant & 0xC0) >> 6`, giving 0..3) plus a 15-cell random offset, using `g_pGuildZone[CityID].CitySpawnX/Y`; (2) overridden by `g_pGuildZone[n].GuildSpawnX/Y` for the first zone whose `ChargeGuild` equals the character's guild (loop over the five zones); (3) overridden by the fixed newbie area `(2112, 2042)` with a +-2 jitter when `BaseScore.Level < FREEEXP` (35) and the character is mortal; (4) then `GetEmptyMobGrid(conn, &tx, &ty)` scans outward for a free cell. Failure of step 4 logs `"Can't start can't get mobgrid"` and closes the session. The `Merchant & 0xC0` field is the "city/kingdom origin" of the character, and `Cap` is 3 because `MAX_GUILDZONE` is 5 but only 4 city values exist - the code does not clamp, relying on the 2-bit extraction.

**SL-17 (guild mantle/clan correction)**: For guilded characters, if `MOB.Clan` differs from the guild's recorded `Clan`, the code rewrites `Equip[15]` (the mantle) to the variant matching the guild's clan. Two clan families are handled - clan 7 and clan 8 - with six inheritance rules each: celestial tiers always get 3197/3198; mantles 543-544 map to 543/544; 545-546 to 545/546; 548 to 543/544; 549 to 545/546; 3191-3193 to 3191/3192; 3194-3196 to 3194/3195. Any other mantle index is left untouched. `sm.mob.Equip[15]` is patched so the client sees the same item. The local `int Server = pMob[conn].MOB.Guild / MAX_GUILD;` (line 918) is computed and never used - the author intended a cross-server guild check that was never written.

**SL-18 (post-login fan-out)**: The composition order matters. The client packet is sent (line 984) *before* the appearance broadcast, so the logging-in player learns their own state first; `pMobGrid[Y][X] = conn` (line 997) is written before `GridMulticast` so that other players' `GetCreateMob` calls can resolve the occupant; `SendPKInfo`, `SendGridMob`, `MountProcess`, `SendWarInfo`, castle-state signal and `ClearCrown` each depend on `Mode == USER_PLAY`, which SL-17 set at line 906. The EXP "segment" block (1015-1054) quarters the gap between the current and next level thresholds using `g_pNextLevel` (mortal/arch, `MAX_LEVEL` 399) or `g_pNextLevel_2` (celestial, `MAX_CLEVEL` 199) and stores 0..3 in `pMob.Segment` for the client's EXP bar; it also stamps `Unk_2736` and, for levels at or above `FREEEXP`, calls `SendBilling` when `CHARSELBILL == 0` (another inert call in this build). The Kefra block (1056-1062) formats `_SN_End_Khepra` with the killer name obtained from `BASE_GetGuildName`; the RvR block (1064-1073) selects the `_SN_KINGDOMWAR_DROP_` text by clan (8 -> red, 7 -> blue). The final line writes the canonical login audit record: `sta,Login char:<name> exp:<exp> level:<level> conn:<slot> money:<coin> store:<donate>` under the account name and IP.

**Rule workflow**:
1. Receive `MSG_DBCNFCharacterLogin` for `conn`; copy into the client structure (SL-01).
2. Normalise equip and carry effects; purge event items (SL-02..SL-04). [result discarded - R-04]
3. Inject class-master effects into `Equip[0]` (SL-05).
4. Adopt donate; floor HP at 2; adopt mob, extra, citizen, short skills, affects (SL-06..SL-08).
5. Derive `MaxCarry`; clear `Merchant`, `Tab`, `Snd`; derive arch mortal level (SL-09..SL-11).
6. Recompute HP/MP, current scores, skill and score bonus points; set `MOB_USER` (SL-12).
7. Compose the client packet: type, id (newbie variant), positions, weather, account name (SL-13).
8. Seed kill-mark item if absent (SL-14).
9. Reset 14 counters and the trade/auto-trade arrays (SL-15).
10. Resolve spawn: city -> guild zone -> newbie spot -> free grid; abort on no free grid (SL-16).
11. Promote to `USER_PLAY`; recompute scores; correct mantle/clan (SL-17).
12. Send login packet; broadcast appearance; refresh PK/grid/mount/war/castle/crown state (SL-18).
13. Derive EXP segment; bill if applicable; emit Kefra and RvR notices; send etc/score snapshots; write audit log.

---

#### BR-35: Character login failure (`_MSG_DBCharacterLoginFail`)

**Overview**: Reports that the database refused the character login, returns the session to character selection and records a crack-log entry.

**Detailed description**:
Lines 1084-1096 set `m->ID = ESCENE_FIELD` (30000, unused because `SendClientSignal` rebuilds the header), signal `_MSG_CharacterLoginFail`, set `USER_SELCHAR` and call `CrackLog(conn, " CharLoginFail")`. `CrackLog` (`Code/TMSrv/Server.cpp:6671-6699`) formats the connection, mode and account name into `temp` and appends the message - it is the codebase's term for "a client action that should never happen". Its presence here means the author regarded a database-side login refusal as an anomaly, not as a normal outcome.

The producer set is empty in this repository: `_MSG_DBCharacterLoginFail` appears only in `Code/Basedef.h:1116` and in this file (plus the commented-out `BASE_CheckPacket` table at `Code/Basedef.cpp:6563`). In a current DBSrv build, character-login failures are instead expressed by never answering (the client times out) or by closing. The branch is protocol-complete but currently unreachable.

**Rule workflow**:
1. Receive `_MSG_DBCharacterLoginFail`.
2. Signal `_MSG_CharacterLoginFail`.
3. Mode = `USER_SELCHAR`.
4. `CrackLog(conn, " CharLoginFail")`.

---

#### BR-36: Message-box relay (`_MSG_DBMessageBoxOk`)

**Overview**: Relays a modal message box to the client with `Type = _MSG_MessageBoxOk` and `ID = 0`.

**Detailed description**:
Lines 1099-1108 rewrite `Type = _MSG_MessageBoxOk` and `ID = 0` in place and forward `sizeof(MSG_MessageBoxOk)`. The structure is `{_MSG; int Useless1; int Useless2; char String[96];}` (`Code/Basedef.h:1202-1208`), and `SendClientMessageOk` on the client-facing side (`Code/TMSrv/SendFunc.cpp:179-195`) builds the same shape. Unlike the chat-panel path, this one is modal, so it is used for interruptions that require acknowledgement.

The type has **no producer in the repository**: `_MSG_DBMessageBoxOk` (0x0402) is referenced only by `Code/Basedef.h:977`, this branch, and the disabled validator at `Code/Basedef.cpp:6555`. It is a reserved channel for NP-issued interactive messages.

**Rule workflow**:
1. Receive `_MSG_DBMessageBoxOk` for `conn`.
2. Rewrite `Type = _MSG_MessageBoxOk`, `ID = 0`.
3. Forward; flush on the next tick.

---

#### BR-37: "Already playing" termination (`_MSG_DBAlreadyPlaying`)

**Overview**: Signals `_MSG_AlreadyPlaying` to the client and terminates the session when the database reports the account is already connected.

**Detailed description**:
Lines 1111-1121 call `SendClientSignal(m->ID, ESCENE_FIELD + 2, _MSG_AlreadyPlaying)`, then `pUser[m->ID].cSock.SendMessageA()` to flush, then `CloseUser(conn)`. Since `m->ID == conn` (both read the same header field), the signal goes to the same slot that is closed; the flush ensures delivery before the socket teardown. This is the correct ordering, and it is the model the failure-signal branches should have followed (compare BR-22).

The producer (`Code/DBSrv/CFileDB.cpp:694`) emits it during account login when the account is already flagged as connected *and* `m->DBNeedSave == 0`, i.e. the client did not request take-over. The companion `_MSG_DBStillPlaying` (BR-38) handles the take-over case.

**Rule workflow**:
1. Receive `_MSG_DBAlreadyPlaying` for `conn`.
2. `SendClientSignal(conn, 30002, _MSG_AlreadyPlaying)`.
3. Flush.
4. `CloseUser(conn)`.

---

#### BR-38: "Still playing" termination with take-over (`_MSG_DBStillPlaying`)

**Overview**: Signals `_MSG_StillPlaying`, terminates the new session, and - on the database side - initiates a forced save-and-quit of the previously connected session.

**Detailed description**:
Lines 1124-1134 are identical in shape to BR-37 with `_MSG_StillPlaying`. The difference is entirely in the producer: `Code/DBSrv/CFileDB.cpp:699-700` sends `_MSG_DBStillPlaying` **and** calls `SendDBSavingQuit(IdxName, 0)` for the *other* connection, which causes that server to execute BR-39 with `Mode = 0` ("your account is in use from elsewhere") and force the old session off. The pair therefore implements "kick the old session, tell the new one to retry": the new client is disconnected so it can reconnect once the old one has saved.

TMSrv does not itself coordinate the two: it simply terminates whichever slot the packet addresses. If the packet is misrouted the wrong player is kicked; there is no cross-check against `AccountName`.

**Rule workflow**:
1. Receive `_MSG_DBStillPlaying` for `conn`.
2. Signal `_MSG_StillPlaying` with `ID = 30002`.
3. Flush.
4. `CloseUser(conn)`.
5. (On the peer side) the old session receives `_MSG_DBSavingQuit` with `Mode = 0`.

---

#### BR-39: Forced save-and-quit of an occupied session (`_MSG_DBSavingQuit`)

**Overview**: A database directive that terminates a specific session, optionally explaining why: `Mode == 0` means "account in use elsewhere", `Mode == 1` means "account disabled".

**Detailed description**:
The packet is `MSG_DBSavingQuit{_MSG; char AccountName[16]; int Mode;}` (`Code/Basedef.h:1003-1008`). The branch opens with a range test on `m->ID` (lines 1141-1146) that is tautologically true because BR-01 already validated the same field - another example of decompiled redundancy. It then applies two *overlapping and contradictory* tests:

* lines 1148-1156: if `Mode != USER_PLAY && Mode != USER_SAVING4QUIT`, send `_MSG_DBNoNeedSave` for this slot to the database;
* lines 1158-1166: if `Mode == USER_PLAY || Mode == USER_SELCHAR`, send the reason text to the client and flush.

`USER_SELCHAR` (11) satisfies the first condition (it is neither 22 nor 24) *and* the second. A player at the character-selection screen therefore causes TMSrv to tell the database "this slot needs no save" **and** to display a message. For `USER_CHARWAIT` (12), `USER_LOGIN` (2) and `USER_ACCEPT` (1) only the first branch fires; for `USER_PLAY` only the second. The two branches are clearly meant to be an `else if`, and the reason text is itself only defined for `Mode` 0 and 1 (`_NN_Your_Account_From_Others`, `_NN_Disabled_Account`); a `m->Mode` of 2 or more sends an uninitialised `temp` through `SendClientMessage` (same leak pattern as BR-24).

The close is unconditional (line 1168): `CloseUser(m->ID)`. When the session was `USER_PLAY`, `CloseUser` takes the save path: it removes the party, closes any trade, builds a full `MSG_SavingQuit` with the mob, cargo, short skills, affects, extra blob, coin, donate and slot, sends it to DBSrv, and marks the slot `USER_SAVING4QUIT` (`Code/TMSrv/Server.cpp:6869-6919`). That is how a kicked player's data is preserved. When the session was at `USER_SELCHAR`, `CloseUser` takes the non-play path, sends `_MSG_DBNoNeedSave` (a second copy, in addition to line 1155) and resets the slot.

**Rule workflow**:
1. Receive `_MSG_DBSavingQuit` for `conn`.
2. Range check on `m->ID` (always passes).
3. If `Mode` is neither `USER_PLAY` nor `USER_SAVING4QUIT`: send `_MSG_DBNoNeedSave(conn)`.
4. If `Mode` is `USER_PLAY` or `USER_SELCHAR`: select text by `m->Mode` (0 -> "in use elsewhere", 1 -> "disabled"), send, flush.
5. `CloseUser(conn)` - which either initiates a full save (play mode) or resets the slot (other modes).

---

#### BR-40: Save-and-quit completion (`_MSG_DBCNFAccountLogOut`)

**Overview**: Acknowledges that the database has persisted the character and finished the logout; the server clears the mob and closes the socket.

**Detailed description**:
Lines 1172-1183 log `etc,charlogout conn:<slot> name:<mob name>` under the account name and IP, set `pMob[conn].Mode = MOB_EMPTY`, set `pUser[conn].Mode = USER_ACCEPT`, and call `CloseUser(conn)`. The producer is `Code/DBSrv/CFileDB.cpp:1259`, reached at the end of `_MSG_SavingQuit` after `DBWriteAccount` and `DBExportAccount` succeed and `RemoveAccountList` releases the slot on the database side.

The mode assignment is the crux. `CloseUser` (`Code/TMSrv/Server.cpp:6834-6926`) reads `Mode` and does nothing further when it equals `USER_ACCEPT` - it does not call `CUser::CloseUser()`, so `Mode` stays 1 and `AccountName` stays populated. Since `GetEmptyUser()` returns only slots in `USER_EMPTY` (`Code/TMSrv/Server.cpp:6743-6752`), and `CPSock::CloseSocket()` zeroes `nSendPosition` (`Code/CPSock.cpp:84-98`) so the 500 ms sweep at `ProcessSecMinTimer.cpp:201-221` never re-enters `CloseUser`, the slot is never reclaimed. This is analysed as risk R-03.

The branch nevertheless performs its primary job correctly: `pMob[conn].Mode = MOB_EMPTY` removes the character from the mob table, and the socket is closed, so the player is gone from the world. `SaveUser` is *not* called here - the save already happened as part of the `MSG_SavingQuit` exchange, which is why the branch does not duplicate it.

**Rule workflow**:
1. Receive `_MSG_DBCNFAccountLogOut` for `conn`.
2. Log the logout with account name and character name.
3. `pMob[conn].Mode = MOB_EMPTY`.
4. `pUser[conn].Mode = USER_ACCEPT`.
5. `CloseUser(conn)` - closes the socket; because the mode is `USER_ACCEPT`, no further reset occurs.

---

#### BR-41: Arch sub-character creation success (`_MSG_DBCNFArchCharacterSucess`)

**Overview**: After an arch character is successfully created, wipe the two temporary equipment slots, persist, log the character out, and play the arch transformation effect.

**Detailed description**:
Lines 1186-1206 read `MSG_STANDARDPARM.Parm` (the character slot that was created), send `_NN_My_King_Bless1`, then zero and re-send equipment slots 10 and 11 via `SendItem(ITEM_PLACE_EQUIP, ...)`, call `SaveUser(conn, 0)`, call `CharLogOut(conn)`, signal `_MSG_SendArchEffect` with the slot number, and log `etc,arch create name:<name> slot:<parm>`.

The order is deliberate and is the interesting part of the rule. Slots 10 and 11 are the "temporary/wing" equipment positions; they are cleared before the save so the newly created sub-character does not inherit them. `CharLogOut` (`Code/TMSrv/Server.cpp:7085-7125`) then re-saves (with export) and removes the character from the world; because it calls `SaveUser(conn, 1)` internally, this branch produces two back-to-back database writes, the second of which carries the cleared slots. The client is told to play the arch effect *after* the logout, which is what allows the transformation animation to run while the character is removed.

Note that the *payload* of the arch request (name, class, mortal face, mortal slot) is not carried in this reply; it was sent outbound in `MSG_DBCreateArchCharacter` (`Code/Basedef.h:1154-1162`) and the reply carries only the resulting slot.

**Rule workflow**:
1. Receive `MSG_STANDARDPARM{Type=_MSG_DBCNFArchCharacterSucess}` with `Parm` = slot.
2. `SendClientMessage(_NN_My_King_Bless1)`.
3. Zero `Equip[10]`, `SendItem(EQUIP,10)`; zero `Equip[11]`, `SendItem(EQUIP,11)`.
4. `SaveUser(conn, 0)`.
5. `CharLogOut(conn)` (saves with export, removes mob, signals `_MSG_CNFCharacterLogout`).
6. `SendClientSignalParm(conn, 30000, _MSG_SendArchEffect, Parm)`.
7. Log the creation.

---

#### BR-42: Arch sub-character creation failure (`_MSG_DBCNFArchCharacterFail`)

**Overview**: Reports "no empty slot" and restores the mob mode, keeping the player in the world.

**Detailed description**:
Lines 1209-1214 set `pMob[conn].Mode = MOB_USER` and send `_NN_NoEmptySlot`. Nothing else changes. The restore is necessary because `_MSG_Quest.cpp` sets `pMob[conn].Mode` to `MOB_WAITDB` (9) before sending `_MSG_DBCreateArchCharacter`; without the restore the player's mob would remain in a waiting state and stop being processed by the AI/physics sweep.

This is a good example of a branch whose whole purpose is to undo a transient state set by a *different* component - a coupling that is invisible from either file alone and only discoverable by reading both.

**Rule workflow**:
1. Receive `_MSG_DBCNFArchCharacterFail`.
2. `pMob[conn].Mode = MOB_USER`.
3. `SendClientMessage(_NN_NoEmptySlot)`.

---

#### BR-43: Cash/donate credit (`_MSG_DBSendDonate`)

**Overview**: Credits a purchased cash amount to the player's `Donate` balance, tells the client, and acknowledges to DBSrv.

**Detailed description**:
The packet is `MSG_DBSendDonate{_MSG; int Result; char Account[16]; int Donate;}` (`Code/Basedef.h:1292-1298`). The authorisation phase mirrors BR-19 exactly (`Mode < USER_SELCHAR` -> `_fail_play_`, account mismatch -> `_fail_name_`), with the item-code logging removed and left as comments (lines 1223, 1233). On success: `pUser[conn].Donate += m->Donate` (an additive credit, not an assignment - the DB sender must therefore send only the delta), `SendClientMessage(_NN_Cash_ChargeOk)`, `m->Result = 0`, echo to DBSrv, log `recv,<account> <donate> donate`.

Two properties matter operationally. First, the credit is additive and unsynchronised with the database: `SaveUser` is not called, so the new balance reaches disk only on the next periodic save (`ProcessSecMinTimer.cpp:189` `SaveUser(SaveCount, 0)`), on character logout (`CharLogOut`), or on forced quit (`CloseUser`). A crash between the credit and the next save loses the purchase while DBSrv has already marked the file processed. Second, on both refusal paths the handler `break`s without setting `Result` and without any reply, so DBSrv cannot detect the failure; the donate file has already been moved to the processed folder by the producer (`Code/DBSrv/CReadFiles.cpp:819-833` plus the move logic that follows).

`Donate` itself is surfaced to the client at character login (SL-06, line 728) and persisted as part of `MSG_DBSaveMob` (`Code/TMSrv/Server.cpp:7073`).

**Rule workflow**:
1. Receive `MSG_DBSendDonate` for `conn`.
2. If `Mode < USER_SELCHAR`: log `_fail_play_`, stop (no reply).
3. If `AccountName != m->Account`: log `_fail_name_`, stop (no reply).
4. `Donate += m->Donate`.
5. `SendClientMessage(_NN_Cash_ChargeOk)`.
6. `Result = 0`; echo to DBSrv; log `recv,...`.

---

#### BR-44: Capsule inventory relay (`_MSG_CNFDBCapsuleInfo`)

**Overview**: Forwards the database's capsule (sealed-character) record to the client unchanged.

**Detailed description**:
Lines 1254-1257 are a single `SendOneMessage((char*)Msg, sizeof(MSG_CNFDBCapsuleInfo))`. The structure is `{_MSG; int Index; STRUCT_SEALOFSOUL Capsule;}` (`Code/Basedef.h:1432-1437`). No `Type` or `ID` rewriting occurs, which is possible because `_MSG_CNFDBCapsuleInfo` is defined as `31 | FLAG_DB2GAME | FLAG_GAME2DB | FLAG_GAME2CLIENT` (`Code/Basedef.h:1431`) - the same numeric value is legal in all three namespaces. This is the only branch that forwards a packet without touching its header, and it is the clearest demonstration of why the flag-based type space works: the value is reused rather than translated.

The producer opens `./capsule/<Index>` on the database side and reads the sealed blob (`Code/DBSrv/CFileDB.cpp:1624-1680`). TMSrv keeps no capsule state, so this branch is stateless by design.

**Rule workflow**:
1. Receive `_MSG_CNFDBCapsuleInfo` for `conn`.
2. Forward `sizeof(MSG_CNFDBCapsuleInfo)` bytes as-is to the client.
3. No state change.

---

#### BR-45: Capsule put failure - no empty slot (`_MSG_DBCNFCapsuleCharacterFail`)

**Overview**: On failure to seal a character for lack of a free capsule slot, resynchronise the client's carry list and report the reason.

**Detailed description**:
Lines 1260-1265 call `SendCarry(conn)` then `SendClientMessage(_NN_NoEmptySlot)`. `SendCarry` (`Code/TMSrv/SendFunc.cpp:1534-1559`) requires `USER_PLAY` and a live socket, then sends `MSG_UpdateCarry` with all 64 carry slots and the coin value, closing the session if the append fails. The resynchronisation is necessary because the client optimistically removed the character's items when the seal was requested; without it the client would show a phantom-free inventory.

The producer (`Code/DBSrv/CFileDB.cpp:1792`) emits this when the capsule directory has no free slot. Note the branch ends with an explicit `break;` immediately before the closing `} break;` - a harmless but telling decompilation artefact that also appears in BR-46 (lines 1264, 1272).

**Rule workflow**:
1. Receive `_MSG_DBCNFCapsuleCharacterFail`.
2. `SendCarry(conn)` - authoritative carry snapshot.
3. `SendClientMessage(_NN_NoEmptySlot)`.

---

#### BR-46: Capsule put failure - invalid character id (`_MSG_DBCNFCapsuleCharacterFail2`)

**Overview**: The second capsule failure reason: the character id/name cannot be used. Same resynchronisation, different text.

**Detailed description**:
Lines 1268-1273 are the mirror of BR-45 with `_NN_CANT_USE_ID`. The producer (`Code/DBSrv/CFileDB.cpp:1801`) emits it when the requested character slot/name is not usable (for example the name does not match the slot). Because both branches send `SendCarry` and differ only in the message id, the client cannot distinguish "capsule storage full" from "wrong character" except by the text.

**Rule workflow**:
1. Receive `_MSG_DBCNFCapsuleCharacterFail2`.
2. `SendCarry(conn)`.
3. `SendClientMessage(_NN_CANT_USE_ID)`.

---

#### BR-47: Capsule put success - character erased (`_MSG_DBCNFCapsuleSucess`)

**Overview**: After a character is sealed into a capsule, the live mob and its extra blob are erased and the now-empty record is saved with export.

**Detailed description**:
Lines 1276-1282 perform two `memset`s (`STRUCT_MOB` and `STRUCT_MOBEXTRA` on the live player slot) followed by `SaveUser(conn, 1)`. The database has already copied the character into `./capsule/<Index>` and cleared it from the account file (`Code/DBSrv/CFileDB.cpp:1723-1747`), so this branch's job is to make the in-memory server state agree with the file before the next save overwrites it. `SaveUser(conn, 1)` sets `Export = 1`, which makes DBSrv additionally write `S:/export/account<ServerIndex>/<name>` (`Code/DBSrv/CFileDB.cpp:1186-1187`, `2471-2490`) - a belt-and-braces backup of the emptied record.

A collateral effect: the player's `pMob[conn].MOB` is zeroed while the session is presumably still `USER_PLAY` (the character was logged in when the capsule operation started; `_MSG_PutoutSeal.cpp` calls `CharLogOut(conn)` only for the *out* direction). The client is expected to follow this packet with the `_MSG_DBCNFDeleteCharacter`-shaped refresh that DBSrv sends immediately afterwards (`Code/DBSrv/CFileDB.cpp:1751-1759`), which returns it to character selection via BR-31. No explicit mode change happens here, so the ordering dependency between two separate packets is load-bearing.

**Rule workflow**:
1. Receive `_MSG_DBCNFCapsuleSucess`.
2. `memset` the live `STRUCT_MOB` to zero.
3. `memset` the live `STRUCT_MOBEXTRA` to zero.
4. `SaveUser(conn, 1)` - persist with export.
5. (Next packet) `_MSG_DBCNFDeleteCharacter` refreshes the client list and mode.

---

#### BR-48: Ranking progress relay with identity check (`_MSG_GrindRankingData`)

**Overview**: Ranking deltas are forwarded to the client only when the packet's `PlayerRank.Name` matches the character currently logged into that slot, preventing one player from receiving another's ranking record.

**Detailed description**:
Lines 1285-1296 implement the only explicit authorisation check on a data payload in the component: `if (pUser[conn].Mode == USER_PLAY && m->PlayerRank.Name[0] != '\0' && !strcmp(pMob[conn].MOB.MobName, m->PlayerRank.Name))`. The comment above it (lines 1287-1289) explains the intent - "in the char sel Mode (0xC) the tmsrv still doesn't have the player data, so ignore the playerRank.Name check" - but the condition additionally requires `USER_PLAY`, so the char-select case is excluded rather than specially handled.

The payload is `MSG_SendExpRanking{_MSG; int RankPosition; STRUCT_RANKING PlayerAbove, PlayerRank, PlayerUnder;}` (`Code/Basedef.h:2197-2215`), built on the DBSrv side by `RankingSystem::sendUpdateIndividualRank` (`Code/DBSrv/CRanking.cpp:279-316`). That function computes the neighbourhood (`PlayerAbove` for the last-ranked and middle cases, `PlayerUnder` for the first-ranked and middle cases) and routes by `pUser[tmsrvId].cSock` with `ID = playerId`, which is exactly the slot this check validates.

Because the forward uses `sizeof(MSG_SendExpRanking)` rather than `m->Size`, the client always receives the full fixed structure - a safe choice that contrasts with BR-06's `m->Size` echo.

Three senders feed this: `Code/TMSrv/MobKilled.cpp:341`, `435`, `528`, `622` (EXP gain events) each send `_MSG_UpdateExpRanking` upstream. A player who has just been removed from the ranking also receives a packet (`OUT_OF_RANK` with `PlayerAbove` = last place), which the name check accepts because `PlayerRank` is populated with the player's record in that call path.

**Rule workflow**:
1. Receive `_MSG_GrindRankingData` for `conn`.
2. If `Mode != USER_PLAY`: drop.
3. If `PlayerRank.Name[0] == '\0'`: drop.
4. If `strcmp(pMob[conn].MOB.MobName, PlayerRank.Name) != 0`: drop.
5. Forward `sizeof(MSG_SendExpRanking)` bytes to the client.

---

#### BR-49: Raw client text (`_MSG_DBClientMessage`)

**Overview**: Relays a 96-byte text line to the addressed client through the chat-panel channel, with no mode gate.

**Detailed description**:
Lines 1299-1304 read `MSG_DBClientMessage{_MSG; char String[96];}` (`Code/Basedef.h:1034-1039`) and call `SendClientMessage(conn, m->String)`. This is the only text branch that does not check `Mode` (compare BR-20 which requires `USER_PLAY`), so it can reach a player at the character-selection screen - which is exactly how DBSrv uses it, for example to announce "Sua conta agora é a primária." during the primary-account flow (`Code/DBSrv/CFileDB.cpp:2092-2100`).

`SendClientMessage` copies a fixed 96 bytes and forces terminators at 94/95, so a non-terminated 96-byte payload is safe here (the panel string is also 96 bytes). Delivery is deferred to the 500 ms flush, which is acceptable for informational text but would be wrong for anything latency-sensitive.

**Rule workflow**:
1. Receive `_MSG_DBClientMessage` for `conn`.
2. `SendClientMessage(conn, m->String)`.
3. Flush on the next timer tick.

---

#### BR-50: Parameterised signal relay (`_MSG_DBServerSend1`)

**Overview**: Forwards a one-parameter signal to the client with `ID = 30000`, queuing and flushing.

**Detailed description**:
Lines 1307-1315 rewrite `m->ID = ESCENE_FIELD` (30000), call `AddMessage(m, sizeof(MSG_STANDARDPARM))` and then `SendMessageA()` - the only branch that uses the manual queue-plus-flush pair instead of `SendOneMessage`. The producer is the failure arm of DBSrv's server-change handler (`Code/DBSrv/CFileDB.cpp:1956-1965`), where `Parm` carries the encryption/validation result code.

Semantically this is the "server change could not be prepared" notification, and it pairs with BR-51 which is the success path. Because `_MSG_DBServerSend1` is defined as `43 | FLAG_DB2GAME | FLAG_GAME2CLIENT` (`Code/Basedef.h:1059`), the numeric value is already client-legal; only the `ID` needs normalising.

**Rule workflow**:
1. Receive `_MSG_DBServerSend1` for `conn`.
2. Set `ID = 30000`.
3. `AddMessage(sizeof(MSG_STANDARDPARM))`.
4. `SendMessageA()` - immediate flush.
5. No state change.

---

#### BR-51: Server-change hand-off (`_MSG_DBCNFServerChange`)

**Overview**: Delivers the encrypted hand-off blob that lets a client migrate to another server, then closes the local session.

**Detailed description**:
Lines 1318-1324 forward `MSG_DBCNFServerChange{_MSG; char AccountName[16]; char Enc[52];}` (`Code/Basedef.h:1052-1058`) to the client and immediately call `CloseUser(conn)`. The `Enc` field is a formatted string - `"*<server> <enc0> ... <enc7> <slot>"` - built by DBSrv (`Code/DBSrv/CFileDB.cpp:1981-1984`) from freshly generated encryption material that is simultaneously stored as the account's `TempKey`. The client reconnects to the target server presenting that string, which the target server replays to DBSrv for validation.

The close is required: the client must not remain connected to the source server while migrating. It is also safe with respect to persistence because the source session is only closed after the character has already been logged out by the initiating client request, so `pUser[conn].Mode` is `USER_SELCHAR` at this point and `CloseUser` takes the non-save path (sending `_MSG_DBNoNeedSave`, which releases the account on the database side).

Note the direct interaction with BR-18: the *same* feature (server transfer) has two inbound packets with different roles - `_MSG_ReqTransper` arrives when a character is pushed *to* this server, `_MSG_DBCNFServerChange` when a client is pushed *away* from it. Only the latter is implemented correctly.

**Rule workflow**:
1. Receive `_MSG_DBCNFServerChange` for `conn`.
2. Forward `sizeof(MSG_DBCNFServerChange)` bytes to the client (queued).
3. `CloseUser(conn)` - socket closed; queue is discarded by `CloseSocket`.
4. (Client) reconnects to the target server with the `Enc` blob.

---

## 4. Component Structure

```
Code/
+-- Basedef.h                          # 2,505-line shared domain + protocol contract (all MSG_* structs,
|                                      # 191 message constants, FLAG_* namespaces, MAX_* limits)
+-- Basedef.cpp                        # BASE_* rule engine: BASE_CanCargo, BASE_GetHpMp, BASE_GetBonus*,
|                                      # BASE_GetItemCode, BASE_GetVillage, BASE_CheckPacket (disabled)
+-- CPSock.h                           # transport contract: HEADER (12 B), MAX_MESSAGE_SIZE 8192,
|                                      # INITCODE 0x1F11F311, 128 KB send/recv buffers
+-- CPSock.cpp                         # framing, obfuscation table, additive checksum, buffered send
+-- ItemEffect.h                       # EF_* effect dictionary (EF_DAMAGE, EF_DAMAGEADD, EF_CURKILL, ...)
`-- TMSrv/
    +-- ProcessDBMessage.h             # THIS COMPONENT - 42 lines: 11 CRT/Win32 includes, 5 project
    |                                  # includes, and one declaration (see note below)
    +-- ProcessDBMessage.cpp           # THIS COMPONENT - 1,330 lines, 1 function, 45 switch cases,
    |                                  # 36 #pragma region markers (one per message family)
    +-- Server.cpp                     # 9,449 lines: Win32 host, WSA_READDB entry point (line 3914),
    |                                  # world globals (pUser/pMob/pMobGrid/GuildInfo/ChargedGuildList/...),
    |                                  # CloseUser, SaveUser, CharLogOut, DoWar, DoAlly, ClearCrown, Log
    +-- Server.h                       # 110 extern declarations + function prototypes used by the dispatcher
    +-- SendFunc.cpp                   # outbound projection: SendClientMessage, SendNotice, SyncMulticast,
    |                                  # SendItem, SendCarry, SendScore, SendEtc, SendPKInfo, SendGridMob,
    |                                  # SendWarInfo, SendClientSignal(Parm)
    +-- GetFunc.cpp                    # derivation/queries: GetEmptyMobGrid, GetGuild, GetCreateMob,
    |                                  # GetAttribute, GridMulticast
    +-- CMob.cpp/.h                    # CMob entity: GetCurrentScore, Segment, MaxCarry, extra blob
    +-- CUser.cpp/.h                   # CUser session: Mode constants USER_EMPTY..USER_SAVING4QUIT
    +-- imple.cpp                      # ProcessImple: admin command interpreter invoked by BR-14
    +-- CReadFiles.cpp/.h              # loads Mac.txt into pMac[] (the MAC block list used by BR-21)
    +-- Language.h                     # message-string table indices (_NN_*, _SN_*, _SS_*, _DN_*)
    +-- ProcessSecMinTimer.cpp         # TIMER_SEC (500 ms) flush loop for all session sockets
    `-- ProcessClientMessage.cpp/.h    # sibling inbound dispatcher for client packets (for contrast)

Code/DBSrv/                            # protocol peer (read for producer analysis only)
+-- CFileDB.cpp                        # 27-case inbound handler; producers of 30+ reply types
+-- Server.cpp                         # DBSrv host; _MSG_DBSetIndex assignment, admin client handler
+-- CReadFiles.cpp                     # web/item and donate import -> _MSG_DBSendItem / _MSG_DBSendDonate
`-- CRanking.cpp                       # RankingSystem::sendUpdateIndividualRank -> _MSG_GrindRankingData
```

**Structural notes**

1. The component is *one function*. There is no class, no namespace, no static helper, no local data structure. Its 1,330 lines are organised only by `#pragma region` blocks named after the message they handle (36 regions for 45 cases, because the character-login branch has no region boundary and the account-login region covers the whole branch).
2. The header is not an interface contract in any meaningful sense: `ProcessDBMessage.h:42` declares `void ProcessDBMessage(int conn, char *pMsg);` - a two-parameter signature that **does not exist anywhere in the codebase**. The real definition is the one-parameter `void ProcessDBMessage(char *Msg)` at `ProcessDBMessage.cpp:39`, separately declared at `Code/TMSrv/Server.h:99`. Because C++ treats these as distinct overloads, both declarations compile; the two-parameter one is a dead declaration. Five other translation units include the header (`MobKilled.cpp:37`, `CWarTower.cpp:37`, `ProcessSecMinTimer.cpp:37`, `Server.cpp:37`, `imple.cpp:30`, `CCastleZakum.cpp:37`) purely to obtain its transitive includes, not because they call the function.
3. The file-level `#pragma region DB - TM` (`55`) / `DB - TM - CLIENT` (`187`) names encode the same split as BR-02: the first region is "database to game server", the second is "database to game server, destined for a client". That naming is the only documentation of the routing convention in the codebase.
4. Every handler follows one of five shapes: adapter (2-6 lines, BR-09/BR-10/BR-26/BR-27/BR-44/BR-50), relay-with-header-rewrite (BR-30/BR-31/BR-36/BR-51), signal-and-close (BR-22/BR-23/BR-25/BR-29/BR-37/BR-38), state-mutation (BR-08/BR-11/BR-12/BR-21/BR-47), or the compound reconstruction of BR-34.
5. There is no unit boundary inside the function: locals `i`, `x`, `n`, `k`, `tx`, `ty`, `ret`, `ItemCode[256]`, `cMsg[128]`, `tt[512]` are declared mid-branch and live until the end of the enclosing block, so several are shadowed or reused across cases (for example `tt` is used only in the character-login branch; `ItemCode` is declared three times inside `_MSG_DBSendItem`).

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (outbound, from this component)

```
ProcessDBMessage
+-- Server.cpp (world state and lifecycle) ......................... 16 call sites
|   +-- Log ..................................... 19   (diagnostics, all branches)
|   +-- SendClientMessage is in SendFunc (see below)
|   +-- CloseUser ............................... 15   (session teardown)
|   +-- SaveUser ................................  5   (persist: BR-19, BR-41, BR-43 via CloseUser, BR-47)
|   +-- CharLogOut ..............................  1   (BR-41)
|   +-- CrackLog ................................  2   (BR-34 dead conn==0 path, BR-35)
|   +-- RemoveParty .............................  1   (BR-17)
|   +-- DoRecall ................................  1   (BR-17)
|   +-- DoWar ...................................  1   (BR-09)
|   +-- DoAlly ..................................  1   (BR-10)
|   +-- ClearCrown ..............................  1   (BR-34 / SL-18)
|   +-- MountProcess ............................  1   (BR-34 / SL-18)
|   `-- IsFree ..................................  1   (BR-21)
+-- SendFunc.cpp (outbound projection) ............................ 34 call sites
|   +-- SendClientMessage ....................... 21
|   +-- SendMessageA / SendOneMessage / AddMessage (CPSock methods) .. 27
|   +-- SendClientSignal ........................  8
|   +-- SendClientSignalParm ....................  2
|   +-- SendNotice ..............................  2
|   +-- SendItem ................................  4
|   +-- SendCarry ...............................  2
|   +-- SendBilling .............................  3   (stub - returns TRUE, no I/O)
|   +-- SyncMulticast ...........................  1
|   +-- SendPKInfo / SendGridMob / SendWarInfo ...  3
|   `-- SendEtc / SendScore .....................  2
+-- GetFunc.cpp (derivation and world queries) ....................  5 call sites
|   +-- GetEmptyMobGrid .........................  1   (BR-34 spawn)
|   +-- GetCreateMob ............................  1   (BR-34 presence)
|   +-- GetGuild ................................  1   (BR-34 no-op wrapper, see note)
|   +-- GetAttribute ............................  1   (BR-17 safe-cell test)
|   `-- GridMulticast ...........................  1   (BR-34 presence)
+-- Basedef.cpp (rule engine) .....................................  7 call sites
|   +-- BASE_GetItemCode ........................  5   (logging of delivered items)
|   +-- BASE_CanCargo ...........................  1   (BR-19 placement)
|   +-- BASE_GetVillage .........................  1   (BR-17)
|   +-- BASE_GetHpMp ............................  1   (SL-12)
|   +-- BASE_GetBonusSkillPoint .................  1   (SL-12)
|   +-- BASE_GetBonusScorePoint .................  1   (SL-12)
|   `-- BASE_GetGuildName .......................  1   (SL-18 Kefra text)
+-- CMob.cpp ......................................................
|   `-- pMob[conn].GetCurrentScore ..............  2   (SL-12, SL-17)
+-- imple.cpp .....................................................
|   `-- ProcessImple ............................  1   (BR-14)
`-- CPSock (DBServerSocket) .......................................
    `-- SendOneMessage ..........................  6   (write-back to DBSrv: lines 197, 278, 310, 327, 1155, 1246)
```

`GetGuild(conn)` is worth a note: its entire body is `STRUCT_ITEM *item = &pMob[conn].MOB.Equip[12]; int guild = pMob[conn].MOB.Guild;` with no effect (`Code/TMSrv/GetFunc.cpp:1862-1867`). The call at line 777 is therefore a no-op with a side-effect-free signature - a dead dependency that still creates a link edge.

### 5.2 Global state consumed (in-process coupling)

| Global | Declared | Size / type | Used for |
|--------|----------|-------------|----------|
| `pUser[MAX_USER]` | `Code/TMSrv/Server.cpp:269` | 1,000 `CUser` | every player-scoped branch; ~40 distinct field reads/writes |
| `pMob[MAX_MOB]` | `Code/TMSrv/Server.cpp:270` | mob/player entity | login reconstruction, spawn, presence |
| `pMobGrid[MAX_GRIDY][MAX_GRIDX]` | `Code/TMSrv/Server.h:54` | 4,096 x 4,096 `unsigned short` | writes the occupant at login (line 997) |
| `DBServerSocket` | `Code/TMSrv/Server.cpp:262` | `CPSock` | 6 write-backs to DBSrv |
| `pMac[MAX_MAC]` | `Code/TMSrv/Server.cpp:271` | 200 `STRUCT_BLOCKMAC` | MAC block enforcement (BR-21) |
| `GuildInfo[65536]` | `Code/TMSrv/Server.cpp:361` | 512 KB | guild record store/replace (BR-11) |
| `ChargedGuildList[10][5]` | `Code/Basedef.cpp:738` | 50 ints | crown-guild matrix (BR-12) |
| `g_pItemList[MAX_ITEMLIST]` | `Code/Basedef.cpp:753` | 6,500 records | item names and `nPos` class for normalisation |
| `g_pMessageStringTable[]` | `Code/TMSrv/...` via `Language.h` | string table | all user-visible text |
| `g_pGuildZone[MAX_GUILDZONE]` | `Code/TMSrv/...` | 5 zones | spawn coordinates, castle clan |
| `g_pNextLevel[]`, `g_pNextLevel_2[]` | `Code/TMSrv/...` | level curves | EXP segment computation |
| `temp[4096]` | `Code/TMSrv/Server.cpp:277` | shared log buffer | every log and formatted message |
| `TransperCharacter`, `ServerGroup`, `ServerIndex`, `Sapphire` | `Server.cpp:382`, `:54` etc. | ints | BR-07, BR-08, BR-18 |
| `evDelete`, `BILLING`, `CHARSELBILL`, `NewbieEventServer`, `KefraLive`, `KefraKiller`, `RvRBonus`, `CastleState`, `CurrentWeather`, `FREEEXP`, `SecCounter` | `Server.h` externs | ints | branch behaviour (12 distinct flags) |

The dependency on the single global `temp` buffer is a hidden coupling: any function the dispatcher calls that also uses `temp` (essentially all of them, including `Log` and `CrackLog`) overwrites the caller's formatted string. Several branches format into `temp` and then pass `temp` to a callee that logs, so the text printed is sometimes the callee's, not the caller's. This is observable in `_MSG_DBAccountLoginFail_Block` where `temp` is formatted then passed to `SendClientMessage` (no interleaving call, safe) versus `_MSG_DBCNFCharacterLogin` where `tt` is used deliberately to avoid the collision (lines 977, 1059, 1078).

### 5.3 External dependencies (protocol peers and artefacts)

| Dependency | Direction | Transport / format | Contract |
|------------|-----------|--------------------|----------|
| DBSrv (`Code/DBSrv`, TCP 7514) | bidirectional | custom binary, obfuscated, additive checksum | 45 inbound types handled; 6 outbound types emitted |
| Game client (build 7662) | outbound only | same framing, TCP 8281 | 11 distinct client packet types emitted from this component |
| `../../DBSRV/Run/Mac.txt` | inbound data file | text lines `<idx> <a>.<b>.<c>.<d>` | loaded once at boot by `CReadFiles::ReadMacblock`; consumed by BR-17/BR-21 |
| `./Log/TM_<date>.txt` | outbound | text | `Log` target (`Code/TMSrv/Server.cpp:6654-6669`) |
| Win32 API (`SetWindowText`, `closesocket`, `WSAAsyncSelect`) | outbound | OS | log mirroring and socket lifecycle |
| CRT (`sprintf`, `strncpy`, `memcpy`, `rand`) | outbound | - | formatting and spawn jitter |
| Windows macro aliasing | compile-time | `#define SendMessage SendMessageA` | `pUser[conn].cSock.SendMessage()` at line 1165 compiles only because `winuser.h` aliases the unattributed `SendMessage` token to `SendMessageA`; the intent and the effect coincide, but the expression depends on a Win32 macro, not on a `CPSock` member |

### 5.4 Dependency direction summary

```
        Client (7662)
             ^  |
   client pkts|  | 11 relay/signal types
             |  v
     +---------------------+       45 inbound types        +-------------+
     | ProcessDBMessage    | <---------------------------- |   DBSrv     |
     | (this component)    |                               |  (7514)     |
     +---------------------+ ----- 6 write-backs --------> +-------------+
        |        |        |
        v        v        v
   Server.cpp  SendFunc  GetFunc/Basedef/CMob/imple
   (world)     (egress)  (rules)
```

The component has **no inbound function dependency at all**: nothing in the codebase calls into it for a service, and nothing can be injected into it. It is a leaf consumer that reads globals and calls helpers.

---

## 6. Afferent and Efferent Coupling

### 6.1 Measurement method

Because this is C-style procedural code with one class per entity, coupling is reported at two levels: (a) the component itself, at function granularity; (b) the units it touches, at module granularity. Counts were obtained by exhaustive enumeration of the component's call sites and by grepping every call site of `ProcessDBMessage` in the repository. The architectural report estimated "Afferent ~9, Efferent ~14"; the measured figures below reconcile with it at module granularity (9 modules including the protocol peer, 8 in-process) and are higher at symbol granularity.

### 6.2 Component-level coupling

| Component | Afferent (Ca) | Efferent (Ce) | Instability Ce/(Ca+Ce) | Criticality |
|-----------|---------------|---------------|------------------------|-------------|
| `ProcessDBMessage` (function) | 1 caller (`Code/TMSrv/Server.cpp:3914`) + 6 header includers (none of which call it) + 1 external process peer (DBSrv, 4 producer files) | 39 distinct project functions + 27 global objects + 3 `CPSock` methods, spread over 8 in-process modules | 0.98 (in-process), 0.98 (with peer = 2 afferents) | High |

The instability figure is a precise statement of the component's architectural position: it is almost pure fan-out. Only one line of the codebase depends on it, while it depends on nearly every other TMSrv module. Any change to `Server.cpp` helpers, `SendFunc` signatures or `Basedef` rule functions can break it, and no other code is affected by changes inside it - except the wire protocol, which is not visible to the compiler.

### 6.3 Unit-level coupling of the units this component touches

The "Afferent" column counts distinct *callers of that unit anywhere in the codebase* (measured by grep over `Code/TMSrv` and `Code/DBSrv`); the "From this component" column is the subset used here. "Critical" reflects the blast radius if the unit misbehaves for this component's call.

| Unit | Kind | Afferent (project-wide) | Efferent from this component | Critical |
|------|------|-------------------------|------------------------------|----------|
| `CloseUser` | function | ~20 call sites | 15 | High - wrong close leaks or double-saves a session |
| `SendClientMessage` | function | ~150 call sites | 21 | Medium - fixed 96-byte copy, no flush |
| `SaveUser` | function | ~6 call sites | 5 | High - full record write, `Export` flag selects backup |
| `Log` | function | ~400 call sites | 19 | Low - but uses a shared 4 KB buffer and a non-literal `fprintf` format |
| `SendClientSignal` | function | ~120 call sites | 8 | Low - 12-byte queued signal |
| `ClearCrown` | function | 1 (this component) | 1 | Medium - mutates equipment on every login |
| `DoWar` / `DoAlly` | functions | 2 each (client handlers + this component) | 2 | Medium - global war/alliance tables |
| `ProcessImple` | function | 2 (whisper `/gm` + this component) | 1 | High - arbitrary admin commands from the DB channel |
| `BASE_CanCargo` | function | 5 (DBSrv import, TMSrv item handlers) | 1 | Medium - identical algorithm duplicated on both peers |
| `GetEmptyMobGrid` | function | ~8 | 1 | High - login fails hard when it returns 0 |
| `GetCurrentScore` | method | ~30 | 2 | High - derives all combat statistics |
| `BASE_GetBonusScorePoint` | function | 2 | 1 | High - can subtract stored stat points |
| `CPSock::SendOneMessage` | method | ~200 | 22 | Medium - silent failure on full buffer/socket 0 |
| `GuildInfo[]` | global | 12 files | 2 (write at 103) | High - 512 KB table, unvalidated index |
| `pUser[]` | global | 30+ files | 34 branches | High - the session table itself |
| `temp[]` | global | 30+ files | 19 | Medium - shared buffer aliasing |

### 6.4 Cohesion assessment

| Dimension | Assessment | Evidence |
|-----------|------------|----------|
| Functional cohesion | High at the level of intent (one job: translate DB messages into world actions), low at the level of implementation (18 unrelated responsibilities inside one function) | 45 cases, 36 region markers, 447-line login branch |
| Temporal cohesion | Dominant in BR-34, where the order of 30 statements is semantically load-bearing (packet before broadcast, grid before multicast, HP floor before `GetCurrentScore`) | `ProcessDBMessage.cpp:730-1013` |
| Logical cohesion | Present in the failure-signal families (BR-22/23/24/25/29), which differ only in message id and close behaviour | `ProcessDBMessage.cpp:475-576` |
| Data cohesion | Strong around `pUser[conn]`/`pMob[conn]`; weak around `GuildInfo`, `ChargedGuildList` and `pMac`, which are shared with non-DB code | section 5.2 |
| Coupling to protocol contract | Total: every branch is bound to `sizeof(...)` of a `Basedef.h` struct and to a numeric constant | `Code/Basedef.h:943-2325` |

---

## 7. Endpoints (Inbound Message-Type Surface)

The component exposes no network listener of its own; its "endpoints" are the 45 message types it accepts on the single DBSrv connection. They are listed here as the component's addressing surface, because that is what a caller (DBSrv) must satisfy. All are delivered over one TCP connection (TMSrv -> DBSrv:7514, accepted by `DBServerSocket.ConnectServer` at `Code/TMSrv/Server.cpp:3562` / `3827`).

### 7.1 Server-scoped endpoints (`ID == 0`)

| Message type | Numeric | Payload | Effect | Producer |
|--------------|---------|---------|--------|----------|
| `_MSG_TransperCharacter` | 0x07A9 | `MSG_STANDARDPARM2` (parms ignored) | Enable transfer mode | `DBSrv/Server.cpp:1174` |
| `_MSG_DBSetIndex` | 0x0423 | `MSG_STANDARDPARM3` | Assign `ServerGroup`/`ServerIndex`/`Sapphire` | `DBSrv/Server.cpp:1159`, `DBSrv/CFileDB.cpp:503` |
| `_MSG_War` | 0x0E0E | `MSG_STANDARDPARM2` | Guild war transition | `DBSrv/CFileDB.cpp:388` |
| `_MSG_GuildAlly` | 0x0E12 | `MSG_STANDARDPARM2` | Guild alliance transition | `DBSrv/CFileDB.cpp:420` |
| `_MSG_GuildInfo` | 0x0E13 | `MSG_GuildInfo` | Replace `GuildInfo[Guild]` | `DBSrv/CFileDB.cpp:2185-2199` (bulk at connect) |
| `_MSG_GuildReport` | 0x0427 | `MSG_GuildReport` | Replace `ChargedGuildList` | `DBSrv/CFileDB.cpp:371` |
| `_MSG_NPNotice` (Parm1=1) | 0x3409 | `MSG_NPNotice` | World broadcast | `DBSrv/Server.cpp:268` |
| `_MSG_MessageDBImple` | 0x0C0B | `MSG_MessageDBImple` | Execute admin command | `DBSrv/CFileDB.cpp:592-609` |
| `_MSG_MagicTrumpet` | 0x0D1D | `MSG_MagicTrumpet` | Relay to all playing users | `DBSrv/CFileDB.cpp:1591-1610` |
| `_MSG_DBNotice` | 0x0D1E | `MSG_DBNotice` | World broadcast | `TMSrv` loop-back via `DBSrv/CFileDB.cpp:1612` |
| `_MSG_DBCheckPrimaryAccount` | 0x0414 | `MSG_DBCheckPrimaryAccount` | Downgrade other accounts on the same MAC | `DBSrv/CFileDB.cpp:788`, `2074-2090` |

### 7.2 Player-scoped endpoints (`ID` in `[1, 1000)`)

| Message type | Numeric | Payload | Session requirement | Producer |
|--------------|---------|---------|---------------------|----------|
| `_MSG_ReqTransper` | 0x0FAA | `MSG_ReqTransper` | `TransperCharacter != 0` | `DBSrv/Server.cpp:656` |
| `_MSG_DBSendItem` | 0x0C0F | `MSG_DBSendItem` | `Mode >= USER_SELCHAR` | `DBSrv/CReadFiles.cpp:245` |
| `_MSG_NPNotice` (Parm1=0) | 0x3409 | `MSG_NPNotice` | `Mode == USER_PLAY` | `DBSrv/Server.cpp:272` |
| `_MSG_DBCNFAccountLogin` | 0x0416 | `MSG_DBCNFAccountLogin` | `Mode == USER_LOGIN` | `DBSrv/CFileDB.cpp:559`, `780` |
| `_MSG_DBNewAccountFail` | 0x041A | `MSG_STANDARD` | `Mode == USER_ACCEPT/LOGIN` | `DBSrv/CFileDB.cpp:539` |
| `_MSG_DBAccountLoginFail_Account` | 0x0421 | `MSG_STANDARD` | any | `DBSrv/CFileDB.cpp:622` |
| `_MSG_DBAccountLoginFail_Block` | 0x0424 | `MSG_STANDARDPARM` | any | DBSrv login path |
| `_MSG_DBAccountLoginFail_Disable` | 0x0425 | `MSG_STANDARD` | any | DBSrv login path |
| `_MSG_AccountSecure` | 0x0FDE | `MSG_STANDARD` | any | `DBSrv/CFileDB.cpp:1387`, `1395`, `1414` |
| `_MSG_AccountSecureFail` | 0x0FDF | `MSG_STANDARD` | any | `DBSrv/CFileDB.cpp:1420` |
| `_MSG_DBOnlyOncePerDay` | 0x0426 | `MSG_STANDARD` | any | none (dead) |
| `_MSG_DBAccountLoginFail_Pass` | 0x0422 | `MSG_STANDARD` | any | DBSrv login path |
| `_MSG_DBCNFNewCharacter` | 0x0418 | `MSG_CNFNewCharacter` | `USER_CHARWAIT` | DBSrv create path |
| `_MSG_DBCNFDeleteCharacter` | 0x0419 | `MSG_CNFDeleteCharacter` | `USER_CHARWAIT` | DBSrv delete + capsule paths |
| `_MSG_DBDeleteCharacterFail` | 0x041E | `MSG_STANDARD` | any | `DBSrv/CFileDB.cpp` (4 sites) |
| `_MSG_DBNewCharacterFail` | 0x041D | `MSG_STANDARD` | any | `DBSrv/CFileDB.cpp` (19 sites) |
| `_MSG_DBCNFCharacterLogin` | 0x0417 | `MSG_CNFCharacterLogin` | `USER_CHARWAIT` | `DBSrv/CFileDB.cpp:818`, `1056` |
| `_MSG_DBCharacterLoginFail` | 0x041C | `MSG_STANDARD` | any | none (dead) |
| `_MSG_DBMessageBoxOk` | 0x0402 | `MSG_MessageBoxOk` | any | none (dead) |
| `_MSG_DBAlreadyPlaying` | 0x041F | `MSG_STANDARD` | any | `DBSrv/CFileDB.cpp:694` |
| `_MSG_DBStillPlaying` | 0x0420 | `MSG_STANDARD` | any | `DBSrv/CFileDB.cpp:699` |
| `_MSG_DBSavingQuit` | 0x040A | `MSG_DBSavingQuit` | any | `DBSrv/CFileDB.cpp:2377` |
| `_MSG_DBCNFAccountLogOut` | 0x040B | `MSG_STANDARD` | `USER_SAVING4QUIT` | `DBSrv/CFileDB.cpp:1259` |
| `_MSG_DBCNFArchCharacterSucess` | 0x040E | `MSG_STANDARDPARM` | `USER_PLAY` | `DBSrv/CFileDB.cpp:1586` |
| `_MSG_DBCNFArchCharacterFail` | 0x040F | `MSG_STANDARD` | `MOB_WAITDB` | `DBSrv/CFileDB.cpp:1442` |
| `_MSG_DBSendDonate` | 0x0C11 | `MSG_DBSendDonate` | `Mode >= USER_SELCHAR` | `DBSrv/CReadFiles.cpp:821` |
| `_MSG_CNFDBCapsuleInfo` | 0x0D1F | `MSG_CNFDBCapsuleInfo` | `USER_PLAY` | `DBSrv/CFileDB.cpp:1657` |
| `_MSG_DBCNFCapsuleCharacterFail` | 0x0410 | `MSG_STANDARD` | `USER_PLAY` | `DBSrv/CFileDB.cpp:1792` |
| `_MSG_DBCNFCapsuleCharacterFail2` | 0x0411 | `MSG_STANDARD` | `USER_PLAY` | `DBSrv/CFileDB.cpp:1801` |
| `_MSG_DBCNFCapsuleSucess` | 0x0412 | `MSG_STANDARD` | `USER_PLAY` | `DBSrv/CFileDB.cpp:1749` |
| `_MSG_GrindRankingData` | 0x4501 | `MSG_SendExpRanking` | `Mode == USER_PLAY` + name match | `DBSrv/CRanking.cpp:313` |
| `_MSG_DBClientMessage` | 0x0413 | `MSG_DBClientMessage` | any | `DBSrv/CFileDB.cpp:2094` |
| `_MSG_DBServerSend1` | 0x052B | `MSG_STANDARDPARM` | any | `DBSrv/CFileDB.cpp:1959` |
| `_MSG_DBCNFServerChange` | 0x052A | `MSG_DBCNFServerChange` | `USER_SELCHAR` | `DBSrv/CFileDB.cpp:1975` |

*Numeric values are the raw composite constants from `Code/Basedef.h` (flags included), which is why several exceed 0x0FFF.*

### 7.3 Endpoint statistics

* 45 handled types out of 191 defined constants (23.5 % of the protocol surface).
* 3 types have no producer anywhere in the repository (`_MSG_DBOnlyOncePerDay`, `_MSG_DBCharacterLoginFail`, `_MSG_DBMessageBoxOk`) and are unreachable in the current build.
* 8 types are pure relays (BR-26, BR-27, BR-30, BR-31, BR-36, BR-44, BR-49, BR-50).
* 3 types cause a session close without any state reconstruction (BR-22, BR-23, BR-24, BR-25, BR-29, BR-37, BR-38, BR-51 - 8 in total).

---

## 8. Integration Points

| Integration | Type | Direction | Purpose | Protocol | Data format | Error handling |
|-------------|------|-----------|---------|----------|-------------|----------------|
| DBSrv inbound stream | External service (peer process) | DBSrv -> TMSrv | All 45 handled message types | TCP 7514, custom framing, 512-byte obfuscation table, 8-bit additive checksum, `INITCODE` handshake | Binary, native-endian x86, compiler-default struct packing (`#pragma pack(1)` only for `MSG_AccountLogin`) | Envelope check (BR-01), dead-session guard (BR-03), per-branch `break`; no payload validation; log-and-drop only |
| DBSrv outbound stream | External service (peer process) | TMSrv -> DBSrv | `_MSG_DBNoNeedSave` (2 sites), `_MSG_DBSendItem` ack (3 sites), `_MSG_DBSendDonate` ack (1 site) | same connection | Binary | `AddMessage` returns FALSE on full buffer/closed socket; the return value is ignored at all 6 sites |
| Game client | External service (client) | TMSrv -> client | 11 packet types emitted here: `_MSG_CNFAccountLogin`, `_MSG_CNFNewCharacter`, `_MSG_CNFDeleteCharacter`, `_MSG_CNFCharacterLogin`, `_MSG_UpdateCarry`, `_MSG_SendItem`, `_MSG_MessagePanel`, `_MSG_MessageBoxOk`, `_MSG_SendExpRanking`, `_MSG_SendArchEffect`, `_MSG_DBCNFServerChange` | TCP 8281, same framing | Binary | Send helpers guard `conn` range and `Mode == USER_PLAY` where appropriate; failures are silent or close the session (`SendCarry` at `SendFunc.cpp:1557-1558`) |
| `pMobGrid` world grid | Internal data structure | write | Register the login occupant before broadcasting | in-memory `unsigned short[4096][4096]` | - | No bound check around the write at line 997 (`sm.PosX/PosY` already validated by `GetEmptyMobGrid`) |
| `Mac.txt` block list | Configuration file | read (indirect) | Populate `pMac[200]` used by MAC block | text, `<idx> <a>.<b>.<c>.<d>` | text | Read once at boot by `CReadFiles::ReadMacblock`; a missing file silently leaves every entry zero (no blocks) |
| System log file | Artefact | write | `err,packet`, `tra,...`, `sta,Login`, `etc,charlogout`, `not ...` | `./Log/TM_dd_mm_yyyy_hh_mm_ss.txt` | text | `fprintf(fLogFile, LogTemp)` uses a non-literal format string (`Code/TMSrv/Server.cpp:6723`); format specifiers inside logged text are interpreted |
| Billing server | External service (optional) | - | Cash/billing verification | - | - | `SendBilling` is a stub returning TRUE (`Code/TMSrv/Server.cpp:1377-1380`); the integration is inert in this build |
| Ranking system | Internal service across processes | both directions | `_MSG_UpdateExpRanking` out, `_MSG_GrindRankingData` in | via DBSrv | packed `STRUCT_RANKING` (`#pragma pack(1)`, `Basedef.h:808-835`) | Identity check before forwarding (BR-48); dropped silently otherwise |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Message Router / Command Dispatch | `switch (std->Type)` over 45 constants, twice (by scope) | `ProcessDBMessage.cpp:58`, `210` | Type-tagged flat dispatch with no polymorphism |
| Discriminated union over one numeric space | Flags (`FLAG_DB2GAME`, `FLAG_GAME2CLIENT`, ...) act as namespace bits inside a 16-bit type field; the same numeric value is legal in several directions | `Code/Basedef.h:932-941` | Lets one wire type serve DB->game, game->DB and game->client, at the cost of flag-equality being replaced by bit tests |
| Adapter (two-way) | The component adapts the DBSrv payload shape into both internal state and client payload shape (e.g. `MSG_DBCNFAccountLogin` -> `MSG_CNFAccountLogin` with `ID` remap; `MSG_DBCNFCharacterLogin` -> `MSG_CNFClientCharacterLogin` with truncation) | `354-472`, `635-1083` | Decouples persistence layout from wire layout |
| Routing key as identity | `ID` field doubles as session slot and scope selector | `54-56` | Avoids an extra routing header; forces `ID == 0` convention |
| Event-sourcing-lite (step-by-step read model rebuild) | Character login rebuilds the live entity from persisted primitives and re-derives every computed attribute | `733-913` | Guarantees derived state is never trusted from disk |
| Data migration at login ("normalise-on-read") | `EF_DAMAGEADD`/`EF_DAMAGE2` -> `EF_DAMAGE`; event item purge | `402-431`, `654-705` | Heals legacy item encodings without a migration tool |
| Transactional envelope (manual) | Save-and-quit is a two-phase exchange: `MSG_SavingQuit` out, `_MSG_DBCNFAccountLogOut` in; the session state machine holds `USER_SAVING4QUIT` between phases | `1137-1183`, `Server.cpp:6869-6919` | Provides durable logout without a transaction manager |
| Two-phase membership protocol | War/alliance proposals are recorded unilaterally and confirmed symmetrically | BR-09, BR-10 | Distinguishes "declared" from "started" |
| Optimistic relay | Packets forwarded to the client without acknowledgement, with the server assuming the client will resynchronise | BR-30, BR-31, BR-44, BR-50 | Minimises round trips; requires client robustness |
| Defensive flag reset ("privilege does not survive reconnect") | Nine chat/privilege flags zeroed at account login | `386-395` | Prevents privilege leakage across sessions |
| Pseudo-identity guard | The account name echoed by the database must match the name this socket submitted | `358` | Weakly authenticates the DB reply to the request |
| Layered architecture (partially enforced, per the architectural report) | `MainWndProc` -> `ProcessDBMessage` -> helpers -> `Basedef` rules -> `CPSock` | whole flow | Conventional only; the dispatcher reaches directly into `pUser[]`/`pMob[]` and bypasses every layer boundary |
| Anti-pattern: duplicated business rule | Cargo placement algorithm implemented twice (TMSrv `229-338` and DBSrv `CReadFiles.cpp:286-308`), with matching but independently written bounds | BR-19 | The two implementations must agree or items land in different slots |
| Anti-pattern: dead code retained for shape | Unreachable `conn == 0` test inside the player branch (`643`), unreachable range guard (`202`), dead normalisation block (`654-705`), unused locals (`ID`, `Server`, `tMac`, `unk`, `unk2`), vestigial `m->ID` writes before `SendClientSignal` | throughout | Decompilation artefacts that inflate complexity and mislead maintainers |
| Anti-pattern: shared mutable scratch buffer | Process-global `temp[4096]` used for formatting in both caller and callee | `Server.cpp:277`, all branches | Creates implicit ordering dependencies between statements that look independent |

---

## 10. Technical Debt & Risks

### 10.1 Risk register

| ID | Risk level | Component area | Issue | Impact |
|----|------------|----------------|-------|--------|
| R-01 | High | BR-18 `_MSG_ReqTransper` (line 222) | `SendOneMessage((char*)&m, sizeof(MSG_ReqTransper))` sends 52 bytes starting at the address of the local pointer, not the message | Stack-content disclosure to the client (48 bytes per invocation) and a non-functional character-transfer notification |
| R-02 | High | BR-21 MAC block (lines 369-383) | `CloseUser(conn)` is followed by `break` instead of `return`; the handler then sets `Mode = USER_SELCHAR`, loads cargo/SelChar and attempts to send on a closed socket | Blocked account ends in a zombie `USER_SELCHAR` slot with `Sock == 0` and no reclaim path (see R-03 mechanics), i.e. one leaked slot per blocked login attempt; a blocked machine can exhaust the 990 usable slots |
| R-03 | High | BR-40 `_MSG_DBCNFAccountLogOut` (lines 1179-1182) | `Mode` is set to `USER_ACCEPT` immediately before `CloseUser`, and `CloseUser` deliberately does nothing for that mode; `CloseSocket` zeroes `nSendPosition`, so the 500 ms reaper never re-enters | Every completed save-and-quit leaves one permanent zombie slot (`USER_ACCEPT`, `Sock == 0`); the server loses one of 990 slots per logout until restart |
| R-04 | High | BR-34 SL-02/SL-03/SL-04 (lines 654-705 vs 784) | The effect normalisation and `evDelete` purge are applied to `sm.mob`, then `sm.mob = pMob[conn].MOB` overwrites the whole struct from the un-normalised `m->mob` | Legacy `EF_DAMAGEADD`/`EF_DAMAGE2` effects survive on the character-login path (client and server disagree with the account-login path); the event item purge silently does nothing for carry items at login |
| R-05 | High | BR-34 SL-18 (lines 1004-1005) | `if (pMob[n].MOB.CurrentScore.Level >= 999) pUser[n].Admin = 1;` uses loop variable `n` (value 5 or a break index) instead of `conn` | Admin flag is applied to the wrong entity; a level-999+ player never becomes admin, and an unrelated mob index may be probed |
| R-06 | High | BR-14 `_MSG_MessageDBImple` + `ProcessImple(0, ...)` | The dispatcher hardcodes `conn = 0`; `ProcessImple`'s own guard is the unsatisfiable `conn <= 0 && conn >= MAX_USER`, so slot 0 is not rejected | Every per-player admin command from the database silently mutates the reserved slot 0 and discards its feedback; remote administration of individual players is non-functional through this channel |
| R-07 | High | BR-19 / BR-43 refusal paths | On `Mode < USER_SELCHAR` or account mismatch the handler `break`s without setting `Result` and without replying | DBSrv cannot detect delivery failure; the operator file has already been moved to "processed", so the item/donate is lost with only a TMSrv log line |
| R-08 | Medium | BR-19 (lines 272, 304) | `g_pItemList[m->Item.sIndex].Name` is dereferenced without validating `sIndex` against `MAX_ITEMLIST` (6,500); `sIndex` is a signed 16-bit field | Out-of-bounds read of up to ~2.7 MB past a 650 MB array on a corrupt or hostile packet; the read value is printed into logs and the client text |
| R-09 | Medium | BR-11 (line 103) | `GuildInfo[m->Guild]` with an unvalidated 32-bit index into a 65,536-entry array | 8-byte out-of-bounds write in the data segment if DBSrv ever emits a guild id above 65,535 |
| R-10 | Medium | BR-24 (lines 507-519), BR-39 (lines 1160-1163) | When the parameter is outside the handled set, `temp` is not assigned and its stale contents (a previous log line) are sent to the client | Cross-session information leak of arbitrary log text to the player |
| R-11 | Medium | BR-22, BR-23, BR-25, BR-29 | Failure text is queued with `SendClientSignal`/`SendClientMessage` and then discarded by `CloseUser` -> `CloseSocket` (which zeroes send positions) | Four of the six login-failure messages are never delivered; players see a bare disconnect with no reason |
| R-12 | Medium | BR-39 (lines 1148-1166) | The two guards are not mutually exclusive: `USER_SELCHAR` satisfies both, so TMSrv sends `_MSG_DBNoNeedSave` **and** a client message | Redundant database traffic and a contradictory instruction to DBSrv for character-selection sessions |
| R-13 | Medium | BR-06, whole component | No size validation; `BASE_CheckPacket` is a commented-out stub returning an undefined value, reachable only under `_PACKET_DEBUG` | Any protocol drift or corrupt packet is interpreted as a larger structure, reading adjacent ring-buffer bytes |
| R-14 | Medium | BR-34 (line 641) | `MSG_CNFCharacterLogin` is truncated into `MSG_CNFClientCharacterLogin` by `memcpy(sizeof(smaller))`; the layout equivalence is implicit (identical field order and types up to `Unk2`) | Any future field insertion in either struct silently shifts every subsequent field of the client packet |
| R-15 | Medium | BR-21 (lines 397-434) | The database structure is forwarded to the client including `HashKeyTable[16]`, `Unknow_28` (`0xCCCCCCCC` marker) and `Keys[12]` | Internal database fields are exposed on the client wire; the marker value also proves the field is uninitialised data shipped to clients |
| R-16 | Medium | BR-19 success path | `SaveUser` is not called for an in-game player, and `SendItem` requires `USER_PLAY`; a delivery at the character-selection screen is persisted but invisible | Item exists in memory and on disk before the client knows about it; a crash between placement and the next periodic save loses a purchase that DBSrv already committed |
| R-17 | Medium | BR-34 SL-16 (lines 848-885) | Spawn precedence uses `MOB.Merchant & 0xC0` as a 2-bit city selector and `rand()` with no seeding shown in this file | Deterministic spawn jitter if `srand` is not called at boot; city selection depends on a field that is also cleared on `BaseScore` two lines earlier (line 765 clears `BaseScore.Merchant`, not `MOB.Merchant` - a subtle distinction that works only because two different fields exist) |
| R-18 | Low | BR-19 (lines 293-323) | Phase-2 fallback ignores item footprint and can place a 2x4 item on a slot that overlaps an existing item's grid cells | Grid inconsistency between client rendering and server placement; duplicates the same flaw in DBSrv's importer |
| R-19 | Low | Header contract | `ProcessDBMessage.h:42` declares `void ProcessDBMessage(int conn, char *pMsg);`, which is never defined; the real signature is in `Server.h:99` and the `.cpp` | Two irreconcilable declarations; any new caller using the header's signature links only by accident (overload) or fails at link time |
| R-20 | Low | Logging (`Log`, lines 6701-6727) | `fprintf(fLogFile, LogTemp)` uses a non-literal format string, and `sprintf(LogTemp, "%s %s %s \n", LogTemp, str2, str1)` copies a buffer onto itself | Undefined behaviour on overlapping `sprintf`; a `%n`/`%s` sequence inside a player-supplied string processed by `Log` is interpreted as a format directive |
| R-21 | Low | Whole component | 27 global objects and 39 free functions are referenced directly; no seam for injection | Unit testing requires linking the entire TMSrv executable; see section 11 |
| R-22 | Low | BR-34 SL-17 (line 918) | `int Server = pMob[conn].MOB.Guild / MAX_GUILD;` is computed and never used | Dead computation; the intended cross-server guild validation was never implemented |
| R-23 | Low | BR-21 (line 395) + `_MSG_AccountLogin` | `OnlyTrade` defaults to 1 at account login and is cleared only by `_MSG_DBCheckPrimaryAccount`; if that packet is lost the player is permanently trade-restricted until re-login | Silent gameplay restriction with no player-facing diagnostics |
| R-24 | Low | `_MSG_DBSaveMob`-driven save cadence | `SaveUser` writes the whole record (`STRUCT_MOB` + 128 cargo + affects + extra, ~3.5 KB) on every call; the periodic saver does one user per second (`ProcessSecMinTimer.cpp:189`) | With 990 concurrent players a full save sweep takes ~16 minutes; a crash can lose up to that much progress per session, and the component's own saves add to the same channel |

### 10.2 Security assessment (detailed)

| Aspect | Observation | Evidence |
|--------|-------------|----------|
| Authentication of the peer | None. The DBSrv connection is accepted on a fixed address/port from `localip.txt`/`serverlist.txt`; no shared secret, no nonce, no mutual authentication | `Code/TMSrv/Server.cpp:3562`, `3827` |
| Message integrity | 8-bit additive checksum over the obfuscated stream, computed and compared on receipt; mismatch returns the packet anyway with `ErrorCode = 1`, and the caller then breaks the read loop | `Code/CPSock.cpp:424-466`, `Server.cpp:3907-3912` |
| Confidentiality | XOR-like masking with a hard-coded 256-entry table | `Code/CPSock.cpp:556-590` |
| Payload validation | Absent (R-13) | section 5.1 note, `Code/Basedef.cpp:6475` |
| Identity binding of replies | Account name echo check only on `_MSG_DBCNFAccountLogin` (line 358); no check on any other branch, and none on `_MSG_DBCNFCharacterLogin` | `ProcessDBMessage.cpp:358` |
| Authorisation decisions taken here | MAC block (BR-21), single-primary-account downgrade (BR-17), ranking self-identity (BR-48), admin level from packet (BR-14) | sections 3.2 |
| Authorisation decisions *imported* from the payload | `MSG_MessageDBImple.Level` determines which admin commands run | `imple.cpp:89`, `569`, `647`, `1119`, `1164` |
| Information disclosure | Stack bytes to client (R-01); stale `temp` to client (R-10); DB hash/key fields to client (R-15); item names logged from unvalidated indices (R-08) | as listed |
| Privilege persistence | `Admin` explicitly zeroed at account login; re-derived from level at character login (but incorrectly, R-05) | `ProcessDBMessage.cpp:391`, `1005` |
| Denial of service | Permanent slot leak per logout (R-03) and per blocked login (R-02); unbounded log write per malformed packet (BR-01) | as listed |
| Credential handling | No passwords traverse this component; the only secrets in flight are `MSG_DBCNFServerChange.Enc` (hand-off blob) which is forwarded unchanged | `ProcessDBMessage.cpp:1318-1324` |

### 10.3 Code-quality metrics

| Metric | Value | Note |
|--------|-------|------|
| File size | 1,330 lines, 39,988 bytes | single function |
| Function length | 1,291 lines (`39`..`1330`) | ~29x the commonly cited 40-line guideline |
| Switch cases | 45 (11 server + 34 player) | 0 `default` labels |
| Cyclomatic complexity (approx.) | > 170 for the function (45 cases + ~120 `if`/`&&`/`||` terms) | not directly measurable without a parser; counted by inspection |
| Nesting depth (max) | 7 (`else` -> `switch` -> `case` -> `if` -> `for` -> `if` -> `if`, e.g. lines 188-1005) | |
| Distinct outbound calls | 39 project functions, 3 CPSock methods, 27 globals | section 5.1 |
| Magic numbers | ~40 literal indices/ids (item 3467, 547, 3191-3198, 543-549, 470-500, 774/775, level 999, 2010, nPos 64/192, 2112/2042, 0x80, 0xC0) | none named |
| Duplication | 6 branches share the "signal-then-close" shape; 3 branches repeat the `int ID = m->ID;` dead local; 3 branches repeat the identical `ItemCode`/`log` block inside `_MSG_DBSendItem`; cargo placement and MAC-block logic duplicated on the DBSrv side | |
| Comment markers | 11 `TODO` / `// return; ?` / `// break; ?` markers recording decompiler uncertainty (`81`, `134`, `649`, `865`, `895`, `1095`, `1287-1289`, `640-660`) | |
| Dead code | 3 unreachable guards/blocks + 3 unreachable message types + 1 no-op call (`GetGuild`) + 6 unused locals | section 10.1 |
| Error-path coverage | 0 explicit error returns; every failure is a log line, a `break`, or a `CloseUser` | |

---

## 11. Test Coverage Analysis

### 11.1 Test inventory: none

An exhaustive search was performed for test artefacts across the project root, excluding `.agents`, `.codegraph`, `.opencode` and `openspec`:

| Search | Result |
|--------|--------|
| Files matching `*test*`, `*spec*`, `*assert*` (any extension) | 0 files |
| Files matching `*.vcxproj`, `*.sln`, `CMakeLists.txt`, `Makefile`, `*.cmake`, `*.csproj`, `package.json`, `*.py` | 3 files: `W2PP Code Project.sln`, `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj` - no test project |
| Text `Test` / `test` / `gtest` / `Catch2` inside the solution and project files | 0 matches |
| CI configuration (`*.yml`, `*.yaml`, `*.cmd`, `*.bat`, `*.ps1`, `*.json`) | 0 files |
| Total source files (`*.cpp`, `*.h`) | 104 (87 TMSrv, 17 DBSrv/shared) |

**Conclusion: the project contains no automated tests of any kind, for any component.** There is no unit test, no integration test, no fixture, no mock, no golden-file test, no fuzz harness and no CI pipeline. The only executable verification artefacts are the prebuilt binaries under `Code/release/TMSrv/run/` (`TMSrv.exe`, 509 KB, plus `TMSrv.pdb`, 2.8 MB) and the MSBuild intermediates under `Code/TMSrv/Debug`, `Code/TMSrv/Release`, `Code/TMSrv/TMSrv/Release`, `Code/DBSrv/Debug`, `Code/DBSrv/Release`, `Code/DBSrv/DBSrv/Release`.

### 11.2 Coverage by branch

| Branch group | Cases | Unit tests | Integration tests | Effective coverage | Test quality |
|--------------|-------|------------|-------------------|--------------------|--------------|
| Envelope/routing (BR-01..BR-05) | 5 | 0 | 0 | 0 % | n/a |
| Server-scoped directives (BR-07..BR-17) | 11 | 0 | 0 | 0 % | n/a - requires a live DBSrv |
| Account/session establishment (BR-21..BR-29) | 9 | 0 | 0 | 0 % | n/a - also requires a client |
| Character management (BR-30..BR-33) | 4 | 0 | 0 | 0 % | n/a |
| Character login (BR-34, 18 sub-rules) | 18 | 0 | 0 | 0 % | n/a - highest-risk, zero coverage |
| Concurrency/teardown (BR-37..BR-40) | 4 | 0 | 0 | 0 % | n/a - zombie-slot defects (R-02, R-03) exist precisely because no test observes slot state |
| Arch/capsule (BR-41..BR-47) | 7 | 0 | 0 | 0 % | n/a |
| Commerce/notification/relay (BR-18, BR-19, BR-20, BR-43..BR-51) | 11 | 0 | 0 | 0 % | n/a |
| **Total** | **45 cases / 51 rules** | **0** | **0** | **0 %** | **None** |

### 11.3 Structural impediments to testing (observed, not recommendations)

1. **No seam.** `ProcessDBMessage` takes a raw `char*` that must already be de-obfuscated and framed; producing a valid input requires replicating `CPSock::AddMessage`'s obfuscation (`Code/CPSock.cpp:556-590`) or calling `ReadMessage`.
2. **Global state dominates.** A test of any branch requires a fully initialised `pUser[1000]`, `pMob[MAX_MOB]`, `pMobGrid[4096][4096]` (32 MB), `GuildInfo[65536]`, `g_pItemList[6500]` (loaded from CSV files that are not in the repository) and `g_pMessageStringTable` (built from `UI/strdef.bin`, also absent).
3. **The behaviour itself depends on absent data files.** `g_pItemList` is populated by `BASE_LoadItemList` from `../../Common/itemlist.csv`; spawn cities come from `Guilds.txt`; the MAC block list comes from `../../DBSRV/Run/Mac.txt`; the message strings come from `UI/strdef.bin`. None of these exist in the repository, so even a linked harness would exercise degenerate branches.
4. **The single-threaded Win32 host is part of the contract.** Several behaviours depend on the 500 ms timer flush and on `SetWindowText` being callable; the component is not usable outside `MainWndProc`.
5. **No assertions anywhere.** The codebase contains no `assert`, no error-code returns from the dispatcher, and no observability beyond `Log`; the only way to observe the effect of a branch is to inspect global state after the call, which is exactly what a test would have to do.

### 11.4 Manual verification evidence that does exist

The only de-facto "test" of this component is the embedded `_PACKET_DEBUG` instrumentation: when compiled with that macro, both the send path (`Code/CPSock.cpp:544-552`) and the DB receive loop (`Code/TMSrv/Server.cpp:3895-3905`) call `BASE_CheckPacket` and log `**PACKET_DEBUG** Type:%d Size:%d` on a mismatch. Because `BASE_CheckPacket`'s body is commented out, the macro currently logs nothing deterministically (the function's return value is undefined), so this instrumentation provides no verification either.

---

## 12. Analysis Notes, Ambiguities and Confidence Levels

| Item | Statement | Confidence |
|------|-----------|------------|
| R-02, R-03 slot leak | Derived by tracing `CloseUser` (`Server.cpp:6834-6926`), `CUser::CloseUser` (`CUser.cpp:81-92`), `CPSock::CloseSocket` (`CPSock.cpp:84-98`) and the 500 ms sweep (`ProcessSecMinTimer.cpp:201-221`). The code paths are unambiguous; whether the deployed operators restart servers often enough to mask the leak is unknown | High (code), Medium (operational impact) |
| R-04 dead normalisation | `sm.mob` is written at 654-705 and wholly replaced at 784 from `pMob[conn].MOB`, which was assigned from `m->mob` at 733. No intervening write to `pMob[conn].MOB.Equip/Carry` exists | High |
| R-05 admin flag | `n` is the loop variable of the guild-zone scan (868-879); if the loop breaks early it holds the zone index, otherwise `MAX_GUILDZONE` (5). Either way it is not `conn` | High |
| BR-17 semantics | The rule's *purpose* (one primary account per machine) is inferred from the text "Sua conta agora é a principal" (`DBSrv/CFileDB.cpp:2098`) and the `OnlyTrade` consumers. The exact policy (why `GetAccountsByMac(...) <= 1` triggers a broadcast rather than `> 1`) is ambiguous and may encode a cross-shard invariant that is not visible in this repository | Medium |
| BR-24 block tiers X/A/B/C | The letters are the observed substitutions; their meaning (permanent/severity/type) is not documented anywhere in the codebase | Low (semantics) / High (mechanism) |
| `_MSG_DBCNFAccountLogin` client payload | The client is sent `MSG_DBCNFAccountLogin` bytes under `Type = _MSG_CNFAccountLogin`; `Basedef.h:1095` carries a TODO acknowledging the structure-name mismatch, and no `MSG_CNFAccountLogin` struct is defined, so the client's interpretation cannot be verified from this repository | Medium |
| SL-16 `Merchant` field | Line 765 clears `BaseScore.Merchant`; line 851 reads `MOB.Merchant`. `STRUCT_MOB` has one `Merchant` field at offset 17 (`Basedef.h:442`) and `STRUCT_SCORE` has a separate `Merchant` at `Basedef.h:420`, so the two are different fields - the code is consistent, but only if `BaseScore.Merchant` is not the city selector | High (fields), Medium (intent) |
| `_MSG_ReqTransper` reachability | The branch requires `TransperCharacter != 0`, which requires `redirect.txt` on DBSrv. Without it the branch is unreachable and R-01 is dormant | High |
| ~~`_MSG_DBCNFCharacterLogin` `conn == 0` check~~ | The `if (conn == 0)` block at 643-650 is unreachable because the `else` was already taken; it is listed as dead code, not as a defect | High |
| Absent data files | `itemlist.csv`, `Guilds.txt`, `serverlist.txt`, `UI/strdef.bin` and `../../DBSRV/Run/Mac.txt` are referenced but not present in the repository; behaviour of every branch that formats a message or resolves an item depends on them at runtime | High |
| Efferent count | 39 project functions + 27 globals + 3 methods is the symbol-level count; the architectural report's "~14" corresponds to module-level counting (8 in-process modules + peer + CRT/OS groups). Both are correct at their granularity; the difference is stated explicitly in section 6.1 | High |
| Line numbers | All references are against the current working tree; `ProcessDBMessage.cpp` has been unchanged since 2018-05-22 (`git show e306ab8`), so the references are stable | High |

---

*End of report.*
