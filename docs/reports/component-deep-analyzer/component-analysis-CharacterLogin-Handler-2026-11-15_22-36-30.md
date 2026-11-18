# Component Deep Analysis Report

**Component:** CharacterLogin Handler
**Primary artifact:** `Code/TMSrv/_MSG_CharacterLogin.cpp` (110 lines, 3,338 bytes, single function `Exec_MSG_CharacterLogin(int conn, char *pMsg)`)
**Project:** W2PP (decompiled C/C++, Win32 / MSVC v143, MMORPG server emulator for W2)
**Host process:** TMSrv (game / world server) — Win32 only (`Code/TMSrv/TMSrv.vcxproj:4-8`, `:28`, `:35`)
**Analysis date:** 2026-11-15 22:36:30
**Analysis scope:** entire project root `E:\wyd\test-setup\w2pp`, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Architectural role (given):** character-slot selection and DB confirmation handshake. Afferent 1, efferent 14 (independently re-measured in section 6).
**DB-side counterpart:** `_MSG_DBCNFCharacterLogin` branch, `Code/TMSrv/ProcessDBMessage.cpp:635-1081`, plus its requester/responder in `Code/DBSrv/CFileDB.cpp:1028-1094`.

---

## 1. Executive Summary

`CharacterLogin Handler` is the TMSrv gate that converts "player picked a character slot" into "player is being materialised in the world". It is a single 90-line procedure, `Exec_MSG_CharacterLogin` at `Code/TMSrv/_MSG_CharacterLogin.cpp:21-110`, reached from exactly one call site — the client dispatcher at `Code/TMSrv/ProcessClientMessage.cpp:72-74` — for wire type `_MSG_CharacterLogin` (`19 | FLAG_CLIENT2GAME` = 0x0213 = 531, `Code/Basedef.h:1335`). It never touches the character payload itself: it performs eligibility checks over the cached character-select snapshot (`pUser[conn].SelChar`), reserves the session (`Mode = USER_CHARWAIT`, `pMob[conn].Mode = MOB_USER`), rewrites the client packet in place into `_MSG_DBCharacterLogin` (`4 | FLAG_GAME2DB` = 0x0804 = 2052, `Code/Basedef.h:979`) and forwards it to DBSrv. Everything the player actually perceives after that point — world placement, stat recalculation, the confirmation frame, area-of-interest broadcast — happens in the DB dispatcher branch, not here.

The component is therefore best understood as a **session state-machine gate with an asynchronous, uncorrelated back end**. Its five distinct responsibilities are: (a) slot domain validation, (b) a three-tier billing/parental-control eligibility ladder, (c) session mode arbitration and in-flight request suppression, (d) request re-addressing to DBSrv, and (e) user-visible rejection messaging. Four of the thirteen business rules extracted in section 3 are unreachable in the shipped build, three more depend on global state that is never written anywhere in the repository, and the component has zero test coverage.

Key findings:

- **The entire billing/parental-control sub-system that this handler gates on is inert.** `SendBilling` is a stub returning `TRUE` (`Code/TMSrv/Server.cpp:1377-1380`), the billing response pump `ProcessBILLMessage` has an empty body (`Code/TMSrv/Server.cpp:4300-4302`) while still being wired to the bill socket (`Code/TMSrv/Server.cpp:3806`), and `pUser[].Unk_1816` is only ever written with `0` (`Code/TMSrv/ProcessDBMessage.cpp:439`) and `5` (`Code/TMSrv/_MSG_CharacterLogin.cpp:100`). The states the handler branches on — `Unk_1816 == 3` (account not allowed) and `Unk_1816 == 4` (using another server group) — are never produced by any code in the project (exhaustive search over `Code/**/*.cpp` for `Unk_1816` returns 8 hits, all in `CUser.h`, the handler and one reset). The same is true for `Unk_2728`, which is only ever set to `0` (`Code/TMSrv/ProcessDBMessage.cpp:394`) and read at `_MSG_CharacterLogin.cpp:69` and `Code/TMSrv/ProcessSecMinTimer.cpp:178`.
- **`g_Hour` is a permanently-zero global.** It is declared `int g_Hour = 0` (`Code/TMSrv/Server.cpp:288`) and assigned nowhere in the repository (verified across all `*.cpp`/`*.h`). Both hour-based policies that depend on it — the login-time child-account window at `_MSG_CharacterLogin.cpp:69` and the periodic child-account kick at `ProcessSecMinTimer.cpp:178` — therefore evaluate a constant.
- **A permanently-false compound condition makes a whole message block dead code.** `_MSG_CharacterLogin.cpp:53-54` requires `Level >= FREEEXP && Level < 999 && BILLING == 3 && Level >= 1000`. `Level < 999 && Level >= 1000` is unsatisfiable, so the block at `:53-67` (three distinct rejection messages, including `_NN_Wait_Checking_Billing`, `_DN_Not_Allowed_Account` and `_NN_Using_Other_Server_Group`) can never execute. The block also disagrees with its own reachability: it is only entered after `goto Label1` from `:32`, which is taken exactly when `BILLING != 2`, yet the block demands `BILLING == 3`.
- **Default configuration bypasses the billing ladder entirely.** `BILLING` defaults to `3` (`Code/TMSrv/Server.cpp:320`) and is loaded from `gameconfig.txt` line 6 (`Server.cpp:917`). At `_MSG_CharacterLogin.cpp:31` the guard `... || BILLING != 2` short-circuits to `goto Label1`, so with the shipped default every billing-specific branch is skipped for every character.
- **No packet-length validation, and the project-wide validator is a stub.** `CPSock::ReadMessage` only enforces the envelope bounds `sizeof(HEADER) <= Size <= 8192` (`Code/CPSock.cpp:397`) and returns a pointer into the 128 KB receive buffer (`Code/CPSock.h:35`). `BASE_CheckPacket`, which contains per-type length assertions including `_MSG_CharacterLogin == sizeof(MSG_CharacterLogin)` (`Code/Basedef.cpp:6491`), has its entire body commented out and unconditionally returns `FALSE` (`Code/Basedef.cpp:6476`, `:6593-6596`); its two TMSrv call sites are additionally compiled only under `#ifdef _PACKET_DEBUG` and only log (`Code/TMSrv/Server.cpp:3895-3905`, `:3980-3988`). The handler never inspects `m->Size` before dereferencing `m->Slot` and `m->Force` at `:23-31`.
- **The handshake has no failure channel from the database.** DBSrv never emits `_MSG_DBCharacterLoginFail` (0 hits in `Code/DBSrv`), although TMSrv contains a full handler for it (`Code/TMSrv/ProcessDBMessage.cpp:1084-1096`). The three DB-side rejection branches for a character login — illegal slot (`Code/DBSrv/CFileDB.cpp:1036-1041`), missing PIN verification (`:1043-1050`), empty character name (`:1073-1078`) — all `Log`/`break`/`return` without any reply. The client therefore receives nothing, and the session remains in `USER_CHARWAIT` until the idle reaper closes it after 720 s (`Code/TMSrv/Server.cpp:4304-4322`, invoked at `Code/TMSrv/ProcessSecMinTimer.cpp:1179-1182`).
- **Double-login prevention is not implemented in this component.** The only "already in use" semantics are account-level and are enforced at account login in DBSrv (`_MSG_DBAlreadyPlaying` / `_MSG_DBStillPlaying`, `Code/DBSrv/CFileDB.cpp:685-703`). The password-less "server group change" bypass at `CFileDB.cpp:664-675` and `:802-853` re-enters this handshake from the DB side without ever passing through `Exec_MSG_CharacterLogin`, so the handler's slot/level/billing checks are silently skipped on that path.
- **The confirmation step is not correlated to a session.** The `_MSG_DBCNFCharacterLogin` branch validates only the ID range (via `ProcessDBMessage.cpp:43`) and never checks `pUser[conn].Mode == USER_CHARWAIT`, the account name, or the character name before writing `pMob[conn].MOB`, re-pointing the spawn position and sending a confirmation to the socket currently occupying index `conn` (`ProcessDBMessage.cpp:643-650`, `:733-792`, `:984`). Because `CloseUser` frees the slot immediately (`Code/TMSrv/Server.cpp:6834-6867`) and `GetEmptyUser` reuses the lowest free index (`Server.cpp:6743-6753`), a delayed or replayed confirmation can be applied to a different connection.
- **The post-login admin grant is computed on the wrong index.** `ProcessDBMessage.cpp:1004-1005` tests `pMob[n].MOB.CurrentScore.Level >= 999` and sets `pUser[n].Admin = 1`, where `n` is the leaked loop variable of the guild-zone scan (`:868-879`), i.e. `0..MAX_GUILDZONE` (0..5) rather than `conn`. `Admin` is cleared at `ProcessDBMessage.cpp:391` and `Server.cpp:6843`, so `pUser[].Admin` is effectively never set correctly for a logging-in player.
- **Control flow is unstructured.** A `goto Label1` at `:32` jumps into an `else` block whose label sits at `:51`; the resulting graph has an estimated cyclomatic complexity of 21 for 90 lines, with three independent `if` chains re-testing the same `Unk_1816` variable and a duplicated `Level`/`BILLING` predicate.
- **Zero automated test coverage exists in the repository** — no test project in `W2PP Code Project.sln`, no test framework reference in either `.vcxproj`, no test/spec files anywhere outside the ignored folders, no CI configuration, no assertions in the codebase (section 11).

Verdict: the handler is functionally a thin, defensively shallow relay whose real behaviour is defined by remote state. Its safety properties rest almost entirely on the DB dispatcher's post-conditions and on an account-level lock acquired earlier in the session; the local checks that do exist (slot range, session mode) are correct but incomplete, and the eligibility ladder around them is dead, unimplemented, or disabled by default configuration.

---

## 2. Data Flow Analysis

### 2.1 Success path (billing ladder bypassed — the shipped default)

