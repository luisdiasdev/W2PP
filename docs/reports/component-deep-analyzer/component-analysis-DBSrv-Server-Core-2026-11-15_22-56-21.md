# Component Deep Analysis Report - DBSrv Server Core

- **Component name:** DBSrv Server Core
- **Primary location:** `Code/DBSrv/Server.cpp` (2,342 lines, 50,485 bytes) and `Code/DBSrv/Server.h` (124 lines, 3,959 bytes)
- **Project scope analysed:** repository root `E:\wyd\test-setup\w2pp` (W2PP - decompiled C/C++ Win32 MMORPG game-server emulator for W2)
- **Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`
- **Analysis date:** 2026-11-15
- **Analysis type:** read-only static analysis of source, project files, configuration contracts and call sites. No file was modified, no code was executed, no test was run.
- **Cross-referenced context:** `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` (component row 41, "DBSrv Server Core", afferent ~6 / efferent 8)

---

## 1. Executive Summary

### 1.1 Component purpose

`DBSrv Server Core` is the process-level spine of the DBSrv account/character database server. It is the only translation unit in DBSrv that owns the Win32 process entry point (`WinMain`, `Code/DBSrv/Server.cpp:427`), the window class and window procedure (`InitApplication` `:314`, `InitInstance` `:335`, `MainWndProc` `:743`), the three socket endpoints (game-server listener, NP-admin listener, outbound redirect client), the periodic maintenance scheduler (`ProcessSecTimer` `:2013`), the logging/day-log subsystem (`StartLog` `:2091`, `Log` `:2115`, `DayLog_ExpLog` `:2141`, `DayLog_ItemLog` `:2284`) and the complete NP administrative command interpreter (`ProcessAdminMessage` `:1463`-`:2011`, 549 lines, 8 message branches).

The component has four distinct responsibilities that are physically interleaved in one 2.3 kLOC file:

1. **Process host and reactive event loop.** A single-threaded Win32 application: one window ("DB Server", 800x600 - `:339`), one `SetTimer` clock (`TIMER_SEC`, 1000 ms - `:613`), and WSAAsyncSelect-driven socket notifications delivered as window messages. There is no thread pool, no overlapped I/O and no worker thread; every packet, every timer tick and every repaint executes on the UI thread.
2. **Connection admission control (the security perimeter).** Two listening sockets: `DB_PORT` 7514 for game servers (`:586`) and `ADMIN_PORT` 8895 for the NPTool administrative client (`:587`), plus one outbound socket to the optional redirect/transfer server (`:599`). Admission is enforced by IP allow-lists (`serverlist.txt` for game servers, `Admin.txt` for NP) plus a `/24` subnet restriction for game servers (`:1150`) plus a challenge/response for NP (`Encode1`/`Encode2`, `:1052`-`:1065`).
3. **NP administrative command interpreter.** Account lookup, account save, account disable/enable, notice broadcasting, character creation, and donation intake, with a privilege model derived from the account's highest character level (`:1713`-`:1726`).
4. **Operations plumbing.** Day-rotated text logging, EXP/item day-log generation by scanning the account directory, Sunday-midnight guild fame reset, base-mob template loading and hot reload, `Config.txt` persistence, and a confirmation-dialog shutdown path (`:1305`).

### 1.2 Architectural role in the system

Within the architecture report's layering, DBSrv is the persistence tier between TMSrv (game servers) and the raw binary account files. This component is the **composition root and the only trust boundary** of that tier: `CFileDB` (`Code/DBSrv/CFileDB.cpp`) does the actual record I/O but never sees a socket; every byte from the network reaches `CFileDB` only after this component has (a) completed the `INITCODE` handshake, (b) validated the framing, (c) validated the `FLAG_GAME2DB` / `FLAG_NP2DB` direction flag and ID range, (d) mapped the socket to a slot in `pUser[]` / `pAdmin[]`, and (e) for NP, authenticated the operator and resolved a privilege level.

Coupling direction is asymmetric and inverted relative to CFileDB: this component owns the global state that every other DBSrv translation unit reads through `Server.h` (`pUser`, `pAdmin`, `cFileDB`, `ServerIndex`, `Sapphire`, `LocalIP`, `ItemDayLog`, `hWndMain`, `CurrentTime`, `GuildInfo`), and it calls into `CFileDB`, `CReadFiles`, `CRanking`, `CPSock`, `CUser` and `Basedef`. Empirically, `Log()` alone has 120 call sites in `Code/DBSrv/CFileDB.cpp`, `Code/DBSrv/CRanking.cpp` and `Code/DBSrv/CReadFiles.cpp` plus call sites in the shared `Code/CPSock.cpp` and `Code/Basedef.cpp`; `pUser[]` is dereferenced 66 times from 3 DBSrv translation units.

### 1.3 Key findings

- **No automated tests exist anywhere in the repository.** Verified by exhaustive file-name, macro and project-file search (evidence in Section 11). The NP administrative interpreter - the component with the highest privilege in the whole system - is executed only by human operators against production files.
- **The privilege model is game-state-derived.** An NP account becomes an operator when any of its four characters has `BaseScore.Level >= 1000` (`:1721`-`:1726`); the privilege level is `maxLevel - 1000` verbatim (`:1724`). There is no separate role store, no audit of failed logins, and a character-level inflation defect converts directly into remote administrative access.
- **Administrative mutation requires level > 2** (`:1852`, `:1929`, `:1959`), i.e. a character level >= 1003, while broadcast notice requires level >= 2 (`:1611`) and read-only account lookup requires level > 0 (`:1742`). These three thresholds are hardcoded in three different branches.
- **The shutdown path does not save anyone.** `WM_CLOSE` (`:1305`) shows a Portuguese warning telling the operator "don't forget to save the accounts", then writes `Config.txt`, closes the log, emits both day logs, and destroys the window. Active accounts in `cFileDB.pAccountList` are neither flushed nor signalled; persistence is the game servers' responsibility through the normal save/quit protocol.
- **Verified defects, not stylistic opinions:** an out-of-bounds read of `pAdmin[10]` on every admin connection (`:968`, `pAdmin` has exactly `MAX_ADMIN == 10` elements); an off-by-one ID bound in `ProcessClientMessage` (`:1343` uses `> MAX_USER` where `>= MAX_USER` is required, allowing `GetIndex(conn, 1000)` and, for the last server slot, index `10000` on a `pAccountList[10000]` array); a NULL-dereference on `fopen` failure in `WriteTransGuildInfo` (`:209`-`:211`); an uninitialised `Parm` field in `SendAdminState` (`:299`-`:311`); and the 549-line admin interpreter's `_MSG_NPDonate` branch (`:1982`) with **no privilege check at all**.
- **The periodic scheduler runs file-system scans on the UI thread every second** (`CReadFiles::ImportUser`, `CReadFiles::UpdateUser` - `:2015`-`:2016`), plus two full-directory scans (`ImportItem`, `ImportDonate`) every 30 ticks (`:2020`-`:2024`), a status-HTML write every 30 ticks, a CSV append every 900 ticks, and a ranking rewrite every 600 ticks (`:2041`). While a scan runs, no socket notification is dispatched: the accept backlog and all inbound packets wait.
- **The 1-second timer is the clock of record for day boundaries.** Log rotation and day-log emission compare `struct tm.tm_mday` against cached integers (`:2052`, `:2055`), so a tick that is missed or delayed (or a host clock jump) crosses the boundary silently; the weekly guild-fame reset requires an *exact* `hh:mm:ss == 00:00:00` and `tm_wday == 0` match (`:2061`), so it is missed whenever the tick lands one millisecond past midnight.
- **The architecture report's "graceful/`ServerDown` control" attribute for this component is not supported by the code.** `ServerDown` exists only in TMSrv (`Code/TMSrv/Server.cpp:46`, `Code/TMSrv/ProcessSecMinTimer.cpp:96`-`:113`); DBSrv has no countdown, no drain, no "broadcast shutdown to game servers" message, and no `_MSG_ServerDown` constant. DBSrv's control surface is: the `WM_CLOSE` confirmation dialog, the `IDC_EXIT` menu item, and the `IDC_SHUTDOWNNP` menu item which is labelled "Reload MobBase" and reloads mob templates rather than shutting anything down.

---

## 2. Data Flow Analysis

### 2.1 Boot / composition flow

```
1.  OS loader -> WinMain (Server.cpp:427)
2.  InitApplication (:314) registers WNDCLASS "MainClass" -> MainWndProc
3.  InitInstance (:335) creates the 800x600 "DB Server" window
4.  BASE_InitModuleDir() + BASE_InitializeBaseDef()  [shared Code/Basedef.cpp]
5.  Read ./BaseMob/{TK,FM,BM,HT} into g_pBaseSet[0..3] (:448-:503)
      -> any missing file: MessageBox "no <class> file" + return FALSE (process aborts)
6.  g_pBaseSet[n].BaseScore = g_pBaseSet[n].CurrentScore (:505-:508)
7.  StartLog("A") (:512) -> .\Log\DB_<dd>_<mm>_<yyyy>_<hh>_<mm>_<ss>_A.txt, LastLogDay = today
8.  Log("start log","-system",0) (:513)
9.  memset(g_pGuildWar), memset(g_pGuildAlly) (:515-:516)
10. ReadConfig() (:518)  <- Config.txt {Sapphire, LastCapsule}; return value IGNORED
11. ListenSocket.WSAInitialize() (:520)  -> WSAStartup(1,1); failure: log + return FALSE
12. BASE_InitializeServerList() (:529)  <- ./serverlist.txt or ../../Common/serverlist.txt
      also writes ./serverlist.bin (obfuscated copy with a 64-byte additive key)
13. gethostname + getaddrinfo (:542-:544); for each resolved IPv4 text:
      match against g_pServerList[group][0]; first match sets ServerIndex and LocalIP[4]
