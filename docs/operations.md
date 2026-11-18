# DBSrv operations (headless)

DBSrv is a console application. It opens no window and needs no desktop
session. The listener ports are unchanged: game-server `7514` and NP admin
`8895`.

## Runtime tree

The process sets its working directory to the executable's directory. The
existing layout is preserved:

```
<exe dir>/
  dbsrv.exe
  Config.txt
  serverlist.txt
  Admin.txt            # optional whitelist
  redirect.txt         # optional redirect link
  BaseMob/TK FM BM HT  # 816-byte base character records
  AttributeMap.dat     # 1 MiB attribute grid
  Import/Item/         # import drop directories
  Import/User/
  Import/Donate/
  Log/                 # log files
  DayLog/              # daily logs
../../Common/          # relative to <exe dir>
  ItemEffect.h
  itemlist.csv
  extraitem.csv
  InitItem.csv
  SkillData.csv
  Guilds.txt
```

A missing base-set file, `Config.txt`, `serverlist.txt` or `AttributeMap.dat`
is fatal: the server writes a diagnostic and exits non-zero.

`serverlist.txt` uses the legacy format `group number address`. Entry
`group 0` is the group's own address, matched against the machine's local IPv4
addresses; entries `1..10` are the game servers admitted from that address.
The loopback address `127.0.0.1` is always considered a local address so a
self-contained test tree can select group 0.

## Starting

```powershell
Set-Location <exe dir>
.\dbsrv.exe
```

The process runs until a shutdown command or signal. Startup diagnostics and
warnings go to the log and to standard error.

## Console commands

The console thread is started only when stdin is a terminal. (The integration
harness sets `W2PP_FORCE_CONSOLE=1` to feed commands through a pipe.)

| Command | Action |
|---|---|
| `status` | Prints the current group, local IP, game-server slots/open state and admin slots. |
| `readmobbase` | Reloads the four base character files (the old menu action). |
| `quit` | Graceful shutdown. |

The former window menu has no replacement for window painting; status is text.

## Shutdown

`quit` and `Ctrl+C`/`SIGTERM` use the same path: stop the listeners, close the
game/admin/redirect links, write `Config.txt`, guild data and the daily logs,
flush and close the log file, then exit with success.

## Loopback verification

`tests/integration/dbsrv_loopback.ps1` builds a minimal runtime tree, starts
DBSrv, and verifies:

- a game-server connection from loopback is accepted and receives the initial
  DB signals;
- an admin connection from the whitelisted loopback address is accepted and
  receives the ID/password challenge;
- a loopback redirect listener receives the four-byte init handshake;
- a connection from a non-whitelisted address is rejected.

Run it through CTest:

```powershell
ctest --preset x86-debug -R dbsrv-loopback --output-on-failure
```

Or directly:

```powershell
pwsh -NoProfile -File tests/integration/dbsrv_loopback.ps1 `
  -ExeDir build/x86-debug/Debug `
  -CommonDir build/Common
```

# TMSrv operations (headless)

TMSrv is a console application. It opens no window and needs no desktop
session. The game listener port is unchanged (`8281`), the DB link connects to
`7514` and the optional billing link uses the address in `biserver.txt`.

## Runtime tree

The process sets its working directory to the executable's directory, so the
existing layout is preserved:

```
<exe dir>/
  tmsrv.exe
  gameconfig.txt        # generated from defaults when absent
  localip.txt           # local address used for the listener and DB bind
  serverlist.txt        # group/number/address table
  biserver.txt          # optional billing host and port
  LevelItem.txt
  NPCGener.txt
  MobName.txt
  Language.txt
  heightmap.dat
  BaseMob/TK FM BM HT
  Admin.txt             # optional
  Log/
../../Common/           # relative to <exe dir>
  ItemEffect.h
  itemlist.csv
  extraitem.csv
  InitItem.csv
  SkillData.csv
  Guilds.txt
  AttributeMap.dat
  Settings/SancRate.txt
  Settings/QuestsRate.txt
  Settings/CompRate.txt
  Settings/MobMerc.txt
  Settings/CastleQuest.txt
```

