# Component Deep Analysis Report — Admin Command Interpreter

| Field | Value |
|-------|-------|
| Component | Admin Command Interpreter (`ProcessImple` + `SaveAll`) |
| Primary location | `Code/TMSrv/imple.cpp` (46,572 bytes, 1,846 lines) |
| Declared in | `Code/TMSrv/Server.h:190` (`void ProcessImple(int conn, int level, char * str);`), `Code/TMSrv/Server.h:169` (`void SaveAll();`) |
| Host process | TMSrv (game/world server, Win32 GUI application) |
| Language / toolchain | C++ (MSVC), Win32 API, no external runtime |
| Repository | W2PP — decompiled W2 MMORPG server emulator |
| Branch / commit | `feat/milestone1` / `bbf8d36cdfcb94f59132e1911756baef8ce65c03` (2026-11-15 22:06:44 -0300) |
| Analysis date | 2026-11-15 22:46:03 |
| Excluded paths | `.agents`, `.codegraph`, `.opencode`, `openspec` (not referenced by the analysis) |

---

## 1. Executive Summary

The Admin Command Interpreter is the single most privileged in-process surface of TMSrv. It is a monolithic, single-function command dispatcher (`ProcessImple`) that parses a whitespace-separated command line coming either from an in-game whisper (`/gm <body>`) or from a database-server message (`_MSG_MessageDBImple`), and then executes one of **65 top-level commands** with **60 additional `set` sub-commands** against process-global game state: player slots (`pMob[]`), account slots (`pUser[]`), world items (`pItem[]`), the mob grid (`pMobGrid[][]`), NPC generator tables (`mNPCGen`), economy/drop configuration (`g_pDropBonus`, `g_pTreasure`, `g_pGuildZone`), event configuration (`ev*`), billing state (`BILLING`, `FREEEXP`, `CHARSELBILL`), logging, NPC files on disk (`./npc/*`) and the billing/DB socket connections.

The component has no header of its own; it is a single translation unit exposing two symbols declared in `Server.h`. It depends on 51 distinct project-level functions, the C runtime file API, and roughly 28 mutable global objects. Afferent coupling is 2 (whisper handler at `Code/TMSrv/_MSG_MessageWhisper.cpp:791` and DB message handler at `Code/TMSrv/ProcessDBMessage.cpp:132`), efferent coupling is 51+ — an instability index of roughly 0.96. It is the archetype of a "god function": one 1,800-line body, 9 stack string buffers of 128 bytes, 9 parallel parsed integers, and a single shared global `temp[4096]` buffer (`Code/TMSrv/Server.h:382`) reused by nearly every branch.

The authorization model is the most important finding. There is no capability check, no role table, no account/IP allow-list, and no per-command ownership check inside the component. Privilege is a single integer computed by the caller as `pMob[conn].MOB.CurrentScore.Level - 1000` (`Code/TMSrv/_MSG_MessageWhisper.cpp:789`), i.e. an in-game character level offset. The component then uses four numeric thresholds (8, 3, 2, 1) that map to character levels 1008, 1003, 1002 and 1001. Because the character level is itself writable by the highest-privilege command (`set level`), and because `pUser[].Admin` is set at login (`Code/TMSrv/ProcessDBMessage.cpp:1004-1005`) but never read anywhere in the codebase, the intended admin flag is dead code and the effective authority is an ordinary, persisted game statistic.

Three secondary findings dominate the risk profile. First, the intended connection-bound guard at `imple.cpp:91` (and four copies at 1051, 1067, 1566, 1832) is the tautology `conn <= 0 && conn >= MAX_USER`, which can never be true, so the component writes through `pMob[0]` on the DB path (`conn == 0`) and has no defense against an out-of-range `conn`. Second, several commands perform unvalidated indexing into fixed-size global arrays (`destroygate`, `cgate`, `ogate`, `ngate` index `pItem[ival2]` with `ival2` up to 4,294,967,295 against `MAX_ITEM = 5000`) and an unbounded `strcpy` into a 16-byte name field (`read`, `name`). Third, the privileged file commands (`save`, `read`) concatenate an unvalidated argument onto `./npc/` and raw-read/raw-write `sizeof(STRUCT_MOB)` bytes, providing arbitrary-path file write and file read at level 8.

Operationally, the component also carries the server's lifecycle levers: `reboot` / `delayreboot` drive the global `ServerDown` countdown that blocks new connections and closes users one by one, `saveall` force-logs-out every playing character to persist them, `timer` retimes the minute heartbeat, and `log` rotates the three log files. Nothing in the component is covered by an automated test; the solution contains exactly two projects (DBSrv, TMSrv) and no test target at all.

---

## 2. Data Flow Analysis

### 2.1 Primary flow — in-game whisper path

```
1. Client sends _MSG_MessageWhisper to TMSrv (MobName = "gm" or "GM", String = "+<command> <args>")
2. Exec_MSG_MessageWhisper (Code/TMSrv/_MSG_MessageWhisper.cpp:20) validates pUser[conn].Mode == USER_PLAY
3. Branch at line 787 matches "gm"/"GM"; computes level = pMob[conn].MOB.CurrentScore.Level - 1000
4. ProcessImple(conn, level, m->String) is called (line 791)
5. ProcessImple parses str+1 through sscanf into cmd / sval1..sval8 (128-byte stack buffers)
6. Numeric mirror parse: ival1..ival8 (%d) plus dval2 (%llu) from the same tokens
7. Audit log is emitted unconditionally: Log("adm " + str, account, IP)   [before any authorization]
8. Four independent level gates are evaluated in order: >=8, >=3, >=2, >=1, >=1
9. Matching branch mutates global state (pMob/pUser/pItem/pMobGrid/mNPCGen/ev*/g_p*)
10. Downstream notifications: SendClientMessage, SendItem, SendScore, SendEtc, SendEquip,
    GetCreateMob + GridMulticast, GuildZoneReport, DBServerSocket.SendOneMessage
11. Optional configuration persistence: DrawConfig(TRUE) rewrites gameconfig.txt
12. Optional file artefacts: ./npc/<arg> (save/read/svexp/waterexp/nigexp), ../../DBSRV/Run/Admin.txt (npko)
```

### 2.2 Secondary flow — database-server path

```
1. An admin client connected to DBSrv (authorized by IP against Admin.txt -> pAdminIP) sends a
   _MSG_MessageDBImple packet (Level = arbitrary int supplied by the client, String = "+cmd args")
2. DBSrv routes the packet (Code/DBSrv/CFileDB.cpp:592-609): forces ID = 0, broadcasts to every
   connected game server
3. TMSrv ProcessDBMessage (Code/TMSrv/ProcessDBMessage.cpp:39) verifies FLAG_DB2GAME and 0 <= ID < MAX_USER
4. conn == 0 branch matches _MSG_MessageDBImple (line 123): clamps String to MESSAGE_LENGTH-2,
   logs it, then calls ProcessImple(0, m->Level, m->String) (line 132)
5. ProcessImple proceeds as above with conn = 0, which means:
   - the audit log is written as "system" with IP 0 (line 87)
   - every pMob[conn] / pUser[conn] access addresses slot 0
   - SendClientMessage(0, ...) is a no-op (Code/TMSrv/SendFunc.cpp:29-30)
   - "save" and "read" are unreachable because they require conn != 0
```

### 2.3 State mutated along the flow

| State object | Declared at | Mutated by |
|---|---|---|
| `pMob[]` (STRUCT_MOB, equipment, carry, affect, extra) | `Code/TMSrv/Server.h` / `Code/TMSrv/CMob.h:40` | `set *`, `cp`, `frag`, `learn`, `class`, `buff`, `nobuff`, `soul`, `gift`, `read`, `kick`, `killkefra`, `kill` |
| `pUser[]` (Mode, MuteChat, IP, AccountName, Admin) | `Code/TMSrv/CUser.h` | `mute`, `muteall`, `desmuteall`, `kick`, `saveall`, `npko` |
| `pItem[]` (CItem, MAX_ITEM = 5000) | `Code/TMSrv/CItem.h:45`, `Code/Basedef.h:103` | `set gate`, `destroygate`, `cgate`, `ogate`, `ngate`, `mgate`, `closearmia`, `openarmia` |
| `pMobGrid[4096][4096]` (unsigned short) | `Code/TMSrv/Server.h:309` | `testegrid` |
| `mNPCGen` (generator table, MAX_NPCGENERATOR = 8192) | `Code/TMSrv/CNPCGene.h` | `generate`, `create`, `createkefra`, `reloadnpc`, `rebuild`, `waterexp`, `svexp`, `nigexp` |
| `g_pDropBonus[64]` | `Code/Basedef.cpp:204` | `set drop` |
| `g_pTreasure[8]` | `Code/TMSrv/Server.cpp:273` | `trtarget`, `trsource`, `trrate` |
| `g_pGuildZone[5]` | `Code/…` (`MAX_GUILDZONE` = 5) | `set champ`, `set chall` |
| `ev*` (8 event ints) | `Code/TMSrv/Server.cpp:299-307` | `set evstart/evend/evitem/evrate/evindex/evdelete/evon/evnotice` |
| Economy globals | `Code/TMSrv/Server.cpp:43-340` | `set billmode/billfree/charselbill/potioncount/potiondelay/partybonus/guildboard`, `set double`, `set deadpoint`, `set dungeonevent`, `partydif`, `rvrhour`, `gtorrehour`, `dropitem`, `maxnightmare`, `statsapphire`, `battleroyal` |
| `ServerDown` lifecycle counter | `Code/TMSrv/Server.cpp:46` | `reboot`, `delayreboot` |
| `pAdminIP[10]` | `Code/TMSrv/Server.cpp:264` | `npko` |
| Files on disk | `./npc/*`, `gameconfig.txt`, `../../DBSRV/Run/Admin.txt` | `save`, `read`, `svexp`, `waterexp`, `nigexp`, `DrawConfig`, `npko` |
| Sockets | `BillServerSocket`, `DBServerSocket` | `billconnect`, `notice`, `fame` |

---

## 3. Business Rules & Logic

### Overview of the business rules

#### 3.1 Authorization and framing rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Framing | The first byte of the command string is always discarded; parsing starts at `str + 1`, so the wire body must be `+<command> <args>` | `Code/TMSrv/imple.cpp:68` |
| BR-02 | Parsing | Exactly 9 whitespace tokens are extracted into 128-byte buffers with unbounded `%s`; missing tokens leave the buffers with stale stack content | `Code/TMSrv/imple.cpp:48-68` |
| BR-03 | Parsing | Every argument is parsed twice: `%d` into `unsigned int` mirror variables and, for token 2 only, additionally `%llu` into `long long dval2` | `Code/TMSrv/imple.cpp:70-78` |
| BR-04 | Audit | Every invocation is logged as `adm <string>` **before** any level check, with account name and IP, or `"system"`/IP 0 when conn == 0 | `Code/TMSrv/imple.cpp:80-87` |
| BR-05 | Authorization (whisper) | Authority = `pMob[conn].MOB.CurrentScore.Level - 1000`; no account, role, or IP check exists | `Code/TMSrv/_MSG_MessageWhisper.cpp:787-792` |
| BR-06 | Authorization (DB) | Authority = the `Level` field of `_MSG_MessageDBImple`, taken at face value from the DB connection; `conn` is forced to 0 | `Code/TMSrv/ProcessDBMessage.cpp:123-134` |
| BR-07 | Authorization tiers | Effective tiers are `level >= 8`, `>= 3`, `>= 2`, `>= 1` (character levels 1008+ / 1003+ / 1002+ / 1001+) | `Code/TMSrv/imple.cpp:89, 569, 647, 1119, 1164` |
| BR-08 | Guard (defective) | The intended connection guard is the tautology `if (conn <= 0 && conn >= MAX_USER) return;`, which never fires (5 occurrences) | `Code/TMSrv/imple.cpp:91, 1051, 1067, 1566, 1832` |
| BR-09 | Contract | Unknown command or unmet level produces no response at all (silent failure); the audit log is the only trace | `Code/TMSrv/imple.cpp:46-1846` |
| BR-10 | Concurrency | The component relies on the single-threaded Win32 message pump and the shared global `temp[4096]`; no reentrancy protection exists | `Code/TMSrv/Server.h:382`, `Code/TMSrv/imple.cpp` (all branches) |

#### 3.2 Level 8 commands (character level >= 1008) — state mutation and privileged file I/O

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-11 | Business Logic | `set sanc <v>` overwrites effect slot 0 of all 8 equipped items with `EF_SANC` / value `<v>` | `imple.cpp:96-107` |
| BR-12 | Business Logic | `set clearquest` zeroes `MOB.Quest`; `set clearskill` zeroes `MOB.LearnedSkill` | `imple.cpp:113-122` |
| BR-13 | Validation | `set level <n>` sets `BaseScore.Level`, clamping only the upper bound to 2010; no lower bound | `imple.cpp:123-130` |
| BR-14 | Business Logic | `set hp/mp <n>` writes BaseScore and CurrentScore MaxHp/Hp (MaxMp/Mp) simultaneously | `imple.cpp:131-142` |
| BR-15 | Business Logic | `set ac/dam/str/int/dex/con <n>` writes a single BaseScore field (silently truncated to field width) | `imple.cpp:143-179` |
| BR-16 | Business Logic | `set exp <n>` writes `MOB.Exp` (64-bit), pushes ETC, then runs a single `CheckGetLevel()` | `imple.cpp:153-159` |
| BR-17 | Business Logic | `set coin/merchant/skillbonus/scorebonus/special/classmaster <n>` writes economy/cosmetic fields | `imple.cpp:180-248` |
| BR-18 | Diagnostics | `set learned <n>` mutates nothing; it echoes `1 << (n % 24)` as a decimal string | `imple.cpp:230-237` |
| BR-19 | Operations | `set reloadfile` reloads base definitions, language table, sanctification/quest/comp rates and castle quest data | `imple.cpp:250-262` |
| BR-20 | Business Logic | `set gate <item> <x> <y> <rot>` creates a ground item and multicasts `_MSG_CreateItem` with `ItemID = item + 10000` | `imple.cpp:264-297` |
| BR-21 | Business Logic | `set destroygate/cgate/ogate/ngate <idx>` mutate `pItem[idx]` and multicast the state change; `<idx>` is unvalidated | `imple.cpp:299-410` |
| BR-22 | Business Logic | `set mgate <x> <y>` operates on a hard-coded item slot 45, relocating it and closing it | `imple.cpp:412-428` |
| BR-23 | Business Logic | `set closearmia` / `set openarmia` locate the first item with `sIndex == 4143` and lock/open it | `imple.cpp:327-373` |
| BR-24 | Business Logic | `set clearcarry` clears carry slots 0..59, deliberately preserving slots 60..63 (incl. `KILL_MARK = 63`) | `imple.cpp:430-439` |
| BR-25 | Business Logic | `set item <idx> <e0> <v0> <e1> <v1> <e2> <v2>` builds an item in carry; item ids 2330..2389 receive a hard-coded bonus line | `imple.cpp:441-468` |
| BR-26 | Business Logic | `set <slot 1..16> <idx> [6 effects]` is an implicit positional form that overwrites the equipment slot and always runs after the keyword chain | `imple.cpp:479-506` |
| BR-27 | Business Logic | `set noatum` forces `CastleServer = 1` and `CastleState = 1` | `imple.cpp:470-476` |
| BR-28 | Operations | `set buildhtml` calls `BuildList()`, which is an empty function, then reports success | `imple.cpp:108-112`, `Code/TMSrv/Server.cpp:3713-3716` |
| BR-29 | File I/O | `save <name>` writes `sizeof(STRUCT_MOB)` raw bytes to `./npc/` + `<name>` (no sanitization, requires conn != 0) | `imple.cpp:508-526` |
| BR-30 | File I/O | `read <name>` raw-reads `sizeof(STRUCT_MOB)` bytes over the live character, then `strcpy`s the argument into the 16-byte `MobName` | `imple.cpp:527-555` |
| BR-31 | Business Logic | `name <n>` overwrites `MobName` by `strcpy` and re-broadcasts the character to the grid | `imple.cpp:556-567` |
| BR-32 | Side effect | Every `set` invocation (keyword or positional) ends with `GetCurrentScore`, `SendEquip`, `SendScore`, `SendEtc` and 16 `SendItem` calls | `imple.cpp:500-506` |

#### 3.3 Level 3 commands (character level >= 1003) — economy configuration

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-33 | Validation | `set billmode <n>` requires a live billing socket when `<n>` is 2 or 3; `<n>` itself is unvalidated | `imple.cpp:593-606` |
| BR-34 | Business Logic | `set billfree <n>` writes `FREEEXP` (free-account level threshold and divisor used by the exp commands) | `imple.cpp:607-612` |
| BR-35 | Business Logic | `set charsetbill/potioncount/potiondelay/partybonus/guildboard <n>` write economy globals and repaint/persist config | `imple.cpp:613-642` |
| BR-36 | Validation | `timer <ms>` requires `ms >= 5000` and retimes the `TIMER_MIN` heartbeat | `imple.cpp:571-580` |
| BR-37 | Business Logic | `saveall` force-logs-out every `USER_PLAY` slot through `CharLogOut` and logs each account | `imple.cpp:33-44, 581-585` |
| BR-38 | Dead code | `decay` is a no-op that still reports `"set decay"` | `imple.cpp:586-590` |

