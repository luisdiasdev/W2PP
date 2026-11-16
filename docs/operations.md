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
