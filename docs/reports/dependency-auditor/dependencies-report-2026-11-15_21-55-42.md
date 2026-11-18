# Dependency Audit Report

**Project:** W2PP (W2 game server emulator, decompiled from "Polly's Server Release")
**Audit date:** 2026-11-15 21:55:42
**Project scope:** entire project root (`E:\wyd\test-setup\w2pp`)
**Excluded folders:** `.agents`, `.codegraph`, `.opencode`, `openspec` (plus `.git`, `.vs`, build output folders `Debug/`, `release/` where they contain compiled artifacts)
**Auditor:** dependency-auditor subagent (analysis and reporting only; no files were modified)

---

## 1. Summary

### 1.1 Project profile

| Attribute | Value |
|---|---|
| Languages | C and C++ (C++20 mode enabled by the toolset default) |
| Source files audited | 104 files (`*.cpp`, `*.h`), 49,330 lines |
| TMSrv (game server) | 87 files, 36,340 lines |
| DBSrv (account/character server) | 12 files, 5,059 lines |
| Shared root sources (`Code/`) | 5 files, 7,931 lines (`Basedef.cpp/.h`, `CPSock.cpp/.h`, `ItemEffect.h`) |
| Solution | `W2PP Code Project.sln` (Format 12.00, VS 17 / 17.14.37314.3) |
| Build system | MSBuild / Visual C++ (`.vcxproj`), left-to-right solution with 2 projects |
| Target platform | Win32 / x86 only (Subsystem: Windows, MBCS character set) |
| Package managers detected | **None.** No npm, pip, Conan, vcpkg, NuGet, Cargo, Go modules, Composer, or CMake dependency declarations exist anywhere in the repository (verified by recursive search for `package.json`, `requirements.txt`, `conanfile.*`, `vcpkg.json`, `packages.config`, `*.props`, `*.targets`, `CMakeLists.txt`, `Makefile`, `*.nuspec`). |
| Lockfiles | **None.** Consequently, no reproducible dependency graph exists. Platform component versions are pinned only indirectly by `PlatformToolset` and `WindowsTargetPlatformVersion`. |

### 1.2 Dependency model (important for interpreting this report)

This project has **no third-party library manifest**. Every dependency is a platform/toolchain dependency supplied by Microsoft or by the Windows operating system:

* MSVC Build Tools (compiler, linker, C/C++ standard library, C runtime)
* Windows SDK (Win32 headers and import libraries, UCRT)
* Winsock (network stack; 1.1 API surface requested at runtime)
* MFC / ATL (declared in one orphan header; not compiled)
* ODBC (import libraries linked in both projects; no ODBC API call exists in the code)
* Core Windows import libraries (`kernel32`, `user32`, `gdi32`, `winspool`, `comdlg32`, `advapi32`, `shell32`, `ole32`, `oleaut32`, `uuid`, `winmm`)
* MSVC CRT MBCS helpers (`mbstring.h`, `_mbschr`)

There is **no vendored third-party source** in the tree (no zlib, OpenSSL, SQLite, Boost, or similar). Third-party CVE identifiers therefore do not apply as library-version CVEs; the vulnerability surface is entirely (a) toolchain/SDK patch level, (b) CRT/API selection, and (c) the project's own unsafe use of CRT functions.

### 1.3 Main findings (count: 7 critical/high)

1. **The shipped "Release" binary links the debug C/C++ runtime** (`MSVCP140D.dll`, `VCRUNTIME140D.dll`, `ucrtbased.dll`) while defining `NDEBUG`. This makes the artifact non-redistributable and non-deployable on machines without Visual Studio.
2. **`Release|Win32` for TMSrv is configured with `RuntimeLibrary=MultiThreadedDebugDLL`** (`Code/TMSrv/TMSrv.vcxproj:80`), i.e. `/MDd` in a Release configuration, while `Debug|Win32` uses `/MTd` (static debug CRT). The two configurations are inverted relative to convention and inconsistent with each other.
3. **Both `Release` configurations are incomplete as committed.** They declare no `<AdditionalDependencies>`, yet the code requires `ws2_32.lib` (Winsock) and `winmm.lib` (`timeGetTime`), which are *not* in the MSBuild Win32 core default set (`kernel32;user32;gdi32;winspool;comdlg32;advapi32;shell32;ole32;oleaut32;uuid;odbc32;odbccp32`). The committed project state cannot reproduce the shipped binary.
4. **Windows Sockets 1.1 is requested at runtime** via `WSAStartup(MAKEWORD(1,1), ...)` (`Code/CPSock.cpp:113`). Microsoft's current guidance is to always request 2.2; Winsock 1.1 lacks IPv6 support and its name-resolution functions are documented as deprecated.
5. **`odbc32.lib` and `odbccp32.lib` are linked but never used.** No ODBC header or API symbol exists in the codebase. They are also duplicated in the Debug link line because the project lists the default core set and then appends `%(AdditionalDependencies)`.
6. **MFC is declared but orphaned.** `Code/DBSrv/stdafx.h:21` includes `<afxwin.h>`, but no translation unit includes `stdafx.h`, the MFC headers are not installed on the audit machine, and the shipped binary imports no MFC DLL. The project carries an MFC licensing/deployment obligation for a dependency it does not use.
7. **No dependency-integrity or reproduction controls exist**: no CI, no lockfile, no build script, floating `WindowsTargetPlatformVersion=10.0`, and stale/orphaned build artifacts and project files committed or left on disk.

