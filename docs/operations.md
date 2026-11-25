# DBSrv operations (headless)

DBSrv is a console application. It opens no window and needs no desktop
session on Windows or on Linux. The listener ports are unchanged: game-server
`7514` and NP admin `8895`. On Linux the binary is `dbsrv`; the Windows name is
`dbsrv.exe`.

## Credential hashing policy

DBSrv stores only BCrypt hashes of account passwords and numeric secure
tokens. Hashing is confined to DBSrv; TMSrv forwards the submitted bytes
unchanged. New hashes use the `$2b$` format with a fresh per-credential salt,
and verification accepts `$2a$`/`$2b$`. A successful verification upgrades a
hash whose cost is below the current policy inside the same transaction.

Configuration (all values can be overridden by the listed environment
variable):

| Key in `Config.txt` | Environment override | Default | Meaning |
| --- | --- | --- | --- |
| `BcryptCost` | `W2PP_BCRYPT_COST` | `12` | BCrypt log2 cost; accepted range 10-14. A value outside the range makes DBSrv refuse to start. |
| `AuthMaxFailures` | `W2PP_AUTH_MAX_FAILURES` | `5` | Consecutive failed password/token verifications before lockout. |
| `AuthLockoutSeconds` | `W2PP_AUTH_LOCKOUT_SECONDS` | `900` | Lockout window; durable in the database across restarts. |

Failed and locked verifications append a row to `credential_events` with the
account id, credential kind and timestamp. No password, token, hash or
submitted value is written to that table, the logs, exports or admin replies.

The numeric secure token is only 4-6 digits, so its hash is guessable offline
if the database leaks even at cost 12. BCrypt and the per-account lockout make
online guessing impractical, but operators should prefer 6-digit tokens, keep
database access restricted and treat the cost factor as a floor. A
server-side pepper remains a possible follow-up.

## PostgreSQL storage

DBSrv uses PostgreSQL 19 as the only writable store for accounts, characters,
cargo, affects and soul capsules; it never reads or writes `./account`,
`./char` or `./capsule` while serving game or admin traffic. A running
PostgreSQL 19 instance, the schema and a completed import are startup
prerequisites.

`Config.txt` keeps `Sapphire` on the first line and adds `Key Value` lines.
Every key has an environment override:

| Key | Environment | Default | Meaning |
| --- | --- | --- | --- |
| `PgHost` | `W2PP_PG_HOST` | `127.0.0.1` | Server host. |
| `PgPort` | `W2PP_PG_PORT` | `5432` | Server port. |
| `PgDatabase` | `W2PP_PG_DATABASE` | `w2pp` | Database name. |
| `PgUser` | `W2PP_PG_USER` | `w2pp` | Runtime DML role. |
| `PgSslMode` | `W2PP_PG_SSLMODE` | `prefer` | libpq `sslmode`. |
| `PgConnectTimeout` | `W2PP_PG_CONNECT_TIMEOUT` | `10` | Connection timeout, seconds. |
| `PgStatementTimeoutMs` | `W2PP_PG_STATEMENT_TIMEOUT_MS` | `5000` | Per-statement timeout. |
| `PgLockTimeoutMs` | `W2PP_PG_LOCK_TIMEOUT_MS` | `2000` | Lock wait timeout. |

The database password is never written to `Config.txt`. Set
`W2PP_PG_PASSWORD`, `PGPASSWORD`, or use a libpq password file.

At startup DBSrv connects, verifies the schema contract, applies pending
migrations (advisory-locked), requires `store_metadata.import_state =
'complete'`, and only then starts listening. An unreachable database, rejected
credentials, a newer schema contract, a failed migration or a pending import
each exit non-zero with a message naming the problem; there is no fallback to
the legacy files.

Operator-visible admin changes: the NP account information reply carries an
empty password field, and the reported state is the explicit account state
(0 normal, 1 blocked, 2 deleted, 3 disabled) with a dated block also reported
as 1. Saving from the admin tool cannot overwrite a stored hash.

## Legacy cutover runbook

1. Provision PostgreSQL 19, a database and a runtime DML role. Put the
   password in `W2PP_PG_PASSWORD` and the connection keys in `Config.txt`.
2. Stop the old DBSrv and keep a copy of `./account`, `./char`, `./capsule`
   and `Config.txt`. The importer only reads the trees.
3. Dry run and review rejects:
   `dbimport --source <legacy root> --dry-run --report import-dry.txt`
   then run `dbimport --source <legacy root> --report import.txt`.
   A successful run reports `result=complete`, sets `import_state = complete`
   and seeds the capsule sequence above the imported maximum.
