# Dependency Audit Report

**Project:** W2PP (legacy) — MMORPG private-server backend (C/C++)
**Audit scope:** `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy`
**Output location:** `docs/reports/dependency-auditor/`
**Audit date:** 2026-08-19
**Ignored folders:** `.git`, `.opencode`
**Ecosystems present:** C/C++ (native Windows), build via MSBuild/Visual Studio (.sln/.vcxproj). **No package manager** (no vcpkg/conan/CMake).
**Analysis mode:** Analysis and reporting only. No project files were modified.

---

## 1. Summary

The `legacy/` folder contains a single Visual Studio solution, **"W2PP Code Project.sln"**, with two C++ application projects:

- **`Code/TMSrv` (TMSrv.vcxproj)** — "TMSrv" (TeleMessage/Game server): the world/map server that maintains client connections, session state, and processes the game logic. ~50,800 lines across ~76 `.cpp` files (dominated by per-message handlers `_MSG_*.cpp`).
- **`Code/DBSrv` (DBSrv.vcxproj)** — "DBSrv" (Database server): manages accounts, characters and related data, communicating with TMSrv over TCP. Persistence is **file-based** (`CFileDB`), not a relational database.

The project is a **decompilation / re-implementation of "Polly's Server Release"** for the game **W2 (War of Destiny / WYD, by Hanbitsoft)**, derived from the Tantra/WYD server lineage, released for study purposes under **GPL-3.0**.

### Dependency inventory (direct, externally-visible)

This is a **native Windows project with no third-party open-source libraries**. All linked dependencies are **Microsoft platform components** (Windows SDK / C Runtime / Visual Studio toolset), plus one commercial build-time protection tool (Themida). There is **no package manager and no lockfile**, so dependency versions are determined by the installed Windows SDK and Visual Studio toolset rather than a manifest.

| Component | Role | Current | Status |
|-----------|------|---------|--------|
| Winsock 2 (`ws2_32.lib`) | TCP networking layer (`CPSock`) | Ships with Windows | Up to Date |
| Winmm (`Winmm.lib`, `timeGetTime`) | High-resolution timer for server tick loops | Ships with Windows | Up to Date |
| Windows SDK (`WindowsTargetPlatformVersion 10.0`) | OS API headers/libs | 10.0.x (installed) | Up to Date |
| Visual Studio toolset **v143** (VS2022) | Compiler/linker | VS 2022 (extended support) | Up to Date |
| Visual Studio **2015** (per README; ToolsVersion 14.0) | Original toolchain (superseded by v143 in project files) | — | **End of Life** |
| MFC headers (`afxwin.h`, `afxres.h`) | Referenced but not linked | — | **Incomplete / inconsistent** |
| ODBC (`odbc32.lib`, `odbccp32.lib`) | Linked but no SQL API used | Ships with Windows | **Dead dependency** |
| Themida (Release-Themida config) | Commercial anti-reverse-engineering protection | 3.2.6.0 | Up to Date (commercial license) |

### Main findings

1. **No third-party open-source runtime libraries** — the entire stack is OS/compiler-provided, which removes typical CVE-in-NPM/PyPI-style risk, but concentrates risk in the **Windows toolchain and custom networking/security code**.
2. **Custom packet "encryption"** (`pKeyWord[512]` key table in `CPSock.cpp`) implements a proprietary, non-standard, obfuscation-style scheme. This is not a validated cryptographic dependency (no OpenSSL/CryptoAPI/MD5/SHA use anywhere), and represents the largest single security exposure.
3. **Raw Winsock TCP with no TLS** — all server-to-server and server-to-client traffic is plaintext; credentials and billing data cross the wire unencrypted.
4. **Dead/legacy build coupling** — ODBC libraries are linked but never called; MFC headers are referenced (`stdafx.h`, `.rc` TEXTINCLUDE) but no MFC libraries are linked; the solution still exposes an outdated `Release-Themida` configuration and legacy `AnkhSVN/SubversionScc` source-control binding.
5. **No package manager / lockfile** — builds are not reproducible and depend on whatever SDK/toolset is installed on the build machine.
6. **GPL-3.0 licensing is declared but unverifiable for upstream game assets** — the README disclaims copyright to Hanbitsoft for the underlying game, which is a legal ambiguity (see Risk Analysis).

---

## 2. Critical Issues