Additionally, the project stores and compares account passwords in plaintext (`Code/DBSrv/CFileDB.cpp:1302`, `Code/DBSrv/Server.cpp:596`, `Code/DBSrv/CReadFiles.cpp:1021`) and has **no cryptographic dependency at all**, which is a functional gap rather than an outdated-library issue.

---

## 2. Critical Issues

### C-1. Debug C/C++ runtime linked into the Release artifact (Critical)

Evidence:

| Item | Value |
|---|---|
| Artifact | `Code/release/TMSrv/run/TMSrv.exe` (509,440 bytes, built 2026-11-06 17:25:48) |
| Sidecar | `Code/release/TMSrv/run/TMSrv.pdb` (2,854,912 bytes) |
| Imports (dumpbin `/dependents`) | `WINMM.dll`, `WS2_32.dll`, `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, **`MSVCP140D.dll`**, **`VCRUNTIME140D.dll`**, **`ucrtbased.dll`** |
| Embedded compile line (from PDB) | `-O2 -Oi -Oy- -GL ... -DNDEBUG -D_MBCS -MDd ... -std:c++20 -permissive-` |
| Project setting | `Code/TMSrv/TMSrv.vcxproj:80` `<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>` |

Microsoft ships debug CRT binaries under `VC\Redist\MSVC\<ver>\debug_nonredist` (components `Microsoft.VC<ver>.DebugCRT`, `.DebugMFC`, `.DebugOpenMP`) and excludes them from the "Distributable code files" list for Visual Studio 2022. A binary that imports `*D.dll` / `ucrtbased.dll` cannot legally be redistributed and cannot start on a clean host.

Additional consequences: debug heap (`_CrtDbgReport`, `_callnewh`, `_free_dbg` are imported), full LTCG (`/GL`) combined with a debug CRT, and mixed `NDEBUG` + debug-CRT iterators — an unsupported combination that changes container and iterator behaviour relative to `/MD`.

### C-2. Release configurations cannot reproduce the shipped binary (Critical)

* `Code/TMSrv/TMSrv.vcxproj:71-88` — `Release|Win32` has no `<AdditionalDependencies>`; `Debug|Win32` (`:67`) has `Winmm.lib;ws2_32.lib;kernel32.lib;...`.
* `Code/DBSrv/DBSrv.vcxproj:75-91` — `Release|Win32` has no `<AdditionalDependencies>` and no `<RuntimeLibrary>`; `Debug|Win32` (`:70`) has the full list.
* `WS2_32.lib` is required (`WSAStartup`, `socket`, `bind`, `listen`, `WSAAsyncSelect`, `recv`, `send`, `closesocket`) and `WINMM.lib` is required (`timeGetTime`). Neither is part of the MSBuild Win32 core library default (`Microsoft.Cpp.CoreWin.props:21`), which lists only `kernel32;user32;gdi32;winspool;comdlg32;advapi32;shell32;ole32;oleaut32;uuid;odbc32;odbccp32`.
* The shipped `.exe` *does* import `WS2_32.dll` and `WINMM.dll`, proving it was produced from a project state that is not in source control.

Result: the committed `Release|Win32` link step is expected to fail with unresolved externals, and no reviewer can rebuild the released artifact from HEAD.

### C-3. Winsock 1.1 API surface (High)

`Code/CPSock.cpp:113`:

```c
if (WSAStartup(MAKEWORD(1,1), &WSAData) != 0)
```

Microsoft's "Getting started with Winsock" guidance (updated 2026-07-17) states: *"Use Winsock 2.2 — always request version 2.2 in WSAStartup. Winsock 1.1 functions such as `gethostbyname` are deprecated and lack IPv6 support."* The project also relies on the legacy async model (`WSAAsyncSelect`, `Code/CPSock.cpp:163`) and includes `<Windows.h>` before any Winsock 2 header, which pulls the 1.1 `winsock.h` in TMSrv while `Code/DBSrv/Server.cpp:21` includes `<winsock2.h>` first. This is a mixed-header configuration inside one solution — the classic source of redefinition and structure-layout conflicts.

Runtime consequence: with 1.1 requested, the process receives a 1.1-limited `WSADATA` (e.g. `iMaxSockets`, `iMaxUdpDg` semantics), no IPv6, and the shipped binary imports all 17 `WS2_32` symbols **by ordinal**, which is brittle across SDK versions.

### C-4. Dead and duplicated link dependencies: ODBC (Medium)

* `Code/TMSrv/TMSrv.vcxproj:67` and `Code/DBSrv/DBSrv.vcxproj:70` both list `odbc32.lib;odbccp32.lib`.
* Zero ODBC headers (`sql.h`, `sqlext.h`, `odbcss.h`) and zero ODBC symbols (`SQLAllocHandle`, `SQLDriverConnect`, `SQLExecDirect`, ...) exist in the codebase.
* The packaged `TMSrv.exe` imports no `ODBC32.dll`.
* Because MSBuild already injects `odbc32.lib;odbccp32.lib` via `CoreLibraryDependencies`, the linker command line contains the pair twice (confirmed in `Code/TMSrv/Debug/TMSrv.tlog/link.command.1.tlog`).

The database layer is in fact **flat-file based** (`Code/DBSrv/CFileDB.cpp`), not SQL. ODBC is pure carry-over from the original vendor build, adding link-time noise and implying a database dependency that does not exist.

### C-5. Orphaned MFC dependency (Medium)

`Code/DBSrv/stdafx.h:21` includes `<afxwin.h>`. Verified facts:

* No file includes `stdafx.h` (repository-wide `#include` scan).
* `DBSrv.vcxproj` does not list `stdafx.h` in `<ClInclude>` and does not enable precompiled headers.
* MFC is not installed on the audit machine (`...\MSVC\14.44.35207\atlmfc\include\afxwin.h` does not exist), yet builds succeed — proving the include is never reached.
* `dumpbin /dependents` shows no `mfc140*.dll`.
* The `TMSrv.rc`/`DBSrv.rc` reference `afxres.h` only inside `APSTUDIO_INVOKED` TEXTINCLUDE blocks, which are not compiled.