14. ServerIndex == -1 -> two MessageBoxes + return TRUE (process ends, no WSACleanup)
15. CReadFiles::ImportItem() (:583)   <- ../../Common/ImportItem/*.*
16. ReadTrandGuildInfo() (:584)       <- Trans.txt (guild-number translation table)
17. ListenSocket.StartListen(hWnd, 0, DB_PORT=7514, WSA_ACCEPT) (:586)
18. AdminSocket.StartListen(hWnd, 0, ADMIN_PORT=8895, WSA_ACCEPTADMIN) (:587)
19. If redirect.txt exists: fscanf("%s %d %s %s") -> sip, port, adminclientid, adminclientpass
      AdminClient.ConnectServer(sip, port, INADDR_ANY, WSA_READADMINCLIENT) (:599)
      failure: MessageBox + return FALSE      success: TransperCharacter = 1 (:609)
20. SetTimer(hWnd, TIMER_SEC, 1000, NULL) (:613)
21. CReadFiles::ReadGuildInfo() (:615)  <- ../../Common/GuildInfo
22. GetMessage loop (:617-:621) -> DispatchMessage -> MainWndProc
```

### 2.2 Game-server connection flow (port 7514)

```
1. TMSrv calls CPSock::ConnectServer(DBServerAddress, 7514, LocalIP, WSA_READDB)
2. Kernel completes TCP handshake; WSAAsyncSelect posts WSA_ACCEPT = WM_USER+3
3. MainWndProc case WSA_ACCEPT (:1068)
4. WSAGETSELECTERROR(lParam) -> log "err WSAGETSELECTERROR - accepting new client." + drop
5. CUser::AcceptUser (:1076) -> accept(), WSAAsyncSelect(FD_READ|FD_CLOSE on this socket),
   TempUser.IP = sin_addr.S_un.S_addr, TempUser.Mode = USER_ACCEPT
6. Slot resolution:
   a. existing pUser[i].IP == TempUser.IP  -> User = i                (:1086-:1093)
   b. else scan g_pServerList[ServerIndex][1..MAX_SERVERNUMBER-1] for the client IP text
      -> User = i - 1                                                 (:1099-:1103)
   c. no match -> close socket, log "err wsa_accept request from <ip>", break
7. If pUser[User].Mode != USER_EMPTY -> close incoming AND existing socket, reset slot, break
8. Subnet gate: cIP[0..2] must equal LocalIP[0..2]  (:1150)
      mismatch -> close socket, log "err,wsa_accept outer ethernet IP: a.b.c.d", break
9. Slot bind: pUser[User].{IP,Mode,cSock.Sock,nRecv/nProc/nSendPosition} = TempUser's  (:1152-:1157)
10. cFileDB.SendDBSignalParm3(User,0,_MSG_DBSetIndex, ServerIndex, Sapphire, User)  (:1159)
11. For i in 0..65535 (:1161-:1171):
      GuildInfo[i].Citizen  -> cFileDB.SendGuildInfo(User, i)
      g_pGuildWar[i] != 0   -> SendDBSignalParm2(...,_MSG_War, i, g_pGuildWar[i])
      g_pGuildAlly[i] != 0  -> SendDBSignalParm2(...,_MSG_GuildAlly, i, g_pGuildAlly[i])
12. If TransperCharacter: SendDBSignalParm2(...,_MSG_TransperCharacter, 0, 0)  (:1173)
13. Steady state: WSA_READ (:877) -> Receive() -> ReadMessage() loop -> ProcessClientMessage (:1339)
      -> FLAG_GAME2DB + ID range gate -> cFileDB.ProcessMessage(msg, User)  [out of component]
```

### 2.3 NP admin connection flow (port 8895)

```
1. NPTool connects; WSAAsyncSelect posts WSA_ACCEPTADMIN = WM_USER+5
2. MainWndProc case WSA_ACCEPTADMIN (:951)
3. ReadAdmin() (:953) re-reads Admin.txt on EVERY accept (IP allow-list reload)
4. WSAGETSELECTERROR(lParam) -> log "err,accept_admin accepting fails" + drop
5. TempUser.AcceptUser(AdminSocket.Sock, WSA_READADMIN) (:962)
6. Slot resolution:
   a. for i in [0..MAX_ADMIN]  if pAdmin[i].IP == TempUser.IP -> User = i   (:968-:975)
      (upper bound READ past the 10-element array)
   b. else for i in [0..MAX_ADMIN-1] if pAdminIP[i] != 0 && pAdminIP[i] == IP -> User = i
   c. no allow-listed IP -> close socket, log "err, wsa_acceptadmin request accept from a.b.c.d"
7. If pAdmin[User].Mode != USER_EMPTY -> close incoming + existing, mark slot empty, break
8. Slot initialisation (:1036-:1044): Level = -1, DisableID = 0, IP, Mode, Name[0] = 0, socket fields
9. Challenge generation (:1052-:1053): Encode1 = rand()%10000, Encode2 = rand()%10000
10. Send _MSG_NPReqIDPASS {Encode1, Encode2, ID=0} (:1055-:1065)
11. Steady state: WSA_READADMIN (:797) -> Receive() -> ReadMessage() loop
      -> ProcessAdminMessage(User, Msg) (:1463) -> FALSE closes the socket
12. First accepted command must be _MSG_NPIDPASS: echo Encode1+Encode2, account, password
```

### 2.4 NP command dispatch flow (inside ProcessAdminMessage)

```
1. Guard (:1467): Type must have FLAG_NP2DB, ID in [0, MAX_DBACCOUNT)
      violation -> log "err,packet:<type> ID:<id> Size:<size> KeyWord:<kw>" + return FALSE (socket closed)
2. switch(std->Type):
   _MSG_NPCreateCharacter   (:1481) -> result 0..4 reply to pAdmin[conn]
   _MSG_NPNotice            (:1600) -> broadcast to all pUser slots OR targeted to one pUser slot
   _MSG_NPIDPASS            (:1680) -> challenge+password+level authentication
   _MSG_NPReqAccount        (:1738) -> DBReadAccount -> MSG_NPAccountInfo + admin chat summary
   _MSG_NPReqSaveAccount    (:1850) -> DBWriteAccount with disk password restored
   _MSG_NPDisable           (:1927) -> immediate DisableAccount  OR deferred saving-quit
   _MSG_NPEnable            (:1957) -> EnableAccount (clears Year/YearDay)
   _MSG_NPDonate            (:1982) -> writes ../../Common/ImportDonate/<stamp+rand>
3. Any unhandled Type falls through the switch and returns TRUE (socket stays open, no log)
```

### 2.5 Redirect / transfer-client flow (outbound)

```
1. DBSrv -> redirect server: ConnectServer(...) sends int INITCODE 0x1F11F311 (Code/CPSock.cpp:249)
2. CFileDB::ProcessMessage case _MSG_ReqTransper (Code/DBSrv/CFileDB.cpp:243)
     validates ID/slot, requires TransperCharacter != 0 && AdminClient.Sock != 0 (CFileDB.cpp:281)
     rewrites the character name in the in-memory account image, calls
     DisableAccount(-1, account, 0, 0) (CFileDB.cpp:338) and forwards MSG_NPCreateCharacter
     to AdminClient (CFileDB.cpp:352)
3. Redirect server answers _MSG_NPCreateCharacter_Reply -> WSA_READADMINCLIENT (:756)
4. ProcessAdminClientMessage (:626):
     EnableAccount(-1, account)                                   (:662)
     Result == 0 -> DBReadAccount, zero file.Char[slot], _strupr, DBWriteAccount  (:669-:712)
     forward _MSG_ReqTransper {Result} to pUser[svr] when IdxName != 0            (:714-:719)
     Result 1/2/3 -> forward the reply unchanged to pUser[svr]
```

### 2.6 Timer / maintenance flow (1 Hz)

```
WM_TIMER(wParam == TIMER_SEC) (:748) -> ProcessSecTimer (:2013)
  1. CReadFiles::ImportUser()            every tick     (../../Common/ImportUser/*.*)
  2. CReadFiles::UpdateUser()            every tick     (../../Common/servNN/update/*.*)
  3. SecCounter++
  4. every 30 ticks: CReadFiles::ImportItem() + CReadFiles::ImportDonate()
  5. DrawConfig()                        every tick     (window status text render)
  6. every 30 ticks: CReadFiles::UpdateConnection()   (C:/wamp/www/servNN.htm)
                     MinCounter++; every 30 MinCounters (== 900 ticks):
                        CReadFiles::UpdateConnectionData() (../../Common/dataNN.csv)
                        HourCounter++
  7. every 600 ticks: CReadFiles::WriteRanking()      (../../Common/Ranking.txt)
  8. tm_mday != LastLogDay  -> StartLog("A")           (log-file rotation)
  9. tm_mday != LastDayLogDay -> DayLog_ItemLog(), DayLog_ExpLog(), LastDayLogDay = tm_mday
 10. hh:mm:ss == 00:00:00 && tm_wday == 0 (Sunday):
        for each guild with Fame != 0: Fame = 0 and SendGuildInfo to every live pUser slot
        CReadFiles::WriteGuildInfo()
```

### 2.7 Shutdown flow

```
1. Menu File->Exit (:1215) -> PostMessage(WM_CLOSE)
   or window close box -> WM_CLOSE (:1305)
2. MessageBox(MB_OKCANCEL|MB_ICONWARNING) "Desligando servidor, nao se esqueca de salvar as contas !"
3. Only on OK: WriteConfig() -> Config.txt (Sapphire, LastCapsule)
               fclose(fLogFile)
               DayLog_ExpLog()  -> .\DayLog\EXP_DB_<stamp>.txt  (full ./account/* scan)
               DayLog_ItemLog() -> .\DayLog\ITEM_DB_<stamp>.txt (counter flush + reset)
               DefWindowProc(WM_CLOSE) -> DestroyWindow
4. WM_DESTROY (:1326): WSACleanup(); PostQuitMessage(0)
5. Message loop exits -> WinMain returns msg.wParam
No account flush, no socket shutdown(), no cFileDB drain, no notification to game servers.
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| # | Rule Type | Rule Description | Location |
|---|-----------|------------------|----------|
| BR-01 | Access control | A game server may connect only if its IP is listed for the configured server group in `serverlist.txt`, and maps to slot `index-1` | `Code/DBSrv/Server.cpp:1095-1118` |
| BR-02 | Access control | A game-server peer must share the first three octets of the DBSrv `LocalIP` (same `/24`) | `Code/DBSrv/Server.cpp:1150-1188` |
| BR-03 | Session integrity | One slot per peer IP; a second connection from the same IP forces both sockets closed and the slot reset | `Code/DBSrv/Server.cpp:1086-1093`, `:1119-1136` |
| BR-04 | Bootstrap protocol | On admission the game server receives `_MSG_DBSetIndex(ServerIndex, Sapphire, slot)` then the full guild/war/ally snapshot and the transfer-mode flag | `Code/DBSrv/Server.cpp:1159-1174` |
| BR-05 | Access control | An NP client may connect only from an IP present in `Admin.txt` (re-read on every accept) | `Code/DBSrv/Server.cpp:353-391`, `:977-1003` |
| BR-06 | Authentication | NP login = challenge echo (`Encode1`/`Encode2`) + exact password match; identity is the account, privilege is `maxCharacterLevel - 1000`, and `maxCharacterLevel < 1000` fails | `Code/DBSrv/Server.cpp:1680-1736` |
| BR-07 | Authorization | Three privilege tiers: `> 0` read-only queries, `>= 2` global broadcast, `> 2` all mutations | `Code/DBSrv/Server.cpp:1611`, `:1742`, `:1852`, `:1929`, `:1959` |
| BR-08 | Account state | Password-prefix/`Year` flags classify an account: `@` or `Year != 0` = blocked, `_` = defective, `#` = disabled | `Code/DBSrv/Server.cpp:1816-1830`, `:305-307` |
| BR-09 | Character creation | NP character creation goes through 5 ordered gates with result codes 3,4,2,1,0 and remaps guild items | `Code/DBSrv/Server.cpp:1481-1598` |
| BR-10 | Guild numbering | Guild items are rewritten to a global guild id `40960 + tableIndex`, and the mapping is appended to `Trans.txt` | `Code/DBSrv/Server.cpp:229-253`, `:167-227` |
| BR-11 | Account mutation | NP save requires privilege `> 2`, an offline account, an existing account file, and preserves the on-disk password | `Code/DBSrv/Server.cpp:1850-1925` |
| BR-12 | Account mutation | Disabling an online account is deferred: a saving-quit is requested, `DisableID`/`Year`/`YearDay` are latched, and completion happens on logout | `Code/DBSrv/Server.cpp:1927-1955`, `:1359-1417` |
| BR-13 | Account mutation | Enabling an account requires privilege `> 2`, requires the account to be offline, and clears `Year`/`YearDay` | `Code/DBSrv/Server.cpp:1957-1980`, `:1419-1461` |
| BR-14 | Messaging | A notice is either a global broadcast (`Parm1 == 1`, empty account) or targeted at an online account resolved through the session table | `Code/DBSrv/Server.cpp:1600-1678` |
| BR-15 | Donation intake | A donation request is materialised as a timestamped file in `../../Common/ImportDonate` and consumed asynchronously | `Code/DBSrv/Server.cpp:1982-2006` |
| BR-16 | Packet admission | Inbound packets must carry the correct direction flag, an ID inside range, a `Size` in `[12, 8192]`, a matching checksum, and be preceded by `INITCODE` | `Code/DBSrv/Server.cpp:630-638`, `:1343-1354`, `:1467-1477`; `Code/CPSock.cpp:353-467` |
| BR-17 | Scheduling | All maintenance duties are expressed as modulo of a 1 Hz counter; `TIMER_MIN` is never armed | `Code/DBSrv/Server.cpp:748-754`, `:613`, `:2013-2084` |
| BR-18 | Logging | The log file is rotated when the calendar day changes; the file name embeds the creation timestamp | `Code/DBSrv/Server.cpp:2091-2113`, `:2052-2053` |
| BR-19 | Day logs | EXP and ITEM day logs are emitted on day change and on shutdown; ITEM counters are reset after writing; EXP reads only accounts written in the current month | `Code/DBSrv/Server.cpp:2055-2060`, `:2141-2166`, `:2202-2321` |
| BR-20 | Weekly reset | Guild fame is zeroed only on a Sunday tick whose wall clock is exactly 00:00:00, then broadcast and persisted | `Code/DBSrv/Server.cpp:2061-2083` |
| BR-21 | Shutdown | Shutdown requires explicit operator confirmation; it persists `Config.txt`, closes the log, emits both day logs; it never saves accounts | `Code/DBSrv/Server.cpp:1305-1324`, `:1326-1329` |
| BR-22 | Configuration | `Config.txt` holds exactly two positional values (`Sapphire`, `LastCapsule`); a missing file is non-fatal | `Code/DBSrv/Server.cpp:393-425`, `:518` |
| BR-23 | Startup gate | Process start aborts on missing `BaseMob/*`, WSA init failure, unresolvable server-group identity, or a configured-but-unreachable redirect server | `Code/DBSrv/Server.cpp:448-527`, `:575-581`, `:599-611` |
| BR-24 | Transfer mode | Character transfer is enabled iff `redirect.txt` was parsed AND the outbound connection succeeded | `Code/DBSrv/Server.cpp:589-611`, `:1173-1174` |

---

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 - Game-server admission by IP allow-list and slot mapping

**Overview**:
Port 7514 is not an open port. A TCP connection from an unknown address is accepted by the kernel and then immediately discarded by application logic. The allow-list is `g_pServerList[MAX_SERVERGROUP][MAX_SERVERNUMBER][64]`, populated by `BASE_InitializeServerList()` from `./serverlist.txt` (fallback `../../Common/serverlist.txt`) and matched against the server group that this host resolved for itself at boot.

**Detailed description**:
The component resolves its own identity in `WinMain` (`:542`-`:569`): `gethostname` yields the machine name, `getaddrinfo` yields its IPv4 addresses as text, and the first address that equals `g_pServerList[group][0]` for some group selects that group as `ServerIndex` and is stored as `LocalIP[0..3]`. Index 0 of each group is therefore reserved for the DBSrv host itself, and the ten remaining slots (`1..MAX_SERVERNUMBER-1`, where `MAX_SERVERNUMBER == MAX_SERVER + 1 == 11`) are the game servers. On accept, the peer's dotted IP is formatted (`:1082`) and linearly searched in exactly that row (`:1099`-`:1103`); on a hit the slot index becomes `i - 1`, i.e. game-server entries are renumbered to `0..9` to index `pUser[MAX_SERVER]`. Consequently the slot number is positional, not negotiated: inserting a server line in `serverlist.txt` silently renumbers every subsequent slot, and the `_MSG_DBSetIndex` value pushed to TMSrv follows the new numbering. The same positional table is what TMSrv uses to decide which servers are "in the group", so the two servers must be deployed with identical files.

There is a subtle identity rule: matching is done on the **textual** IP produced by `inet_ntop`, so `serverlist.txt` must contain canonical dotted-decimal addresses with no leading zeros and no hostnames. A hostname or an IPv6 address in the file can never match, and the process then reports "Can't get Server Group Index" and terminates. The same textual string is reused for the connection-time search, but with a separately generated string (`sprintf("%d.%d.%d.%d")` at `:1082`) - consistent only because both are plain dotted decimal.

Two independent slot-reuse paths exist. If the incoming IP already occupies a slot (`User != -1`), the code does not allocate a new slot; it requires `pUser[User].Mode == USER_EMPTY` and otherwise closes both the new and the old socket (`:1121`-`:1135`). If the IP was not previously known, the serverlist-search branch runs and the same emptiness test is applied. In effect, the slot is keyed by IP forever: a game server that reconnects always reuses its positional slot, which is what keeps the `cFileDB` session index space (`conn * MAX_USER + id`) stable across reconnects.

**Rule workflow**:
```
TCP connect from IP X
  -> format X as "a.b.c.d"
  -> slot = first i with pUser[i].IP == X            (reconnect case)
  -> else slot = first i in [1..10] with g_pServerList[ServerIndex][i] == "a.b.c.d"
  -> else: close(X), log "err wsa_accept request from <ip>", STOP
  -> if pUser[slot].Mode != USER_EMPTY: close(X), close(old), Mode=0, STOP
  -> subnet gate BR-02
  -> bind slot, push BR-04 bootstrap
```

---

### Business Rule: BR-02 - Same-subnet restriction for game servers

**Overview**:
Even an allow-listed address is rejected unless its first three octets equal the first three octets of `LocalIP` (`:1150`). The rule is a `/24` containment test, not an exact-IP test, and it is evaluated **after** slot resolution.

**Detailed description**:
`LocalIP` is derived from the host-name resolution performed at boot, and each octet is parsed with `sscanf(name, "%hhu.%hhu.%hhu.%hhu", ...)` (`:561`). The comparison at `:1150` deliberately omits the fourth octet, so any host in the DBSrv's own `/24` is admissible if it is also present in `serverlist.txt`. This is the mechanism that lets a game server and the database server coexist on a LAN segment while still rejecting a connection that arrives through a NAT or a second NIC whose address happened to be allow-listed, e.g. `127.0.0.1` versus `10.0.0.5`.

The rule produces an asymmetric failure mode: the socket is closed (`:1180`) **after** the slot may already have been resolved and after the "not empty" test has mutated state, and the log line is emitted at the same priority as a normal accept. Because the check is `/24`-based rather than per-address, a mis-configured or spoofed host inside the same subnet that is also listed (or that squats an allow-listed IP) passes the gate; the only remaining discriminator is the game-server protocol itself. When `LocalIP` cannot be resolved, boot aborts before this rule is ever evaluated.

Practically, the rule constrains deployment: DBSrv and all TMSrv instances of one group must live in one `/24`. The architecture report's deployment notes ("both servers write/read `../../Common/...`") already imply a co-located deployment, so the rule codifies that assumption. It has no configuration switch, no override, and no log category that would let an operator distinguish "same subnet but not listed" from "listed but different subnet" without reading the message text.

**Rule workflow**:
```
allow-list hit (BR-01 passed)
  -> if (X.o1,X.o2,X.o3) == (LocalIP.o1,LocalIP.o2,LocalIP.o3)
        accept: bind pUser[slot], bootstrap
     else
        close(socket of X), log "err,wsa_accept outer ethernet IP: a.b.c.d", STOP
```

---

### Business Rule: BR-03 - One session per peer IP

**Overview**:
`pUser[0..MAX_SERVER-1]` is a fixed pool of ten slots, one per game server. A slot is considered free only when `Mode == USER_EMPTY` (value 0, `Code/DBSrv/CUser.h:26`). The pool is never searched for "any free slot": a slot is bound to the peer's IP.

**Detailed description**:
`GetEmptyUser()` exists (`:2191`) but is dead code - no call site anywhere in the repository - so allocation is always by IP. The consequence is that a stale slot is a permanent denial of service for that one IP until the socket is reset: the "not empty" branch closes both the incoming and the stored socket and sets `pUser[User].Mode = 0` (`:1131`-`:1132`), which is intended as self-healing but also means an attacker on an allow-listed IP can disconnect a healthy game server. The admin pool behaves the same way (`:1007`-`:1019`), except that it additionally clears `pAdmin[User].Name[0]`.

Slot state is written field-by-field rather than by assignment: `IP`, `Mode`, `cSock.Sock`, and the three buffer positions are copied from `TempUser` (`:1152`-`:1157`), while `Level`, `DisableID`, `Encode1`, `Encode2`, `Name`, `Year`, `YearDay` and `Count` are **not** copied. For game-server slots that is harmless (those fields are only meaningful for admin slots); for admin slots the omission is compensated by the explicit initialisation at `:1036`-`:1044`. `Init` (the `INITCODE` handshake flag of `CPSock`) is never copied in either path, which is why every accepted peer must send `INITCODE` again on reconnect.

**Rule workflow**:
```
slot already bound to IP X and Mode != 0
  -> close(new socket of X)
  -> close(old socket of X)
  -> pUser[slot].Mode = 0
  -> STOP (game server must retry)
```

---

### Business Rule: BR-04 - Bootstrap snapshot pushed on admission

**Overview**:
Admission is not complete with the socket bind. The server immediately pushes a five-part snapshot to the newly attached game server: server index/sapphire/slot, all "citizen" guilds, all guild-war entries, all guild-ally entries, and (conditionally) the transfer-mode flag.

**Detailed description**:
The first message is `_MSG_DBSetIndex` sent through `cFileDB.SendDBSignalParm3(User, 0, _MSG_DBSetIndex, ServerIndex, Sapphire, User)` (`:1159`): the game server learns which group it belongs to (`ServerIndex`), the capsule/warehouse page count (`Sapphire`, from `Config.txt`), and its own slot number. TMSrv stores all three and uses them to compute session indices for every later request, so this packet is the anchor of the whole protocol; if it is lost or the game server starts before DBSrv, the game server's `pAccountList` index computation diverges from the server's.

The second part is a full sweep of the 65,536-entry guild tables (`:1161`-`:1171`). For every guild with `GuildInfo[i].Citizen != 0` one `_MSG_GuildInfo` message is emitted; for every non-zero `g_pGuildWar[i]` one `_MSG_War`; for every non-zero `g_pGuildAlly[i]` one `_MSG_GuildAlly`. Each message is sent with `SendOneMessage`, which performs an immediate `send()` (`Code/CPSock.cpp:686`-`:693`), so this loop can push hundreds or thousands of small TCP segments before control returns to the message pump. During that window no other socket event is processed.

The third part is conditional: if `TransperCharacter != 0` (i.e. `redirect.txt` existed and the outbound connection succeeded - BR-24), a `_MSG_TransperCharacter` with two zero parameters is sent (`:1174`). The flag is sticky for the life of the process; losing the redirect connection later does not clear it, although `CFileDB` re-checks `AdminClient.Sock` before using the channel (`Code/DBSrv/CFileDB.cpp:281`).

**Rule workflow**:
```
slot bound
  -> SendDBSignalParm3(_MSG_DBSetIndex, ServerIndex, Sapphire, slot)
  -> for g in 0..65535: Citizen(g) -> SendGuildInfo(slot, g)
  -> for g in 0..65535: war(g)     -> SendDBSignalParm2(_MSG_War, g, war)
  -> for g in 0..65535: ally(g)    -> SendDBSignalParm2(_MSG_GuildAlly, g, ally)
  -> if TransperCharacter: SendDBSignalParm2(_MSG_TransperCharacter, 0, 0)
```

---

### Business Rule: BR-05 - NP admin admission by IP allow-list, re-read on every accept

**Overview**:
Port 8895 admits only addresses listed in `Admin.txt`. The file is re-read inside the accept handler (`ReadAdmin()` at `:953`, definition `:353`-`:391`), so edits take effect without restarting DBSrv.

**Detailed description**:
`ReadAdmin` opens `Admin.txt` in text mode, and for each line replaces '.' with ' ' across 255 bytes and parses `"%d %d %d %d %d"` into `idx, a, b, c, d`. The resulting address is assembled as `(d << 24) + (c << 16) + (b << 8) + a` (`:382`), which is the little-endian in-memory form of `a.b.c.d` on x86 and therefore directly comparable with `accept()`'s `sin_addr.S_un.S_addr`. A line is ignored entirely when the parsed index is outside `[0, MAX_ADMIN)` (`:384`-`:385`), so a malformed line can never silently grant slot 0; a line whose address fields fail to parse yields `0.0.0.0`, which never matches a real peer.

The file is the *only* address filter for port 8895, and the table `pAdminIP[MAX_ADMIN]` is a plain array indexed by the first number on the line. Two entries with the same index simply overwrite each other, last one winning. There is no duplicate detection, no ordering rule, and no warning when the file is missing - the loop simply leaves the table as it was (initially all zeros, and zeros are skipped by the matcher at `:985`). The architecture report notes that a source exists at `../../DBSRV/Run/Admin.txt`, i.e. the working directory must be arranged so that the relative path resolves.

Admission then continues with the ephemeral-slot search. The first loop iterates `i <= MAX_ADMIN` (`:968`) over a ten-element array, reading one element past the end; the second loop is correctly bounded. When an allow-listed index is found, the code requires that slot to be `USER_EMPTY`, otherwise it closes both the new and the previous connection - there is no queuing and no "kick the older session" policy, so a half-open session from a crashed NPTool blocks the operator's IP until the stale socket is reset.

**Rule workflow**:
```
accept on 8895
  -> ReadAdmin(): Admin.txt -> pAdminIP[idx] = a.b.c.d
  -> select(AdminSocket.Sock) -> TempUser(IP, sock)
  -> slot = first i in [0..10] with pAdmin[i].IP == IP        (one element past the array)
  -> else slot = first i in [0..9] with pAdminIP[i] != 0 and == IP
  -> no slot: close socket, log "err, wsa_acceptadmin request accept from a.b.c.d", STOP
  -> slot busy: close socket, log "err, wsa_acceptadmin not empty previous slot i",
                close previous, Mode = 0, STOP
  -> initialise Level=-1, DisableID=0, Encode1/Encode2 = rand()%10000
  -> send _MSG_NPReqIDPASS{Encode1,Encode2}  (BR-06 must follow)
```

---

### Business Rule: BR-06 - NP authentication and privilege derivation

**Overview**:
Authentication is a two-factor-in-name-only sequence: proof that the peer received the per-connection random challenge, plus the plaintext account password read from the account file. Authorization is derived exclusively from the highest character level on the account: `adminLevel = maxLevel - 1000`, and `maxLevel < 1000` is a hard failure.

**Detailed description**:
`_MSG_NPIDPASS` (`:1680`-`:1736`) is the only branch that can set a non-negative `Level`. It upper-cases the submitted account name, then compares `pAdmin[conn].Encode1`/`Encode2` against the echoed values; a mismatch returns `FALSE`, which `MainWndProc` interprets as "close this socket" (`:863`-`:872`). If `Level` is already non-negative the packet is ignored with `TRUE` (a second login on an already-authenticated session cannot change identity). The account file is then read with `cFileDB.DBReadAccount`; a read failure is silently swallowed with `TRUE`, i.e. the socket stays open and unauthenticated, with no log line and no failure counter.

Password policy is encoded in the first byte of `AccountPass`: `'_'` ("defective") and `'@'` ("blocked") cause `FALSE` - the connection is dropped before the password is even compared (`:1701`-`:1704`). `'#'` ("disabled") is **not** checked here, so a `#`-prefixed account can still authenticate if the password matches; the state is only reported to operators (BR-08). The comparison itself is `strncmp(m->Pass, file.Info.AccountPass, ACCOUNTPASS_LENGTH)` (`:1708`) - plaintext, case-sensitive, fixed-length 12, with the stored value already forced to be NUL-terminated by `DBReadAccount`. No salt, no hash, no rate limit, no delay, and no audit record of a failed attempt.

Privilege is computed by scanning the four character slots for the highest `BaseScore.Level` (`:1715`-`:1719`) and subtracting 1000. A level of exactly 1000 yields admin level 0, which passes the "read-only" tier only if the comparison is strict (`> 0`), so effectively the first usable operator level is 1001. Because the check is over the *stored* characters, privilege follows the account's strongest character, is silently lost when that character is deleted, and can be gained by any gameplay defect that inflates a level. On success the branch copies the account name into `pAdmin[conn].Name`, logs `sys,Admin Login Success - Level: <n>` with the operator's IP, and echoes the same string back to the client.

**Rule workflow**:
```
_MSG_NPIDPASS
  -> _strupr(Account)
  -> if Encode1/Encode2 mismatch: return FALSE (socket closed)
  -> if Level != -1: return TRUE (already authenticated, ignored)
  -> DBReadAccount(Account); on failure: return TRUE (no log, still unauthenticated)
  -> if Pass[0] == '_' or '@': return FALSE (socket closed)
  -> if strncmp(Pass, stored, 12) != 0: return FALSE (socket closed)
  -> maxLevel = max over 4 slots of BaseScore.Level
  -> if maxLevel < 1000: return FALSE (socket closed)
  -> Level = maxLevel - 1000 ; DisableID = 0 ; Name = Account
  -> log "sys,Admin Login Success - Level: n" ; send same text to client
```

---

### Business Rule: BR-07 - Three privilege tiers

**Overview**:
The admin level is compared against three different constants in eight branches: `> 0` for read-only operations, `>= 2` (equivalently `< 2` rejects) for the global broadcast, and `> 2` for every state mutation.

**Detailed description**:
Read-only access is granted at `:1742` (`_MSG_NPReqAccount`) and again defensively at `:1797`. The broadcast privilege is checked at `:1611` with `if (pAdmin[conn].Level < 2) return TRUE;` - note the inverted form: an unauthorised broadcast is *silently ignored* rather than rejected, and no error message is sent, so an operator at level 1 sees a tool that "does nothing". Mutations - account save (`:1852`), disable (`:1929`), enable (`:1959`) - require `Level > 2` and, for save, additionally emit the string `"Not allowed"` to the client. `_MSG_NPCreateCharacter` (`:1481`) has **no level check at all**, and `_MSG_NPDonate` (`:1982`) has **no level check at all** either.

`Level` starts at `-1` (unauthenticated), so every tier test also functions as an authentication test except in the two branches that omit it. The skip-level behaviour is intentional in the notice branch (broadcast requires level 2 even though the targeted send would work at level 1), but it is undocumented and inconsistent with the message constants in `Code/Basedef.h` (`_MSG_NPNotice = (9 | FLAG_NP2DB | FLAG_DB2NP | FLAG_DB2GAME)`), which carry no privilege annotation.

Because `Level` is inherited from a character level, the effective mapping is: level 1001 = read, 1002 = read + broadcast, 1003 = read + broadcast + mutate. There is no path to revoke a privilege from a live session other than disconnecting it or deleting the character, and no path to grant "create character" or "donate" because those are unrestricted.

**Rule workflow**:
```
Level == -1                      -> only _MSG_NPCreateCharacter / _MSG_NPDonate reachable
Level  > 0                       -> _MSG_NPReqAccount
Level >= 2                       -> _MSG_NPNotice broadcast form
Level  > 2                       -> _MSG_NPReqSaveAccount, _MSG_NPDisable, _MSG_NPEnable
(no level test)                  -> _MSG_NPCreateCharacter, _MSG_NPDonate
```

---

### Business Rule: BR-08 - Account state classification by password prefix and Year

**Overview**:
Account state is not a column; it is encoded in the first byte of `AccountPass` and in `Info.Year`/`Info.YearDay`. Four states are defined: normal, blocked (`@` or `Year != 0`), defective (`_`), disabled (`#`), reported as `State` 0/1/2/3 in `MSG_NPAccountInfo` and as `Parm` 1/2/3 in `_MSG_NPState`.

**Detailed description**:
When an operator queries an account, the branch builds `MSG_NPAccountInfo` and fills `sm.State` by evaluating three independent `if` statements in sequence (`:1811`-`:1830`). The state is therefore last-writer-wins: an account whose password starts with `_` **and** whose `Year` is non-zero reports `2` (defective), because the `_` test comes after the blocked test. Each state also produces a Portuguese human-readable string ("Conta bloqueada.", "Conta Defeituosa.", "Conta Desativada.") that is concatenated with the account name and the four character names into one admin chat line (`:1832`-`:1838`), truncated to `MESSAGE_LENGTH - 2`. That line is the operator's primary confirmation that the write landed.

`SendAdminState` (`:297`-`:312`) mirrors the encoding towards the client: it is called with the account password and maps `@@`/`_`/`#` to `Parm` 1/2/3 - which means the response to "enable this account" or "disable this account" carries *the state of the password prefix*, not of `Year`. For any password not starting with one of the three markers, `Parm` is left uninitialised in a stack structure and the garbage value is sent (see Section 10). This defect is observable whenever an operator enables a normal account, because `EnableAccount` unconditionally calls `SendAdminState(conn, 0, _MSG_NPState, file.Info.AccountPass)` (`:1458`).

The `Year`/`YearDay` pair is the actual block mechanism used by the disable path (BR-12): the game server is expected to refuse login when `Info.Year != 0`. The password prefix is a second, independent mechanism used by external tooling and by `_MSG_NPIDPASS`; because the two are not synchronised, an account can be "blocked" by `Year` while its password still authenticates at the NP interface (only `_` and `@` are refused there).

**Rule workflow**:
```
state = 0 (normal)
if (AccountPass[0] == '@') or (Info.Year != 0): text="Conta bloqueada.",   state=1
if (AccountPass[0] == '_'):                    text="Conta Defeituosa.",   state=2
if (AccountPass[0] == '#'):                    text="Conta Desativada.",   state=3
send MSG_NPAccountInfo{State}; send admin chat "<account> < c0 | c1 | c2 | c3> - <text>"
```

---

### Business Rule: BR-09 - NP character creation gates and guild-item remapping

**Overview**:
`_MSG_NPCreateCharacter` (`:1481`-`:1598`) creates a character directly from an operator-supplied `STRUCT_MOB`, returning result codes: `3` (session already present), `4` (read or write failure), `2` (no free slot), `1` (creation refused by the store), `0` (success).

**Detailed description**:
The gates are strictly ordered and each one answers immediately. First the session table is consulted (`cFileDB.GetIndex(account)`): a non-zero index means the account is currently in memory - i.e. a player is logged in - and the request is rejected with `3` and the comment "That account is playing" (`:1500`-`:1507`). Second the account file is read; any failure yields `4`. Third the four character slots are scanned for the first with an empty `MobName`; if none is empty the reply is `2` ("No character space. delete first."). Fourth `cFileDB.CreateCharacter(account, name)` is called, which also rejects reserved account/character names of the form `COM0..COM9` and `LPT0..LPT9` (`Code/DBSrv/CFileDB.cpp:2244`-`:2256`) and refuses to overwrite an existing character file; a refusal becomes `1` ("There's Same Name Already"). Only then is the `STRUCT_MOB` copied into the read snapshot and written back with `_strupr` applied to the account name.

Before the write, the item loop (`:1556`-`:1569`) walks the 16 equipment slots and 63 carry slots and calls `ConvertGuildNumber(conn, item)` for every item whose `sIndex` is a guild artefact: `446`, `508`, `509`, `522`, and `526..537`. Note the loop bound is `MAX_EQUIP + MAX_CARRY - 1` while the branch keys on `i < 16`, so the last carry slot (index 79) is never inspected - an off-by-one that can leave a character holding an unmapped guild item. Note also that `ConvertGuildNumber` is passed the **admin connection index** as the "group" argument, not the target server or guild group; the mapping namespace is therefore the admin session, which is functionally arbitrary but persistent (BR-10).

The reply always goes to `pAdmin[conn].cSock`, not to a game server, because the request came from the NP tool; the tool is responsible for telling the game server to reload. All four failure paths and the success path send exactly one `MSG_NPCreateCharacter_Reply` carrying `Slot`, `Account`, `Name` and `Result`.

**Rule workflow**:
```
_MSG_NPCreateCharacter(Slot, Account, Mob)
  -> reply.Result = 0 ; fill Slot/Account/Name
  -> if GetIndex(Account) != 0            : Result = 3 ; send ; return
  -> if !DBReadAccount(Account)           : Result = 4 ; send ; return
  -> find first empty MobName slot in 4
     -> none                              : Result = 2 ; send ; return
  -> if !CreateCharacter(Account, Name)   : Result = 1 ; send ; return
  -> for i in 0..78: if item is guild item -> ConvertGuildNumber(conn, item)
  -> file.Char[empty] = Mob ; _strupr(Account)
  -> if !DBWriteAccount(file)             : Result = 4 ; send ; return
  -> send Result = 0
```

---

### Business Rule: BR-10 - Guild number translation table (`Trans.txt`)

**Overview**:
Guild identity is globally unique in storage but locally numbered per server. `ConvertGuildNumber` rewrites an item's guild reference to a synthetic global id in the range `40960 .. 40960 + MAX_TRANS - 1` and persists the `(group, localGuild) -> index` mapping in `Trans.txt`.

**Detailed description**:
The function extracts the item's guild number (`BASE_GetGuild`), scans the in-memory table `TransGuildConn[]`/`TransGuildIndex[]` for a matching pair (`:235`), and on a miss appends a new entry at the current high-water mark `CurrentTransInfo` and immediately rewrites `Trans.txt` (`:237`-`:245`). The synthetic id is computed as `i + MAX_TRANS * 2` where `MAX_TRANS == 20480`, i.e. `i + 40960`, and is stored in two item effects: `EF_HWORDGUILD` receives the high byte and `EF_LWORDGUILD` receives `id % 256` (`:249`-`:252`). The low-word write uses `% 256` rather than `% 65536` and the high-word write uses `HIBYTE(id)`, so for ids above `65535` the reconstruction is lossy; that is consistent with `i + 40960 <= 61439` while the table has room.

`Trans.txt` is the persistence contract. `ReadTrandGuildInfo` (`:167`) first reads a single integer and then repeats `"%d %d %d"` expecting the first number to equal `i + MAX_TRANS * 2`; any mismatch stops the parse, and the final `CurrentTransInfo` is set to the number of successfully parsed rows (`:202`) - the leading integer is effectively discarded after the read. `WriteTransGuildInfo` (`:207`) writes the count followed by one row per entry, and **does not check `fopen` for `NULL`** before calling `fprintf` (`:209`-`:211`), so a read-only working directory or a missing drive crashes the server at the moment a guild item is converted. Both functions operate on the process working directory, not on `../../Common`.

The table has no eviction, no compaction and no uniqueness check beyond the linear scan, and the append writes are performed with `fopen(..., "wt")` - a full rewrite of the file for every new mapping, on the UI thread, inside a packet handler. When `CurrentTransInfo` reaches `MAX_TRANS`, the append writes `TransGuildConn[MAX_TRANS]` and `TransGuildIndex[MAX_TRANS]`, one element past both arrays (`:239`-`:240`).

**Rule workflow**:
```
ConvertGuildNumber(group, item)
  -> g = BASE_GetGuild(item)
  -> find i in [0, CurrentTransInfo) with (conn==group && index==g)
  -> miss: TransGuildConn[i] = group ; TransGuildIndex[i] = g ; CurrentTransInfo++
           WriteTransGuildInfo()  (fopen "Trans.txt" unchecked)
  -> target = i + 40960
  -> item.stEffect[0] = {EF_HWORDGUILD, HIBYTE(target)}
     item.stEffect[1] = {EF_LWORDGUILD, target % 256}
```

---

### Business Rule: BR-11 - NP account save invariants

**Overview**:
`_MSG_NPReqSaveAccount` (`:1850`-`:1925`) is the only path that lets an operator rewrite an entire `STRUCT_ACCOUNTFILE`. It enforces four invariants: privilege `> 2`, the account must be offline, the account must exist on disk, and the password may not be changed.

**Detailed description**:
The offline test is `cFileDB.GetIndex(account) == 0` (`:1861`-`:1868`), and the failure message is explicit: "For saving, account should be disabled." - i.e. the operator is expected to disable (BR-12) first, which forces a save-and-quit so that the on-disk image is current, and only then edit. Because `GetIndex` returns `0` both for "not in memory" and for the legitimate session index `0` (server slot 0, user id 0), an account logged in as the very first user of the first server cannot be distinguished from an offline one; the save proceeds while the game server holds a copy, and the two copies then diverge.

The privilege escalation guard is partial. After computing `maxLevel` from the submitted data, the code only enters the guard when `maxLevel >= 2000` (`:1882`): an operator may not submit a character whose derived admin level exceeds their own, and at equal level they may only edit their own account (`strcmp(pAdmin[conn].Name, AccountName)`). For `maxLevel` in `[1000, 1999]` there is no check at all, so an operator at level 3 can write an account containing a level-1999 character - a persistent privilege-escalation primitive that survives the session that created it.

The existence test reads the current file and, on success, copies the **stored** password over the submitted one before writing (`:1913`-`:1915`). That is a deliberate rule (NP cannot change passwords) but it also means the submitted password field is never validated, and the write is a plain `DBWriteAccount` with no backup, no journal and no size/format validation of the ~4.7 KB structure. Success is reported twice: once as `Saving account [<name>] success` to the client, and once as `sys,<name> written` to the server log with the operator name and IP.

**Rule workflow**:
```
_MSG_NPReqSaveAccount(MSG_NPAccountInfo)
  -> if Level <= 2: send "Not allowed" ; return
  -> if GetIndex(AccountName) != 0: send "For saving, account should be disabled." ; return
  -> maxLevel = max(Char[i].BaseScore.Level)
  -> if maxLevel >= 2000:
        if maxLevel-1000 > Level:                      send "Set admin level error." ; return
        if maxLevel-1000 == Level && Name != Account:  send "Set admin level error." ; return
  -> if !DBReadAccount(existing): send "There's no account with that account name" ; return
  -> submitted.AccountPass = existing.AccountPass
  -> DBWriteAccount(submitted)
  -> send "Saving account [x] success" ; log "sys,x written" with operator IP
```

---

### Business Rule: BR-12 - Disable: immediate for offline accounts, deferred for online accounts

**Overview**:
Disabling an account sets `Info.Year`/`Info.YearDay` on the account file. If the player is online, the write is deferred until the game server has saved and logged the character out, so that the save cannot overwrite the block.

**Detailed description**:
`_MSG_NPDisable` (`:1927`-`:1955`) requires privilege `> 2`, terminates the account name, upper-cases it, and then branches on `GetIndex`. For an offline account it calls `DisableAccount(conn, name, m->Year, m->YearDay)` directly. For an online account it does **not** write: it sends `cFileDB.SendDBSavingQuit(IdxName, 1)` (`:1949`), which produces a `_MSG_DBSavingQuit` to the owning game server (`Code/DBSrv/CFileDB.cpp:2369`-`:2388`), and latches `pAdmin[conn].DisableID = IdxName`, `.Year`, `.YearDay`. The completion is event-driven: when the game server later reports `_MSG_DBSavingQuit` or `_MSG_DBSaveMob`, `CFileDB` looks up every authenticated admin whose `DisableID` matches the freed session index and calls back into this component's `DisableAccount` (`Code/DBSrv/CFileDB.cpp:1108`-`:1120` and `:1261`-`:1272`). That callback is the only reason `DisableAccount` is not `static` to this file.

`DisableAccount` (`:1359`-`:1417`) mutates the caller's buffer in place (`account[14] = 0; account[15] = 0; _strupr`), reads the file, and returns with `_MSG_NPNotFound` + a chat message when the read fails. If `Year != 0 && YearDay != 0` it declares the account already blocked, sends `_MSG_NPState` with `"1"`, and returns without writing. Otherwise it zero-terminates the password tail, stores `Year`/`YearDay`, writes the file, and - when called from an admin session (`conn >= 0`) - re-enters `ProcessAdminMessage` with a synthetic `_MSG_NPReqAccount` for the same account so the operator's window refreshes with the new state (`:1400`-`:1415`). That re-entrancy means one inbound packet can produce a read plus a full account-info packet before the original reply.

Only the deferred path has a failure mode worth noting: the latched `DisableID` is cleared in `DisableAccount` only when `conn >= 0`, and it is not cleared if the player never returns (for example, the game server crashes and the session is dropped by `_MSG_DBNoNeedSave`), leaving the operator's session permanently "armed" to disable that account on the next matching logout.

**Rule workflow**:
```
_MSG_NPDisable(account, Year, YearDay)
  -> if Level <= 2: return (silent)
  -> idx = GetIndex(account)
  -> idx == 0  : DisableAccount(conn, account, Year, YearDay)
                   -> read file ; not found -> _MSG_NPNotFound + text
                   -> already blocked -> _MSG_NPState(Parm "1")
                   -> else Year/YearDay written ; re-issue _MSG_NPReqAccount
  -> idx != 0  : SendDBSavingQuit(idx,1) ; DisableID = idx ; Year/YearDay latched
                   -> game server saves and quits
                   -> CFileDB sees logout -> DisableAccount(adminSlot, acc, Year, YearDay)
```

---

### Business Rule: BR-13 - Enable clears the block and reports the state

**Overview**:
`_MSG_NPEnable` (`:1957`-`:1980`) removes the block by zeroing `Year` and `YearDay`, and refuses to act while the account is in memory.

**Detailed description**:
The online test is the same `GetIndex != 0` used by save and disable, but the reaction differs: enable refuses outright with the chat line "Check session. already playing." rather than requesting a saving-quit. The rationale is that an in-memory account cannot be in a blocked state anyway (it is playing), so the request is treated as an operator error rather than a state transition. Because of the `0`-is-ambiguous index (BR-11), the "already playing" check is bypassed for session index 0.

`EnableAccount` (`:1419`-`:1461`) mirrors `DisableAccount`: terminate and upper-case the name, read the file, report `_MSG_NPNotFound` when missing, and short-circuit with `SendAdminState(conn, 0, _MSG_NPState, storedPassword)` when the account is already unblocked (`Year == 0 && YearDay == 0`). Otherwise it terminates the password tail, clears both fields, writes the file, and reports the state again. Note that `EnableAccount` does **not** reset the password prefix: an account blocked by an `@` prefix stays blocked at the game-server login path even after a successful "enable", because only `Year` is cleared.

**Rule workflow**:
```
_MSG_NPEnable(account)
  -> if Level <= 2: return (silent)
  -> if GetIndex(account) != 0: send "Check session. already playing." ; return
  -> EnableAccount(conn, account)
       -> read file ; not found -> _MSG_NPNotFound + "There's no account with that account name"
       -> Year == 0 && YearDay == 0 -> _MSG_NPState(Parm from password prefix)
       -> else Year = 0 ; YearDay = 0 ; DBWriteAccount ; _MSG_NPState(...)
```

---

### Business Rule: BR-14 - Notice routing (broadcast vs targeted)

**Overview**:
`_MSG_NPNotice` (`:1600`-`:1678`) has two modes selected by the payload, not by the message type: a broadcast when `AccountName` is empty and `Parm1 == 1`, and a targeted notice otherwise.

**Detailed description**:
The handler first terminates the account name and the 96-byte message at two positions. The broadcast test is `m->AccountName[0] == 0 && m->Parm1 == 1` (`:1609`); inside it, privilege `>= 2` is required (silently ignored otherwise, `:1611`), and the message is re-serialised with `Parm1 = 1, Parm2 = 1` to every `pUser[i]` whose `Mode != USER_EMPTY` and `cSock.Sock != NULL` (`:1614`-`:1635`). The two parameters are the game-server-side flags that make the client display the text as a global notice.

The targeted path requires only `Level > 0` (`:1640`) and resolves the account through `cFileDB.GetIndex(account)`, which searches the **in-memory session table** - so the target must be currently logged in. A miss produces the chat line "Specified user not connected. can't send notice." and returns. The index is then decomposed into `svr = idx / MAX_USER` and `id = idx % MAX_USER` and validated as `0 <= svr < MAX_SERVER` and `1 <= id < MAX_USER` (`:1655`); the lower bound of `1` is deliberate (id 0 is not addressable, consistent with the `0`-is-not-found convention). The notice is then sent to that one slot with `Parm1 = 0, Parm2 = 0`.

Text handling is length-bounded but not fully validated: `strncpy(sm.String, m->String, MAX_NOTIFY_LENGTH)` copies up to 96 bytes and does not force a terminator when the source is longer, which is possible because the packet's own `String` was only terminated at positions 95 and 94 by the handler - so a 96-byte payload stays unterminated in the outbound copy.

**Rule workflow**:
```
_MSG_NPNotice
  -> truncate AccountName[14..15], String[94..95]
  -> if AccountName empty && Parm1 == 1:
        if Level < 2: return (silent)
        for each live pUser slot: send _MSG_NPNotice{Parm1=1,Parm2=1,String}
        return
  -> if Level <= 0: return (silent)
  -> idx = GetIndex(AccountName) ; 0 -> "Specified user not connected. can't send notice."
  -> svr = idx/1000, id = idx%1000 ; bounds check
  -> send _MSG_NPNotice{Parm1=0,Parm2=0,String} to pUser[svr]
```

---

### Business Rule: BR-15 - Donation intake as file drop

**Overview**:
`_MSG_NPDonate` (`:1982`-`:2006`) does not touch the account at all. It materialises one file per request in `../../Common/ImportDonate`, named from the local timestamp plus a random suffix, containing `"<account> <amount>"`, and relies on `CReadFiles::ImportDonate` (invoked every 30 ticks, `:2023`) to apply it.

**Detailed description**:
The file name is built as `"%02d%02d%04d%02d%02d%02d%02d"` from `tm_mday, tm_mon, tm_year+1900, tm_hour, tm_min, tm_sec, rand()%255` (`:1995`) - note `tm_mon` is **not** incremented, so January is written as `00`. The last component is a random byte, which makes collisions possible within the same second (birthday bound over 255 values) and silently overwrites a previous donation: `fopen(..., "wt")` truncates. If the target directory does not exist, `fopen` returns `NULL` and the branch simply breaks out of the switch with `TRUE`, i.e. the request is acknowledged without any error, no log line and no file.

The branch has no privilege check, no account-existence check, no validation of `Donate` (it may be negative, zero, or enormous), and no audit record. Because `pAdmin[conn].Level` is not consulted, an unauthenticated socket - one that passed the IP allow-list but has not sent `_MSG_NPIDPASS` - can write arbitrary files into the donation intake directory. The only structural protection is the fixed directory and the fixed two-token content format, which the importer parses.

**Rule workflow**:
```
_MSG_NPDonate(AccountName, Donate)
  -> name   = sprintf("%02d%02d%04d%02d%02d%02d%02d", day, mon, year, h, m, s, rand()%255)
  -> file   = "../../Common/ImportDonate/" + name
  -> fopen(file,"wt") ; NULL -> break (silent)
  -> fprintf("%s %d", AccountName, Donate)
  -> fclose
  (later) ProcessSecTimer tick % 30 == 0 -> CReadFiles::ImportDonate()
```

---

### Business Rule: BR-16 - Packet admission and framing

**Overview**:
Every inbound byte stream passes five independent gates before it reaches any handler: the 4-byte `INITCODE` handshake, a `Size` window, a keyword-driven de-obfuscation and checksum, a direction flag, and an ID range check.

**Detailed description**:
The handshake and framing live in `CPSock::ReadMessage` (`Code/CPSock.cpp:353`-`:467`) and are shared with TMSrv. Because `CPSock::Init` is 0 for every accepted socket, the first four bytes received must equal `INITCODE` (`0x1F11F311`); anything else sets `ErrorCode = 2` and the caller logs and stops processing the buffer. Then the 12-byte header is decoded: `Size` at offset 0, `KeyWord` at 2 (an index into the 512-byte `pKeyWord` table), `CheckSum` at 3, `Type` and `ID` following. `Size` must lie in `[sizeof(HEADER), MAX_MESSAGE_SIZE] == [12, 8192]`; out-of-range sizes reset both buffer cursors. Payload bytes from offset 4 are de-obfuscated with a position-dependent arithmetic using the keyword pair, and the computed sum is compared to the transmitted `CheckSum`.

The checksum rule is unusual and important: a mismatch sets `ErrorCode = 1` **but still returns the decoded message pointer** (`Code/CPSock.cpp:458`-`:463`). The decision to drop it is made by each caller. `WSA_READ` checks `Error` *after* calling `ReadMessage` but *before* `ProcessClientMessage`, and breaks out of the loop (`:938`-`:944`), so corrupt game-server packets are dropped - but the break also abandons any further well-formed packets already buffered behind it. `WSA_READADMIN` performs the same check before `ProcessAdminMessage` (`:855`-`:861`), with an additional `_PACKET_DEBUG`-only structural check (`BASE_CheckPacket`) that exists solely in Debug builds (the define is set only by the Debug configuration, `Code/DBSrv/DBSrv.vcxproj:61`).

Above the transport, `ProcessClientMessage` requires `Type & FLAG_GAME2DB` and `ID` in `[0, MAX_USER]` (`:1343`) - the inclusive upper bound is an off-by-one that admits `ID == 1000`; `ProcessAdminMessage` requires `Type & FLAG_NP2DB` and `ID` in `[0, MAX_DBACCOUNT)` (`:1467`) - effectively no ID restriction for a 16-bit signed field, since `MAX_DBACCOUNT == 10000` and negative values are rejected. A game-server violation is logged and the packet dropped (socket kept); an NP violation is logged and the socket **closed** (`FALSE` return). `ProcessAdminClientMessage` uses the same NP rule (`:630`) but its return value is ignored by the caller (`:793`), so violations from the redirect peer are only logged.

**Rule workflow**:
```
bytes arrive -> CPSock::Receive() -> ReadMessage()
  -> first 4 bytes != INITCODE        -> ErrorCode=2, stop
  -> Size outside [12, 8192]          -> ErrorCode=2, stop
  -> Size > bytes buffered            -> wait for more
  -> de-obfuscate payload, compare checksum
       mismatch -> ErrorCode=1, message still returned (caller decides)
  -> caller: Error in {1,2} -> log + stop
  -> direction flag test (FLAG_GAME2DB / FLAG_NP2DB)
  -> ID range test
  -> dispatch
```

---

### Business Rule: BR-17 - One-second maintenance cadence

**Overview**:
All periodic work is driven by a single 1000 ms Win32 timer that increments `SecCounter`. Longer periods are expressed as modulo of that counter, but the code mixes "ticks" with "minutes" in its own comments.

**Detailed description**:
`SetTimer(hWndMain, TIMER_SEC, 1000, NULL)` (`:613`) is the only timer armed in the process; `TIMER_MIN` is never scheduled, therefore `ProcessMinTimer` (`:2086`-`:2089`, an empty body) is unreachable and the `else if (wParam == TIMER_MIN)` branch at `:752` is dead. Every tick runs `ImportUser` and `UpdateUser`, i.e. two directory scans plus per-file processing of operator drop folders (`../../Common/ImportUser/*.*`, `../../Common/servNN/update/*.*`), then `DrawConfig` (a full GDI redraw of the status text). Every 30th tick adds `ImportItem` + `ImportDonate`; the comment claims "Once every two minutes" but 30 ticks at 1 Hz is 30 seconds. Within the same 30-tick block, `UpdateConnection` writes the status HTML; every 30th of those (900 ticks, nominally 15 minutes) `UpdateConnectionData` appends the CSV. Every 600 ticks (10 minutes) the ranking file is rewritten.

The consequence is that the UI thread is continuously doing file I/O, so packet latency is a function of directory size. None of these services is interruptible, rate-limited or bounded in the number of files processed per tick. `SecCounter` is a plain `int` (`:52`) incremented without wraparound handling, so the modulo phases shift after overflow (well beyond realistic uptime, but worth noting because `MinCounter` and `HourCounter` are nested increments, not real clocks).

Two calendar duties are evaluated on the same tick: log rotation by `tm_mday` (`:2052`) and day-log emission by `tm_mday` (`:2055`). Neither is persisted; both are process-local and compare against `-1` at start, so the first tick after boot always emits both day logs.

**Rule workflow**:
```
WM_TIMER(TIMER_SEC)
  -> ImportUser, UpdateUser
  -> SecCounter++
  -> SecCounter % 30 == 0 -> ImportItem, ImportDonate
  -> DrawConfig
  -> SecCounter % 30 == 0 -> UpdateConnection ; MinCounter++ ;
                             MinCounter % 30 == 0 -> UpdateConnectionData ; HourCounter++
  -> SecCounter % 600 == 0 -> WriteRanking
  -> tm_mday != LastLogDay -> StartLog("A")
  -> tm_mday != LastDayLogDay -> DayLog_ItemLog, DayLog_ExpLog, LastDayLogDay = tm_mday
  -> Sunday 00:00:00 -> guild fame reset (BR-20)
```

---

### Business Rule: BR-18 - Log rotation by calendar day

**Overview**:
`StartLog("A")` (`:2091`-`:2113`) closes the current log and opens a new one whose name embeds `dd_mm_yyyy_hh_mm_ss_A`, and records `LastLogDay = tm_mday`.

**Detailed description**:
Rotation is triggered at boot (`:512`) and thereafter whenever `when.tm_mday != LastLogDay` (`:2052`). The comparison is against day-of-month, not against a full date or an ordinal day, which means the trigger fires on the first tick of a new calendar day - including the 1st of a month after the 31st - and would also fire spuriously if the host clock were moved backwards within the same month (the equality test is one-directional, so `LastLogDay` is simply overwritten). The file is opened in append mode (`"a+"`), so a same-second restart appends to an existing file rather than truncating.

`Log` (`:2115`-`:2139`) formats `[dd/mm/yyyy][hh:mm:ss]` optionally followed by `IP: a.b.c.d`, then `"<str2> <str1>"` and a newline into a 1024-byte stack buffer, writes it if `fLogFile` is non-NULL, and then calls `SetWindowText(hWndMain, LogTemp)`. That last call is not incidental: the window title is the live log tail, and every log line generates a `WM_SETTEXT`/`WM_NCPAINT` cycle. During a burst (for example the accept-time guild sweep logging errors, or a per-second import error), the title updates are a measurable share of UI-thread time.

The file handle is never protected against a failed `fopen` - `fLogFile` may be `NULL` and `Log` silently degrades to title-only, which loses the bootstrap diagnostics precisely when the working directory is wrong. The `.\\Log\\` directory must pre-exist; if it does not, `StartLog` produces a `NULL` handle and the failure is invisible except for the missing file.

**Rule workflow**:
```
StartLog(tag)
  -> if fLogFile != NULL: fclose (log "Logfile close fail!!" on error)
  -> name = ".\Log\DB_<dd>_<mm>_<yyyy>_<hh>_<mm>_<ss>_<tag>.txt"
  -> fLogFile = fopen(name, "a+")
  -> LastLogDay = tm_mday
Log(s1, s2, ip)
  -> "[dd/mm/yyyy][hh:mm:ss] " [+ "IP: a.b.c.d"] " s2 s1 \n"
  -> fprintf(fLogFile) if non-NULL ; SetWindowText(hWndMain, line)
```

---

### Business Rule: BR-19 - Day-log generation (EXP and ITEM)

**Overview**:
Two day logs are emitted into `.\\DayLog\\`: `EXP_DB_<stamp>.txt`, produced by scanning every account file, and `ITEM_DB_<stamp>.txt`, produced by draining the in-memory item counter. They are emitted on day change and on confirmed shutdown.

**Detailed description**:
`DayLog_ItemLog` (`:2284`-`:2321`) iterates `ItemDayLog[0..MAX_ITEMLIST-1]` (6,500 entries), skips zero counters, writes `ItemID`, the name from `g_pItemList[i].Name` and the count, then **zeroes the counter** (`:2317`). The counters are incremented elsewhere: the game servers send `_MSG_DBItemDayLog` with the item index, and `CFileDB` increments `ItemDayLog[ItemID].Num` (`Code/DBSrv/CFileDB.cpp:2044`-`:2057`) without any bounds check on `ItemID`. Emission is therefore destructive: if `fopen` fails, the function returns before the loop and the counters survive; if it succeeds, any crash mid-loop loses the remaining counts.

`DayLog_ExpLog` (`:2141`-`:2166`) opens its file and calls `DayLog_ReadAccountInDir("./account/*", fp)` (`:2202`-`:2282`), a recursive `FindFirstFile`/`FindNextFile` walk. Two filters define the rule: names beginning with `.` are skipped (`:2222`), and files whose last-write year/month differ from the current year/month are skipped, so only "fresh active accounts" are considered (`:2238`). For each surviving file the whole `STRUCT_ACCOUNTFILE` is read (`fread(&accBuffer, sizeof(STRUCT_ACCOUNTFILE), 1, fs)`), and for every character slot with a non-empty name, a line is written when `mobExtra[i].DayLog.YearDay == tm_yday - 1` (`:2266`) - i.e. when the stored day-of-year marks yesterday, printing `DayLog.Exp` as "received X exp today". That is a one-day-lag contract between TMSrv (which stamps `DayLog`) and DBSrv (which reports it).

The walk has two structural weaknesses. Recursion builds the child pattern as `sprintf(tmp, "./account/%s/*", fileName)` (`:2227`), hardcoding the `./account/` prefix instead of extending `dir`, so any directory deeper than one level is never visited (the recursion silently re-enters the first level). And the buffer reuse at `:2241`-`:2243` (`strcpy(tmp, dir); tmp[strlen(dir)-1] = 0; sprintf(tmp, "%s%s", tmp, fileName)`) truncates the trailing `*` and concatenates the file name into the same buffer, which is safe only because `dir` always ends with `/*`.

**Rule workflow**:
```
DayLog_ItemLog()
  -> fopen ".\DayLog\ITEM_DB_<stamp>.txt" ("a+") ; NULL -> log "err,daylog item open fail!!" + return
  -> for i in 0..6499: if ItemDayLog[i].Num == 0 skip
                       write "[stamp] ItemID:i ItemName:x ItemNum:n" ; ItemDayLog[i].Num = 0
  -> fclose
DayLog_ExpLog()
  -> fopen ".\DayLog\EXP_DB_<stamp>.txt" ("a+") ; NULL -> log + return
  -> walk ./account/*: skip dot-names ; descend one level ; keep files with
     ftLastWriteTime.year/month == now.year/month
  -> per file: read STRUCT_ACCOUNTFILE
       per slot with MobName: if mobExtra[i].DayLog.YearDay == tm_yday-1
          write "[stamp] <account> Char:<name> recebeu <exp> exp hoje."
  -> fclose
```

---

### Business Rule: BR-20 - Sunday midnight guild fame reset

**Overview**:
Guild fame (`GuildInfo[i].Fame`) is cleared once a week and the cleared value is broadcast to every attached game server, then persisted to `../../Common/GuildInfo`.

**Detailed description**:
The trigger is a compound equality on the current wall clock: `when.tm_hour == 0 && when.tm_wday == 0 && when.tm_min == 0 && when.tm_sec == 0` (`:2061`). `tm_wday == 0` is Sunday. The window is one second wide and must coincide exactly with a timer tick. Win32 `SetTimer` is not a real-time scheduler: it coalesces messages and is delayed by any work in progress. Since every tick performs at least two directory scans and one GDI redraw, the probability that a tick lands on the exact second is not guaranteed, and a single missed tick means the reset does not happen for a week. There is no catch-up, no "last reset" persistence and no log line on success or failure.

When the trigger does fire, the loop scans all 65,536 guild entries; for each guild with a non-zero `Fame` it zeroes the value and re-sends the full `STRUCT_GUILDINFO` to every active `pUser` slot (skipping `cSock.Sock == 0` and `Mode == 0`). After the sweep it calls `CReadFiles::WriteGuildInfo()` to persist the table. The ordering means a crash between the zeroing and the write leaves memory reset and disk stale, and the next reboot restores fame that was supposed to be cleared.

**Rule workflow**:
```
every tick:
  if (hh == 0 && wday == 0 && mm == 0 && ss == 0):
     for g in 0..65535:
        if GuildInfo[g].Fame != 0:
           GuildInfo[g].Fame = 0
           for s in 0..9: if pUser[s].cSock.Sock && pUser[s].Mode: SendGuildInfo(s, g)
     CReadFiles::WriteGuildInfo()
```

---

### Business Rule: BR-21 - Shutdown requires confirmation and does not save accounts

**Overview**:
`WM_CLOSE` (`:1305`-`:1324`) is the only shutdown path. It demands an explicit `IDOK`, then persists configuration, closes the log, emits both day logs, and destroys the window. It does not flush any account, does not notify the game servers, and does not call `WSACleanup` (that happens later, in `WM_DESTROY`).

**Detailed description**:
The dialog is `MessageBox(hWndMain, "Desligando servidor, nao se esqueca de salvar as contas !", "Aviso !! Desligando Servidor !!", MB_OKCANCEL | MB_ICONWARNING)`; cancellation (`rret != 1`) leaves the process running with all state intact. Confirmation runs, in order: `WriteConfig()` (rewrites `Config.txt` with the in-memory `Sapphire` and `LastCapsule`), `fclose(fLogFile)`, `DayLog_ExpLog()` (a full account-directory scan), `DayLog_ItemLog()` (counter drain), then `DefWindowProc(hWnd, WM_CLOSE, ...)` to destroy the window. The day-log calls happen **after** the log handle is closed, so any diagnostic they would emit goes only to the (dying) window title.

The operator-facing warning is the entire save semantics: nothing in this component iterates `cFileDB.pAccountList[]`, nothing sends `_MSG_DBSavingQuit` to game servers, and nothing waits for their acknowledgement. Accounts are written by the game servers during normal play (each `_MSG_DBSaveMob` / logout), so an operator who closes DBSrv while players are online silently discards everything since the last per-character save. Because the socket layer never calls `shutdown()` before exit, peers see an abrupt RST, and TMSrv responds by attempting DBSrv reconnection and then terminating itself (`Code/TMSrv/Server.cpp` reconnect path) - a deliberate cascade documented in the architecture report.

After `DestroyWindow`, `WM_DESTROY` (`:1326`-`:1329`) calls `WSACleanup()` and `PostQuitMessage(0)`, the message loop ends and `WinMain` returns `msg.wParam`. No destructor for `cFileDB`, `CPSock` buffers or `CRanking` runs deterministically beyond the normal static/global destruction at process exit - the `CPSock` instances are globals and do free their buffers in `~CPSock`, but the listening sockets and per-slot sockets are not explicitly closed.

**Rule workflow**:
```
WM_CLOSE
  -> MessageBox(OK/Cancel, warning)
  -> Cancel: return true (no action)
  -> OK    : WriteConfig() ; fclose(fLogFile)
             DayLog_ExpLog() ; DayLog_ItemLog()
             DefWindowProc(WM_CLOSE) -> DestroyWindow -> WM_DESTROY
  -> WM_DESTROY: WSACleanup() ; PostQuitMessage(0)
  -> WinMain returns
```

---

### Business Rule: BR-22 - Two-value configuration contract, fail-open

**Overview**:
`Config.txt` carries exactly two positional lines, `Sapphire <int>` and `LastCapsule <unsigned short>`. The file is read once at boot and rewritten on shutdown; a missing file is reported but does not stop the process.

**Detailed description**:
`ReadConfig` (`:393`-`:410`) opens `Config.txt` in text mode, shows `MessageBox("cant find Config.txt", "BOOTING ERROR")` when the open fails, and returns `FALSE`. `WinMain` calls it without inspecting the result (`:518`), so the process continues with `Sapphire = 8` (the compile-time initialiser, `:50`) and `LastCapsule = 0` (defined in `Code/DBSrv/CFileDB.cpp:54`). It then uses two `fscanf` calls with literal format strings `"Sapphire %d\n"` and `"LastCapsule %hu\n"`; a file whose text deviates (different order, different case, extra blank line, CRLF-only differences are tolerated by `\n`, but reordering is not) leaves one or both values untouched, again silently. `Sapphire` is propagated to every game server in the `_MSG_DBSetIndex` bootstrap (BR-04) and is used by `CFileDB` capsule logic, so a silent default changes capsule capacity for the whole group.

`WriteConfig` (`:412`-`:425`) rewrites both values, so the file is normalised on every clean shutdown. It writes `LastCapsule` with `"%d"` while reading it with `"%hu"` - harmless for values in range, but a value above 32767 written as a negative decimal would then be read back through `%hu` with modulo semantics. A failed `fopen` returns `FALSE`, ignored by the caller. Because the write is a truncating `"wt"`, a crash between truncation and the second `fprintf` leaves a `Config.txt` that no longer parses, and the next boot silently falls back to the defaults.

**Rule workflow**:
```
boot : fopen("Config.txt","rt") ; NULL -> MessageBox + return FALSE (caller ignores)
       fscanf("Sapphire %d", &Sapphire)
       fscanf("LastCapsule %hu", &LastCapsule)
shutdown: fopen("Config.txt","wt") ; NULL -> return FALSE (caller ignores)
       fprintf("Sapphire %d") ; fprintf("LastCapsule %d")
```

---

### Business Rule: BR-23 - Startup gate

**Overview**:
Four conditions abort the process, and one only abandons the message loop: missing `BaseMob` templates, WSA initialisation failure, unresolvable server-group identity, and a configured-but-unreachable redirect server.

**Detailed description**:
The `BaseMob` gate (`:448`-`:503`) opens `./BaseMob/TK`, `FM`, `BM`, `HT` in binary mode, reads one `STRUCT_MOB` each and copies `CurrentScore` to `BaseScore` (`:505`-`:508`). Any missing file shows a class-specific message and returns `FALSE` from `WinMain`, terminating the process before the sockets exist. The WSA gate (`:520`-`:527`) logs `err,wsainitialize fail` and returns `FALSE`. The identity gate (`:575`-`:581`) shows two messages - the second dereferences `g_pServerList[i][0]` where `i` is the loop-exit value, i.e. one past the group array - and then `return TRUE`, which ends `WinMain` without `WSACleanup` and without entering the message loop; the window created by `InitInstance` is destroyed by process exit. The redirect gate (`:599`-`:611`) is strict: if `redirect.txt` parses and the outbound `ConnectServer` fails, the process shows "there's redirect.txt but can't connect" and returns `FALSE`; the file must therefore be removed, not merely pointed at an offline host, to run without a redirect server.

`CReadFiles::ImportItem()` (`:583`) and `ReadTrandGuildInfo()` (`:584`) are called before the listeners start and their return values are ignored; `CReadFiles::ReadGuildInfo()` (`:615`) runs after the timer is armed. A failure in any of these leaves the process running with an empty item table, an empty guild table or an empty translation table - all of which are consumed later without validation.

**Rule workflow**:
```
BaseMob/TK, FM, BM, HT  : any missing -> MessageBox + exit FALSE
ListenSocket.WSAInitialize() : fail -> log + exit FALSE
serverlist group match  : none -> 2 MessageBoxes + return TRUE (no message loop)
redirect.txt present and connect fails -> MessageBox + exit FALSE
ImportItem / ReadTrandGuildInfo / ReadGuildInfo : failures ignored
```

---

### Business Rule: BR-24 - Transfer mode is enabled only by a successful redirect connection

**Overview**:
`TransperCharacter` (`:54`) is the single flag that switches character-transfer behaviour on, both in the bootstrap snapshot and in `CFileDB::ProcessMessage`'s `_MSG_ReqTransper` branch.

**Detailed description**:
The flag is set to `1` only inside the success arm of the `redirect.txt` block (`:609`) and is never cleared. `redirect.txt` is parsed with `fscanf(fp, "%s %d %s %s", sip, &port, adminclientid, adminclientpass)` (`:596`); `adminclientid` and `adminclientpass` are read into globals that are **never used anywhere in the repository** (verified by search: only the declaration, the `fscanf` and the extern declaration in `Server.h:95`-`:96`), so the redirect server apparently accepts unauthenticated peers or authenticates by address. `sip` is resolved by `inet_addr` inside `CPSock::ConnectServer`, so hostnames silently become `INADDR_NONE` and the connect fails - `redirect.txt` must carry a dotted IP.

The downstream contract is checked in two places: on admission the flag causes a `_MSG_TransperCharacter` to be pushed to the game server (`:1173`), and in `CFileDB` the transfer request is refused with result `4` when `TransperCharacter == 0 || AdminClient.Sock == 0` (`Code/DBSrv/CFileDB.cpp:281`). The second condition is the only runtime check on the outbound socket's health; if the redirect server disappears after boot, the flag stays set but transfers are refused with the same generic result code as a validation failure, and the only diagnostic is the log line `err,transper but not transper_server`.

**Rule workflow**:
```
boot: redirect.txt exists
  -> fscanf("%s %d %s %s") -> sip, port, adminclientid (unused), adminclientpass (unused)
  -> ConnectServer(sip, port, INADDR_ANY) (sends INITCODE)
       fail -> MessageBox + exit FALSE
       ok   -> log "sys redirection server connected" ; TransperCharacter = 1
admission: TransperCharacter -> push _MSG_TransperCharacter to the game server
transfer : TransperCharacter == 0 or AdminClient.Sock == 0 -> result 4
```

---

## 4. Component Structure

```
Code/DBSrv/
├── Server.cpp                     # THIS COMPONENT - 2,342 lines
│   ├── globals (44-99)            # hWndMain, hMainMenu, hInst, ServerIndex, Sapphire, Localip,
│   │                              # MinCounter/SecCounter/HourCounter, LocalIP, pAdminIP,
│   │                              # TransGuildConn/Index, adminclientid/pass, fLogFile,
│   │                              # LastLogDay/LastDayLogDay, TempUser, pUser[10], pAdmin[10],
│   │                              # AdminClient/AdminSocket/ListenSocket, cFileDB, g_pBaseSet[4],
│   │                              # ItemDayLog[6500], hFont/hDC/x/y
│   ├── TextOutWind (101)          # in-window text line writer (non-literal format argument)
│   ├── DrawConfig (117)           # status render: 10 game-server rows + 10 admin rows
│   ├── ReadTrandGuildInfo (167)   # Trans.txt -> TransGuildConn/Index, CurrentTransInfo
│   ├── WriteTransGuildInfo (207)  # table -> Trans.txt (fopen result unchecked)
│   ├── ConvertGuildNumber (229)   # (group, guild) -> global id 40960+i, writes item effects
│   ├── SendAdminSignal (255)      # MSG_STANDARD helper
│   ├── SendAdminMessage (268)     # MSG_NPNotice helper (chat line)
│   ├── SendAdminParm (283)        # MSG_STANDARDPARM helper - DEAD (no call site)
│   ├── SendAdminState (297)       # MSG_STANDARDPARM state helper (Parm may be uninitialised)
│   ├── InitApplication (314)      # WNDCLASS "MainClass" registration
│   ├── InitInstance (335)         # 800x600 window "DB Server"
│   ├── ReadAdmin (353)            # Admin.txt -> pAdminIP[10]; re-read on every admin accept
│   ├── ReadConfig (393)           # Config.txt {Sapphire, LastCapsule}
│   ├── WriteConfig (412)          # Config.txt rewrite on shutdown
│   ├── WinMain (427)              # composition root: base mobs, sockets, redirect, timer, pump
│   ├── ProcessAdminClientMessage (626)  # redirect peer protocol (1 message type)
│   ├── MainWndProc (743)          # message dispatcher: 6 socket events, 5 window messages
│   ├── ProcessClientMessage (1339)      # game-server packet filter -> cFileDB.ProcessMessage
│   ├── DisableAccount (1359)      # Year/YearDay writer + refresh (called from CFileDB)
│   ├── EnableAccount (1419)       # Year/YearDay clearer + state report
│   ├── ProcessAdminMessage (1463) # 549-line NP command interpreter (8 branches)
│   ├── ProcessSecTimer (2013)     # 1 Hz maintenance scheduler
│   ├── ProcessMinTimer (2086)     # empty; unreachable (TIMER_MIN never armed)
│   ├── StartLog (2091)            # day-rotated log file creation
│   ├── Log (2115)                 # log line + window-title echo
│   ├── DayLog_ExpLog (2141)       # EXP day log (account-directory scan)
│   ├── GetUserFromSocket (2169)   # socket -> pUser slot
│   ├── GetAdminFromSocket (2180)  # socket -> pAdmin slot
│   ├── GetEmptyUser (2191)        # first USER_EMPTY slot - DEAD (no call site)
│   ├── DayLog_ReadAccountInDir (2202)   # recursive account walk for the EXP log
│   ├── DayLog_ItemLog (2284)      # ITEM day log + counter reset
│   └── file_exists (2323)         # varargs fopen test - DEAD (no call site)
├── Server.h                       # 124 lines: 30 prototypes + 37 externs (the component's public
│                                  # surface: Log, pUser, pAdmin, cFileDB, ServerIndex, ItemDayLog, ...)
├── CUser.cpp/.h                   # slot entity (IP, Mode, cSock, Level, Encode1/2, DisableID, Year)
│                                  # AcceptUser() = accept + WSAAsyncSelect(FD_READ|FD_CLOSE)
├── CFileDB.cpp/.h                 # account/char store: session table, 40+ handlers, raw binary I/O
├── CReadFiles.cpp/.h              # bulk import/export, guild info, status HTML/CSV, ranking file
├── CRanking.cpp/.h                # 500-slot grind ranking + ranking system
├── DBSrv.vcxproj                  # 7 .cpp compiled; Debug defines _CRT_SECURE_NO_WARNINGS;_PACKET_DEBUG
├── DBSrv.rc / resource.h / icon1.ico / stdafx.h
└── Debug/, Release/, DBSrv/Release/    # MSBuild intermediates (not source)
Code/
├── CPSock.cpp/.h                  # shared transport: framing, obfuscation, checksum, INITCODE
└── Basedef.cpp/.h                 # shared domain: STRUCT_* definitions, message constants,
                                   # BASE_InitializeServerList, BASE_GetFirstKey, BASE_GetGuild
```

Internal organisation is **flat and monolithic**: one translation unit, no classes, 33 free functions and 26 file-scope globals. There is no header/implementation boundary inside the component - `Server.h` is the inter-TU contract, and `MainWndProc` plays the role a router class would play in an object-oriented design.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (compile-time, this component -> project)

```
Server.cpp
├── ../Basedef.h  ../ItemEffect.h               (types, constants, externs)
├── ../CPSock.h                                 (CPSock transport class)
├── CUser.h                                     (pUser/pAdmin slot type, AcceptUser)
├── CFileDB.h                                   (cFileDB global, STRUCT_ACCOUNTLIST, GuildInfo)
├── CRanking.h  CReadFiles.h                    (maintenance services)
└── Win32/SDK: winsock2.h ws2tcpip.h windows.h windowsx.h stdlib stdio fcntl io sys/types
    sys/stat sys/timeb string time
```

Symbol-level efferent edges out of `Server.cpp` (49 distinct external symbols):

| Target unit | Symbols used | Purpose |
|---|---|---|
| `Code/Basedef.cpp` | `BASE_InitModuleDir`, `BASE_InitializeBaseDef`, `BASE_InitializeServerList`, `BASE_GetGuild`, `BASE_CheckPacket` (debug) | boot, allow-list load, guild extraction, packet sanity |
| `Code/CPSock.cpp` | `WSAInitialize`, `StartListen` x2, `ConnectServer`, `Receive`, `ReadMessage`, `SendOneMessage`, `CloseSocket` | all transport |
| `Code/DBSrv/CUser.cpp` | `CUser::AcceptUser` | accept + async-select per socket |
| `Code/DBSrv/CFileDB.cpp` | `ProcessMessage`, `GetIndex` x2 forms, `GetAccountByChar`, `DBReadAccount` x5, `DBWriteAccount` x4, `CreateCharacter`, `SendDBSignalParm2` x3, `SendDBSignalParm3`, `SendGuildInfo` x2, `SendDBSavingQuit` | persistence and outbound protocol |
| `Code/DBSrv/CReadFiles.cpp` | `ImportItem`, `ImportUser`, `UpdateUser`, `ImportDonate`, `UpdateConnection`, `UpdateConnectionData`, `WriteRanking`, `ReadGuildInfo`, `WriteGuildInfo` | scheduled maintenance |
| Win32/Winsock | `RegisterClassA`, `CreateWindowExA`, `CreateMenu`, `CreatePopupMenu`, `AppendMenuA`, `SetMenu`, `GetMessageA`, `TranslateMessage`, `DispatchMessageA`, `DefWindowProcA`, `PostQuitMessage`, `PostMessageA`, `SetTimer`, `MessageBoxA`, `SetWindowTextA`, `BeginPaint`/`EndPaint`, `GetDC`/`ReleaseDC`, `CreateFontA`, `SelectObject`, `SetTextColor`, `TextOutA`, `FindFirstFileA`/`FindNextFileA`/`FindClose`, `FileTimeToSystemTime`, `GetLocalTime`, `WSACleanup`, `closesocket`, `gethostname`, `getaddrinfo`, `inet_ntop` | host, UI, log rendering, file walk |
| CRT | `time`, `localtime`, `timeGetTime` (winmm), `sprintf`, `sscanf`, `strncpy`, `memcpy`, `memset`, `strcmp`, `strncmp`, `_strupr`, `fopen`/`fclose`/`fprintf`/`fscanf`/`fgets`, `_open`/`_read`/`_close`, `rand` | formatting, files |

### 5.2 Reverse dependencies (who calls into this component)

| Caller | Symbol(s) | Notes |
|---|---|---|
| `Code/DBSrv/CFileDB.cpp` (3 sites) | `DisableAccount` | deferred-disable completion after logout / saving-quit |
| `Code/DBSrv/CFileDB.cpp`, `CRanking.cpp`, `CReadFiles.cpp` (120 sites) | `Log` | logging service |
| `Code/CPSock.cpp` (8 sites), `Code/Basedef.cpp` | `Log`, `hWndMain`, `CurrentTime`, `LastSendTime` | transport and shared-domain diagnostics |
| `Code/DBSrv/CFileDB.cpp` (66 sites), `CRanking.cpp`, `CReadFiles.cpp` | `pUser[]` | slot occupancy, user counts, targeted sends |
| `Code/DBSrv/CFileDB.cpp` (15 sites), `CRanking.cpp` | `pAdmin[]` | admin lookup for deferred disable and chat |
| `Code/DBSrv/CFileDB.cpp` (12 sites), `CReadFiles.cpp` | `cFileDB` | the singleton store instance is *defined here* |
| `Code/DBSrv/CFileDB.cpp` (2 sites) | `ItemDayLog[]` | item counter increments |
| `Code/DBSrv/CFileDB.cpp`, `CReadFiles.cpp` (7 sites) | `ServerIndex` | file path suffixes, ranking/status naming |
| `Code/DBSrv/CFileDB.cpp` (9 sites) | `Sapphire` | capsule capacity |
| `Code/DBSrv/CFileDB.cpp` (9 sites) | `g_pBaseSet[]` | class templates |
| `Code/DBSrv/CReadFiles.cpp`, `CUser.cpp` (4 sites) | `hWndMain` | window title, async-select target |
| `Code/DBSrv/CFileDB.cpp` (3 sites) | `ChargedGuildList` | guild report broadcast |
| Win32 | `MainWndProc` | registered as `WNDCLASS.lpfnWndProc`, invoked by `DispatchMessage` |

### 5.3 External dependencies

| Dependency | Version / form | Role | Failure mode |
|---|---|---|---|
| `ws2_32.lib` (Winsock 1.1 startup) | `WSAStartup(MAKEWORD(1,1))` | all networking | `WinMain` aborts |
| `winmm.lib` | `timeGetTime` | `CurrentTime` stamping for outbound packets | link-time |
| `user32/gdi32` | window, menu, font, text | operator console + log tail | silent |
| Win32 file API + CRT stdio | `FindFirstFile`, `_open`, `fopen` | the "database" | silent or crash (`WriteTransGuildInfo`) |
| `serverlist.txt` (`./` or `../../Common/`) | positional text | game-server allow-list + own identity | boot abort |
| `Admin.txt` (`./` or via working dir) | `idx a.b.c.d` lines | NP allow-list | all admin connections refused (logged) |
| `Config.txt` | 2 positional lines | Sapphire, LastCapsule | fail-open defaults (8 / 0) |
| `Trans.txt` | count + `target conn index` rows | guild id translation | silent parse stop; crash on write failure |
| `redirect.txt` | `ip port id pass` | transfer server endpoint | boot abort if unreachable; `id`/`pass` unused |
| `./BaseMob/{TK,FM,BM,HT}` | binary `STRUCT_MOB` | class base templates | boot abort |
| `../../Common/{ImportUser,ImportItem,ImportDonate,servNN/update,GuildInfo,Ranking.txt}` | operator drop folders | bulk I/O | per-service, silent |
| `C:/wamp/www/servNN.htm`, `../../Common/dataNN.csv` | absolute + relative | public status publishing | silent |
| `./Log`, `./DayLog` | directories | audit + day logs | `fopen` NULL, log lost |

### 5.4 Dependency chains (runtime)

```
WinMain -> InitApplication/InitInstance -> MainWndProc (registered)
WinMain -> BASE_InitializeServerList -> g_pServerList -> WSA_ACCEPT (BR-01)
WinMain -> AdminClient.ConnectServer -> WSA_READADMINCLIENT -> ProcessAdminClientMessage
WinMain -> SetTimer -> WM_TIMER -> ProcessSecTimer -> {CReadFiles x9, DayLog x2, DrawConfig, StartLog}
WSA_ACCEPT -> CUser::AcceptUser -> cFileDB.Send* -> CPSock::SendOneMessage -> send()
WSA_READ -> ProcessClientMessage -> cFileDB.ProcessMessage -> [CFileDB internals] -> DisableAccount (callback)
WSA_ACCEPTADMIN -> ReadAdmin -> pAdminIP -> slot bind -> _MSG_NPReqIDPASS
WSA_READADMIN -> ProcessAdminMessage -> {DBReadAccount|DBWriteAccount|CreateCharacter|
                                        GetIndex|GetAccountByChar|SendDBSavingQuit|
                                        DisableAccount|EnableAccount|ConvertGuildNumber}
```

---

## 6. Afferent and Efferent Coupling

Counting convention (C, no classes): the "components" are the 33 functions and the 26 global state objects defined in `Server.cpp`. **Afferent (Ca)** = number of distinct call sites/reference sites that target the unit (all translation units, including within the component). **Efferent (Ce)** = number of distinct external units (project symbols, Win32/CRT functions, file contracts) the unit itself touches. **Criticality** reflects the consequence of a defect: `High` = silent data loss, privilege bypass or crash; `Medium` = degraded availability or operator misbehaviour; `Low` = cosmetic/observability.

| Component (unit in `Code/DBSrv/Server.cpp`) | Afferent | Efferent | Critical | Evidence |
|---|---|---|---|---|
| `pUser[MAX_SERVER]` (state object) | 66 | 0 | High | 3 TUs dereference; slot binding in 3 branches |
| `Log` | 127 | 5 | High | 120 DBSrv sites + 7 shared sites |
| `pAdmin[MAX_ADMIN]` (state object) | 17 | 0 | High | auth + deferred disable state |
| `ProcessAdminMessage` | 2 | 14 | High | called by `MainWndProc:863`, `DisableAccount:1413` |
| `MainWndProc` | 2 | 21 | High | `WNDCLASS.lpfnWndProc` + `DispatchMessage` |
| `WinMain` | 1 | 23 | High | process entry |
| `ProcessSecTimer` | 1 | 14 | High | 9 `CReadFiles` services + 2 day logs |
| `cFileDB` (state object) | 12 | 0 | High | the store singleton |
| `DisableAccount` | 4 | 7 | High | 3 external callbacks + self |
| `SendAdminMessage` | 16 | 3 | Medium | chat/notice helper |
| `SendAdminSignal` | 5 | 3 | Medium | signal helper |
| `SendAdminState` | 4 | 3 | Medium | state helper (uninitialised `Parm`) |
| `DayLog_ItemLog` | 2 | 8 | Medium | shutdown + day change |
| `DayLog_ExpLog` | 2 | 8 | Medium | shutdown + day change |
| `DayLog_ReadAccountInDir` | 1 | 11 | Medium | recursion defect |
| `StartLog` | 2 | 6 | Medium | rotation |
| `DrawConfig` | 2 | 8 | Low | GDI render, 1 Hz |
| `ReadAdmin` | 1 | 4 | High | allow-list, OOB-adjacent parse |
| `ReadConfig` / `WriteConfig` | 1 / 1 | 4 / 4 | Medium | fail-open |
| `ReadTrandGuildInfo` / `WriteTransGuildInfo` | 1 / 1 | 4 / 3 | High | unchecked `fopen` on write |
| `ConvertGuildNumber` | 1 | 4 | High | index `MAX_TRANS` overflow |
| `ProcessClientMessage` | 1 | 3 | High | `>` vs `>=` ID bound |
| `ProcessAdminClientMessage` | 1 | 8 | Medium | return value ignored by caller |
| `EnableAccount` | 2 | 6 | Medium | state report |
| `GetUserFromSocket` | 1 | 0 | Low | `WSA_READ` only |
| `GetAdminFromSocket` | 1 | 0 | Low | `WSA_READADMIN` only |
| `TextOutWind` | 4 | 3 | Low | GDI text |
| `SendAdminParm` | 0 | 3 | Low | dead |
| `GetEmptyUser` | 0 | 0 | Low | dead |
| `file_exists` | 0 | 2 | Low | dead |
| `ProcessMinTimer` | 1 | 0 | Low | unreachable |
| `InitApplication` / `InitInstance` | 1 / 1 | 6 / 6 | Medium | window class/window |

Aggregate for the whole component: **afferent ≈ 6 dependent translation units** (`CFileDB.cpp`, `CReadFiles.cpp`, `CRanking.cpp`, `CUser.cpp`, `CPSock.cpp`, `Basedef.cpp`) and **efferent = 8 external contracts** (Winsock transport, Win32 window/UI, Win32+CRT file I/O, `CFileDB`, `CReadFiles`, `Basedef`, `CUser`/`CPSock`, the `serverlist/Admin/Config/Trans/redirect` file contracts). This matches the architecture report's `~6 / 8` estimate.

Cohesion assessment: the module is a **coincidental-to-logical cohesion** mix by modern standards - "process host", "access control", "admin interpreter" and "operations" share only the globals they manipulate. Function-level cohesion is better: `ReadAdmin`, `DayLog_ItemLog`, `ConvertGuildNumber` and each `ProcessAdminMessage` branch are individually focused. The 549-line `ProcessAdminMessage` is the cohesion hotspot: eight unrelated use cases share one switch, one buffer-convention preamble and one error-logging style.

---

## 7. Endpoints

This component exposes no HTTP surface. Its endpoints are two TCP listeners and one outbound TCP client, all carrying the same length-prefixed, obfuscated binary protocol defined in `Code/CPSock.h:42`-`:50` and `Code/Basedef.h:925`-`:930` (12-byte header: `Size:int16`, `KeyWord:int8`, `CheckSum:int8`, `Type:int16`, `ID:int16`, `ClientTick:uint32`).

| Endpoint | Direction | Protocol | Accepted peers | Framing / limits | Handler |
|---|---|---|---|---|---|
| `0.0.0.0:7514` (`DB_PORT`) | inbound listen | TCP, binary `MSG_*` with `FLAG_GAME2DB` | IP listed in `serverlist.txt` for `ServerIndex`, entries 1..10, and same `/24` as `LocalIP` | `INITCODE 0x1F11F311` preamble; `Size` in [12, 8192]; checksum; keyword de-obfuscation | `WSA_ACCEPT` `:1068`, `WSA_READ` `:877` -> `ProcessClientMessage` `:1339` |
| `0.0.0.0:8895` (`ADMIN_PORT`) | inbound listen | TCP, binary `MSG_NP*` with `FLAG_NP2DB` | IP listed in `Admin.txt` (`idx a.b.c.d`) | same framing; extra `BASE_CheckPacket` sanity only in Debug builds | `WSA_ACCEPTADMIN` `:951`, `WSA_READADMIN` `:797` -> `ProcessAdminMessage` `:1463` |
| `<sip>:<port>` from `redirect.txt` | outbound client | TCP, binary, `MSG_NPCreateCharacter` / `_MSG_NPCreateCharacter_Reply` | configured transfer/redirect server | `INITCODE` sent immediately on connect (`Code/CPSock.cpp:249`) | `WSA_READADMINCLIENT` `:756` -> `ProcessAdminClientMessage` `:626` |

### 7.1 Game-server (7514) message surface accepted by this component

The component itself only validates direction/ID and forwards; the full message set is in `CFileDB::ProcessMessage`. Messages that pass this component's filter:

| Type constant | Value | Direction flag check | Handled by |
|---|---|---|---|
| `_MSG_DBAccountLogin` | `3 | 0x0800` | `FLAG_GAME2DB` | `CFileDB` |
| `_MSG_DBCharacterLogin` | `4 | 0x0800` | `FLAG_GAME2DB` | `CFileDB` |
| `_MSG_ReqTransper` | `170 | ... | 0x0800` | `FLAG_GAME2DB` | `CFileDB` -> `AdminClient` |
| `_MSG_GuildZoneReport` | `13 | 0x0800` | `FLAG_GAME2DB` | `CFileDB` |
| `_MSG_War`, `_MSG_GuildAlly`, `_MSG_GuildInfo` | `14`, `18`, `19` (+`0x0800`) | `FLAG_GAME2DB` | `CFileDB` |
| `_MSG_DBItemDayLog` | `21 | 0x0800` | `FLAG_GAME2DB` | `CFileDB` -> `ItemDayLog[]` |

### 7.2 NP admin (8895) command surface - complete

| Command (`Code/Basedef.h`) | Value | Min level | Reply | Branch |
|---|---|---|---|---|
| `_MSG_NPIDPASS` | `2 | 0x2000` | none (this *is* auth) | `_MSG_NPState`/chat on success, socket close on failure | `:1680` |
| `_MSG_NPReqAccount` | `3 | 0x2000` | `> 0` | `MSG_NPAccountInfo` + chat line; `_MSG_NPNotFound` | `:1738` |
| `_MSG_NPReqSaveAccount` | `6 | 0x2000` | `> 2` | chat "Saving account [x] success" / error texts | `:1850` |
| `_MSG_NPDisable` | `7 | 0x2000 | 0x1000` | `> 2` | `_MSG_NPState`, `_MSG_NPNotFound`, refreshing `MSG_NPAccountInfo` | `:1927` |
| `_MSG_NPEnable` | `8 | 0x2000 | 0x1000` | `> 2` | `_MSG_NPState`, `_MSG_NPNotFound`, chat "Check session. already playing." | `:1957` |
| `_MSG_NPNotice` | `9 | 0x2000 | 0x1000 | 0x0400` | `>= 2` broadcast, `> 0` targeted | re-emitted to `pUser[svr]` | `:1600` |
| `_MSG_NPCreateCharacter` | `11 | 0x2000 | 0x1000` | **none** | `MSG_NPCreateCharacter_Reply` with Result 0..4 | `:1481` |
| `_MSG_NPDonate` | `13 | 0x2000 | 0x1000` | **none** | none (file drop only) | `:1982` |

Declared but **not** handled by this component: `_MSG_NPAppeal` (`16 | 0x0400 | 0x0800 | 0x1000`), `_MSG_NPState` as an inbound request, and every `FLAG_DB2NP`-only type. They fall through the switch and are acknowledged with `TRUE` (socket kept, nothing logged).

### 7.3 Redirect peer surface (outbound socket)

| Message | Processed | Effect |
|---|---|---|
| `_MSG_NPCreateCharacter_Reply` (`12 | 0x1000 | 0x2000`) | yes | enable account; on Result 0 delete the transferred character slot from the account file and forward `_MSG_ReqTransper` to the owning game server |
| any other type | no | `ProcessAdminClientMessage` returns `FALSE`; caller ignores it, so only a log line is produced |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / format | Error handling |
|---|---|---|---|---|
| TMSrv game servers | Internal service, inbound | account/character persistence, guild state, item day counters | TCP `DB_PORT` 7514, custom binary packets, 12-byte header, `INITCODE` preamble, keyword obfuscation + additive checksum, max 8192 B | IP allow-list + `/24` gate; invalid flag/ID logged and dropped; checksum failure logged and the packet loop broken; socket closed on `Receive()` failure; no retry, no ACK, no flow control |
| NPTool admin client | Internal/privileged, inbound | account admin, notices, character creation, donations | TCP `ADMIN_PORT` 8895, same framing, `FLAG_NP2DB` | `Admin.txt` allow-list re-read per accept; `FALSE` return closes the socket on any validation/auth failure; failures mostly unlogged (only packet rejects and auth success are logged) |
| Redirect / transfer server | External service, outbound | cross-server character transfer | TCP client, `INITCODE` on connect, `MSG_NPCreateCharacter` out / `_Reply` in | fail-fast at boot (`MessageBox` + exit); at runtime `TransperCharacter == 0 \|\| AdminClient.Sock == 0` yields result 4; no reconnect |
| `CFileDB` account store | Internal library | all record persistence | in-process calls, raw `STRUCT_ACCOUNTFILE` (≈4.7 KB) images | return codes checked at every call site in this component; no transaction, no rollback |
| `CReadFiles` bulk services | Internal library | import/export, status publishing, ranking | in-process calls + files | return values void; all failures silent |
| Account/char file tree (`./account/<K>/<NAME>`, `./char/<K>/<NAME>`, `./capsule/<n>`) | Data store | primary persistence | fixed-size binary records, `_open/_read/_write` | errors logged inside `CFileDB`; no backup/journal |
| `Config.txt` | Configuration | Sapphire, LastCapsule | 2 positional text lines | fail-open; no validation; write on shutdown |
| `Admin.txt` | Configuration | NP allow-list | `idx a.b.c.d` per line | missing/malformed -> no admins |
| `serverlist.txt` / `serverlist.bin` | Configuration | group identity + game-server allow-list | `group index address` per line; obfuscated binary copy written at boot | missing -> boot abort |
| `redirect.txt` | Configuration | transfer server endpoint | `ip port id pass` | present-but-unreachable -> boot abort; `id`/`pass` unused |
| `Trans.txt` | State file | guild id translation | count + `target conn index` | read tolerant, write unchecked (`fopen` NULL deref) |
| `./Log/DB_*.txt` | Audit sink | operational log | text, day-rotated, `a+` | `NULL` handle -> title-only logging |
| `./DayLog/{EXP,ITEM}_DB_*.txt` | Audit sink | daily business reports | text, timestamped, `a+` | `fopen` failure logged and skipped |
| `../../Common/ImportDonate/*` | Operator channel (written by this component) | donation intake queue | one file per request, `"<account> <amount>"` | `fopen` failure silently ignored |
| `../../Common/{ImportUser,ImportItem,servNN/update}` | Operator channel (read by `CReadFiles`) | bulk import | text files | silent |
| `C:/wamp/www/servNN.htm`, `../../Common/dataNN.csv` | Publishing | population status | HTML / CSV | silent |
| Windows message queue / timers | OS service | the entire control flow | `WSAAsyncSelect` + `WM_TIMER` + `WM_PAINT` | no queue-depth limits; a slow handler delays every notification |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / observation |
|---|---|---|---|
| Reactive dispatcher (Reactor) | `WSAAsyncSelect` posts `WM_USER+n` messages; `MainWndProc` switches on the message id | `MainWndProc` `:743`-`:1337`; `Code/CPSock.h:25`-`:31` | Single-threaded event loop. No per-connection state machine object; the socket is the identity and `GetUserFromSocket`/`GetAdminFromSocket` are linear scans |
| Table-driven command dispatch | Two large `switch(std->Type)` blocks | `ProcessAdminMessage` `:1479`, `ProcessAdminClientMessage` `:640` | 8 + 1 branches; unknown types fall through silently |
| Fixed-size object pool | `pUser[MAX_SERVER]`, `pAdmin[MAX_ADMIN]`, `TempUser` staging | `:80`-`:87`, `:962`, `:1076` | Slot index is the session handle. Pool is keyed by IP, not allocated on demand |
| Session index encoding | `index = serverSlot * MAX_USER + userID`; decoded as `idx / MAX_USER`, `idx % MAX_USER` | `CFileDB::GetIndex` `Code/DBSrv/CFileDB.cpp:2328`; used at `:650`, `:1652`, `:1749` | 10,000-slot flat keyspace. `0` is overloaded as "not found" and as a valid key |
| Challenge/response authentication | Server-generated `Encode1`/`Encode2` echoed with the password | `:1052`-`:1065` (issue), `:1686`-`:1687` (verify) | Nonce is per-connection, 2 x [0, 9999]; replay across connections is blocked, but there is no lockout |
| Authenticator/Authorizer split (partial) | `Level` set only by `_MSG_NPIDPASS`; checked per branch | `:1726`, `:1611`, `:1742`, `:1852`, `:1929`, `:1959` | Two of eight branches forget the check |
| Two-phase (deferred) write / saga | Disable online -> `SendDBSavingQuit` -> latch `DisableID` -> completion on logout | `:1949`-`:1952`; `Code/DBSrv/CFileDB.cpp:1108`-`:1120`, `:1261`-`:1272` | Compensating action, no timeout, no persistence of the pending intent |
| Repository + singleton | Global `CFileDB cFileDB`; every persistence call is `cFileDB.X()` | `:89`, `Server.h:113` | Global instance rather than injected dependency; the repository is also the message router for game-server packets |
| Service locator / global registry | 26 globals exported by `Server.h` | `Server.h:68`-`:116` | Every other TU pulls state through the extern declarations - the dominant coupling mechanism |
| Adapter (file-as-database) | `DBReadAccount`/`DBWriteAccount` map structs onto fixed-size files | `Code/DBSrv/CFileDB.cpp:2390`, `:2518` | No SQL, no index, no locking; the file path is derived from the first character of the name (`BASE_GetFirstKey`) |
| Passive View | Window renders state; all input arrives as socket messages | `DrawConfig` `:117`, `WM_PAINT` `:1290` | The window is a status display + log tail; the menu is the only input surface |
| Protocol constants module | `FLAG_*` + `_MSG_*` constants and fixed `struct` layouts | `Code/Basedef.h:925`-`:2325` | Type identity is a bit-flag union (`FLAG_GAME2DB`, `FLAG_NP2DB`, `FLAG_DB2GAME`, ...), validated by mask |
| Lazy/background integration | Operator drop folders polled by the 1 Hz tick | `CReadFiles` via `ProcessSecTimer` `:2015`-`:2044` | Integration by shared file system instead of an API; no acknowledgement path back to the operator |
| Guard-clause heavy validation | Early `return TRUE`/`return FALSE` per condition | `ProcessAdminMessage` throughout | `TRUE` = keep socket, `FALSE` = close socket; the polarity is the error-handling contract |

Architectural decisions worth recording as context (not recommendations):

1. **Single-threaded by design.** All ten connections share one thread that also does directory scans and GDI painting. Correctness is preserved by never blocking on a peer (sends are `send()` on a 128 KB buffer with no waiting), but latency is unbounded.
2. **Direction-bit protocol multiplexing.** One framing serves three peer classes; the `FLAG_*` mask is the only tenant discriminator, and the listener a packet arrived on is *not* re-validated against the flag (a game-server socket could carry an `FLAG_NP2DB` packet and would be accepted by the `FLAG_NP2DB` test only if `ProcessAdminMessage` were reached with that `conn` - it is not, because `ProcessClientMessage` and `ProcessAdminMessage` are separate entry points; the isolation is structural, not checked).
3. **Persistence is delegated, not owned.** This component never writes an account except through `_MSG_NPReqSaveAccount`, `_MSG_NPCreateCharacter`, `DisableAccount`, `EnableAccount` and the transfer reply. Normal gameplay writes are `CFileDB`'s.
4. **Configuration is positional and fail-open.** Every file contract is line/position based, unvalidated, and non-fatal except the four boot gates.

---

## 10. Technical Debt & Risks

| Risk | Component area | Issue | Evidence | Impact |
|---|---|---|---|---|
| High | Admin accept | `for (i = 0; i <= MAX_ADMIN; i++)` reads `pAdmin[10]` on every admin connection | `:968`-`:975`; array declared `pAdmin[MAX_ADMIN]` (`:83`, `Server.h:107`) | Out-of-bounds read of adjacent global memory; comparison against an attacker-controlled IP in that slot could mis-bind a session |
| High | `_MSG_NPDonate` | No privilege check, no account validation, no amount validation | `:1982`-`:2006` | Any allow-listed peer, authenticated or not, writes files consumed as credits for any account |
| High | Privilege model | Admin level = `maxCharacterLevel - 1000`; no separate role store | `:1721`-`:1726` | A level defect or GM item becomes remote admin access; deleting the character silently revokes privilege |
| High | Save gate | Escalation guard only applies when `maxLevel >= 2000` | `:1882`-`:1898` | An operator at level 3 can persist a level-1999 character, then re-authenticate as a higher-privileged admin |
| High | `WriteTransGuildInfo` | `fopen("Trans.txt","wt")` result used without a NULL check | `:209`-`:211` | Crash (NULL deref) during character creation with a guild item or during table growth if the working directory is not writable |
| High | `ProcessClientMessage` | ID bound `std->ID > MAX_USER` instead of `>=` | `:1343` | `ID == 1000` passes; `GetIndex(conn, 1000)` yields `conn*1000+1000`, colliding with the next server's slot 0 and, for `conn == 9`, indexing `pAccountList[10000]` past the 10,000-element array |
| High | Shutdown | `WM_CLOSE` does not flush `pAccountList`, does not signal game servers | `:1305`-`:1324` | Loss of every write since the last per-character save when operated while players are online |
| High | Scheduler | Directory scans + GDI paint on the UI thread every second | `:2013`-`:2046` | Packet and accept latency proportional to `../../Common` contents; the accept backlog of both listeners can overflow |
| Medium | `ConvertGuildNumber` | Append writes `TransGuildConn[MAX_TRANS]` / `TransGuildIndex[MAX_TRANS]` when the table is full | `:237`-`:240` | Two-element OOB write followed by a corrupted `Trans.txt` |
| Medium | `SendAdminState` | `Parm` assigned only for `@`/`_`/`#`; structure not zero-initialised | `:297`-`:311` | Garbage `Parm` sent on every enable of a normal account |
| Medium | `_MSG_NPIDPASS` | `'#'` (disabled) prefix not rejected, unlike `'@'`/`'_'` | `:1701`-`:1704` | "Disabled" accounts can still authenticate to the admin interface |
| Medium | Auth failures | Failed password/level checks return `FALSE` (socket close) with **no log line** and no counter | `:1686`-`:1722` | Brute force and lockout incidents are invisible in the audit log |
| Medium | Admin slot squatting | Unauthenticated connections consume a slot indefinitely; no idle timeout, no challenge expiry | `:951`-`:1066` | Availability: a stalled NPTool blocks that operator's IP until the socket resets |
| Medium | Sunday reset window | Requires `hh:mm:ss == 00:00:00` exactly on a 1 Hz timer | `:2061` | Weekly fame reset can be skipped entirely; no log, no catch-up |
| Medium | `ReadConfig` fail-open | Return value ignored by `WinMain` | `:518` vs `:393`-`:410` | Silent fallback to `Sapphire = 8`, `LastCapsule = 0` for the whole group |
| Medium | Server-group abort | Second `MessageBox` indexes `g_pServerList[i][0]` with the loop-exit index | `:578` | One-row OOB read at a boot failure - the diagnostic path itself is unsafe |
| Medium | `DayLog_ReadAccountInDir` | Recursion hardcodes `./account/` instead of extending `dir`; `if (!strncmp(fileName,".",1))` only filters leading dots | `:2222`, `:2227` | Nested account trees are not fully logged; the report silently under-counts |
| Medium | `DayLog_ExpLog` | Reads a full `STRUCT_ACCOUNTFILE` without checking the read length; no `ferror` | `:2254` | Truncated files are reported as zero-exp records |
| Medium | Accept-time burst | Up to 65,536 sequential `send()` calls inside the accept handler, no backpressure | `:1161`-`:1171` | Long UI-thread block on a populated guild table; partial snapshots on failure |
| Medium | Time base | `CurrentTime = timeGetTime()` updated only inside `WSA_READ`/`WSA_READADMIN` | `:823`, `:902`; `Code/CPSock.cpp:541` | Packets sent outside a read (accept bootstrap, notices, guild broadcast) carry a stale `ClientTick` |
| Medium | Checksum policy | `ReadMessage` returns the packet on checksum mismatch; callers must self-drop and the drop path `break`s out of the pending-message loop | `Code/CPSock.cpp:458`-`:463`; `:938`-`:944` | One corrupt packet can strand well-formed packets already buffered behind it |
| Medium | No memory cleanup on boot abort | `freeaddrinfo` commented out; `WSACleanup` skipped when `ServerIndex == -1` | `:571`-`:581` | Leak and unclean Winsock teardown on the failure path only |
| Medium | Font handle churn | `CreateFont` on every `WM_PAINT`, never deleted | `:1296` | GDI handle growth per repaint; combined with per-second `SetWindowText` from `Log`, a steady repaint loop |
| Low | Dead code | `GetEmptyUser` `:2191`, `file_exists` `:2323`, `SendAdminParm` `:283`, `ProcessMinTimer` `:2086` + `:752`, `fDayLogFile` `:75`/`:98`, `adminclientid`/`adminclientpass` `:71`-`:72` | verified by call-site search (0 references) | Misleads maintainers; obscures the real control flow |
| Low | Duplication | Base-mob loading block duplicated between `WinMain` `:448`-`:508` and `WM_COMMAND` `:1221`-`:1285` | — | Divergence risk; the reload path lacks the null-class-fold guarantee |
| Low | `InitApplication` oddity | `if(!RegisterClass(&wc)) return RegisterClass(&wc);` | `:329`-`:330` | Calls `RegisterClass` twice on failure; the second call cannot succeed either |
| Low | Non-literal format | `sprintf(String, str)` in `TextOutWind` | `:107` | Latent format-string hazard if a caller ever passes peer data |
| Low | Reused stack buffer | `sprintf(Temp, "%s", Temp)`-style self-formatting in `Log` | `:2133` | Undefined behaviour in older CRT versions; tolerated by MSVC |
| Low | String termination | `strncpy(sm.String, ..., MESSAGE_LENGTH)` without forced NUL in `SendAdminMessage` and the notice re-serialisation | `:276`, `:1632`, `:1672` | Non-terminated 96-byte string can be sent |
| Low | Dead `_PACKET_DEBUG` path | Structural sanity check compiled only in Debug | `DBSrv.vcxproj:61` | Release builds have no structural validation at all |
| Low | Ambiguous "not found" | `GetIndex` returns `0` for both "absent" and "session 0" | `Code/DBSrv/CFileDB.cpp:2335`-`:2349` | The first user of the first server bypasses offline checks (save/disable/enable/notice) |
| Low | `ID` field is `short` | `MSG_STANDARD::ID` is 16-bit while session indices reach 9,999 | `Code/Basedef.h:929` | Not a defect today; constrains any future index redesign |

Not represented as risks here (documented as architectural facts instead): absence of SQL, absence of transactions, file-based persistence, single-threaded design. Section 9 records them; they are design decisions of the original codebase, not defects.

---

## 11. Test Coverage Analysis

### 11.1 Result: no automated tests exist for this component - or for the project

| Search performed (scope: repository root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`) | Result |
|---|---|
| File names matching `test`, `spec`, `mock`, `fixture`, `gtest`, `catch` across `*.cpp/*.h/*.hpp/*.c/*.vcxproj/*.sln/*.py/*.js/*.ts` | 0 files |
| Content search for `TEST(`, `TEST_F(`, `ASSERT_`, `EXPECT_`, `catch2`, `gtest`, `CppUnit`, `BOOST_AUTO_TEST` in all `*.cpp`/`*.h` | 0 matches |
| Content search for `assert(` in all `*.cpp`/`*.h` | 0 matches |
| Projects in `W2PP Code Project.sln` | 2 (`DBSrv`, `TMSrv`) - both `Application`; no test project |
| `Code/` top-level directories | `ClientPatch_v7662`, `DBSrv`, `release`, `TMSrv` - no test/stub/mocks directory |
| Directory tree (depth 2, excluding ignored folders) | no test directory anywhere |
| CI configuration (`*.yml`, `*.yaml`, `Makefile`, `*.ps1`, `*.bat`) | none at repository root or in `Code/` |
| `README.md` "Things to do" | explicitly lists `[ ] Create an documentation.` with no test item |

No test files, fixtures, harnesses, golden files or recorded protocol captures were found for `Code/DBSrv/Server.cpp`, for its collaborators (`CFileDB`, `CReadFiles`, `CRanking`, `CUser`) or for the shared transport (`CPSock`). The component is therefore **untested at every level**.

### 11.2 Coverage table

| Unit / scenario | Unit tests | Integration tests | Coverage | Test quality / consequence |
|---|---|---|---|---|
| `WinMain` boot sequence and the four abort gates | 0 | 0 | 0% | Each gate requires a specific broken deployment to exercise; no harness exists |
| `MainWndProc` message routing (11 cases) | 0 | 0 | 0% | Routing is only exercised by live peers |
| `WSA_ACCEPT` admission (allow-list, `/24`, slot reuse) | 0 | 0 | 0% | The `/24` rule and the "close both sockets" branch have no regression protection |
| `WSA_ACCEPTADMIN` + `ReadAdmin` allow-list | 0 | 0 | 0% | The `pAdmin[10]` OOB read is undetected by anything |
| `_MSG_NPIDPASS` authentication matrix (5 outcomes) | 0 | 0 | 0% | Password prefixes, level threshold and challenge echo are all unverified |
| `ProcessAdminMessage` (8 branches, ~40 decision points) | 0 | 0 | 0% | The highest-privilege surface in the system is unverified |
| `_MSG_NPCreateCharacter` result-code ladder (0..4) | 0 | 0 | 0% | Five mutually exclusive outcomes, none covered |
| `DisableAccount` / `EnableAccount` / deferred disable completion | 0 | 0 | 0% | The cross-TU callback contract is only exercised by a live save-and-quit |
| `ConvertGuildNumber` + `Trans.txt` round-trip | 0 | 0 | 0% | Persistence contract and the `MAX_TRANS` overflow untested |
| `ProcessSecTimer` cadence and day-boundary behaviour | 0 | 0 | 0% | Requires clock manipulation; no fake clock exists |
| `StartLog` / `Log` / day-log rotation | 0 | 0 | 0% | File naming and rotation are verified only by human inspection |
| `DayLog_ExpLog` / `DayLog_ReadAccountInDir` | 0 | 0 | 0% | Recursive walk defect and month filter untested |
| `DayLog_ItemLog` counter drain | 0 | 0 | 0% | Destructive semantics untested |
| `ProcessAdminClientMessage` (transfer reply) | 0 | 0 | 0% | Requires a redirect server |
| `WM_CLOSE` / `WM_DESTROY` shutdown | 0 | 0 | 0% | No assertion that logs/day logs are produced |

### 11.3 Manual/operational substitutes observed in the code

The only verification mechanisms in the component are runtime: the text log (`Log` + window title), the admin chat channel (`SendAdminMessage`), Chinese/Vietnamese-fallback `MessageBox` boot diagnostics, and the `_PACKET_DEBUG` structural check that exists **only in Debug builds** (`Code/DBSrv/DBSrv.vcxproj:61`; Release defines only `_CRT_SECURE_NO_WARNINGS;WIN32;NDEBUG;_WINDOWS`, `:83`). There is no self-test, no `--version`/health endpoint, no smoke script and no recorded fixture in the repository.

---

## Appendix A - Verified symbol index for `Code/DBSrv/Server.cpp`

| Symbol | Lines |
|---|---|
| globals (26 objects) | 44-99 |
| `TextOutWind` | 101-115 |
| `DrawConfig` | 117-165 |
| `ReadTrandGuildInfo` | 167-205 |
| `WriteTransGuildInfo` | 207-227 |
| `ConvertGuildNumber` | 229-253 |
| `SendAdminSignal` | 255-266 |
| `SendAdminMessage` | 268-281 |
| `SendAdminParm` | 283-295 |
| `SendAdminState` | 297-312 |
| `InitApplication` | 314-333 |
| `InitInstance` | 335-351 |
| `ReadAdmin` | 353-391 |
| `ReadConfig` | 393-410 |
| `WriteConfig` | 412-425 |
| `WinMain` | 427-624 |
| `ProcessAdminClientMessage` | 626-741 |
| `MainWndProc` | 743-1337 |
| — `WM_TIMER` | 748-754 |
| — `WSA_READADMINCLIENT` | 756-795 |
| — `WSA_READADMIN` | 797-875 |
| — `WSA_READ` | 877-949 |
| — `WSA_ACCEPTADMIN` | 951-1066 |
| — `WSA_ACCEPT` | 1068-1191 |
| — `WM_CREATE` | 1193-1209 |
| — `WM_COMMAND` | 1211-1288 |
| — `WM_PAINT` | 1290-1303 |
| — `WM_CLOSE` | 1305-1324 |
| — `WM_DESTROY` | 1326-1329 |
| `ProcessClientMessage` | 1339-1357 |
| `DisableAccount` | 1359-1417 |
| `EnableAccount` | 1419-1461 |
| `ProcessAdminMessage` | 1463-2011 |
| — `_MSG_NPCreateCharacter` | 1481-1598 |
| — `_MSG_NPNotice` | 1600-1678 |
| — `_MSG_NPIDPASS` | 1680-1736 |
| — `_MSG_NPReqAccount` | 1738-1848 |
| — `_MSG_NPReqSaveAccount` | 1850-1925 |
| — `_MSG_NPDisable` | 1927-1955 |
| — `_MSG_NPEnable` | 1957-1980 |
| — `_MSG_NPDonate` | 1982-2006 |
| `ProcessSecTimer` | 2013-2084 |
| `ProcessMinTimer` | 2086-2089 |
| `StartLog` | 2091-2113 |
| `Log` | 2115-2139 |
| `DayLog_ExpLog` | 2141-2166 |
| `GetUserFromSocket` | 2169-2178 |
| `GetAdminFromSocket` | 2180-2189 |
| `GetEmptyUser` | 2191-2200 |
| `DayLog_ReadAccountInDir` | 2202-2282 |
| `DayLog_ItemLog` | 2284-2321 |
| `file_exists` | 2323-2342 |

## Appendix B - External constants that govern this component

| Constant | Value | Source | Effect here |
|---|---|---|---|
| `DB_PORT` | 7514 | `Code/Basedef.h:45` | game-server listener |
| `ADMIN_PORT` | 8895 | `Code/Basedef.h:46` | NP listener |
| `MAX_SERVER` | 10 | `Code/Basedef.h:48` | `pUser` slots, guild sweep fan-out |
| `MAX_SERVERGROUP` | 10 | `Code/Basedef.h:49` | `ServerIndex` range, `serverlist.txt` rows |
| `MAX_SERVERNUMBER` | `MAX_SERVER + 1` = 11 | `Code/Basedef.h:50` | `serverlist.txt` columns; entry 0 is the DBSrv itself |
| `MAX_ADMIN` | 10 | `Code/Basedef.h:58` | `pAdmin` slots, `Admin.txt` indices |
| `MAX_USER` | 1000 | `Code/Basedef.h:56` | session-index divisor |
| `MAX_DBACCOUNT` | `MAX_USER * MAX_SERVER` = 10000 | `Code/Basedef.h:60` | NP packet ID bound, `pAccountList` size |
| `MAX_TRANS` | 20480 | `Code/Basedef.h:126` | guild translation range, id offset 40960 |
| `MOB_PER_ACCOUNT` | 4 | `Code/Basedef.h:71` | character slots per account, max-level scan |
| `MAX_ITEMLIST` | 6500 | `Code/Basedef.h:139` | `ItemDayLog` size, day-log loop |
| `MESSAGE_LENGTH` | 96 | `Code/Basedef.h:129` | admin chat truncation |
| `MAX_NOTIFY_LENGTH` | 96 | `Code/Basedef.h:128` | notice text |
| `ACCOUNTNAME_LENGTH` / `ACCOUNTPASS_LENGTH` | 16 / 12 | `Code/Basedef.h:65`-`66` | string truncation, password compare length |
| `MAX_MESSAGE_SIZE` | 8192 | `Code/CPSock.h:38` | framing upper bound |
| `INITCODE` | `0x1F11F311` | `Code/CPSock.h:40` | mandatory 4-byte preamble on every inbound socket |
| `USER_EMPTY` / `USER_ACCEPT` | 0 / 1 | `Code/DBSrv/CUser.h:26`-`27` | slot occupancy |
| `FLAG_GAME2DB` / `FLAG_NP2DB` | `0x0800` / `0x2000` | `Code/Basedef.h:936`, `:939` | direction gate |
| `TIMER_SEC` / `TIMER_MIN` | 0 / 1 | `Code/Basedef.h:136`-`137` | tick id; only `TIMER_SEC` is armed |