| # | Severity | Component | Issue | Detail |
|---|----------|-----------|-------|--------|
| C1 | **Critical** | Custom packet crypto (`CPSock.cpp` `pKeyWord[]`) | Proprietary, weak, non-standard encryption | 512-byte hardcoded key table used to obfuscate packets. No use of a vetted crypto library (no CryptoAPI, OpenSSL, MD5, SHA). Reversable/weak by design; hardcoded static key in source. |
| C2 | **High** | Winsock 2 / network layer | Plaintext traffic, no TLS/authentication integrity | All client-server and TMSrv–DBSrv–Billing traffic is unencrypted raw TCP. Session/auth and billing data exposed to network sniffing/MITM. |
| C3 | **High** | Visual Studio 2015 (legacy toolchain) | End of support | Per Microsoft lifecycle: VS2015 mainstream ended 2020-10-14, **extended support ended 2025-10-15** — now unsupported. (Project files have already been migrated to v143/VS2022, but the README and `ToolsVersion 14.0` still reference the 2015 lineage.) |
| C4 | **High** | MFC linkage inconsistency | Referenced but not linked | `Code/DBSrv/stdafx.h` includes `<afxwin.h>` and `.rc` files declare `#include "afxres.h"` (TEXTINCLUDE) while **no MFC libraries are in `AdditionalDependencies`** — a latent build-environment fragility (fails if MFC not installed; undefined if MFC code were added). |
| C5 | **Medium** | ODBC libs | Dead dependency | `odbc32.lib;odbccp32.lib` are linked by both projects but **zero** `SQL*`/ODBC functions are referenced anywhere in the code. |
| C6 | **Medium** | No package manager / lockfile | Non-reproducible builds | No vcpkg/conan/CMake; binary output and behavior depend on the exact installed Windows SDK + toolset. |
| C7 | **Medium** | Themida commercial dependency | License/build cost + legal/commercial exposure | `Release-Themida` config implies use of Oreans Themida (commercial, ~EUR 249–499). Unlicensed use would violate its EULA; also complicates AV false-positive handling. |
| C8 | **Low/Medium** | Legacy source-control metadata | AnkhSVN/SubversionScc bindings in `.sln` | Stale SVN metadata (repo is on Git); harmless but indicates incomplete migration. |

---

## 3. Dependencies

> Note: This is a C/C++ native Windows project. "Versions" below refer to the platform/toolchain components consumed at build time, since no package manifest pins them.

| Dependency | Current Version | Latest Version | Status |
|------------|-----------------|----------------|--------|
| Winsock 2 (`ws2_32.lib`) | OS-provided (Win SDK 10.0) | OS-provided (10.0) | Up to Date |
| Winmm (`Winmm.lib`) | OS-provided | OS-provided | Up to Date |
| Windows SDK | `WindowsTargetPlatformVersion 10.0` | 10.0.x (latest installed) | Up to Date |
| Visual Studio toolset | **v143** (VS2022) | v143 (VS2022; VS2026 available) | Up to Date |
| Visual Studio 2015 (legacy) | ToolsVersion 14.0 | — (EOL) | **Legacy / End of Life** |
| MFC (`afxwin.h`, `afxres.h`) | referenced only | — | **Inconsistent** |
| ODBC (`odbc32.lib`, `odbccp32.lib`) | OS-provided | OS-provided | **Outdated (dead)** |
| Themida (build/protection) | — (Release-Themida cfg) | 3.2.6.0 (2026-08-11) | Up to Date |
| Standard CRT + STL (malloc, `std::map/list/vector/string`) | compiler-provided | compiler-provided | Up to Date |

### Version validation notes (external sources consulted)
- **Visual Studio lifecycle** — Microsoft Learn Lifecycle page for Visual Studio 2015: mainstream end **2020-10-14**, extended end **2025-10-15** (now passed). Visual Studio 2022 remains under extended support; Visual Studio 2026 (18.6.1, May 2026) is the current stable.
- **Themida** — Oreans product page: current release **3.2.6.0** (2026-08-11); Demo 3.2.4.0 (2025-09-08).
- **Winsock 2 / Windows SDK** — part of Windows; no independent release cycle.

---

## 4. Risk Analysis