Keeping this include means any future inclusion of `stdafx.h`, or any forced-include/pch change, instantly creates an unmanaged proprietary MFC dependency (distribution terms, DLL deployment, and a component the team does not install).

### C-6. Legacy MBCS CRT usage (Medium)

`Code/Basedef.cpp:32` includes `<mbstring.h>` and calls `_mbschr` at `:2922` and `:2928`. The project is compiled `/D_MBCS` with `<CharacterSet>MultiByte</CharacterSet>`. MBCS string handling is a legacy CRT area superseded by wide-character APIs and by the `_s` bounded variants; it also interacts badly with the file/CSV data path that uses fixed 128/256-byte stack buffers.

### C-7. Unsafe CRT function surface, with deprecation warnings suppressed (High)

Both projects define `_CRT_SECURE_NO_WARNINGS` (`TMSrv.vcxproj:61,79`; `DBSrv.vcxproj:61,83`), disabling MSVC's deprecation diagnostics for the exact functions that dominate the code:

| Function | Occurrences (source, excluding `Debug/` and `release/`) |
|---|---|
| `sprintf` | ~700+ across the tree; top files: `_MSG_UseItem.cpp` (145), `Server.cpp` TMSrv (104), `_MSG_Quest.cpp` (71), `Server.cpp` DBSrv (50) |
| `strcpy` | highest: `CNPCGene.cpp` (45), `Basedef.cpp` (21), `Server.cpp` TMSrv (14), `imple.cpp` (14) |
| `strcat` | `imple.cpp` (10), `Basedef.cpp` (9), `_MSG_CombineItemOdin.cpp` (8) |
| `memcpy` | `CFileDB.cpp` (41), `Server.cpp` TMSrv (20), `Basedef.cpp` (19) |
| unbounded `sscanf`/`fscanf` into fixed buffers | throughout `Basedef.cpp`, `DBSrv/Server.cpp`, `DBSrv/CReadFiles.cpp` |

`CPSock.cpp:457-464` additionally returns the packet to the caller **even when the checksum does not match** (`*ErrorCode = 1` but the buffer is returned), so integrity failures do not stop dispatch.

### C-8. Absence of any cryptographic dependency for credentials (High, functional/legal exposure)

`STRUCT_ACCOUNTFILE.Info.AccountPass` (16-byte field, `Code/Basedef.h:66`) is stored and compared verbatim:

* `Code/DBSrv/CFileDB.cpp:1302` — `strncmp(m->Password, pAccountList[Idx].File.Info.AccountPass, ACCOUNTPASS_LENGTH)`
* `Code/DBSrv/Server.cpp:596` — admin credentials read with `fscanf(fp, "%s %d %s %s", sip, &port, adminclientid, adminclientpass)`
* `Code/DBSrv/CReadFiles.cpp:1021,1028` — `sprintf(temp, "AccountName:%s AccountPass:%s filename:%s", id, pass, filename)` writes plaintext passwords to logs

There is no hashing, no KDF, and no crypto library in the dependency set. Any leaked `account/` folder or log file exposes every credential. This is not an "outdated library" issue but a missing-dependency issue that a dependency audit must surface.

---

## 3. Dependencies

Direct dependencies only. All versions verified against the local installation, the last build's compiler command line (`.tlog`), the PDB, the shipped binary's import table, and current Microsoft documentation.

### 3.1 Toolchain and platform components