```
 1. Client sends _MSG_CharacterLogin (531 / 0x0213) carrying MSG_CharacterLogin
       { Size, KeyWord, CheckSum, Type, ID, ClientTick, Slot:int, Force:int }  (20 bytes)
 2. CPSock::ReadMessage (Code/CPSock.cpp:353-467) enforces 12 <= Size <= 8192, decrypts
       in place and returns a pointer INTO the 128 KB pRecvBuffer (no per-type size check)
 3. Code/TMSrv/Server.cpp:3973-4001 (WSA_READ event) -> ProcessClientMessage(User, Msg, FALSE)
 4. ProcessClientMessage.cpp:42-64: ID range check, ServerDown guard, refreshes
       pUser[conn].LastReceiveTime, rejects ClientTick == SKIPCHECKTICK, rejects _MSG_Ping
 5. ProcessClientMessage.cpp:72-74 -> Exec_MSG_CharacterLogin(conn, pMsg)
 6. _MSG_CharacterLogin.cpp:23      cast pMsg -> MSG_CharacterLogin*  (no Size validation)
 7. _MSG_CharacterLogin.cpp:25-29   BR-01 slot domain check 0 <= Slot < MOB_PER_ACCOUNT (4)
                                    -> on failure: _NN_SelectCharacter message, return
 8. _MSG_CharacterLogin.cpp:31-32   BR-02 billing shortcut: (Level < FREEEXP || Level >= 999
                                    || BILLING != 2) -> goto Label1  (taken with default BILLING=3)
 9. _MSG_CharacterLogin.cpp:69-91   BR-06 child-account hour gate (always passes: Unk_2728 != 1)
10. _MSG_CharacterLogin.cpp:71-72   BR-07 session gate: pUser[conn].Mode == USER_SELCHAR
11. _MSG_CharacterLogin.cpp:73-80   BR-08/BR-09 reservation + re-addressing:
                                       m->Type = _MSG_DBCharacterLogin (2052)
                                       m->ID   = conn
                                       pUser[conn].Mode = USER_CHARWAIT (12)
                                       pMob[conn].Mode  = MOB_USER (2)
                                       pMob[conn].MOB.Merchant = 0
12. _MSG_CharacterLogin.cpp:81      DBServerSocket.SendOneMessage(m, sizeof(MSG_CharacterLogin)=20)
                                       -> CPSock::AddMessage re-encodes and COPIES into the DB
                                          send buffer (CPSock.cpp:513-591), so the mutated
                                          receive-buffer frame is safe to discard
13. Code/DBSrv/CFileDB.cpp:1028-1094  DB side: GetIndex(conn, m->ID), slot re-check (BR-10),
                                       SecurePass != -1 requirement, mobname non-empty test,
                                       build MSG_CNFCharacterLogin from file.Char[Slot]
                                       + Donate + ShortSkill + affect + mobExtra
14. Code/DBSrv/CFileDB.cpp:1080      DB sends _MSG_DBCNFCharacterLogin to TMSrv
15. Code/TMSrv/ProcessDBMessage.cpp:635-1081  TMSrv applies the character:
       :641  memcpy into a MSG_CNFClientCharacterLogin view (truncating the larger CNF)
       :654-696  normalise legacy damage effects on equipment/carry items
       :706-726  class-master face/effect fix-ups on Equip[0]
       :730-731  HP floor: CurrentScore.Hp <= 0 -> 2
       :733      pMob[conn].MOB = m->mob            (authoritative overwrite)
       :735-749  reset counters, copy ShortSkill (16 B) and Affects
       :751-757  carry capacity: 30 base, +15 per 3467 item in Carry[60]/Carry[61]
       :773-780  BASE_GetHpMp / GetCurrentScore / GetGuild / bonus points
       :782-807  build and send MSG_CNFClientCharacterLogin to the client
       :848-904  BR-11 world placement + GetEmptyMobGrid occupancy search
       :906      pUser[conn].Mode = USER_PLAY (22)
       :997-999  pMobGrid[PosY][PosX] = conn; GridMulticast(_MSG_CreateMob, CreateType=2)
      :1001-1013 SendPKInfo, SendGridMob, MountProcess, SendWarInfo, castle state, ClearCrown
      :1015-1051 EXP segment computation, optional billing reminder (CHARSELBILL == 0)
      :1064-1080 RvR bonus notice, SendEtc, SendScore, "sta,Login char:..." audit log
```

### 2.2 Branch map for the eligibility ladder (all paths)

```
Slot out of [0,4)                 -> _NN_SelectCharacter, return            (:25-29)
Level < FREEEXP                   -> goto Label1 (no billing checks)        (:31-32)
Level >= 999                      -> goto Label1 (treated as staff char)    (:31-32)
BILLING != 2                      -> goto Label1  <-- DEFAULT (BILLING=3)   (:31-32)
BILLING == 2 and Unk_1816 > 1:
    Unk_1816 == 3                 -> _DN_Not_Allowed_Account + signal 404    (:36-43)
    Unk_1816 == 4                 -> _NN_Using_Other_Server_Group, return    (:46-47)
    otherwise (2,5,6,...)         -> fall through to Label1                 (:49-51)
BILLING == 2 and Unk_1816 <= 1:
    Unk_2732 != 0 and Unk_2732 < SecCounter-10
                                  -> Unk_2732 = 0; Unk_1816 = 5; return      (:97-101)
    else                          -> _NN_Wait_Checking_Billing + SendBilling (:102-106)
Label1:
    (Level >= FREEEXP && Level < 999 && BILLING == 3 && Level >= 1000)
                                  -> dead block, three messages              (:53-67)
    (BILLING != 2 || Unk_2728 != 1 || Level < FREEEXP || (g_Hour > 7 && g_Hour < 23))
        Mode == USER_SELCHAR      -> BR-08/09 reservation + DB request        (:71-81)
        Mode != USER_SELCHAR      -> "Wait a moment." + err log, return      (:83-88)
    else                          -> _NN_Child_Pay, return                   (:90-91)
```

### 2.3 Failure and stall paths

```
DB rejects (slot / PIN / empty name)   -> no reply                            (CFileDB.cpp:1036-1078)
DB account not in list                 -> pAccountList[idx] is stale/zeroed    (CFileDB.cpp:1034)
DB slot data malformed                 -> CNF with garbage mob, applied anyway (ProcessDBMessage.cpp:733)
Client disconnects during USER_CHARWAIT-> CloseUser -> _MSG_DBNoNeedSave        (Server.cpp:6852-6867)
CFN arrives after slot reuse           -> applied to the NEW connection         (ProcessDBMessage.cpp:635)
Client silent for > 720 s              -> CheckIdle -> CloseUser               (Server.cpp:4304-4322)
Repeated login while USER_CHARWAIT     -> "Wait a moment." + log, no state change (:83-88)
```

The strict ordering property to note is that the state transition to `USER_CHARWAIT` and the DB request are issued in the same critical section (`:73-81`), with no await point in between, so a duplicate client frame cannot produce two DB requests. The reverse property — a DB request producing two state transitions — is not protected, because the CNF branch has no idempotency or correlation check.

---

## 3. Business Rules & Logic

## Overview of the business rules:

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Validation | Requested character slot must satisfy `0 <= Slot < MOB_PER_ACCOUNT` (4); otherwise reply `_NN_SelectCharacter` and abort | `Code/TMSrv/_MSG_CharacterLogin.cpp:25-29` |
| BR-02 | Business Logic / Entitlement | Characters below `FREEEXP` (default 35) or at/above level 999, or any server whose `BILLING != 2`, bypass the billing eligibility ladder | `_MSG_CharacterLogin.cpp:31-32`, `Code/TMSrv/Server.cpp:43`, `:320` |
| BR-03 | Business Logic / Eligibility | If `Unk_1816 == 3` the account is billing-blocked: message formatted with `FREEEXP`, plus undocumented client signal 404 and explicit flush; if `Unk_1816 == 4` the account is considered logged into another server group: reject with `_NN_Using_Other_Server_Group` | `_MSG_CharacterLogin.cpp:34-50` |
| BR-04 | Business Logic / Timing | Billing verification grace: with `Unk_1816 <= 1`, a login attempt is refused with `_NN_Wait_Checking_Billing` and a billing request; if at least 10 seconds have elapsed since `Unk_2732` was stamped, the seat is promoted to `Unk_1816 = 5` and the client must retry | `_MSG_CharacterLogin.cpp:95-107`, `:34-51` |
| BR-05 | Business Logic (unreachable) | Re-evaluation block for `BILLING == 3` and celestial-level characters (`Level >= 1000`) that re-emits the billing/account messages | `_MSG_CharacterLogin.cpp:53-67` |
| BR-06 | Policy / Parental control (inert) | Child accounts (`Unk_2728 == 1`) at/above `FREEEXP` are refused outside the 08:00-22:59 window (`g_Hour > 7 && g_Hour < 23` is the allow-condition); the periodic sweep instead kicks them outside 13:00-18:59 | `_MSG_CharacterLogin.cpp:69-91`, `Code/TMSrv/ProcessSecMinTimer.cpp:178-186` |
| BR-07 | State Guard | A character login is only accepted while the session is in `USER_SELCHAR`; a second attempt while the DB reply is outstanding is answered with the literal `"Wait a moment."` and audited | `_MSG_CharacterLogin.cpp:71-88` |
| BR-08 | State Reservation | Before the DB request, the session is reserved: `Mode = USER_CHARWAIT`, `pMob[].Mode = MOB_USER`, `pMob[].MOB.Merchant = 0` | `_MSG_CharacterLogin.cpp:76-79` |
| BR-09 | Protocol Binding | The inbound client frame is re-addressed in place to `_MSG_DBCharacterLogin` with `ID = conn` and forwarded at the fixed `sizeof(MSG_CharacterLogin)` length | `_MSG_CharacterLogin.cpp:73-81` |
| BR-10 | Validation (DB side) | DBSrv re-validates the slot, requires a completed account PIN verification (`SecurePass != -1`) and a non-empty character name before answering | `Code/DBSrv/CFileDB.cpp:1036-1078` |
| BR-11 | Business Logic / Placement | Spawn position is derived from `MOB.Merchant` bits 6-7 (city), overridden by a charged guild's zone spawn, overridden for sub-`FREEEXP` mortals to the newbie coordinates (2112,2042 +-2), then corrected by a spiral grid search | `Code/TMSrv/ProcessDBMessage.cpp:848-904` |
| BR-12 | Business Logic / Initialisation | On confirmation the character is normalised: HP floor of 2, carry capacity 30 (+15 per vial item 3467 in carry slots 60/61), Kill Mark item 547 injected if absent, per-session counters reset, legacy damage effects rewritten | `Code/TMSrv/ProcessDBMessage.cpp:654-757`, `:809-845` |
| BR-13 | Error Handling / Recovery | If DBSrv reports a character login failure the session returns to `USER_SELCHAR`, the client receives `_MSG_CharacterLoginFail` and a crack log is written; otherwise a stuck session is reclaimed only by the 720 s idle reaper | `Code/TMSrv/ProcessDBMessage.cpp:1084-1096`, `Code/TMSrv/Server.cpp:4304-4322` |

## Detailed breakdown of the business rules:

---

### Business Rule: BR-01 Slot domain validation

**Overview**:
The first and only locally-verifiable precondition: the requested slot index must address one of the four character slots of the account.

**Detailed description**:
At `_MSG_CharacterLogin.cpp:25` the handler tests `m->Slot < 0 || m->Slot >= MOB_PER_ACCOUNT` and, on violation, emits the generic `_NN_SelectCharacter` message (message id 34, `Code/TMSrv/Language.h:54`) before returning. `MOB_PER_ACCOUNT` is the compile-time constant `4` (`Code/Basedef.h:71`), and the same bound appears in the account snapshot (`STRUCT_SELCHAR` holds `Name[4]`, `Score[4]`, `Equip[4][16]`, `Guild[4]`, `Coin[4]`, `Exp[4]`, `Code/Basedef.h`), in the DB selector loop (`Code/DBSrv/CFileDB.cpp:2613`), in `IsFree` (`Code/TMSrv/Server.cpp:1363-1375`) and in the DB-side re-validation (`CFileDB.cpp:1036`). Because the subsequent expression at `:31` and `:53` indexes `pUser[conn].SelChar.Score[m->Slot]`, this check is what prevents an out-of-bounds read of up to `(2^31-1 - 4) * sizeof(STRUCT_SCORE)` past the snapshot inside the `CUser` object.

The check is a pure domain test with no side effects, no logging and no anti-cheat accounting. This is notable because nearly every other TMSrv client handler feeds malformed input into `AddCrackError` (`Code/TMSrv/Server.cpp:684-707`) or `CrackLog` (`Server.cpp:6671-6693`); here a client can send arbitrary `Slot` values indefinitely with no penalty counter increment, no disconnect and no rate limit. A client that repeatedly probes slots produces one outbound `_MSG_MessagePanel` per probe and nothing else, which is cheap for the attacker and for the server, but invisible in the crack logs.

The rejection path is also asymmetric with the DB-side rule (BR-10): the local bound is `>= 4`, matching `MOB_PER_ACCOUNT`, while the save/quit path elsewhere in the same server validates `Slot >= MAX_CLASS` (`Server.cpp:6893`), and `MAX_CLASS` is also `4` (`Code/Basedef.h:115`) — the same numeric value reached through a semantic constant that means something different (class count). This duality is a latent hazard if `MOB_PER_ACCOUNT` and `MAX_CLASS` ever diverge.

