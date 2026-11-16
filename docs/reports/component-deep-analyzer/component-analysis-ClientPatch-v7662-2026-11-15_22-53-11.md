# Component Deep Analysis Report

**Component:** ClientPatch_v7662 (source absent)
**Analyzed path:** `Code/ClientPatch_v7662/ClientPatch_v7662.vcxproj.user`
**Repository:** W2PP — decompiled C/C++ Win32 MMORPG server emulator (MSVC)
**Analysis baseline:** `HEAD` = `bbf8d36cdfcb94f59132e1911756baef8ce65c03` (2026-11-15 22:06:44 -0300, `feat/milestone1`)
**Analysis date:** 2026-11-15 22:53:11
**Excluded from analysis:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Report type:** Documented absence

---

## 1. Executive Summary

ClientPatch_v7662 is listed in `README.md:19` as "DLL to unlock Client", positioned between the `BISrv` and `DBSrv` entries of the project content list. In the analysed baseline it is not a component: there is no project file, no source file, no header, no resource, no link against any other module, and no build target. The directory `Code/ClientPatch_v7662/` contains exactly one file, `ClientPatch_v7662.vcxproj.user` (168 bytes, `LastWriteTime` 2026-04-30 22:24:10, SHA-256 `C06A75B13F855A94D46616796E024C52B499F8F92CF00CCB571DDBC6FF574676`), and that file is an empty MSBuild user-properties stub whose entire body is `<PropertyGroup />` (`.vcxproj.user:3`).

Three independent facts establish the absence rather than a partial-presence:

1. `ClientPatch_v7662.vcxproj.user:1-4` is well-formed XML with a single empty `PropertyGroup`. It declares no `DebuggerWorkingDirectory`, no `LocalDebuggerCommand`, no launch profile and no environment. It therefore carries zero behavioural information.
2. `W2PP Code Project.sln:6-8` declares only two buildable projects, `DBSrv` and `TMSrv`, plus an empty "Solution Items" folder. There is no `ClientPatch_v7662` project entry, no project GUID for it, and no configuration mapping for it in `sln:19-31`.
3. The file is **not tracked by git** and is **explicitly ignored**: `git status --ignored` reports `!! Code/ClientPatch_v7662/`, and `git check-ignore -v` resolves it to `.gitignore:6 (*.user)`. `git ls-tree HEAD -- Code/ClientPatch_v7662` returns nothing. The directory is local working-tree residue (an artefact of Visual Studio opening a project that has since been deleted), not repository content.

The source that once backed this README entry is recoverable from git history, not from the working tree: commit `dfe35d0` (2026-11-14 13:41:49 -0300, "feat: upgrade solution to visual studio 2022 (v143)"), which **is** an ancestor of the current `HEAD`, deleted `ClientPatch_v7662.vcxproj` (121 lines), `ClientPatch_v7662.vcxproj.filters` (18 lines) and `main.cpp` (53 lines). An earlier removal of `main.cpp` alone exists on an unmerged branch (`2f78f42`, 2026-08-19, `origin/feat/project-analysis`). The original addition was `c52e5ed` (2016-07-07, "Client v7662 patch included.").

Key findings, stated plainly:

* **No business rules are derivable from the component as it exists.** Not "few" — none. There is no executable statement in the component directory.
* **No endpoints, no integrations, no design patterns, no algorithms, no configuration, and no tests** can be attributed to the current component.
* **Coupling is zero in both directions.** No tracked `.cpp`, `.h`, `.sln` or `.vcxproj` file references ClientPatch_v7662; the only textual mentions are the README description and prior analysis documents under `docs/reports/`.
* **The README overstates the buildable surface.** Of the twelve content entries listed in `README.md:17-28`, only `DBSrv` and `TMSrv` have projects in the solution.
* Historical behaviour (what the deleted DLL did) is summarised in the Appendix, explicitly labelled as historical and out of scope for the current component.

---

## 2. Data Flow Analysis

**N/A — no data-flow path exists.**

The component has no entry point, no message handler, no persistence layer, no response formatting and no code of any kind. There is no first step and no last step to trace.

```
(None)
```