| Dependency | Current Version | Latest Version | Status |
|---|---|---|---|
| MSVC Build Tools / Platform Toolset | `v143`, `14.44.35207` (VS 2022 17.14 GA) | `v145` / MSVC `14.50` (VS 2026); newest `v143` servicing builds are later `14.44.x` | Outdated (one major toolset behind) |
| Visual C++ compiler | 19.44.x (implied by 14.44.35207) | 19.50 (MSVC 14.50) | Outdated |
| Windows SDK (headers + import libs) | `10.0.26100.0` (resolved from `WindowsTargetPlatformVersion=10.0`; confirmed by last build include paths) | `10.0.28000.2957` | Outdated by one SDK series; also floating/unpinned |
| MSVC C++ Standard Library (STL) | 14.44 (v143) | 14.50 (v145) | Outdated (tied to toolset) |
| MSVC C Runtime / Universal CRT | `vcruntime140` + `ucrt` 10.0.26100 — **but debug variants `VCRUNTIME140D`/`ucrtbased` shipped** | `vcruntime140`/`ucrtbase` from 14.50 / SDK 10.0.28000 | Critical mismatch (debug CRT in release artifact) |
| Microsoft Foundation Classes (MFC) | Declared via `<afxwin.h>` only; **not compiled, not installed, not imported** | Ships with MSVC 14.50 (v145) | Orphaned / Legacy |
| Active Template Library (ATL) | **Not present** (no `atlbase.h`, `CAtl*`, no `atlmfc` usage) | n/a | Not present |
| Winsock (`ws2_32.lib` -> `WS2_32.dll`) | API level requested: **1.1**; header: `winsock.h` (TMSrv) | Winsock **2.2** | Legacy |
| ODBC (`odbc32.lib`, `odbccp32.lib`) | Linked from SDK 10.0.26100; **no API usage** | SDK 10.0.28000 import libs; runtime `odbc32.dll` = OS version | Orphaned / Unused |
| Winmm (`winmm.lib` -> `WINMM.dll`) | OS component; only `timeGetTime` used | OS component (no standalone versioning) | Legacy API (low-resolution timer), supported |
| Core Windows import libs (`kernel32`, `user32`, `gdi32`, `winspool`, `comdlg32`, `advapi32`, `shell32`, `ole32`, `oleaut32`, `uuid`) | SDK 10.0.26100 | SDK 10.0.28000 | Current for the installed SDK |
| MSVC MBCS CRT (`mbstring.h`, `_mbschr`) | 14.44 | 14.50 | Legacy (superseded by wide-char and `_s` APIs) |

### 3.2 Resolution details

| Component | Resolution mechanism | Consequence |
|---|---|---|
| Toolset | `PlatformToolset=v143` in all four configuration blocks (`TMSrv.vcxproj:28,35`; `DBSrv.vcxproj:28,35`) | Binds to the newest installed 14.44.x build; no `VCToolsVersion` pin (`MSBuild -getProperty:VCToolsVersion` = `14.44.35207`, i.e. whatever the machine has) |
| Windows SDK | `WindowsTargetPlatformVersion=10.0` (`TMSrv.vcxproj:21`, `DBSrv.vcxproj:21`) | Resolves to the highest installed 10.0.x. Machine has `10.0.19041.0` and `10.0.26100.0`; last build used **26100** (from `.tlog` include paths). A different machine can silently build against 19041. |
| Runtime library | Per-configuration `RuntimeLibrary`; `DBSrv` Release leaves it unset -> MSBuild default `/MD`; `TMSrv` Release sets `/MDd`; both Debug blocks set `/MTd` | Three different CRT linkage models across two projects and two configurations |

### 3.3 Non-source artifacts (noted, not treated as dependencies)

| Artifact | Location | Note |
|---|---|---|
| `TMSrv.exe` (509,440 bytes) | `Code/release/TMSrv/run/TMSrv.exe` | Prebuilt binary; not tracked by git. Imports debug CRT (see C-1). PE: x86, linker 14.44, subsystem 6.00, `DllCharacteristics=0x8140` (DYNAMIC_BASE, NX_COMPAT, TERMINAL_SERVER_AWARE), **no CFG**, `/GS` present in 118 functions, `/sdl` absent |
| `TMSrv.pdb` (2,854,912 bytes) | `Code/release/TMSrv/run/TMSrv.pdb` | Embeds absolute build paths (`E:\wyd\test-setup\w2pp\Code\TMSrv`, toolset path, full command lines) |
| Stale object files | `Code/TMSrv/Debug/DumpStruct.obj`, all `*.obj`, `*.tlog`, `*.ilk`, `*.idb` | `DumpStruct.cpp` no longer exists and is not in the project; `TMSrv.tlog` still references it |
| Orphan project file | `Code/ClientPatch_v7662/ClientPatch_v7662.vcxproj.user` | The `ClientPatch_v7662` project and `main.cpp` were deleted in commit `dfe35d0` (2026-11-14); only the user file remains and no `.vcxproj` exists |

---

## 4. Risk Analysis

