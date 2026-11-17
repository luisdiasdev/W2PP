# Tasks

## 1. Build scaffolding (CMake + vcpkg)

- [x] 1.1 Add `vcpkg.json` with `asio` and `catch2`, run `vcpkg x-update-baseline --add-initial-baseline` to pin `builtin-baseline`, and verify `cmake --preset x86-debug` resolves both packages into `build/x86-debug/vcpkg_installed/x86-windows`.
- [x] 1.2 Add root `CMakeLists.txt` and `CMakePresets.json` (`x86-debug`, `x86-release`, build/test presets, `VCPKG_TARGET_TRIPLET=x86-windows`, dynamic CRT) and verify both configurations configure and build the current sources unchanged; link the transitional system libraries (`ws2_32`, `winmm`, `user32`, `gdi32`) until the Win32 code is removed.
- [x] 1.3 Transcribe the source and include lists from `Code/TMSrv/TMSrv.vcxproj` and `Code/DBSrv/DBSrv.vcxproj` into explicit CMake target sources (`w2pp_core`, `tmsrv`, `dbsrv`) and verify both executables link and launch to the existing failure path.
- [x] 1.4 Add compile-time ABI guards (`static_assert` for `sizeof(HEADER) == 12` and the account/character record sizes) in a shared header and verify the build fails when a guarded field width is temporarily changed.
- [x] 1.5 Add a CTest target with one placeholder test registered, and verify `ctest --preset x86-debug` runs it and returns success.
- [x] 1.6 Verify a Release build imports no debug CRT by running `dumpbin /dependents` on `tmsrv.exe` and confirming no `*D.dll` or `ucrtbased.dll` appears.
- [x] 1.7 Document the CMake/vcpkg configure, build and test commands in `README.md` and verify they succeed from a clean build directory.

## 2. Wire codec extraction and parity

- [x] 2.1 Create `Code/net/Protocol.h` with the 12-byte header, ports, buffer/frame limits, init code, keyword table and billing record size, and verify all existing references compile with no duplicated definitions.
- [x] 2.2 Implement `net/FrameCodec` (init-code gate, header parse/stamp, transform, checksum, frame extraction) with an explicit keyword parameter and no socket or global dependencies.
- [x] 2.3 Point `CPSock` at `FrameCodec` without changing its public behavior, then verify the existing servers still build and start against the existing runtime tree.
- [x] 2.4 Add codec tests for encode/decode round-trip, fixed-keyword golden vectors, checksum corruption, init mismatch, size-window edges, partial frames and multiple frames in one buffer; verify `ctest --preset x86-debug -R codec` passes.
- [x] 2.5 Add a legacy-reference test that independently reimplements the old transform/checksum and asserts byte equality with `FrameCodec` for every frame size from 12 to 8192 in representative steps; verify it passes.
- [x] 2.6 Document the wire format, handshake, limits and codec invariants in `docs/net-protocol.md` and verify every example frame in the document matches a test vector.
- [x] 2.7 Capture a scripted message sequence through the legacy transport before cutover, recording the keyword byte for each frame, and save the raw byte stream plus keywords as a test fixture; verify the fixture decodes identically with the legacy algorithm and `FrameCodec`.

## 3. Platform wrappers (files, time, paths, diagnostics)