| Severity | Dependency | Issue | Details |
|----------|------------|-------|---------|
| **Critical** | Custom packet crypto | Weak proprietary encryption | Hardcoded 512-byte key table (`CPSock.cpp`); no vetted crypto library; trivially reverse-engineered from source/binary. |
| **High** | Winsock 2 / CPSock | Plaintext, unauthenticated traffic | No TLS; credentials, auth and billing data transmitted in clear over TCP between clients, TMSrv, DBSrv and the billing server. |
| **High** | Visual Studio 2015 lineage | End of Life | Extended support ended 2025-10-15; no security patches for the original toolchain (mitigated by v143 migration in project files). |
| **High** | MFC headers | Incomplete linkage | `afxwin.h`/`afxres.h` referenced without MFC libs linked → fragile build; unknown if MFC runtime is intended. |
| **Medium** | ODBC libs | Dead dependency | `odbc32.lib;odbccp32.lib` linked but unused — dead weight in link line; misleading about DB connectivity. |
| **Medium** | No package manager | Non-reproducible / drift | No lockfile or manifest; builds depend on the machine's installed SDK/toolset. |
| **Medium** | Themida | Commercial licensing / build tooling | Protection step requires a paid license; unlicensed use violates EULA; AV false positives. |
| **Low** | GPL-3.0 + upstream assets | License compatibility ambiguity | Code is GPL-3.0, but README disclaims copyright of the underlying Hanbitsoft game; legality of distributing/reverse-engineering game protocol/assets is uncertain. |
| **Low** | SubversionScc/AnkhSVN metadata | Stale config | Solution still bound to SVN although repo is Git. |

---

## 5. Unverified Dependencies

| Dependency | Current Version | Reason Not Verified |
|------------|-----------------|---------------------|
| Windows SDK exact patch level | `10.0` (no exact version pinned) | No manifest/lockfile records the precise SDK; depends on installed build machine. |
| MFC runtime | unknown | Referenced but never linked; no version or library can be confirmed. |
| Themida build step | unknown | `Release-Themida` maps to the generic `Release` config in the `.sln`; the actual Themida wrapper/version used at build time is not recorded in the repo. |
| Upstream game client / protocol version | "latest client build" (per README) | No client artifact or protocol version identifier is pinned in the repository. |

---

## 6. Critical File Analysis

All paths are relative to the audit root `legacy/`.

1. **`Code/CPSock.cpp`** — Networking core and **custom packet encryption** (`pKeyWord[512]` key table). Every socket operation (connect, send, recv, accept) and the proprietary handshake (`INITCODE 0x1F11F311`) live here. Critical because the entire security posture of the server (both TMSrv and DBSrv use this class) hinges on this file. **Risk: Critical** (weak custom crypto, no TLS).

2. **`Code/CPSock.h`** — Defines the socket wrapper interface, buffer sizes, and the `_AUTH_GAME` handshake structure. Central contract consumed by both servers. Risk: High (security-relevant shared abstraction; single point of failure for all networking).

3. **`Code/TMSrv/Server.cpp`** (9,449 lines) — **TMSrv entry point** (`WinMain`), server bootstrap, message dispatch, client connection handling, and the link to DBSrv (port 7514) and the Billing server (`biserver.txt`). Critical as the primary integration point and the largest single dependency concentration in the project. Risk: High.

4. **`Code/TMSrv/ProcessClientMessage.cpp`** — Central router for all client message types (dispatches to the `_MSG_*.cpp` handlers). Any networking/parse defect propagates to all gameplay features. Risk: High.

5. **`Code/TMSrv/ProcessDBMessage.cpp`** — Handles messages exchanged with DBSrv (account/character data flow). Critical cross-server integration; depends on the plaintext TCP link to DBSrv. Risk: High.

6. **`Code/DBSrv/Server.cpp`** — **DBSrv entry point** and socket loop; receives connections from TMSrv and serves account/character data. Critical as the backend data authority. Risk: High.

7. **`Code/DBSrv/CFileDB.cpp`** (2,688 lines) — **File-based persistence layer** ("database"). All account/character/ranking data is read/written here (binary flat files, no SQL). Critical business data integrity + portability concern. Risk: High.

8. **`Code/DBSrv/CUser.cpp`** — Per-connection session handling on the DB side (`accept`/`closesocket`), authentication flows. Risk: High (auth data handling).

9. **`Code/Basedef.h`** — Shared foundational definitions/constants used by both servers. Dependency concentration point; changes ripple project-wide. Risk: Medium.

