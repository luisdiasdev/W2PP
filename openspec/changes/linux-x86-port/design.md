## Context

See `proposal.md` — Why. The tree already builds on Windows x86 through CMake + vcpkg, uses asio for all transport, and routes filesystem, clock, path and diagnostic work through `Code/platform/`. The remaining coupling is concentrated in the build definition, a small set of MSVC-only CRT/compiler constructs, Win32-shaped path literals, and the Linux compiler's default i386 ABI.

The investigation compiled all 95 `Code/` translation units and all 10 test translation units with `g++ -m32` against the project headers and vcpkg's asio/Catch2 headers. Every TU reaches the same short blocker list below; no architectural port is needed.

## Goals / Non-Goals

**Goals:**

- Configure, build, test and run both servers natively on Linux i386 with the pinned vcpkg manifest.
- Keep the wire format, on-disk formats, ports and Windows x86 behavior byte-identical.
- Make the ABI match Win32 x86 explicitly, gated by the existing `Code/AbiGuards.h` assertions.
- Keep every source and build change mechanical and reviewable.

**Non-Goals:**

- Linux x86_64 or any record-field/format change.
- macOS, BSD, Windows-on-ARM, cross-compilation.
- Container images, systemd units, packaging or CI hosting.
- Gameplay, protocol, persistence or content-encoding changes.

## Decisions

### D1 — Target Linux i386, not x86_64

Keep the Win32 x86 ABI and add Linux i386 presets. On i386 Linux the widths of `int`, `long`, pointers, `size_t` and the guarded record fields already match Win32 x86; only two calling-convention details differ. The x86_64 route would require changing bare `long` fields in layout-sensitive records and auditing every guarded struct for LP64, which is a larger compatibility risk.

Alternatives considered:

- **Linux x86_64 with fixed-width record fields** (the existing `linux-x64-port` proposal): rejected for this change because it changes the compatibility surface; it remains a separate proposal.
- **32-bit only for tests, x64 for production**: rejected; the point is a binary that talks to the same client and reads the same files.

### D2 — Match the Win32 x86 layout with two explicit flags

Plain `-m32` does **not** reproduce the Win32 ABI. Measured with the project headers:

| Build flags | `STRUCT_MOB` | `STRUCT_MOBEXTRA` | `STRUCT_CAPSULE` | `STRUCT_ACCOUNTFILE` |
|---|---:|---:|---:|---:|
| Win32 MSVC expectation | 816 | 536 | 1352 | 7808 |
| `-m32` | 812 | 492 | 1304 | 7608 |
| `-m32 -malign-double` | 816 | 520 | 1336 | 7744 |
| `-m32 -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64` | 812 | 504 | 1316 | 7656 |
| **`-m32 -malign-double` + time flags** | **816** | **536** | **1352** | **7808** |

The two causes:

1. GCC i386 aligns `long long` and `double` to 4 bytes; MSVC x86 aligns them to 8. `-malign-double` restores the 8-byte alignment.
2. glibc i386 `time_t` is 4 bytes; MSVC `time_t` is 8. `_TIME_BITS=64` (which requires `_FILE_OFFSET_BITS=64`) restores the 64-bit type so `time(&field)` and the persisted `STRUCT_MOBEXTRA` fields stay compatible. It requires glibc ≥ 2.34 on i386.

Size checks alone are insufficient: internal offsets shift too. With plain `-m32`, `STRUCT_ACCOUNTFILE::Char` is at 220 instead of 224, `Cargo` at 3468 instead of 3488, and `STRUCT_CAPSULE::extra` at 812 instead of 816. The flag pair fixes both sizes and offsets, and `AbiGuards.h` remains the build-time gate.

Alternatives considered:

- **`alignas(8)` on non-packed `long long` members plus `int64_t` time fields**: avoids the global alignment switch, but requires annotating every layout-sensitive non-packed struct and changing all `time(&field)` call sites. Rejected as a larger, error-prone diff.
- **`-mstackrealign`/toolchain defaults**: does not change struct member alignment or `time_t` width.
- **16-bit/other widths**: not applicable.