**Rule workflow**:
```
receive m->Slot
  if (Slot < 0)              -> SendClientMessage(_NN_SelectCharacter); return
  if (Slot >= MOB_PER_ACCOUNT) -> SendClientMessage(_NN_SelectCharacter); return
  -> continue to BR-02
```

---

### Business Rule: BR-02 Billing shortcut for low-level and staff-level characters

**Overview**:
Characters below the free-experience threshold, characters at or above level 999, and all characters on a server that is not in billing mode 2 skip the billing ladder and go straight to the login decision.

**Detailed description**:
The predicate at `:31` — `if (pUser[conn].SelChar.Score[m->Slot].Level < FREEEXP || pUser[conn].SelChar.Score[m->Slot].Level >= 999 || BILLING != 2) goto Label1;` — expresses three separate policies by short-circuiting the same jump. The first is the free-to-play window: `FREEEXP` is a mutable global initialised to `35` (`Code/TMSrv/Server.cpp:43`) and overwritten from `gameconfig.txt` line 6 (`Server.cpp:917-918`) or at runtime by the GM command `set billfree` (`Code/TMSrv/imple.cpp:607-611`). Characters below that level never touch the billing machinery. The second policy marks level 999 and above as an out-of-band staff/test tier: since the game's normal maxima are `MAX_LEVEL` 399 and `MAX_CLEVEL` 199 (`Code/Basedef.h:117-118`), the only way to reach 999 is by direct file edit, so this clause effectively exempts administrator-flagged characters from billing. The third policy is server-wide: `BILLING` defaults to `3` (`Server.cpp:320`), is loaded from the same config file (`Server.cpp:917`), is settable to 2 or 3 by GM command (`imple.cpp:591-606`) and is forced back to `3` when a bill server connection is established (`imple.cpp:1159`). Since the guard requires `BILLING == 2` to proceed to the billing block, the default configuration makes BR-03, BR-04, BR-05 and BR-06 unreachable on every server that has not explicitly been put in mode 2.

The level used here is the cached snapshot value `pUser[conn].SelChar.Score[Slot].Level`, not the authoritative `pMob[conn].MOB.CurrentScore.Level`. The snapshot is produced by DBSrv at account login (`DBGetSelChar` copies `file.Char[i].CurrentScore` into `sel->Score[i]`, `Code/DBSrv/CFileDB.cpp:2626`) and is refreshed only at a clean character logout (`pUser[conn].SelChar.Score[pUser[conn].Slot].Level = pMob[conn].MOB.BaseScore.Level`, `Code/TMSrv/Server.cpp:7115`). Consequently, if a character levels past `FREEEXP` during a session and the client is dropped abnormally (the idle reaper, a crash, a `CrackLog` logout), the cached snapshot can still report a level below the threshold on the next attempt — a policy-bypass window bounded by that stale value, though it is currently academic because the billing ladder is inert.

`FREEEXP` is also read in three other places with different semantics: `IsFree` (`Server.cpp:1363-1375`) decides whether an account has any billable character, the periodic sweep exempts characters below it (`ProcessSecMinTimer.cpp:178`), and the world-placement rule uses it to send low-level mortals to the newbie spawn (`ProcessDBMessage.cpp:881-885`). It is thus a single global that simultaneously encodes "free to play", "newbie", and "exempt from parental control" — one of the strongest indications that this decompiled code base collapses distinct policy concepts into one tunable.

**Rule workflow**:
```
Level = SelChar.Score[Slot].Level
  if (Level < FREEEXP)      -> goto Label1   (no billing checks)
  if (Level >= 999)         -> goto Label1   (staff/test tier)
  if (BILLING != 2)         -> goto Label1   (billing disabled - DEFAULT)
  -> enter billing ladder (BR-03, BR-04)
```

---

### Business Rule: BR-03 Billing-state gate (blocked account / other server group)

**Overview**:
Two terminal rejections driven by the session's billing state byte `pUser[conn].Unk_1816`, intended to represent "account not permitted" and "account already resident on another server group".

**Detailed description**:
The outer condition `if (pUser[conn].Unk_1816 > 1)` at `:34` opens the state-by-state ladder. State `3` (`:36-43`) is treated as an account-level billing block: the handler formats `_DN_Not_Allowed_Account` (id 39) with `FREEEXP` as the substitution argument, so the player is told the account is not allowed below the free-experience level, then sends an undocumented raw client signal `404` through `SendClientSignalParm(conn, 0, 404, 0)` and forces the outbound buffer with `pUser[conn].cSock.SendMessageA()`. State `4` (`:46-47`) is treated as "already connected through another server group" and answered with `_NN_Using_Other_Server_Group` (id 40). Any other value greater than one (`2`, `5`, `6`, ...) falls through the `else` at `:49-51` into the login decision at `Label1`.

The mechanics of the two terminal paths differ in a way that is easy to miss. The state-3 path performs an explicit flush (`:42`) because it terminates inside a branch that never reaches the normal end-of-handler flush performed by the per-second pump (`ProcessSecMinTimer.cpp:201-221`); the state-4 path does not flush explicitly and relies on that pump. Neither path resets `Unk_1816`, so the client remains in the blocked state and any retry follows the same path. Neither path writes a log entry, unlike most other rejection branches in the handler.

The critical fact for a reader of this component is that neither state is producible. An exhaustive search of `Code/**/*.cpp` for `Unk_1816` yields exactly one write of a non-zero value — `pUser[conn].Unk_1816 = 5` at `_MSG_CharacterLogin.cpp:100` — plus a reset to zero at account-login confirmation (`ProcessDBMessage.cpp:439`). There is no code assigning 2, 3 or 4, and the only external channel that could plausibly do so (the billing server pump) has an empty body (`Server.cpp:4300-4302`) and a no-op send function (`Server.cpp:1377-1380`). The states therefore document an intended protocol that was never completed in this build; their only real effect is that value `5`, once written by BR-04, is routed through the `else` branch at `:49-51` into the normal login path.

**Rule workflow**:
```
if (Unk_1816 > 1)
    if (Unk_1816 == 3) -> format _DN_Not_Allowed_Account(FREEEXP); signal 404; flush; return
    else if (Unk_1816 == 4) -> _NN_Using_Other_Server_Group; return
    else -> goto Label1   (state 2, 5, 6, ...)
else -> BR-04
```

---

### Business Rule: BR-04 Billing verification wait window and seat promotion

**Overview**:
When the session has no billing verdict yet (`Unk_1816 <= 1`), the handler refuses the login, asks the billing backend to check the account, and only permits login after a state promotion gated on a ten-second timer.

**Detailed description**:
The branch at `:95-107` handles the "not yet checked" state. If `pUser[conn].Unk_2732` is non-zero and is strictly less than `SecCounter - 10`, the handler clears the timestamp and promotes the session to `Unk_1816 = 5`, then returns without any message to the client. `Unk_2732` is stamped with `SecCounter` at the end of a successful account login precisely when `BILLING > 0 && IsFree(...)` holds (`ProcessDBMessage.cpp:443-451`), and `SecCounter` is the one-second tick counter incremented in `ProcessSecTimer` (`ProcessSecMinTimer.cpp:123`, declared `unsigned int`, `Server.cpp:384`). Otherwise — i.e. on the first attempt, or on any attempt within ten seconds of the stamp — the client is told `_NN_Wait_Checking_Billing` (id 38) and `SendBilling(conn, AccountName, 1, 1)` is invoked.

The intended protocol is therefore three-phase and client-driven: attempt 1 asks for a billing check (and, in the original design, the answer would arrive asynchronously and be written into `Unk_1816`); an attempt at least ten seconds later promotes the seat to state 5; the following attempt takes the `Unk_1816 > 1` path (`:34`) whose `else` branch (`:49-51`) falls into the normal login. Because `SendBilling` is a stub that returns `TRUE` without transmitting anything (`Server.cpp:1377-1380`) and because nothing writes states 2/3/4, the practical result in this build is: the first attempt shows "wait", the second attempt is silently ignored, the third attempt logs in. The middle step is a completely silent state transition, which is a notable usability defect.

Two further details are worth recording. First, `Unk_2732` is `unsigned int` (`Code/TMSrv/CUser.h:100`) while `SecCounter` is also `unsigned int`; during the first ten seconds after process start, `SecCounter - 10` underflows to a value near 2^32-1, so any non-zero `Unk_2732` satisfies `Unk_2732 < SecCounter - 10` and the seat is promoted immediately — an edge case that only matters in the first seconds of uptime. Second, the promotion never re-verifies that the seat is still the same account: `Unk_1816`/`Unk_2732` are reset at account-login confirmation (`ProcessDBMessage.cpp:393-394`, `:439`), but not on `CloseUser` (`Server.cpp:6834-6867`), so a slot reused without a new account login inherits the previous occupant's billing verdict.

**Rule workflow**:
```
if (Unk_1816 <= 1)
    if (Unk_2732 != 0 && Unk_2732 < SecCounter - 10)
        Unk_2732 = 0; Unk_1816 = 5; return        (silent; client must retry)
    else
        _NN_Wait_Checking_Billing; SendBilling(conn, account, 1, 1); return
```

---

### Business Rule: BR-05 Mid-session billing re-evaluation for celestial-tier characters (unreachable)

**Overview**:
A block intended to re-apply billing/account messaging for celestial-tier characters on a billing-mode-3 server; provably unreachable because of an internal contradiction.

**Detailed description**:
After the `goto Label1` at `:32`, the very first statement is a compound test (`:53-54`): `Level >= FREEEXP && Level < 999 && BILLING == 3 && Level >= 1000`. The conjunction of `Level < 999` and `Level >= 1000` is unsatisfiable for any integer, so the block can never be entered. Even ignoring that contradiction, the `BILLING == 3` requirement conflicts with the only way to reach the block from the top of the handler, which is `BILLING != 2` — so a server in mode 3 would enter the block, but only if the unreachable level test also held.

The block's body (`:56-66`) is nonetheless informative about intent: it re-sends `_NN_Wait_Checking_Billing` when `Unk_1816 <= 1`, re-sends the not-allowed message when `Unk_1816 == 3`, and re-sends the other-server-group message when `Unk_1816 == 4`. That is the same set of messages already produced by BR-03/BR-04, which suggests this block was duplicated during decompilation or refactoring rather than designed. Note also that the level-1000 test does not match any other constant in the project: the celestial cap is `MAX_CLEVEL` 199 and the mortal cap `MAX_LEVEL` 399 (`Code/Basedef.h:117-118`), and the only other "1000" comparisons found in the login path are the `Level < 1000` recall guards in `ProcessSecMinTimer.cpp:1192`, `:1196`, `:1200`.

For analysis purposes the practical consequence is that the three `SendClientMessage` calls inside this block are dead code, and any expected behaviour a maintainer might infer from them (for example "celestial characters on a mode-3 server get a billing reminder at character selection") does not occur. Since the remaining `if` chain from `:69` onward is reached through the label, the block is a no-op inserted between the label and the decision, and its removal would not change behaviour.

**Rule workflow**:
```
Label1:
  if (Level >= FREEEXP && Level < 999 && BILLING == 3 && Level >= 1000)   [never true]
      if (Unk_1816 <= 1) -> _NN_Wait_Checking_Billing
      if (Unk_1816 == 3) -> _DN_Not_Allowed_Account(FREEEXP)
      if (Unk_1816 == 4) -> _NN_Using_Other_Server_Group
  -> continue to BR-06
```