| Severity | Dependency | Issue | Details |
|---|---|---|---|
| Critical | MSVC CRT (debug variant) | Debug CRT in release artifact | `TMSrv.exe` imports `MSVCP140D.dll`, `VCRUNTIME140D.dll`, `ucrtbased.dll`. Debug CRT is shipped under `debug_nonredist` and is excluded from the VS 2022 distributable list: the artifact is not redistributable and fails to start on a host without Visual Studio. Enabled by `TMSrv.vcxproj:80` (`MultiThreadedDebugDLL` = `/MDd` in `Release|Win32`). |
| Critical | MSVC Build Tools (`v143` Release config) | Release configuration is not buildable/reproducible | `Release|Win32` declares no `AdditionalDependencies` in either project, while the code needs `ws2_32.lib` and `winmm.lib` (not in the MSBuild core default set). The shipped `.exe` imports both DLLs, so it was built from a project state absent from HEAD. No lockfile, no CI, no build script. |
| High | Winsock 1.1 | Deprecated/legacy network API surface | `WSAStartup(MAKEWORD(1,1))` at `CPSock.cpp:113`. Microsoft: always request 2.2; 1.1 functions are deprecated and lack IPv6. Mixed headers: TMSrv pulls `winsock.h` via `<Windows.h>`; `DBSrv/Server.cpp:21` pulls `winsock2.h` first. Binary imports 17 `WS2_32` ordinals. |
| High | MSVC CRT (`sprintf`/`strcpy`/`sscanf`) | Memory-safety surface with warnings disabled | ~700+ `sprintf`, ~100 `strcpy`, ~40 `strcat`, plus unbounded `sscanf`/`fscanf` into fixed buffers, on network- and file-supplied data. `_CRT_SECURE_NO_WARNINGS` suppresses the compiler's deprecation diagnostics in all four configurations. |
| High | Packet integrity layer (`CPSock`) | Integrity check does not gate dispatch | `CPSock.cpp:457-464` returns the parsed message with `*ErrorCode = 1` instead of rejecting it, so a checksum failure does not stop `ProcessClientMessage` dispatch. |
| High | No crypto dependency | Plaintext credential storage and logging | `CFileDB.cpp:1302` compares passwords with `strncmp`; `DBSrv/Server.cpp:596` reads admin passwords with `fscanf("%s")`; `CReadFiles.cpp:1021` logs `AccountPass` verbatim. No hashing/KDF library is present. |
| Medium | ODBC (`odbc32.lib`, `odbccp32.lib`) | Unused, duplicated link dependency | Listed in both projects' Debug link inputs, injected again by MSBuild core defaults, imported by no binary, used by no source file. Falsely implies a SQL Server dependency; the real store is flat files. |
| Medium | MFC (`afxwin.h`) | Orphaned proprietary dependency | Included only in `DBSrv/stdafx.h:21`, which no translation unit includes. Not installed, not compiled, not imported. Carries licensing/deployment obligations for zero functionality; a future pch/forced-include change would break the build. |
| Medium | MSVC MBCS CRT (`mbstring.h`, `_mbschr`) | Legacy string API | `Basedef.cpp:2922,2928` with `/D_MBCS`. Superseded by wide-character and `_s` variants; tied to the 128/256-byte stack buffers used by the CSV loaders. |
| Medium | Windows SDK (`10.0` floating) | Non-reproducible SDK binding | `WindowsTargetPlatformVersion=10.0` resolves to whatever is installed (26100 here, 19041 also present, 28000 is current). Struct layouts, `#pragma pack` blocks and import libs can differ across machines. |
| Medium | `v143` toolset vs `v145` | One major toolset generation behind | MSVC 14.50 / `v145` ships with VS 2026; the newest `v143` servicing builds are later `14.44.x` than the installed `14.44.35207` (17.14 GA). VS 2022 17.14 remains supported through 2032-01-13. |
| Medium | Build mitigations | Hardening gaps in the shipped binary | PE `DllCharacteristics=0x8140`: ASLR and DEP present, **CFG absent**; PDB records `/sdl-`, `/analyze-`, `/WX-`; `TMSrv` Debug disables ASLR (`RandomizedBaseAddress=false`, `TMSrv.vcxproj:68`). `/GL` + `/MDd` is an unsupported optimization/CRT combination. |
| Low | Winmm (`timeGetTime`) | Legacy low-resolution timer | Only `WINMM.dll!timeGetTime` is imported. Supported OS API, but 1 ms resolution and 49.7-day wraparound; `QueryPerformanceCounter`/`GetTickCount64` are the current equivalents. |
| Low | Solution/project metadata | Stale VS 2015 markers, phantom x64 platforms | `.vcxproj` still declares `ToolsVersion="14.0"` while the toolset is `v143`; the solution exposes `x64` configurations that all map to `Win32` (`sln:22,25,28,31`), so an `x64` build silently produces 32-bit binaries. |
| Low | Licensing (project-level) | GPLv3 claim over decompiled proprietary code | `README.md:5,9,41` states the code is decompiled from a Hanbitsoft release and simultaneously distributes it under GPLv3. The redistributed MSVC runtime/MFC/SDK components carry Microsoft license terms; the debug CRT is explicitly non-redistributable. |

### 4.1 CVE status

No CVE identifier is asserted for the enumerated components. There is no package registry entry (npm/PyPI/Maven/nuget.org) for "MSVC toolset", "Windows SDK import library" or "Winsock 1.1"; Microsoft services these components through Visual Studio / Windows security updates rather than per-package CVE records. Fabricating CVE numbers for them would be incorrect.

What *is* actionable and verifiable:

* Toolset and SDK are behind current servicing (`14.44.35207` vs later `14.44.x` servicing and `14.50`; SDK `10.0.26100.0` vs `10.0.28000.2957`), so any MSRC fix shipped in those updates is absent.
* The debug CRT is on no supported deployment path at all (see C-1), independent of CVE state.
* The dominant exploitability drivers here are the project's own unsafe CRT usage and the integrity-bypass in `CPSock.cpp:457-464`, not third-party library CVEs.

---

## 5. Unverified Dependencies

