# Component Deep Analysis Report

**Component:** Prebuilt Release Artifact Set (`TMSrv.exe` + `TMSrv.pdb`)
**Project:** W2PP (decompiled C/C++ Win32 MMORPG server emulator for W2)
**Analysis type:** Static, read-only artifact forensics (no execution, no modification)
**Analysis date:** 2026-11-15
**Analyzed paths (relative to project root):**

- `Code/release/TMSrv/run/TMSrv.exe` — 509,440 bytes
- `Code/release/TMSrv/run/TMSrv.pdb` — 2,854,912 bytes

**Excluded from scope per instructions:** `.agents`, `.codegraph`, `.opencode`, `openspec`

---

## 1. Executive Summary

The "Prebuilt Release Artifact Set" is not a code component. It is a checked-out (and git-ignored) pair of MSVC build outputs: a 32-bit Windows GUI executable for the game server `TMSrv` and its full private Program Database. Consequently:

- **No business rules, domain logic, validation rules or use cases are implemented by these files.** A binary encodes compiled instructions, not readable rules; the domain rules of the system live in `Code/TMSrv/*.cpp` and `Code/Basedef.h`. Section 3 therefore states N/A for domain business rules and instead documents the *build, provenance and deployment rules that the artifact itself encodes and proves*.
- The artifact is **internally consistent and complete**: the EXE's CodeView entry and the PDB's info stream carry the same GUID (`1ae429cb-d036-4e88-ae81-7a57e3b0ae66`, age 1), the exact same PE timestamp (`0x6AEE37CC`), and the PDB path embedded in the EXE (`E:\wyd\test-setup\w2pp\Code\release\TMSrv\run\TMSrv.pdb`) is the path where the PDB actually sits. The pair is a valid, non-orphaned debug build product.
- **The debug-CRT claim from the dependency audit is confirmed at the import-table level, not merely at the project-file level.** The import directory contains `MSVCP140D.dll` (4 imports), `VCRUNTIME140D.dll` (11) and `ucrtbased.dll` (55). No release CRT (`MSVCP140.dll`, `VCRUNTIME140.dll`, `ucrtbase.dll`, `api-ms-win-crt-*`) is imported. The link info stored inside the PDB shows a normal `/LTCG:incremental /OPT:REF /OPT:ICF /DEBUG` release link, so the debug CRT comes from `/MDd` (`MultiThreadedDebugDLL`), which `Code/TMSrv/TMSrv.vcxproj:80` still sets for `Release|Win32`.
- **The artifact does not correspond to the current working tree (HEAD).** It contains 76 compiled translation units including `DumpStruct.cpp`, a file that does not exist at HEAD and is present only on branch `test-setup`; HEAD's project lists 75 TUs. The artifact was produced with MSVC 14.44.35207 / Windows SDK 10.0.26100.0 (v143) on 2026-11-06 at 17:25:48 -03:00, which places it on the `test-setup` lineage (commits `9be4cf0` 16:39, `df36f19` 16:58, `42910d1` 17:14 on that same day), not on the v143 merge that HEAD received on 2026-11-14.
- **The artifact is not the output of the current solution layout.** The linker output path recorded in the PDB is `E:\wyd\test-setup\w2pp\Code\TMSrv\..\release\TMSrv\run\TMSrv.exe`, i.e. `$(ProjectDir)..\release\...`. With the current solution at the repository root, `$(SolutionDir)..\release\...` (`Code/TMSrv/TMSrv.vcxproj:49,53`) resolves to `E:\wyd\test-setup\release\TMSrv\run` — outside the repository. The recorded path is only reproducible if `$(SolutionDir)` resolved to the project directory at build time (building the `.vcxproj` outside the `.sln`). This folder is therefore a stale, orphaned output location.
- **Two significant behavioral exposures are compiled into this "release" image**: (a) the debug CRT, which is not redistributable through `VC_redist` and changes failure semantics (`_CrtDbgReport`, `_invoke_watson` are imported), and (b) the packet-fixture dumper from `DumpStruct.cpp`, whose ungated `fopen(..., "ab")` path appends every handled client packet — including login/account-secure packets — to `fixtures.ndjson` in the process working directory. The strings `fixtures.ndjson`, `fixtureType`, `PACKET`, `.git\HEAD`, `revision` and the `DumpStruct.cpp` `__FILE__` path are all present in the shipped image.
- **Security posture of the image:** ASLR (`DYNAMICBASE` + `.reloc`), DEP (`NXCOMPAT`), `/SAFESEH` (8 handlers), `/GS` cookie present, `asInvoker` manifest, no writable/executable sections, no overlay bytes (section sizes sum exactly to the file size), no packer signatures (the Themida configuration was removed from the solution in commit `ce3c9b2`, which postdates this build). Gaps: no Control Flow Guard (`GuardFlags = 0`), `CheckSum = 0`, **no digital signature** (no Certificate data directory), **no VERSIONINFO resource**, and **no embedded VCS revision**.
- **Memory layout anomaly worth flagging:** `.data` declares a virtual size of `0x0ABEB948` (180,355,400 bytes) against only 6,144 raw bytes, which drives `SizeOfImage` to `0x0AC6A000` (172.5 MiB) for a 0.5 MB file. This is consistent with the fixed global pools `CUser pUser[MAX_USER]` and `CMob pMob[MAX_MOB]` (`Code/TMSrv/Server.cpp:269-270`, `MAX_USER 1000` at `Code/Basedef.h:56`, `MAX_MOB 25000` at `Code/Basedef.h:107`), not with corruption.
- **No tests exist anywhere in the repository** for this or any other component (no test files, no test project in `W2PP Code Project.sln`, no CI configuration, zero `assert`/`static_assert` in the TMSrv sources), so measured test coverage of the artifact is 0% and cannot be computed from any harness.

---

## 2. Data Flow Analysis

Because the component is a build artifact, "data flow" has three distinct layers. All three are evidenced below.

### 2.1 Build-time flow (how the artifact was produced)