`localip.txt` and `serverlist.txt` are required: when either is missing (or the
local address is not listed) the server logs and writes a diagnostic to
standard error and exits non-zero. The same applies to a listener that cannot
bind and to a DB link that fails both startup attempts.

## Starting

```powershell
Set-Location <exe dir>
.\tmsrv.exe
```

Startup order is preserved from the former `WinMain`: configuration, content
and base-set reads, `Reboot`, local address/server group selection, DB link,
billing link, guild report and the game listener. The DB connect is
asynchronous: the first attempt runs immediately, the second one 200 ms later
when the first fails, and the event loop keeps running between them. If both
fail the process performs a graceful shutdown and exits non-zero during boot.

## Console commands

The console thread is started only when stdin is a terminal. The integration
harness sets `W2PP_FORCE_CONSOLE=1` to feed commands through a pipe.

| Command | Action |
|---|---|
| `saveall` | Saves every online character (former menu action). |
| `reboot` | Starts the legacy `ServerDown` countdown. |
| `readguild` | Re-reads guild data. |
| `readguildname` | Re-builds the guild name table. |
| `readgameconfig` | Re-reads `gameconfig.txt` and refreshes the status snapshot. |
| `reloadmob` | Re-reads `NPCGener.txt`. |
| `status` | Prints the group/index/local address, user count, billing and server-down state plus the game configuration. |
| `quit` | Graceful shutdown. |

Commands are posted to the event loop, so they execute in submission order and
never interleave with a timer tick.

## Shutdown

`quit`, `Ctrl+C` and `SIGTERM` share one path: stop the listener, stop the
reconnect ladder, send the billing logout record, close the client/DB/billing
links, write guild data, flush and close the log files, then exit with success.
A requested `reboot` instead runs the legacy countdown and shuts down through
the second-tick handler once every user has been saved.

## Loopback verification

`tests/integration/tmsrv_smoke.ps1` builds a minimal runtime tree, runs the
real executable and verifies:

- a missing `localip.txt` exits non-zero with a diagnostic;
- a missing `serverlist.txt` exits non-zero with a diagnostic;
- the DB link connects to a stub listener and sends the four-byte init
  handshake;
- the game port accepts a loopback client with the init handshake;
- the `status`, `saveall` and `quit` console commands work and the process
  exits successfully.

Run it through CTest:

```powershell
ctest --preset x86-debug -R tmsrv-smoke --output-on-failure
```

Or directly:

```powershell
pwsh -NoProfile -File tests/integration/tmsrv_smoke.ps1 `
  -ExeDir build/x86-debug/Debug `
  -CommonDir build/Common
```

## Connection diagnostics

Every log line is flushed as it is written, so the dated file under `Log/`
shows what the running process is doing. A client connection produces:

| Log line | Meaning |
|---|---|
| `sys,game listener <ip>:8281 db <ip>:7514` | Listener and DB endpoint chosen at startup. |
| `sys,accept slot=N ip=<ip>` | TCP connection reached the game listener. |
| `sys,login account=<name> client=<version> size=<n>` | A decoded `_MSG_AccountLogin` frame. |
| `err,login rejected account=... client=... server=...` | Build/version mismatch; the client is told to rerun. |
| `err,client link N: init code mismatch` | The peer did not send the `INITCODE` handshake. |
| `err,db link: ...` / `err,db read fail` | The TMSrv -> DBSrv link failed; the two-attempt ladder runs. |

If a client shows "connecting" and **no `sys,accept` line appears**, the TCP
connection never reached the process. The listener binds the address in
`localip.txt`; a `127.0.0.1` value only accepts connections from the same
machine, so a remote client needs the machine's LAN address there (and in
`serverlist.txt`). If `sys,accept` appears without a `sys,login` line, the
handshake or the client build is the next thing to check.

`tests/tools/login_probe.cpp` (target `login_probe`) plays one real
`_MSG_AccountLogin` against a running server and prints every reply, which
separates a TMSrv/DBSrv problem from a client problem:

```powershell
.\login_probe.exe 127.0.0.1 8281 ADMIN <password>
```