- [x] 3.1 Implement `platform::File` (binary open/read/write/seek/size/close with short-read and short-write detection) and verify record round-trip, missing-file and seek tests pass.
- [x] 3.2 Implement `platform::RenameFile`, `DeleteFile` and `DirectoryIterator`, and verify tests distinguish not-found from access failure and enumerate the same file set as the legacy scan.
- [x] 3.3 Implement `platform::MonotonicMs` (uint32, steady clock) and `platform::LocalNow`, and verify wrap-safe difference arithmetic and local date-field tests pass.
- [x] 3.4 Implement `platform::SetWorkingDirectoryToExecutable` and verify a test executable launched from a different working directory resolves its sibling files.
- [x] 3.5 Implement `platform::ReportFatal`, `ReportWarning` and the status sink, replace every `MessageBox`/`MessageBoxA` call in `Code/` except the two shutdown-confirmation dialogs, and verify `Select-String -Path Code\*\*.cpp,Code\*\*.h -Pattern MessageBox` returns only those two confirmations.
- [x] 3.6 Convert persistence and import/export paths in `Basedef.cpp`, `DBSrv/CFileDB.cpp`, `DBSrv/CReadFiles.cpp`, `DBSrv/CRanking.cpp`, `TMSrv/ProcessSecMinTimer.cpp`, `TMSrv/imple.cpp`, `TMSrv/Server.cpp`, `TMSrv/CNPCGene.cpp`, `TMSrv/CCastleZakum.cpp`, `TMSrv/CWarTower.cpp` and `TMSrv/MobKilled.cpp` to the platform wrappers, and verify no `_open`, `_read`, `_write`, `_lseek`, `_filelength`, `MoveFile`, `DeleteFile`, `FindFirstFile`, `FindNextFile` remains in `Code/` outside `Code/platform/`.
- [x] 3.7 Replace `timeGetTime` and `GetLocalTime` call sites with the clock wrappers, and verify neither name remains in `Code/` outside `Code/platform/`; confirm log-name and calendar tests pass. Leave the two legacy reconnect `Sleep` calls for phases 4 and 5.
- [x] 3.8 Document the wrapper contracts (error model, byte semantics, path behavior) in `docs/platform-abstractions.md` and verify the documented examples correspond to passing tests.

## 4. Async session, host runtime and DBSrv cutover

- [x] 4.1 Implement `net::Session` (async read loop, fixed receive buffer with compaction, FIFO outbound queue with 128 KiB cap, one outstanding `async_write`, generation id, graceful drain and force-close) with loopback tests for round-trip, ordering, queue-full rejection and write failure.
- [x] 4.2 Implement `net::Acceptor` and `net::Connector` (IPv4 endpoint parsing, bind fallback ladder, init handshake, billing 196-byte mode) and verify accept/connect/refused/reconnect tests pass.
- [x] 4.3 Replace DBSrv's `getaddrinfo`/`gethostname`/`inet_ntop` local-address discovery with the asio resolver and verify `serverlist.txt` group/index selection against a loopback fixture.
- [x] 4.4 Implement `host::Runtime` (single `io_context`, 500 ms/12 s TMSrv and 1 s DBSrv timer registration, signal set, interactive-console detection, posted commands, status snapshot, graceful shutdown) and verify tick-ordering, command-ordering and shutdown tests pass.
- [x] 4.5 Implement the `net::Connection` facade (`SendOneMessage`/`AddMessage`/`SendMessageA`/`CloseSocket`) over `net::Session` with unit tests for queued, dropped and flushed-on-close messages; verify the tests pass without wiring it to a server yet.
- [x] 4.6 Cut DBSrv over to the headless runtime in one atomic change: convert `main()`/startup, switch `CUser` and all links to `net::Connection`, replace `TempUser` staging with `PendingConnection`, replace accept/read handlers and timers, and remove the window procedure, menus, GDI drawing and dialogs. Verify a headless run with a temporary runtime tree starts listening and exits non-zero when a required base-set file is missing.
- [x] 4.7 Verify DBSrv's game-server accept, admin accept, unknown-IP rejection, redirect-connect and local-address selection with loopback tests, and verify the tests pass.
- [x] 4.8 Document DBSrv's headless run steps and console commands in `docs/operations.md`, and verify the documented commands work against the test runtime tree.

## 5. TMSrv cutover

