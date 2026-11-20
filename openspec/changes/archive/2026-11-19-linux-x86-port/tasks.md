## 1. Build scaffolding

- [x] 1.1 Make `CMakeLists.txt` target-aware: accept Windows 32-bit and Linux i386, reject other platforms/architectures with a clear message, and scope `CMAKE_MSVC_RUNTIME_LIBRARY`, `/W3 /permissive /Zc:strictStrings-` and the Windows-only preprocessor definitions to MSVC/Windows. Verify the existing Windows `x86-debug` configure still succeeds and a non-i386 Linux configure fails with the new message.
- [x] 1.2 Keep `ASIO_STANDALONE` and `ASIO_USE_TS_EXECUTOR_AS_DEFAULT` in the shared definitions, ensure `WIN32`/`_WIN32` are not defined on Linux, and verify asio selects the POSIX backend during a Linux compile.
- [x] 1.3 Add `linux-x86-debug` and `linux-x86-release` configure/build/test presets using `VCPKG_TARGET_TRIPLET=x86-linux` and `VCPKG_OVERLAY_TRIPLETS=$env{VCPKG_ROOT}/triplets/community`. Verify a clean checkout configures after `bootstrap-vcpkg.sh` and resolves the pinned asio/Catch2 packages.
- [x] 1.4 Add `Threads::Threads` (or equivalent pthread linkage) to the shared core and verify the Linux servers and tests link.
- [x] 1.5 Make the `../release` post-build copy opt-in on Linux and verify a Linux build succeeds without an existing release tree.

## 2. ABI and platform settings

- [x] 2.1 Apply `-malign-double` and `_TIME_BITS=64` + `_FILE_OFFSET_BITS=64` to Linux project targets, and verify every `Code/AbiGuards.h` static assertion passes.
- [x] 2.2 Add compile-time offset assertions for `STRUCT_ACCOUNTFILE::Char`, `STRUCT_ACCOUNTFILE::Cargo`, `STRUCT_ACCOUNTFILE::mobExtra` and `STRUCT_CAPSULE::extra`, and verify their Linux i386 offsets equal the Win32 x86 values (224, 3488, 5608, 816).
- [x] 2.3 Add a configure-time check that Linux uses a 32-bit pointer width and a 64-bit `time_t` (glibc ≥ 2.34), and verify a machine without 32-bit multilib fails with an actionable message.
- [x] 2.4 Verify the Windows x86 Debug and Release builds still pass the same ABI assertions after the build changes.

## 3. Source portability

- [x] 3.1 Replace the 48 backslash `#include "..\..."` directives in 38 files with forward-slash paths, and verify all translation units resolve their headers.
- [x] 3.2 Remove the 297 `#pragma region` / `#pragma endregion` pairs (594 lines) from the 14 affected files, and verify GCC no longer reports pragma parse errors in initializer lists or `if`/`else` chains.
- [x] 3.3 Remove the unconditional `<mbstring.h>` include and replace the live `_mbsinc` use with a CP949/ANSI lead-byte helper; verify `Code/Basedef.cpp` compiles and the helper treats `0x81`–`0xFE` as a two-byte lead.
- [x] 3.4 Replace the 43 `_strupr` calls with a portable byte-wise uppercase helper, and verify ASCII and high-byte behavior matches the legacy ANSI uppercase path.
- [x] 3.5 Replace the two `fopen_s` calls with `std::fopen` while preserving the null checks, and verify DBSrv compiles and still logs an unreadable file.
- [x] 3.6 Replace the three `unsigned __int8` casts with `unsigned char` and the four `FLOAT` uses with `float`, and verify `Code/TMSrv/Server.cpp` compiles.
- [x] 3.7 Change the one `cSock.SendMessage()` call to `SendMessageA()`, and verify `Code/TMSrv/ProcessDBMessage.cpp` compiles without relying on a `windows.h` macro.
- [x] 3.8 Remove the stray `CReadFiles::CReadFiles();` statement and verify TMSrv compiles without `-fpermissive`.
- [x] 3.9 Add `<cstring>` to `Code/Basedef.h` and `<cstdarg>` to `Code/DBSrv/Server.cpp`, and verify the headers compile in a standalone include check.
- [x] 3.10 Implement Linux executable-path discovery in `platform::ExecutablePath()` with `/proc/self/exe`, keep the Windows `_pgmptr` path, and verify the executable-relative-working-directory test passes on Linux and Windows.

