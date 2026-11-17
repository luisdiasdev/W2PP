# Release notes — migrate-win32-to-asio

Reviewer and operator notes for the Win32/Winsock-to-asio migration. These
notes accompany the change and record the intentional behavior deviations from
design decision D12.

## Compatibility

- The wire format is unchanged: 12-byte `HEADER`, `INITCODE` handshake,
  512-byte keyword table, per-byte transform, 8-bit additive checksum, the
  12–8192 byte frame window, the 128 KiB queue limit and the unframed 196-byte
  billing records.
- Ports and runtime tree layout are unchanged: TMSrv game `8281`, DBSrv
  game-server `7514`, NP admin `8895`, TMSrv -> DBSrv link `7514`, and the
  optional billing address from `biserver.txt`.
- Account, character and content file formats are unchanged.

## Breaking changes for operators

- **Console, not a window.** Both servers are console applications. The status
  window, menus, GDI painting and modal dialogs are gone. Operator actions are
  console commands (`status`, `quit`, `saveall`, `reboot`, `readguild`,
  `readguildname`, `readgameconfig`, `reloadmob`, `readmobbase`) and
  `Ctrl+C`/`SIGTERM`. See `docs/operations.md`.
- **Build system.** The MSBuild solution, project files, resources and icon are
  removed. Configure and build with the CMake presets and the pinned vcpkg
  manifest (`vcpkg.json`, `builtin-baseline`), as documented in `README.md`.
- **Boot failures.** A fatal startup error is written to the log and standard
  error and the process exits non-zero. Nothing waits for an operator to
  dismiss a dialog.
- **Console input.** The console reader starts only when stdin is a terminal.
  Scripted runs set `W2PP_FORCE_CONSOLE=1` to pipe commands.

## Intentional behavior deviations (D12)

1. **No modal dialogs.** Fatal boot errors (`ReportFatal`) log, write to stderr
   and exit non-zero. Data warnings (`ReportWarning`) log and continue, which
   preserves the legacy "continue after dismiss" behavior without blocking.
2. **No window or menu.** Status and diagnostics are text. The former menu
   actions are console commands and the former painting is a status snapshot.
3. **Non-blocking reconnects.** The DB retry ladder keeps the legacy 200 ms
   spacing and two-attempt limit, but the event loop keeps servicing timers,
   reads and console commands between attempts instead of stalling.
4. **Full-buffer receive is a protocol violation.** When the receive buffer
   holds no complete frame and cannot accept more bytes, the session is
   force-closed. The legacy code reported a retryable receive failure and could
   leave the stream permanently desynchronized. The 8192-byte frame window
   makes this unreachable in normal traffic.
5. **Checksum mismatch is rejected.** A frame whose computed checksum differs
   from the header checksum is dropped and the connection is closed. Legacy
   dispatch already suppressed such frames in every server caller; the new
   transport removes the "delivered with an error flag" footgun.
6. **Send errors are observable.** Queue overflow and write failures are
   logged and reported to the link owner (client close, DB reconnect, billing
   reset) instead of returning success silently.
7. **Final frames flush before close.** `CloseSocket` stops reading, writes the
   queued frames for up to one second, then closes. This preserves
   `SendOneMessage(...); CloseUser(...);` notice patterns without blocking the
   event loop.
8. **`CurrentTime` epoch is monotonic since boot.** It is derived from
   `steady_clock` and truncated to `uint32_t`, not an exact `timeGetTime`
   value. All comparisons are difference-based and `ClientTick` is opaque.
9. **Lower idle memory.** Receive buffers are allocated per accepted session
   instead of per `CUser`, and the per-session 128 KiB send buffer is replaced
   by a frame queue. Idle memory no longer scales with `MAX_USER` slots.

Behavior that is deliberately **not** changed: the 500 ms tick called a
"second", the 12 s tick called a "minute", the inverted 120% reload, the
calendar-exact scheduling, gameplay/economy rules, packet validation, admin
authorization and the plaintext on-disk formats. Those belong to separate
changes.

## Verification checklist

Verified by task 7.4 of the change in a clean copy of the working tree:

- [x] Clean configure/build from the documented presets (Debug and Release)
- [x] `ctest` passes for both configurations (46/46)
- [x] Release executables import no debug CRT (`MSVCP140.dll`,
      `VCRUNTIME140.dll`, `api-ms-win-crt-*` only)
- [x] Both servers start from a test runtime tree (`dbsrv-loopback`,
      `tmsrv-smoke`)
- [x] Graceful shutdown through the console `quit` command (both integration
      scripts)
- [x] Graceful shutdown through the SIGINT path (`runtime handles a shutdown
      signal`); the console maps Ctrl+C to this SIGINT handler. The
      non-interactive verification sandbox cannot inject a console Ctrl+C into
      a child console, so this path is exercised by raising SIGINT through the
      same asio signal set.

## Rollback

The migration is shipped as a phased commit series. Revert the branch to return
to the MSBuild/Win32 code; no dual-stack runtime switch is shipped.