- [x] 5.1 Cut TMSrv over to the headless runtime in one atomic change: convert `main()`/startup, switch `CUser` and all links to `net::Connection`, replace client accept/read handling, replace the blocking DB retry with the two-attempt/200 ms timer ladder, add the billing session and 196-byte parser, convert timers, remove `GetUserFromSocket`, and remove the window procedure, menus, GDI drawing and dialogs. Verify a missing `localip.txt`/`serverlist.txt` exits non-zero with a diagnostic.
- [x] 5.2 Verify the DB link reconnect ladder: first connect fails and second succeeds, and both attempts fail leading to graceful shutdown; verify both tests pass.
- [x] 5.3 Verify billing outbound record length/content and disconnect/reset behavior with loopback tests, and verify the 196-byte parser handles split and coalesced reads.
- [x] 5.4 Verify client login/character-select loopback tests with slot reuse and generation checks, including a handler that closes the session mid-drain.
- [x] 5.5 Add an end-to-end loopback smoke test that connects a fixture client, completes the handshake and one game message, and verifies the decoded reply matches the expected frame.
- [x] 5.6 Verify no window/GDI/menu/`MessageBox`/`Sleep` symbols remain in `Code/TMSrv/`, and verify no `WM_*` or `WSA*` identifier remains in either server.
- [x] 5.7 Document TMSrv's headless run steps, console commands and shutdown behavior in `docs/operations.md`, and verify the commands against the smoke-test runtime tree.

## 6. Remove Win32 from shared code and headers

- [x] 6.1 Delete the legacy `Code/CPSock.{h,cpp}` now that both servers use `net::Connection`, update includes, and verify no `CPSock` symbol remains in `Code/`.
- [x] 6.2 Remove `<Windows.h>` and the other platform includes from `Basedef.h`, `Server.h` (both), `ProcessClientMessage.h`, `ProcessDBMessage.h`, `GetFunc.h` and all per-TU includes; replace `BOOL`/`TRUE`/`FALSE`/`UINT`/`LONG` with standard types and verify both servers compile.
- [x] 6.3 Delete `hWndMain`, `hMainMenu`, `hFont`, `hDC`, `hInst` and `MainWndProc` declarations/definitions, and verify no `HWND`/`HDC`/`HFONT`/`HMENU`/`HINSTANCE` remains in `Code/`.
- [x] 6.4 Add the dependency-check script that fails on Windows/Winsock headers and API symbols in `Code/` (excluding `Code/platform/`), register it with CTest, and verify it fails on an injected `<windows.h>` include and passes on the clean tree.
- [x] 6.5 Run the full Debug and Release build plus the whole test suite, and verify both servers start, accept a loopback connection and shut down gracefully.

## 7. Cleanup, documentation and final verification

- [ ] 7.1 Delete `W2PP Code Project.sln`, both `.vcxproj`/`.vcxproj.user`, `TMSrv.rc`, `DBSrv.rc`, `resource.h` (both), `icon1.ico` and `.aps`, and verify a clean configure/build from the repository root.
- [ ] 7.2 Add `build/`, `vcpkg_installed/` and `out/` to `.gitignore`, remove stale untracked build directories from the working tree, and verify `git status` shows only intended source changes.
- [ ] 7.3 Update `README.md` with the headless operation model, CMake/vcpkg workflow, dependency pinning, console commands and the intentional behavior deviations from design D12.
- [ ] 7.4 Perform a clean-checkout verification: configure, build Debug and Release, run `ctest`, confirm Release imports no debug CRT, start both servers with a test runtime tree, and shut them down with the console command and with an interrupt.
- [ ] 7.5 Replay the task 2.7 fixture and its recorded keywords through the new codec/transport and verify header, transform, checksum and payload match the captured legacy bytes byte-for-byte.
- [ ] 7.6 Record the intentional deviations (no window/menu, non-blocking reconnect, full-buffer close, checksum rejection, observable send errors, drain-before-close, monotonic epoch) in the change notes for reviewers and operators.

## Workflow follow-up

- Archive the change after implementation review and after the clean-checkout verification passes.
- Verify the archived specs reflect the final transport, host, platform and build behavior.