| Dependency | Current Version | Reason Not Verified |
|---|---|---|
| MFC/ATL runtime (`mfc140*.dll`, `atl140*.dll`) | Not applicable (never compiled) | The MFC component is not installed on the audit machine, so no MFC binary version could be resolved. Status "unused" is verified; any MFC version claim would be unverified. |
| `v143` servicing patch level | `14.44.35207` (17.14 GA) | The toolset version is confirmed from the install and the PDB. The precise latest `14.44.x` patch build could not be confirmed: Microsoft's 17.14 release-notes page did not expose a complete, parseable release table on fetch. Confirmed separately: VS 2022 17.14 is supported through 2032-01-13, and `v145`/14.50 is the newest toolset. |
| ODBC runtime (`odbc32.dll`) | Not resolvable from repository | Runtime version is a property of the deployment OS, not of the project. The audit verified only that the import libraries are linked and that no ODBC symbol is referenced. |
| `WS2_32.dll` runtime | Not resolvable from repository | OS-provided; the *requested* API version (1.1) is verified, the deployed DLL version is not. |
| Winmm (`WINMM.dll`) runtime | Not resolvable from repository | OS-provided component with no independent version; only the imported symbol (`timeGetTime`) is verified. |
| `TMSrv.exe` build provenance | Linker 14.44, build timestamp 2026-11-06 17:25:48 | The binary's imports and PDB command line are verified, but the exact source revision used cannot be confirmed because no build metadata, CI record, or reproducible `Release` configuration exists. |

---

## 6. Critical File Analysis

Ranking criteria: dependency concentration (how many platform components a file binds), blast radius (include fan-in), handling of untrusted input, and business impact. All paths are relative to the project root.

| # | File | Size | Dependency pressure | Why critical |
|---|---|---|---|---|
| 1 | `Code/Basedef.h` | 60,163 B | 30 includers; 4 `#pragma pack` regions; `Windows.h` | Single definition point for every wire and disk structure |
| 2 | `Code/CPSock.cpp` | 18,324 B | Winsock 1.1; header conflicts; unbounded buffer math | Only network transport for both servers |
| 3 | `Code/TMSrv/Server.cpp` | 214,293 B | Winmm; Winsock; MFC-free; 104 `sprintf` | Game-server main loop and admin surface |
| 4 | `Code/DBSrv/CFileDB.cpp` | 65,686 B | Flat-file persistence; 41 `memcpy`; 33 `sprintf`; no SQL/ODBC | Sole account/character persistence engine |
| 5 | `Code/TMSrv/ProcessClientMessage.cpp` | 6,380 B | 66 includers of its header; Winsock dispatch | Single dispatch gate for all client traffic |
| 6 | `Code/TMSrv/_MSG_UseItem.cpp` | 160,607 B | 145 `sprintf`; `_CRT_SECURE_NO_WARNINGS` | Largest packet handler; item/effect economy |
| 7 | `Code/Basedef.cpp` | 161,052 B | MBCS CRT (`_mbschr`); 76 file-IO calls; 21 `strcpy` | Loads every data table (item, skill, guild, server list) |
| 8 | `Code/DBSrv/Server.cpp` | 50,485 B | Plaintext admin credentials; 50 `sprintf` | Admin/NP-tool control plane of the DB server |
| 9 | `Code/CPSock.h` | 3,565 B | 16 includers; hand-rolled `HEADER` layout | Wire contract shared with the W2 client build |
| 10 | `Code/TMSrv/_MSG_Quest.cpp` | 73,906 B | 71 `sprintf`; `_CRT_SECURE_NO_WARNINGS` | Largest state machine parsing client-supplied quest data |

### 6.1 `Code/Basedef.h` — the ABI anchor

Declares `APP_VERSION 6975`, the ports (`GAME_PORT 8281`, `DB_PORT 7514`, `ADMIN_PORT 8895`), array limits (`MAX_USER 1000`, `MAX_DBACCOUNT 10000`), and the account field widths (`ACCOUNTNAME_LENGTH 16`, `ACCOUNTPASS_LENGTH 16`, `EMAIL_LENGTH 48`). Four `#pragma pack(push, 1)` regions (`:808`, `:1212`, `:1465`, `:2063`) wrap the structures that are written to disk and sent over the socket. Every consumer of the platform dependency (Windows SDK types, `size_t`, `time_t`, alignment rules of `v143`) is funnelled through this header, and it is included by 30 of 104 source files. Any toolset or SDK change that alters fundamental type sizes or packing behaviour silently corrupts the account/character files and desynchronises the protocol. Highest dependency blast radius in the project.

### 6.2 `Code/CPSock.cpp` — Winsock 1.1 concentration

The only transport implementation (16 files include `CPSock.h`). `WSAInitialize()` at `:109-117` requests 1.1; `StartListen()` at `:128` binds and arms `WSAAsyncSelect`; `ConnectServer()` at `:176` implements a retry-by-port-bump scheme (`ConnectPort += 10`) that silently shifts the source port. `ReadMessage()` at `:380-467` casts raw receive-buffer offsets to `unsigned short`/`unsigned int` (`:390-395`), bounds-checks `Size` against `MAX_MESSAGE_SIZE`, decrypts in place, and then **returns the message even when the checksum fails** (`:457-464`). This file is where the legacy platform dependency (Winsock 1.1), the mixed-header risk, and the project's own validation weakness all meet.

### 6.3 `Code/TMSrv/Server.cpp` — game loop and admin surface

214 KB, the largest file in the repository, and the main window/message loop for the game server. Contains the admin command handlers (`IDC_MOBRELOAD`, `IDC_SAVEALL`, `IDC_SHUTDOWNNP` from `Basedef.h`), the per-second timers, and 104 `sprintf` calls into fixed `char` buffers (`:107`, `:148`, `:157`, `:3924` socket close path). It depends on Winmm indirectly through shared sources and on Winsock through `CPSock`. Any transport or CRT change lands here first; a fault here takes down every connected player.