```
1. 76 translation units compiled by cl.exe 14.44.35207 (HostX86\x86) from
   Code/TMSrv/*.cpp (74) + Code/Basedef.cpp + Code/CPSock.cpp
2. Intermediate objects emitted to Code/TMSrv/TMSrv/Release/*.obj
   (paths recorded in the PDB, e.g. ...\TMSrv\Release\DumpStruct.obj)
3. /GL-style whole-program compilation feeds the linker
   (/LTCG:incremental /LTCGOUT:TMSrv\Release\TMSrv.iobj recorded in /LinkInfo)
4. link.exe 14.44.35207 emits:
      /OUT:...\Code\TMSrv\..\release\TMSrv\run\TMSrv.exe
      /PDB:...\Code\TMSrv\..\release\TMSrv\run\TMSrv.pdb
      /IMPLIB:...\release\TMSrv\run\TMSrv.lib
5. Resource TMSrv.rc compiles to TMSrv.res and is embedded (ICON 101, GROUP_ICON, MANIFEST)
6. CodeView RSDS record (GUID 1ae429cb-..., age 1) is written into the PE debug directory
   and the same GUID/age is written into PDB MSF stream 1
7. VC_FEATURE (entry 1), POGO (entry 2, 864 bytes) and ILTCG (entry 3, size 0) debug
   directories are written for the optimized/PGO profile bookkeeping
```

### 2.2 Load-time flow (what the Windows loader sees)

```
1. Loader maps PE32 image, preferred base 0x400000, 5 sections, SizeOfImage 0x0AC6A000
2. .data is mapped with 172.5 MiB virtual size but only 6,144 bytes of file backing;
   the remainder is zero-fill (the pUser[1000] / pMob[25000] pools)
3. Import resolution (8 modules, 139 functions):
      KERNEL32(21), USER32(24), GDI32(6), WINMM(1 ord), WS2_32(17 ord),
      MSVCP140D(4), VCRUNTIME140D(11), ucrtbased(55)
   Failure mode: "The code execution cannot proceed because ucrtbased.dll was not found"
   if the debug CRT is absent on the target host
4. Relocations applied if the image is not at 0x400000 (.reloc at RVA 0x0AC62000)
5. SafeSEH table registered (LoadConfig SEHandlerTable RVA 0x473504, 8 entries)
6. GS cookie initialised, CRT initialisers run (_initterm / _initterm_e imported)
7. Entry point 0x62050 reached; GUI subsystem startup path; USER32 message loop
   (RegisterClassA, ShowWindow, GetMessageA, TranslateMessage, SetTimer, PostQuitMessage)
```

### 2.3 Runtime flow implied by the image (no execution performed)

```
1. Process working directory resolved (SetCurrentDirectoryA imported)
2. Configuration/data files read relative to the executable working directory,
   visible as literal paths in .rdata: ../../Common/ItemList.bin, ../../Common/Settings/*.txt,
   ../../DBSRV/Run/Admin.txt, ./serverlist.txt, ./localip.txt, ./AttributeMap.dat,
   ./gameconfig.txt, ./Language.txt, ./NPCGener.txt, ./Guilds.txt, ./heightmap.dat, ...
3. WS2_32 sockets (17 imports, all by ordinal) accept client sessions
4. Client packets dispatched to _MSG_* handlers
5. DumpStruct::DumpPacket appends a JSON fixture line per handled packet to
   "fixtures.ndjson" (fopen mode "ab", no gate; reads .git\HEAD to stamp "revision")
6. Responses sent back through WS2_32; timer work through USER32 SetTimer/WINMM
7. Logging/telemetry to the *Teste.txt / Gethalf.txt / AttackDieTeste.txt diagnostic files
   whose names are present in .rdata
8. Failure paths: debug CRT assert/_CrtDbgReport/_invoke_watson
```

---

## 3. Business Rules & Logic

### Overview of the business rules

**Domain business rules: N/A.** The artifact set implements no readable business rule. All payment-free game rules (drop rates, combine success, exp, trade validation, guild war scoring, etc.) are expressed in `Code/TMSrv/*.cpp` and are only *compiled into* this image; they cannot be extracted as rules from the artifact without decompilation, which is out of scope and not required for this analysis.

What the artifact *does* encode, and what is fully verifiable here, is a set of **build, provenance and deployment rules**. They are documented below because they are the only rule-bearing content of this component.

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Build configuration | `Release\|Win32` links the debug CRT (`/MDd`) | `Code/TMSrv/TMSrv.vcxproj:80`; verified in PE import table |
| Build configuration | Whole-program optimisation with COMDAT folding and reference elimination | `Code/TMSrv/TMSrv.vcxproj:33,85-86`; verified via `/LinkInfo` in PDB |
| Build configuration | Output directory is `$(SolutionDir)..\release\$(ProjectName)\run\` | `Code/TMSrv/TMSrv.vcxproj:49,53`; contradicts PDB-recorded output path |
| Provenance | EXE and PDB are bound by one GUID + age pair | PE debug dir offset 0x71D9C; PDB MSF stream 1 |
| Provenance | Source identity is `E:\wyd\test-setup\w2pp\Code\...` (absolute, machine-bound) | `.rdata` `__FILE__` literals; PDB `/names` |
| Provenance | No VCS revision is embedded at build time; revision is read at runtime from `.git\HEAD` | Absence of revision literal; `fixtures.ndjson` schema strings |
| Execution envelope | GUI subsystem, `asInvoker`, no VERSIONINFO, unsigned | PE optional header; resource directory; data directory 4 = 0 |
| Runtime side effect | Every handled packet is appended to `fixtures.ndjson` | `DumpStruct.cpp` (branch `test-setup`); strings in image |
| Memory envelope | Fixed static pools sized at compile time | `Code/TMSrv/Server.cpp:269-270`, `Code/Basedef.h:56,107`; `.data` virtual size |

### Detailed breakdown of the business rules

---

### Business Rule: Release configuration produces a debug-CRT image

**Overview**:
The project's `Release|Win32` configuration resolves the C/C++ runtime to `/MDd` (`MultiThreadedDebugDLL`). The shipped artifact is the direct consequence of that setting: it imports the three debug CRT DLLs and no release CRT.

**Detailed description**:
`Code/TMSrv/TMSrv.vcxproj:80` sets `<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>` inside the `Release|Win32` `ItemDefinitionGroup`. This value is not a recent regression introduced by the v143 migration: the same setting exists in the 2020-era revision that the artifact's source lineage descends from, and it is unchanged at HEAD. Changing the platform toolset from v140 to v143 (`dfe35d0`) and the SDK from 8.1 to 10.0 did not touch it. The setting is therefore a long-lived, intentional or unnoticed property of the project rather than an accident of one build.

The consequence is observable in the PE import directory, which imports exactly `MSVCP140D.dll` (4 functions, including `std::_Xlength_error` and `std::_Xout_of_range`), `VCRUNTIME140D.dll` (11) and `ucrtbased.dll` (55, including `_free_dbg`, `_invoke_watson`, `_seh_filter_exe`, `_initialize_onexit_table`). No release-runtime module and no `api-ms-win-crt-*` API set is imported. Because the image is also linked with `/DEBUG` and `/OPT:REF`, dead-strip has already removed unused CRT imports: `_CrtDbgReport` survives only once, which shows that at least one CRT assertion path remains reachable.

This rule affects the project in three practical ways. First, the binary cannot be deployed on a machine that only has the redistributable runtime installed; the debug CRT ships only with Visual Studio ("Debug" runtime), and `ucrtbased.dll` is not part of `VC_redist.x86.exe` at all. Second, debug-CRT semantics differ from release semantics: the debug heap adds guard bytes and leak tracking, `malloc`/`free` are slower, and failing heap operations can raise a Watson dialog instead of terminating quietly — inappropriate for an unattended game server. Third, `NDEBUG` is defined (release preprocessor definition) while the CRT is still debug, so the application's own `assert()` calls are compiled out while library-level assertions remain active, producing an inconsistent failure contract. Debug CRT also makes the binary unsuitable for performance-sensitive packet handling.

**Rule workflow**:
```
MSBuild reads Release|Win32 ItemDefinitionGroup
  -> ClCompile.RuntimeLibrary = MultiThreadedDebugDLL
  -> cl.exe /MDd
  -> defaultlib directives point at msvcprtd / vcruntimed / ucrtd
  -> link.exe writes import table with MSVCP140D, VCRUNTIME140D, ucrtbased
  -> loader requires the VS debug runtime on the host at process start
