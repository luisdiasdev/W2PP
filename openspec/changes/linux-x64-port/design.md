# Design

## Context

See `proposal.md` for motivation. The current tree already builds Windows x86 through CMake + vcpkg, runs headless on asio, and keeps all project code free of Windows headers and Winsock calls. The remaining Linux work is concentrated in the build definition, a few MSVC CRT calls, two layout-sensitive `long` fields and Windows-shaped path construction.

Constraints carried into this design:

- The closed-source client depends on the 12-byte header, the keyword/transform/checksum scheme and the 196-byte billing records; none of that may change.
- Account, character, capsule and selected-character records must stay byte-identical to the existing files.
- The existing Windows x86 presets, tests and operator behavior must keep working.
- The supported Linux target selected for this change is x86_64; 32-bit Linux is not required.
- The `migrate-win32-to-asio` change is complete but not archived. Its capability specs are the base for this change, so it must be archived before this change is archived.

## Goals / Non-Goals

**Goals:**

- Configure, build, test and run both servers natively on Linux x86_64 with the pinned vcpkg manifest.
- Keep the wire format, on-disk formats, ports and Windows behavior unchanged.
- Replace the remaining MSVC-only portability blockers with standard C/C++ or platform wrappers.
- Prove the Linux runtime through a real startup, loopback accept and both shutdown paths.

**Non-Goals:**

- macOS, BSD, Windows-on-ARM or 32-bit Linux.
- Cross-compiling Linux binaries from Windows.
- Container images, systemd units, packaging or CI hosting.
- Changing gameplay, protocol, persistence or encoding of runtime data files.
- Reworking the Windows build layout or renaming the existing presets.

## Decisions

### D1 — Supported target matrix: Windows x86 + Linux x86_64

Keep the Windows `x86-debug`/`x86-release` presets on the Win32 x86 ABI, and add Linux `linux-debug`/`linux-release` presets for x86_64. Reject other platforms and architectures at configure time with a clear diagnostic.

Alternatives considered:

- **32-bit Linux only** to preserve the old pointer width: rejected because 32-bit vcpkg triples and multilib toolchains are poorly supported and would not solve the `long` issue portably.
- **Both 32-bit and 64-bit Linux**: rejected as unnecessary scope; the fixed-width ABI decision makes 64-bit safe.
- **Rename the existing presets** to `win-x86-*`: rejected because the README, tests and completed change already reference the current names.

### D2 — Fixed-width record fields instead of pointer-width ABI

The only bare `long` fields inside the guarded records are `STRUCT_MOB::LearnedSkill` and the same field in the nested `SaveCelestial` block of `STRUCT_MOBEXTRA`. Replace both with a fixed-width 32-bit integer. `long long` fields already have the same width on Win32 and LP64. Keep the existing `#pragma pack` regions and the `static_assert` guards for the header and all persisted records.

This keeps the Windows layout and every on-disk/wire byte unchanged while making the Linux x64 layout identical. The ABI guards are the gate: a missed field fails the build instead of silently changing a save file.

Alternatives considered:

- **Keep `long` and build Linux with `-m32`**: rejected; does not meet the selected x86_64 target.
- **Remove the ABI asserts and accept x64 layouts**: rejected; it would break account/character compatibility.
- **Serialize records field-by-field**: rejected; changes the persistence design far beyond a platform port.

### D3 — Platform-aware CMake, additive presets