`-malign-double` is applied only to project targets, not to vcpkg-built dependencies. It is documented to require consistent compilation; here no struct containing `double` or `long long` is passed by value across the libstdc++ boundary (verified with `<thread>`, `<condition_variable>`, `<chrono>` and `<filesystem>` on i386), so the practical risk is contained.

### D3 — Platform-aware CMake with additive presets

- Replace the `if(NOT WIN32) FATAL_ERROR` and unconditional pointer-width check with a target matrix: Windows must be 32-bit; Linux must be 32-bit i386; other platforms fail with a clear message.
- Scope `CMAKE_MSVC_RUNTIME_LIBRARY`, `/W3 /permissive /Zc:strictStrings-` and the Windows-only preprocessor definitions to MSVC/Windows. In particular, do **not** define `WIN32`/`_WIN32` on Linux — asio selects its Windows backend from that macro.
- Keep `ASIO_STANDALONE` and `ASIO_USE_TS_EXECUTOR_AS_DEFAULT` portable.
- On Linux, add `-malign-double`, `_TIME_BITS=64`, `_FILE_OFFSET_BITS=64` and `Threads::Threads` to project targets.
- Add `linux-x86-debug`/`linux-x86-release` configure/build/test presets with `VCPKG_TARGET_TRIPLET=x86-linux`. Use `VCPKG_OVERLAY_TRIPLETS=$env{VCPKG_ROOT}/triplets/community` rather than vendoring a triplet, so the vcpkg toolchain's existing `x86` → `-m32` mapping is used. If the overlay is unavailable, a project-local triplet file is the fallback.
- Keep the `../release` post-build copy opt-in on Linux; Linux runs from the build tree.

The vcpkg manifest and pinned baseline are unchanged; `asio` is header-only and `catch2` builds for `x86-linux`.

### D4 — Source compatibility shims

- **Include paths**: convert 48 `#include "..\..."` directives in 38 files to forward slashes. GCC does not treat backslash as a path separator.
- **Pragmas**: delete the 297 `#pragma region`/`#pragma endregion` pairs (594 lines, 14 files). They are Visual Studio folding hints and are the only pragmas GCC rejects in these positions (inside initializer lists and between `if`/`else`). `#pragma once` and `#pragma pack` are portable and stay.
- **MSVC CRT**: remove `<mbstring.h>`; `_mbschr` is only referenced in a commented block. Replace the lone live `_mbsinc` use with a lead-byte helper that preserves the CP949/ANSI rule (`0x81`–`0xFE` starts a two-byte character). Replace 43 `_strupr` calls with a byte-wise uppercase helper using `toupper` on `unsigned char`, matching the legacy ANSI behavior. Replace the two `fopen_s` calls with `std::fopen` plus the existing null check. Replace three `unsigned __int8` casts with `unsigned char`.
- **Missing includes**: add `<cstring>` to `Basedef.h` (inline constructor uses `strncpy`) and `<cstdarg>` to `DBSrv/Server.cpp` (`va_start`).
- **Win32 leftovers**: replace the four `FLOAT` uses with `float`; call `SendMessageA()` at the one site that relied on the `windows.h` macro. Remove the stray `CReadFiles::CReadFiles();` statement.
- **Executable path**: keep `_pgmptr` on Windows and use `readlink("/proc/self/exe")` on Linux, returning an empty path on failure so the existing boot failure path applies. Use a buffer that grows or a `PATH_MAX` buffer with explicit truncation handling.
- **Source encoding**: `Code/ItemEffect.h` is ISO-8859, not UTF-8. GCC compiles it because the non-UTF-8 bytes are in comments; converting it is a cleanup, not a blocker.

### D5 — Fix six MSVC-permissive `goto` sites

GCC rejects jumps over initialized declarations even with `-fpermissive`. The fixes are local restructuring, not control-flow redesign:

| File | Label(s) | Fix |
|---|---|---|
| `_MSG_Buy.cpp` | `LABEL_BUY1` | Hoist `GuildTax` declaration before the jump or scope the tax calculation. |
| `_MSG_Buy.cpp` | `DonateBuy` | Refactor the donate branch so the common tail is reached without jumping over the later declarations; this is the only non-trivial file. |
| `_MSG_Sell.cpp` | `LABEL_S2` | End the scope of the tax locals before the label, or hoist them. |
| `_MSG_UpdateItem.cpp` | `NoNeedKey` | Move `key`, `Quest` and `i` above the key-check jumps. |
| `_MSG_UseItem.cpp` | `CanSave` | Move `Arena` and `Village` above the jumps. |