```

---

### Business Rule: EXE and PDB form one immutable, verifiable pair

**Overview**:
A build product of this set is only usable for debugging when the EXE's CodeView RSDS record and the PDB's info stream agree on GUID and age. This artifact satisfies the rule exactly.

**Detailed description**:
The PE debug directory starts at RVA `0x73350` (file offset `0x71A90` region is the load config; the debug array is at file offset `0x71B50`). Its first entry is type 2 (CodeView) with `PointerToRawData = 0x71D9C`, signature `RSDS` (`0x53445352`), GUID `1ae429cb-d036-4e88-ae81-7a57e3b0ae66`, age `1`, and the NUL-terminated path `E:\wyd\test-setup\w2pp\Code\release\TMSrv\run\TMSrv.pdb`. Parsing the PDB as an MSF 7.00 container (page size 4096, 160 streams) and decoding stream 1 yields version `20000404`, signature `1793996748` (`0x6AEE37CC`, identical to the PE `TimeDateStamp`), age `1` and the same GUID.

This proves three things at once. The PDB in the folder belongs to the EXE beside it and has not been swapped or rebuilt independently. The two files share one build event, so the PDB's 2.85 MB of type and symbol data describes exactly this image. And a debugger pointed at this folder will resolve symbols without a symbol-path search, which is why the artifact is valuable as shipped evidence of the original build environment (toolchain, flags, source list, absolute paths) even though it is useless for reproducing that build today.

The rule also has a negative side: because the binding is machine-path dependent, any relocation of the two files breaks the automatic association, and because the PDB embeds absolute developer paths and the full link command line, publishing it discloses the build machine layout. The PDB contains no embedded source text, only path and checksum records.

**Rule workflow**:
```
compile/link with /DEBUG /PDB:<path>
  -> linker writes RSDS(GUID, age=1, pdb path) into PE debug directory
  -> linker writes MSF stream 1 (version, timestamp, age, GUID)
