# Proposal

## Why

Both servers are still bound to a 1990s Win32 host: networking runs on Winsock 1.1 through `WSAAsyncSelect`, the game loop is a GUI window procedure, timers are `SetTimer`, failures block the whole process in `MessageBox`, and file/time/logging code calls the Win32 API directly. The current audit documents the consequences: a single transport class is the only network path for three protocols, the transport cannot exist without a window handle, there is no headless or service mode, no test seam, no dependency manifest, and the committed Release configuration cannot reproduce the shipped binary.

Migrating to standalone asio (through vcpkg) replaces the hand-rolled async-select reactor with a portable, testable asynchronous I/O model, removes the GUI and other Win32 dependencies from project code, and makes the build reproducible. This is the prerequisite for tests, CI, and any future cross-platform work, and it is cheaper to do before more gameplay fixes are layered onto the current host.

## What Changes

- **Transport**: replace `CPSock`/Winsock with a standalone-asio transport. Game (8281), DB (7514), admin (8895), billing and redirect links all run on `asio::ip::tcp` with asynchronous accept, connect, read and write operations on one `asio::io_context`.
- **Wire compatibility**: preserve the existing 12-byte `HEADER`, `INITCODE` handshake, 512-byte keyword table, byte transform, 8-bit additive checksum, 8192-byte frame window, 128 KiB queue limits, and the 196-byte unframed billing records exactly. Existing clients and DBSrv/TMSrv peers must not need changes.
- **Host**: replace `WinMain`, `WNDCLASS`, `CreateWindow`, `GetMessage`/`DispatchMessage`, `WndProc`, `WSAAsyncSelect` and `SetTimer` with a headless `main()` that runs an asio event loop with `steady_timer` ticks at the existing 500 ms and 12 s intervals.
- **BREAKING — operator interface**: the status window, GDI `TextOut` painting, menus (`Reboot`, `SaveAll`, `ReadGuild`, `ReadGameConfig`, `ReloadMob`) and modal dialogs are removed. Status/logs go to console and log files; the same actions are available as console commands and via graceful SIGINT/SIGTERM shutdown. No interactive desktop is required.
- **BREAKING — build system**: `.sln`/`.vcxproj` MSBuild projects are replaced by CMake with a `vcpkg.json` manifest and a pinned `builtin-baseline`; `asio` is the first managed dependency, with a test-only framework added for transport/codec tests. Release builds use the release CRT (`/MD`), fixing the debug-CRT-in-Release defect.
- **Platform APIs**: remove `#include <Windows.h>`, `winsock*.h`, `windowsx.h`, `mmsystem.h` and all direct Win32 calls from project sources (`Code/`). Replace `_open/_read/_write/_lseek/_filelength`, `MoveFile`, `DeleteFile`, `FindFirstFile/FindNextFile`, `GetModuleFileName`, `SetCurrentDirectory`, `GetLocalTime`, `timeGetTime`, `Sleep` and `MessageBox` with standard C++/CRT equivalents behind small platform wrappers.
- **Diagnostics**: `Log`, `ChatLog`, `ItemLog` and `CrackLog` keep their call signatures and file outputs, but the status-line sink (`SetWindowText`) becomes a console/file status sink; boot aborts log and exit non-zero instead of showing a modal dialog.
- **Error handling**: integrity failures, receive-buffer desync and send failures are no longer ignored. Corrupted frames are dropped and the connection is closed, queue overflow is rejected and logged as before, write failures reach the link owner, and a closing connection flushes queued frames for up to one second before the socket closes.
- **Tests**: add a CTest target with golden-frame tests for the codec (handshake, framing, checksum, obfuscation round-trip, size window) and a loopback integration test that exercises accept/connect/read/write on the new transport.

Out of scope: changing the game protocol or account/character file formats; fixing the audit's gameplay/security defects (disabled `BASE_CheckPacket`, level-based admin, plaintext credentials, persistence atomicity); adding x64 support; adding TLS; containerization, CI hosting, or service-installer work.

## Capabilities

### New Capabilities

- `transport/asio-transport`: asynchronous TCP transport for game, DB, admin, billing and redirect links, including frame codec parity, connection lifecycle, queue limits and error reporting.
- `host/headless-runtime`: headless process entry, single-threaded event loop, periodic tick scheduling, graceful shutdown/signals and the operator console that replaces the Win32 menu.
- `platform/win32-free-code`: project sources contain no direct Win32/Winsock usage; filesystem, time, path resolution and diagnostics use portable wrappers with defined failure behavior.
- `build/cmake-vcpkg`: reproducible CMake + vcpkg manifest build for Win32 x86 with pinned dependencies, consistent CRT linkage, and test execution through CTest.

### Modified Capabilities

None. No capability specs exist in `openspec/specs/` yet; this change introduces the initial set.

## Impact

- **Code**: `Code/CPSock.{h,cpp}` replaced; `Code/TMSrv/Server.cpp` and `Code/DBSrv/Server.cpp` lose their Win32 host and window procedure (~700 lines of host/UI code replaced); `Basedef.{h,cpp}`, `TMSrv/Server.h`, `DBSrv/Server.h`, `CUser.{h,cpp}` (both), `CReadFiles.cpp` (both), `CFileDB.cpp`, `CRanking.cpp`, `ProcessSecMinTimer.cpp`, `imple.cpp`, `CNPCGene.cpp`, `CCastleZakum.cpp`, `CWarTower.cpp`, `MobKilled.cpp` and the `_MSG_*` translation units lose transitive `Windows.h`/`io.h` dependencies and direct Win32 calls. Message dispatch and handler signatures are unchanged.
- **Headers/contracts**: `Basedef.h` stops including `Windows.h`; `HWND`/`HMENU`/`HFONT`/`HDC` globals disappear; `CPSock::Sock` handle-based lookup is replaced by slot/session identity. Protocol structs and `#pragma pack` regions are untouched.
- **Build/dependencies**: new `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `vcpkg-configuration.json` (or pinned baseline), `tests/`; removed `W2PP Code Project.sln`, `Code/*/*.vcxproj`, `.vcxproj.user`, `.rc`, `resource.h`, `icon1.ico`, and `.aps`. New third-party dependency: standalone asio (header-only) plus a test framework.
- **Runtime/deployment**: executables become console subsystems; no Win32 resources or icon; runtime directory layout (`../../Common`, `./account`, `./char`, `./Log`) and ports are unchanged; Release now requires only the release VC++ runtime (or none if static CRT is chosen later).
- **Operations**: desktop operators lose the window/menu and must use console commands or signals; startup failures are logged instead of modal; DB reconnect and billing retry no longer block the event loop.
- **Risk**: the transport is shared by every link; a framing regression breaks all connectivity. Mitigation is golden-frame parity tests, a loopback harness, and migrating DBSrv before TMSrv with side-by-side packet capture comparison.
