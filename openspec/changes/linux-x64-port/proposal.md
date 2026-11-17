# Proposal

## Why

The servers build only for Windows x86 and depend on a few MSVC-specific CRT facilities plus 32-bit `long` record layouts. That blocks native Linux x86_64 builds, tests and deployment even though the recent Win32/asio migration removed nearly all platform coupling from the project sources.

## What Changes

- **Build system**: add native Linux x86_64 configure/build/test presets with the vcpkg Linux triplet; keep the existing Windows x86 presets and toolchain behavior. Configure SHALL fail clearly on unsupported platforms and architectures.
- **ABI normalization**: replace bare `long` fields in layout-sensitive records with fixed-width integer types so the 12-byte header and the 816/536/1352/7800/840-byte records stay byte-identical on both Windows x86 and Linux x86_64. The on-disk and wire formats do not change.
- **Portability fixes**: remove the unconditional MSVC-only `<mbstring.h>` include, replace the two `fopen_s` calls, and implement executable-path discovery for Linux (`/proc/self/exe`). Convert the CP949 comments in `Code/ItemEffect.h` to valid UTF-8 without changing runtime data bytes.
- **Runtime paths**: construct log, day-log, chat-log and item-log paths with the platform path rules instead of hard-coded backslashes so the existing `./Log` and `./DayLog` layouts work on Linux.
- **Linux verification**: run the portable unit tests plus a POSIX integration smoke that starts DBSrv and TMSrv against a temporary runtime tree, accepts a loopback connection, and verifies shutdown through both the console command and SIGINT.
- **Documentation**: document Linux prerequisites, presets, runtime layout, signal behavior and the remaining Windows/Linux differences.
- **BREAKING — supported platforms**: the build contract changes from "Windows x86 only" to "Windows x86 plus Linux x86_64". Windows behavior, ports, file formats and wire format remain unchanged.

## Capabilities

### New Capabilities

None — the change extends the existing build, host and platform capabilities to a second supported target.

### Modified Capabilities

- `build/cmake-vcpkg`: adds the Linux x86_64 target, a platform-aware configure/build/test workflow, cross-platform test execution and fixed-width ABI assumptions that are no longer tied to 32-bit pointer width.
- `platform/win32-free-code`: adds portable CRT/source-encoding constraints and defines platform-appropriate path case rules while preserving the existing error and byte semantics.
- `host/headless-runtime`: adds POSIX terminal and signal integration so the headless lifecycle, console commands and graceful shutdown work on Linux.

## Impact

- **Code**: `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json` (only if a manifest change proves necessary), `Code/Basedef.cpp`, `Code/Basedef.h`, `Code/platform/Path.cpp`, `Code/DBSrv/CRanking.cpp`, `Code/DBSrv/Server.cpp`, `Code/TMSrv/Server.cpp`, `Code/ItemEffect.h`, `tests/` and the README/operation docs.
- **Build**: new Linux x86_64 presets and vcpkg triplet configuration; no new third-party dependency. Windows keeps the existing `x86-debug`/`x86-release` presets and tests.
- **Runtime**: Linux binaries gain native executable-path resolution, path construction and SIGINT/SIGTERM handling; Windows binaries keep their current behavior. Log and data directory names remain the same.
- **Compatibility**: wire format, ports, account/character/capsule record bytes and content-file formats remain unchanged.
- **Prerequisite**: this change modifies capabilities introduced by the completed `migrate-win32-to-asio` change. Archive that change before archiving this one so the capability specs exist in `openspec/specs/`.
- **Out of scope**: macOS, BSD, 32-bit Linux, Windows-on-ARM, container packaging, cross-compilation, service-manager units and CI hosting.