#### 3.4 Level 2 commands (character level >= 1002) — lifecycle, population, economy, events

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-39 | Lifecycle | `reboot` sets `ServerDown = 1`, starting a ~119 second countdown that blocks new connections and closes users | `imple.cpp:649-652`, `ProcessSecMinTimer.cpp:42-119`, `Server.cpp:4032-4039` |
| BR-40 | Lifecycle | `delayreboot <n>` computes `ServerDown = -1 * ServerIndex * (n * 2)` and clamps to 1 when out of the range -1000..1 | `imple.cpp:653-659` |
| BR-41 | Validation | `generate <idx>` requires `1 <= idx <= mNPCGen.NumList` and spawns that generator at the admin's target cell | `imple.cpp:660-671` |
| BR-42 | Business Logic | `create <name>` loads a mob definition from the `boss` folder at the admin's target cell and maps four return codes to messages | `imple.cpp:672-684` |
| BR-43 | Business Logic | `gift <mob name with _> <item> [effects]` injects an item into a player's carry or a monster's carry, with a 2330..2389 bonus line | `imple.cpp:686-762` |
| BR-44 | Business Logic | `cp <v>` writes the PK point into `Carry[63].stEffect[0].cEffect` clamped to 1..150 and re-broadcasts the character | `imple.cpp:764-775`, `Code/TMSrv/GetFunc.cpp:2145-2159` |
| BR-45 | Business Logic | `frag <v>` writes the kill counter into `Carry[63].stEffect[1..2]` clamped to 0..32767 and re-broadcasts the character | `imple.cpp:777-788`, `Code/TMSrv/GetFunc.cpp:2087-2103` |
| BR-46 | Operations | `log` restarts the three log streams (server log, chat log, item log) | `imple.cpp:792-799` |
| BR-47 | Validation | `guildday <0..6>`, `guildhour <0..23>`, `newbiehour <0..23>` set schedule globals with upper-bound-only validation | `imple.cpp:800-823` |
| BR-48 | Business Logic | `weekmode <0..5>` forces guild battle mode `n-1` (wrapping -1 to 5); out-of-range clears the override | `imple.cpp:824-838` |
| BR-49 | Operations | `reloadnpc`, `reloadguild`, `readguildname` reload generator, guild and guild-name tables | `imple.cpp:839-853` |
| BR-50 | Business Logic | `trtarget <1..8> <1..5> <item> [4 effects]` re-parses the line with a second `sscanf` and writes a treasure target | `imple.cpp:854-886` |
| BR-51 | Business Logic | `trsource <1..8> <item>` writes the treasure source item; `trrate <1..8> <1..5> <rate>` writes a treasure rate | `imple.cpp:887-917` |
| BR-52 | Validation | `statsapphire <n>` and `battleroyal <n>` perform a dead `n < 0` check on an unsigned value, then write the global | `imple.cpp:918-939` |
| BR-53 | Business Logic | `set evstart/evend/evitem/evrate/evindex/evdelete/evnotice <n>` write the event engine fields and echo a one-line summary | `imple.cpp:942-1020` |
| BR-54 | Validation | `set evon <n>` refuses to enable the event unless start, end, rate and item are all non-zero | `imple.cpp:996-1011` |
| BR-55 | Business Logic | `set double/deadpoint/dungeonevent <n>` toggle global event modes and persist config | `imple.cpp:1021-1048` |
| BR-56 | Validation | `set champ <zone> <guild>` requires `0 <= zone < 5`, persists the charged guild and reports guild zones | `imple.cpp:1049-1064` |
| BR-57 | Business Logic | `set chall <zone> <guild>` sets the challenger guild for a zone and persists guild data | `imple.cpp:1065-1079` |
| BR-58 | Validation | `set drop <pos> <rate>` accepts rate 0..8000 (upper bound only) and maps `pos` 1..16 onto `g_pDropBonus[64]` | `imple.cpp:1080-1115` |

#### 3.5 Level 1 commands (character level >= 1001) — world, moderation, broadcasts

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-59 | Business Logic | `weather <n>` forces the current weather and re-sends weather to all clients | `imple.cpp:1121-1126` |
| BR-60 | Diagnostics | `attmap` prints the map attribute byte under the admin's target cell | `imple.cpp:1127-1133` |
| BR-61 | Integration | `billconnect` connects to the billing server using `biserver.txt` and sets `BILLING = 3` | `imple.cpp:1134-1162` |
| BR-62 | Business Logic | `impost` zeroes the experience of the five `GuildImpostoID` mobs | `imple.cpp:1166-1175` |
| BR-63 | Business Logic | `trn` teleports the admin to (4000,4000) and back to the original cell | `imple.cpp:1177-1183` |
| BR-64 | Broadcast | `notice <text>` forwards the text as `_MSG_DBNotice` to the DB server for cluster-wide delivery | `imple.cpp:1185-1201` |
| BR-65 | Bulk file I/O | `waterexp <room> <xp>` rewrites 9 NPC files per water room with `Exp = xp * Level / FREEEXP` | `imple.cpp:1203-1309` |
| BR-66 | Destructive test | `testegrid` fills `pMobGrid[2100..4095][2100..4095]` with `1`, marking ~4 million cells as occupied by mob 1 | `imple.cpp:1311-1321` |
| BR-67 | Bulk file I/O | `svexp <level> <xp>` rewrites leader and follower NPC files; divides by `<level>` without a zero check | `imple.cpp:1323-1385` |
| BR-68 | Bulk file I/O | `nigexp <room> <xp>` rewrites 8..10 nightmare NPC files with `Exp = xp * Level / FREEEXP` | `imple.cpp:1387-1493` |
| BR-69 | Broadcast | `chiefnotice <text>` broadcasts to guild leaders only (`GuildLevel == 9`, guild > 0) | `imple.cpp:1494-1502`, `Code/TMSrv/SendFunc.cpp:64-101` |
| BR-70 | Business Logic | `chiefsummon` sends the chief summon signal cluster-wide (requires a valid conn) | `imple.cpp:1503-1507` |
| BR-71 | Operations | `rebuild` recomputes generator population counters from live mobs | `imple.cpp:1508-1512` |
| BR-72 | Moderation | `mute <name>` toggles `MuteChat` for the named playing character | `imple.cpp:1514-1532` |
| BR-73 | Moderation | `muteall` / `desmuteall` set or clear `MuteChat` but abort on the first idle slot | `imple.cpp:1534-1562` |
| BR-74 | Business Logic | `snoop` toggles the `MSV_SNOOP` bit in `MOB.Merchant`, then recomputes and re-sends score | `imple.cpp:1564-1583` |
| BR-75 | Diagnostics | `event` prints the event engine summary (read-only) | `imple.cpp:1584-1588` |
| BR-76 | Business Logic | `learn` sets `MOB.LearnedSkill = -1` (all bits) and pushes ETC | `imple.cpp:1590-1595` |
| BR-77 | Business Logic | `class <n>` writes `MOB.Class` and pushes ETC | `imple.cpp:1597-1602` |
| BR-78 | Business Logic | `buff <n>` installs affect slot 0 with `Time = 162000`; `nobuff` clears all 32 affect slots | `imple.cpp:1604-1626` |
| BR-79 | Business Logic | `soul <n>` writes `extra.Soul` | `imple.cpp:1628-1633` |
| BR-80 | Business Logic | `fame <guild> <delta>` adds to `GuildInfo[guild].Fame` and pushes `_MSG_GuildInfo` to the DB | `imple.cpp:1634-1655` |
| BR-81 | Business Logic | `killkefra` kills every mob whose `GenerateIndex == KEFRA_BOSS` by calling `MobKilled` | `imple.cpp:1657-1664` |
| BR-82 | Security | `npko` writes `Admin.txt` and rewrites `pAdminIP`, but only for three hard-coded account names | `imple.cpp:1666-1703` |
| BR-83 | Business Logic | `kill <name>` zeroes HP and calls `MobKilled` for every mob matching the 16-byte name | `imple.cpp:1705-1722` |
| BR-84 | Business Logic | `createkefra` resets `KefraLive` and generates the Kefra boss plus four adds | `imple.cpp:1724-1736` |
| BR-85 | Business Logic | `partydif/rvrhour/gtorrehour/dropitem/maxnightmare <n>` write global schedule/economy values and persist config | `imple.cpp:1738-1782` |
| BR-86 | Business Logic | `emotion <motion> <parm>` sends a single emotion packet to the admin | `imple.cpp:1785-1789` |
| BR-87 | Business Logic | `teleport <x> <y>` converts values below 100 from sector to cell coordinates and bounds-checks against 4096 | `imple.cpp:1792-1801` |
| BR-88 | Moderation | `kick <name>` logs out and closes the target socket only if the target level is strictly lower than the admin's | `imple.cpp:1802-1828` |
| BR-89 | Business Logic | `gsummon <guild>` teleports up to 48 members of that guild to the admin's target cell | `imple.cpp:1830-1836` |
| BR-90 | Business Logic | `allsummon` teleports every eligible player to the admin's target cell | `imple.cpp:1837-1844` |

---

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 to BR-04 — Command framing, parsing and pre-authorization audit

**Overview**:
The component defines an implicit wire format through the parser itself: the first byte of the string is skipped, the next nine whitespace-separated tokens are copied into fixed 128-byte buffers with unbounded `%s` conversions, and each token is then re-read as a number. The audit record is produced from the raw string before any privilege decision is taken.

**Detailed description**:
Parsing begins at `str + 1` (`imple.cpp:68`). Because `str` is the whisper body (or the DB message body), the documented-by-implementation syntax is `+<command> <arg1> ... <arg8>`: a single discarded prefix byte followed by the command word. A body without that prefix silently shifts every token, so `set level 2010` typed as `set level 2010` is parsed as command `et`, which matches nothing and produces no reply. The `sscanf` has no field widths, but the destination buffers are 128 bytes and the source is at most 100 bytes (`MESSAGEWHISPER_LENGTH`, `Code/Basedef.h:130`) on the whisper path and 96 bytes (`MESSAGE_LENGTH`, `Code/Basedef.h:129`) on the DB path, so the immediate overflow is bounded by the protocol; however, absent tokens are not initialized, meaning `sval1..sval8` may contain stale stack data from a previous invocation in the same frame.

Each numeric argument is derived twice. `ival1..ival8` are `unsigned int` and are produced with `%d`; `dval2` is a `long long` produced with `%llu` from token 2 only. That dual parse is why `set exp` reads `dval2` (64-bit, unsigned semantics) while every other command reads the truncated 32-bit mirror. It also means negative numeric input silently wraps: `-1` becomes 4294967295 in `ival*` and a huge unsigned value in `dval2`, which matters because most downstream range checks are written as `x < 0` (always false) rather than as a guard on the unsigned value.

The audit line is built with `sprintf(logtemp, "adm %s", str)` into a 256-byte buffer and handed to `Log()` with the account name and IP when `conn != 0`, or `"system"`/0 otherwise (`imple.cpp:80-87`). This happens **before** `if (level >= 8)` and therefore before any authorization decision: an unauthorized player can generate one log line, one file write, and one window-title update per whisper. The log entry records only the raw command, never the derived level, the outcome, or whether the command was actually executed.

**Rule workflow**:
```
1. Receive str (whisper body or DB message body)
2. cmd = first token of str+1; sval1..sval8 = following tokens
3. ival1..ival8 = atoi-like parse of sval1..sval8; dval2 = 64-bit parse of sval2
4. logtemp = "adm " + str
5. conn != 0 -> Log(logtemp, pUser[conn].AccountName, pUser[conn].IP)
   conn == 0 -> Log(logtemp, "system", 0)
6. Continue to the level gates (no authorization decision has been made yet)
```

---

### Business Rule: BR-05, BR-07, BR-08 — Authorization model and its tiers

**Overview**:
Authority is a single derived integer compared against four thresholds. There is no identity, role, ownership, or origin check anywhere in the component, and the connection-bound guard that was intended to validate `conn` is a tautology.

**Detailed description**:
The whisper caller computes `int level = pMob[conn].MOB.CurrentScore.Level - 1000;` and passes it unchanged (`_MSG_MessageWhisper.cpp:789`). The DB caller passes `m->Level` from the packet (`ProcessDBMessage.cpp:132`). Inside `ProcessImple` the only authorization construct is the sequence of `if (level >= 8)`, `if (level >= 3)`, `if (level >= 2)`, `if (level >= 1)` blocks. There is no `else` between them, so a level-8 admin traverses five blocks: the level-8 block, the level-3 block, the level-2 block, and both level-1 blocks. Commands that appear in more than one block (`set`) are dispatched twice by the same input, each time against a different keyword set.

`CurrentScore.Level` is recomputed from `BaseScore.Level` on every `GetCurrentScore()` (`Code/TMSrv/CMob.cpp:598`, `Code/Basedef.cpp:2979` — `MOB.CurrentScore = MOB.BaseScore`). No equipment, affect or buff path contributes to `Level`, so the value is exactly the persisted character level stored in the account record. The game's own level ceilings are `MAX_LEVEL = 399` and `MAX_CLEVEL = 199` (`Code/Basedef.h:117-118`), which means no legitimate player can reach 1001; authority is therefore entirely dependent on the integrity of the persisted level field and on the DB channel that can write it. The intended explicit flag exists — `pUser[n].Admin = 1` is set at character login when `CurrentScore.Level >= 999` (`ProcessDBMessage.cpp:1004-1005`) and cleared at logout (`:391`, `Server.cpp:6843`) — but a repository-wide search finds no read of `pUser[].Admin` anywhere, so the flag is dead code and no command consults it.

The connection guard is written as `if (conn <= 0 && conn >= MAX_USER) return;` at `imple.cpp:91` and repeated at 1051, 1067, 1566 and 1832. The conjunction is unsatisfiable, so the guard never executes. The practical consequences differ by entry point: on the whisper path `conn` is always a valid user slot, so the missing guard is a defense-in-depth loss; on the DB path `conn` is hard-coded to 0, so all `pMob[conn]`/`pUser[conn]` mutations land on slot 0 and the "return" that the author expected never protects anything. Slot 0 is also reserved by convention (`GetUserByName` iterates from 1, `SendClientMessage` rejects `conn <= 0`), which means DB-issued `set hp`, `set coin`, `learn`, `buff` and similar commands mutate a slot that is invisible to clients.

**Rule workflow**:
```
1. Whisper path: level = pMob[conn].MOB.CurrentScore.Level - 1000
   DB path:      level = packet.Level (attacker-controlled at the DB client)
2. level >= 8 -> execute character-state / file / equipment blocks
3. level >= 3 -> execute economy configuration block (second dispatch of "set")
4. level >= 2 -> execute lifecycle / population / event block (third dispatch of "set")
5. level >= 1 -> execute world/broadcast block, then moderation/self block
6. No match -> no reply, no denial record (only the pre-audit line from BR-04)
```

---

### Business Rule: BR-09, BR-10 — Silent failure and shared-buffer execution model

**Overview**:
The component has no negative acknowledgement path. Every rejection (unknown command, failed range check, unmet level, failed file open) either returns silently or emits an unstructured one-line message, and all branches share one global buffer.

**Detailed description**:
Of the 125 command tokens handled, only a minority acknowledge success, and responses are literal English strings such as `"set level"`, `"CREATE GATE"`, `"err: +treasure (1-8) (1-5) (item)"`, `"Wrong Drop Position"`. Some commands intentionally produce no output at all (`teleport`, `trn`, `testegrid`, `gsummon`, `allsummon`, `attmap` on failure, `muteall`/`desmuteall` success path for the target but not the actor). Commands that lose their guard by `return` also skip the acknowledgement of later work in the same block — for example `set gate` returns from the whole function when `CreateItem` fails (`imple.cpp:274-275`), and `set clearcarry` never runs.

The execution model assumes one thread. `ProcessImple` is driven from `MainWndProc` through `WSAAsyncSelect` notifications (`Server.cpp:3718+`), so there is no concurrency inside the function. It nevertheless uses the process-global `temp[4096]` for interim formatting in `set item`, `set <slot>`, `gift`, `cp`-adjacent logging, `notice`, `chiefnotice` and the event summaries. Any callee that also writes `temp` — and several do — would corrupt the caller's message; the code avoids the visible bugs only by copying into a local `MSG_*` structure before the next call.

There is no rate limiting, no per-session command budget, and no lockout on repeated failures. Because the audit line is written before authorization, and because `SendClientMessage` writes into the socket send buffer without back-pressure checks, an unauthenticated player can drive log-file growth and window-title churn indefinitely with arbitrary `/gm` bodies.

**Rule workflow**:
```
1. Parse command and arguments
2. Write pre-audit log line
3. Evaluate level gates; if none applies -> return with no output
4. If a branch applies -> validate arguments
5. Validation failure -> one SendClientMessage or bare return
6. Success -> mutate state, optionally notify clients, optionally draw/persist config
```

---

### Business Rule: BR-11 to BR-19 — Level 8 `set` keyword family (character attributes)

**Overview**:
Thirty-nine `set` keywords plus one implicit positional form operate directly on the admin's own mob record. They bypass every game rule that normally governs attribute growth: caps, costs, quest prerequisites, class restrictions and stat point accounting.

**Detailed description**:
`set sanc <v>` iterates the eight equipment slots and, where `sIndex != 0`, overwrites `stEffect[0]` with `EF_SANC` and the given value (`imple.cpp:96-107`). This is a direct sanctification write that ignores the sanctification rate tables and the failure model implemented elsewhere in the server, and it also overwrites whatever effect was validly present in slot 0 of each item. `set clearquest` and `set clearskill` zero the quest byte and the 32-bit learned-skill mask respectively, which removes progression state without touching rewards already consumed. `set level <n>` writes `BaseScore.Level` with a single clamp at 2010 — above the game's own maximum of 399 — and no lower clamp, so an admin can silently demote themselves or others to level 0. Because `GetCurrentScore` skips the resist/equipment sanitisation branch when `BaseScore.Level >= 2000` (`CMob.cpp:603`), a level above 2000 takes a different code path than a level of 2010's neighbours.

The numeric writers are one-liners with implicit narrowing. `set hp` and `set mp` assign both the base and current maxima and both current values in one statement pair, so the character becomes full and cannot be distinguished from a legitimately healed character. `set ac`, `set dam`, `set str`, `set int`, `set dex`, `set con` write `BaseScore` only, relying on the mandatory tail (`imple.cpp:500-506`) to recompute `CurrentScore`, resend score, equip, ETC and all 16 equipment slots. Fields narrower than 32 bits truncate silently: `Str/Int/Dex/Con` are `short`, `Merchant/AttackRun/Critical` are `unsigned char`, `Special[]` is `short`, and `ScoreBonus/SpecialBonus/SkillBonus` are `unsigned short` (`Code/Basedef.h:414-481`). `set coin` writes a full 32-bit `int`, and `set exp` writes the 64-bit `Exp` and then executes exactly one `CheckGetLevel()` pass, so a very large experience injection advances at most one level until the next natural gain.

