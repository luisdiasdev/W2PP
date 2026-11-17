# Tasks

## 1. Build target scaffolding

- [ ] 1.1 Make `CMakeLists.txt` platform-aware: accept Windows x86 and Linux x86_64, reject other platforms/architectures with a clear message, scope MSVC-only flags/defines/runtime settings to MSVC/Windows, and keep the release-tree copy Windows-only or opt-in; verify the Windows `x86-debug`/`x86-release` configure and build still succeed and an unsupported-target check fails clearly.
- [ ] 1.2 Add `linux-debug`/`linux-release` configure, build and test presets using Ninja (or Unix Makefiles) and `VCPKG_TARGET_TRIPLET=x64-linux`; verify `cmake --preset linux-debug` configures a clean Linux x86_64 checkout and resolves the pinned `asio`/`catch2` packages.

## 2. ABI, portable CRT and source encoding

- [ ] 2.1 Replace the two bare `long LearnedSkill` fields in `STRUCT_MOB` and the nested `SaveCelestial` block of `STRUCT_MOBEXTRA` with a fixed-width 32-bit type, audit the guarded records for other pointer-width-sensitive fields, and verify the `HEADER`/`STRUCT_ACCOUNTFILE`/`STRUCT_MOB`/`STRUCT_MOBEXTRA`/`STRUCT_CAPSULE`/`STRUCT_SELCHAR` static asserts compile on Windows x86 and Linux x86_64.
- [ ] 2.2 Implement Linux executable-path resolution in `platform::ExecutablePath()` using `/proc/self/exe` with a growing buffer, keep the Windows `_pgmptr` path, and verify the existing executable-relative-working-directory test passes on both platforms, including the empty-path failure case.
- [ ] 2.3 Remove the unconditional `<mbstring.h>` include and the dead commented `_mbschr` block in `Basedef.cpp`, replace both `fopen_s` calls with `std::fopen` while preserving the null check and error logging, and verify the Linux build compiles the shared core and DBSrv and the Windows build/tests still pass.
- [ ] 2.4 Convert `Code/ItemEffect.h` to UTF-8 without a BOM (comments only), verify GCC builds it under the default input encoding, and verify runtime content files are not transcoded and no data bytes change.
- [ ] 2.5 Add `tests/tools/check_portable_sources.sh` that rejects MSVC-only CRT headers/functions in project sources outside `Code/platform/`, register it with CTest on UNIX, and verify it fails on an injected `<mbstring.h>`/`fopen_s` and passes on the clean tree.
- [ ] 2.6 Document the Linux build prerequisites, presets, triplet and fixed-width ABI rule in `README.md` and the Linux executable-path, path-case and source-encoding contracts in `docs/platform-abstractions.md`; verify the documented Linux configure/build/test commands on a clean Linux checkout and the Windows commands still work.

## 3. Runtime paths and host behavior

- [ ] 3.1 Replace the hard-coded backslash log/day-log/chat-log/item-log and debug-dump path literals in `TMSrv/Server.cpp`, `DBSrv/Server.cpp` and `Basedef.cpp` with forward-slash literals that preserve the logical directories; verify on Linux that a started server creates its dated logs under `Log`/`DayLog` and not as literal backslash file names, and verify the Windows build/tests still pass.
- [ ] 3.2 Make `platform::DirectoryIterator` wildcard matching case-insensitive on Windows and case-sensitive on Linux, add a platform test covering names that differ only by case, and verify the test passes on both platforms.
- [ ] 3.3 Add a runtime test for SIGTERM shutdown (alongside the existing SIGINT test), verify it passes on Windows and Linux, and confirm SIGINT/SIGTERM use the same shutdown handler as the console `quit` command.

## 4. Linux integration and operator documentation

- [ ] 4.1 Add `tests/integration/linux_smoke.sh` and register it with CTest under UNIX: build a temporary runtime tree, start DBSrv, start TMSrv with `W2PP_FORCE_CONSOLE=1`, and verify both listening ports answer; verify `ctest --preset linux-debug -R linux-smoke` passes and the script fails clearly when a port is occupied.
- [ ] 4.2 Extend the Linux smoke test to send the four-byte init handshake to 8281, send `quit` through the console pipe, and send SIGINT to a second run; verify both runs exit with success, the expected log files exist, and the script cleans up processes, FIFOs and temporary directories on every path.
- [ ] 4.3 Update `README.md` with the Linux run steps and `docs/operations.md` with the Linux runtime tree, console commands, `W2PP_FORCE_CONSOLE`, signal shutdown and exit codes; verify the documented commands against the smoke-test runtime tree.

## 5. Cross-platform verification

- [ ] 5.1 On Windows x86, run the full Debug and Release configure/build and the complete CTest suite; verify the Windows server start/accept/shutdown checks and the existing PowerShell integration harnesses still pass.
- [ ] 5.2 On Linux x86_64, run the full Debug and Release configure/build and the complete CTest suite; verify every unit test and the Linux smoke test pass.
- [ ] 5.3 On both platforms, run the codec fixture replay and a record round-trip and confirm the 12-byte header and the 7800/816/536/1352/840-byte records match the pre-change bytes and sizes.
- [ ] 5.4 Confirm the Linux servers start without a desktop, accept a loopback connection, reject an oversized/invalid frame with a logged close, and shut down cleanly through both `quit` and SIGINT; confirm the Windows behavior is unchanged.

## Workflow follow-up

- Archive `migrate-win32-to-asio` before archiving this change so the modified capability specs exist in `openspec/specs/`.
- Archive this change after review and after the Windows and Linux verification tasks pass.
- Verify the archived capability specs reflect the final platform matrix, ABI rule and Linux runtime behavior.