verification: EXE.GUID == PDB.GUID and EXE.age == PDB.age and PE.timestamp == PDB.signature
```

---

### Business Rule: The artifact's output location cannot be produced by the current solution

**Overview**:
The linker output path recorded in the PDB is `Code/TMSrv/..\release\TMSrv\run\`, which requires `$(SolutionDir)` to have resolved to the project directory. The repository's solution now lives at the root, so the same property expands to a directory outside the repository.

**Detailed description**:
The `/LinkInfo` stream in the PDB records the literal command line, including `/OUT:E:\wyd\test-setup\w2pp\Code\TMSrv\..\release\TMSrv\run\TMSrv.exe`, `/PDB:E:\wyd\test-setup\w2pp\Code\release\TMSrv\run\TMSrv.pdb`, `/IMPLIB:E:\...\Code\release\TMSrv\run\TMSrv.lib`, and `/LTCGOUT:TMSrv\Release\TMSrv.iobj`. The unnormalised `Code\TMSrv\..\release` segment is the fingerprint of an MSBuild property that was expanded relative to the project directory.

`Code/TMSrv/TMSrv.vcxproj:49,53` defines `OutDir` as `$(SolutionDir)..\release\$(ProjectName)\run\`. With the solution file at the repository root (`W2PP Code Project.sln`), `$(SolutionDir)` is `E:\wyd\test-setup\w2pp\`, so `OutDir` resolves to `E:\wyd\test-setup\release\TMSrv\run\` — outside the repository. The only way the observed path arises is `$(SolutionDir) = E:\wyd\test-setup\w2pp\Code\TMSrv\`, i.e. the project was built outside a solution file (direct `msbuild`/`devenv` on the `.vcxproj`, where `SolutionDir` defaults to the project directory). The older, committed `OutDir` (`$(SolutionDir)Build\$(Configuration)\Server\$(ProjectName)\run\`, revision `00ace5f`) does not match either.

The practical consequences are the ones the dependency audit already suspected and this analysis confirms: the folder is not a build destination today, nothing in the build regenerates or overwrites it, and because `.gitignore:16` ignores `[Rr]elease/` and `.gitignore:54` ignores `*.pdb`, the drift is invisible to `git status`. A future build from the solution will write to `..\release\` outside the repo while this stale pair remains in place inside it, and any tooling that treats "release folder" as authoritative will read a binary that is days and one branch away from the source.

**Rule workflow**:
```
$(OutDir) = $(SolutionDir)..\release\$(ProjectName)\run\
SolutionDir := <repo root>           -> <parent-of-repo>\release\TMSrv\run   (current sln)
SolutionDir := <project directory>   -> Code\release\TMSrv\run              (artifact, observed)
SolutionDir := <2020 committed sln>  -> Build\Release\Server\TMSrv\run      (historic)
```

---

### Business Rule: The executable's execution envelope is fixed and unsigned

**Overview**:
The image declares a GUI-subsystem, non-elevated, unversioned, unsigned executable that runs from its own working directory and owns no Windows service, registry or driver surface.

**Detailed description**:
`Subsystem = 2` (`IMAGE_SUBSYSTEM_WINDOWS_GUI`) with subsystem version 6.0, entry point `0x62050`, `DllCharacteristics = 0x8140` (DYNAMIC_BASE, NX_COMPAT, TERMINAL_SERVER_AWARE; GUARD_CF clear). The resource tree contains only `ICON` (ID 101), `GROUP_ICON` and `MANIFEST`; the manifest requests `asInvoker` with `uiAccess='false'`, and there is no `VERSION` resource, hence no CompanyName/FileVersion/ProductVersion metadata anywhere in the image. Data directory 4 (Certificate Table) is absent, so the file is unsigned.

This envelope matters for supply-chain and operational policy. An unsigned, unversioned executable cannot be whitelisted by publisher or version, cannot be attested, and will trigger SmartScreen/EDR suspicion when deployed. `asInvoker` with no service registration means the process must be started manually or by a scheduler, with whatever privileges the launcher holds; the image itself does not require elevation, but it also has no defense against being run elevated. GUI subsystem with a USER32 message loop (`GetMessageA`, `TranslateMessage`, `SetTimer`, `TextOutA`, `GetDC`) indicates a visible window or console text rendering even though the server is socket-driven, which matters for headless deployment. The absence of `ADVAPI32.dll` in the import table confirms no registry, service control, or crypto API usage, consistent with purely file-based configuration.

**Rule workflow**:
```
/code + /SUBSYSTEM:WINDOWS -> GUI subsystem, WinMain entry
/manifest:embed + MANIFESTUAC asInvoker -> no elevation request
no VERSION resource in TMSrv.rc -> no file/version metadata
no /secton for certificates and no signing step -> unsigned image
```

---

### Business Rule: The "release" image contains an ungated packet-fixture dumper

**Overview**:
The compiled translation unit `DumpStruct.cpp` appends a JSON line, per handled packet, to `fixtures.ndjson` in the process working directory. The file is not referenced by HEAD's tree but is compiled into this artifact.

**Detailed description**:
The PDB's module list contains `E:\wyd\test-setup\w2pp\Code\TMSrv\DumpStruct.cpp` and `...\TMSrv\Release\DumpStruct.obj`. The EXE's `.rdata` contains the literal `E:\wyd\test-setup\w2pp\Code\TMSrv\DumpStruct.cpp` together with the dumper's schema fragments: `fixtures.ndjson`, `fixtureType`, `PACKET`, `Server2Client`, `Client2Server`, `"source":{"repository":"W2PP","revision":"..."`, `"path"`, `"function"`, `"line"`, `clientTick`, `opcodeHex`, `.git\HEAD`. On branch `test-setup` (where the file exists), `W2::DumpPacket` opens `g_fixturePath` in `"ab"` mode and writes header fields plus a full hex dump of the packet bytes; `DumpStruct.h` sets `g_fixturePath = "fixtures.ndjson"`. There is no configuration gate, no compile-time `#ifdef`, and no feature flag in that function; the only failure branch is a failed `fopen`. The dispatcher in `ProcessClientMessage.cpp` calls `W2::DumpPacket(...)` for `_MSG_Ping`, `_MSG_AccountLogin`, `_MSG_AccountSecure`, `_MSG_CharacterLogin`, `_MSG_CreateCharacter` and many more.

Because the function's string literals survive dead-strip and are present in the image, the dumper is linked and reachable in this build. Its effect on a deployed server is to persist every accepted client packet — including credential-bearing login and account-secure payloads — in cleartext hex in a working-directory file that grows without bound and is never rotated or deleted by the code shown. This is a data-exposure and disk-growth risk, and it also means the "release" artifact can reconstruct client credentials from its own log.

Confidence: high that the code is linked (direct string and TU evidence); high that the call sites are retained (function retained implies a live reference under `/OPT:REF`); no execution was performed, so runtime activation is inferred, not observed.

**Rule workflow**:
```
client message received
  -> _MSG_* handler / dispatcher
  -> W2::DumpPacket(name, packet, size, direction, loc)
  -> fopen("fixtures.ndjson", "ab")
  -> WriteSourceRef()  (reads .git\HEAD for revision)
  -> JSON: fixtureType, name, schemaVersion, source, meta(opcode), header, bytes(hex)
  -> fclose
```

---

### Business Rule: Memory footprint is fixed at compile time by static pools

**Overview**:
The image reserves 172.5 MiB of virtual address space for `.data` while carrying 6 KiB of file data, because the server allocates its world as fixed global arrays.

**Detailed description**:
Section `.data` has `VirtualSize = 0x0ABEB948` (180,355,400 bytes) and `RawSize = 0x1800` (6,144 bytes), forcing `SizeOfImage = 0x0AC6A000` (180,912,128 bytes) for a 509,440-byte file. `.rsrc` (`0x0AC61000`) and `.reloc` (`0x0AC62000`) are therefore placed above that zero-fill region. The uninitialised tail is the static world state: `CUser pUser[MAX_USER]` and `CMob pMob[MAX_MOB]` at `Code/TMSrv/Server.cpp:269-270`, with `MAX_USER 1000` (`Code/Basedef.h:56`) and `MAX_MOB 25000` (`Code/Basedef.h:107`), plus the `g_pGuildWar[65536]` / `g_pGuildAlly[65536]` / `GuildInfo[65536]` arrays declared in `Code/TMSrv/Server.h:51-52,229-230`. The exact per-object `sizeof(CMob)` was not extracted from the PDB type stream, so the 180 MB figure is correlated with, not arithmetically decomposed into, these arrays.

The rule has two consequences. It fixes capacity at build time: no runtime configuration can raise `MAX_MOB` without a rebuild, and the pools are committed page by page as they are touched, so steady-state working set grows with population. And on a 32-bit process (2 GiB user space), a 172 MiB contiguous reservation at `0x400000` plus the DLL and heap arenas reduces the address space available to other reservations; any large `VirtualAlloc` in the future has less room. The image is not corrupt — the loader is designed for exactly this zero-fill pattern — but the file-to-image ratio of roughly 1:355 is a deployment characteristic that operators must expect.

**Rule workflow**:
```
MAX_USER / MAX_MOB macros (compile time)
  -> CUser pUser[1000]; CMob pMob[25000]  (Server.cpp:269-270)
  -> linker places zero-initialised globals in .data and extends VirtualSize
  -> .rsrc/.reloc placed after the zero-fill region
  -> SizeOfImage 0x0AC6A000; loader reserves, commits on touch
```

---

## 4. Component Structure

```
Code/release/TMSrv/run/                     # component root (git-ignored, stale output folder)
├── TMSrv.exe                               # 509,440 B  PE32 x86 GUI exe, link 14.44, entry 0x62050
│   ├── .text      RVA 0x00001000  raw 406,528 B   code (RX, 0x60000020)
│   ├── .rdata     RVA 0x00065000  raw  64,000 B   literals, import/reloc/loadconfig (R, 0x40000040)
│   ├── .data      RVA 0x00075000  raw   6,144 B   globals + 180 MB zero-fill (RW, 0xC0000040)
│   ├── .rsrc      RVA 0x0AC61000  raw   2,048 B   ICON/GROUP_ICON/MANIFEST (R, 0x40000040)
│   └── .reloc     RVA 0x0AC62000  raw  29,696 B   base relocations (discardable, 0x42000040)
└── TMSrv.pdb                               # 2,854,912 B  MSF 7.00, 4096 B pages, 160 streams
    ├── stream 1   PDB info: version 20000404, GUID 1ae429cb-..., age 1
    ├── stream 2   TPI (407,236 B)  type records
    ├── stream 3   DBI (220,031 B)  module/section/contributions
    ├── stream 4   names (39,856 B) symbol name table
    └── /names, /src/headerblock, /LinkInfo, /TMCache, /UDTSRCLINEUNDONE
```

Source-side boundary of the artifact (what is inside the image), as proven by the PDB module list — 76 translation units:

```
Code/Basedef.cpp, Code/CPSock.cpp                     # shared, one level up
Code/TMSrv/Server.cpp, imple.cpp, GetFunc.cpp, CReadFiles.cpp,
           ProcessClientMessage.cpp, ProcessDBMessage.cpp, ProcessSecMinTimer.cpp,
           SendFunc.cpp, MobKilled.cpp, CItem.cpp, CMob.cpp, CNpcGene.cpp,
           CUser.cpp, CWarTower.cpp, CCastleZakum.cpp, DumpStruct.cpp   (absent at HEAD)
Code/TMSrv/_MSG_*.cpp                                 # 58 message handlers
Code/TMSrv/TMSrv.rc                                    # icon + manifest only
```

---

## 5. Dependency Analysis

### Internal dependencies

None at repository level: no project, script, test or source file references `Code/release/**`. The folder is an output sink, and `.gitignore` excludes it, so no internal consumer exists in the tree.

Internal dependencies *inside* the artifact (compile-time unit graph, evidenced by the PDB):

```
TMSrv.exe -> TMSrv.pdb                     (CodeView RSDS: GUID 1ae429cb-..., age 1)
TMSrv.pdb -> 76 object modules             (Code\TMSrv\TMSrv\Release\*.obj)
TMSrv.exe -> Code/TMSrv/*.cpp + Code/Basedef.cpp + Code/CPSock.cpp   (via line tables + __FILE__)
TMSrv.exe -> Code/TMSrv/TMSrv.rc           (ICON 101, GROUP_ICON, MANIFEST asInvoker)
```

### External dependencies (import table, 8 modules, 139 functions)

```
- KERNEL32.dll        21  process/thread/memory, SRW locks, GetStartupInfoW, IsDebuggerPresent,
                           SetUnhandledExceptionFilter, QueryPerformanceCounter, GetTickCount
- USER32.dll          24  window class + message loop, SetTimer, TextOutA, MessageBoxA, GetDC/ReleaseDC
- GDI32.dll            6  SelectObject, SetTextColor, TextOutA, DeleteObject, GetStockObject
- WS2_32.dll          17  all by ordinal (WSAStartup/socket/bind/listen/accept/recv/send family)
- WINMM.dll            1  by ordinal (timer/sound entry point)
- MSVCP140D.dll        4  std::_Xlength_error, std::_Xout_of_range, __std_* debug helpers
- VCRUNTIME140D.dll   11  C++ EH/terminate debug runtime
- ucrtbased.dll       55  debug UCRT: malloc/free(_free_dbg), fopen/fread/fwrite/fgets/fputs,
                           strncpy/strncmp/_stricmp, _sopen_dispatch, _errno, localtime64, rand,
                           _invoke_watson, _seh_filter_exe, _initterm/_initterm_e, atexit tables
```

Notable **absences** (meaningful negative evidence): no `ADVAPI32` (no registry/service/crypto), no `ODBC32`/`ODBCCP32` despite them being listed in `AdditionalDependencies` (`Code/TMSrv/TMSrv.vcxproj:87`) — the linker dropped them, so the server has no direct database client, matching the socket-to-DBSrv design; no release CRT; no `api-ms-win-crt-*` API sets; no `dbghelp`/`psapi`; no network security libraries.

### Toolchain and SDK (from PDB `/names` and source paths)

```
MSVC toolset      14.44.35207   C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
Linker            HostX86\x86\link.exe 14.44
Windows SDK       10.0.26100.0 (Include\10.0.26100.0\shared|um|ucrt)
Intermediate PDB  Code\TMSrv\TMSrv\Release\vc143.pdb
```

### Source-control lineage

```
HEAD               feat/milestone1 @ bbf8d36 (2026-11-15) — tree has 75 TUs, no DumpStruct.cpp
Artifact matches   test-setup branch   @ 42910d1 lineage (2026-11-06 17:14 -03) — 76 TUs
Divergence         32 files changed, 209 insertions(+), 722 deletions(-) between test-setup and HEAD
                   (Code/TMSrv/*, incl. deletion of DumpStruct.cpp/.h at HEAD)
```

---

## 6. Afferent and Efferent Coupling

This component contains no classes or functions of its own; coupling is measured at artifact granularity (file -> external modules/symbols), with the internal source graph summarized for context.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|-------------------|
| `TMSrv.exe` (image) | 0 (nothing in the repo references it) | 8 DLL modules / 139 imported functions | Critical |
| `TMSrv.pdb` | 0 (only a debugger would consume it) | 1 (bound EXE by GUID/age) + 76 object modules | High |
| Source units in image (`Code/TMSrv/*.cpp`) | 0 (from artifact perspective) | 76 TUs -> 5 shared headers (`Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`), 8 OS modules | High (shared global state) |

Cohesion note: the 76 TUs share mutable global state through `extern` declarations (`pUser`, `pMob`, `g_pGuildWar`, `g_pGuildAlly`, `GuildInfo`, `temp`), visible in `Code/TMSrv/Server.h:229-230,306-307,382`. That is extremely low cohesion / maximal data coupling at the source level, and it is what makes the 180 MB `.data` region possible. Boundary note: the artifact itself is a boundary object with no inbound edges, which is exactly why its staleness is invisible to the compiler and to `git status`.

---

## 7. Endpoints

N/A. The artifact is a Win32 GUI-subsystem executable, not a REST/GraphQL/gRPC/WebSocket service definition; it declares no routes, no schema and no interface contract. Its runtime TCP listener is created by `WS2_32` calls emitted from source (`Code/TMSrv/Server.cpp`), with the bind address/port coming from configuration files (`localip.txt`, `serverlist.txt`, `gameconfig.txt`), none of which is expressed in or discoverable from the artifact metadata. The wire protocol is a packed binary packet structure (`Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick` — as recorded in the fixture header shape embedded in the image), again defined by source, not by the artifact.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| `WS2_32.dll` | OS network library | Client sessions, DBSrv sessions | Winsock TCP/UDP (17 ordinal imports) | Packed binary packets | Return-code checks in source; no visible retry/circuit breaker in artifact |
| `WS2_32.dll` -> remote DBSrv | Internal service (separate process) | Account/character persistence | Winsock over TCP, `../../DBSRV/Run/Admin.txt`, `Mac.txt` config paths | Packed binary (`_MSG_*`) | Socket error paths; process-level, no transactionality |
| `KERNEL32.dll` | OS | Process, threads, SRW locks, timing | Win32 API | Native | `SetUnhandledExceptionFilter`, `UnhandledExceptionFilter` installed |
| `USER32.dll` | OS | Window class, message pump, timers, text rendering | Win32 API | Native | `MessageBoxA` on fatal paths |
| `GDI32.dll` | OS | Text drawing into the window/console | Win32 API | Native | N/A |
| `WINMM.dll` | OS | Timing (1 ordinal) | Win32 API | Native | N/A |
| `MSVCP140D` / `VCRUNTIME140D` / `ucrtbased` | External runtime | C++ STL + CRT (debug) | DLL import | Native | Debug assert / `_invoke_watson`; **not redistributable, not present by default** |
| File system (working dir) | Local I/O | Config/asset load: `ItemList.bin`, `extraitem.bin`, `Guilds.txt`, `NPCGener.txt`, `CastleQuest.txt`, `SancRate.txt`, `QuestsRate.txt`, `CompRate.txt`, `AttributeMap.dat`, `heightmap.dat`, `serverlist.bin`, `Language.txt`, `gameconfig.txt`, `levelitem.txt`, `mobname.txt`, `biserver.txt`, `dbserver.txt` | Win32 file I/O | Text and packed binary | `fopen` null checks; Portuguese/English message strings embedded |
| File system (working dir) | Local I/O | Fixture dumping: `fixtures.ndjson`, `TesteDie.txt`, `AttackDieTeste.txt`, `Gethalf.txt` | Append writes | NDJSON / text | Silent `return` on `fopen` failure |
| `.git\HEAD` | Local VCS metadata | Reads repository revision to stamp fixture records | File read | Text ref | "unknown" on failure |
| Debugger / symbol consumer | Tooling | Post-mortem symbolization of this exact image | RSDS GUID + age | MSF 7.00 PDB | Binding fails if either file is replaced |

Note: the `AdditionalDependencies` list in `Code/TMSrv/TMSrv.vcxproj:87` names `odbc32`, `odbccp32`, `advapi32`, `ole32`, `oleaut32`, `uuid`, `shell32`, `comdlg32`, `winspool`; none of them appears in the import table, so the effective runtime surface is much smaller than the project file suggests.

---

## 9. Design Patterns & Architecture

The artifact implements no application pattern of its own. The relevant patterns are build- and packaging-level decisions, all directly evidenced.

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Two-file debug-info binding | CodeView RSDS GUID + age in PE; identical GUID/age in PDB stream 1 | `TMSrv.exe` debug dir; `TMSrv.pdb` stream 1 | Deterministic symbolization of this exact image |
| Whole-program optimization | `/LTCG:incremental` with `/LTCGOUT:TMSrv\Release\TMSrv.iobj`, `/OPT:REF`, `/OPT:ICF` | PDB `/LinkInfo`; `Code/TMSrv/TMSrv.vcxproj:33,85-86` | Cross-module inlining and COMDAT folding in a Release build |
| Static arena allocation | Global arrays sized by macros | `Code/TMSrv/Server.cpp:269-270`; `Code/Basedef.h:56,107` | Zero-allocation runtime for world entities |
| Compile-time instrumentation via `__FILE__` | Absolute source paths embedded in `.rdata` and PDB | `.rdata` (`E:\wyd\...\DumpStruct.cpp`, `ProcessClientMessage.cpp`, `ProcessDBMessage.cpp`, `SendFunc.cpp`) | Assert/log provenance; also the source of path disclosure |
| Structural exception handling | `/SAFESEH` with 8 registered handlers | Load config at RVA `0x473504` | Blocking of non-image SEH handlers |
| Stack protection | `/GS` cookie (`__security_cookie`, `__security_check_cookie` present in PDB symbols) | N/A in PE metadata | Buffer-overrun mitigation |
| Embedded manifest | `MANIFEST` resource, `asInvoker`, `uiAccess=false` | `.rsrc` at file offset `0x74A00` | UAC/execution-level declaration, no SxS CRT assembly |
| Single-binary / no installer | One EXE + one debug PDB, no dependency manifest, no bootstrap | Component root | Manual copy deployment; assumes host prerequisites |
| Non-deterministic build | Real PE `TimeDateStamp` `0x6AEE37CC`, no `/Brepro` | PE file header | Reproducibility is not achievable from this artifact |

Architectural observation: the artifact confirms the intended process topology of the project — `TMSrv` is a socket peer of `DBSrv` (no ODBC import), with all persistence delegated to that second process and all configuration loaded from relative files at startup. The presence of `pUser[1000]` and `pMob[25000]` as statics confirms a single-threaded, fixed-capacity world model held in process memory, with the debug CRT and the packet dumper layered on top of it.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Debug CRT in Release | `MSVCP140D`/`VCRUNTIME140D`/`ucrtbased` imported; `/MDd` from `TMSrv.vcxproj:80` | Not deployable without VS debug runtime; `ucrtbased` is not in `VC_redist`; slower heap, assert dialogs, different failure semantics |
| High | `DumpStruct` fixture dumper | Ungated append of every handled client packet (incl. `_MSG_AccountLogin`, `_MSG_AccountSecure`) to `fixtures.ndjson`; reads `.git\HEAD` | Credential/PII exposure on disk, unbounded file growth, repository metadata leakage |
| High | Provenance | Binary unsigned, no VERSIONINFO, no embedded VCS revision, no CI/release process | Cannot attribute or verify the artifact; cannot map to a commit without this forensic analysis |
| High | Source drift | Artifact built from `test-setup` lineage (76 TUs incl. `DumpStruct.cpp`); HEAD has 75 TUs; 32 files, +209/-722 diverge | Debugging HEAD with this PDB will mis-resolve lines and locals; "release" no longer represents the product |
| High | Orphaned output location | `OutDir` resolves outside the repo from the current `.sln`; folder is git-ignored (`.gitignore:16,54`) | Stale binary silently persists inside the repo; consumers may trust it as current |
| Medium | Memory envelope | `.data` virtual size 180,355,400 B; `SizeOfImage` 172.5 MiB for a 0.5 MB file; `pMob[25000]` | 32-bit VA pressure, large working set, capacity fixed at build time |
| Medium | PDB publication | Private PDB includes absolute developer paths, full link command line, all type/symbol names; committed next to the EXE | Build-environment disclosure; eases reverse engineering |
| Medium | Hardening gaps | `GuardFlags = 0` (no CFG), `CheckSum = 0` | Weaker exploit mitigation and weaker tamper detection than a modern build |
| Medium | Reproducibility | Non-deterministic timestamp, no `/Brepro`, no documented toolset pin beyond v143 | Binary cannot be rebuilt bit-identically; artifact-first debugging only |
| Medium | Legacy/dead linker inputs | `AdditionalDependencies` lists ODBC/OLE/ADVAPI libs that are not imported | Misleading dependency documentation; audit noise |
| Low | Multiplexed global state | 76 TUs coupled through `extern` globals | Any change to pool sizes/structures shifts `.data` layout and invalidates the PDB |
| Low | Resource metadata | Portuguese-Brazilian language block, icon-only resources, no version strings | Minor localization/packaging inconsistency |
| Low | Naming drift | Readings "release" for a `/MDd` build | Organizational confusion between "Release configuration" and "releasable artifact" |

Explicitly out of scope per instructions: no refactoring recommendations, no remediation plan, no effort estimates.

---

## 11. Test Coverage Analysis

**There is no test coverage for this component, and none exists anywhere in the repository.** The artifact is a build output and is not itself testable; the code it contains has zero automated tests.

| Target | Unit Tests | Integration Tests | Coverage | Test Quality |
|--------|------------|-------------------|----------|--------------|
| `TMSrv.exe` (artifact) | 0 | 0 | 0% (not measurable) | N/A — no harness exists |
| `TMSrv.pdb` | 0 | 0 | N/A | N/A — build metadata, not executable logic |
| Source units inside the image (76 TUs) | 0 | 0 | 0% | N/A |
| `DumpStruct` fixture dumper | 0 | 0 | 0% | N/A — yet it writes client credentials to disk |

Evidence for the absence (searched entire project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`, `.git`, `.vs`):

1. No file matching `*test*`, `*Test*`, `*_test*` or `*.spec.*` anywhere in the tree.
2. No directory named `test`, `tests`, `spec`, `specs` or `__tests__`.
3. `W2PP Code Project.sln` declares exactly three projects: `DBSrv`, `TMSrv`, and the `Solution Items` folder. No test project exists.
4. No reference to any test framework (`gtest`, `catch2`, `doctest`, `BOOST_TEST`, `TEST_CASE`, `CPPUNIT`) in any `.cpp`, `.h`, `.vcxproj` or `.sln`.
5. Zero occurrences of `assert(`, `ASSERT(` or `static_assert` in `Code/TMSrv/*.cpp|*.h` or `Code/*.cpp|*.h`, so not even compile-time self-checks are present.
6. No CI or build automation: no `.yml`, `.yaml`, `.ps1`, `.bat`, `.cmd`, `Makefile` or `CMakeLists.txt` anywhere in scope.
7. `Code/release/TMSrv/run/` contains only the two binaries; no test fixtures, logs or expected-output data accompany them.

The only verification artefacts that exist are operator-visible diagnostic outputs referenced by string literals in the image (`TesteDie.txt`, `AttackDieTeste.txt`, `Gethalf.txt`, `fixtures.ndjson`). These are runtime dumps, not tests: they have no assertions, no pass/fail criteria and no automation.

Risk framing: with 0% automated coverage, the only correctness signal for this binary is manual gameplay observation. The drift documented in Sections 1 and 5 means even that signal describes a source branch (a packet-dump/C++20 experimental line) that is not what HEAD builds, so manual findings cannot be reliably attributed to current code.

---

## 12. Appendix: Verification Evidence, Hashes and Limitations

### 12.1 File identity

```
TMSrv.exe  509,440 bytes  mtime 2026-11-06 17:25:50 -03:00
           SHA-256  F8C744BD65648C71190ABDB18B5C7960F7917B760616179C62917222CCC66199
TMSrv.pdb  2,854,912 bytes mtime 2026-11-06 17:25:50 -03:00
           MD5      21D56BBAF931B0424E7E40BB8F104C8F
```

### 12.2 PE header facts

```
DOS e_magic 0x5A4D, e_lfanew 0xF8, PE sig 0x4550
Machine 0x014C (I386)              NumberOfSections 5
TimeDateStamp 0x6AEE37CC = 1793996748 = 2026-11-06 20:25:48 UTC
Characteristics 0x0102 (EXECUTABLE_IMAGE | 32BIT_MACHINE)
OptionalHeader magic 0x010B (PE32), linker 14.44
AddressOfEntryPoint 0x62050 (inside .text), ImageBase 0x400000
SectionAlignment 0x1000, FileAlignment 0x200, SizeOfHeaders 0x400
OS version 6.0, subsystem version 6.0
Subsystem 2 (WINDOWS_GUI), DllCharacteristics 0x8140
  = DYNAMIC_BASE | NX_COMPAT | TERMINAL_SERVER_AWARE   (GUARD_CF absent)
SizeOfImage 0x0AC6A000, CheckSum 0x0 (not set)
Data directories present: IMPORT(1), RESOURCE(2), BASERELOC(5), DEBUG(6),
  TLS(9), LOAD_CONFIG(10), IAT(12). Certificate table (4) absent -> unsigned.
Section size sum = 0x400 + 0x63400 + 0xFA00 + 0x1800 + 0x800 + 0x7400 = 509,440 = file size
  -> no overlay, no appended payload
```

### 12.3 Authenticode / signature check

No `IMAGE_DIRECTORY_ENTRY_SECURITY` entry exists, therefore the file carries no Authenticode signature, no timestamp countersignature and no publisher identity. `TMSrv.pdb` likewise carries no signature.

### 12.4 Debug directory and PDB binding

```
DebugDir RVA 0x73350, size 0x70, 4 entries
[0] Type 2 CodeView   size 80   -> RSDS, GUID 1ae429cb-d036-4e88-ae81-7a57e3b0ae66, age 1,
                                   path E:\wyd\test-setup\w2pp\Code\release\TMSrv\run\TMSrv.pdb
[1] Type 12 VC_FEATURE size 20
[2] Type 13 POGO       size 864
[3] Type 14 ILTCG      size 0
PDB superblock: "Microsoft C/C++ MSF 7.00", PageSize 4096, NumPages 697, 160 streams
PDB stream 1: version 20000404, signature 1793996748, age 1, GUID 1ae429cb-...  -> matches
```

### 12.5 Recovered linker command (verbatim, from PDB `/LinkInfo`)

```
cmd /ERRORREPORT:QUEUE
    /OUT:E:\wyd\test-setup\w2pp\Code\TMSrv\..\release\TMSrv\run\TMSrv.exe
    /INCREMENTAL:NO /NOLOGO /MANIFEST "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
    /manifest:embed /DEBUG
    /PDB:E:\wyd\test-setup\w2pp\Code\TMSrv\..\release\TMSrv\run\TMSrv.pdb
    /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG:incremental
    /LTCGOUT:TMSrv\Release\TMSrv.iobj /TLBID:1 /DYNAMICBASE /NXCOMPAT
    /IMPLIB:E:\wyd\test-setup\w2pp\Code\TMSrv\..\release\TMSrv\run\TMSrv.lib
    /MACHINE:X86 /SAFESEH
```

### 12.6 Rich header (linker fingerprint)

```
Rich key 0x8FADE68E, DanS at 0x80, Rich tag at 0xE0
prodId=261 build=35207 count=30     prodId=265 build=35229 count=76
prodId=260 build=35207 count=12     prodId=258 build=35229 count=1
prodId=259 build=35207 count=5      prodId=255 build=35229 count=1
prodId=257 build=35207 count=4      prodId=151 build=0      count=1
prodId=257 build=33145 count=13     prodId=1   build=0      count=147 (import records)
```

`build=35207` is the MSVC 14.44 toolset build; `build=35229` matches the Windows SDK 10.0.26100 era. The `count=76` entry for `prodId=265` equals the number of compiled translation units, independently corroborating the 76-TU PDB module list. Individual product-ID names are not asserted here because they are undocumented internal linker constants.

### 12.7 Cross-checks used to establish drift

```
HEAD (feat/milestone1 @ bbf8d36) TMSrv ClCompile entries: 75 ; ClInclude: 17
test-setup @ edc4bd1            TMSrv ClCompile entries: 76 ; ClInclude: 18  (+DumpStruct.cpp/.h)
PDB module list                 TMSrv-local .cpp: 74 (incl. DumpStruct.cpp) + Basedef.cpp + CPSock.cpp = 76
=> artifact TU set == test-setup exactly, != HEAD
test-setup same-day commits before the 17:25:48 build: 9be4cf0 (16:39), df36f19 (16:58), 42910d1 (17:14)
test-setup next commit: 2e23b1a (2026-11-07 09:02) -> build sits at the 2026-11-06 evening state
git diff test-setup..HEAD -- Code/TMSrv Code/Basedef.* : 32 files, 209 insertions, 722 deletions
DumpStruct.cpp introduced in 7db00b2 ("upgrade to C++20"), absent at HEAD, present at test-setup
```

### 12.8 Method and limitations

- All inspection was read-only: PowerShell byte-level PE parsing (headers, sections, imports, resources, load config, debug directory, Rich header), MSF/PDB container parsing (superblock, stream directory, stream 1), string extraction, `Get-FileHash`, and read-only Git queries. The executable was never launched, neither was any test runner.
- `sizeof(CMob)` / `sizeof(CUser)` were not decoded from the PDB TPI stream, so the 180 MB `.data` region is correlated with `pUser[1000]` + `pMob[25000]` rather than arithmetically attributed to them.
- Rich header product IDs are reported as numeric IDs and build numbers only; no mapping to Microsoft product names is assumed.
- The compiled language standard cannot be read from the PDB; C++20 is inferred from the presence of `DumpStruct.cpp`, which uses `std::source_location` and exists only on the C++20 branch. It is therefore an inference, not a measurement.
- Whether `DumpPacket` call sites are retained is inferred from the survival of its literals under `/OPT:REF` plus the branch source; runtime activation was not observed.
- The artifact's `TMSrv.lib` (import library written by `/IMPLIB`) is not present in the folder, so the set is incomplete relative to the linker's full output, though complete for debugging the EXE.
- The report intentionally contains no remediation guidance, no refactoring proposals and no time estimates, per scope.