Three keywords are special. `set learned <n>` is purely diagnostic: it computes `1 << (n % 24)`, formats it into the global `temp`, and sends it back, changing no state (`imple.cpp:230-237`). `set reloadfile` is an operational reload that reruns `BASE_InitializeBaseDef`, `BASE_InitializeMessage("Language.txt")`, the sanctification, quest and composition rate readers and `CCastleZakum::ReadCastleQuest` (`imple.cpp:250-262`) — a heavy table swap triggered by one chat line. `set classmaster <n>` writes `extra.ClassMaster`, the field that selects the mortal / celestial / arch progression table in `CheckGetLevel` and the soul multipliers in `BASE_GetCurrentScore`, so it is a progression-class switch rather than a cosmetic change.

**Rule workflow**:
```
1. cmd == "set"
2. Match sval1 against 39 keywords (else-if chain)
3. On match: write the corresponding field(s) of pMob[conn].MOB / MOB.BaseScore / MOB.extra
4. Special cases: reloadfile reloads tables; learned only echoes a bitmask
5. Fall through to the positional equipment block (BR-26)
6. Tail: GetCurrentScore(conn), SendEquip, SendScore, SendEtc, 16 x SendItem
```

---

### Business Rule: BR-20 to BR-23, BR-25, BR-26 — Item forgery and world gate manipulation

**Overview**:
This family creates arbitrary items in the admin's carry or equipment slots and manipulates the persistent world-item array. Item ids 2330..2389 receive a hard-coded effect line; ground-item operations index `pItem[]` without validating the index.

**Detailed description**:
`set item <idx> <e0> <v0> <e1> <v1> <e2> <v2>` builds a zeroed `STRUCT_ITEM`, sets `sIndex` from `ival2`, applies an automatic bonus when the id falls in 2330..2389 (`stEffect[0].sValue = 2000`, `stEffect[1] = 120/60`), then overwrites all three effect pairs from the remaining six arguments and calls `PutItem` (`imple.cpp:441-468`). `PutItem` searches the first empty slot up to `MaxCarry` and returns FALSE when full, but the branch replies `"SET ITEM CARRY"` regardless, so a failed injection is reported as success. The same 2330..2389 special case is duplicated in `gift` and in the positional equipment form, giving three copies of the same magic constant range.

The positional form is the least obvious part of the component. After the keyword chain, an independent `if (ival1 >= 1 && ival1 <= MAX_EQUIP)` block runs for **every** `set` invocation, using `ival1` — the value obtained by parsing `sval1` as a number (`imple.cpp:479-499`). Consequently `+set 3 2330 1 2 3 4 5 6` means "zero equipment slot 2, put item 2330 in it, apply effects", while `+set sanc 5` reaches the same block with `ival1 == 0` and skips it. `MAX_EQUIP` is 16 and `STRUCT_MOB::Equip` is `STRUCT_ITEM Equip[MAX_EQUIP]`, so the bound is correct, but the semantics are undocumented and inseparable from the keyword chain: a numeric first argument that also happens to be a keyword's numeric prefix is reinterpreted. Each successful write logs a `-system` line with the full item fingerprint.

The gate commands operate on `pItem[]`, the 5000-entry world item array (`MAX_ITEM = 5000`, `Basedef.h:103`). `set gate` builds the item, calls `CreateItem(x, y, &item, rot, 1)`, rejects ids outside `1 .. MAX_ITEM-1`, and then multicasts `_MSG_CreateItem` with `ItemID = ival2 + 10000` — using the *item definition id* where every other producer uses the *pItem slot index* (`GetCreateItem` sets `sm->ItemID = idx + 10000`, `GetFunc.cpp:1205`). The client therefore receives an object whose identity is derived from the wrong value. `set destroygate`, `cgate`, `ogate` and `ngate` take `ival2` as a `pItem` index and use it directly in `GetCreateItem(ival2, ...)`, `pItem[ival2]`, `memset(&pItem[ival2], 0, sizeof(CItem))` and `UpdateItem(ival2, ...)` with no comparison against `MAX_ITEM`. `set mgate` avoids the issue by hard-coding slot 45. `closearmia`/`openarmia` search linearly for `sIndex == 4143` and return silently when absent.

**Rule workflow**:
```
set item:  build STRUCT_ITEM -> 2330..2389 bonus -> apply 3 effect pairs -> PutItem -> always reply success
set <slot>: 1 <= ival1 <= 16 -> memset Equip[ival1-1] -> sIndex = ival2 -> 2330..2389 bonus
            -> apply 3 effect pairs -> Log fingerprint -> (tail re-sends equipment)
set gate:  item.sIndex = ival2 -> CreateItem(ival3,ival4,...,ival5,1) -> validate 1..4999
           -> MSG_CreateItem with ItemID = ival2+10000 -> GridMulticast(ival3,ival4)
set destroygate: GetCreateItem(ival2) -> GridMulticast -> memset pItem[ival2]
set cgate: Delay=0 -> UpdateItem(ival2, STATE_LOCKED) -> multicast create
set ogate: Delay=1 -> UpdateItem(ival2, STATE_OPEN) -> multicast update
set ngate: Delay=0 -> UpdateItem(ival2, STATE_CLOSED) -> multicast create
set mgate: operate on pItem[45] -> set PosX/PosY -> multicast create
```

---

### Business Rule: BR-24 — Carry purge with reserved tail slots

**Overview**:
`set clearcarry` clears inventory slots 0 through 59 and deliberately preserves slots 60 through 63, protecting the kill-mark pseudo-item stored at index 63.

**Detailed description**:
The loop bound is `MAX_CARRY - 4` (`MAX_CARRY = 64`, `Basedef.h:76`), so slots 0..59 are zeroed with `BASE_ClearItem` and synchronised to the client individually. Slots 60..63 are untouched because the server stores non-inventory data in the tail of the carry array: `KILL_MARK` is defined as 63 (`Basedef.h:166`) and holds the PK point (`stEffect[0]`), the kill counter (`stEffect[1..2]`) and the guilt value (`GetFunc.cpp:2105-2145`). Wiping slot 63 would reset the character's PK state and, because the client mirrors the carry array, desynchronise the PK display.

The rule is an implicit invariant rather than a checked one: nothing prevents other commands, or the DB path, from writing into 60..63. `cp` and `frag` depend on that layout, as does the `/cp` whisper handler which reads `GetPKPoint(conn)`. The number 4 is a literal with no named constant, so the coupling between the loop bound and `KILL_MARK` is invisible to a reader.

The command also demonstrates the component's habit of reporting success after partial work: it clears and re-sends 60 items before the acknowledgement, with no error path if `SendItem` fails or if the slot is out of the client's view.

**Rule workflow**:
```
1. cmd == "set", sval1 == "clearcarry"
2. for i in 0 .. 59: BASE_ClearItem(Carry[i]); SendItem(conn, ITEM_PLACE_CARRY, i, Carry[i])
3. Slots 60..63 (incl. KILL_MARK = 63) remain untouched
4. Reply "SET CLEAR CARRY"
```

---

### Business Rule: BR-27, BR-28, BR-38 — Declared-but-inert operations

**Overview**:
Three commands exist in the dispatch table but perform no work (`set buildhtml`, `decay`) or perform work with no validation path (`set noatum` force-enables a castle war state).

**Detailed description**:
`set buildhtml` calls `BuildList()`, whose entire body is empty (`Code/TMSrv/Server.cpp:3713-3716`), then replies `"html built"`. This is a decompilation artefact or a deliberately stubbed feature; either way the acknowledgement is false and there is no way for an operator to detect it from the chat panel.

`decay` is commented out at the assignment site (`//DecayValue = ival1;`, `imple.cpp:588`) but the acknowledgement `"set decay"` remains, so the command is a pure no-op that still produces an audit line and a client message.

`set noatum` sets `CastleServer = 1` and `CastleState = 1` with no arguments and no confirmation of the previous state. `CastleState` is consumed by the timer processing and by the login path (`ProcessDBMessage.cpp:1010-1011`), so a single mistyped command changes the castle/atum war state for every player, and there is no command to restore a prior value other than `set noatum` again (which is idempotent in the same direction).

**Rule workflow**:
```
set buildhtml: BuildList() [empty] -> reply "html built"
decay:         no state change -> reply "set decay"
set noatum:    CastleServer = 1; CastleState = 1 -> reply "SET NOATUM WAR"
```

---

### Business Rule: BR-29 to BR-31 — Privileged NPC file I/O and name overwrite

**Overview**:
`save`, `read` and `name` give a level-8 administrator raw read/write access to files under `./npc/` plus a `strcpy` into a 16-byte name field. The file path is built by string concatenation with no sanitisation.

**Detailed description**:
`save <name>` copies `"./npc/"` into the global `temp`, appends the argument, opens with `_O_CREAT | _O_RDWR | _O_BINARY`, and writes `sizeof(pMob[conn].MOB)` bytes of the live `STRUCT_MOB` (`imple.cpp:508-526`). There is no check that the name is short, contains no separators, or ends with a sane extension, so a value containing `.` or `\` components escapes the `npc` directory in the same way any pre-validated path concatenation does on Win32. The written blob is the live character record, which contains the account's equipment, carry, coin, experience and extra block — i.e. the command is an exfiltration primitive onto the local filesystem.

`read <name>` performs the inverse operation (`imple.cpp:527-555`): it opens read-only, `_read`s `sizeof(STRUCT_MOB)` bytes directly over `pMob[conn].MOB`, then sends the equipment and score to the client, then executes `strcpy(pMob[conn].MOB.MobName, sval1)` — an unbounded copy from a buffer that may hold up to 127 characters into a 16-byte field (`NAME_LENGTH = 16`). This overwrites `MobName`, `Clan`, `Merchant`, `Guild`, `Class`, `Rsv`, `Quest` and the first bytes of `Coin`/`Exp` with attacker-influenced text, and because `pMob` is a global array the write continues into adjacent `CMob` members. There is no validation that the file is a valid mob record, no size check, and no rollback if the read is short: a truncated file leaves a partially overwritten character that is then saved by the next logout.

`name <n>` (`imple.cpp:556-567`) repeats the same unbounded `strcpy` into `MobName` and then re-broadcasts the character through `GetCreateMob` + `GridMulticast`, so the corrupted name is propagated to nearby clients. `save` and `read` additionally require `conn != 0` (`imple.cpp:508, 527`), which is the only capability distinction in the whole component; the DB path cannot reach them.

**Rule workflow**:
```
save <name>:
  1. temp = "./npc/" + <name>
  2. _open(temp, _O_CREAT|_O_RDWR|_O_BINARY, _S_IREAD|_S_IWRITE)   -> -1 => "fail - save file"
  3. _write(handle, &pMob[conn].MOB, sizeof(STRUCT_MOB))
  4. reply "npc: <name> saved"

read <name>:
  1. temp = "./npc/" + <name>
  2. _open(temp, _O_RDONLY|_O_BINARY)                              -> -1 => "fail - open file"
  3. _read(handle, &pMob[conn].MOB, sizeof(STRUCT_MOB))            [no validation]
  4. SendItem x16, strcpy(MobName, <name>) [unbounded], BaseScore = CurrentScore,
     GetCurrentScore, SendScore, SendEtc

name <n>:
  1. strcpy(pMob[conn].MOB.MobName, <n>) [unbounded]
  2. GetCreateMob -> GridMulticast(TargetX, TargetY)
```

---

### Business Rule: BR-32 — Mandatory post-`set` resynchronisation

**Overview**:
Every `set` invocation that reaches the end of the level-8 block performs a full client resynchronisation, regardless of which keyword matched, and persists the recomputed scores into the live record.

**Detailed description**:
The tail at `imple.cpp:500-506` runs `GetCurrentScore(conn)`, `SendEquip(conn, conn)`, `SendScore(conn)`, `SendEtc(conn)` and a loop of 16 `SendItem` calls. Its purpose is to make the mutated `BaseScore` visible in `CurrentScore` and on the client. Because it is unconditional, even purely read-only or operational keywords (`set learned`, `set reloadfile`, `set buildchar`-style diagnostics) pay the full cost, and each of the 16 calls allocates and queues a packet.

The tail also has semantic weight: `GetCurrentScore` recomputes `CurrentScore` from `BaseScore`, reapplies affects, clamps HP/MP to their maxima and applies item-derived bonuses. A `set hp 999999` therefore does not necessarily survive the tail intact if equipment or affect logic adjusts the maxima, and a `set level 2010` skips the resist/equipment sanitisation branch entirely. This ordering — mutate first, recompute afterwards — is the component's central invariant and the reason most attribute commands are one assignment rather than a validated transaction.

Branches that `return` early (`set gate` with an invalid id, the invalid-gate paths in `closearmia`/`openarmia`) skip the tail, so a partial mutation can remain visible in `BaseScore` but not in `CurrentScore` until the next unrelated recomputation. No command in the component performs a rollback.

**Rule workflow**:
```
1. Keyword handler writes BaseScore / MOB / extra
2. (optional) positional equipment write
3. GetCurrentScore(conn)  -> CurrentScore = BaseScore + items + affects
4. SendEquip(conn, conn)
5. SendScore(conn)
6. SendEtc(conn)
7. for j in 0..15: SendItem(conn, 0, j, Equip[j])
8. If an earlier branch returned, steps 3-7 are skipped
```

---

### Business Rule: BR-33 to BR-38 — Level 3 economy and heartbeat control

**Overview**:
Seven `set` keywords write the billing/free-tier/economy globals, two commands control the minute heartbeat and the global save, and one command is inert. All of them persist through `DrawConfig(1)`, which rewrites `gameconfig.txt` and repaints the server window.

**Detailed description**:
`set billmode <n>` has the only meaningful precondition in the family: when `<n>` is 2 or 3 the code requires `BillServerSocket.Sock != NULL` and otherwise replies `"not connected to billing server.+billconnect first"` (`imple.cpp:595-602`). Any other value is written unconditionally, so `BILLING` can be set to arbitrary integers that no consumer expects; `BILLING` gates character login (`_MSG_CharacterLogin.cpp:31, 69`) and billing packet emission (`ProcessSecMinTimer.cpp:44`), so an out-of-range value changes login behaviour rather than failing closed. `set billfree <n>` writes `FREEEXP`, which is simultaneously the free-account level threshold (`_MSG_CharacterLogin.cpp:31, 53, 69`) and the divisor used by `waterexp` and `nigexp` (`imple.cpp:1234, 1267, 1300, 1418, 1451, 1484`); setting it to 0 converts those commands into division-by-zero crashes. `set charsetbill`, `set potioncount`, `set potiondelay`, `set partybonus` and `set guildboard` follow the same write-and-persist pattern, and `set guildboard` acknowledges with the wrong string (`"SET PARTYBONUS"`, `imple.cpp:640`).

`timer <ms>` rejects values below 5000 with the message `"SET TIMER can't be less than 5 sec"` and then calls `SetTimer(hWndMain, TIMER_MIN, ival1, NULL)` (`imple.cpp:571-580`). `TIMER_MIN` drives `ProcessMinTimer` (`Server.cpp:3727-3728`), so the command changes the frequency of the server's minute tick — the clock behind regeneration, spawn cadence, event timing and economy accumulation. Values above the Win32 timer maximum are accepted without warning.

`saveall` calls `SaveAll()`, which iterates every slot, and for each slot in `USER_PLAY` executes `CharLogOut(i)` and logs the account (`imple.cpp:33-44`). `CharLogOut` refuses non-playing slots, persists the character through `SaveUser`, removes it from the grid and returns the connection to `USER_SELCHAR`; the player is not disconnected. The command therefore performs a cluster-wide forced relog of every online character and is the only bulk persistence primitive in the component.

**Rule workflow**:
```
set billmode N:   if N in {2,3} and BillServerSocket.Sock == NULL -> refuse
                  BILLING = N; DrawConfig(1); reply "set billing"
set billfree N:   FREEEXP = N; DrawConfig(1)
set charsetbill/potioncount/potiondelay/partybonus/guildboard N:
                  <global> = N; DrawConfig(1)
timer <ms>:       ms < 5000 -> refuse; else SetTimer(hWndMain, TIMER_MIN, ms)
saveall:          for each slot: if Mode == USER_PLAY -> CharLogOut(slot) + Log("saveall", ...)
decay:            no-op
```

---

### Business Rule: BR-39, BR-40 — Server lifecycle countdown

**Overview**:
`reboot` and `delayreboot` write the global `ServerDown` counter, which is consumed by the accept path and the second timer to produce a staged shutdown.

**Detailed description**:
`reboot` sets `ServerDown = 1` (`imple.cpp:649-652`). From that moment the accept handler rejects every new connection with `_NN_ServerReboot_Cant_Connect` (`Server.cpp:4032-4039`), and `ProcessSecTimer` increments the counter once per second; whenever `ServerDown % 20 == 1` it broadcasts the corresponding shutdown string from the message table (`ProcessSecMinTimer.cpp:105-109`), and at 120 it switches the timer to 200 ms and begins closing users one per tick (`ProcessSecMinTimer.cpp:42-93, 113-118`). The full cycle is therefore roughly two minutes from command to `PostQuitMessage`.

`delayreboot <n>` computes `ServerDown = -1 * ServerIndex * (ival1 * 2)` and then clamps any value greater than 1 or less than -1000 to 1 (`imple.cpp:653-659`). Because `ServerIndex` is initialised to -1 and defaults are assigned by the DB at startup (`_MSG_DBSetIndex`, `ProcessDBMessage.cpp:69-81`), the sign of the expression depends on runtime state: with `ServerIndex = -1` the result is positive and immediately clamped to 1; with `ServerIndex = 0` the result is 0, which is inside the accepted range and therefore *not* clamped, producing a different starting phase than `reboot`. The intent is clearly "delay the reboot by N seconds scaled by server index", but the arithmetic and the clamp interact in ways that make the effective delay dependent on deployment topology.

Neither command validates that a shutdown is not already in progress, and neither can be cancelled: there is no command that resets `ServerDown` to -1000. Recovery requires a process restart, which is the same action the counter is driving towards.

**Rule workflow**:
```
reboot:
  1. ServerDown = 1
  2. Next ProcessSecTimer: ServerDown % 20 == 1 -> SendNotice(reboot message N)
  3. ServerDown == 120 -> TIMER_SEC = 200 ms; one CloseUser per tick
  4. UserCount >= MAX_USER -> PostQuitMessage