---

### Business Rule: BR-06 Child-account play-hour restriction (inert, and internally inconsistent)

**Overview**:
A parental-control policy that refuses login to child accounts at or above `FREEEXP` outside the permitted hours, mirrored by a periodic sweep that disconnects them during the complementary hours.

**Detailed description**:
The login-side predicate at `:69` is the *allow* condition: `if (BILLING != 2 || pUser[conn].Unk_2728 != 1 || Level < FREEEXP || (g_Hour > 7 && g_Hour < 23))` proceeds to the DB request; the `else` at `:90-91` emits `_NN_Child_Pay` (id 206) and returns. Read as a denial rule, a child account (`Unk_2728 == 1`) with `Level >= FREEEXP` on a billing-mode-2 server is refused when the hour is **not** in 8..22 — that is, refused at 00:00-07:59 and 23:00-23:59, and allowed during 08:00-22:59. The mirror rule in the periodic sweep (`ProcessSecMinTimer.cpp:176-186`) runs every 8 seconds over a rotating user and kicks child accounts with `Level >= FREEEXP` when `(g_Hour <= 12 || g_Hour >= 19)`, i.e. it disconnects at 00:00-12:59 and 19:00-23:59. The two windows are not complements: hours 08:00-12:59 and 19:00-22:59 are *allowed to log in* by BR-06 but *immediately kicked* by the sweep. The only self-consistent permitted window is 13:00-18:59, which the login rule does not encode.

`Unk_2728` is documented in the source as "Related to BILLING (Child!?)" (`Code/TMSrv/CUser.h:99`) and is written exactly once in the project — to zero (`ProcessDBMessage.cpp:394`) — so the child-account branch is dead in this build. `g_Hour` is likewise never assigned; it is declared `int g_Hour = 0` (`Server.cpp:288`) and exposed in `Server.h:244`, but no code computes it, even though the minimal timer already holds a `struct tm` from `localtime` and other code in the same file uses `timeinfo->tm_hour` for castle scheduling (`Server.cpp:6233-6272`). The most defensible interpretation is that the assignment was lost during decompilation and the intended value was the local hour of day, which would make the 8..22 login window and the 13..18 play window the two halves of a deliberately asymmetric policy; this cannot be confirmed from the code and is recorded as an ambiguity.

Finally, the child-pay concept also appears at character logout: `CharLogOut` calls `SendBilling(conn, AccountName, 2, 0)` when the session has `IsBillConnect` and `CHARSELBILL == 0` (`Server.cpp:7101-7102`), and the login confirmation path conditionally reminds about billing when `CHARSELBILL == 0` (`ProcessDBMessage.cpp:1048-1052`). All of these routes terminate in the same no-op stub, so the whole feature family is documentation rather than behaviour.

**Rule workflow**:
```
Login side (:69):
  if (BILLING != 2)            -> allow
  if (Unk_2728 != 1)           -> allow
  if (Level < FREEEXP)         -> allow
  if (g_Hour in [8..22])       -> allow (g_Hour is always 0 -> never)
  else                         -> _NN_Child_Pay; return

Sweep side (ProcessSecMinTimer.cpp:178, every 8 s):
  if (BILLING == 2 && Unk_2728 == 1 && Level >= FREEEXP && (g_Hour <= 12 || g_Hour >= 19))
      -> _NN_Child_Pay; CharLogOut(SaveCount)
```

---

### Business Rule: BR-07 Session-state gating and single in-flight login

**Overview**:
A character login is only valid from the character-selection state; any attempt while a DB confirmation is outstanding is rejected with the literal English string `"Wait a moment."` and audited.

**Detailed description**:
Inside the decision at `:71`, `pUser[conn].Mode == USER_SELCHAR` (value 11, `Code/TMSrv/CUser.h:30`) is the precondition for proceeding. `USER_SELCHAR` is established at the end of account login (`ProcessDBMessage.cpp:400`) and restored at character logout (`Server.cpp:7113`) and on DB character-login failure (`ProcessDBMessage.cpp:1092`). If the mode is anything else, the handler sends the hardcoded string `"Wait a moment."` (`:85`) and writes an audit line with the account name and IP: `sprintf(temp, "err,charlogin not user_selchar %d %d", conn, pUser[conn].Mode)` (`:86-87`). Because the reservation step sets `USER_CHARWAIT` (`:76`) before the DB request, this predicate is what makes a duplicate or replayed client frame a no-op with respect to the database.

The gate is correct but shallow. It checks only that the session is in the right state, not that the session belongs to the account whose `SelChar` snapshot is being used, and not that the requested slot still exists. It also relies on no other handler changing `Mode` concurrently: the server is a single-threaded Win32 message loop (socket events, `TIMER_SEC`, `TIMER_MIN` are all dispatched from the same window procedure, `Server.cpp:3726-3728`, `:3731`, `:3812`, `:4006`), so the check is race-free by construction.

The rejection message is the only user-visible string in the component that is not taken from the message table — a localisation inconsistency (the message table is a 2,000-entry, 128-byte-per-entry array loaded from `Language.txt`, `Code/Basedef.cpp:742`, `:4637-4706`, `Code/Basedef.h:150`). The audit log also omits the slot, which makes forensic reconstruction of a failed attempt impossible without correlating with other logs.

**Rule workflow**:
```
if (Mode == USER_SELCHAR)
    -> BR-08 + BR-09
else
    SendClientMessage(conn, "Wait a moment.")
    Log("err,charlogin not user_selchar <conn> <mode>", AccountName, IP)
    return
```

---

### Business Rule: BR-08 Session reservation before the DB round trip

**Overview**:
Immediately before forwarding the request, the handler reserves the session and the mob slot so that the account appears "in transition" to every other subsystem.

**Detailed description**:
Three state writes happen in sequence at `:76-79`: `pUser[conn].Mode = USER_CHARWAIT` (value 12, `CUser.h:31`), `pMob[conn].Mode = MOB_USER` (value 2, `Code/TMSrv/CMob.h:28`) and `pMob[conn].MOB.Merchant = 0`. The first is the concurrency guard described in BR-07. The second marks the mob slot as belonging to a user so that NPC generators and mob allocation (`GetEmptyNPCMob`, `Server.cpp:6765-6790`, which scans from `MAX_USER` upward) do not reuse index `conn`. The third clears the merchant tie-in byte, which is significant because the same byte is used later as the city selector during world placement — `CityID = (pMob[conn].MOB.Merchant & 0xC0) >> 6` (`ProcessDBMessage.cpp:851`) — and again as a "route digit" carrier by the movement code.

`USER_CHARWAIT` has no timeout of its own: no code branches on it except this handler's own gate, and nothing escalates a session that is stuck in it. The only reclamation is the generic idle reaper, `CheckIdle`, which closes a session when `LastReceiveTime < SecCounter - 720` (`Server.cpp:4304-4322`), executed once per 32 seconds per connection (`ProcessSecMinTimer.cpp:1179-1182`). Because `ProcessClientMessage` refreshes `LastReceiveTime` for every inbound frame before dispatching (`ProcessClientMessage.cpp:56-57`) — including the early-returned `_MSG_Ping` — a client can keep a stuck `USER_CHARWAIT` session alive indefinitely by pinging, so the 720 s bound is only a bound for silent clients.

The reservation is also not rolled back on every failure path. On DB rejection there is no reply and no state change (BR-13), so the session and the mob slot stay reserved until the reaper or the client disconnects. On a stale DB reply arriving after the client disconnected, `CloseUser` has already emitted `_MSG_DBNoNeedSave` (`Server.cpp:6852-6867`) and reset the user slot, but the reply is still processed against whatever session now occupies `conn`.

**Rule workflow**:
```
m->Type = _MSG_DBCharacterLogin
m->ID   = conn
pUser[conn].Mode        = USER_CHARWAIT
pMob[conn].Mode         = MOB_USER
pMob[conn].MOB.Merchant = 0
DBServerSocket.SendOneMessage(m, sizeof(MSG_CharacterLogin))
return
```

---

### Business Rule: BR-09 Protocol re-addressing and identity binding

**Overview**:
The inbound client frame is converted in place into a game-to-DB frame whose `Type` and `ID` are server-controlled, and forwarded at a fixed length rather than the client-declared length.

**Detailed description**:
`MSG_CharacterLogin` is `_MSG` (12-byte header: `Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`, `Code/Basedef.h:925-931`) plus `int Slot; int Force;` (`Code/Basedef.h:1336-1341`), i.e. 20 bytes. The handler overwrites `m->Type` with `_MSG_DBCharacterLogin` and `m->ID` with the authoritative connection index, then calls `DBServerSocket.SendOneMessage((char*)m, sizeof(MSG_CharacterLogin))` (`:81`). This is the correct pattern for this code base: `CPSock::AddMessage` re-encrypts and copies the payload into the destination socket's own send buffer (`Code/CPSock.cpp:513-591`), so mutating the receive buffer before the send is safe, and `SendOneMessage` normalises the header `Size` to 20 regardless of the `Size` the client declared (`CPSock.cpp:686-694` calling `AddMessage`, which writes `pSMsg->Size = Size`).

Three properties follow. First, the `ID` rebinding closes the only field a client could otherwise use to impersonate another session at the DB layer. Second, the `Size` normalisation prevents a short frame from being relayed as short (which would make the DB read stale bytes of its own buffer) and prevents an oversized frame from being relayed at all — the 20-byte copy is what reaches the wire. Third, `Force` is carried through but never read by either side (no reference to `->Force` exists anywhere under `Code/`), so the field is dead in the protocol.

The weakness is on the receive side, not the send side. Because no length check precedes the field access at `:23-31`, a frame that declares `Size == 12` still causes the handler to read `Slot` and `Force` from bytes 12..19 of the 128 KB receive buffer, i.e. from the next queued frame or from residual plaintext of a previous message. The BR-01 range check limits the damage to a rejection, but the read itself is undefined behaviour with respect to the protocol contract and is the reason `BASE_CheckPacket`'s disabled body matters (`Code/Basedef.cpp:6475-6596`).

**Rule workflow**:
```
pMsg (client, arbitrary Size) -> cast MSG_CharacterLogin*
Slot read (possibly outside the declared frame)
m->Type = 2052 (_MSG_DBCharacterLogin)
m->ID   = conn
SendOneMessage(m, 20)  -> re-encoded + Size normalised to 20
```

---

### Business Rule: BR-10 Database-side defence in depth (PIN prerequisite, slot and name revalidation)

**Overview**:
The DB counterpart refuses to serve a character login unless the slot is in range, the account has completed numeric-token verification, and the character record has a name.

**Detailed description**:
`case _MSG_DBCharacterLogin` (`Code/DBSrv/CFileDB.cpp:1028`) begins by recomputing the account index from the forwarding server and the connection id — `int Idx = GetIndex(conn, m->ID)` with `GetIndex(server, id) = server * MAX_USER + id` (`CFileDB.cpp:2328-2333`) — then re-tests the slot (`:1036-1041`), reads `SecurePass` (`:1043`) and refuses when it is `-1` (`:1045-1050`), and finally rejects an empty `MobName` (`:1073-1078`). `SecurePass` is set to `-1` at account login (`CFileDB.cpp:762`), set to `1` when the client completes the numeric-token exchange via `_MSG_AccountSecure` (`CFileDB.cpp:1386`, `:1394`, `:1413`), and set to `1` on the password-less server-change path (`:806`). It is therefore the evidence that the second authentication factor has been satisfied.

