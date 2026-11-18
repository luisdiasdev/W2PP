# Design

## Context

See `proposal.md` for motivation. Current constraints that shape the approach:

- **One shared transport class.** `CPSock` (`Code/CPSock.{h,cpp}`) carries game, DB, admin, billing and redirect traffic. It is embedded by value in `CUser`, allocates two 128 KiB buffers per instance in its constructor, and depends on `hWndMain`, `CurrentTime`, `LastSendTime` and a global `Log` function. `pUser[MAX_USER]` therefore allocates ~250 MiB during static initialization.
- **Event delivery is the window queue.** `WSAAsyncSelect` posts `WM_USER+n` messages; `MainWndProc` decodes `WSAGETSELECTEVENT` and calls `Receive`/`ReadMessage` or `accept`. Timers are `SetTimer` messages. There are no threads, locks, or queued work.
- **Recv/send are pull-style.** `Receive()` performs one `recv`; `ReadMessage()` drains frames; `SendOneMessage()` encodes into a 128 KiB send buffer and attempts one `send`. Partial sends are retried on later calls; `SEND` errors are swallowed.
- **DBSrv uses a staging socket.** `TempUser` accepts a socket, then the accept handler copies the OS handle and buffer positions into `pUser[User]` after an IP/slot decision.
- **Wire compatibility is hard.** The closed-source client speaks the 12-byte header, `INITCODE` handshake, 512-byte keyword table, per-byte transform, 8-bit additive checksum, 12–8192 byte frame window, and 196-byte billing records. The transport cannot change any of these.
- **Build is MSBuild/VS-only.** No package manifest, no CMake, no tests, no CI, and the Release configuration links a debug CRT.

Target decisions confirmed with the user: full headless (no Win32 in project code), CMake + vcpkg manifest, Win32 x86 only.

## Goals / Non-Goals

**Goals:**

- Replace all project-owned Winsock and Win32 usage with standalone asio plus standard C++ wrappers.
- Preserve byte-for-byte wire compatibility and the account/character disk formats.
- Preserve gameplay-visible timing (500 ms / 12 s ticks for TMSrv, 1 s for DBSrv) and message ordering.
- Keep the existing send-side call surface (`SendOneMessage`, `AddMessage`, `SendMessageA`, `CloseSocket`) so the ~130 outbound call sites and the 60 handlers do not need rewriting.
- Make the transport testable without a window, a clock, a real client or content files.
- Produce reproducible Debug/Release builds from a clean checkout with pinned dependencies.

**Non-Goals:**

- Fixing gameplay, economy, validation or persistence defects documented in `docs/reports/` (`BASE_CheckPacket`, checksum acceptance policy beyond the transport, level-based admin, plaintext passwords, atomic saves).
- Changing message layouts, `#pragma pack` regions, array limits or file formats.
- Adding x64, TLS, IPv6, IOCP tuning, thread pools or a Windows service wrapper.
- Improving the 500 ms/12 s interval interpretation, adding drift compensation, or fixing the "seconds are really 500 ms" convention.
- Adding a web UI; the operator surface is a text console.

## Decisions

### D1 — One `asio::io_context`, one world thread

Run a single `asio::io_context` on the main thread. All accept, connect, read, write, timer and shutdown handlers execute there, so `pUser`, `pMob`, `pItem` and the grids keep their existing no-lock invariant.

Alternatives considered:

- **Thread-per-connection**: rejected — every handler mutates shared globals; this would require locks around the entire world.
- **`io_context` thread pool**: rejected for the same reason; asio makes no ordering guarantee across threads, and the existing dispatch relies on serialized callbacks.
- **Keep a poll loop with `io_context.poll_one()` plus `Receive()`**: rejected — it preserves pull semantics but adds latency and keeps the transport coupled to a manual clock.

Console input is the only other thread: it reads lines and `asio::post`s commands to the event loop. It never touches world state. When stdin is not a terminal (service/non-interactive), the console thread is not started.

### D2 — Layered transport with a connection facade

New modules:

- `net/Protocol.h` — wire constants: header layout, ports, buffer/frame limits, init code, billing record size, `_AUTH_GAME`.
- `net/FrameCodec` — pure, stateful-only-in-buffer functions: init-code check, header parse/stamp, transform, checksum, frame extraction. No sockets, no globals except the keyword table. Fully unit-testable.
- `net/Session` — owns one `asio::ip::tcp::socket`, the receive buffer, the outbound queue, and a generation id. Performs `async_read_some`, frame extraction and `async_write`; invokes callbacks for "frame received", "closed", "write failed".
- `net/Acceptor` / `net/Connector` — listener and outbound link setup, endpoint parsing, bind-fallback ladder.
- `net::Connection` — thin session facade with `std::shared_ptr<net::Session>`:

  | Legacy method | New behavior |
  |---|---|
  | `SendOneMessage(msg, size)` | Encode with the codec, append to the session queue, start `async_write` if idle |
  | `AddMessage(msg, size)` | Same encode/append step without starting the write |
  | `SendMessageA()` | Start the write pump; returns whether the queue is non-empty |
  | `CloseSocket()` | Request graceful close: stop reads, flush the queue with a deadline, then close |
  | `Receive()`, `ReadMessage()`, `Refresh*Buffer()` | Removed in the converted server; its read loop is replaced by session callbacks |
  | `WSAInitialize()`, `StartListen()`, `Connect*()` | Removed; replaced by `net::Acceptor`/`Connector` |
  | `Sock`, `nSendPosition`, `nRecvPosition`, `nProcPosition`, `nSentPosition` | Removed; replaced by `Id()`, `IsOpen()`, and queue metrics |

The facade keeps the method names used at 129 `SendOneMessage` sites, so handlers and `SendFunc` do not change. It is held by `std::shared_ptr` inside `CUser`; `CUser` becomes non-copyable (`CUser(const CUser&) = delete`) to prevent shared-session aliasing. `pUser[]` is never copied today, so this is a compile-time guard rather than a behavioral change.

**Coexistence constraint:** TMSrv keeps the legacy `CPSock` and its window-message loop until phase 5, while DBSrv is converted in phase 4. Therefore the new facade is introduced as `net::Connection` and DBSrv's `CUser`/links use it first; TMSrv's `CUser` keeps `CPSock` until its own conversion. The legacy files are deleted only after both servers are converted (phase 6). No runtime switch and no dual transport inside one executable.

Alternatives considered: rewriting all call sites to `session.send(...)` — larger diff, no functional gain, and it would mix feature work into the migration. Keeping the `CPSock` name for the new facade in phase 4 — rejected because the old class is still compiled for TMSrv and would collide.

### D3 — Session lifetime: detach-and-drain

Legacy code frequently does `SendOneMessage(notice); CloseUser(conn);`, relying on the synchronous `send` having accepted bytes. Raising the same pattern on an async socket can lose the final frame if the socket closes immediately.

Design:

- `CloseUser` detaches the `shared_ptr<net::Session>` from the slot, resets the slot (`Mode = USER_EMPTY`, account cleared) immediately, and hands the session to a host-owned `draining` list. The slot can be reused at once.
- The draining session keeps reading disabled, flushes its queue with a 1 s deadline, then closes and is removed from the list.
- Fatal/protocol errors take a `ForceClose` path: cancel operations, close, discard the queue.
- Callbacks capture the slot index plus a generation number; if the generation no longer matches the slot's current session, the callback is ignored. This replaces `GetUserFromSocket` handle matching and prevents recycled-handle delivery.

Alternatives considered:

- Keep the slot occupied until flush completes: rejected — changes admission behavior and can strand slots on a slow client.
- Synchronous `socket.write_some` before close: rejected — blocks the event loop on a slow peer and can still accept only part of the data.

### D4 — Receive path: fixed buffer, compaction, explicit violation handling

Each `Session` allocates its receive buffer on attach (not in the `CUser` constructor) at the existing 128 KiB size, and frees it on close. This removes ~125 MiB of idle committed memory while preserving the existing maximum in-flight window.

Read loop:

1. `async_read_some` into `buffer[write_pos..]`.
2. `FrameCodec` consumes complete frames from `read_pos`; each frame is passed to the link callback while the handler runs.
3. Compact unprocessed bytes to offset 0 when `read_pos > 0` and space is needed.
4. If the buffer is full and no complete frame can be extracted, treat it as a protocol violation and force-close.

This fixes the legacy "full buffer is a receive failure" misclassification, which caused permanent stream desync. It is an intentional, documented deviation; the frame window (8192) makes it unreachable in normal traffic.