10. **`Code/TMSrv/imple.cpp`** — Administrative/misc logic including billing-server connectivity (`biserver.txt`) and admin account writes. Risk: Medium–High (billing/account surface).

---

## 7. Integration Notes

- **Winsock 2 (`ws2_32.lib`)** — Consumed exclusively through `CPSock`. Used for: client↔TMSrv connections, TMSrv↔DBSrv (port 7514), TMSrv↔Billing server (`biserver.txt`). No TLS anywhere.
- **Winmm (`Winmm.lib`)** — `timeGetTime()` drives the server tick/timer loops in `Code/TMSrv/ProcessSecMinTimer.cpp` and `Code/TMSrv/Server.cpp`.
- **Windows SDK / CRT / STL** — Standard headers (`windows.h`, `io.h`, `stdio.h`, `math.h`, `time.h`) and STL containers (`map`, `list`, `vector`, `string`) across all source. `_CRT_SECURE_NO_WARNINGS` suppresses deprecation warnings for unsafe CRT calls (`sprintf`/`strcpy` appear in **61 files**).
- **ODBC (`odbc32.lib`, `odbccp32.lib`)** — Linked by both projects but **no ODBC/SQL API is invoked**; the "DBSrv" is purely file-based. Dead dependency retained in the link line.
- **MFC (`afxwin.h`, `afxres.h`)** — Only `Code/DBSrv/stdafx.h` includes `<afxwin.h>`; `.rc` files reference `afxres.h` in TEXTINCLUDE. No MFC libraries are linked and no MFC classes are instantiated — an orphaned/incomplete dependency.
- **Themida** — `Release-Themida` appears in the `.sln` and maps to the standard `Release` config; indicates an intended post-build protection pass (commercial Oreans tool).
- **Custom crypto** — `pKeyWord[]` in `CPSock.cpp` is a hardcoded, proprietary obfuscation key table (no standard crypto library). It is a single point of failure for the whole security model.
- **Source-control metadata** — `.sln` carries `SubversionScc`/`AnkhSVN` bindings (stale; repo is Git).

---

## 8. Recommendations (report only — no code changes made)

1. **Replace the custom packet crypto with a vetted library or CryptoAPI/BCrypt** (highest priority). A hardcoded key table and proprietary scheme provide no real security and complicate any audit.
2. **Introduce TLS or an authenticated/encrypted channel** for all server-to-server (TMSrv↔DBSrv↔Billing) and client traffic; today credentials/billing data travel in plaintext.
3. **Complete the Visual Studio 2015 → v143 migration** already started in the project files; remove remaining `ToolsVersion 14.0`/2015 references and update the README. Note VS2015 is out of support.
4. **Resolve the MFC inconsistency**: either link the MFC libraries and use them, or remove `afxwin.h`/`afxres.h` references so the build does not depend on MFC being installed.
5. **Remove the dead ODBC link dependencies** (`odbc32.lib`, `odbccp32.lib`) from both `.vcxproj` link lines, or implement real ODBC/SQL usage if DB-backed storage is intended.
6. **Adopt a package/component manager** (vcpkg or Conan) and record the exact Windows SDK + toolset so builds are reproducible.
7. **Confirm Themida licensing** and record the exact version/wrapper used for the `Release-Themida` config; ensure it is not applied to open-source GPL deliverables in a way that conflicts with the license.
8. **Review GPL-3.0 compliance** given the disclaimed Hanbitsoft game assets; confirm the project's distribution model is legally sound.
9. **Remove stale SVN/AnkhSVN solution metadata** and the legacy `Release-Themida|x64` mapping (which currently collapses to Win32 `Release`).

---

## 9. File Saved

The full report has been saved to:

```
docs/reports/dependency-auditor/dependencies-report-2026-08-19 17:13:23.md
```

(Relative to the repository root `/home/luisdias/dev/github/luisdiasdev/w2pp`.)

---

## 10. Limitations

- No package manager or lockfile exists, so dependency versions are OS/toolset-provided rather than pinned by a manifest; exact SDK patch levels could not be verified.
- No internet-accessible CVE database could be consulted for the (platform-provided) components; no third-party open-source libraries are present that would be tracked by NVD-style databases.
- Themida build step and the upstream game client/protocol version are not recorded in the repository and remain unverified.
- Audit is analysis-only; no project files were modified.