The important structural observation is that these three checks duplicate the TMSrv checks (slot) or enforce something TMSrv cannot see (PIN state, stored name). The PIN state is genuinely server-side-only information: TMSrv's `Exec_MSG_AccountSecure` merely relays the client's token to the DB (`Code/TMSrv/_MSG_AccountSecure.cpp:21-31`), so a client that skips the PIN step reaches this branch with `SecurePass == -1` and is refused.

All three refusals are silent from the client's point of view. The slot branch logs `"err,charlogin slot illegal"` and `break`s; the PIN branch logs `"err,charlogin secure illegal"` and `break`s; the name branch logs `"err,charlogin mobname empty"` and `return TRUE`s. None of them sends `_MSG_DBCharacterLoginFail` — that constant exists (`Code/Basedef.h:1116`), TMSrv implements a handler for it (`ProcessDBMessage.cpp:1084-1096`), but no producer exists in DBSrv (zero hits for `CharacterLoginFail` under `Code/DBSrv`). The consequence is analysed in BR-13.

A fourth, undocumented behaviour lives in the same branch: the DB writes the response field-by-field from `pAccountList[Idx].File` without confirming that the account list entry is the one belonging to this connection. If the account was removed between the account-login confirmation and the character login (for example by `_MSG_DBNoNeedSave` from a prior `CloseUser`), `pAccountList[Idx]` holds a stale or zeroed record and the branch will happily emit a confirmation built from it.

**Rule workflow**:
```
Idx = GetIndex(conn, m->ID)
if (Slot < 0 || Slot >= MOB_PER_ACCOUNT)      -> log; break  (no reply)
if (SecurePass == -1)                          -> log; break  (no reply)
Slot stored on account entry
build MSG_CNFCharacterLogin (mob, Donate, ShortSkill, affect, mobExtra)
if (Char[Slot].MobName[0] == 0)                -> log; return (no reply)
send _MSG_DBCNFCharacterLogin
ranking bookkeeping (grind ranking conn id + individual rank push)
```

---

### Business Rule: BR-11 World placement policy

**Overview**:
The spawn coordinate of a logging-in character is computed from four successive policies: city spawn from the merchant byte, guild-zone spawn for a charged guild, an unconditional newbie spawn for low-level mortals, and finally a spiral search for a free grid cell.

**Detailed description**:
Placement happens in the DB dispatcher, not in this component, but it is the observable purpose of the handshake and is therefore recorded here. After `pMob[conn].MOB = m->mob` (`:733`) the code computes `CityID = (pMob[conn].MOB.Merchant & 0xC0) >> 6` (`:851`) and seeds the position at `g_pGuildZone[CityID].CitySpawnX + rand() % 15`, `CitySpawnY + rand() % 15` (`:853-854`). `CityID` is a two-bit field, so it addresses at most four of the five zone records (`MAX_GUILDZONE` is 5, `Code/Basedef.h:148`). It then validates the class (`:857-866`) and, if the character's guild is the charged guild of any zone, replaces the position with that zone's `GuildSpawnX/GuildSpawnY` (`:870-879`) — note this override drops the random jitter used by the city path. Next, any character with `BaseScore.Level < FREEEXP` and `ClassMaster == MORTAL` is moved to `(2112 + rand()%5 - 2, 2042 + rand()%5 - 2)` (`:881-885`), i.e. the newbie area, overriding both previous policies. Finally `GetEmptyMobGrid(conn, &tx, &ty)` (`:887`) may adjust the coordinates by up to three cells in each direction to find a cell that is unoccupied (`pMobGrid[y][x] == 0`) and not a blocked height tile (`pHeightGrid != 127`) — see `Code/TMSrv/GetFunc.cpp:1901-1970+`.

Two anomalies are worth recording. The newbie rule is unconditional with respect to the merchant/guild result, so a guild member below `FREEEXP` is always sent to the newbie area rather than to the guild spawn, which may or may not be intended. And `GetEmptyMobGrid` contains an inner test `if (pHeightGrid[*ty][*tx] == 127) continue;` that consults the *original* candidate rather than the candidate being examined `(x, y)` (`GetFunc.cpp:1929`, `:1949`), so the height test is effectively evaluated once for the whole search.

Failure of the search is terminal: `ret == 0` causes `Log("Can't start can't get mobgrid")` and `CloseUser(conn)` (`:889-896`). Because the login was already half-completed at that point (`pMob[conn].MOB` populated, `Mode` still `USER_CHARWAIT`), the close path emits `_MSG_DBNoNeedSave` and frees the account entry, so the client is dropped at the moment of entering the world with a server-side log but no client-visible message.

**Rule workflow**:
```
tx,ty = g_pGuildZone[(Merchant & 0xC0) >> 6].CitySpawn + rand()%15
if (Class < 0 || Class > MAX_CLASS-1) -> log; CloseUser; break
for each zone: if (Guild != 0 && Guild == ChargeGuild) -> tx,ty = zone GuildSpawn
if (Level < FREEEXP && ClassMaster == MORTAL) -> tx,ty = newbie (2112,2042) +-2
if (!GetEmptyMobGrid(conn,&tx,&ty)) -> log; CloseUser; break
pMob.TargetX/LastX/TargetY/LastY = tx,ty ; sm.PosX/PosY = tx,ty
pMobGrid[ty][tx] = conn
```

---

### Business Rule: BR-12 Post-confirmation character normalisation

**Overview**:
Before the client is told it is in the world, the character record is repaired and bounded by a fixed set of invariants: HP floor, carry capacity, kill-mark item, effect normalisation and per-session counter resets.

**Detailed description**:
The dispatcher rewrites three categories of data. Category one is data repair of legacy content: for every equipped and carried item whose `nPos` is 64 or 192, effects `EF_DAMAGE2`/`EF_DAMAGEADD` are renamed to `EF_DAMAGE` (`:654-696`), and if the global `evDelete` event flag is set, carry items in the 470..500 id range are wiped (`:698-705`). Category two is authoritative state that overrides whatever the file said: `mob.CurrentScore.Hp <= 0` becomes `2` (`:730-731`), `pMob[conn].MaxCarry` is reset to `30` plus `15` for each of carry slots 60 and 61 holding item 3467 (`:751-757`), a Kill Mark item (id 547 with effects `EF_CURKILL`, `EF_LTOTKILL`, `EF_HTOTKILL`) is injected into `Carry[KILL_MARK]` when empty (`:809-817`), and class-master face/effect fix-ups are applied to `Equip[0]` per class (`:706-726`). Category three is per-session reset: message/potion/item timers, party request, quest flag, trade and autotrade position arrays (`-1` filled), PK and trade modes, ranking targets, castle status, chat history and error counters (`:735-845`).

The HP floor of 2 is the only guard against a dead character entering the world; there is no analogous floor for MP, and no validation that `CurrentScore` is consistent with `BaseScore` before `BASE_GetHpMp`/`GetCurrentScore` recompute derived values (`:773-780`). The carry capacity rule is item-driven and hardcoded to two slot indices and one item id, which makes capacity a content-editable property only by editing code. The Kill Mark injection is a "free item" grant executed on every login where the slot is empty, so deleting the item and relogging restores it.

Effect normalisation at `:664-693` is a silent server-side rewrite of player data: the client is given the corrected effects in the confirmation payload (`sm.mob` is copied after the fix-ups, `:784`), but the on-disk record is not rewritten here (writes happen later through the save pipeline). A player moving between servers with different content tables can therefore observe effects mutating on each login.

**Rule workflow**:
```
CNF received -> memcpy to client view
for Equip/Carry items with nPos 64 or 192: EF_DAMAGE2|EF_DAMAGEADD -> EF_DAMAGE
if (evDelete) drop carry items 470..500
if (ClassMaster == MORTAL|ARCH|CELESTIAL*): patch Equip[0] effects 98/106
Donate = m->Donate ; if (Hp <= 0) Hp = 2 ; pMob.MOB = m->mob
reset counters (Message, UseItemTime, PotionTime, LastReqParty, ProcessorCounter, QuestFlag)
MaxCarry = 30 (+15 if Carry[60]==3467) (+15 if Carry[61]==3467)
BASE_GetHpMp ; GetCurrentScore ; GetGuild ; bonus points
if (Carry[KILL_MARK].sIndex == 0) grant item 547 + kill effects
```

---

### Business Rule: BR-13 Failure handling and recovery

**Overview**:
The component has one intended negative acknowledgement (DB character-login failure) and one implicit one (idle reaper); neither covers the failures that actually occur.

**Detailed description**:
The designed failure path is `_MSG_DBCharacterLoginFail` → `SendClientSignal(conn, 0, _MSG_CharacterLoginFail)` (281 / 0x0119) → `pUser[conn].Mode = USER_SELCHAR` → `CrackLog(conn, " CharLoginFail")` (`ProcessDBMessage.cpp:1084-1096`). Restoring `USER_SELCHAR` is the correct recovery: the client may pick another slot without re-authenticating. However no DB code emits this message, so the branch is unreachable in practice.

The failures that do occur are handled silently: (a) the DB's three rejections return without a reply, leaving the session in `USER_CHARWAIT` with no client feedback; (b) a rejected slot at the TMSrv layer produces a message but no mode change, which is correct but gives no indication of *why* the slot was refused; (c) a non-`USER_SELCHAR` attempt produces the `"Wait a moment."` message and an audit line; (d) a placement failure closes the connection silently. Recovery from (a) depends on `CheckIdle` closing the session after 720 s of inactivity (`Server.cpp:4304-4322`), which a pinging client can postpone indefinitely.

There is also no compensation for the reservation made in BR-08. If the DB never answers, `pMob[conn].Mode` stays `MOB_USER` and `pUser[conn].Mode` stays `USER_CHARWAIT` until the reaper fires; `CloseUser` then takes the non-play branch, emits `_MSG_DBNoNeedSave` and resets both (`Server.cpp:6852-6867`). Because that branch runs for any mode other than `USER_PLAY`/`USER_SAVING4QUIT`, the cleanup itself is correct — the gap is purely the missing client-visible error and the missing server-side timeout.

**Rule workflow**:
```
_MSG_DBCharacterLoginFail (never produced):
    ID = ESCENE_FIELD ; SendClientSignal(conn, 0, _MSG_CharacterLoginFail)
    Mode = USER_SELCHAR ; CrackLog(conn, " CharLoginFail")

DB rejection without reply:
    session stays USER_CHARWAIT ; pMob stays MOB_USER
    ... up to 720 s idle (or indefinite while the client pings) ...
    CheckIdle -> CloseUser -> _MSG_DBNoNeedSave -> slot freed
```

---

## 4. Component Structure