The frame buffer remains mutable because several handlers write terminators into received packets, as they do today.

### D5 — Outbound queue and ordering

Replace the 128 KiB send buffer with a per-session `std::deque<std::vector<uint8_t>>` of encoded frames plus a queued-byte counter.

- Submission order is preserved; only one `async_write` is outstanding.
- Queue limit is 128 KiB, matching legacy capacity. A frame that would exceed it is rejected and logged with the same "buffer full" diagnostic.
- Partial writes are completed by `asio::async_write` before the next frame starts.
- Write failure reports `TransportError` to the host, which applies the link-specific cleanup (client `CloseUser`, DB reconnect ladder, billing reset).

Alternative rejected: `asio::streambuf`/`async_write` chained buffers — workable, but a deque of frames makes accounting, drop policy and tests simpler.

### D6 — Timers: same cadence, no drift compensation

TMSrv keeps two `asio::steady_timer`s at 500 ms and 12,000 ms; DBSrv keeps one at 1,000 ms. Each handler re-arms after it returns (`expires_after` relative to now), so a slow tick delays the next one instead of overlapping. `CurrentTime` is refreshed at the same points as the legacy handlers.

Deliberately not corrected: the 500 ms counter being called a "second", the 12 s "minute", the inverted 120% reload and calendar-exact comparisons. Fixing them would change gameplay pacing and belongs in a separate change.

### D7 — Headless host

New `host/Runtime` owns the `io_context`, timers, acceptors, connectors, draining sessions, signal set and console. Each server gets a `main.cpp` that:

1. Resolves the executable directory and sets it as the working directory.
2. Initializes logging and diagnostics.
3. Runs the legacy startup sequence (config, content, base sets, listeners, DB/billing links) with error returns instead of `MessageBox`.
4. Starts the console thread when stdin is interactive.
5. Runs `io_context.run()`.
6. On shutdown: stop acceptors, perform the legacy shutdown writes, flush/close logs, exit with a defined code.

Startup code currently in `WinMain` moves into `TMSrvApp::Start()` / `DBSrvApp::Start()` in each server, preserving the order of `BASE_InitModuleDir`, log start, config reads, `Reboot`, DB connect, billing connect, listener start, and guild read.

The `WndProc` cases become link callbacks:

| Legacy message | Replacement |
|---|---|
| `WSA_ACCEPT` | `Acceptor::onAccept(socket, endpoint)` → slot selection → attach |
| `WSA_READ` | `Session::onFrame(slot, generation, frame)` → `ProcessClientMessage` |
| `WSA_READDB` | DB connector session callback → `ProcessDBMessage` |
| `WSA_READBILL` | billing session callback → 196-byte parser → `ProcessBILLMessage` |
| `WSA_ACCEPTADMIN` / `WSA_READADMIN` | admin acceptor/session → `ProcessAdminMessage` |
| `WSA_READADMINCLIENT` | redirect connector callback → `ProcessAdminClientMessage` |
| `WM_TIMER` | `onSecondTick` / `onMinuteTick` timer handlers |
| `WM_PAINT` / `WM_CREATE` / `WM_COMMAND` / `WM_CLOSE` / `WM_DESTROY` | removed; console commands and signal handling |

DBSrv's `TempUser` staging object disappears: the accept callback builds a `PendingConnection { tcp::socket, peer_address }`, applies the IP allow-list and slot rules, then moves the socket into the chosen slot's session or closes it.

### D8 — Operator console and signals

`host/Console` starts a stdin reader thread only when `_isatty(_fileno(stdin))` is true. Lines map to:

- `saveall`, `reboot`, `readguild`, `readguildname`, `readgameconfig`, `reloadmob`, `status`, `quit` (TMSrv)
- `readmobbase` (DBSrv's former `IDC_SHUTDOWNNP`), `status`, `quit`

Each command is posted to the event loop, so commands execute in submission order and never interleave with a tick. `reboot` sets the existing `ServerDown` countdown rather than restarting the process, matching current behavior. `quit` starts graceful shutdown.

`asio::signal_set` handles `SIGINT` and `SIGTERM` with the same shutdown path. A second signal can force `io_context.stop()` after a short grace period.

### D9 — Diagnostics and status without GDI

`Log`, `ChatLog`, `ItemLog`, `CrackLog` keep their signatures and file outputs. The `SetWindowText` call becomes `host::StatusSink::Publish(line)`, which updates an in-memory status snapshot and writes to the console (when attached).

`DrawConfig`/`TextOutWind`/`TextOutA` are deleted. Their data — server group/index, connected users, admin slots, guilds, billing state, uptime — moves to `host::StatusSnapshot` rendered by the `status` command and periodically into the log. The DB `DrawConfig`'s `cSock.Sock` column becomes the slot's open/closed state.

`MessageBox` sites split into two categories:

- **Boot/data-fatal** (missing base set, bad config, listener bind failure, missing local IP): `platform::ReportFatal(...)` → log + stderr + non-zero exit.
- **Data warnings** (long item/message strings, missing optional content): `platform::ReportWarning(...)` → log and continue, preserving the current "continue after dismiss" behavior.

### D10 — Platform wrappers (the non-network Win32 surface)

Introduce `Code/platform/`:

| Legacy usage | Wrapper / replacement | Notes |
|---|---|---|
| `_open/_read/_write/_lseek/_close/_filelength` | `platform::File` (`std::fstream` binary, explicit size/seek, short-read/write detection) | Preserve record bytes and offsets |
| `MoveFile`, `DeleteFile` | `platform::RenameFile`, `platform::DeleteFile` (`std::filesystem`) | Distinguish not-found vs failure |
| `FindFirstFile/FindNextFile/FindClose` | `platform::DirectoryIterator` (`std::filesystem::directory_iterator`) | Same file set; no new ordering guarantee |
| `GetModuleFileName`, `SetCurrentDirectory` | `platform::SetWorkingDirectoryToExecutable()` using `_pgmptr` (CRT) / `/proc/self/exe` | Keeps `../../Common` and `./account` layouts |
| `timeGetTime` | `platform::MonotonicMs()` over `std::chrono::steady_clock`, truncated to `uint32_t` | Same ms unit and wrap width; absolute epoch is opaque |
| `GetLocalTime` | `platform::LocalNow()` over `system_clock` + `localtime_s` | Log names and calendar scheduling |
| `Sleep` | scheduled `steady_timer` in runtime paths; `std::this_thread::sleep_for` only on the console thread | No sleeps on the event loop |
| `MessageBox` | `platform::ReportFatal` / `ReportWarning` | Non-blocking |
| `_mbschr` | kept as CRT (`<mbstring.h>`) initially | MBCS trail-byte semantics; moving to UTF-8 is a separate change |
| `getaddrinfo`, `gethostname`, `inet_ntop` (DBSrv) | `asio::ip::host_name()` + `tcp::resolver` | Enumerate local IPv4 addresses to match `serverlist.txt` |

`Basedef.h` drops `<Windows.h>` (it only needs `time.h`). `BOOL`/`TRUE`/`FALSE` (536 occurrences), plus `UINT`/`LONG`/`HANDLE`/`SOCKET` in the remaining host code, are replaced with standard types in a mechanical pass gated by compilation. `HWND`, `HMENU`, `HFONT`, `HDC`, `HINSTANCE` globals and `LONG APIENTRY MainWndProc(...)` declarations are deleted.

RNG: production keeps `rand()` (unseeded) for keyword selection to preserve the existing deterministic sequence; the codec accepts an explicit keyword so tests are independent of RNG state.

### D11 — Build: CMake + vcpkg manifest

- `vcpkg.json`: `"name": "w2pp"`, `"version-string"`, `"builtin-baseline": "<pinned commit>"`, dependencies `asio` and `catch2`. The vcpkg `asio` port is header-only and forced standalone (`ASIO_STANDALONE`), exposing `asio::asio`. No Boost.
- `CMakePresets.json`:
  - `x86-debug` / `x86-release`, generator `Visual Studio 17 2022` (or the installed VS 2026 generator), `architecture: Win32`.
  - `VCPKG_TARGET_TRIPLET=x86-windows` (dynamic CRT), `CMAKE_TOOLCHAIN_FILE=$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`, binary dir `build/x86-<config>`.
  - `test` presets run `ctest --output-on-failure`.
- `CMakeLists.txt` (root): C++20, `MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`, `_MBCS`, `_CRT_SECURE_NO_WARNINGS` (retained to avoid a warning avalanche during migration), `_PACKET_DEBUG` only in Debug. No `/utf-8` yet: source/data literals are CP949/ANSI and changing interpretation would alter message text.
- Targets:
  - `w2pp_core` (static): `Basedef.cpp`, `net/`, `platform/`, shared codec.
  - `tmsrv` (executable): TMSrv sources + `main.cpp` + host app.
  - `dbsrv` (executable): DBSrv sources + `main.cpp` + host app.
  - `w2pp_tests` (executable, CTest): `tests/codec`, `tests/loopback`, `tests/platform`.
- Explicit source lists copied from the two `.vcxproj` files; no globs.
- `static_assert(sizeof(HEADER) == 12)` and `static_assert` on account/character record sizes to guard ABI.
- Remove `.sln`, both `.vcxproj`, `.vcxproj.user`, `TMSrv.rc`, `DBSrv.rc`, `resource.h` (both), `icon1.ico`, `.aps`, and committed build intermediates; add `build/`, `vcpkg_installed/`, `out/` to `.gitignore`.
- README build section rewritten; document `cmake --preset`, build, ctest, and the console commands.

Rationale for `x86-windows`: matches the current 32-bit ABI and keeps the release CRT (`/MD`), fixing the debug-CRT defect. Static CRT (`x86-windows-static`) remains an option without source changes.

### D12 — Behavior deviations (explicit)

These are intentional and will be recorded in the change's release notes:

1. **No modal dialogs**: fatal boot errors exit non-zero; warnings log and continue.
2. **No window or menu**: operator actions are console commands/signals.
3. **Non-blocking reconnects**: the 200 ms DB retry spacing is preserved, but the event loop is no longer stalled.
4. **Full-buffer receive** is treated as a protocol violation/close instead of a retryable failure.
5. **Checksum mismatch is rejected and closed** instead of delivered with an error flag. (Legacy dispatch already suppressed the frame in every server caller; this removes the footgun.)
6. **Send errors are observable**: queue-full and write failures are reported rather than silently returning success.
7. **Final frames flush before close**, up to a 1 s deadline.
8. **`CurrentTime` epoch** becomes monotonic-since-boot rather than exactly `timeGetTime`; all comparisons are difference-based, and `ClientTick` is treated as opaque.
9. **Idle memory drops** because receive buffers are allocated on accept, and the per-session send buffer is replaced by a frame queue.

## Risks / Trade-offs

- **Wire regression breaks every link** → Extract the codec behind a pure interface first; add golden vectors generated from the legacy algorithm with fixed keywords; add a loopback client/server test; before TMSrv cutover, run the new DBSrv against the legacy TMSrv and compare frame logs.
- **Close/use-after-free during callback** → `shared_ptr<Session>` + generation tokens + detach-and-drain; never call into a slot after `CloseUser` resets it; tests that close from inside a frame handler.
- **Ordering loss with async writes** → single outstanding `async_write`, FIFO deque, explicit queue accounting; test interleaved sends from a handler.
- **Reentrancy changes semantics** → a read callback may dispatch a handler that closes the session; after each frame, stop draining and re-check the generation before continuing.
- **Performance regression** → asio adds per-operation overhead; keep one read per callback and avoid allocations per frame (reuse the receive buffer, move encoded frames). Add a throughput smoke test (10k frames) to the loopback suite.
- **Memory** → lazy 128 KiB buffers reduce idle memory but connect bursts write 128 KiB per session; on Win32 the total remains bounded by `MAX_USER`. Document the new allocation point.
- **File/enumeration semantics drift** → `std::filesystem` shares Windows semantics but directory order is not guaranteed; callers must not depend on order beyond the file set. Tests assert sets.
- **ANSI/CP949 literals** → CMake preserves the default code page; no `/utf-8`. Console output remains byte-preserving. UTF-8 migration is explicitly out of scope.
- **vcpkg/network/toolchain availability** → pin baseline, document `VCPKG_ROOT`, use a binary cache in CI (when added), and fail configure with a clear message if the manifest cannot resolve.
- **Scope creep into gameplay fixes** → the migration preserves existing rules and defects; deviations are limited to D12 and reviewed separately.
- **Losing the prebuilt binary reference** → keep `Code/release/TMSrv.exe` untouched during the work (it is untracked); do not overwrite it until a new build is verified.

## Migration Plan

Phases are independently reviewable and leave the tree buildable at each gate.

1. **Baseline and scaffolding**
   - Record the legacy build commands and startup behavior; keep the existing binary for comparison.
   - Add CMake + vcpkg manifest and build the *current* sources (still Win32) through CMake; add CTest wiring with a placeholder test.
   - Gate: Debug and Release build from a clean configure; Release imports no debug CRT.

2. **Codec extraction and parity tests**
   - Move header/transform/checksum/init logic out of `CPSock` into `net/FrameCodec` with no behavior change; keep `CPSock` calling it.
   - Add golden-frame, round-trip, corruption, size-window and partial-frame tests.
   - Gate: legacy build still passes; codec tests pass.

3. **Platform wrappers**
   - Add `platform::File`, filesystem, clock, path and diagnostics wrappers.
   - Convert `Basedef.cpp`, `CFileDB.cpp`, `CReadFiles.cpp` (both), `CRanking.cpp`, `ProcessSecMinTimer.cpp`, `imple.cpp`, `Server.cpp` persistence/import paths, `CNPCGene.cpp`, `CCastleZakum.cpp`, `CWarTower.cpp`, `MobKilled.cpp`.
   - Replace `MessageBox` sites with fatal/warning diagnostics, except the shutdown-confirmation dialogs that disappear with the window procedures in phases 4 and 5.
   - Gate: no `MessageBox` (other than the two shutdown confirmations), `_open`, `MoveFile`, `FindFirstFile`, `timeGetTime` or `GetLocalTime` in project sources except the new wrappers; `Sleep` remains only in the two legacy reconnect paths scheduled for phases 4 and 5.

4. **Host runtime and DBSrv cutover**
   - Implement `host/Runtime` (io_context, timers, signals, console, status, shutdown).
   - Implement the `net::Connection` facade over `net::Session`.
   - Cut DBSrv over atomically: `main()` startup, `CUser`/links switched to `net::Connection`, accept callbacks with `PendingConnection`, admin/game/redirect session callbacks, timer handler, removal of the window procedure.
   - Gate: DBSrv builds headless; loopback test passes; a TMSrv (legacy or migrated) can connect, log in and exchange messages.

5. **TMSrv cutover**
   - Cut TMSrv over atomically: startup, `WndProc` cases → callbacks, `CUser`/links switched from `CPSock` to `net::Connection`, DB reconnect ladder, billing session, timers, slot attachment, and removal of `GetUserFromSocket`.
   - Gate: client login/character select/play smoke test; DB link reconnect test; billing link connects and sends.

6. **Remove Win32 from shared code and headers**
   - Delete the legacy `Code/CPSock.{h,cpp}` and its Winsock includes once both servers use `net::Connection`.
   - Drop `<Windows.h>` from `Basedef.h`, `Server.h` (both), `ProcessClientMessage.h`, `ProcessDBMessage.h`, `GetFunc.h`, and the per-TU includes.
   - Replace `BOOL`/`TRUE`/`FALSE`/`UINT`/`LONG`/`HANDLE`/`SOCKET`; delete window/menu/font/DC globals and `MainWndProc` declarations.
   - Add the dependency check that rejects Windows headers/symbols in `Code/`.
   - Gate: dependency check passes; both servers build and pass tests.

7. **Cleanup and documentation**
   - Delete MSBuild projects, resources, icons, resource caches and stale build intermediates.
   - Update README with build/test/run and console-command documentation.
   - Gate: clean-checkout build from documented commands; `ctest` green; both servers start headless and shut down gracefully.

**Rollback:** work is phased and each phase is a separate commit series on a branch. The legacy transport remains compiled until phase 5 passes; after phase 6 this change is rolled back by reverting the branch, not by a runtime switch. No dual-stack runtime flag is shipped.

## Open Questions

- Which VS generator to pin (`Visual Studio 17 2022` vs the installed VS 2026 generator) — resolved when the presets are written; both accept `-A Win32`.
- Exact `builtin-baseline` commit and asio/catch2 versions — pinned from the local vcpkg checkout during implementation (`vcpkg x-update-baseline`).
- Whether to keep a minimal `--status-file` output for external monitoring — deferrable; the `status` command and periodic log line cover the current need.
- Whether to replace `_mbschr` with a byte-wise search or move the data plane to UTF-8 — deferred to a separate encoding change.