### D6 — Runtime paths and case rules

- Replace the hard-coded backslash log/day-log/chat-log/item-log and debug-dump literals with forward slashes. Windows accepts forward slashes, so one code path serves both platforms.
- Make `platform::DirectoryIterator` wildcard folding Windows-only; Linux observes case-sensitive names. This matches platform path lookup and prevents a scan from returning a name that a subsequent `fopen` cannot open.
- glibc accepts `"rt"`/`"wt"` modes, so no `fopen` mode changes are needed.

### D7 — Linux test strategy

- The unit tests run unchanged through CTest on i386. The only test source change is the platform test's `"w2pp_tests.exe"` assertion, which becomes platform-aware.
- Keep the PowerShell integration harnesses under `if(WIN32)`.
- Add a POSIX smoke test under `if(UNIX)`: build a temporary runtime tree, start DBSrv, start TMSrv with `W2PP_FORCE_CONSOLE=1`, verify the listener accepts the init handshake, send `quit` through the console pipe, and send `SIGINT` to a second run. Bound every wait with a timeout and clean up processes and temp directories on every path.
- Add an optional POSIX static source check equivalent to `check_platform_dependencies.ps1` so MSVC-only headers/functions cannot return unnoticed.

### D8 — Relationship to `linux-x64-port`

This change records the i386 route and, if accepted, supersedes `linux-x64-port` as the Linux plan. The x64 change is not deleted or archived here; both are planning-only, and applying both would produce contradictory target matrices. Most source shims in D4/D5 are shared by either route; only D2 is i386-specific and only the x64 route needs the `long`-field changes.

## Risks / Trade-offs

- **`-malign-double` ABI mixing** → apply it only to project targets, keep all project TUs consistent, and rely on the static assertions; verify `<thread>`, `<condition_variable>`, `<chrono>` and `<filesystem>` behavior on i386.
- **glibc older than 2.34** → `_TIME_BITS=64` is unavailable; document the minimum and fail configure clearly. The alternative is the `int64_t` time-field refactor.
- **Missing 32-bit multilib or 32-bit vcpkg artifacts** → document `gcc-multilib`/`g++-multilib`/`libc6-dev-i386`; keep the `x86-linux` triplet community overlay and fail configure with a clear message when the toolchain cannot produce a 32-bit object.
- **Stale or unbootstrapped vcpkg** → document `bootstrap-vcpkg.sh`; a mismatched vcpkg binary currently fails before dependency resolution.
- **CP949 vs UTF-8 content** → preserve the legacy lead-byte/uppercase rules for byte compatibility; do not transcode content files.
- **Case sensitivity** → make wildcard folding platform-specific and add a test with names that differ only by case.
- **Windows regression from the shims** → keep Windows syntax and behavior unchanged, and run the full Windows x86 Debug/Release build and test suite after the change.
- **Integration test flakiness** → use a temporary tree, timeouts, trap-based cleanup and explicit port-conflict reporting.

## Migration Plan

1. Build scaffolding: platform-aware CMake, `x86-linux` presets/triplet, 32-bit configure check.
2. Mechanical source cleanup: includes, pragmas, MSVC CRT shims, missing includes, Win32 leftovers.
3. Linux executable path and runtime path separators.
4. `goto`/declaration fixes.
5. ABI flags and platform test executable-name fix; confirm every `AbiGuards.h` assertion passes.
6. Build and link, run unit tests, then the POSIX smoke test.
7. Documentation and Windows regression verification.

**Rollback:** revert the branch. Windows presets and behavior are unchanged, no wire or on-disk format changes are involved, and the previous Windows build remains available throughout.

## Open Questions

None at this time. The remaining choices (CP949 lead-byte rule, case-sensitive Linux matching, community triplet overlay) are decided above; they do not change the spec-level contract or the task breakdown.