- Replace the `if(NOT WIN32) FATAL_ERROR` and the unconditional 32-bit pointer check with a target check: Windows must be Win32 x86, Linux must be x86_64, everything else fails with a clear message.
- Apply `/W3 /permissive /Zc:strictStrings-` only to MSVC. Use the compiler default warning setup on GCC/Clang; no `-Werror`, because the legacy sources still produce warnings.
- Keep `CMAKE_MSVC_RUNTIME_LIBRARY` only for MSVC. On Linux, the system C++ runtime is used for both configurations.
- Scope `_CRT_SECURE_NO_WARNINGS`, `_MBCS`, `WIN32`, `_WINDOWS`, `_WIN32_WINNT`, `WIN32_LEAN_AND_MEAN` and `_WINSOCK_DEPRECATED_NO_WARNINGS` to Windows. Keep `ASIO_STANDALONE` and the asio executor workaround on both.
- Add `linux-debug`/`linux-release` configure, build and test presets with the Ninja generator (falling back to Unix Makefiles if Ninja is unavailable) and `VCPKG_TARGET_TRIPLET=x64-linux`, `VCPKG_MANIFEST_MODE=ON`, `CMAKE_TOOLCHAIN_FILE=$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`.
- Keep the post-build copy into `../release` Windows-only, or require an explicit `W2PP_RELEASE_ROOT` on Linux. Linux runs directly from the build tree.

The vcpkg manifest (`asio`, `catch2`) and the pinned baseline are unchanged; both ports support Linux.

### D4 — Portability shims

- **Executable path**: `platform::ExecutablePath()` keeps `_pgmptr` on Windows and uses `readlink("/proc/self/exe")` with a growing buffer on Linux. A failure returns an empty path, which `SetWorkingDirectoryToExecutable()` already turns into a non-zero boot failure.
- **MSVC MBCS header**: `<mbstring.h>` is compiled unconditionally, but the only `_mbschr` uses are inside a commented-out block. Remove the include and the dead commented block. No behavior changes.
- **Annex K file open**: replace `fopen_s(&fs, path, "rb")` in `DBSrv/CRanking.cpp` and `DBSrv/Server.cpp` with `std::fopen(path, "rb")` and keep the existing null check and error logging.
- **Source encoding**: `Code/ItemEffect.h` contains CP949 Korean comments. Convert the file to UTF-8 (no BOM); it has no string literals or data, so runtime bytes are unaffected. Do not change content/data files, which stay byte-preserving.
- **Dependency guard**: add a Linux static check (`tests/tools/check_portable_sources.sh`) that rejects MSVC-only headers/identifiers in project sources outside `Code/platform/` and register it with CTest on UNIX. The existing PowerShell check remains the Windows guard.

Alternatives considered:

- **`-finput-charset=CP949` globally**: rejected; depends on iconv support and hides other non-UTF-8 source text.
- **Keep `<mbstring.h>` behind `#ifdef _WIN32`**: unnecessary once the dead uses are removed; removing it also removes the trap.
- **Add a portable `fopen_s` polyfill**: rejected; `std::fopen` is the direct equivalent for these call sites.

### D5 — Platform-neutral path construction

Several TMSrv/DBSrv log paths and the debug checksum dumps use hard-coded `".\\Log\\..."`, `".\\DayLog\\..."` and `"..\\SkillCSum.h"` literals. Forward slashes are accepted by the Windows CRT and by `std::filesystem`, so replace the backslash literals with `/` and keep the same logical directories (`./Log`, `./DayLog`, `./chat_log`, `./item_log`, `../SkillCSum.h`). This keeps one code path on both platforms and preserves the existing file names.

`platform::DirectoryIterator`'s wildcard matcher currently folds case on every platform. Make the fold Windows-only so a Linux scan observes the platform's case-sensitive names; Windows behavior is unchanged.

Alternatives considered:

- **A new `platform::JoinPath` helper**: workable, but the path fragments are simple and a forward-slash literal is a smaller diff.
- **Platform-specific path constants**: duplicates every log path and invites drift.

### D6 — Linux host, console and signals

No new host subsystem is needed: `host::Runtime` already uses `asio::signal_set` with `SIGINT`/`SIGTERM`, `platform::StandardInputIsTerminal()` already has an `isatty` branch, and the console thread already honors `W2PP_FORCE_CONSOLE`. The Linux change is verification and documentation: a TTY starts the console, a pipe with the force-console flag feeds commands, and Ctrl+C/SIGTERM take the same graceful shutdown path as `quit`. Windows console behavior is untouched.