```
Code/TMSrv/
├── _MSG_CharacterLogin.cpp          # COMPONENT BODY - 110 lines / 3,338 bytes
│                                    #   :19   includes ProcessClientMessage.h
│                                    #   :21   void Exec_MSG_CharacterLogin(int conn, char *pMsg)
│                                    #   :23   cast pMsg -> MSG_CharacterLogin*
│                                    #   :25   BR-01 slot domain check
│                                    #   :31   BR-02 billing shortcut -> goto Label1
│                                    #   :34   BR-03 billing state ladder (3 / 4 / other)
│                                    #   :51   Label1 (jump target inside the else block)
│                                    #   :53   BR-05 dead celestial block
│                                    #   :69   BR-06 child-account hour gate
│                                    #   :71   BR-07 session-mode gate
│                                    #   :73   BR-09 re-address to _MSG_DBCharacterLogin
│                                    #   :76   BR-08 session + mob reservation
│                                    #   :81   DBServerSocket.SendOneMessage
│                                    #   :83   "Wait a moment." + audit log
│                                    #   :85   rejection message
│                                    #   :90   _NN_Child_Pay rejection
│                                    #   :109  return
├── ProcessClientMessage.cpp         # dispatcher: entry :72-74 ; guards :42-64 ; includes :19-36
├── ProcessClientMessage.h           # declaration :78
├── ProcessDBMessage.cpp             # DB counterpart: branch :635-1081 ; fail branch :1084-1096
│                                    #   also: account-login confirmation :353-472 that seeds
│                                    #   SelChar (:440), Unk_1816 (:439), Unk_2732 (:450)
├── CUser.h                          # session object: Mode :45, Unk_1816 :76, SelChar :77,
│                                    #   Unk_2728 :99, Unk_2732 :100, Slot :43 ; mode constants :26-37
├── CMob.h                           # MOB_EMPTY :26, MOB_USER :28
├── Server.h                         # globals + declarations (DBServerSocket :224, SecCounter :329,
│                                    #   g_Hour :244, BILLING :276, IsFree/SendBilling :78-79)
├── Server.cpp                       # FREEEXP :43, BILLING :320, CHARSELBILL :321, DBServerSocket :262,
│                                    #   g_Hour :288, temp :277, SecCounter :384, config load :896-925,
│                                    #   IsFree :1363-1375, SendBilling stub :1377-1380,
│                                    #   CloseUser :6834+, CheckIdle :4304-4322, ProcessBILLMessage :4300-4302,
│                                    #   crack scoring :684-707, audit log :6701, CharLogOut :7085-7125
├── SendFunc.cpp                     # SendClientMessage :27-60, SendClientSignalParm :208-220
├── ProcessSecMinTimer.cpp           # SecCounter tick :123 ; child-account sweep :169-199 ;
│                                    #   per-user flush :201-221 ; CheckIdle call :1179-1182
├── GetFunc.cpp                      # GetEmptyMobGrid :1901-1970+
└── Language.h                       # message ids: _NN_SelectCharacter :54, _NN_Wait_Checking_Billing :58,
                                     #   _DN_Not_Allowed_Account :39, _NN_Using_Other_Server_Group :40,
                                     #   _NN_Child_Pay :206, _NN_Your_Account_From_Others :134

Code/
├── Basedef.h                        # MSG_CharacterLogin :1336-1341 ; _MSG_CharacterLogin :1335 ;
│                                    #   _MSG_CNFCharacterLogin :1344-1367 ; MSG_CNFClientCharacterLogin :1368-1386 ;
│                                    #   _MSG_DBCharacterLogin :979 ; _MSG_DBCNFCharacterLogin :1111 ;
│                                    #   _MSG_DBCharacterLoginFail :1116 ; _MSG_CharacterLoginFail :1392 ;
│                                    #   _MSG macro :925-931 ; MOB_PER_ACCOUNT :71 ; MAX_USER :56 ;
│                                    #   FREEEXP default not here (Server.cpp) ; message table extern :2461
├── Basedef.cpp                      # BASE_CheckPacket stub :6475-6596 ; g_pMessageStringTable :742
├── CPSock.h                         # framing constants :35-50 ; class :53-86
├── CPSock.cpp                       # ReadMessage :353-467 ; AddMessage :513-591 ; SendMessageA :617+ ;
│                                    #   SendOneMessage :686-694
├── DBSrv/CFileDB.cpp                # _MSG_DBAccountLogin :611-854 ; _MSG_DBCharacterLogin :1028-1094 ;
│                                    #   duplicate-login policy :685-703 ; server-change bypass :664-675, :802-853 ;
│                                    #   _MSG_AccountSecure / SecurePass :1357-1419 ; DBGetSelChar :2611-2631 ;
│                                    #   GetIndex :2328-2340 ; _MSG_DBNoNeedSave :1096+
└── DBSrv/CFileDB.h                  # API declarations :63-75
```

Ownership boundary: only `Exec_MSG_CharacterLogin` is owned by this component. The file contains no state, no helpers and no statics; the whole component is one translation unit with one external symbol, declared in `ProcessClientMessage.h:78`, listed in the project at `TMSrv.vcxproj:119`, and placed by MSVC solution layout in the `MSG\USER_SELCHAR` filter (`TMSrv.vcxproj.filters:111-113`) — the same filter as `_MSG_AccountSecure.cpp`, `_MSG_CharacterLogout.cpp`, `_MSG_CreateCharacter.cpp` and `_MSG_DeleteCharacter.cpp`, which confirms the character-selection scope of the component from the build metadata as well.

---

## 5. Dependency Analysis

### 5.1 Internal (compile-time) dependencies

```
Exec_MSG_CharacterLogin            (Code/TMSrv/_MSG_CharacterLogin.cpp:21)
  ├─ MSG_CharacterLogin / _MSG_DBCharacterLogin        (Code/Basedef.h:1336, :979)
  ├─ MOB_PER_ACCOUNT, FREEEXP, USER_SELCHAR,
  │  USER_CHARWAIT, MOB_USER                           (Basedef.h:71, Server.cpp:43, CUser.h:30-31, CMob.h:28)
  ├─ SendClientMessage                                  (SendFunc.cpp:27)
  ├─ SendClientSignalParm                               (SendFunc.cpp:208)
  ├─ pUser[conn].cSock.SendMessageA                     (CPSock.cpp:617)
  ├─ SendBilling                                        (Server.cpp:1377)
  ├─ DBServerSocket.SendOneMessage                      (CPSock.cpp:686 + Server.cpp:262)
  ├─ Log                                                (Server.cpp:6701)
  ├─ sprintf                                            (MSVCRT / UCRT)
  └─ data: pUser[] (CUser), pMob[] (CMob), temp[4096], g_pMessageStringTable[2000][128],
           BILLING, FREEEXP, g_Hour, SecCounter
```

Include chain: `_MSG_CharacterLogin.cpp:19` includes only `ProcessClientMessage.h`, which transitively pulls in `Windows.h`, `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`, `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h`, `CMob.h`, `CUser.h`, `CNPCGene.h`, `CReadFiles.h`, `CCastleZakum.h` and `CWarTower.h` (`ProcessClientMessage.h:21-43`). There is no module-level abstraction: the handler is coupled directly to five global arrays and four mutable globals.

### 5.2 Internal (runtime / message) dependencies

```
Client  --_MSG_CharacterLogin(531)-->  ProcessClientMessage  -->  Exec_MSG_CharacterLogin
                                                                       |
                                                                       | _MSG_DBCharacterLogin(2052)
                                                                       v
                                                                  DBSrv (CFileDB)
                                                                       |
                                                                       | _MSG_DBCNFCharacterLogin(1047)
                                                                       v
                                                             ProcessDBMessage
                                                                       |
                                                                       | _MSG_CNFCharacterLogin(276)
                                                                       v
                                                                    Client
```

The DB leg is a single blocking write into a kernel socket buffer (`SendOneMessage` → `AddMessage` → `SendMessageA`); there is no request queue, no sequence number, no retry and no timeout. Correlation is by `ID` only, and the CNF branch does not validate `Mode`, so the runtime coupling is "fire and hope" in the strict sense.

### 5.3 External dependencies

| Dependency | Kind | Version / identity | Purpose | Evidence |
|---|---|---|---|---|
| DBSrv process (`DBServerSocket`) | Internal service over TCP | compiled from same repo | character data, PIN state, account lock | `Server.cpp:262`, `CPSock.cpp:686` |
| Billing server (`BillServerSocket`) | External service over TCP | address/port from `BillServer.inf`-style file + `BillServerPort` | parental/billing verdicts consumed by BR-03..BR-06 | `Server.cpp:252`, `:3603`, `:3731-3810`; `SendBilling` is a stub |
| Client (W2 game client) | External binary | protocol version checked at *account* login (`_MSG_AccountLogin.cpp:44`) | character-select UI, `_MSG_CharacterLogin` sender | `ProcessClientMessage.cpp:72` |
| `Language.txt` | Runtime asset | message table, 2,000 entries x 128 bytes | all player-facing text used here | `Basedef.cpp:742`, `:4637`; `MAX_STRING` `Basedef.h:150` |
| `gameconfig.txt` | Runtime config | line 6: `billmode`, `freeexp`, `charselbill`, `potioncount`, `partybounus`, `guildboard` | `BILLING`, `FREEEXP` | `Server.cpp:896-925` |
| `heightmap.dat` | Runtime asset | grid of `pHeightGrid` | placement validity test | `Server.cpp:7146-7156`, `GetFunc.cpp:1913` |
| Guild-zone table `g_pGuildZone[5]` | Runtime state | `STRUCT_GUILDZONE` | city/guild spawn coordinates | `Server.h:417`, `ProcessDBMessage.cpp:851-879` |

---

## 6. Afferent and Efferent Coupling

Measured over the whole repository (`Code/**`, ignoring `.agents`, `.codegraph`, `.opencode`, `openspec`). For C procedural code, the unit of coupling is the external symbol (function, global, type) at file/function level.

| Component (symbol / file) | Afferent (Ca) | Efferent (Ce) | Instability I = Ce/(Ca+Ce) | Criticality |
|---|---|---|---|---|
| `Exec_MSG_CharacterLogin` (`_MSG_CharacterLogin.cpp`) | 1 (`ProcessClientMessage.cpp:73`) | 14 | 0.93 | High (session entry gate) |
| `MSG_CharacterLogin` (struct) | 6 (TMSrv handler, DB requester, size validators, dispatcher contracts) | 0 | 0.00 | Medium |
| `ProcessClientMessage` (dispatcher) | 1 (`Server.cpp:4001`) | ~60 (one per handler) | 0.98 | Critical |
| `ProcessDBMessage` (`_MSG_DBCNFCharacterLogin` branch) | 1 (DB socket loop) | ~40 (Send*, CPSock, globals) | 0.98 | Critical |
| `CPSock` (class) | ~9 (pUser[], DBServerSocket, BillServerSocket, listeners, admin sockets) | ~20 (Winsock, crypto tables, globals) | 0.69 | Critical |
| `CUser` (class) | ~40 files reference `pUser[]` | ~10 | 0.20 | High |
| `SendClientMessage` | 671 call sites (lines containing) | 3 (`cSock.AddMessage`, globals, `MESSAGE_LENGTH`) | ~0.004 | High |
| `SendClientSignalParm` | 104 call sites | 2 | 0.02 | Medium |
| `Log` | 497 call sites | 3 (file handles, `time`, `SetWindowTextA`) | ~0.006 | High |
| `SendBilling` | 7 call sites | 0 (stub) | 0.00 | Low (inert) |
| `DBServerSocket.SendOneMessage` | 51 call sites | 3 (`AddMessage`, `SendMessageA`, Winsock) | 0.06 | High |
| `BILLING` (global) | 8 read sites, 5 write sites | 0 | n/a | High (policy switch) |
| `FREEEXP` (global) | 4 read sites, 3 write sites | 0 | n/a | High (policy threshold) |
| `g_Hour` (global) | 2 read sites, 1 declaration, 0 writes | 0 | n/a | High (dead policy input) |
| `Unk_1816` (field) | 5 read sites, 2 write sites | 0 | n/a | High |
| `Unk_2728` (field) | 2 read sites, 1 write site (zero) | 0 | n/a | High |
| `Unk_2732` (field) | 1 read site, 3 write sites | 0 | n/a | Medium |

Interpretation:

- **Efferent 14 is confirmed exactly** for the handler when counting *runtime* dependencies: `SendClientMessage`, `SendClientSignalParm`, `SendBilling`, `Log`, `DBServerSocket`, `pUser[]`, `pMob[]`, `temp`, `g_pMessageStringTable`, `FREEEXP`, `BILLING`, `g_Hour`, `SecCounter`, `CPSock::SendMessageA`. Five further symbols are compile-time only (`MOB_PER_ACCOUNT`, `USER_SELCHAR`, `USER_CHARWAIT`, `MOB_USER`, `_MSG_DBCharacterLogin`/`MSG_CharacterLogin`).
- **Afferent 1 is confirmed**: the only caller is the dispatcher case at `ProcessClientMessage.cpp:72-74`. The handler is never invoked by the DB loop, never re-entered, and has no other referencing symbol in the project.
- The handler is a **maximally unstable leaf** (I = 0.93): it depends on 14 things and nothing depends on it. That is acceptable for a command handler, but it means the component has no leverage — every policy in it can only be changed by editing it, and every policy it relies on lives in a global that something else may mutate while the handler is not looking (`BILLING`, `FREEEXP`, `g_Hour` are all mutated/never-mutated outside it).
- **The real blast radius is on the other side of the wire.** Deleting this component would not change what a player sees as much as one would expect, because the DB dispatcher branch (`ProcessDBMessage.cpp:635-1081`, ~450 lines) performs the heavy work. Conversely, any change to `MSG_CharacterLogin` or to `pUser[].SelChar` semantics propagates into six components simultaneously.
- **Hidden logical coupling** outranks declared coupling here: `pMob[conn].MOB.Merchant` is written at `_MSG_CharacterLogin.cpp:79` and read at `ProcessDBMessage.cpp:851` as a city selector; `SelChar.Score[Slot].Level` is read here and written at `Server.cpp:7115`; `pUser[].Slot` is read at `Server.cpp:6891`, `:7115` and written at `ProcessDBMessage.cpp:821`. None of these relationships is expressed by an interface.

---

## 7. Endpoints

This component exposes **no network endpoint** of its own: TMSrv is a raw-socket, length-prefixed binary protocol server with a single listening socket (`Server.cpp:4006-4030`); there is no REST, GraphQL, gRPC, HTTP or admin-protocol surface in this file. The component's only entry point is a **protocol message type** dispatched from a single in-process switch.

| Entry point | Kind | Wire type | Payload | Handler |
|---|---|---|---|---|
| Client character selection | Binary TCP message (client→game) | `_MSG_CharacterLogin` = 0x0213 (531) | `MSG_CharacterLogin` (20 bytes: Slot, Force) | `Exec_MSG_CharacterLogin` (`ProcessClientMessage.cpp:72-74`) |
| Outbound request | Binary TCP message (game→DB) | `_MSG_DBCharacterLogin` = 0x0804 (2052) | `MSG_CharacterLogin` (Slot reused, ID rebound) | `CFileDB.cpp:1028` |
| Expected response | Binary TCP message (DB→game) | `_MSG_DBCNFCharacterLogin` = 0x0417 (1047) | `MSG_CNFCharacterLogin` (character, affects, extra, donate) | `ProcessDBMessage.cpp:635` |
| Expected failure signal | Binary TCP message (DB→game) | `_MSG_DBCharacterLoginFail` = 0x041C (1052) - never produced | `MSG_STANDARD` | `ProcessDBMessage.cpp:1084` |
| Client confirmation | Binary TCP message (game→client) | `_MSG_CNFCharacterLogin` = 0x0114 (276) | `MSG_CNFClientCharacterLogin` (truncated CNF) | client binary |
| Client rejection | Message panel + optional signal | `_MSG_MessagePanel` = 0x0101 (257); raw signal `404` | `MSG_MessagePanel`, `MSG_STANDARDPARM` | `SendFunc.cpp:27`, `:208` |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| `DBServerSocket` → DBSrv | Internal service, outbound | character record retrieval + login authorisation | TCP, WSAAsyncSelect, length-prefixed encrypted frames | `MSG_CharacterLogin` mutated to `_MSG_DBCharacterLogin` | None. Fire-and-forget; `SendOneMessage` returns a BOOL that is ignored (`:81`); no timeout, no retry, no correlation token |
| DBSrv → TMSrv confirmation | Internal service, inbound | applies character to world | TCP, same framing | `MSG_CNFCharacterLogin` (multi-KB, dominated by `STRUCT_MOB`, 208-byte `unk`, 765-byte `Unk2`, 32 affects, `STRUCT_MOBEXTRA`) | No `Mode`/account validation; range-checked ID only (`ProcessDBMessage.cpp:43`); failures close the session on placement errors only |
| Client socket → handler | External, inbound | character slot selection | TCP, custom XOR/checksum obfuscation (`CPSock.cpp:430-466`) | `MSG_CharacterLogin` (20 bytes) | Envelope bounds only; per-type length check disabled project-wide (`Basedef.cpp:6475-6596`) |
| Handler → client (rejection) | External, outbound | user feedback | TCP, queued via `cSock.AddMessage` | `MSG_MessagePanel` (96-byte text), `MSG_STANDARDPARM` (signal 404) | Explicit flush only on the state-3 path (`:42`); otherwise the per-second pump flushes (`ProcessSecMinTimer.cpp:201-221`) |
| Billing server | External service | billing verdicts (`Unk_1816`) and child-account flag (`Unk_2728`) | TCP, 196-byte fixed records (`_AUTH_GAME`, `CPSock.h:88-91`, `:132`) | opaque 196-byte blob | `ProcessBILLMessage` is empty (`Server.cpp:4300-4302`); `SendBilling` is a stub (`:1377-1380`); reconnect policy in `ProcessSecMinTimer.cpp:130-167` |
| `Language.txt` message table | Runtime asset | all player-visible strings | file, tab/line separated | `char[2000][128]` loaded at boot (`Basedef.cpp:4637-4706`) | Missing file yields empty strings; no bounds check on the index at use site (constants are in range: 34, 38, 39, 40, 206) |
| `gameconfig.txt` | Runtime config | `BILLING`, `FREEEXP`, `CHARSELBILL` | text, strict format validation via `MessageBox` on mismatch (`Server.cpp:899-915`) | integers | Fail-fast `MessageBox` + `return` from `ReadConfig`, leaving defaults in place unless the process aborts |
| GM console (`imple.cpp`) | In-process command channel | live reconfiguration of `BILLING`, `FREEEXP`, `CHARSELBILL` | text commands `set billmode` / `billfree` / `charselbill` | integers | Prints confirmation; requires a live bill-server socket for modes 2/3 (`imple.cpp:591-606`) |
| `pMobGrid` / `pHeightGrid` | In-process shared state | spawn occupancy and terrain validity | arrays | `unsigned short[GRIDY][GRIDX]`, `char` height | `GetEmptyMobGrid` returns FALSE → `CloseUser` (`ProcessDBMessage.cpp:887-896`) |

---

## 9. Design Patterns & Architecture

| Pattern / decision | Implementation | Location | Purpose / assessment |
|---|---|---|---|
| Command Handler (message dispatch table) | `switch (std->Type)` with one function per wire type | `ProcessClientMessage.cpp:66-311` | Uniform, flat, O(1); the only extension point for new client messages. Type collisions are avoided by flag bits (`FLAG_CLIENT2GAME` etc.) rather than by value ranges |
| Session State Machine (implicit) | `CUser::Mode` with named constants and ad-hoc transitions | `CUser.h:26-37`; transitions at `_MSG_CharacterLogin.cpp:76`, `ProcessDBMessage.cpp:400`, `:782`, `:906`, `:1092`, `Server.cpp:7113` | State names are self-documenting but transitions are spread across 5 files with no table, no guard helper and no assertion; `USER_CHARWAIT` has no timeout |
| Two-phase commit handshake (request/confirm) | Forward the client frame, apply the reply later | `_MSG_CharacterLogin.cpp:73-81` → `ProcessDBMessage.cpp:635-1081` | Correct shape, but without a transaction id, sequence number or timeout. Liveness is delegated to the generic idle reaper |
| Optimistic concurrency guard | Mode check + immediate mode change before the async call | `_MSG_CharacterLogin.cpp:71-81` | Effectively a mutex on the session; works because the server is single-threaded |
| Defence in depth (duplicated validation) | Slot re-check and name check on the DB side | `CFileDB.cpp:1036-1078` | Good separation of trust domains; undermined by having no failure channel back to the client |
| ad-hoc Policy Flags as globals | `BILLING`, `FREEEXP`, `CHARSELBILL`, `g_Hour`, `Unk_1816/2728/2732` | `Server.cpp:43`, `:288`, `:320-321`; `CUser.h:76`, `:99-100` | No configuration object; policy is read directly from mutable globals at decision time, so behaviour can change mid-session from a GM command |
| God dispatcher / transaction script | All post-login business logic inline in one `case` (~450 lines) | `ProcessDBMessage.cpp:635-1081` | An anti-pattern for this domain: placement, content repair, ranking, guild mantle selection and event notices are interleaved in one block |
| Procedural globals (no DI, no interfaces) | `pUser[]`, `pMob[]`, `DBServerSocket`, `temp` | `Server.cpp:262`, `:277` | Typical of the decompiled 2000s code base; makes unit testing impossible without a global harness |
| Undocumented magic values | `404` signal, `999` level, `1000` level, `3467` item, `547` kill mark, `2112/2042` newbie coords, `0xC0` merchant mask | `_MSG_CharacterLogin.cpp:41`; `ProcessDBMessage.cpp:751-757`, `:809-817`, `:851`, `:881-885` | No named constants anywhere in the login path; every value is a knowledge-transfer hazard |

Architectural boundary assessment: the component sits between the "client protocol" layer and the "DB protocol" layer and belongs to neither. Its natural home would be a session/login service with an explicit state enum, a request table with timeouts, and a failure channel; the current design instead spreads that service across this handler, the DB dispatcher, `CUser` and two timer functions. Cohesion within the function is low (five unrelated concerns), and cohesion of the "login feature" across the code base is also low (four files, two processes). Coupling is high and entirely implicit (globals plus wire protocol).

---

## 10. Technical Debt & Risks