4. Start the new DBSrv. It refuses to start if `import_state` is not complete.
5. Smoke test with `db_login_probe 127.0.0.1 7514 <account> <password>` (exit 0
   admitted, 2 credential/state refusal, 3 connect failure), then check save,
   capsule put-in/take-out and the ranking report.
6. Cut over: keep the legacy tree untouched and let the new DBSrv be the only
   writer. Archive the tree after the observation window.
7. Rollback before cutover: stop the new DBSrv and start the previous binary
   (the files were not modified). After cutover: run
   `dbimport --legacy-export <restore dir>` and start the previous binary on
   that tree; credentials are intentionally absent from the export, so reset
   affected passwords through the admin flow if rollback is required.

## Runtime tree

The process sets its working directory to the executable's directory. The
existing layout is preserved:

```
<exe dir>/
  dbsrv.exe            # dbsrv on Linux
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

Paths use the platform separator. Linux uses forward slashes and case-sensitive
file names; Windows keeps its case-insensitive lookup. Log, day-log, chat-log
and item-log files are created under `./Log`, `./DayLog`, `./chat_log` and
`./item_log` on both platforms.

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

```bash
cd <exe dir>
./dbsrv
```

The process runs until a shutdown command or signal. Startup diagnostics and
warnings go to the log and to standard error.

## Console commands

The console thread is started only when stdin is a terminal. Set
`W2PP_FORCE_CONSOLE=1` on Windows or Linux to feed commands through a pipe or
FIFO in scripted runs.

| Command | Action |
|---|---|
| `status` | Prints the current group, local IP, game-server slots/open state and admin slots. |
| `readmobbase` | Reloads the four base character files (the old menu action). |
| `quit` | Graceful shutdown. |

The former window menu has no replacement for window painting; status is text.

## Shutdown and exit codes

`quit`, `Ctrl+C`, `SIGINT` and `SIGTERM` use the same path: stop the listeners,
close the game/admin/redirect links, write `Config.txt`, guild data and the
daily logs, flush and close the log file, then exit with status `0`. A boot
failure (missing base set, missing `Config.txt`/`serverlist.txt`, listener bind
failure) logs a diagnostic and exits non-zero. Signals are handled through
asio; no window or desktop session is involved.

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
session on Windows or on Linux. The game listener port is unchanged (`8281`),
the DB link connects to `7514` and the optional billing link uses the address
in `biserver.txt`. On Linux the binary is `tmsrv`; the Windows name is
`tmsrv.exe`.

## Runtime tree

The process sets its working directory to the executable's directory, so the
existing layout is preserved:

```
<exe dir>/
  tmsrv.exe             # tmsrv on Linux
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

```bash
cd <exe dir>
./tmsrv
```

Startup order is preserved from the former `WinMain`: configuration, content
and base-set reads, `Reboot`, local address/server group selection, DB link,
billing link, guild report and the game listener. The DB connect is
asynchronous: the first attempt runs immediately, the second one 200 ms later
when the first fails, and the event loop keeps running between them. If both
fail the process performs a graceful shutdown and exits non-zero during boot.

## Console commands

The console thread is started only when stdin is a terminal. Set
`W2PP_FORCE_CONSOLE=1` on Windows or Linux to feed commands through a pipe or
FIFO.

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

## Shutdown and exit codes

`quit`, `Ctrl+C`, `SIGINT` and `SIGTERM` share one path: stop the listener,
stop the reconnect ladder, send the billing logout record, close the
client/DB/billing links, write guild data, flush and close the log files, then
exit with status `0`. A boot failure (missing `localip.txt`/`serverlist.txt`,
listener bind failure, both DB connect attempts failed) logs a diagnostic and
exits non-zero. A requested `reboot` instead runs the legacy countdown and
shuts down through the second-tick handler once every user has been saved.

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

### POSIX smoke test

On Linux, `tests/integration/posix_smoke.sh` builds a temporary runtime tree,
starts the real `dbsrv` and `tmsrv`, verifies the `8281` listener accepts the
init handshake, sends `quit` through a console FIFO, and starts a second TMSrv
that is shut down with `SIGINT`. Both runs must exit with status `0`; every
wait is timeout-bounded and the temporary tree is removed on exit. It is
registered as the `posix-smoke` test:

```bash
ctest --preset linux-x86-debug -R posix-smoke --output-on-failure
```

The static source check (`platform-dependency-check`) also runs on Linux and
rejects Win32 or MSVC-only headers/functions outside `Code/platform/`.

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