delayreboot <n>:
  1. ServerDown = -1 * ServerIndex * (n * 2)
  2. if ServerDown > 1 or ServerDown < -1000 -> ServerDown = 1
  3. Countdown proceeds as above (from a different starting phase if ServerDown == 0)
```

---

### Business Rule: BR-41, BR-42 — Spawning from the generator table and from boss files

**Overview**:
`generate` spawns a configured NPC generator at the admin's target cell; `create` instantiates a mob from a file in the `boss` folder at the same cell. Both are level 2 and both mutate the live mob population.

**Detailed description**:
`generate <idx>` validates `1 <= idx <= mNPCGen.NumList`, re-checks `conn` with the tautological guard, and calls `GenerateMob(idx, TargetX, TargetY)` (`imple.cpp:660-671`). `GenerateMob` applies the group-size roll, respects `MaxNumMob`, copies the leader template, resolves grid placement and multicasts the create (`Server.cpp:2995-3280`). The bounds check is correct here, which is notable because `GenerateMob` itself does not validate its index; the safety depends entirely on this caller.

`create <name>` calls `CreateMob(sval1, TargetX, TargetY, "boss", 0)` and maps four return values: `1` success, `0` no monster file in the boss directory, `-1` no empty mob slot, `-2` no empty mob grid (`imple.cpp:672-684`). `CreateMob` truncates the name to 16 bytes, loads the definition through `ReadMob`, rewrites `_` to space and `@` to NUL, seeds the segment list around the target cell, sets `RouteType` and `WaitSec` from `Type`, recomputes score and multicasts (`Server.cpp:2865-2993`). Because the folder is hard-coded to `"boss"` and `Type` is 0, the command is a boss-spawn primitive with full template authority: the created mob carries whatever equipment and stats the file defines, including the `220`/`219`/`358` special-case equipment branches.

Neither command is idempotent, neither records which operator spawned what beyond the generic audit line, and neither removes the mob. `createkefra` (BR-84) and `generate` share the same code path but different bounds discipline.

**Rule workflow**:
```
generate <idx>:
  1. idx <= 0 or idx > mNPCGen.NumList -> "generation index wrong"; return
  2. conn guard (tautology) -> returns only if conn is simultaneously <= 0 and >= 1000
  3. GenerateMob(idx, pMob[conn].TargetX, pMob[conn].TargetY)

create <name>:
  1. CreateMob(<name>, TargetX, TargetY, "boss", 0)
  2. ret == 1  -> "Create mob success"
     ret == 0  -> "No monster file in boss directory"
     ret == -1 -> "No empty mob"
     ret == -2 -> "No empty mob grid"
```

---

### Business Rule: BR-43 — `gift`: item injection into players or monsters

**Overview**:
`gift` resolves a target by name across the whole mob space, builds an item identical in construction to `set item`, and inserts it into the target's carry with a slot-scan that is bounded by the shop-list size rather than the carry size.

**Detailed description**:
The target name is first normalised by replacing `_` with space over `NAME_LENGTH` bytes, then matched with `strcmp` against `pMob[i].MOB.MobName` for `i` in `1 .. MAX_MOB-1` (`imple.cpp:693-709`). A miss replies `"MOB not found"`. Because the scan covers both player slots (1..999) and monster slots (1000..24999), the command can target a live player by character name or a live monster by its template name, and the first match wins.

The item is constructed with the same three-effect pattern and the same 2330..2389 bonus line as `set item`. The insertion path then splits on `i >= MAX_USER`: for a player it calls `PutItem(i, &Item)`; for a monster it scans `pos` in `0 .. MAX_SHOPLIST-1` (27) computing `invpos = (pos % 9) + ((pos / 9) * MAX_SHOPLIST)` — which is identity for 0..26 — and breaks on the first empty `Carry[invpos]` (`imple.cpp:727-753`). The post-loop test is `if (pos >= 0 && pos < MAX_CARRY)`, so when all 27 scanned slots are full the loop ends with `pos == 27`, the test succeeds, and the item overwrites `Carry[27]` instead of taking the "no space" branch. The intended failure message is therefore unreachable for the 37 slots that the scan never inspects.

Monster carry is meaningful game data: `GenerateMob` copies `pMobMerc[i].Carry` into the generator leader template (`Server.cpp:3006`), and mob death drops the carry. A gifted item on a live monster therefore becomes a guaranteed drop, and on a generator-backed mob the effect can outlive the instance depending on which mailbox path is used. The command logs a `-system` line with the item fingerprint.

**Rule workflow**:
```
1. Replace '_' with ' ' in sval1
2. Linear search pMob[1..MAX_MOB-1] for MobName == sval1 -> not found => "MOB not found"
3. Build STRUCT_ITEM from ival2..ival8 (+ 2330..2389 bonus)
4. if target >= MAX_USER (monster):
     scan 27 carry slots -> first empty -> memcpy + SendItem
     all 27 full -> pos == 27 -> passes bounds test -> overwrite Carry[27]
   else (player):
     PutItem(target, &Item)
5. Log item fingerprint -> reply "SET NPCGIFT"
```

---

### Business Rule: BR-44, BR-45 — PK point and kill-counter injection

**Overview**:
`cp` and `frag` write the two numeric halves of the `KILL_MARK` pseudo-item and re-broadcast the character so the client picks up the new state.

**Detailed description**:
`cp <v>` calls `SetPKPoint(conn, ival1)`, which clamps the value into 1..150 and stores it in `Carry[63].stEffect[0].cEffect` (`GetFunc.cpp:2145-2159`). The clamp is meaningful on the low side because `GetPKPoint` returns the raw byte — the display path subtracts 75 (`_MSG_MessageWhisper.cpp:35`), so the stored value is a point on a 1..150 scale whose neutral point is 75. The command then rebuilds and multicasts the character packet (`imple.cpp:764-775`) so that observers refresh the PK colour.

`frag <v>` calls `SetTotKill(conn, ival1)`, which clamps into 0..32767 and splits the value into a low byte stored in `stEffect[1].cValue` and a high byte in `stEffect[2].cValue` (`GetFunc.cpp:2087-2103`); `GetTotKill` reassembles them (`:2058-2069`). The 32767 ceiling is the signed 16-bit limit even though the storage is two bytes, so the top half of the byte range is never used. As with `cp`, the character is re-broadcast afterwards.

Both commands write into slot 63, which is precisely the slot that `set clearcarry` preserves (BR-24). The pairing is the clearest example of an implicit layout contract in the component: the carry array is simultaneously an inventory, a PK record and a kill record, and three separate command families depend on that.

**Rule workflow**:
```
cp <v>:   SetPKPoint(conn, clamp(v, 1, 150)) -> Carry[63].stEffect[0].cEffect
          GetCreateMob + GridMulticast(TargetX, TargetY) -> reply "CP"
frag <v>: SetTotKill(conn, clamp(v, 0, 32767)) -> Carry[63].stEffect[1..2].cValue
          GetCreateMob + GridMulticast(TargetX, TargetY) -> reply "FRAG"
```

---

### Business Rule: BR-46 to BR-49 — Logging, schedules and table reloads

**Overview**:
`log` rotates the three log streams; `guildday`, `guildhour`, `newbiehour` and `weekmode` set the battle schedule; `reloadnpc`, `reloadguild` and `readguildname` refresh three different data tables.

**Detailed description**:
`log` calls `StartLog`, `StartChatLog` and `StartItemLog` in sequence (`imple.cpp:792-799`). Each closes the current file and opens a new timestamped one (`Server.cpp:6654`, `9208`, `9247`), so the command is a manual log rotation that also resets whatever day counters the writers maintain. There is no confirmation of the new file names and no failure path if the open fails.

The schedule writers are `GuildDay` (0..6), `GuildHour` (0..23), `NewbieHour` (0..23) and `ForceWeekMode`/`WeekMode` (`imple.cpp:800-838`). All four are validated with `x >= 0 && x <= N` where `x` is `unsigned int`, so the lower half of each comparison is vacuous and only the upper bound is enforced. `weekmode` additionally transforms its input: `WeekMode = ival1 - 1` and a wrap of `-1` to `5`, so the command's argument space 0..5 maps to an internal space 5,0,1,2,3,4. When the argument is outside 0..5 the handler *falls through* to `ForceWeekMode = -1` and replies `"WEEKLY GUILD MODE"`, which is a different behaviour from the in-range path that replies and returns — so the same command can either set or clear the override depending on the number.

The reload trio is heterogeneous: `reloadnpc` re-reads the generator file through `mNPCGen.ReadNPCGenerator()`, `reloadguild` re-reads guild data through `CReadFiles::ReadGuild()`, and `readguildname` rebuilds the guild-name table with `BASE_InitializeGuildName()` (`imple.cpp:839-853`). None of them validates that the underlying files exist, and none reports the number of records loaded; the acknowledgement is a fixed string.

**Rule workflow**:
```
log:           StartLog(); StartChatLog(); StartItemLog(); reply "RESTART LOG"
guildday N:    0 <= N <= 6  -> GuildDay = N, reply; else silent
guildhour N:   0 <= N <= 23 -> GuildHour = N, reply; else silent
newbiehour N:  0 <= N <= 23 -> NewbieHour = N, reply; else silent
weekmode N:    0 <= N <= 5  -> ForceWeekMode = N; WeekMode = (N-1), wrap -1 -> 5; reply; return
               otherwise    -> ForceWeekMode = -1; reply "WEEKLY GUILD MODE"
reloadnpc / reloadguild / readguildname -> reload table, reply fixed string
```

---

### Business Rule: BR-50, BR-51 — Treasure table editing with a second parse

**Overview**:
`trtarget`, `trsource` and `trrate` edit the eight-entry treasure table (`g_pTreasure[8]`) and persist configuration. `trtarget` is the only command that re-parses its own argument string with a different format.

**Detailed description**:
`trtarget <slot> <target> <item> <e0> <v0> <e1> <v1>` validates `1 <= ival1 <= 8` and `1 <= ival2 <= 5`, then discards the first parse and re-runs `sscanf(str + 1, "%s %s %s %d %d %d %d %d", cmd, sval1, sval2, &iv1, &iv2, &iv3, &iv4, &iv5)` (`imple.cpp:874`). The third token is therefore the item id, and the following four tokens are effect/value pairs. It writes `g_pTreasure[slot-1].Target[target-1]` with `sIndex`, two effect pairs and an explicit zeroing of the third pair, then calls `DrawConfig(1)` and replies `"+troktarget success"` (the string misspells the command).

`trsource <slot> <item>` validates the slot and writes `g_pTreasure[slot-1].Source = ival2` with no validation of the item id, so an id outside the item table can be stored and later dereferenced by the drop logic. `trrate <slot> <target> <rate>` validates both indices and writes `g_pTreasure[slot-1].Rate[target-1] = ival3` with no range check on the rate at all — unlike `set drop`, which at least bounds its rate at 8000.

All three persist by calling `DrawConfig(1)`, which rewrites `gameconfig.txt` serially and re-renders the server window (`Server.cpp:1117-1332`). The persistence format is positional and asymmetric: the reader (`ReadConfig`) validates fixed header strings and column counts (`Server.cpp:761-1160`) and rejects the file with a message box if any line does not match, so a mid-write crash during `DrawConfig` leaves a configuration file the next boot will refuse.

**Rule workflow**:
```
trtarget <1-8> <1-5> <item> <e0> <v0> <e1> <v1>:
  1. validate ival1 in 1..8 and ival2 in 1..5 -> else error message
  2. re-parse str+1: cmd, sval1, sval2(into iv1), iv2..iv5
  3. g_pTreasure[ival1-1].Target[ival2-1] = {iv1, iv2/iv3, iv4/iv5, 0}
  4. DrawConfig(1); reply "+troktarget success"
trsource <1-8> <item>:
  1. validate ival1 in 1..8
  2. g_pTreasure[ival1-1].Source = ival2
  3. DrawConfig(1); reply "+trsource success"
trrate <1-8> <1-5> <rate>:
  1. validate both indices
  2. g_pTreasure[ival1-1].Rate[ival2-1] = ival3
  3. DrawConfig(1); reply "+trrate success"
```

---

### Business Rule: BR-52 to BR-55 — Event engine configuration

**Overview**:
Eight `set ev*` keywords plus `set double`, `set deadpoint` and `set dungeonevent` configure the drop-event engine and two global toggles, and `event` prints the current state. `statsapphire` and `battleroyal` write two standalone economy values.

**Detailed description**:
The event engine is eight integers — `evItem`, `evStartIndex`, `evEndIndex`, `evCurrentIndex`, `evRate`, `evIndex`, `evDelete`, `evOn`, `evNotice` (`Server.cpp:299-307`). Seven keywords write one each and echo a formatted summary through the global `temp` before persisting with `DrawConfig(TRUE)`. The eighth, `set evon`, is the only one with cross-field validation: enabling the event requires `evStartIndex`, `evEndIndex`, `evRate` and `evItem` to all be non-zero, otherwise it prints the summary and returns without changing `evOn` (`imple.cpp:996-1011`). Disabling (`ival2 == 0`) is unconditional.

The weakest validation in this family is `statsapphire` and `battleroyal`, which both test `if (ival1 < 0)` on an `unsigned int` parameter (`imple.cpp:918-939`). The test is unreachable, so the only check is the message text, which advertises a range (`"err: +statsapphire (0-64)"`) that the code never enforces. `set double`, `set deadpoint` and `set dungeonevent` write `DOUBLEMODE`, `DEADPOINT` and `DUNGEONEVENT` with no validation whatsoever and reply with on/off strings produced by a ternary on the stored value.

Persistence is again `DrawConfig(TRUE)`, which serialises the whole economy surface — event settings, `double/deadpoint/dungeonevent/statsapphire/battleroyal`, billing settings, 64 drop-bonus cells, eight treasure rows with rates, and the etc settings block — into `gameconfig.txt` (`Server.cpp:1150-1323`). The event summary echo format is duplicated verbatim eight times; two variants exist (`[I:%d D:%d O:%d N:%d]` inside `set`, `[I:%d D:%d O:%d]` in `event`), so the read-only view omits `evNotice`.

**Rule workflow**:
```
set evstart/evend/evitem/evrate/evindex/evdelete/evnotice N:
  1. <field> = N
  2. temp = "item:%d index:%d-%d(%d) rate:1/%d [I:%d D:%d O:%d N:%d]"
  3. SendClientMessage(conn, temp); DrawConfig(TRUE)
set evon N:
  1. N != 0 and any of start/end/rate/item == 0 -> print summary; return (event stays off)
  2. evOn = N; print summary; DrawConfig(TRUE)
set double/deadpoint/dungeonevent N: <global> = N; reply; DrawConfig(TRUE)
statsapphire / battleroyal N: dead negative check; <global> = N; repaint form; DrawConfig(TRUE)
event: print summary (read-only)
```

---

### Business Rule: BR-56, BR-57 — Guild zone champion and challenger assignment

**Overview**:
`set champ` and `set chall` assign the charged (champion) guild and the challenger guild for one of five guild zones, persist the guild file and broadcast the zone report.

**Detailed description**:
Both commands share a shape: guard `conn`, validate `0 <= ival2 < MAX_GUILDZONE` (5), write `g_pGuildZone[ival2].ChargeGuild` or `.ChallangeGuild` from `ival3`, echo a formatted confirmation, and then persist. The guild id in `ival3` is not validated against the guild table or the server's own guild range (`ServerIndex * MAX_GUILD + counter`), so a zone can be assigned to a guild that does not exist; downstream consumers such as `GuildZoneReport` and the castle rendering simply propagate the number.

Only `set champ` calls `GuildZoneReport()`, which broadcasts `_MSG_GuildZoneReport` to the DB with the five charged guilds (`Server.cpp:739-754`), giving immediate cluster visibility of the castle ownership change. `set chall` does not send that report, so the challenger assignment becomes visible only through the guild file and the periodic readers — an asymmetry that means the two commands with nearly identical code have different propagation behaviour.

Both call `CReadFiles::WriteGuild()`, which serialises guild state to disk. Neither validates that the target zone is currently in a state where reassignment is legal (for example during an active castle war), and neither logs the previous value. The confirmation strings are the only feedback.

**Rule workflow**:
```
set champ <zone> <guild>:
  1. conn guard (tautology)
  2. 0 <= zone < 5 -> g_pGuildZone[zone].ChargeGuild = guild
  3. reply "Set Champion Guild - Zone:%d Guild %d"
  4. GuildZoneReport(); CReadFiles::WriteGuild()
set chall <zone> <guild>:
  1. conn guard (tautology)
  2. 0 <= zone < 5 -> g_pGuildZone[zone].ChallangeGuild = guild
  3. reply "Set Challanger Guild - Zone:%d Guild %d"
  4. CReadFiles::WriteGuild()
```

---

### Business Rule: BR-58 — Drop-bonus matrix editing

**Overview**:
`set drop <position> <rate>` writes cells of the 64-entry drop-bonus matrix using a three-way position scheme: `16` means "all", `1..7` means "eight cells of a class block", `8..15` means "one cell in the tail block".

**Detailed description**:
The rate is validated with `if (ival3 < 0 || ival3 > 8000)`, where `ival3` is unsigned, so only the upper bound is live (`imple.cpp:1082-1086`). The position logic is the most intricate mapping in the component: `pos == 16` writes all 64 cells; `1 <= pos <= 7` writes `idx = (pos - 1) * 8 + i` for `i` in 0..7, covering 0..55; `8 <= pos <= 15` writes the single cell `56 + (pos - 8)`, covering 56..63; anything else replies `"Wrong Drop Position"`. The scheme therefore allocates 56 cells to seven "class" blocks of eight and the last eight cells to individual positions, with 16 as a broadcast sentinel — a layout that matches the four-line, sixteen-column rendering in `DrawConfig` (`Server.cpp:1238-1254`), which prints 64 values in 4 rows of 16.

The sentinel choice is notable: `16` is also a valid-looking position in a 1..16 numbering, but it means "all", so the numbering is not contiguous with its own semantics. There is no read-back command for a single cell; the only way to inspect the matrix is the server window or `gameconfig.txt`. Rates are stored as raw `int` and consumed by the drop roll without a documented unit.

**Rule workflow**:
```
1. rate: ival3 > 8000 -> "Wrong Drop Bonus Rate"; return   (ival3 < 0 is unreachable)
2. pos == 16                -> for i in 0..63: g_pDropBonus[i] = rate
   pos in 1..7              -> for i in 0..7: g_pDropBonus[(pos-1)*8 + i] = rate
   pos in 8..15             -> g_pDropBonus[56 + (pos-8)] = rate
   otherwise                -> "Wrong Drop Position"; return