For completeness, the absence was verified at the only place a flow could originate. The single file present is an MSBuild user-properties document consumed by Visual Studio's debugger configuration at project-load time; with an empty `PropertyGroup` it produces no effect on any process, file or network endpoint. There is no `.vcxproj` to bind the `.user` file to, so even that load-time consumption path is inert: MSBuild's `.user` import happens inside a project evaluation that cannot start.

---

## 3. Business Rules & Logic

**No business rules are derivable from this component. This is an explicit negative finding, not an omission.**

The component contains no conditional logic (other than the XML declaration), no constants, no validation, no state machine, no domain vocabulary and no configuration values. A business rule requires at least one decidable predicate over domain data; the only file present contains `<PropertyGroup />`, which evaluates to nothing.

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| — | None derivable | — |

### Detailed breakdown of the business rules

None. There is nothing to break down.

To make the negative finding auditable rather than assertive, the artefacts that could carry a business rule were each checked and each came back empty:

| Potential rule carrier | Check performed | Result |
|------------------------|-----------------|--------|
| Source logic | Recursive listing of `Code/ClientPatch_v7662/` | 1 file, `.vcxproj.user` only; no `.cpp`, `.c`, `.h`, `.def`, `.asm` |
| Build-time policy (defines, output name, subsystem, target type) | Read `.vcxproj.user:1-4` | Single empty `PropertyGroup`; zero properties |
| Debug/launch policy (working directory, command line, environment) | Read `.vcxproj.user:1-4` | No debugger properties present |
| Contract with the server | `Select-String` for `xtrap`, `211.115.86.66`, `0x5D9491`, `0x5D8E8C`, `Build\Client` over all `*.cpp`, `*.h`, `*.md`, `*.sln`, `*.vcxproj`, `*.rc` | Zero matches anywhere in the tracked tree |
| Project-level rules | `W2PP Code Project.sln:6-8`, `:19-31` | Only `DBSrv` and `TMSrv` are declared; no ClientPatch entry to carry rules |

Confidence in this negative finding: **high**. It rests on file-system enumeration, a full-text scan of the tracked tree and direct reading of the solution file — not on inference.

What a business rule *would* need, and what is missing, for the README claim ("DLL to unlock Client") to become analyzable: an exported or `DllMain`-based entry point, the target client build it patches, the specific checks or protections it neutralises, and the observable effect on the client-server handshake. None of these are present. The historical revision addressed two of them (see Appendix, H-2); the current component addresses none.

---

## 4. Component Structure

```
Code/ClientPatch_v7662/
└── ClientPatch_v7662.vcxproj.user    # 168 bytes; empty MSBuild user-properties stub; untracked + git-ignored
```

That is the complete structure: one file, one directory, no subdirectories.

| Element | Status | Evidence |
|---------|--------|----------|
| `ClientPatch_v7662.vcxproj` | **Absent** | deleted in `dfe35d0`; not in `git ls-tree HEAD`; not in `W2PP Code Project.sln` |
| `ClientPatch_v7662.vcxproj.filters` | **Absent** | deleted in `dfe35d0` |
| `main.cpp` (or any translation unit) | **Absent** | deleted in `dfe35d0` (also in `2f78f42` on `origin/feat/project-analysis`) |
| Headers, `.def`, `.rc`, `.ico`, `.asm` | **Absent** | recursive listing returns a single file |
| `ClientPatch_v7662.vcxproj.user` | **Present, untracked, ignored** | `git status --ignored` → `!! Code/ClientPatch_v7662/`; `.gitignore:6` matches `*.user` |
| Build output (`*.dll`, `Build/Client/`) | **Absent** | no `Build` directory in the repository; `Code/release/` holds only `TMSrv/run/TMSrv.exe` and `TMSrv.pdb` |