### 6.4 `Code/DBSrv/CFileDB.cpp` — persistence with no database dependency

65 KB implementing the entire account/character store as flat files: `sprintf(temp, "./account/%s/%s", First, check)` at `:97` builds a path from an account-name supplied by a client, then `_open`/`_read`/`_write`. The password comparison at `:1302` is a plain `strncmp` over the 16-byte plaintext field. 41 `memcpy` calls move whole `STRUCT_ACCOUNTFILE`/`STRUCT_MOB` blocks in and out of the file cache. This file is the reason the ODBC link dependency is dead weight: all database work is here, on the filesystem, with no crash-safe journal and no integrity check.

### 6.5 `Code/TMSrv/ProcessClientMessage.cpp` — the dispatch gate

Small (6 KB) but its header (`ProcessClientMessage.h`) is included by 66 files, making it the project's routing hub: every `_MSG_*.cpp` and every subsystem depends on it. It maps packet `Type` values onto handlers, so a defect in the size/keyword handling in `CPSock::ReadMessage` propagates from here to all 50+ handlers. Dependency-wise it is the point where the Winsock 1.1 read path meets application logic; it also pulls in `<memory>` and the MSVC STL.

### 6.6 `Code/TMSrv/_MSG_UseItem.cpp` — largest handler, highest parse surface

160 KB, the largest single handler, with 145 `sprintf` calls and extensive item/effect branching (`ItemEffect.h`, 14 includers). It processes item-use packets from untrusted clients, resolves effect tables loaded by `Basedef.cpp`, and mutates player state. Its combination of unbounded formatting, fixed-size structures from `Basedef.h`, and suppressed CRT warnings makes it the most likely place where a crafted packet becomes memory corruption. It is also the file most exposed to any change in the CRT or in the packed structure layout.

### 6.7 `Code/Basedef.cpp` — data ingestion and MBCS CRT

161 KB, 76 file-IO calls (`fopen`/`fgets`/`sscanf`/`fprintf`), 21 `strcpy`, 19 `sprintf`, and the only MBCS CRT usage in the project (`#include <mbstring.h>` at `:32`, `_mbschr` at `:2922,2928`). It loads item lists (`:5062` reads 34 fields with one `sscanf`), skill tables, guild names, and the server list into global arrays; a malformed data file here corrupts server-wide state. Typical patterns like `sscanf(str, "%s", part1)` then `strcpy(g_pMessageStringTable[Index], cont)` (`:4706`) write unbounded data into fixed destinations.

### 6.8 `Code/DBSrv/Server.cpp` — DB control plane and credential handling

50 KB. Reads the admin/NP-tool allow-list and credentials: `fscanf(fp, "%s %d %s %s", sip, &port, adminclientid, adminclientpass)` at `:596`, and `fscanf(fp, "Sapphire %d\n", &Sapphire)` at `:404`. Handles admin logins (`:1732`), account state messages with plaintext passwords (`:1445`, `:1458`, `:1913`), and generates replacement passwords with `rand()` (`:1840-1844`). It is the administrative entry point to every stored account, and it depends on the same Winsock 1.1 transport with no additional authentication library.

### 6.9 `Code/CPSock.h` — the wire contract

Only 3.5 KB but defines `HEADER` (`short Size; char KeyWord; char CheckSum; short Type; short ID; unsigned int ClientTick`), `MAX_MESSAGE_SIZE 8192`, `RECV_BUFFER_SIZE`/`SEND_BUFFER_SIZE` 128 KB, and the `WSA_*` message constants. `HEADER` is **not** inside a `#pragma pack` region, so its size (12 bytes with default alignment on x86) depends on compiler layout rules and on the order/width of members. Because this structure is shared with the closed-source W2 client (`ClientPatch_v7662` was the client-side half and has been deleted from the repo), any toolset/alignment change breaks interoperability with a client that cannot be rebuilt. 16 files include it.

### 6.10 `Code/TMSrv/_MSG_Quest.cpp` — second-largest untrusted-input parser

72 KB with 71 `sprintf` calls, no bounded variants, and no compiler diagnostics (warnings suppressed). Quest state is driven by client packets and by quest data files, and the handler mutates persistent character state (`STRUCT_MOB`). It ranks tenth only because its dependency surface is the same CRT/Winsock set as the files above; its parse complexity is second only to `_MSG_UseItem.cpp`.

---

## 7. Integration Notes

### 7.1 How each dependency is consumed