| Risk | Area | Issue | Impact |
|---|---|---|---|
| High | Handshake integrity | `_MSG_DBCNFCharacterLogin` applies a character to `conn` without checking `Mode == USER_CHARWAIT`, account name or requested slot (`ProcessDBMessage.cpp:635-792`) | A delayed, duplicated or replayed confirmation can materialise a character on a connection that never authenticated; `CloseUser` frees the slot immediately (`Server.cpp:6852-6867`) and `GetEmptyUser` reuses the lowest index (`Server.cpp:6743-6753`), so a session slot is a realistic reuse target |
| High | Failure handling | DBSrv never emits `_MSG_DBCharacterLoginFail` (0 producers); all three DB rejections are silent (`CFileDB.cpp:1036-1078`) while TMSrv implements a handler for the message (`ProcessDBMessage.cpp:1084-1096`) | Client hangs on the character-select screen with no message; session and mob slot stay reserved until the 720 s reaper (`Server.cpp:4304-4322`), which a pinging client can postpone forever (`ProcessClientMessage.cpp:56-60`) |
| High | Policy correctness | BR-05 is unreachable (`Level < 999 && Level >= 1000`, `_MSG_CharacterLogin.cpp:53-54`); BR-03/BR-04/BR-06 depend on `Unk_1816` states 2/3/4 and `Unk_2728 == 1` that no code produces (`CUser.h:76`, `:99`); `g_Hour` is never assigned (`Server.cpp:288`) | Four of thirteen business rules are dead; the billing/parental-control feature set is unimplemented despite appearing complete |
| High | Protocol validation | No per-type length check anywhere (`Basedef.cpp:6475-6596` returns FALSE unconditionally, and only under `#ifdef _PACKET_DEBUG`); handler reads `Slot`/`Force` without checking `m->Size` (`_MSG_CharacterLogin.cpp:23-31`) | Undefined-behaviour read of receive-buffer residue; the same project-wide gap enables the 8 KB send-buffer disclosure described in the sibling `Action Handler` report |
| High | Access control | Staff exemption at `Level >= 999` is a data-driven privilege (`_MSG_CharacterLogin.cpp:31`); no revalidation against `Admin` on the login path; `pUser[].Admin` is actually set with the wrong index (`ProcessDBMessage.cpp:1004-1005`) | A level field that can be edited in the account file bypasses billing; the intended admin flag is never granted, so admin paths are governed only by scattered level tests |
| Medium | Auth bypass route | The password-less "server change" path in DBSrv re-enters the character-login confirmation directly (`CFileDB.cpp:664-675`, `:802-853`) without passing through `Exec_MSG_CharacterLogin` | Slot/level/billing checks of this component are skipped for transfers; only `SecurePass = 1` is enforced |
| Medium | Identity/authorisation | The handler never compares the account name, slot name or IP against the DB record it is about to fetch; `MSG_CharacterLogin::Force` is dead | No defence against a client that selects a slot belonging to a different character of the same account than the UI suggested (harmless today because the DB uses the account list, but the field is unvalidated) |
| Medium | Race / state leakage | `pMob[conn].Mode = MOB_USER` set before confirmation (`:77`); no rollback on DB silence | Mob slot appears occupied while the session is stuck; NPC allocation skips index `conn` until cleanup |
| Medium | Timer arithmetic | `Unk_2732 < SecCounter - 10` with unsigned operands (`CUser.h:100`, `Server.cpp:384`) | Underflow during the first 10 s of uptime promotes the billing seat immediately, bypassing the intended grace period |
| Medium | Data integrity | `DBGetSelChar` writes `sel->SPX[i]` twice and never sets `SPY[i]` (`CFileDB.cpp:2623-2624`) | Saved-position data in the account snapshot is wrong; any consumer of `SelChar.SPY` sees the X coordinate (currently no consumer in this path) |
| Medium | Inconsistent policy | Login gate allows child accounts 08:00-22:59 (`:69`) while the sweep kicks them 00:00-12:59 and 19:00-23:59 (`ProcessSecMinTimer.cpp:178`) | Even if the flags were live, a child account could log in and be disconnected seconds later, 14 of 24 hours |
| Medium | Observability | Invalid-slot rejection produces no log and no crack accounting (`:25-29`), unlike sibling handlers; the mode-mismatch log omits the slot (`:86`) | Slot-probing attacks are invisible; failed logins cannot be reconstructed |
| Medium | Localisation | `"Wait a moment."` hardcoded (`:85`); `sprintf` uses table entries as format strings (`:38`, `:61`) | Inconsistent player experience; a message entry containing `%` specifiers would be interpreted (asset-controlled, not user-controlled) |
| Low | Control flow | `goto Label1` into a nested `else` (`:32` → `:51`); estimated cyclomatic complexity 21 in 90 lines; `Unk_1816` re-tested three times (`:36`, `:46`, `:56-66`) | High maintenance risk; equivalence of the branches is not evident from the shape of the code |
| Low | Dead protocol field | `MSG_CharacterLogin::Force` never read anywhere under `Code/` | Protocol documentation mismatch |
| Low | Struct truncation | CNF is copied into the smaller `MSG_CNFClientCharacterLogin` (`memcpy(&sm, m, sizeof(MSG_CNFClientCharacterLogin))`, `ProcessDBMessage.cpp:641`) | Intentional compatibility shim, but silent if either struct changes |
| Low | Duplicate guard | `if (conn == 0) { CrackLog(0, " CNFCharLogin"); CloseUser(conn); }` (`ProcessDBMessage.cpp:643-650`) is redundant with the `conn == 0` system-message switch above (`:56-186`) | Dead defensive code; shows uncertainty about the dispatcher contract |
| Low | Leaked loop variable | `pMob[n]` / `pUser[n].Admin = 1` with the guild-zone loop variable (`:868-879`, `:1004-1005`) | Wrong index; staff flag effectively never set (functional defect noted under High above) |
| Low | Build hygiene | No test project, no CI, no assertions, `SecCounter`-style globals mutated from GM console | Regression detection relies on manual play-testing |

---

## 11. Test Coverage Analysis

**Result: no automated test coverage exists for this component, and none exists for the project as a whole.** The following evidence is exhaustive within the analysed scope.

| Evidence | Command / artefact | Result |
|---|---|---|
| Test project in solution | `W2PP Code Project.sln` project list (lines 3-8) | Only `DBSrv` and `TMSrv` (plus a "Solution Items" folder). No test project |
| Test framework reference | `*.vcxproj`, `*.sln`, `*.props`, `*.targets` searched for `gtest`, `gmock`, `Catch2`, `cppunit`, `doctest`, `boost/test`, `Microsoft.VisualStudio.TestTools`, `NUnit` | Zero matches in TMSrv, DBSrv, ClientPatch_v7662 |
| Test / spec files | recursive search for `*test*`, `*Test*`, `*spec*` under the project root, excluding `.git`, `.codegraph`, `.opencode`, `.agents`, `openspec` | Zero files |
| Test directories | recursive search for `test`, `tests`, `__tests__`, `spec` directories | None |
| CI configuration | search for `.github`, `.gitlab-ci*`, `.circleci`, `ci` directories | None |
| Assertions | `grep "\bassert\s*\("` across `Code/**/*.cpp`, `Code/**/*.h` | Zero matches |
| Build configuration | `Code/TMSrv/TMSrv.vcxproj:4-8` | Only `Debug|Win32` and `Release|Win32`; no test/coverage configuration |

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `Exec_MSG_CharacterLogin` (`_MSG_CharacterLogin.cpp:21-110`) | 0 | 0 | 0% | No harness exists. Behaviour can only be exercised by running TMSrv + DBSrv + a client and driving the character-select UI |
| `MSG_CharacterLogin` parsing / BR-01 slot validation | 0 | 0 | 0% | Boundary values (`-1`, `0`, `3`, `4`, `INT_MIN`, `INT_MAX`) untested |
| BR-02..BR-06 eligibility ladder | 0 | 0 | 0% | Requires mutation of `BILLING`, `FREEEXP`, `g_Hour`, `Unk_1816`, `Unk_2728`, `Unk_2732`; no way to set the latter three states exists, so they cannot even be reached in a running build |
| BR-07/BR-08 session gating and reservation | 0 | 0 | 0% | Requires a race harness (duplicate `_MSG_CharacterLogin` frames) |
| BR-09 re-addressing / BR-10 DB re-validation | 0 | 0 | 0% | Would need DBSrv with a disposable account directory |
| BR-11..BR-12 placement and normalisation | 0 | 0 | 0% | Victim of the `pMob[n]` defect; no assertions exist to catch it |
| BR-13 failure and recovery | 0 | 0 | 0% | The intended failure message is never produced, so the branch cannot be exercised end-to-end even manually |

Observations on testability:

- **Static globals block unit testing.** `pUser`, `pMob`, `DBServerSocket`, `temp`, `g_pMessageStringTable`, `FREEEXP`, `BILLING`, `g_Hour`, `SecCounter` are all defined in `Server.cpp`/`Basedef.cpp` and referenced directly. A test binary would have to link the whole server or provide a full set of shadow definitions.
- **The decision inputs are unreachable.** Three of the handler's branching variables (`Unk_1816` values 2/3/4, `Unk_2728 == 1`, `g_Hour != 0`) cannot be produced through any production code path, so any test for those branches would have to write the fields directly — which would then pin dead behaviour.
- **The observable contract is on the wire.** The cheapest realistic verification is a packet-level harness that drives `ProcessClientMessage` with hand-built frames and inspects `DBServerSocket`'s send buffer. That harness does not exist.
- **Manual verification paths that do exist**: `Log` lines `"sta,Login char:%s exp:%llu level:%d conn:%d money:%d, store:%d"` (`ProcessDBMessage.cpp:1078`), `"err,charlogin not user_selchar %d %d"` (`_MSG_CharacterLogin.cpp:86`), `"err,charlogin slot illegal"` / `"err,charlogin secure illegal"` / `"err,charlogin mobname empty"` (`CFileDB.cpp:1038`, `:1047`, `:1075`) and `CrackLog(conn, " CharLoginFail")` (`ProcessDBMessage.cpp:1094`) provide the only post-hoc verification surface.

---

## 12. Analysis Notes, Confidence and Limitations

**Confidence levels for the extracted rules**

| Rule | Confidence | Basis |
|---|---|---|
| BR-01 | High | Explicit predicate with named constants; bound cross-checked against 5 other sites |
| BR-02 | High | Explicit predicate; `FREEEXP`/`BILLING` origins traced to config and GM console |
| BR-03 | High (behaviour of the branches) / High (unreachability) | Branch bodies are explicit; unreachability established by exhaustive search for writers of the state variable |
| BR-04 | High (mechanics) / Medium (intent) | Mechanics explicit; the three-phase client-retry protocol is inferred from the state values and is not documented in the code |
| BR-05 | High | Unsatisfiable conjunction is a mathematical fact |
| BR-06 | Medium | The predicate is explicit, but the intended hour semantics are inferred: `g_Hour` has no assignment anywhere, so "hour of day" is an assumption supported only by the sibling policy in `ProcessSecMinTimer.cpp:178` and by the castle code using `timeinfo->tm_hour` |
| BR-07 | High | Explicit |
| BR-08 | High | Explicit writes; the absence of a timeout is established by exhaustive search for `USER_CHARWAIT` (1 hit, the write itself) |
| BR-09 | High | Explicit; copy semantics of `CPSock::AddMessage` verified at `CPSock.cpp:513-591` |
| BR-10 | High | Explicit; `SecurePass` lifecycle traced across 10 sites |
| BR-11 | High | Explicit; ordering and overrides verified line by line |
| BR-12 | High | Explicit; constants (`3467`, `547`, `30`, `15`) enumerated |
| BR-13 | High | Producer of `_MSG_DBCharacterLoginFail` established absent by search over `Code/DBSrv` |

**Deliberate limitations**

- No code was executed, compiled or instrumented; all statements about runtime behaviour are static inferences. Packet sizes quoted as exact (12-byte header, 20-byte request, 52-byte action frame elsewhere) are derived from `#pragma pack`-free struct layouts on Win32 (`TMSrv.vcxproj` targets Win32 only), matching the sibling reports' convention. The CNF payload is described qualitatively rather than numerically because it aggregates `STRUCT_MOB`, `STRUCT_MOBEXTRA`, 32 affects and three fixed arrays.
- Runtime assets (`Language.txt`, `gameconfig.txt`, height map, guild-zone table) are not present in the repository, so the deployed values of `BILLING`, `FREEEXP`, `CHARSELBILL` and the actual player-facing strings could not be read. Conclusions about policy therefore describe the code's semantics for a given configuration, and the shipped defaults where the config is absent.
- The billing server protocol (`_AUTH_GAME`, 196 opaque bytes) is undocumented and its consumer is empty; the intended assignment of `Unk_1816` values is inferred from the *reader* in this handler, not from any writer.
- The distinction between "decompilation artefact" and "deliberate design" (notably the dead `:53-67` block and the `goto` structure) cannot be settled from the code; the report records reachability facts and leaves intent open.
- No modification, refactoring or remediation is proposed, in line with the analysis-only scope.