3. reply "SET DROP BONUS"; DrawConfig(TRUE)
```

---

### Business Rule: BR-59 to BR-63 — World state, diagnostics and billing link

**Overview**:
The first level-1 block contains `weather`, `attmap` and `billconnect`; the second contains `impost` and `trn`. Together they are the lowest-privilege operations that touch world or infrastructure state.

**Detailed description**:
`weather <n>` writes both `ForceWeather` and `CurrentWeather` and calls `SendWeather()`, which pushes the new weather to connected clients (`imple.cpp:1121-1126`). There is no range validation and no way to return to "automatic" other than setting a value that the weather scheduler also produces. `attmap` is read-only: it fetches `GetAttribute(TargetX, TargetY)` and prints it with a Portuguese label (`"Atributo do mapa é %d"`, `imple.cpp:1131`), making it one of the few commands with a localised string rather than a hard-coded English one.

`billconnect` is an infrastructure command with a file dependency: it refuses if `BillServerSocket.Sock != NULL`, opens `biserver.txt`, reads an address and port with `fscanf("%s %d")`, and calls `ConnectBillServer(address, port, LocalIP, WSA_READBILL)` (`imple.cpp:1134-1162`). On success it forces `BILLING = 3` and repaints config. The file name is relative to the process working directory and is not validated beyond existence, and the address buffer is a fixed global (`BillServerAddress[32]`) that `%s` can overrun with a long token in the file.

`impost` zeroes `Exp` on the five mobs indexed by `GuildImpostoID[0..4]` (`imple.cpp:1166-1175`). `trn` teleports the admin to (4000,4000) and immediately back (`imple.cpp:1177-1183`), a view-refresh trick that reuses `DoTeleport`. Neither validates that the indices or coordinates are sane; `impost` does not even check that the mobs are alive.

**Rule workflow**:
```
weather N:     ForceWeather = N; CurrentWeather = N; SendWeather()
attmap:        GetAttribute(TargetX, TargetY) -> sprintf + SendClientMessage
billconnect:   if BillServerSocket.Sock != NULL -> "already connected"; return
               fopen("biserver.txt") -> NULL => "no biserver.txt"; return
               fscanf("%s %d", BillServerAddress, &BillServerPort)
               ConnectBillServer(...) -> failure => "set connect fails"; return
               BILLING = 3; DrawConfig(1)
impost:        pMob[GuildImpostoID[0..4]].MOB.Exp = 0; reply "IMPOST"
trn:           x = TargetX; y = TargetY; DoTeleport(4000,4000); DoTeleport(x,y)
```

---

### Business Rule: BR-64, BR-69, BR-70 — Three broadcast mechanisms

**Overview**:
`notice` forwards to the DB for cluster-wide delivery, `chiefnotice` targets guild leaders only, and `chiefsummon` sends a chief summon signal. Three separate broadcast paths exist with different reach, and one of them duplicates an existing whisper command (`/not`).

**Detailed description**:
`notice <text>` copies `MESSAGE_LENGTH` bytes starting at `str + 7` (skipping `"notice "`), truncates to `MESSAGE_LENGTH-1`, builds `MSG_DBNotice` with `ID = 0`, and sends it to the DB socket (`imple.cpp:1185-1201`). The DB side turns `_MSG_DBNotice` into `SendNotice` for every game server (`ProcessDBMessage.cpp:141-146`), so this is the only command in the component that reaches players on other servers. The copy reads up to three bytes past the 100-byte whisper string; the underlying receive buffer is larger, so the practical effect is bounded, but the length arithmetic assumes a 96-byte source that does not match the 100-byte field.

`chiefnotice <text>` copies from `str + 12` (`"chiefnotice "`) and calls `SendNoticeChief`, which iterates all slots, keeps only `USER_PLAY` characters with `GuildLevel == 9` and a positive guild, and sends the message (`SendFunc.cpp:64-101`). It also logs the notice as `not <text>` and therefore produces a second audit line in addition to the generic `adm` line. `chiefsummon` calls `SendSummonChief()` and requires `0 < conn < MAX_USER` in the branch condition itself (`imple.cpp:1503`), making it the only command where a connection check is part of the dispatch guard rather than a body statement.

The duplication is structural: the whisper handler already implements `/not` for level >= 1000 (`_MSG_MessageWhisper.cpp:1088-1101`) with the same DB packet, so there are two independent, differently-named paths to the same broadcast primitive, plus a third (`chiefnotice`) with different filtering.

**Rule workflow**:
```
notice <text>:      temp = str+7 (96 bytes) -> MSG_DBNotice(ID=0) -> DBServerSocket.SendOneMessage
                    DB broadcasts to every game server -> SendNotice to every USER_PLAY
chiefnotice <t>:    temp = str+12 -> SendNoticeChief -> only GuildLevel == 9 and Guild > 0
                    also logs "not <t>" via Log
chiefsummon:        conn in (0, MAX_USER) -> SendSummonChief(); reply "CHIEFSUMMON"
```

---

### Business Rule: BR-65, BR-67, BR-68 — Experience rebalancing by rewriting NPC files

**Overview**:
`waterexp`, `svexp` and `nigexp` recompute NPC experience values and write them directly into the `./npc/` files, then reload the generator table. Two of them divide by the configurable `FREEEXP`; the third divides by a caller-supplied level without a zero check.

**Detailed description**:
`waterexp <room> <xp>` selects one of three index ranges by the room argument: room 0 uses `WATER_N_INITIAL .. WATER_N_INITIAL+8` (171..179), room 1 uses `WATER_M_INITIAL .. WATER_N_INITIAL+8` (10..179 — the termination constant belongs to the *N* family, so the M branch iterates 170 generators instead of nine), and any other value uses `WATER_A_INITIAL .. WATER_A_INITIAL+8` (183..191) (`imple.cpp:1203-1309`). For each index it converts spaces to underscores in the template name, opens `./npc/<name>`, computes `Exp = xp * Level / FREEEXP`, writes the whole `STRUCT_MOB`, and closes. A failed open logs `"fail - save npc"` and returns, leaving a partially rewritten set.

`svexp <level> <xp>` iterates every generator from 1 to `NumList`, rewrites both the leader and the follower file, and computes the leader experience as `Level < lv ? xp : xp * Level / lv`, with the follower always `xp * Level / lv` (`imple.cpp:1323-1385`). `lv` is the caller's first argument, taken as `int` from an unsigned value; there is no check for zero, and the division is executed for any generator whose level is at or above `lv`, so `svexp 0 <xp>` divides by zero. `nigexp <room> <xp>` mirrors `waterexp` for the three nightmare ranges (2368..2375, 2377..2384, 2385..2394) and divides by `FREEEXP` (`imple.cpp:1387-1493`).

All three end with `mNPCGen.ReadNPCGenerator()` so the in-memory table matches the files, and all three reply with a fixed success string. Because `FREEEXP` is writable at level 3 (`set billfree`), a level-1 operator can trigger a division by zero in two of them by first setting `FREEEXP = 0` — a two-command path to a process-level fault.

**Rule workflow**:
```
waterexp <room> <xp>:
  1. room == 0 -> indices 171..179
     room == 1 -> indices 10..179   [sic: bound uses WATER_N_INITIAL + 8]
     else      -> indices 183..191
  2. per index: name = MobName with ' ' -> '_'; path = "./npc/" + name
  3. _open(_O_CREAT|_O_RDWR|_O_BINARY) -> -1 => Log("fail - save npc"); return
  4. Exp = xp * Level / FREEEXP; _write(STRUCT_MOB); _close
  5. mNPCGen.ReadNPCGenerator(); reply "WATER EXP CHANGED"
svexp <lv> <xp>:
  1. per generator 1..NumList: leader Exp = Level < lv ? xp : xp * Level / lv
     follower Exp = xp * Level / lv        [division by lv, no zero check]
  2. write both files; ReadNPCGenerator(); reply "EXP CHANGED"
nigexp <room> <xp>:
  1. room 0 -> 2368..2375; room 1 -> 2377..2384; else 2385..2394
  2. Exp = xp * Level / FREEEXP; write; ReadNPCGenerator(); reply
```

---

### Business Rule: BR-66 — `testegrid` grid flooding

**Overview**:
`testegrid` writes the value `1` into every cell of the mob grid from 2100 to 4095 on both axes, marking roughly four million cells as occupied by mob slot 1.

**Detailed description**:
The double loop at `imple.cpp:1313-1320` covers `x` in 2100..4095 and `y` in 2100..4095 and, where the cell is zero, sets it to `1`. `pMobGrid` is `unsigned short pMobGrid[MAX_GRIDY][MAX_GRIDX]` (`Server.h:309`), a 32 MB global, so the write touches approximately 1,996 × 1,996 ≈ 3.98 million cells. The value `1` is a mob slot index, and slot 1 is a legitimate user slot, so the grid now claims that mob 1 occupies a quarter of the map.

There is no acknowledgement, no confirmation, no reversal command, and no undo. Downstream, `GridMulticast` resolves each cell to `pMob[tmob]` and filters on `Mode == MOB_EMPTY` (`SendFunc.cpp:843-888`), so the exact consequences depend on mob 1's state at that moment: with a live player in slot 1, the surrounding teleport/attack visibility logic sees a phantom occupant in every cell of the quadrant. The command is reachable at the lowest admin tier (level 1) with no arguments and no preconditions.

This is the clearest case in the component of a "test" entry point being shipped in the production dispatch table, and it is also the clearest case where the tautological `conn` guard would have been the only barrier had it been written correctly.

**Rule workflow**:
```
1. cmd == "testegrid"
2. for x in 2100..4095:
     for y in 2100..4095:
       if pMobGrid[y][x] == 0: pMobGrid[y][x] = 1
3. No reply, no audit beyond the generic "adm" line, no reversal path
```

---

### Business Rule: BR-71, BR-49 — Generator rebuild and reload discipline

**Overview**:
`rebuild` recounts live mobs per generator and rewrites the generator's population counter; `reloadnpc` re-reads the generator definition file from disk.

**Detailed description**:
`RebuildGenerator()` clears `mNPCGen.NumOld[]` for the whole list, counts every live mob from `MAX_USER` to `MAX_MOB` by its `GenerateIndex`, then writes each generator's `CurrentNumMob` back and logs every difference (`Server.cpp:3363-3390`). The command therefore reconciles the spawn budget with reality, which is the recovery path after population drift caused by force-kills, `createkefra`, or `generate` spam. Its loop bound on the second pass is `MAX_USER .. mNPCGen.NumList`, which is only safe because `NumList <= MAX_NPCGENERATOR (8192) < MAX_MOB (25000)`.

`reloadnpc` discards the in-memory table and re-reads the file, which resets every `CurrentNumMob` to the file's value and therefore *undoes* what `rebuild` just computed. The two commands are adjacent in the dispatch table but semantically opposed, and nothing in the acknowledgement strings hints at that interaction. The same tension exists between `waterexp`/`svexp`/`nigexp` (which write files and reload) and `rebuild` (which recomputes counters from the live world) — a full exp rebalance followed by a reload resets the population bookkeeping.

**Rule workflow**:
```
rebuild:
  1. for i in 0..NumList: NumOld[i] = 0
  2. for mob in MAX_USER..MAX_MOB: if Mode != 0 -> NumOld[GenerateIndex]++
  3. for i in MAX_USER..NumList: if CurrentNumMob != NumOld[i] -> log diff; CurrentNumMob = NumOld[i]
  4. reply "REBUILDGENERATOR"
reloadnpc:
  1. mNPCGen.ReadNPCGenerator()
  2. reply "RELOADNPC"   (resets CurrentNumMob to file values)
```

---

### Business Rule: BR-72, BR-73 — Chat moderation

**Overview**:
`mute`, `muteall` and `desmuteall` toggle `pUser[].MuteChat`. The single-target form resolves a name; the broadcast forms abort on the first slot that is not at least in character-selection state.

**Detailed description**:
`mute <name>` resolves the target through `GetUserByName`, which skips empty mobs and non-`USER_PLAY` accounts and compares either the character name or, with a leading `+`, the account name (`Server.cpp:6793-6827`). A miss replies `_NN_Not_Connected`; a hit toggles `MuteChat` between 0 and 1 and sends `_NN_Chat_Prohibition` to the target (`imple.cpp:1514-1532`). The toggle semantics mean the command cannot set an absolute state: issuing `mute` twice unmutes, so an operator cannot reliably enforce a mute without knowing the prior value.

`muteall` and `desmuteall` loop from slot 1 to `MAX_USER-1` and contain `if (pUser[user].Mode < USER_SELCHAR) return;` inside the loop (`imple.cpp:1536-1539, 1552-1555`). `USER_SELCHAR` is 11 and `USER_EMPTY` is 0 (`CUser.h:26-36`), so the first empty slot terminates the command. In practice the loop mutes or unmutes only the contiguous run of busy slots starting at 1, and because slot allocation reuses the lowest free index the effective coverage is unpredictable. The intended `continue` is missing.

Neither bulk command reports how many accounts were affected, and `desmuteall` does not notify the target players (unlike `muteall`, which sends the prohibition message). `snoop` (BR-74) is in the same block and is the only moderation-adjacent command that touches the admin's own record rather than a target's.

**Rule workflow**:
```
mute <name>:
  1. user = GetUserByName(<name>) -> 0 => "Not Connected"; return
  2. pUser[user].Mode != USER_PLAY -> return
  3. MuteChat = (MuteChat == 0) ? 1 : 0
  4. reply "MUTE" to admin; send _NN_Chat_Prohibition to target; return
muteall:
  for user in 1..MAX_USER-1:
     if pUser[user].Mode < USER_SELCHAR -> return        [aborts the whole loop]
     MuteChat = 1; send _NN_Chat_Prohibition to user
  reply "MUTE ALL"
desmuteall:
  for user in 1..MAX_USER-1:
     if pUser[user].Mode < USER_SELCHAR -> return
     MuteChat = 0
  reply "DESMUTE ALL"
```

---

### Business Rule: BR-74 — Snoop bit toggle

**Overview**:
`snoop` flips the `MSV_SNOOP` (0x01) bit in `MOB.Merchant` for the issuing admin and resends score.

**Detailed description**:
The branch guards `conn` with the tautology, then tests `pMob[conn].MOB.Merchant & MSV_SNOOP`. If set, it clears with `& 0xFE` and replies `"SNOOP OFF"`; otherwise it sets with `| MSV_SNOOP` and replies `"SNOOP ON"` (`imple.cpp:1564-1583`). `MSV_SNOOP` is defined as `0x01` (`Basedef.h:194`), and the clear uses a hard-coded `0xFE` rather than `~MSV_SNOOP`, so the two halves of the toggle are not expressed in the same terms.

The bit lives in `STRUCT_MOB::Merchant`, an `unsigned char` that is also written by `set merchant` (BR-17) and by the login path (`ProcessDBMessage.cpp:79` zeroes `pMob[conn].MOB.Merchant` at login). A login therefore silently clears the snoop flag, and `set merchant 0` does the same without any indication. `GetCurrentScore` is invoked afterwards and the score is resent, which also copies `BaseScore.Merchant` into `CurrentScore.Merchant`.

**Rule workflow**:
```
1. conn guard (tautology)
2. if MOB.Merchant & 0x01 -> MOB.Merchant &= 0xFE; reply "SNOOP OFF"
   else                   -> MOB.Merchant |= 0x01; reply "SNOOP ON"