Sibling layout for context (the component's directory is a leaf among otherwise-populated siblings):

```
Code/
├── ClientPatch_v7662/     # 1 ignored file, no project, no source
├── DBSrv/                 # DBSrv.vcxproj + 7 .cpp + headers
├── release/TMSrv/run/     # prebuilt TMSrv.exe + PDB
├── TMSrv/                 # TMSrv.vcxproj + ~70 .cpp + headers
├── Basedef.cpp            # shared rules engine
├── Basedef.h              # shared contract header
├── CPSock.cpp / CPSock.h  # transport
└── ItemEffect.h           # effect ID contract
```

---

## 5. Dependency Analysis

**Internal dependencies: none.**

```
ClientPatch_v7662 → (no outgoing edges)
```

No file in the component includes, imports, links or references any other module. The component is not included in `W2PP Code Project.sln` (`sln:6-8`), so the solution's build order does not involve it either.

**External dependencies: none.**

| Dependency | Evidence of absence |
|------------|---------------------|
| Windows SDK / MSVC toolset | No `.vcxproj` to pin `PlatformToolset` or `WindowsTargetPlatformVersion`; the `.user` stub declares neither |
| Winsock / `ws2_32` / `user32` | No `#include`, no `.cpp` |
| Third-party libraries (Themida, VMProtect, injectors) | `Select-String` for `xtrap`/protector terms across the tracked tree returns zero hits; `ce3c9b2` removed the last Themida configuration from the solution |
| `Basedef.h`, `CPSock.h`, `ItemEffect.h` (shared contracts) | Not referenced by this component; the 16 files that include `Basedef.h` belong to TMSrv/DBSrv |

**Reverse dependency check (who depends on this component).**

| Seeker | Scope | Result |
|--------|-------|--------|
| `W2PP Code Project.sln` | full file | no ClientPatch project reference |
| `*.vcxproj`, `*.props`, `*.targets` | all three project trees | no reference |
| `*.cpp` / `*.h` (TMSrv, DBSrv, shared) | full tree | no reference |
| `README.md` | full file | one descriptive mention (`README.md:19`) — prose, not a dependency |
| `docs/reports/**` | full tree | analysis prose only (architectural report, dependency audit, sibling component reports) |

There is therefore no compile-time, link-time or runtime dependency graph to draw. This matches the architectural report's classification "Unbuilt / Placeholder" with afferent 0 and efferent 0.

---

## 6. Afferent and Efferent Coupling

Coupling unit here is the component directory treated as a single build unit (its intended granularity in C/C++/MSVC: one DLL project). No class, struct or function exists inside it to measure at a finer grain.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| ClientPatch_v7662 | 0 | 0 | None (unbuilt / placeholder) |

**Afferent = 0, verified three ways:** absent from the solution's project list (`sln:6-8`); no textual reference from any tracked `.cpp`/`.h`/`.vcxproj`/`.props`/`.targets`; not present in `git ls-tree HEAD` at all.

**Efferent = 0, verified by construction:** the only file present is a 4-line XML stub with an empty `PropertyGroup`; there is no import, include, link input or path reference to anything.

**Cohesion note:** cohesion is undefined for an empty unit. The directory's name is the only surviving signal of intended purpose.

---

## 7. Endpoints

**N/A.** The component exposes no REST, GraphQL, gRPC, Winsock, named-pipe, COM or exported-DLL surface. There is no module-definition file, no `.def`, no `extern "C"` declaration, and no header — and no binary to inspect for an export table (`Code/release/` contains only `TMSrv.exe`/`TMSrv.pdb`). The server-side message table in `CPSock.h`/`Basedef.h` is unrelated to this component.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| — | N/A | No integration exists in the current component | — | — | — |

The README's stated purpose implies a client-process boundary (a DLL loaded into the W2 client), but the current artifact participates in no such boundary: it is not built, not deployed, and not listed in any solution configuration.

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| — | N/A | — | No code exists to exhibit a pattern |

No architectural decision is expressed by the current artifact. The only architectural statements that can be made are negative and verifiable: the component is not a solution member, contributes no build target, and has no output directory configured.

Historical note (labelled, see Appendix H-1): the deleted revision used no design pattern at all — a bare `DllMain` plus two functions that each wrote one byte sequence to a fixed image address. That is a direct-memory-patch style, not an abstraction, and it is not part of the analysed component.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Repository documentation | `README.md:19` advertises "DLL to unlock Client" as project content, but no project, source or build target exists; the README lists 12 components while `W2PP Code Project.sln:6-8` builds 2 | A reader or new contributor cannot distinguish shipped from aspirational content; the documented surface exceeds the buildable surface |
| High | Component directory | The sole file is **untracked and git-ignored** (`.gitignore:6`; `git status --ignored` → `!! Code/ClientPatch_v7662/`) | The directory vanishes on a clean clone; any analysis or tooling that expects the component to exist is non-reproducible across machines |
| Medium | Protocol compatibility evidence | With the client-side half removed, nothing in the repository can validate client/server interop assumptions (the shared `HEADER` struct in `Code/CPSock.h` is consumed by a closed-source client that cannot be rebuilt here) | Layout or toolchain drift in the shared contract cannot be cross-checked against the counterpart that used to exist |
| Medium | History archaeology | The component was removed twice on divergent lines: `2f78f42` (2026-08-19, `origin/feat/project-analysis`) deleted `main.cpp`, and `dfe35d0` (2026-11-14, ancestor of `HEAD`) deleted the project, filters and `main.cpp` together | Determining "when the component stopped existing" depends on which branch is inspected; both removals are silent (no README update, no migration note) |
| Low | Dead directory | An empty-`PropertyGroup` `.vcxproj.user` persists with no `.vcxproj` to bind to | Cosmetic residue; Visual Studio may re-create or delete it unpredictably depending on local state |
| Low | Documentation-only trace | The only surviving description is one sentence and one version number in the directory name (`v7662`) | The meaning of `v7662` relative to the server's `APP_VERSION` (6975) is undocumented; the two numbering schemes cannot be reconciled from the repository |

No security vulnerabilities, resilience gaps, performance bottlenecks or code smells are attributable to the component, because there is no code. The historical revision's security-relevant properties (fixed absolute addresses, a hardcoded private IP literal) are recorded in the Appendix as history, not as current risk.

---

## 11. Test Coverage Analysis

**No tests exist for this component. No test infrastructure exists in the repository.**

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| ClientPatch_v7662 | 0 | 0 | N/A (no code to cover) | N/A |

Evidence, gathered across the whole project root with the ignore list applied:

| Check | Method | Result |
|-------|--------|--------|
| Test directories | recursive directory scan for names `test`, `tests`, `__tests__`, `spec`, `specs`, `unit`, `unittest`, `qa` | zero matches |
| Test files | recursive file scan for `*_test*`, `test_*`, `*.test.*`, `*.spec.*`, `*Test(s).cpp/.h/.c` | zero matches |
| Test frameworks | `Select-String` over all `*.vcxproj`, `*.sln`, `*.props`, `*.targets`, `*.cpp`, `*.h` for `gtest`, `gmock`, `Catch2`, `cppunit`, `doctest`, `boost/test`, `Microsoft.VisualStudio.TestTools`, `NUnit` | zero matches |
| Test projects in solution | `W2PP Code Project.sln:6-8` | two application projects and an empty solution folder; no test project |
| CI harness | no CI configuration in the tree (no `.github/workflows`, no pipeline definition) | zero matches |
| Test files for the deleted component | `git log --all -- "Code/ClientPatch_v7662"` | five commits, all of them project/source/licence/locale changes; no test file ever existed in the directory (`git ls-tree -r dfe35d0^` lists exactly 3 files) |

The last row is the strongest statement available: even in the revision where the component was complete, it consisted of one 53-line translation unit with no tests. Test coverage for this component has been zero for the component's entire recorded lifetime.

---

## 12. Analysis Method, Evidence Index and Ambiguities

### Method

1. Enumerated `Code/ClientPatch_v7662/` recursively (found 1 file) and read it in full.
2. Read `README.md`, `W2PP Code Project.sln`, `.gitignore` and `.gitattributes`.
3. Whole-tree `Select-String` for `ClientPatch` (24 hits, all in `README.md:19` and prior analysis reports under `docs/reports/`), and for `xtrap`, `211.115.86.66`, `0x5D9491`, `0x5D8E8C`, `Build\Client` (zero hits in the tracked tree).
4. Established tracking/ignore status with `git ls-files`, `git check-ignore -v`, `git ls-tree HEAD`, `git status --ignored`.
5. Reconstructed provenance with `git log --all -- "Code/ClientPatch_v7662"`, `git show --stat dfe35d0`, `git show --stat 2f78f42`, and `git merge-base --is-ancestor dfe35d0 HEAD` (exit 0 — the deletion is in the current lineage).
6. Searched the whole repository for test artefacts and test-framework references (all empty).
7. No file was modified; no build or test was executed.

### Evidence index

| Ref | Fact | Source |
|-----|------|--------|
| E-1 | Component directory holds exactly one file, 168 bytes, SHA-256 `C06A75B1…676` | recursive listing + `Get-FileHash` |
| E-2 | The file is a 4-line MSBuild stub with `<PropertyGroup />` and nothing else | `.vcxproj.user:1-4` |
| E-3 | README describes the component as "DLL to unlock Client" | `README.md:19` |
| E-4 | Solution builds only `DBSrv` and `TMSrv` | `W2PP Code Project.sln:6-8`, `:19-31` |
| E-5 | The file is untracked and ignored by `*.user` | `.gitignore:6`; `git status --ignored`; `git check-ignore -v` |
| E-6 | The directory does not exist in `HEAD`'s tree | `git ls-tree HEAD -- Code/ClientPatch_v7662` (empty) |
| E-7 | Project, filters and `main.cpp` deleted in `dfe35d0` (2026-11-14), an ancestor of `HEAD` | `git show --stat dfe35d0`; `git merge-base --is-ancestor` exit 0 |
| E-8 | `main.cpp` was also deleted in `2f78f42` on unmerged `origin/feat/project-analysis` | `git show --stat 2f78f42` |
| E-9 | Component originally added 2016-07-07 | `c52e5ed "Client v7662 patch included."` |
| E-10 | No server-side term (`xtrap`, patch addresses, hardcoded IP) survives anywhere in the tracked tree | whole-tree `Select-String` → 0 hits |
| E-11 | No tests and no test framework in the repository | directory/file/framework scans → 0 hits |
| E-12 | `HEAD` = `bbf8d36`, 145 tracked files | `git log -1`; `git ls-files` count |
| E-13 | Architectural report classifies this component as "Unbuilt / Placeholder", afferent 0, efferent 0 | `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:170` |

### Ambiguities and limits of the analysis

| # | Question | Status |
|---|----------|--------|
| A-1 | What exactly the DLL unlocked, and which client checks it bypassed | **Cannot be established** from the current component. No source, no documentation, no server-side counterpart reference |
| A-2 | Whether the `.vcxproj.user` on disk corresponds to the deleted project or to a newer, never-committed project | **Cannot be established.** The file's `ToolsVersion="Current"` differs from the deleted project's `ToolsVersion="14.0"`; its mtime (2026-04-30) predates the deletion commit (2026-11-14). Both observations are consistent with several histories, none verifiable |
| A-3 | Why the component was deleted | **Not documented.** Commit `dfe35d0`'s message describes a toolchain upgrade; the deletion is an unremarked side effect. No ADR or changelog entry exists |
| A-4 | Relationship between `v7662` (directory name) and the server's `APP_VERSION` 6975 | **Unknown.** No source comment links the two numbering schemes |
| A-5 | Whether the component was ever built and shipped | **Not determinable.** No DLL exists in the repository and no output path is configured; the historical `OutDir` (`$(SolutionDir)Build\Client\`) is absent |
| A-6 | Runtime directory layout / deployment expectations | **Not determinable** — no deployment documentation or script references the component |

---

## Appendix — Historical Evidence (NOT part of the analysed component)

Everything in this appendix describes files that **do not exist** in the analysed baseline. It is included because the component cannot be discussed at all without it, and it is separated so that no historical statement is mistaken for a current finding. The content of the deleted files was read from `dfe35d0^` (the tree immediately before the deletion commit).

### H-1 Deleted file set

| File | Lines | Nature |
|------|-------|--------|
| `Code/ClientPatch_v7662/main.cpp` | 53 | Single translation unit; GPL v3 header (Copyright 2015, "VK, Charles TheHouse"); includes `<Windows.h>` only |
| `Code/ClientPatch_v7662/ClientPatch_v7662.vcxproj` | 121 | MSBuild project, `ToolsVersion 14.0`, toolset `v140`, SDK `8.1`, `CharacterSet MultiByte`, GUID `{388B5CEE-6842-42F1-B192-98B2DBB559EF}` |
| `Code/ClientPatch_v7662/ClientPatch_v7662.vcxproj.filters` | 18 | Two filters ("Source Files", "Header Files"); only `main.cpp` mapped |

### H-2 Historical structure and the two rules it encoded

The deleted `main.cpp` defined three functions and no other symbols — `strip_xtrap()`, `strip_odd_connection_request()` and `DllMain(HINSTANCE, DWORD, LPVOID)`. `DllMain` on `DLL_PROCESS_ATTACH` called `VirtualProtect((void*)0x401000, 0x1F3000, PAGE_READWRITE, &old)` over the client's `.text` range, invoked the two patch functions, then restored the original protection; on `DLL_PROCESS_DETACH` it called `FreeLibrary(hInstDLL)`.

The two hardcoded patches encode the component's entire historical behaviour:

| Historical rule | Statement in the deleted source | Effect |
|-----------------|--------------------------------|--------|
| H-R1 XTrap neutralisation | `*(char*)0x5D9491 = 0xEB;` — write `JMP` opcode at absolute client address `0x5D9491` | Skips the client's XTrap integrity/anti-tamper routine |
| H-R2 Odd connection request suppression | `*(short*)0x5D8E8C = 0x9090;` — write two `NOP`s at absolute client address `0x5D8E8C`, with the source comment "This weird connection request to 211.115.86.66:2424 that always fails make the client laggy. This patch kill the call." | Removes a lag-inducing failing outbound connection attempt from the client's startup/login path |

Both rules are stated here as **historical code content**, not as business rules of the analysed component. They are unenforceable in the baseline: no binary, no source, no build target.

Observable properties of that historical artefact, relevant only as archaeology:

* It was **address-bound to one client binary**: two fixed virtual addresses inside a fixed image range mean the DLL only functions against the exact W2 client build `v7662`. Any client rebuild moves both addresses and the patch writes to unrelated memory.
* It was **not portable to x64**: the addresses and the `VirtualProtect` range assume a 32-bit image loaded at its preferred base.
* Its **historical build configuration was internally inconsistent**: `ConfigurationType` was `DynamicLibrary` only for `Debug|Win32`; `Release|Win32`, `Debug|x64` and `Release|x64` were all `Application`, which for a `DllMain`-only translation unit would not produce a DLL (and would not link as an executable, since there is no `main`/`WinMain`). The project also set `OutDir $(SolutionDir)Build\Client\` and `TargetName $(ProjectName)_d` for `Debug|Win32` only. This is a property of a file that no longer exists and is recorded for completeness, not as a current defect.

### H-3 Historical timeline

| Commit | Date | Author | Change |
|--------|------|--------|--------|
| `c52e5ed` | 2016-07-07 05:03:12 -0300 | ptr0x | "Client v7662 patch included." — component added |
| `d82d8ee`, `ca8bce1` | — | — | "..." (locale/licence restructuring touches) |
| `f8f9013` | — | — | "License update" |
| `626e093` | — | — | "Update main.cpp" |
| `4e83014` | — | — | "Update to platform v141." |
| `2f78f42` | 2026-08-19 16:59:02 -0300 | Luis Dias | "chore: remove unused client patch files" — deleted `main.cpp` only; branch `origin/feat/project-analysis`, **not merged into `HEAD`** |
| `dfe35d0` | 2026-11-14 13:41:49 -0300 | Luis Dias | "feat: upgrade solution to visual studio 2022 (v143)" — deleted the project, filters and `main.cpp`; **ancestor of `HEAD`** |

Only the `.vcxproj.user` survived both removals, because `.gitignore:6` excludes it from version control and therefore from every `git rm`.

### H-4 What this appendix does not claim

It does not claim the DLL currently exists, builds, ships, or is required by `TMSrv`/`DBSrv`. The whole-tree scan found no server-side reference to XTrap, to the two patch addresses, or to the hardcoded IP (`E-10`), so no protocol-level contract with the server can be inferred from either side of the repository.

---

*Report generated by the component-deep-analyzer agent. Read-only analysis: no project file was modified, no build or test was executed, no refactoring is recommended. All paths are relative to the repository root `E:\wyd\test-setup\w2pp`.*