### D7 — Linux test strategy

- Unit tests (`w2pp_tests`) run unchanged through CTest on both platforms.
- Windows keeps `dbsrv_loopback.ps1` and `tmsrv_smoke.ps1` under `if(WIN32)`.
- Add a POSIX smoke script under `if(UNIX AND NOT APPLE)` that:
  1. builds a temporary runtime tree (`mktemp -d`, `mkdir -p`, `printf`, `dd`, copies `ItemEffect.h`);
  2. starts `dbsrv`, waits for ports 7514/8895, then starts `tmsrv` with `W2PP_FORCE_CONSOLE=1` and FIFOs for stdin;
  3. connects to 8281, sends the four-byte init handshake and confirms the socket stays open;
  4. sends `quit` through the FIFOs and checks both exit codes;
  5. starts a server again and sends `SIGINT`, checking the graceful exit code.
- Add a Linux static source check as described in D4.

A C++ process-spawning harness was considered; a POSIX shell script is smaller, uses the same operator surface and avoids adding portable process-creation code to the test target.

### D8 — Documentation

- README: Linux prerequisites (CMake, Ninja, C++20 compiler, vcpkg, `VCPKG_ROOT`), Linux presets, build/test commands, run steps and the updated supported-target statement.
- `docs/operations.md`: Linux runtime tree, console commands, `W2PP_FORCE_CONSOLE`, signals and exit codes.
- `docs/platform-abstractions.md`: Linux executable-path mechanism, path case rules and source-encoding policy.

## Risks / Trade-offs

- **Missed LP64-sensitive field** → the existing `static_assert`s are the gate; audit every persisted/guarded struct and add guards where a layout is not yet covered.
- **Windows regression from the ABI edit** → `long` and `int32_t` have the same width and alignment on Win32; run the full Windows build and test suite after the change.
- **Legacy GCC warnings** → do not enable `-Werror`; keep the same warning policy as the Windows build and fix only errors.
- **CP949 conversion damage** → the affected file contains comments only; review the diff and assert runtime data files are untouched.
- **vcpkg baseline/triplet differences on Linux** → pin the same baseline; document `VCPKG_ROOT`; fail configure clearly if the triplet cannot resolve.
- **Path case sensitivity** → make wildcard folding Windows-only, document the rule and test case-only names.
- **Integration test flakiness** → use a temporary tree, bound every wait with a timeout, trap cleanup, and report port conflicts explicitly.
- **Windows-only integration coverage** → retain the PowerShell harnesses and mirror the essential start/accept/shutdown checks in the POSIX smoke test.
- **Console thread lifetime** → the existing runtime detaches the console reader on shutdown; the Linux tests must confirm the process still exits cleanly rather than blocking on a TTY read.

## Migration Plan

Phases leave the tree buildable on Windows at every gate:

1. **Build scaffolding**: platform-aware CMake, Linux presets and triplet; configure/build the current sources on Linux to enumerate blockers.
2. **ABI and portability**: fixed-width record fields, executable path, `fopen_s`, `<mbstring.h>`, source encoding; Windows and Linux ABI guards pass.
3. **Runtime paths**: forward-slash log/dump paths and platform-aware wildcard matching.
4. **Linux tests and docs**: POSIX smoke test, static portability check, README/operations/platform docs.
5. **Verification**: Windows x86 Debug/Release build + CTest regression; Linux x86_64 Debug/Release build + CTest, loopback accept, `quit` and SIGINT shutdown.

**Rollback:** revert the branch. Windows presets and runtime behavior are additive changes, no data or wire format changes are involved, and the previous Windows build remains available throughout.

## Open Questions

- Whether to add hosted CI for Linux later is deferred; this change only makes the local and CI-ready commands available.
- Whether Windows x86 should eventually move to x64 is deferred to a separate change and does not affect this design.
