## Why

The servers and their tests still build only on Windows x86, so Linux hosts cannot compile the project even though the Win32/asio migration already removed direct Win32 and Winsock calls from project sources. Building the same **32-bit x86** environment on Linux preserves the existing `long`, pointer and record layouts, avoiding the fixed-width record changes an x64 migration would require. An investigation of all 95 server/core translation units and 10 test translation units found the complete, bounded set of remaining blockers.

## What Changes

- **Build system**: add Linux i386 (32-bit) configure/build/test presets using the vcpkg `x86-linux` triplet and `-m32`; keep the existing Windows `x86-debug`/`x86-release` presets unchanged. Configure SHALL reject non-i386 Linux and non-Win32 Windows targets with a clear message.
- **ABI preservation on Linux**: compile project targets with `-malign-double` and `_TIME_BITS=64` + `_FILE_OFFSET_BITS=64` so `long long`/`double` alignment and 64-bit `time_t` match the Win32 x86 ABI. The existing `Code/AbiGuards.h` static assertions remain the gate; no record field changes are required.
- **Source portability fixes** (all required to compile):
  - Replace 48 backslash include paths in 38 files with forward slashes.
  - Remove 594 `#pragma region` / `#pragma endregion` lines that GCC rejects inside initializer lists and between `if`/`else`.
  - Replace MSVC-only CRT use: `<mbstring.h>`/`_mbsinc`, 43 `_strupr` calls, two `fopen_s` calls, three `__int8` casts, and add missing `<cstring>`/`<cstdarg>` includes.
  - Replace Win32 typedef/macro leftovers: `FLOAT` and the `SendMessage()` call that only worked because `windows.h` expanded it.
  - Implement Linux executable-path discovery in `platform::ExecutablePath()` with `/proc/self/exe`.
  - Fix six MSVC-permissive `goto`-over-initialization sites in `_MSG_Buy.cpp`, `_MSG_Sell.cpp`, `_MSG_UpdateItem.cpp` and `_MSG_UseItem.cpp`; `-fpermissive` does not cover these.
  - Remove the stray `CReadFiles::CReadFiles();` statement.
- **Runtime paths**: replace hard-coded backslash log/day-log/dump path literals with forward slashes so logs land in the existing `./Log`, `./DayLog`, `./chat_log` and `./item_log` directories on Linux.
- **Linux verification**: run the existing unit tests on the i386 build, adjust the platform test's executable-name assertion, and add a POSIX smoke test for DBSrv/TMSrv startup, loopback accept, `quit` and SIGINT.
- **Documentation**: document 32-bit Linux prerequisites (multilib, vcpkg bootstrap, `x86-linux` triplet), presets, run steps and the ABI flags.
- **BREAKING — supported platforms**: the build contract changes from "Windows x86 only" to "Windows x86 plus Linux i386". Wire format, ports, on-disk formats and Windows behavior are unchanged.

This change records the 32-bit Linux route. If accepted, it supersedes `linux-x64-port` as the Linux platform plan; that change is left untouched for review but should not be applied as well.

## Capabilities

### New Capabilities

None — the change extends the existing build and platform capabilities to a second supported target.

### Modified Capabilities

- `build/cmake-vcpkg`: adds a Linux i386 build target, i386-specific CMake presets/triplet, platform-scoped compiler flags, explicit Win32-compatible layout flags on Linux, and Linux test execution.
- `platform/win32-free-code`: adds a prohibition on MSVC-only CRT facilities in shared sources, portable executable-path discovery, platform path separators, and platform path-case rules while preserving the existing error and byte semantics.

## Impact

- **Code**: `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json` (no dependency change expected), `Code/Basedef.h`, `Code/Basedef.cpp`, `Code/platform/Path.cpp`, `Code/platform/FileSystem.cpp` (case rule), `Code/DBSrv/CRanking.cpp`, `Code/DBSrv/Server.cpp`, `Code/TMSrv/Server.cpp`, `Code/TMSrv/App.cpp`, `Code/TMSrv/ProcessDBMessage.cpp`, the four `_MSG_*` files with `goto` labels, `tests/`, README and `docs/`.
- **Build**: two new Linux i386 presets plus a vcpkg overlay for the community `x86-linux` triplet; no new third-party dependency. Windows presets and behavior remain.
- **Runtime**: Linux binaries gain native executable-path resolution and POSIX paths; Windows behavior is unchanged.
- **Compatibility**: wire format, ports, account/character/capsule bytes, the 12-byte header and the 7808/816/536/1352/840-byte record sizes remain unchanged.
- **Prerequisites**: 32-bit multilib C/C++ runtime, a bootstrapped vcpkg checkout compatible with the pinned baseline, CMake 3.25+ and Ninja or Make.
- **Out of scope**: Linux x86_64, macOS, BSD, Windows-on-ARM, cross-compilation, container packaging, service-manager units and CI hosting.