3. GetCurrentScore(conn); SendScore(conn); return
```

---

### Business Rule: BR-75 to BR-79 — Self-service character shaping

**Overview**:
`learn`, `class`, `buff`, `nobuff`, `soul` and `event` operate on the admin's own character or print state. They are the lowest-tier commands with the largest direct effect on combat power.

**Detailed description**:
`learn` sets `MOB.LearnedSkill = -1`, i.e. all 32 skill bits, and pushes ETC (`imple.cpp:1590-1595`). Because `LearnedSkill` is a `long` and every skill gate in the server tests one bit (for example `learn & 0x40000` for summon, `1 << 10` for the hunter perk, `1 << 28` for concentration), the command grants every passive and active ability in one write. `class <n>` writes `MOB.Class` with no validation against the class table (`0..3` in practice; `Class == 2` triggers the shield/reflect branches in `GetCurrentScore`), so an out-of-range class silently disables class-dependent logic.

`buff <n>` writes only `Affect[0]` with `Type = n`, `Time = 162000` and `Value = 0` (`imple.cpp:1604-1612`), overwriting whatever affect occupied slot 0; because `Time` is a fixed large constant the effect is effectively permanent until cleared. `nobuff` clears all 32 slots including `Level` and `Value` (`imple.cpp:1614-1626`), which is the only way to remove affects in bulk. Neither validates the affect type against the affect table, and neither checks whether the affect is already running.

`soul <n>` writes `extra.Soul`, the selector used by `BASE_GetCurrentScore` to multiply Intelligence or Constitution by 1.4x, 1.8x or 2.2x when affect type 29 is active and the class master is neither mortal nor arch (`Basedef.cpp:3009-3033`). The command therefore only has an effect in combination with a `buff 29`, which is a cross-command dependency that no validation expresses.

**Rule workflow**:
```
learn:   LearnedSkill = -1; SendEtc; reply "LEARN"
class N: MOB.Class = N; SendEtc; reply "CLASS"
buff N:  Affect[0] = {Type: N, Time: 162000, Value: 0}; SendScore; reply "BUFF"
nobuff:  for i in 0..MAX_AFFECT-1: Affect[i] = {0,0,0,0}; SendScore; reply "NOBUFF"
soul N:  extra.Soul = N; reply "SOUL"
event:   print event summary
```

---

### Business Rule: BR-80 — Guild fame adjustment over the DB link

**Overview**:
`fame <guild> <delta>` adds a delta to a guild's fame record and pushes the updated structure to the DB server, which then broadcasts it to every game server.

**Detailed description**:
The handler validates `0 < ival1 < 65536`, copies the current `GuildInfo[ival1]` into a `MSG_GuildInfo` packet after adding `ival2` to `.Fame`, and sends it to `DBServerSocket` (`imple.cpp:1634-1655`). The guild id bound matches the 16-bit guild space; the delta is an `unsigned int` added to an `int` field, so a large value wraps into negative fame, and there is no floor or ceiling.

Because the packet carries the full `STRUCT_GUILDINFO`, the command is a read-modify-write of the entire guild record from the game server's local copy — a concurrent modification on another server is overwritten rather than merged. The DB side stores the structure and rebroadcasts (`CFileDB.cpp:454-470`), so the last writer wins across the cluster.

There is no acknowledgement to the operator: the branch sends nothing on success and nothing on an out-of-range guild. The only evidence is the DB-side log of `_MSG_GuildInfo` handling and the generic `adm` line.

**Rule workflow**:
```
1. if not (0 < guild < 65536) -> no reply, no effect
2. sm.Type = _MSG_GuildInfo; sm.ID = 0; sm.Guild = guild
3. GuildInfo[guild].Fame += delta
4. sm.GuildInfo = GuildInfo[guild]
5. DBServerSocket.SendOneMessage(sizeof(MSG_GuildInfo)) -> DB stores + rebroadcasts
```

---

### Business Rule: BR-81, BR-84 — Kefra encounter control

**Overview**:
`killkefra` force-kills every mob generated from the Kefra boss generator; `createkefra` resets the Kefra live flag and regenerates the boss plus four adds.

**Detailed description**:
`killkefra` iterates all monster slots (`MAX_USER .. MAX_MOB-1`), and for each with `GenerateIndex == KEFRA_BOSS` (396, `Basedef.h:388`) calls `MobKilled(i, conn, 0, 0)` (`imple.cpp:1657-1664`). `MobKilled` performs the full death pipeline — loot, experience attribution to `conn`, quest hooks, generator counter decrement — so the command does not merely remove the mobs, it awards every kill to the issuing admin. There is no check that the encounter is active, and no limitation to a single instance.

`createkefra` sets `KefraLive = 0`, calls `GenerateMob(KEFRA_BOSS, 0, 0)`, then calls `GenerateMob` for every index in `KEFRA_MOB_INITIAL .. KEFRA_MOB_END` (397..400), then persists with `DrawConfig(TRUE)` (`imple.cpp:1724-1736`). The two zero coordinates are passed straight into `GenerateMob`, which derives placement from the generator's own segment lists rather than from the arguments, so the zeros are placeholders rather than a location. Unlike `generate`, this path performs no bounds check on the indices; safety depends on the constants being below `NumList`, which is not verified at runtime.

The pair is asymmetric: `createkefra` resets `KefraLive` before spawning, but `killkefra` never sets it, so the flag's meaning differs between the two commands and neither reflects the actual live count.

**Rule workflow**:
```
killkefra:
  for i in MAX_USER..MAX_MOB-1:
     if pMob[i].GenerateIndex == KEFRA_BOSS -> MobKilled(i, conn, 0, 0)
  reply "KILLKEFRA"
createkefra:
  KefraLive = 0
  GenerateMob(KEFRA_BOSS, 0, 0)
  for i in KEFRA_MOB_INITIAL..KEFRA_MOB_END: GenerateMob(i, 0, 0)
  DrawConfig(TRUE); reply "CREATE KEFRA"
```

---

### Business Rule: BR-82 — `npko`: hard-coded identity and cross-process file write

**Overview**:
`npko` rewrites the DB server's `Admin.txt` allow-list with the issuer's IP, but only if the issuer's account name is one of three literals compiled into the binary.

**Detailed description**:
The command first calls `CReadFiles::ReadAdmin()`, then opens `"../../DBSRV/Run/Admin.txt"` for writing (`imple.cpp:1666-1703`). It then compares `pUser[conn].AccountName` against `"VERBANSKI"`, `"MATEUS654"` and `"PTR0X"` to select an index 0, 1 or 2, and returns silently if none matches. On a match it writes the issuer's `pUser[conn].IP` into `pAdminIP[index]` and then serialises every non-zero entry as `"<idx> <a>.<b>.<c>.<d>"`.

This is the only place in the entire repository where an authorization decision is made from a literal account name rather than from data, and it is the only place where TMSrv writes into DBSrv's directory. The written file is the input to DBSrv's admin-client IP allow-list (`CReadFiles::ReadAdmin`, `CReadFiles.cpp:640-676`; consumer at `DBSrv/Server.cpp:985-1000`), so the command is a privilege-granting operation on a *different process*: it can add an IP to the allow-list that gates the `_MSG_MessageDBImple` channel. The write itself does not validate the path, the file handle (`fp == NULL` is handled by skipping the write while still replying success), or whether the resulting file is consistent with DBSrv's in-memory `pAdminIP` (which only refreshes when DBSrv re-reads Admin.txt).

The three hard-coded names also mean the command's capability set is fixed at build time; there is no configuration path, no group membership, and no audit beyond the generic `adm` line. Because the account names are compared with `strcmp` against a fixed-length field, the check is exact and case-sensitive.

**Rule workflow**:
```
1. CReadFiles::ReadAdmin()                       [reload TMSrv copy of the allow-list]
2. fp = fopen("../../DBSRV/Run/Admin.txt", "wt")
3. IPPos = -1
   AccountName == "VERBANSKI"  -> 0
   AccountName == "MATEUS654"  -> 1
   AccountName == "PTR0X"      -> 2
4. IPPos == -1 -> return silently
5. pAdminIP[IPPos] = pUser[conn].IP
6. if fp != NULL: for x in 0..MAX_ADMIN-1: if pAdminIP[x] != 0 -> fprintf "%d a.b.c.d"
7. reply "NPKO IP CHANGED"
```

---

### Business Rule: BR-83 — Targeted mob kill by name

**Overview**:
`kill <name>` zeroes the HP of every monster whose name matches the argument and runs the full death pipeline for each.

**Detailed description**:
The name is normalised by replacing `_` with space, then compared with `strncmp(pMob[i].MOB.MobName, sval1, NAME_LENGTH)` over all monster slots (`i` from `MAX_USER` to `MAX_MOB-1`) (`imple.cpp:1705-1722`). The comparison length is the full 16-byte field, so a shorter argument compares its terminating NUL against the stored byte and only matches if the stored name is exactly the argument. The `strncmp` is effectively a prefix comparison bounded by the field width.

For each match the handler sets `CurrentScore.Hp = 0` and calls `MobKilled(i, conn, 0, 0)`, which credits the kill, runs loot and quest logic and decrements the generator. Multiple mobs sharing a template name are all killed, so a single command can eliminate an entire spawn wave and award all of it to the issuer. There is no name validation, no confirmation, and no cap on the number of kills.

The command cannot target players: the loop starts at `MAX_USER`, so character names are out of scope regardless of the argument.

**Rule workflow**:
```
1. replace '_' with ' ' in sval1
2. for i in MAX_USER..MAX_MOB-1:
     if strncmp(pMob[i].MOB.MobName, sval1, NAME_LENGTH) == 0:
        pMob[i].MOB.CurrentScore.Hp = 0
        MobKilled(i, conn, 0, 0)
3. reply "KILL"
```

---

### Business Rule: BR-85 — Global schedule and economy writers

**Overview**:
`partydif`, `rvrhour`, `gtorrehour`, `dropitem` and `maxnightmare` write single global values and persist the full configuration file.

**Detailed description**:
Each command is a three-line body: assign the global from `ival1`, call `DrawConfig(TRUE)`, reply with a fixed string (`imple.cpp:1738-1782`). None validates a range. The globals themselves have different consumers: `PARTY_DIF` (default 150) governs party level-difference rules, `RvRHour` (22) and `GTorreHour` (22) drive the RvR and GTorre schedules, `isDropItem` (0) gates ground-item dropping, and `maxNightmare` (3) caps the nightmare instance count. `BRHour` is written to the config file by `DrawConfig` but has no command that sets it, so it can only change through the file.

Because `DrawConfig(TRUE)` serialises the entire economy block, a single `partydif` command rewrites the file that also carries the event engine, drop matrix, treasure table and billing settings. A failure mid-write (or a concurrent edit) invalidates the whole file, and `ReadConfig` will reject it on the next boot with a message box rather than falling back to defaults.

**Rule workflow**:
```
partydif N:     PARTY_DIF = N; DrawConfig(TRUE); reply "PARTY DIF"
rvrhour N:      RvRHour = N; DrawConfig(TRUE); reply "RVR HOUR"
gtorrehour N:   GTorreHour = N; DrawConfig(TRUE); reply "GTorre HOUR"
dropitem N:     isDropItem = N; DrawConfig(TRUE); reply "DROP ITEM"
maxnightmare N: maxNightmare = N; DrawConfig(TRUE); reply "MAXNIGHTMARE"
```

---

### Business Rule: BR-86, BR-87 — Emotion and coordinate-aware teleport

**Overview**:
`emotion <motion> <parm>` sends one emotion packet; `teleport <x> <y>` accepts either cell coordinates (>= 100) or sector coordinates (< 100) and bounds-checks the result.

**Detailed description**:
`emotion` calls `SendEmotion(conn, ival1, ival2)` and acknowledges (`imple.cpp:1785-1789`). The motion and parameter values are passed to the client without validation against the animation table, so an arbitrary pair is sent; the packet is unicast to the issuer only.

`teleport` is one of the few commands with real input normalisation: if a coordinate is below 100 it is treated as a sector index and converted with `x * 128 + 64` (the centre of the 128-cell sector), and the result is accepted only when `0 <= x < MAX_GRIDX` and `0 <= y < MAX_GRIDY` (4096) (`imple.cpp:1792-1801`). Because the parameters are unsigned, the lower bounds are vacuous and only the upper bounds matter. On rejection the command is completely silent — no message distinguishes "out of range" from "teleported successfully".

The conversion threshold means the coordinates 0..99 are impossible to target as raw cells, so the two coordinate spaces overlap at 100..127 cells versus sectors 0 (which maps to 64). An operator targeting cell 100 and an operator targeting sector 0 both end up near 64..100 depending on the path, and there is no query command to disambiguate.

**Rule workflow**:
```
emotion <motion> <parm>: SendEmotion(conn, motion, parm); reply "EMOTION"
teleport <x> <y>:
  1. if x < 100: x = x * 128 + 64
     if y < 100: y = y * 128 + 64
  2. if 0 <= x < 4096 and 0 <= y < 4096 -> DoTeleport(conn, x, y)
  3. else silent
```

---

### Business Rule: BR-88 — Target kick with level ordering

**Overview**:
`kick <name>` resolves a playing character, refuses when the target's current level is greater than or equal to the issuer's, and then logs the target out and closes the socket.

**Detailed description**:
The handler rejects an empty argument, truncates the name to 14 bytes (`sval1[NAME_LENGTH-1] = 0; sval1[NAME_LENGTH-2] = 0;`), resolves it with `GetUserByName`, and refuses if the target is not `USER_PLAY` (`imple.cpp:1802-1828`). It then compares `pMob[tid].MOB.CurrentScore.Level >= pMob[conn].MOB.CurrentScore.Level` and refuses with `"Can't kick equal or high level admin"`. The comparison is on the derived level, not on the admin flag, and it is the only peer-protection rule in the component: it prevents a lower-tier admin from kicking a higher-tier one, while allowing an admin to kick any character below its own level, including non-admin players.

On success the handler calls `CharLogOut(tid)` followed by `CloseUser(tid)`. `CharLogOut` persists the character, removes it from the grid and returns the connection to `USER_SELCHAR` (`Server.cpp:7085-7125`); `CloseUser` then closes the socket and frees the slot (`Server.cpp:6834+`). The pair is the same sequence used by the normal disconnect path, which is why the target's data survives the kick. Note that `CharLogOut` also refuses when the target is not `USER_PLAY`, so the guard at the top of `kick` and the guard inside `CharLogOut` are redundant.

The self-kick case is blocked implicitly: `GetUserByName` returns the issuer's own slot, and the equality branch refuses. There is no kick-by-account path (unlike `mute`, which supports a `+account` prefix through `GetUserByName`), so renamed characters cannot be targeted by account.

**Rule workflow**:
```
1. sval1 empty -> return
2. sval1[14] = 0; sval1[15] = 0
3. tid = GetUserByName(sval1) -> 0 => "Can't find user."; return
4. pUser[tid].Mode != USER_PLAY -> return
5. pMob[tid].Level >= pMob[conn].Level -> "Can't kick equal or high level admin"; return
6. CharLogOut(tid); CloseUser(tid)
```

---

### Business Rule: BR-89, BR-90 — Mass summoning

**Overview**:
`gsummon <guild>` teleports up to 48 members of one guild to the issuer's target cell; `allsummon` teleports every eligible player to the same cell.

**Detailed description**:
`gsummon` guards `conn` with the tautology and calls `SummonGuild(ival1, TargetX, TargetY, 48, -1)` (`imple.cpp:1830-1836`). `SummonGuild` iterates all user slots, filters on `USER_PLAY`, matching guild, and `!OnlyTrade`, then applies a village filter whose condition is written as `if (village && village != 1 && village != 3 || BASE_GetVillage(...) == village)` (`Server.cpp:5701`) — with the passed `village == -1`, the first conjunct is true, so the village test is irrelevant and every guild member is teleported. The positional offset logic staggers the destination after 30 and again after 45 summons, and the loop stops at `maxsummon` (48).

`allsummon` captures `TargetX`/`TargetY` and calls `SummonServer(x, y)` (`imple.cpp:1837-1844`). Unlike `gsummon`, it performs no guild filter and no cap expressed at the call site; the population-wide teleport depends entirely on `SummonServer`'s internal filtering (`Server.cpp:8914`). Both commands acknowledge only in the `allsummon` case; `gsummon` is silent on success.

Neither command records which players were moved, and neither checks whether the destination cell is a valid, non-blocked map position — the grid placement is delegated to the summon routine.

**Rule workflow**:
```
gsummon <guild>:
  1. conn guard (tautology)
  2. SummonGuild(guild, TargetX, TargetY, 48, -1)
  3. no reply
allsummon:
  1. x = TargetX; y = TargetY
  2. SummonServer(x, y)
  3. reply "ALLSUMMON"
```

---

### Business Rule: BR-37 detail — `SaveAll` persistence semantics

**Overview**:
`SaveAll` is the only bulk persistence primitive in the component and is exposed both as a command (`saveall`, level 3) and as a server-window menu item.

**Detailed description**:
The loop at `imple.cpp:35-42` walks all 1000 slots and acts only on `pUser[i].Mode == USER_PLAY`. For each such slot it calls `CharLogOut(i)`, which writes the character through `SaveUser`, removes it from `pMobGrid` via `DeleteMob`, resets the position to the saved point and notifies the client with `_MSG_CNFCharacterLogout`. The player is not disconnected; the account returns to character selection. The command therefore has a visible gameplay effect (everyone is returned to the character-select screen) in addition to the persistence effect.

Each successful logout also emits a `Log("saveall", account, IP)` line, so the operation produces up to 1000 log lines plus the caller's own `adm` line. There is no summary count, no failure tracking, and no verification that the DB write succeeded — `CharLogOut` reports failures internally via `Log` but returns nothing to `SaveAll`.

The same function is bound to the `IDC_SAVEALL` menu item (`Server.cpp:4072, 4097-4098`), which means the operation is reachable both through the privileged chat surface and through local GUI interaction; the GUI path is not audited with an `adm` line.

**Rule workflow**:
```
1. for i in 0..MAX_USER-1:
     if pUser[i].Mode == USER_PLAY:
        CharLogOut(i)          [SaveUser + DeleteMob + _MSG_CNFCharacterLogout]
        Log("saveall", pUser[i].AccountName, pUser[i].IP)