| Dependency | Where it enters | Wiring | Coupling assessment |
|---|---|---|---|
| MSVC toolset `v143` | All four configuration blocks; last build resolved `14.44.35207`, `CL.exe` at `...\MSVC\14.44.35207\bin\HostX86\x86\CL.exe` | `PlatformToolset` only; no `VCToolsVersion` pin | Tight and unpinned: any 14.44.x patch on a build agent changes codegen. `/std:c++20 /permissive-` is applied to decompiled C-idiom code (`-Zc:strictStrings-` compensates). |
| Windows SDK | `<windows.h>`, `<windowsx.h>`, `<winsock2.h>`, `<ws2tcpip.h>`, `<io.h>`, `<fcntl.h>`, `<sys/stat.h>`, `<sys/timeb.h>` | `WindowsTargetPlatformVersion=10.0` (floating) | Tight and unpinned. `sys/timeb.h`/`_ftime` and `io.h`/`_open` are legacy CRT/SDK surfaces used by both servers for file timestamps and handles. |
| MSVC STL | `<vector>`, `<list>`, `<map>`, `<string>`, `<memory>` (`CNPCGene.cpp:22-25`, `SendFunc.cpp:19`) | Header-only, `/MD(d)` linkage to `MSVCP140(D)` | Low volume but ABI-sensitive: containers hold `STRUCT_*` PODs from `Basedef.h`; mixing `/MDd` objects with `/MTd` objects across the same binary is not permitted. |
| MSVC CRT / UCRT | Everywhere (`sprintf`, `strcpy`, `memcpy`, `fopen`, `_open`, `_mbschr`) | `MultiThreadedDebug` (Debug = `/MTd`), `MultiThreadedDebugDLL` (TMSrv Release = `/MDd`), unset (DBSrv Release = `/MD`) | **Three linkage models in one solution.** `DBSrv` and `TMSrv` cannot be linked together into one module under these settings, and the shipped TMSrv artifact requires debug CRT DLLs. |
| Winsock | `CPSock.cpp` (all socket work), `DBSrv/Server.cpp:21-22`, `CUser.cpp` (`closesocket`) | `ws2_32.lib` + `WSAStartup(1,1)`; TMSrv includes `<Windows.h>` before any `winsock2.h` | Tight: single transport class, no abstraction layer, no IPv6, no IOCP. Header order differs between projects. |
| Winmm | `timeGetTime()` in timers (`Server.cpp`, `ProcessSecMinTimer.cpp`) | `Winmm.lib` in Debug link inputs only | Tight but trivial; Release configuration omits the library. |
| ODBC | Not referenced by any source file | Import libraries only, duplicated by MSBuild defaults | Zero coupling to code, non-zero coupling to the link step. |
| MFC | `DBSrv/stdafx.h:21` only | None — header never included | Zero coupling; carries only licensing/installation risk. |
| ATL | Absent | n/a | n/a |
| `mfc140`/`atl140` DLLs | Not imported by `TMSrv.exe` | n/a | n/a |

### 7.2 Architecture-level dependency observations

* **Two binaries, one shared core, divergent CRT settings.** `Basedef.cpp`, `CPSock.cpp`, and `ItemEffect.h` are compiled into both `TMSrv` and `DBSrv` (`TMSrv.vcxproj:90-91`, `DBSrv.vcxproj:93-94`). Because the two projects use different `RuntimeLibrary` values and different Winsock header orders, the "same" shared sources are not built the same way twice.
* **Single points of failure (dependency view):**
  * `Code/CPSock.cpp` + `Code/CPSock.h` — the only transport; any Winsock/SDK change disables both servers simultaneously.
  * `Code/Basedef.h` — 30 includers; a packing or type-width change corrupts every persisted file and every packet.
  * `Code/TMSrv/ProcessClientMessage.h` — 66 includers; the dispatch contract for all gameplay handlers.
  * The MSVC debug CRT — if absent from a deployment host, `TMSrv.exe` does not start at all.
* **No dependency injection or abstraction boundary** exists around the platform: socket calls, file I/O, string formatting, and time are called directly. There are no forks, patches, or vendored overrides to audit, and consequently nothing to diff against upstream.
* **No supply-chain integrity controls:** no lockfile, no `VCToolsVersion` pin, no pinned SDK, no CI, no artifact checksum, and a committed-by-absence prebuilt binary whose build inputs are not in the repository. `git log` shows the toolchain move `v140`/SDK 8.1 -> `v143`/SDK 10.0 in `dfe35d0` (2026-11-14), preceded by removal of the `ClientPatch_v7662` project, and `ce3c9b2` (2026-11-15) removed a Themida protector configuration from the solution. No Themida/VMProtect reference remains in the tree.
* **Licensing chain:** the project claims GPLv3 (`license.txt`, `README.md:41`) over code it describes as decompiled from a proprietary Hanbitsoft release (`README.md:5,9`); each source header carries a GPLv3 notice with an empty copyright holder field (`Copyright (C) {2015} {VK, Charles TheHouse}`). Platform components add Microsoft license terms on top, and the debug CRT adds a hard redistribution prohibition.

### 7.3 Maintenance burden

The dependency set is small (13 directly declared components) but almost entirely unmanaged: 2 projects x 2 configurations each carry hand-maintained, divergent linker lists; the SDK and toolset are floating; and the Debug/Release runtime-library choices are inverted. Keeping this current requires manual, per-configuration edits in four places rather than a single manifest bump, which is why the shipped artifact already diverges from the committed project state.

---

## 8. Report Location

Saved to:

```
E:\wyd\test-setup\w2pp\docs\reports\dependency-auditor\dependencies-report-2026-11-15_21-55-42.md
```

---

## 9. Orchestrator Handoff

The dependency audit is complete and the report has been saved. No project file, source file, build configuration, or artifact was modified during this audit; all findings come from read-only inspection of the repository, the installed toolchain, the last build's `.tlog` files, the shipped PE import table, and external version verification against Microsoft documentation.

**Absolute path of the saved report:**

```
E:\wyd\test-setup\w2pp\docs\reports\dependency-auditor\dependencies-report-2026-11-15_21-55-42.md
```