## 4. MSVC-permissive `goto` fixes

- [x] 4.1 Move the `key`, `Quest` and `i` declarations above the key-check jumps in `_MSG_UpdateItem.cpp`, and verify the file compiles with GCC.
- [x] 4.2 Move the `Arena` and `Village` declarations above the `CanSave` jumps in `_MSG_UseItem.cpp`, and verify the file compiles with GCC.
- [x] 4.3 Scope or hoist the tax locals in `_MSG_Sell.cpp` so the `LABEL_S2` jump crosses no initialization, and verify the file compiles with GCC.
- [x] 4.4 Refactor `_MSG_Buy.cpp` so `LABEL_BUY1` and the donate path reach the common tail without jumping over live declarations, and verify the file compiles with GCC.
- [x] 4.5 Verify the whole `tmsrv` target compiles and links without `-fpermissive`.

## 5. Runtime paths and case rules

- [x] 5.1 Replace the hard-coded backslash log/day-log/chat-log/item-log and debug-dump literals in `TMSrv/Server.cpp`, `DBSrv/Server.cpp` and `Basedef.cpp` with forward slashes, and verify on Linux that logs land under `./Log`, `./DayLog`, `./chat_log` and `./item_log`.
- [x] 5.2 Make `platform::DirectoryIterator` wildcard folding Windows-only, add a test with names that differ only by case, and verify the test reflects case-insensitive Windows and case-sensitive Linux behavior.

## 6. Linux tests

- [x] 6.1 Replace the hard-coded `w2pp_tests.exe` assertion in `tests/platform/PlatformTests.cpp` with a platform-aware executable-name check, and verify the platform tests pass on Linux.
- [x] 6.2 Add a POSIX static source check equivalent to `check_platform_dependencies.ps1` that rejects MSVC-only headers/functions outside `Code/platform/`, register it with CTest, and verify it fails on an injected `<mbstring.h>`/`fopen_s` and passes on the clean tree.
- [x] 6.3 Add a POSIX smoke test that builds a temporary runtime tree, starts DBSrv, starts TMSrv with `W2PP_FORCE_CONSOLE=1`, verifies the 8281 listener accepts the init handshake, sends `quit`, and sends `SIGINT` to a second run; verify both runs exit successfully with timeout-bounded cleanup.
- [x] 6.4 Verify `ctest --preset linux-x86-debug` runs the unit tests and the POSIX smoke test and fails on a forced failure.

## 7. Documentation

- [x] 7.1 Update `README.md` with Linux i386 prerequisites (multilib, bootstrapped vcpkg, `x86-linux` triplet), presets, build/test commands and the updated supported-target statement.
- [x] 7.2 Update `docs/operations.md` with the Linux runtime tree, console behavior, `W2PP_FORCE_CONSOLE`, SIGINT/SIGTERM shutdown and exit codes.
- [x] 7.3 Update `docs/platform-abstractions.md` with Linux executable-path discovery, path separator and path case rules, and the CP949 source/content encoding policy.
- [x] 7.4 Record in the change notes that this proposal supersedes `linux-x64-port` as the Linux plan and that the x64 change must not be applied alongside it.

## 8. Cross-platform verification

- [x] 8.1 On Linux i386, build Debug and Release and run the full CTest suite plus the POSIX smoke test; verify every test passes.
- [x] 8.2 On Windows x86, build Debug and Release and run the full CTest suite including the existing PowerShell harnesses; verify every test passes.
- [x] 8.3 On both platforms, replay the codec fixture and a record round-trip; verify the 12-byte header and the 7808/816/536/1352/840-byte record sizes and the guarded field offsets match.
- [x] 8.4 On Linux, confirm the servers start without a desktop, accept a loopback connection, reject an oversized/invalid frame with a logged close, and shut down cleanly through both `quit` and SIGINT.