2. reply "SAVEALL" (command path only)
```

---

## 4. Component Structure

```
Code/TMSrv/
├── imple.cpp                      # THE COMPONENT (1,846 lines, 46,572 bytes)
│   ├── :1-17                      # GPL header
│   ├── :18-31                     # Includes: windows.h, Basedef.h, CPSock.h, ItemEffect.h,
│   │                              #   Language.h, CItem.h, Server.h, GetFunc.h, SendFunc.h,
│   │                              #   ProcessClientMessage.h, ProcessDBMessage.h, CCastleZakum.h
│   ├── :33-44                     # SaveAll()  — bulk persistence helper (only other symbol)
│   ├── :46-1846                   # ProcessImple() — single dispatcher function
│   │   ├── :48-78                 #   9 x char[128] + 9 numeric mirrors + sscanf parsing
│   │   ├── :80-87                 #   unconditional audit log
│   │   ├── :89-568                #   level >= 8 block  (set family, save, read, name)
│   │   │   ├── :94-507            #     set keyword chain (39 keywords) + positional equip
│   │   │   └── :508-567           #     save / read / name
│   │   ├── :569-645               #   level >= 3 block  (timer, saveall, decay, set economy)
│   │   ├── :647-1117              #   level >= 2 block  (lifecycle, spawn, gift, cp/frag, log,
│   │   │                          #     schedules, reloads, treasure, event engine, set toggles)
│   │   ├── :1119-1163             #   level >= 1 block A (weather, attmap, billconnect)
│   │   └── :1164-1845             #   level >= 1 block B (world, broadcast, bulk exp, moderation,
│   │                              #     self-service, teleport, summon)
│   └── (no internal helper functions, no anonymous namespace, no classes)
│
├── Server.h                       # Declarations: ProcessImple (:190), SaveAll (:169),
│                                  #   extern char temp[4096] (:382), extern int ServerDown (:198),
│                                  #   extern unsigned short pMobGrid[][] (:309), pAdminIP (:318)
├── Server.cpp                     # Callee hub: Log (:6701), GetUserByName (:6793), CharLogOut (:7085),
│                                  #   CloseUser (:6834), CreateMob (:2865), GenerateMob (:2995),
│                                  #   SummonGuild (:5683), SummonServer (:8914), DrawConfig (:1117),
│                                  #   RebuildGenerator (:3363), BuildList (:3713, empty),
│                                  #   PutItem (:709), UpdateItem (:7472), Reboot (:7127)
├── GetFunc.cpp                    # SetPKPoint (:2145), GetPKPoint (:2105), SetTotKill (:2087),
│                                  #   GetCreateItem (:1195), GetCreateMob, GetAttribute
├── SendFunc.cpp                   # SendClientMessage (:27), SendNotice (:47), SendNoticeChief (:64),
│                                  #   SendSummonChief (:103), SendEmotion (:825), SendScore, SendEtc,
│                                  #   SendItem, SendEquip, GridMulticast (:843), SendWeather
├── CMob.cpp                       # GetCurrentScore (:598), CheckGetLevel (:1034)
├── CNPCGene.cpp                   # mNPCGen.ReadNPCGenerator (:45)
├── CReadFiles.cpp                 # ReadGuild, WriteGuild, ReadSancRate, ReadQuestsRate, ReadCompRate,
│                                  #   ReadAdmin (:640)
├── CCastleZakum.cpp               # ReadCastleQuest (:396)
├── _MSG_MessageWhisper.cpp        # CALLER (afferent) — /gm dispatch at :787-793
├── ProcessDBMessage.cpp           # CALLER (afferent) — _MSG_MessageDBImple at :123-134
└── CUser.h                        # USER_* state constants (:26-36)
```

Boundary notes:

- The component owns no data. Every persistent structure it mutates is declared elsewhere (`Server.cpp`, `Basedef.cpp`, `CUser.h`, `CItem.h`, `CNPCGene.h`).
- There is no `.h` for `imple.cpp`; its two symbols are declared in the general-purpose `Server.h` alongside 200+ unrelated declarations.
- No class, no namespace, no anonymous linkage: every callee is reachable from every other translation unit, and nothing prevents a second caller from invoking `ProcessImple` with an arbitrary `level`.

---

## 5. Dependency Analysis

### Internal dependencies (efferent, from `imple.cpp`)

```
ProcessImple
├── Parsing / formatting
│   └── sscanf, sprintf, strcmp, strcpy, strcat, memcpy, memset, strncpy (CRT)
├── Persistence / files
│   ├── _open, _read, _write, _close, _S_IREAD|_S_IWRITE  (io.h / fcntl.h)
│   ├── fopen, fscanf, fprintf, fclose
│   ├── CReadFiles::ReadAdmin, ReadGuild, WriteGuild, ReadSancRate, ReadQuestsRate, ReadCompRate
│   └── DrawConfig  -> writes gameconfig.txt and repaints the window
├── Messaging
│   ├── SendClientMessage, SendNotice, SendNoticeChief, SendSummonChief
│   ├── SendScore, SendEtc, SendEquip, SendItem, SendWeather, SendEmotion
│   ├── GridMulticast -> per-viewer filtering
│   └── DBServerSocket.SendOneMessage, BillServerSocket.ConnectBillServer
├── Character / world
│   ├── CMob::GetCurrentScore, CMob::CheckGetLevel
│   ├── PutItem, CreateItem, UpdateItem, GetCreateItem, GetCreateMob, CreateMob, GenerateMob
│   ├── SetPKPoint, SetTotKill, GetAttribute, DoTeleport, MobKilled, SummonGuild, SummonServer
│   ├── CharLogOut, CloseUser, GetUserByName
│   ├── GuildZoneReport, RebuildGenerator
│   └── mNPCGen.ReadNPCGenerator
├── Configuration reload
│   ├── BASE_InitializeBaseDef, BASE_InitializeMessage, BASE_InitializeGuildName
│   └── CCastleZakum::ReadCastleQuest
└── Infrastructure
    ├── Log
    ├── SetTimer(hWndMain, TIMER_MIN, ...)
    └── StartLog, StartChatLog, StartItemLog

SaveAll
├── CharLogOut
└── Log
```

### Afferent dependencies (who calls into the component)

```
Exec_MSG_MessageWhisper   (Code/TMSrv/_MSG_MessageWhisper.cpp:791)  -> ProcessImple
ProcessDBMessage          (Code/TMSrv/ProcessDBMessage.cpp:132)     -> ProcessImple
MainWndProc / IDC_SAVEALL (Code/TMSrv/Server.cpp:4097-4098)         -> SaveAll
ProcessImple              (Code/TMSrv/imple.cpp:583)                -> SaveAll
```

### External dependencies

| Dependency | Type | Purpose | Notes |
|---|---|---|---|
| Win32 API (`SetTimer`, `GetDC`, `TextOutA`, `SetWindowTextA`) | Platform | Heartbeat retiming, config repaint, log window title | `DrawConfig` requires a live window handle and font |
| CRT file API (`_open/_read/_write/_close`, `fopen/fprintf/fscanf`) | Platform | NPC record files, `biserver.txt`, `gameconfig.txt`, `Admin.txt` | No validation of paths or payloads |
| DBSrv socket (`DBServerSocket`) | Internal service | `_MSG_DBNotice`, `_MSG_GuildInfo` | Cross-server broadcast |
| Billing socket (`BillServerSocket`) | External service | `billconnect`, billing mode precondition | Address/port from `biserver.txt` |
| Filesystem layout `../../DBSRV/Run/Admin.txt` | External artefact | DB admin IP allow-list | Hard-coded relative path crossing process boundaries |
| `./npc/` directory | External artefact | `STRUCT_MOB` blobs used by generator, save/read, exp rewrites | Name derived from unvalidated arguments |
| `gameconfig.txt` | External artefact | Economy/event/treasure persistence | Strictly validated on read (`ReadConfig`), rewritten wholesale on change |

---

## 6. Afferent and Efferent Coupling

The component is procedural C++, so the coupling unit is the function/symbol rather than a class.

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Instability (Ce/(Ca+Ce)) | Critical | Notes |
|---|---|---|---|---|---|
| `ProcessImple` | 2 | 51 project functions + 15 CRT/Win32 + 28 globals | 0.96 | Critical | Dispatches 65 commands / 60 `set` keywords; writes 14 global state families |
| `SaveAll` | 2 (ProcessImple, MainWndProc) | 2 (`CharLogOut`, `Log`) | 0.50 | High | Bulk persistence; reachable from GUI without audit |
| `imple.cpp` (file) | 2 | 51 | 0.96 | Critical | No header, no encapsulation, no internal helpers |

Coupling observations:

- **Afferent breadth is tiny; efferent breadth is enormous.** Two callers can trigger mutations across the player table, item table, mob grid, generator table, economy configuration, blocking files, and two sockets. Any change to a callee signature propagates into one file only, but any change in a callee's *semantics* silently changes a privileged surface.
- **Hub-and-spoke with no intermediate layer.** `ProcessImple` calls `Server.cpp`, `SendFunc.cpp`, `GetFunc.cpp`, `CMob.cpp`, `CNPCGene.cpp`, `CReadFiles.cpp` and `CCastleZakum.cpp` directly. There is no command abstraction, no validation layer, and no transaction boundary.
- **Data coupling dominates.** The function depends on ~28 mutable globals whose declarations live in `Server.cpp`/`Basedef.cpp`. Because most are `int` flags (e.g. `DOUBLEMODE`, `DEADPOINT`, `isDropItem`), any typo or out-of-range write is undetectable to the compiler.
- **Stamp coupling through `MSG_*` structures.** `MSG_CreateItem`, `MSG_UpdateItem`, `MSG_CreateMob`, `MSG_GuildInfo`, `MSG_DBNotice` are filled field-by-field in several branches, so a protocol change requires edits in multiple places inside one function.
- **Control coupling through return codes.** `CreateMob` returns `1/0/-1/-2` and `CreateItem` returns an id range; both are interpreted inline with magic numbers.

---

## 7. Endpoints

The component exposes no HTTP/REST/GraphQL/gRPC surface. Its two protocol entry points are TMSrv packet types, both of which are internal server protocols.

| Endpoint | Protocol / Type | Direction | Description | Guard |
|---|---|---|---|---|
| `/gm <body>` whisper (`MobName == "gm"` or `"GM"`) | TMSrv `_MSG_MessageWhisper` (client -> game) | Inbound command | Invokes `ProcessImple(conn, CurrentScore.Level - 1000, String)`; body must start with the discarded prefix byte | `pUser[conn].Mode == USER_PLAY` only |
| `_MSG_MessageDBImple` (`Type = 11 \| FLAG_GAME2DB \| FLAG_DB2GAME`) | TMSrv DB link (DB -> game) | Inbound command | Invokes `ProcessImple(0, m->Level, m->String)`; `ID` forced to 0 by DBSrv, `Level` supplied by the admin client | `FLAG_DB2GAME` bit + `0 <= ID < MAX_USER`; origin gated by DBSrv IP allow-list |
| `gameconfig.txt` (side effect) | Filesystem | Outbound artefact | Serialised economy/event/treasure state after most level <= 3 mutations | None |
| `../../DBSRV/Run/Admin.txt` (side effect) | Filesystem | Outbound artefact | DB admin IP allow-list rewritten by `npko` | Hard-coded account-name check |
| `./npc/<name>` (side effect) | Filesystem | Outbound/inbound artefact | `STRUCT_MOB` blobs written by `save`, `svexp`, `waterexp`, `nigexp`; read by `read` and the generator | Level 8 (`save`/`read`), level 1 (bulk exp) |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| Client whisper channel | Inbound | Command delivery | TMSrv packet (`_MSG_MessageWhisper`) | Fixed struct: `MobName[16]`, `String[100]` | None inside the component; caller checks `USER_PLAY` |
| DBSrv game link | Bidirectional | Command delivery, notice broadcast, guild fame push | TMSrv packet (`_MSG_MessageDBImple`, `_MSG_DBNotice`, `_MSG_GuildInfo`) | Fixed structs with `Level`/`String` | Flag bit check at `ProcessDBMessage`; no per-message authentication |
| Billing server | Outbound | `billconnect`; billing mode precondition | Custom socket (`ConnectBillServer`) | Address/port from `biserver.txt` | Reply strings; `BILLING` still mutated on partial success paths |
| `./npc/` mob files | File system | NPC templates, character snapshots | Raw `_read`/`_write` of `sizeof(STRUCT_MOB)` | Binary `STRUCT_MOB` | `-1` handle check; short reads unhandled |
| `gameconfig.txt` | File system | Economy/event/treasure persistence | Text, fixed header/column layout | Positional integers, validated on read | Write failures ignored; no atomic replace |
| `../../DBSRV/Run/Admin.txt` | File system (cross-process) | DB admin IP allow-list | Text `idx a.b.c.d` | 10 entries max | `fp == NULL` skips write but still reports success |
| `biserver.txt` | File system | Billing endpoint discovery | Text `address port` | `%s %d` into fixed buffers | Missing file -> message; unvalidated content |
| Player/mob state (`pMob`, `pUser`) | In-process | All character mutations | Direct memory | `CMob` / `CUser` | No validation layer; guards are tautologies |
| World item state (`pItem`) | In-process | Gate and object manipulation | Direct memory | `CItem` (5000 entries) | Indices unvalidated in most branches |
| Mob grid (`pMobGrid`) | In-process | Spawn visibility, occupancy | Direct memory write | `unsigned short[4096][4096]` | `testegrid` has none |
| NPC generator (`mNPCGen`) | In-process + file | Spawn table and population accounting | Direct memory + file rewrite | `CNPCGene` list (8192) | Bounds checked in `generate`, not in `createkefra` |
| Log subsystem | File system + window | Audit and operations log | `fprintf` + `SetWindowTextA` | Text lines | No error handling; `fLogFile` may be NULL |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Command dispatcher (chain of `if/else if` on strings) | `cmd` / `sval1` compared with `strcmp` | `imple.cpp:94-1845` | Map text to a handler; no table, no registration |
| Level-based access tiers | Four independent `if (level >= N)` blocks | `imple.cpp:89, 569, 647, 1119, 1164` | Coarse privilege grading by character level |
| Privileged-command ladder with implicit numeric sub-dispatch | Numeric parse of `sval1` drives the equipment form | `imple.cpp:479-506` | Overload `set` with a positional syntax |
| Façade over world state | One function fronts 51 callees | `imple.cpp:46` | Give operators one entry point |
| Transaction script (no rollback) | Mutate, then resynchronise | `imple.cpp:500-506` | Make changes visible to the client |
| Configuration snapshot / replay | `DrawConfig(TRUE)` serialises all tuning state; `ReadConfig` replays it | `Code/TMSrv/Server.cpp:1117-1332, 761-1160` | Survive restart without a database |
| Two-channel privileged transport | Whisper path and DB path converge on one function | `_MSG_MessageWhisper.cpp:791`, `ProcessDBMessage.cpp:132` | In-game admin and remote admin tooling |
| Staged shutdown state machine | `ServerDown` sentinel (-1000 idle, 1..119 countdown, 120 closing) | `ProcessSecMinTimer.cpp:42-119`, `Server.cpp:4032` | Graceful reboot with connection blocking |
| Sentinel-index pseudo-items | `Carry[63]` = kill mark; `pItem[45]` = mgate target; `sIndex 4143` = armia gate | `imple.cpp:308-428`, `GetFunc.cpp:2105-2158` | Store non-inventory data in fixed slots |
| Magic-range item templates | ids 2330..2389 get a fixed effect line | `imple.cpp:448-453, 483-488, 713-718` | Pre-blessed admin item family |
| Hard-coded identity gate | Three literal account names | `imple.cpp:1674-1681` | Restrict the allow-list write |

Architectural decisions observed:

- **Authority is data, not policy.** The level thresholds are compiled constants; the level value is a persisted character statistic. There is no policy object, configuration entry, ACL, or capability list.
- **One function, one failure domain.** All 125 command tokens share a single stack frame with 9 x 128-byte buffers and a single exit; a fault in any branch terminates the server thread that is processing the message.
- **Configuration lives in files, not in a database.** `gameconfig.txt` is the source of truth for economy/event/treasure tuning and is written by the commands themselves.
- **Cross-process control via files.** `npko` writes into the DB server's directory; the DB server reads it at startup or on demand. This is the only inter-process control channel besides sockets.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|---|---|---|---|
| Critical | Authorization (`imple.cpp:89-91`) | No identity check; authority is `CurrentScore.Level - 1000`, a persisted character statistic. `pUser[].Admin` is set at login (`ProcessDBMessage.cpp:1004`) and never read | Anyone who obtains a character with level >= 1001 (via DB manipulation, a character file, or `set level` at tier 8) gets the entire command surface |
| Critical | Authorization (`ProcessDBMessage.cpp:132`) | `m->Level` is taken verbatim from the DB message; no signature, nonce, or re-validation in TMSrv | Trust boundary is the DB link and the Admin.txt IP list; a compromised or spoofing DB peer executes arbitrary commands at arbitrary level |
| Critical | Guard (`imple.cpp:91, 1051, 1067, 1566, 1832`) | `if (conn <= 0 && conn >= MAX_USER)` is unsatisfiable; five copies | Dead defense-in-depth; DB path (`conn == 0`) writes through slot 0 with no protection |
| Critical | `set destroygate` (`imple.cpp:299-310`) | `memset(&pItem[ival2], 0, sizeof(CItem))` with `ival2` unvalidated against `MAX_ITEM` | Out-of-bounds zero-fill of up to 4 GB displacement from a 5000-entry global |
| Critical | `set cgate/ogate/ngate` (`imple.cpp:312-410`) | `pItem[ival2]` and `UpdateItem(ival2, ...)` index unchecked | Arbitrary read/write through `pItem` and `pMobGrid`/`pHeightGrid` writes inside `UpdateItem` |
| Critical | `read` (`imple.cpp:527-549`) | `_read` of `sizeof(STRUCT_MOB)` over the live record with no validation, then `strcpy(MobName, sval1)` into a 16-byte field | Arbitrary file content becomes character state; unbounded copy corrupts adjacent fields |
| High | `save` (`imple.cpp:508-526`) | Unvalidated path concatenation `"./npc/" + sval1` | Arbitrary file creation/overwrite on the server host with character-record content |
| High | `svexp` (`imple.cpp:1352, 1376`) | Division by the caller-supplied level with no zero check | `svexp 0 <xp>` raises an integer divide-by-zero and terminates the process |
| High | `waterexp` / `nigexp` (`imple.cpp:1234, 1267, 1300, 1418, 1451, 1484`) | Division by `FREEEXP`, which is writable via `set billfree 0` at tier 3 | Two-command path from tier 1 to a divide-by-zero fault |
| High | `waterexp` (`imple.cpp:1243`) | Room 1 branch iterates `WATER_M_INITIAL .. WATER_N_INITIAL + 8` (10..179) instead of the M range | 170 NPC files rewritten with the wrong experience formula; silent mass data corruption |
| High | `testegrid` (`imple.cpp:1311-1321`) | Fills ~3.98 million `pMobGrid` cells with `1`; no reply, no undo | Persistent world-grid corruption reachable at the lowest admin tier |
| High | `npko` (`imple.cpp:1674-1687`) | Three hard-coded account names; writes the DB server's admin IP allow-list across a relative path | Privilege granting depends on literals in the binary; no audit of the previous value; path traversal from the TMSrv working directory |
| High | Audit (`imple.cpp:80-87`) | Log written before authorization; content is `"adm " + raw string` only | Unauthenticated log flooding; no record of level, outcome, or target |
| Medium | `gift` (`imple.cpp:731-752`) | Slot scan bounded by `MAX_SHOPLIST` (27), then tested against `MAX_CARRY` (64) | When all 27 scanned slots are full the item overwrites `Carry[27]`; the "no space" branch is unreachable for slots 27..63 |
| Medium | `muteall` / `desmuteall` (`imple.cpp:1536-1555`) | `return` inside the loop on the first slot below `USER_SELCHAR` | Bulk moderation silently affects only the leading run of occupied slots |
| Medium | Range validation (`imple.cpp:920, 931, 802, 810, 818, 826, 1082`) | `x < 0` checks on `unsigned int` values; only upper bounds are effective | Advertised ranges (e.g. `0-64` for `statsapphire`) are not enforced; negative input wraps to large positives |
| Medium | `set gate` (`imple.cpp:284`) | `sm_cig.ItemID = ival2 + 10000` uses the item definition id, while `GetCreateItem` uses the `pItem` slot index | Client object identity is inconsistent with every other item producer |
| Medium | `set item` (`imple.cpp:462`) | `PutItem` result ignored; success string sent unconditionally | Inventory-full failures are reported as success |
| Medium | `set buildhtml` / `decay` (`imple.cpp:108-112, 586-590`) | `BuildList()` is empty; `decay` assignment is commented out; both reply success | Operators cannot distinguish success from no-op |
| Medium | `set level` (`imple.cpp:123-130`) | Upper clamp 2010 (above `MAX_LEVEL` 399), no lower clamp | Self-demotion possible; levels >= 2000 skip the resist/equipment sanitisation branch in `GetCurrentScore` |
| Medium | `set billmode` (`imple.cpp:593-606`) | Any integer accepted; only 2/3 require a billing socket | `BILLING` drives login gating (`_MSG_CharacterLogin.cpp:31, 69`); out-of-range values change login behaviour |
| Medium | `delayreboot` (`imple.cpp:653-659`) | `-1 * ServerIndex * (n * 2)` with `ServerIndex` defaulting to -1 | Effective delay depends on runtime topology; `ServerIndex == 0` yields `ServerDown == 0` and a different countdown phase |
| Medium | `notice` (`imple.cpp:1187`) | `memcpy(temp, str + 7, 96)` from a 100-byte field | Three-byte overread of the whisper string into the packet buffer |
| Medium | `fame` (`imple.cpp:1649`) | `GuildInfo[guild].Fame += ival2` with unsigned add into `int` | Overflow to negative fame; read-modify-write clobbering across servers |
| Low | Buffers (`imple.cpp:48-56, 68`) | 9 x `char[128]` with unbounded `%s`; uninitialised when tokens are missing | Stale stack data used as arguments; bounded today by the 100-byte source field |
| Low | `m->Level` path divergence | DB path cannot use `save`/`read` (conn != 0) but can execute every other level-8 command against slot 0 | Confusing and inconsistent capability matrix between the two entry points |
| Low | Duplicate notice implementations | `notice`, `/not`, `chiefnotice` all broadcast with different filters | Three maintenance points for one behaviour; `chiefnotice` also double-logs |
| Low | Global `temp[4096]` (`Server.h:382`) | Shared across all branches and mixed with local `MSG_*` structures | Fragile to any future callee that also uses `temp` |
| Low | Localisation inconsistency | `"Atributo do mapa é %d"` mixed with hard-coded English strings | Operator-facing messages are not localisable as a set |
| Low | Dead symbols | `IsImple()` returns 0 and is never called (`Server.cpp:6829`); `pUser[].Admin` written but never read | Misleading to maintainers; suggests an abandoned capability model |
| Low | Initialisation order | `set reloadfile` can run before `ReadConfig`; `DrawConfig` returns early when `ConfigReady == 0` | Early mutations are applied in memory but never persisted, with a success reply |

---

## 11. Test Coverage Analysis

**Result: no automated tests exist for this component, and no automated test infrastructure exists in the repository.**

Evidence:

| Evidence | Finding |
|---|---|
| `W2PP Code Project.sln` | Contains exactly two projects: `Code\DBSrv\DBSrv.vcxproj` and `Code\TMSrv\TMSrv.vcxproj`. No test project, no test target, no solution folder for tests. |
| Repository-wide file search for `*test*`, `*Test*`, `*spec*` (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`) | No source, project, or fixture files matched. The only matches were the `openspec` directory (excluded) and the solution file itself. |
| `Code/TMSrv/TMSrv.vcxproj` and related project files | No references to GoogleTest, Catch2, MSTest, CppUnit, or any test runner framework. |
| Repository-wide searches for `ASSERT`, `TEST(`, `TEST_F`, `EXPECT_`, `CATCH_` in `Code/` | No matches. |
| Codegraph blast-radius output for `ProcessImple`, `SaveAll`, `DrawConfig`, `CloseUser`, `CharLogOut` | "no tests found within 3 caller hops" for every symbol. |
| `Code/TMSrv/Debug/`, `Code/TMSrv/Release/`, `Code/TMSrv/TMSrv/` | Build output directories only; no test artefacts. |

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `ProcessImple` — parsing (BR-01..BR-04) | 0 | 0 | 0% | None. Framing rule (discarded first byte), missing-token behaviour and unsigned wrap are untested and undocumented. |
| `ProcessImple` — authorization (BR-05..BR-08) | 0 | 0 | 0% | None. No test asserts that level 0 executes nothing, that level 1 cannot reach tier-8 commands, or that the `conn` guard is reachable. |
| `ProcessImple` — level 8 `set` family (BR-11..BR-32) | 0 | 0 | 0% | None. Field truncation, the 2010 level clamp, the 2330..2389 bonus range and the implicit positional form are unverified. |
| `ProcessImple` — level 3 economy (BR-33..BR-38) | 0 | 0 | 0% | None. The billing-socket precondition and the `FREEEXP` divisor coupling are untested. |
| `ProcessImple` — level 2 lifecycle/events (BR-39..BR-58) | 0 | 0 | 0% | None. The `ServerDown` arithmetic, drop-position mapping and treasure re-parse are untested. |
| `ProcessImple` — level 1 world/moderation (BR-59..BR-90) | 0 | 0 | 0% | None. `testegrid`, `svexp 0`, `muteall`, `kick` level ordering and `npko` identity literals are untested. |
| `SaveAll` | 0 | 0 | 0% | None. Non-`USER_PLAY` slots and partial-failure behaviour are unverified. |
| `DrawConfig` / `ReadConfig` round trip (used by ~25 commands) | 0 | 0 | 0% | None. The write-then-reject asymmetry on a malformed file is untested. |
| `g_pTreasure` / `g_pDropBonus` / `g_pGuildZone` mutators | 0 | 0 | 0% | None. Boundary indices (8, 16, 64, 5) are unverified. |

Coverage assessment:

- **No safety net for a privileged surface.** The component is exercised only by manual in-game commands on a running server. There is no fixture that constructs a `CMob`, no injectable clock for the `ServerDown` countdown, and no seam for the DB or billing socket.
- **Testability is structurally blocked.** `ProcessImple` takes a raw `char*`, reads `pMob`/`pUser`/`pItem` globals directly, calls `DrawConfig` (which requires an `HWND` and a font), and writes to relative filesystem paths. Any test would need the full Win32 application context.
- **Highest-risk paths are the least observable.** The out-of-bounds writes (`pItem[ival2]`, `testegrid`, `strcpy` into `MobName`) and the divide-by-zero paths (`svexp 0`, `FREEEXP == 0`) would require fault-injection or a memory checker; neither is configured anywhere in the repository.
- **Regression risk is concentrated in one file.** Because 85 behavioral rules live in a single 1,846-line function with shared buffers and a shared global `temp`, a change to any branch can affect any other branch; without tests, the only verification is manual replay.

---

## 12. Configuration & Environment Handling

| Configuration | Source | Read by | Written by |
|---|---|---|---|
| `gameconfig.txt` (event, double/deadpoint/dungeonevent, statsapphire, battleroyal, billing, 64 drop bonuses, 8 treasure rows, partydif/kefrastatus/GTorreHour/RVRHour/DropItem/BRHour/maxNightmare/PotionDelay) | File, own working directory | `ReadConfig` (`Server.cpp:761-1160`), strictly validated header by header | `DrawConfig(TRUE)` (`Server.cpp:1145-1331`) after ~25 commands |
| `biserver.txt` | File | `billconnect` (`imple.cpp:1141-1148`) | Never |
| `../../DBSRV/Run/Admin.txt` | File, relative to TMSrv working directory | `CReadFiles::ReadAdmin` (`CReadFiles.cpp:644-673`) and DBSrv's own copy | `npko` (`imple.cpp:1670-1699`) |
| `./npc/*` | Directory of binary `STRUCT_MOB` files | `read`, `mNPCGen.ReadNPCGenerator`, `CreateMob` via `ReadMob` | `save`, `svexp`, `waterexp`, `nigexp` |
| `Language.txt` | File | `set reloadfile` -> `BASE_InitializeMessage` | Never |
| Sanctification / quest / composition rate tables | Files read by `CReadFiles::ReadSancRate` / `ReadQuestsRate` / `ReadCompRate` | `set reloadfile` | Never |
| Guild data | File via `CReadFiles::ReadGuild` / `WriteGuild` | `reloadguild`, `set champ`, `set chall` | `set champ`, `set chall` |
| Castle quest data | File via `CCastleZakum::ReadCastleQuest` | `set reloadfile` | Never |
| Log streams | Files opened by `StartLog` / `StartChatLog` / `StartItemLog` | `log` | Runtime |

Environment-dependent behaviour:

- `SimpleDraw`/`DrawConfig` returns immediately when `ConfigReady == 0` (`Server.cpp:1119-1120`), so commands issued before `ReadConfig` completes mutate memory but persist nothing while still reporting success.
- `DrawConfig` also requires a valid `hWndMain`, `hDC` and `hFont`; in a non-interactive or service context the repaint is skipped before the `fopen`, so configuration writes are coupled to GUI availability.
- `billconnect` and `set billmode 2|3` behave differently depending on whether `biserver.txt` exists and whether the socket is already connected.
- `delayreboot` depends on `ServerIndex`, which is -1 until the DB assigns it (`ProcessDBMessage.cpp:69-81`).
- `npko` depends on a directory layout in which the TMSrv working directory is two levels below a sibling `DBSRV/Run` directory — a deployment assumption not expressed anywhere in configuration.
- `set reloadfile`, `waterexp`, `svexp`, `nigexp` and `reloadguild` fail at different points depending on which files are present, and each failure path has its own message or silence.

---

## 13. Security Assessment (privileged-surface audit)

This section consolidates the authorization findings required by the component's architectural role.

| # | Finding | Evidence | Severity |
|---|---|---|---|
| S1 | No authentication of the command origin on the whisper path; only `USER_PLAY` | `_MSG_MessageWhisper.cpp:29, 787-792` | Critical |
| S2 | No authorization by identity, account, IP or role; only a derived character level | `imple.cpp:89, 569, 647, 1119, 1164` | Critical |
| S3 | Intended admin flag `pUser[].Admin` is written but never read | `ProcessDBMessage.cpp:1004-1005, 391`; `Server.cpp:6843`; no read site | High |
| S4 | DB path trusts the packet's `Level` field verbatim | `ProcessDBMessage.cpp:123-134` | Critical |
| S5 | Dead connection guard (5 copies) provides no bounds protection | `imple.cpp:91, 1051, 1067, 1566, 1832` | High |
| S6 | Pre-authorization audit allows unauthenticated log flooding | `imple.cpp:80-87` | Medium |
| S7 | Hard-coded account-name gate for the allow-list write | `imple.cpp:1674-1681` | High |
| S8 | Privilege state can be written by the privileged surface itself (`set level`, `set classmaster`, `learn`) | `imple.cpp:123-130, 244-248, 1590-1595` | Medium |
| S9 | Out-of-bounds array access from unvalidated indices (`pItem`, `pMobGrid`) | `imple.cpp:299-410, 1311-1321` | Critical |
| S10 | Unbounded `strcpy` into `MobName[16]` | `imple.cpp:549, 558` | High |
| S11 | Arbitrary path file write/read under the process account | `imple.cpp:508-555` | High |
| S12 | Divide-by-zero reachable through command arguments and writable configuration | `imple.cpp:1352, 1376`; `set billfree` + `waterexp`/`nigexp` | High |
| S13 | Lowest tier (level 1) includes cluster-wide broadcast, mass teleport, mass kill and grid corruption | `imple.cpp:1185-1201, 1311-1321, 1657-1664, 1837-1844` | High |
| S14 | No rate limiting, quota, or lockout on any command | `imple.cpp` (no counter state) | Medium |

Authorization chain, end to end:

```
Player client
  -> /gm whisper (pUser.Mode == USER_PLAY)
  -> level = pMob[conn].MOB.CurrentScore.Level - 1000      [persisted character stat]
  -> ProcessImple(conn, level, String)
       -> tier gates 8 / 3 / 2 / 1
       -> no identity, no capability, no target-ownership check

Admin tool
  -> TCP to DBSrv, source IP must be in Admin.txt -> pAdminIP (DBSrv/Server.cpp:985-1000)
  -> _MSG_MessageDBImple { Level = <client supplied>, String = "+cmd args" }
  -> DBSrv rewrites ID = 0 and broadcasts to all game servers (CFileDB.cpp:592-609)
  -> TMSrv ProcessDBMessage (FLAG_DB2GAME, 0 <= ID < MAX_USER)
  -> ProcessImple(0, m->Level, m->String)                   [no re-validation of Level]
```

Note on slot 0: `GetUserByName` iterates from 1 (`Server.cpp:6801, 6815`), `SendClientMessage` rejects `conn <= 0` (`SendFunc.cpp:29`), and `PutItem` rejects `conn <= 0` (`Server.cpp:711`). The DB path's `conn == 0` therefore operates on a slot that client-facing code treats as invalid, which means DB-issued `set item`, `set hp` and `learn` mutate state that no later read path exposes — but `set gate`, `testegrid`, `svexp`, `notice` and the file writers remain fully effective from that path.

---

## 14. Performance Notes

| Area | Observation | Location |
|---|---|---|
| Dispatch cost | Linear `strcmp` chain of up to 39 comparisons for `set`, evaluated twice more for tiers 3 and 2 — at most ~60 comparisons per invocation | `imple.cpp:94-1116` |
| Config persistence | `DrawConfig(TRUE)` rewrites the entire `gameconfig.txt` and repaints the whole server window on ~25 commands, including a full GDI redraw and one `TextOutA` per data line (~30 lines plus 4 x 16 drop cells) | `Server.cpp:1117-1331` |
| Bulk client I/O | Every `set` sends 16 equipment items plus score, equip and ETC; `set clearcarry` sends 60 items; `SaveAll` sends one logout signal per playing slot | `imple.cpp:432-436, 500-506` |
| Bulk filesystem I/O | `svexp` opens, writes and closes two files per generator (up to 2 x 8,192), then reloads the whole generator table; `waterexp` room 1 opens up to 170 files because of the bound bug | `imple.cpp:1323-1385, 1241-1272` |
| Grid flooding | `testegrid` writes ~3.98 million `unsigned short` cells (about 8 MB of stores) in one call | `imple.cpp:1313-1320` |
| Broadcast fan-out | `muteall`, `desmuteall` and `SaveAll` iterate the full 1000-slot user table; `SendNotice`/`SendNoticeChief` iterate 1000 slots per delivery | `imple.cpp:1534-1562`, `SendFunc.cpp:47-101` |
| Generator reload | `reloadnpc`, `rebuild` and all three exp commands re-read or recount the 8,192-entry generator table | `imple.cpp:841, 1307, 1383, 1491` |
| String formatting | Eight duplicated `sprintf` summary blocks for the event engine, each formatting 9 integers into the shared global buffer | `imple.cpp:946-1019` |

---

## 15. Ambiguities and Confidence Levels

| Item | Statement | Confidence |
|---|---|---|
| Wire prefix | The first byte of the body is discarded, so the intended syntax is `+<command> <args>`; the actual client prefix byte is not visible in server code | High (code); Medium (exact character) |
| `set noatum` | The command name suggests "no atum"; its effect is to force `CastleServer = 1` and `CastleState = 1`, and no code path restores a previous state | High for the effect; Medium for intent |
| `set buildhtml` | `BuildList()` is empty, so the feature is either stubbed or lost in decompilation | High (behaviour); Medium (cause) |
| `trtarget` syntax | Recovered from the second `sscanf` and the error string `"+treasure (1-8) (1-5) (item)"`; the acknowledgement uses `+troktarget` | High for parsing; Medium for intended operator syntax |
| `mgate` slot 45 | The hard-coded index 45 is an observed constant; which gate it represents is not defined in code | High (constant); Low (semantics) |
| `sIndex 4143` (armia gate) | Identified only by the literal in `closearmia`/`openarmia`; no symbolic constant exists | Medium |
| `waterexp` room 1 bound | The loop uses `WATER_N_INITIAL + 8` in the M branch; whether this is a typo or an intentional range is unknowable from code | Medium (deliberate defect) |
| `gift` slot scan | The `MAX_SHOPLIST` bound plus the `MAX_CARRY` test implies the "no space" branch is unreachable for slots 27..63; no comment explains the intent | Medium |
| `delayreboot` intent | The arithmetic and clamp produce different behaviour per `ServerIndex`; the intended delay formula cannot be confirmed | Medium |
| DB `Level` provenance | The admin client that originates `_MSG_MessageDBImple` is not part of this repository; only the DBSrv forwarding and the TMSrv consumption are visible | High (consumption); Unknown (origin) |
| `Admin.txt` refresh | Whether DBSrv re-reads the file after `npko` writes it depends on DBSrv's own read triggers, which are outside this component | Medium |
| `set exp` level-up count | Only one `CheckGetLevel()` pass runs, so a large injection advances one level immediately; subsequent natural gains may continue the progression | High |

---

## 16. Component Summary Table

| Attribute | Value |
|---|---|
| Symbols owned | `ProcessImple`, `SaveAll` |
| Lines of code | 1,846 (single translation unit) |
| Top-level commands | 65 |
| `set` sub-commands | 60 (39 tier 8, 7 tier 3, 14 tier 2) |
| Authorization tiers | 4 (levels 8, 3, 2, 1 -> character levels 1008, 1003, 1002, 1001) |
| Inbound protocols | 2 (client whisper, DB message) |
| Outbound protocols | 2 (DB socket, billing socket) |
| File artefacts written | 5 categories (`gameconfig.txt`, `./npc/*`, `Admin.txt`, `biserver.txt` read-only, log files) |
| Direct callees | 51 project functions + 15 CRT/Win32 entry points |
| Mutable globals touched | ~28 |
| Afferent / efferent coupling | 2 / 51 |
| Instability index | 0.96 |
| Automated tests | 0 |
| Highest-severity defect | Unauthored authorization boundary combined with unvalidated `pItem` indexing |
