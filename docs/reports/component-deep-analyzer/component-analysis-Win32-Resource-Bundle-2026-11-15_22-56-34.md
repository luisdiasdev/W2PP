# Component Deep Analysis Report: Win32 Resource Bundle

**Component**: Win32 Resource Bundle (TMSrv + DBSrv resource scripts, symbol headers, icon assets)
**Project**: W2PP — decompiled C/C++ Win32 MMORPG server emulator (2 executables: TMSrv, DBSrv)
**Analysis scope**: `Code/TMSrv/TMSrv.rc`, `Code/TMSrv/resource.h`, `Code/TMSrv/icon1.ico`, `Code/DBSrv/DBSrv.rc`, `Code/DBSrv/resource.h`, `Code/DBSrv/icon1.ico`, `Code/DBSrv/DBSrv.aps`
**Excluded from analysis**: `.agents`, `.codegraph`, `.opencode`, `openspec`
**Report date**: 2026-11-15 22:56:34
**Analysis authority**: static source reading of tracked files + PE resource-table inspection of the shipped `Code/release/TMSrv/run/TMSrv.exe` (read-only, no execution)

---

## 1. Executive Summary

The Win32 Resource Bundle is a build-time-only component. It carries no runtime logic, no business domain rules, and no endpoints. It exists to bind three kinds of static assets into the two server executables:

1. A single application icon, declared as `IDI_ICON1` = 101 in both projects, compiled from `icon1.ico`.
2. One dialog template in DBSrv only (`IDD_MAINDLG`), captioned `"DBSRV"`, 415 x 280 dialog units, using the legacy `"System"` bitmap font.
3. Nothing else. There is **no** `VERSIONINFO` block, no `MENU` resource, no `ACCELERATORS`, no `STRINGTABLE`, no bitmap, and no cursor in either resource script.

The bundle therefore does **not** own the menus that appear in the two server windows. Both `MainWndProc` implementations set `wc.lpszMenuName = NULL` and build the entire menu bar at runtime with `CreateMenu` / `CreatePopupMenu` / `AppendMenu`, using command IDs that are `#define`d in `Code/Basedef.h:31-39` (9000-9006, 9050), completely outside `resource.h`. The architectural role description ("version/menu resources") should be corrected on the version and menu points: neither exists in this component.

Key findings, in order of severity:

- **The icon resource is never loaded by the running program.** Both servers call `LoadIcon((HINSTANCE)hInstance, "MAINICON")` (`Code/TMSrv/Server.cpp:3401`, `Code/DBSrv/Server.cpp:322`). No resource named `MAINICON` exists anywhere in the projects. The shipped `TMSrv.exe` resource table contains exactly `RT_ICON` ids 1 and 2, `RT_GROUP_ICON` id 101, and `RT_MANIFEST` id 1 — the string `"MAINICON"` occurs once in the image, as the failed lookup argument. `LoadIconA` returns `NULL`, so `WNDCLASS.hIcon` is `NULL` and the window receives the system default application icon. The icon in the EXE's shell/Explorer entry still renders (Windows uses the lowest-numbered group icon), so the defect is limited to the title bar, Alt-Tab, taskbar grouping and any `WM_GETICON` consumer.
- **No version metadata ships at all.** There is no `RT_VERSION` resource in `TMSrv.exe`. File properties (company, product, file version, description) are blank for both servers. Program identity is carried only by `APP_VERSION 6975` in `Code/Basedef.h:42`, which is a protocol/build constant, not a Win32 version resource.
- **The DBSrv dialog is an orphan, and it is not even addressed by its integer ID.** `DBSrv.rc:61` uses `IDD_MAINDLG`, but `Code/DBSrv/resource.h:26` defines `IDD_MainDlg` (different casing). The C preprocessor is case-sensitive, so the identifier never expands; the compiled `DBSrv.res` proves this by storing the dialog under the **string name** `"IDD_MAINDLG"` (RT_DIALOG, type 5) rather than ordinal 103. A secondary ID, `IDD_DIALOG1` = 102 (`Code/DBSrv/resource.h:25`), has no corresponding resource at all.
- **The two icons are byte-identical** (SHA-256 `C585D6D521C8FF60CB072923A22889F24C5B18C2F79CCE92208782D8DC33A6D2` for both). TMSrv and DBSrv are visually indistinguishable in the Windows shell and taskbar.
- **`DBSrv.aps` is not a checked-in artifact.** It is present in the working tree (83,420 bytes) but explicitly ignored by `.gitignore:77` (`*.aps`) and untracked per `git check-ignore`. It is machine-local Visual Studio resource-editor cache, contains a stale absolute path (`E:\wyd\test-setup\w2pp-agateownz\Code\DBSrv\DBSrv.rc`), and is consumed by nothing in the build. See BR-11.

Overall assessment: minimal, working, and low-risk as a build input, but the component is materially incomplete with respect to what it is assumed to provide (icon at runtime, version info), and it contains one demonstrable resource-ID/casing defect plus two dead resource IDs.

---

## 2. Data Flow Analysis

This component has two distinct flows: a build-time flow (where the resources are assembled into the PE) and a runtime flow (where the executable asks the loader for them).

### 2.1 Build-time flow

```
1. msbuild resolves project: Code/TMSrv/TMSrv.vcxproj (Release|Win32)
2. ResourceCompile item TMSrv.rc (TMSrv.vcxproj:187) is dispatched to rc.exe
3. rc.exe preprocesses TMSrv.rc:
      TMSrv.rc:3   #include "resource.h"   -> IDI_ICON1 becomes 101
      TMSrv.rc:10  #include "Windows.h"    -> RT_* type constants, dialog styles
4. rc.exe /l"0x0409" sets the default language for un-tagged resources
5. TMSrv.rc:19 LANGUAGE LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN
      -> overrides the /l default; all resources become 0x0416 (1046)
6. TMSrv.rc:54 IDI_ICON1 ICON "icon1.ico"
      -> icon1.ico parsed; RT_ICON ids 1..N emitted in .ico entry order,
         RT_GROUP_ICON name 101 emitted as the directory
7. Output written to TMSRV\RELEASE\TMSRV.RES (relative to project dir,
      which is why a nested Code/TMSrv/TMSrv/Release/ directory exists)
8. link.exe consumes TMSrv.res plus the linker-generated default manifest
      -> single .rsrc section in TMSrv.exe
9. No VERSIONINFO, no MENU, no DIALOG block is encountered -> nothing else emitted
```

Observed `rc.exe` invocation (from `Code/TMSrv/TMSrv/Release/TMSrv.tlog/rc.command.1.tlog`):

```
rc.exe /l"0x0409" /nologo /fo"TMSRV\RELEASE\TMSRV.RES" E:\...\CODE\TMSRV\TMSRV.RC
```

Observed rc.exe input dependency set (from `rc.read.1.tlog`): `TMSRV.RC`, `RESOURCE.H`, `WINDOWS.H`, `WINAPIFAMILY.H`, `WINPACKAGEFAMILY.H`, `SDKDDKVER.H`, `WINRESRC.H`, `WINUSER.RH`, `COMMCTRL.RH`, `DDE.RH`, `WINNT.RH`, `DLGS.H`, `WINVER.H`, `SPECSTRINGS.H`, `SAL.H`, `CONCURRENCYSAL.H`, `DRIVERSPECS.H`, `SDV_DRIVERSPECS.H`, `VERRSRC.H`, `ICON1.ICO`. Note that `ICON1.ICO` **is** tracked as a build input by rc.exe even though the project file lists it as `<None>` (see BR-10).

### 2.2 Runtime flow

```
1. WinMain (TMSrv: Code/TMSrv/Server.cpp:3459, DBSrv: Code/DBSrv/Server.cpp:427)
2. InitApplication(hInstance)
      TMSrv:Server.cpp:3392 / DBSrv:Server.cpp:314
3. WNDCLASS assembled; wc.hIcon = LoadIcon(hInstance, "MAINICON")
      TMSrv:Server.cpp:3401 / DBSrv:Server.cpp:322
      -> loader searches .rsrc for RT_GROUP_ICON with NAME "MAINICON"
      -> not found (resources are: id 101 group icon, ids 1/2 images) -> NULL
4. wc.lpszMenuName = NULL   (TMSrv:Server.cpp:3405, DBSrv:Server.cpp:326)
      -> no MENU resource is requested; none exists
5. RegisterClass(&wc)
      -> class registered with a NULL icon; window falls back to IDI_APPLICATION
6. InitInstance: hMainMenu = CreateMenu()  (TMSrv:3416, DBSrv:337)
7. CreateWindow("MainClass", "Game Server" | "DB Server", ...)
      -> empty menu handle passed as hMenu
8. WM_CREATE handler builds the real menu bar at runtime
      TMSrv:Server.cpp:4055-4079, DBSrv:Server.cpp:1193-1209
      -> CreateMenu + CreatePopupMenu + AppendMenu(IDC_*), then SetMenu(hWnd, hMenu)
9. WM_COMMAND dispatch on the Basedef.h command IDs
      TMSrv:Server.cpp:4081-4110 (7 commands), DBSrv:Server.cpp:1211-1219 (2 commands)
10. Shell/Explorer (out of process) reads RT_GROUP_ICON 101 for the file icon
```

Note the split behaviour at step 3 versus step 10: the shell resolves the file icon by *lowest group-icon ID* and therefore succeeds, while the window resolves by *explicit name* and fails. This asymmetry is the whole substance of BR-04.

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Resource identity | Each executable declares exactly one icon resource, symbol `IDI_ICON1`, numeric ID 101, compiled from `icon1.ico` | `Code/TMSrv/resource.h:22`, `Code/TMSrv/TMSrv.rc:54`, `Code/DBSrv/resource.h:24`, `Code/DBSrv/DBSrv.rc:54` |
| BR-02 | Resource identity | The application icon must be the lowest-ID icon in the script so Windows keeps it stable across systems | `Code/TMSrv/TMSrv.rc:52-54`, `Code/DBSrv/DBSrv.rc:52-54` |
| BR-03 | Localisation | All resources are tagged `LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN`; the rc.exe `/l` default is overridden | `Code/TMSrv/TMSrv.rc:19`, `Code/DBSrv/DBSrv.rc:19`, rc tlog |
| BR-04 | Runtime contract | The loader expects an icon resource named by the string `"MAINICON"`; the bundle must expose that name | `Code/TMSrv/Server.cpp:3401`, `Code/DBSrv/Server.cpp:322` |
| BR-05 | Boundary | Menus are not resource-backed; command IDs live in `Basedef.h` in the application command range, not in `resource.h` | `Code/Basedef.h:31-39`, `Code/TMSrv/Server.cpp:4055-4079`, `Code/DBSrv/Server.cpp:1193-1209` |
| BR-06 | Resource identity | DBSrv defines one dialog template `IDD_MAINDLG` (415x280 DLU, caption "DBSRV", `"System"` font) that no code instantiates | `Code/DBSrv/DBSrv.rc:61-66`, `Code/DBSrv/resource.h:26` |
| BR-07 | ID allocation | Resource IDs are allocated per project by the VS editor; `_APS_NEXT_*` sentinels are the allocation frontier; every ID in `resource.h` must correspond to a resource in the `.rc` | `Code/TMSrv/resource.h:28-31`, `Code/DBSrv/resource.h:32-35` |
| BR-08 | Versioning | The resource bundle carries no version metadata; there is no `VERSIONINFO` / `RT_VERSION` resource | `Code/TMSrv/TMSrv.rc` (whole), `Code/DBSrv/DBSrv.rc` (whole), shipped `TMSrv.exe` |
| BR-09 | Linker policy | The only manifest in the image is the linker default, declaring `requestedExecutionLevel level='asInvoker'` | shipped `TMSrv.exe` RT_MANIFEST |
| BR-10 | Build inclusion | Only `TMSrv.rc` / `DBSrv.rc` are compiled; `icon1.ico` is a `<None>` item resolved by rc.exe relative to the `.rc` | `Code/TMSrv/TMSrv.vcxproj:187,190`, `Code/DBSrv/DBSrv.vcxproj:116,119` |
| BR-11 | Build hygiene | `*.aps` is never a build input and never versioned; it is a machine-local editor cache | `.gitignore:77`, `git check-ignore`, `Code/DBSrv/DBSrv.aps` |
| BR-12 | Character encoding | Both projects are `MultiByte` (ANSI); dialog templates and icon lookups bind to the ANSI variants (`DialogBoxA`-family semantics, `LoadIconA`) | `Code/TMSrv/TMSrv.vcxproj:27,34`, `Code/DBSrv/DBSrv.vcxproj:27,34` |

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 — One icon resource per executable, symbolised as IDI_ICON1 = 101

**Overview**:
Each of the two server executables embeds exactly one application icon resource. The resource is declared once, with the symbolic name `IDI_ICON1`, which resolves to the integer 101 through the project's own `resource.h`. The binary payload is `icon1.ico`, a two-image 16-colour icon file.

**Detailed description**:

The declaration is textually identical in both projects except for file paths: `IDI_ICON1               ICON                    "icon1.ico"` at `Code/TMSrv/TMSrv.rc:54` and `Code/DBSrv/DBSrv.rc:54`. The numeral 101 comes exclusively from the per-project symbol header — `#define IDI_ICON1 101` at `Code/TMSrv/resource.h:22` and `Code/DBSrv/resource.h:24`. Because rc.exe runs the C preprocessor over the script, the identifier is expanded before compilation, so the shipped resource directory entry stores the *ordinal* 101, not the string `IDI_ICON1`. This was verified against the shipped image: the PE resource tree of `Code/release/TMSrv/run/TMSrv.exe` (309,440-byte `.rsrc`-bearing PE32, SHA-256 `f8c744bd65648c71190abdb18b5c7960f7917b760616179c62917222ccc66199`) contains `RT_GROUP_ICON` (type 14) with `name = id 101` and language 1046, and contains no UTF-16 `IDI_ICON1` string anywhere. The same ordinal choice in both projects is deliberate duplication, not sharing: the two `resource.h` files are independent copies, so the "101" is an invariant of convention rather than a compile-time guarantee.

The icon file itself is a legacy ICO container with two entries, both 4 bits per pixel (16 colours). Parsing the ICO directory of `Code/TMSrv/icon1.ico` (1,078 bytes) gives entry 0 = 16x16, 16 colours, 4bpp, 296 bytes of image data at offset 38, and entry 1 = 32x32, 16 colours, 4bpp, 744 bytes at offset 334 (334 + 744 = 1,078, i.e. the file is exactly covered). rc.exe converts each ICO entry into a separate `RT_ICON` resource and emits a synthetic `RT_GROUP_ICON` that references them. The shipped executable confirms this precisely: `RT_ICON` id 1 with language 1046 and 296 bytes, `RT_ICON` id 2 with language 1046 and 744 bytes, and `RT_GROUP_ICON` id 101 with 34 bytes (the 6-byte group header plus 2 x 14-byte entry records). The image-to-resource binding was independently validated as size-consistent for both entries. Note that the `RT_ICON` ordinals (1, 2) are generated by rc.exe and are unrelated to the 101 in `resource.h`; this is the source of the frequent misunderstanding that `IDI_ICON1` "is" icon 1. It is not: 101 is the group, 1 is the first member image.

Both icon files are byte-identical. `Get-FileHash -Algorithm SHA256` returns `C585D6D521C8FF60CB072923A22889F24C5B18C2F79CCE92208782D8DC33A6D2` for `Code/TMSrv/icon1.ico` and for `Code/DBSrv/icon1.ico`, and both are 1,078 bytes with identical modification timestamps. Consequently there is no visual way to distinguish the Game Server from the DB Server in the Windows shell, taskbar, or any `RT_GROUP_ICON`-consuming tool. The rule "each executable carries its own bundle" is therefore satisfied structurally but not semantically.

**Rule workflow**:

```
resource.h  #define IDI_ICON1 101
      |
      v
TMSrv.rc:54 / DBSrv.rc:54   IDI_ICON1 ICON "icon1.ico"
      |
      v
rc.exe: preprocess -> 101 ICON "icon1.ico"
      |
      +--> parse icon1.ico directory (2 entries: 16x16@4bpp, 32x32@4bpp)
      +--> emit RT_ICON name=1 (296 B), RT_ICON name=2 (744 B)
      +--> emit RT_GROUP_ICON name=101 (34 B) referencing 1 and 2
      |
      v
TMSrv.res -> link.exe -> PE .rsrc
```

---

### Business Rule: BR-02 — The application icon must be the lowest-ID icon in the script

**Overview**:
The resource script carries an explicit invariant, stated in a comment shipped in both files, that the icon with the lowest ID value must be placed first so that the application icon stays consistent on all systems. This is the mechanism by which Windows' shell and the common dialogs choose the executable's representative icon.

**Detailed description**:

`Code/TMSrv/TMSrv.rc:52-53` and `Code/DBSrv/DBSrv.rc:52-53` both contain the two-line comment "Icon with lowest ID value placed first to ensure application icon / remains consistent on all systems." This comment is boilerplate emitted by the Visual C++ resource editor for any icon block, but it encodes a real Win32 rule: when the shell needs a single icon for a PE file it selects the icon group with the lowest resource ID, so the ordering and numbering inside the script determine what the user sees in Explorer, the Start menu shortcut, the Alt-Tab tile of an ungrouped window and the `WM_GETICON`/`ICON_BIG` query path. Since 101 is the only group icon in each script, and 101 is also the minimum, the invariant holds trivially today.

The practical consequence is that this rule and BR-04 are complementary, not redundant: BR-02 governs the shell-visible icon and is currently satisfied, while BR-04 governs the window-visible icon and is currently violated. The result is a running server whose title bar and taskbar button show the generic Win32 application icon while its .exe file in Explorer shows the correct custom icon. There is no code that reconciles these two, e.g. there is no `LoadImage(hInst, MAKEINTRESOURCE(101), IMAGE_ICON, ...)` anywhere in either source tree, and no `SetIcon`/`WM_SETICON` call (the only occurrences of `SetIcon` are absent entirely; the `HICON hIcon, hIconSm;` declaration that would have supported them is commented out at `Code/TMSrv/Server.cpp:4058` and `Code/DBSrv/Server.cpp:1196`).

Because the invariant is only about relative ordering, adding a second icon resource to either script is the only way to break it. Any future addition must be given a numeric ID greater than 101, otherwise the shell icon silently changes. Nothing in the build enforces the ordering — there is no test, no check and no generated guard — so the invariant survives purely by the fact that no one has edited the icon block since 2016.

**Rule workflow**:

```
shell requests file icon for TMSrv.exe
      |
      v
enumerate RT_GROUP_ICON entries in .rsrc  -> {101}
      |
      v
select minimum ID -> 101 -> load member images 1 (16x16) and 2 (32x32)
      |
      v
render file icon
```

---

### Business Rule: BR-03 — All resources are tagged Brazilian Portuguese, overriding the compiler default language

**Overview**:
Every resource in both bundles is tagged with `LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN` (MAKELANGID 0x0416 = 1046). This is asserted by an explicit `LANGUAGE` statement and is what actually ships, in spite of the resource compiler being invoked with a US English default language.

**Detailed description**:

`Code/TMSrv/TMSrv.rc:19` and `Code/DBSrv/DBSrv.rc:19` each contain `LANGUAGE LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN`, placed immediately after the `#if !defined(AFX_RESOURCE_DLL) || defined(AFX_TARG_PTB)` guard at line 18 and before the `TEXTINCLUDE` block. Because every resource declaration in the file (line 54 in TMSrv, lines 54 and 61 in DBSrv) appears after that statement and inside the same guard, all of them inherit the tag. There is exactly one language in each script; no neutral (`LANG_NEUTRAL`, 0x00) resource exists, and no second language block exists. The `AFX_TARG_PTB` guard is MFC boilerplate; `AFX_RESOURCE_DLL` is undefined for these projects, so the `||` short-circuits and the block is always compiled.

The build log shows rc.exe being started with `/l"0x0409"`, i.e. the project system's default language for the resource compiler is US English. The `/l` switch only supplies a language for resources that carry no `LANGUAGE` statement; an explicit `LANGUAGE` in the script takes precedence. That this precedence actually applied is directly observable in the shipped binary: the `RT_ICON`, `RT_GROUP_ICON` and dialog entries are all language 1046 (`pt-BR`), not 1033/1034, while the linker-generated `RT_MANIFEST` is 1033 (`en-US`). So within a single `.rsrc` section, the resource-compiler-authored entries are pt-BR and the linker-authored entry is en-US. The `/l 0x0409` switch is effectively dead configuration for these projects.

The runtime consequence is the standard Win32 language-fallback chain. `LoadIcon(hInstance, "MAINICON")` performs a lookup whose language is derived from the calling thread's locale; if no entry matches the preferred language, the loader falls back to the closest available language and finally to the first entry in the directory. With only pt-BR present, every lookup succeeds against 1046 regardless of the operator's Windows locale — which is why the icons display correctly in Explorer on non-Portuguese systems. The rule therefore has no functional downside today, but it does mean that adding a second-language resource later requires understanding that the neutral-language entry is the one that would win for non-pt-BR callers, and that `/l 0x0409` will not help.

**Rule workflow**:

```
msbuild -> rc.exe /l"0x0409" ... TMSrv.rc
      |
      v
preprocess: LANGUAGE LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN  (0x0416)
      |
      +--> explicit LANGUAGE overrides /l default
      |
      v
every RT_ICON / RT_GROUP_ICON / RT_DIALOG entry tagged 0x0416
      |
      v
linker adds RT_MANIFEST separately, tagged 0x0409 (en-US)
      |
      v
mixed-language .rsrc section: resources 1046, manifest 1033
```

---

### Business Rule: BR-04 — The window icon contract requires a string-named icon "MAINICON"; the bundle does not provide it

**Overview**:
Both servers register their window class with an icon obtained by *name*, using the literal `"MAINICON"`. The resource bundles expose their icon only by *ordinal* 101. The name lookup therefore fails, `LoadIconA` returns `NULL`, and both windows run with the system default application icon. This is the only rule in this component with an observable runtime effect.

**Detailed description**:

`Code/TMSrv/Server.cpp:3401` and `Code/DBSrv/Server.cpp:322` contain, respectively, `wc.hIcon = (HICON)LoadIcon((HINSTANCE)hInstance, "MAINICON");` and `wc.hIcon = (HICON)LoadIcon ((HINSTANCE)hInstance, "MAINICON");`. `LoadIcon` is the two-argument legacy form; with `"MAINICON"` as the second argument and no `MAKEINTRESOURCE` wrapper, the API treats the argument as a *pointer to a null-terminated string resource name* and searches `RT_GROUP_ICON` for that name. `MAINICON` is not defined in `Code/TMSrv/resource.h`, not defined in `Code/DBSrv/resource.h`, and not present as a resource name in either `.rc`. A repository-wide search for `MAINICON` across all `.h`, `.cpp` and `.rc` files returns only the two `LoadIcon` call sites. The identifier `MAINICON` is the conventional symbol name that the Visual C++ *application wizard* and the `WIN32` sample templates use for `IDI_APPICON`-style icons, which strongly suggests this code was adapted from a template whose `.rc` declared `MAINICON ICON "..."` and the rename to `IDI_ICON1` was never mirrored back into the source.

Direct evidence of failure is available in the shipped binary rather than being merely inferred. The PE resource inventory of `TMSrv.exe` lists four resources: `RT_ICON`/1, `RT_ICON`/2, `RT_GROUP_ICON`/101, `RT_MANIFEST`/1. There is no named `RT_GROUP_ICON`. The literal byte string `MAINICON` occurs exactly once in the whole 509,440-byte image, which is consistent with it existing only as the argument pushed to `LoadIconA` in `InitApplication`, not as a `.rsrc` name. A resource lookup for a missing name returns `NULL` and sets last error 1813/1812-class (`ERROR_RESOURCE_TYPE_NOT_FOUND` / `ERROR_RESOURCE_NAME_NOT_FOUND`); the code performs no error check and assigns the result straight to `wc.hIcon`.

The behaviour that follows is two distinct and contradictory outcomes for one asset. The shell still shows the correct icon, because it selects the lowest-numbered icon group (BR-02) and does not care about names. The window does not: a `WNDCLASS` with `hIcon = NULL` makes `CreateWindow` fall back to the system default application icon for the title bar, the taskbar button, Alt-Tab and `WM_GETICON`/`ICON_BIG`. Neither server compensates later — `WM_CREATE` (`Code/TMSrv/Server.cpp:4055-4079`, `Code/DBSrv/Server.cpp:1193-1209`) builds menus only, and the commented-out `HICON hIcon, hIconSm;` declarations at `Code/TMSrv/Server.cpp:4058` and `Code/DBSrv/Server.cpp:1196` show a `WM_SETICON` path was contemplated and removed. The same defect exists verbatim in both executables because the two `InitApplication` functions are near-identical copies.

**Rule workflow**:

```
WNDCLASS wc
wc.hIcon = LoadIcon(hInstance, "MAINICON")
      |
      v
loader: FindResource(hInst, "MAINICON", RT_GROUP_ICON)
      |
      +-- .rsrc contains RT_GROUP_ICON name=101 only  -> NOT FOUND
      |
      v
return NULL  (no error check in caller)
      |
      v
RegisterClass(&wc) with hIcon = NULL
      |
      v
CreateWindow -> window uses IDI_APPLICATION (system default)
      |
      +--> Explorer/shell icon path unaffected: lowest group icon 101 still used
```

---

### Business Rule: BR-05 — Menus are not part of the resource bundle; command IDs belong to the application range in Basedef.h

**Overview**:
Neither resource script contains a `MENU` block. The menu bar of each server window is constructed imperatively at runtime, and the command identifiers it uses (9000-9006 and 9050) are declared in `Code/Basedef.h`, not in `resource.h`. This rule defines the boundary of the resource bundle and corrects the assumption that "menu resources" are owned here.

**Detailed description**:

`wc.lpszMenuName` is set to `NULL` in both window classes (`Code/TMSrv/Server.cpp:3405`, `Code/DBSrv/Server.cpp:326`), which means the class itself requests no menu resource at all. `InitInstance` then calls `CreateMenu()` and passes the resulting empty handle to `CreateWindow` (`Code/TMSrv/Server.cpp:3416` and `:3420`; `Code/DBSrv/Server.cpp:337` and `:342`). The actual population happens in the `WM_CREATE` handler: `Code/TMSrv/Server.cpp:4055-4079` creates a `&File` popup with `IDC_EXIT`, a `&Mob` popup with `IDC_MOBRELOAD`, and a `&Sistema` popup with `IDC_REBOOT`, `IDC_SAVEALL`, `IDC_READGUILD`, `IDC_READGUILDNAME` and `IDC_READGAMECONFIG`, then calls `SetMenu(hWnd, hMenu)`; `Code/DBSrv/Server.cpp:1193-1209` does the same with a `&File` popup containing `IDC_EXIT` and a `&Sistema` popup containing a single `IDC_SHUTDOWNNP` item labelled `"&Reload MobBase"`. Menu labels are hard-coded ANSI string literals, so no localisation path exists for them (contrast BR-03, where the icon is at least language-tagged).

The command identifiers live in `Code/Basedef.h:31-39`: `IDC_EXIT 9000`, `IDC_MOBRELOAD 9001`, `IDC_REBOOT 9002`, `IDC_SAVEALL 9003`, `IDC_READGUILD 9004`, `IDC_READGUILDNAME 9005`, `IDC_READGAMECONFIG 9006`, and `IDC_SHUTDOWNNP 9050`. All are outside the range a resource compiler would allocate (the `_APS_NEXT_COMMAND_VALUE` sentinel is 40001 in both projects, and the whole 9000-block is hand-assigned), and none of them appears in either `resource.h`. Dispatch is by `switch (LOWORD(wParam))` in `WM_COMMAND` — `Code/TMSrv/Server.cpp:4081-4110` handles seven cases and `Code/DBSrv/Server.cpp:1211-1219` handles two. Because `IDC_MOBRELOAD`, `IDC_REBOOT`, `IDC_SAVEALL`, `IDC_READGUILD`, `IDC_READGUILDNAME` and `IDC_READGAMECONFIG` are defined in the shared `Basedef.h` but only ever appended in TMSrv, DBSrv defines six constants it never uses.

The architectural consequence is that the resource bundle has zero efferent coupling to the menu subsystem and the two `resource.h` files are *not* the ID authority for UI commands. Anyone extending the menu must edit `Basedef.h` and the `WM_CREATE`/`WM_COMMAND` pair, and must not expect the VS resource editor to see the command IDs at all — opening either `.rc` in the resource editor shows an empty command-ID symbol table (`_APS_NEXT_COMMAND_VALUE 40001`), which is correct but misleading relative to the actual running menu.

**Rule workflow**:

```
InitInstance -> hMainMenu = CreateMenu() -> CreateWindow(..., hMainMenu, ...)
      |
      v
WM_CREATE
   hMenu = CreateMenu()
   hSubMenu = CreatePopupMenu(); AppendMenu(hSubMenu, MF_STRING, IDC_EXIT, "&Exit")
   AppendMenu(hMenu, MF_STRING | MF_POPUP, hSubMenu, "&File")
   ... repeat for &Mob / &Sistema ...
   SetMenu(hWnd, hMenu)
      |
      v
user click -> WM_COMMAND, LOWORD(wParam) = IDC_*
      |
      v
switch in WM_COMMAND (TMSrv 7 cases, DBSrv 2 cases)
```

---

### Business Rule: BR-06 — DBSrv defines one dialog template that nothing instantiates, and it ships under a string name instead of its ordinal

**Overview**:
`Code/DBSrv/DBSrv.rc:61-66` declares `IDD_MAINDLG DIALOGEX 0, 0, 415, 280` with caption `"DBSRV"`, a fixed set of window styles, font `10, "System", 700` and an empty body. No code in the repository ever creates this dialog. In addition, the identifier used in the script does not match the identifier defined in the header, so the dialog is compiled under the string name `"IDD_MAINDLG"` rather than ordinal 103.

**Detailed description**:

The template is exactly five effective lines: the `DIALOGEX` line with size 415 x 280 dialog units, a `STYLE` line combining `DS_SETFONT | DS_MODALFRAME | DS_3DLOOK | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU`, `CAPTION "DBSRV"`, `FONT 10, "System", 700, 0, 0x0`, and an empty `BEGIN`/`END` pair. There are no controls. It is a legacy main-window shell left behind by an earlier design in which DBSrv was to be a dialog-based application, before it became a `CreateWindow`-based application with a painting `MainWndProc`. Its `WS_POPUP | WS_VISIBLE` combination plus `DS_MODALFRAME` is the signature of a `DialogBox`-driven main window, not a child or modal helper.

No consumer exists. A repository-wide search for `DialogBox`, `DialogBoxParam`, `CreateDialog`, `CreateDialogParam` and `EndDialog` across all `.cpp` and `.h` files returns only the two `resource.h` `#define` lines; not a single call site. All 156 `MessageBox`/`MessageBoxA` calls in the codebase are direct calls that pass string literals and `MB_*` flags rather than dialog templates, so even the error-reporting path does not touch this resource. DBSrv's actual main window is created with `CreateWindow("MainClass", "DB Server", WS_OVERLAPPED | WS_CLIPCHILDREN | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, ..., 800, 600, ...)` at `Code/DBSrv/Server.cpp:339-342` — a different size (800 x 600 pixels vs 415 x 280 DLU) and a different style set. The dialog is therefore shipped dead weight in `DBSrv.res`; the compiled file is 1,320 bytes versus 1,204 for `TMSrv.res`, and its UTF-16 content includes `IDD_MAINDLG`, `DBSRV` and `System`, confirming the template is really in the artifact.

The naming defect is separate and more interesting. `Code/DBSrv/resource.h:26` defines `IDD_MainDlg` (capital `M`, lower-case `ainlg`), while `Code/DBSrv/DBSrv.rc:61` and `:77` use `IDD_MAINDLG` (all caps). The rc.exe preprocessor is a C preprocessor and C identifiers are case-sensitive, so `IDD_MAINDLG` does not expand; rc.exe treats an unexpanded identifier in the resource-name position as a *string literal* name. This is confirmed by the compiled artifact: the UTF-16 string `IDD_MAINDLG` appears in `Code/DBSrv/Release/DBSrv.res`, immediately preceded by the `0xFFFF, 0x0005` marker for an ordinal resource type `RT_DIALOG` (5), followed by the dialog template's 415/280 dimensions and the `DBSRV` caption. No `0xFFFF, 0x0067` pair (ordinal 103) exists anywhere in that file. The contrast case validates the method: `IDI_ICON1` is correctly defined and therefore does *not* appear as a string in either `.res`, and its group icon appears as ordinal 101. The practical implication is that `MAKEINTRESOURCE(103)` would fail to find this dialog; only the literal name `"IDD_MAINDLG"` would work. A secondary documentation inconsistency sits on the same resource: `GUIDELINES DESIGNINFO` at `Code/DBSrv/DBSrv.rc:77-80` declares `RIGHTMARGIN, 257` for a dialog that is 415 units wide, a stale editor guideline.

**Rule workflow**:

```
resource.h:26   #define IDD_MainDlg 103
DBSrv.rc:61     IDD_MAINDLG DIALOGEX 0,0,415,280
      |
      v
rc.exe preprocessor: IDD_MAINDLG != IDD_MainDlg (case-sensitive) -> NOT expanded
      |
      v
identifier in name position treated as string literal
      |
      v
DBSrv.res: RT_DIALOG (ordinal type 5), NAME = "IDD_MAINDLG" (UTF-16 string)
           ordinal 103 does not exist in the file
      |
      v
link.exe -> DBSrv.exe .rsrc ; no code ever calls DialogBox/CreateDialog
      |
      v
resource is dead at runtime
```

---

### Business Rule: BR-07 — Resource IDs are allocated per project by the IDE; every declared ID must bind to a resource

**Overview**:
Each project owns an independent `resource.h` produced by the Visual C++ resource editor, whose `_APS_NEXT_*` sentinels encode the allocation frontier. The rule implied by that scheme is that every `#define` in the header correspond to a resource declared in the `.rc`, and conversely. DBSrv violates the rule three ways.

**Detailed description**:

The headers are small and entirely machine-generated in structure: a licence comment (`Code/TMSrv/resource.h:1-17`, `Code/DBSrv/resource.h:1-18`), the `//{{NO_DEPENDENCIES}}` marker, the ID `#define`s, and an `#ifdef APSTUDIO_INVOKED` block defining `_APS_NEXT_RESOURCE_VALUE`, `_APS_NEXT_COMMAND_VALUE`, `_APS_NEXT_CONTROL_VALUE` and `_APS_NEXT_SYMED_VALUE`. In TMSrv the single ID is `IDI_ICON1 101` (line 22) and the sentinels are 102 / 40001 / 1001 / 101 (lines 28-31). In DBSrv the IDs are `IDI_ICON1 101`, `IDD_DIALOG1 102`, `IDD_MainDlg 103` (lines 24-26) with sentinels 103 / 40001 / 1001 / 101 (lines 32-35). Both `_APS_NEXT_SYMED_VALUE` values are 101, a leftover of the wizard default rather than a meaningful allocation point.

Three inconsistencies follow. First, `IDD_DIALOG1` = 102 is defined but there is no resource with that ID or name in `DBSrv.rc`, so the symbol is dead. Second, `IDD_MainDlg` = 103 is defined but never bound as an ordinal, for the case-sensitivity reason in BR-06; the resource exists only as a string name. Third, `_APS_NEXT_RESOURCE_VALUE` is 103 in DBSrv while the highest *bound* ordinal is 101 — so the sentinel reflects the two unbound dialog IDs rather than the actual resource set, meaning the editor would hand out 103 for a new resource and collide with the stale `IDD_MainDlg` definition. Note also that the two projects diverge: TMSrv's sentinel is 102 against a highest ID of 101, which is self-consistent. The divergence is a side effect of DBSrv having had a dialog deleted from its `.rc` without the corresponding `resource.h` lines ever being removed — that is also the most economical explanation for `IDD_DIALOG1` existing at all. The file's git history is minimal and consistent with that reading: `resource.h` was last changed in 2018-05 (`cff828d`, `12c5198`, "Update resource.h") and untouched since, apart from the 2026-11-14 project-file upgrade commit `dfe35d0`.

The practical risk is collision on the next edit rather than a current defect. Because rc.exe tolerates both unused `#define`s and a dialog bound only by string, nothing fails today; but re-opening `DBSrv.rc` in the resource editor and adding a control or dialog would consume the `_APS_NEXT_RESOURCE_VALUE` counter, and the editor's symbol browser would present `IDD_MainDlg` as a valid existing dialog symbol. The ID scheme is not validated by the build in any way.

**Rule workflow**:

```
VS resource editor allocates ID = _APS_NEXT_RESOURCE_VALUE, then increments it
      |
      v
resource.h updated:  #define <SYMBOL> <id>
      |
      v
.rc updated:         <SYMBOL> <TYPE> <body>
      |
      v
rc.exe preprocess: SYMBOL must match exactly (case-sensitive)
      |
      +-- match   -> ordinal resource
      +-- no match-> string-named resource (silent)
      |
      v
DBSrv state: 101 bound (icon), 102 unbound (#define only),
             103 unbound as ordinal (bound as string "IDD_MAINDLG")
```

---

### Business Rule: BR-08 — The resource bundle carries no version metadata

**Overview**:
Neither resource script contains a `VERSIONINFO` block. The shipped TMSrv executable contains no `RT_VERSION` resource. Consequently the Windows file-properties Version tab is empty for both servers, and version identity is carried only by the compile-time constant `APP_VERSION 6975`.

**Detailed description**:

The complete resource inventory of `TMSrv.exe` is four entries: `RT_ICON` id 1 (296 bytes, language 1046), `RT_ICON` id 2 (744 bytes, 1046), `RT_GROUP_ICON` id 101 (34 bytes, 1046) and `RT_MANIFEST` id 1 (381 bytes, 1033). There is no `RT_VERSION` (type 16), no `RT_STRING` (6), no `RT_MENU` (4), no `RT_DIALOG` (5). For DBSrv, the only addition visible in the compiled `DBSrv.res` is the dead `RT_DIALOG` from BR-06; still no version block. The `.rc` files confirm the same by omission — `Code/TMSrv/TMSrv.rc` is 69 lines and `Code/DBSrv/DBSrv.rc` is 98 lines, and neither contains the string `VERSIONINFO`, `FILEVERSION`, `PRODUCTVERSION`, `VS_VERSION_INFO` or `StringFileInfo`. Each `.rc` ends with the empty `TEXTINCLUDE 3` / "Generated from the TEXTINCLUDE 3 resource." block, which is where the editor would place a version block if one had been added.

The functional impact is confined to diagnostics and deployment hygiene rather than to server behaviour. Operators who right-click `TMSrv.exe` see no company, product, file version, description or copyright; typically the fields are absent entirely rather than blank, because the whole resource is missing. Antivirus reputation, patch-management inventory and any update tooling that keys on `VS_FIXEDFILEINFO` will see nothing. There is no `GetFileVersionInfo`/`VerQueryValue` call anywhere in the source, so no code path depends on the absence being benign — the gap is simply unused capability.

The de facto version carrier is `Code/Basedef.h:42`, `#define APP_VERSION 6975`, which is a protocol/build number consumed by the game protocol rather than by Windows. It is not tied to the resource bundle, is not surfaced in any UI, and changes independently of the executables' file properties. The licensing text that a version block would normally carry does exist in the repository — the GPLv3 notice repeated as a header comment in `Code/TMSrv/resource.h:1-17` and `Code/DBSrv/resource.h:1-18` ("Copyright (C) {2015} {VK, Charles TheHouse}") — but it is never compiled into the binaries' metadata.

**Rule workflow**:

```
TMSrv.rc / DBSrv.rc
      |
      +-- IDI_ICON1 ICON      -> RT_ICON / RT_GROUP_ICON
      +-- IDD_MAINDLG DIALOGEX-> RT_DIALOG  (DBSrv only, dead)
      +-- (no VERSIONINFO)    -> nothing emitted
      |
      v
link.exe -> .rsrc has no RT_VERSION
      |
      v
Explorer Properties -> Version tab absent
      |
      v
identity carried only by APP_VERSION 6975 (Basedef.h:42), not by the PE
```

---

### Business Rule: BR-09 — The only manifest in the image is the linker default, declaring asInvoker

**Overview**:
The shipped `TMSrv.exe` contains one `RT_MANIFEST` resource (type 24, id 1, language 1033, 381 bytes). It is generated by the MSVC linker, not authored in the resource script, and declares `requestedExecutionLevel level='asInvoker' uiAccess='false'`. It contains no DPI-awareness declaration and no side-by-side assembly dependency.

**Detailed description**:

The manifest body is exactly the `trustInfo` skeleton: an `<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>` root, a `<trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">` with a `<security><requestedPrivileges><requestedExecutionLevel level='asInvoker' uiAccess='false' /></requestedPrivileges></security></trustInfo>`, and nothing else — no `<application>`, no `<compatibility>` section with supported-OS GUIDs, no `<windowsSettings>` with `dpiAware`/`dpiAwareness`/`longPathAware`, and no `<dependency>` on the common controls v6 assembly. Neither `.rc` contains a `1 RT_MANIFEST "app.manifest"` line and neither project file sets `<AdditionalManifestFiles>` or `<GenerateManifest>`, so this is pure linker default behaviour (link.exe synthesizes an `asInvoker` manifest when none is supplied). Its language 1033 differs from the 1046 of the resource-compiler-authored entries, which is how it can be identified as a separate authoring stage inside one `.rsrc` section.

The consequences are consistent with a server console built for Windows XP-era targets and never revisited. `asInvoker` means the process never requests elevation: TMSrv and DBSrv cannot bind their listening ports below 1024, but both use unprivileged ports (`GAME_PORT 8281`, `DB_PORT 7514`, `ADMIN_PORT 8895` at `Code/Basedef.h:44-46`), so the choice is deliberate and appropriate. The absence of a `dpiAware` declaration means both windows are bitmap-scaled by the desktop compositor on high-DPI displays, which affects the readability of the `Fixedsys` 12px font created at `Code/TMSrv/Server.cpp:3473` and of the `DrawConfig` output painted in `WM_PAINT` (`Code/TMSrv/Server.cpp:4047-4053`). The absence of a common-controls v6 dependency matters little here because neither server uses common-controls widgets — the UI is a plain `CreateWindow` window, a programmatically built menu bar, and `MessageBoxA` calls.

Because the manifest is not in this component's authored files, it is listed here for completeness of the shipped resource inventory rather than as a rule the resource bundle enforces. It is the only resource in the image that this component does not control.

**Rule workflow**:

```
rc.exe processes .rc          (no RT_MANIFEST authored -> nothing)
      |
      v
link.exe, no /MANIFESTFILE  -> synthesize default manifest
      +-- requestedExecutionLevel level='asInvoker' uiAccess='false'
      +-- language 1033, name 1, type RT_MANIFEST(24)
      |
      v
merged into .rsrc alongside the 1046-tagged rc.exe resources
      |
      v
process runs unelevated; DPI virtualisation applies to both windows
```

---

### Business Rule: BR-10 — Only the .rc files are compiled; the icon is resolved by rc.exe relative to the script

**Overview**:
Build inclusion is declared in the project files: `TMSrv.rc` and `DBSrv.rc` are `<ResourceCompile>` items, `resource.h` is a `<ClInclude>`, and `icon1.ico` is a `<None>` item. The icon nevertheless reaches the compiler because rc.exe resolves `"icon1.ico"` relative to the directory containing the `.rc`.

**Detailed description**:

`Code/TMSrv/TMSrv.vcxproj:187` declares `<ResourceCompile Include="TMSrv.rc" />`, line 182 declares `<ClInclude Include="resource.h" />` and line 190 declares `<None Include="icon1.ico" />`; the filters file maps `.rc` and `.ico` into the "Resource Files" filter (`Code/TMSrv/TMSrv.vcxproj.filters:349-356`). DBSrv is identical at `Code/DBSrv/DBSrv.vcxproj:116`, `:111` and `:119` (filters at `:85-92`). No `<Image>` item type is used, and no `AdditionalIncludeDirectories` or `ResourceOutputFileName` is customised for the resource compile step, so the icon's inclusion depends entirely on the `.rc` living in the same directory as the `.ico` — which it does, and which the tlog proves was exercised: `rc.read.1.tlog` lists `E:\...\CODE\TMSRV\ICON1.ICO` as a consumed input. MSBuild's incremental mechanism therefore does track the icon, because rc.exe's own dependency log records it even though the project system does not model it as a compile input. The link is implicit, not declared.

The output path is also implicit and has a visible side effect. Without `ResourceOutputFileName`, MSBuild passes `/fo"TMSRV\RELEASE\TMSRV.RES"` — a path relative to the project directory — producing `Code/TMSrv/TMSrv/Release/TMSrv.res`, a nested duplicate of the project directory. Both projects exhibit this: `Code/TMSrv/Release/TMSrv.res` and `Code/TMSrv/TMSrv/Release/TMSrv.res` coexist (1,204 bytes each), as do `Code/DBSrv/Release/DBSrv.res` and `Code/DBSrv/DBSrv/Release/DBSrv.res` (1,320 bytes each). The nested directories also hold the full object files, `.iobj`/`.ipdb`, `vc143.pdb` and `.tlog` sets, so each project carries two parallel intermediate trees. This is untracked build residue under `.gitignore`'s `[Rr]elease/` rule, not a repository defect, but it means the `.res` file that actually got linked is not necessarily the one in the obvious `Release/` directory.

One further consequence of `icon1.ico` being `<None>` rather than `<Image>`: the resource view in Visual Studio will not offer to edit the icon, and the resource editor's symbol list will not reflect it. Combined with BR-11, this means the icon is effectively managed as a raw binary file plus a one-line `.rc` declaration, with no IDE-mediated path.

**Rule workflow**:

```
msbuild (Release|Win32)
      |
      +-- <ResourceCompile> TMSrv.rc  -> dispatched to rc.exe
      +-- <ClInclude>       resource.h -> included by the .rc at line 3
      +-- <None>            icon1.ico  -> not dispatched; found by rc.exe
      |
      v
rc.exe /l"0x0409" /nologo /fo"TMSRV\RELEASE\TMSRV.RES" TMSRV.RC
      |
      +-- resolves "icon1.ico" relative to the .rc directory
      +-- writes TMSrv\Release\TMSrv.res (nested)
      |
      v
rc.read.1.tlog records ICON1.ICO  -> incremental build tracks it
      |
      v
link.exe consumes the .res plus its own manifest
```

---

### Business Rule: BR-11 — The .aps file is a machine-local editor cache and is never a build input or versioned artifact

**Overview**:
`Code/DBSrv/DBSrv.aps` exists in the working tree (83,420 bytes) but is not part of the repository, is not referenced by any project file, and is not read by any build step. It is Visual Studio's binary resource-editor cache, and it holds a stale absolute path from a previous checkout location.

**Detailed description**:

`.gitignore:77` contains `*.aps` under the "Visual C++ cache files" section. `git check-ignore -v Code/DBSrv/DBSrv.aps` resolves to that rule, and `git status --porcelain --ignored` reports the file as `!!` (ignored). `git ls-files` confirms that no `.aps` file is tracked; the only tracked resource files are `Code/DBSrv/DBSrv.rc`, `Code/DBSrv/icon1.ico`, `Code/DBSrv/resource.h`, `Code/TMSrv/TMSrv.rc`, `Code/TMSrv/icon1.ico`, `Code/TMSrv/resource.h`. Neither `DBSrv.vcxproj` at 124 lines nor `DBSrv.vcxproj.filters` at 94 lines contains any `.aps` reference. So the file is untracked local state — the premise that `.aps` is "a checked-in generated resource cache" is not supported by the repository evidence; it is an untracked generated cache that happens to be present on this machine.

Its content is nonetheless diagnostic. Extracting ASCII strings yields the resource symbol table (`IDI_ICON1`, `IDD_DIALOG1`, `IDD_MainDlg`, `_APS_NEXT_RESOURCE_VALUE`, `_APS_NEXT_COMMAND_VALUE 40001`, `_APS_NEXT_CONTROL_VALUE 1001`, `_APS_NEXT_SYMED_VALUE`) plus a very large volume of Windows SDK symbol text (`WM_*`, `IDI_*`, `SUBLANG_*`, `WINAPI_*`) that the editor caches for its pickers, and one absolute source path: `E:\wyd\test-setup\w2pp-agateownz\Code\DBSrv\DBSrv.rc`. That path does not exist in the current checkout, so the cache is bound to a different, earlier directory name (`w2pp-agateownz` versus `w2pp`). This is the canonical failure mode of `.aps` files: they embed absolute paths, go stale on rename or relocation, and silently repopulate the editor's symbol browser with entries that no longer exist in the `.rc` — here, `IDD_DIALOG1` and `IDD_MainDlg`, both of which are exactly the two unbound IDs identified in BR-07. The cache is thus the archaeological record of the dialog deletion that created those dead IDs.

Timestamps corroborate staleness: `DBSrv.aps` was last written 2026-08-18 13:33:26 while `DBSrv.rc` carries 2026-11-14 15:34:10 (a checkout-time write; the file's last real commit is `12c5198`, 2018-05-22). There is no `.aps` for TMSrv at all, which is consistent with the file being created only when someone opens a resource editor session against a project. Because the file is ignored, correctly, its only cost is local disk and the risk that a future editor session inherits the stale symbol set; there is no build-hygiene defect to fix in the repository itself, only a working-tree artifact that should not be treated as component source.

**Rule workflow**:

```
git status --ignored
      |
      +-- .gitignore:77  *.aps  -> Code/DBSrv/DBSrv.aps  reported as "!!"
      |
      v
git ls-files -> no .aps tracked
vcxproj      -> no .aps item
      |
      v
build: rc.exe reads DBSrv.rc + resource.h + icon1.ico  (never the .aps)
      |
      v
Visual Studio resource editor (only writer/reader of .aps)
      +-- caches symbol table: IDI_ICON1, IDD_DIALOG1, IDD_MainDlg
      +-- caches absolute path: E:\wyd\test-setup\w2pp-agateownz\Code\DBSrv\DBSrv.rc
      +-- stale relative to the .rc and to the current checkout path
```

---

### Business Rule: BR-12 — Both projects compile as MultiByte, so all resource lookups bind to ANSI APIs

**Overview**:
Both projects set `<CharacterSet>MultiByte</CharacterSet>`, so `UNICODE` and `_UNICODE` are undefined (or defined as `_MBCS`). Every Win32 call in the window setup therefore resolves to its narrow `...A` variant: `LoadIconA`, `LoadCursorA`, `RegisterClassA`, `CreateWindowA`, `SetMenu` (character-set agnostic). Dialog templates authored as ANSI `DIALOGEX` are handled by the ANSI dialog machinery.

**Detailed description**:

`Code/TMSrv/TMSrv.vcxproj:27` and `:34` (Debug and Release) and `Code/DBSrv/DBSrv.vcxproj:27` and `:34` all specify `<CharacterSet>MultiByte</CharacterSet>`. The `.cpp` sources reinforce this with ANSI-only idioms throughout: `_CRT_SECURE_NO_WARNINGS` is set in the preprocessor definitions (`Code/TMSrv/TMSrv.vcxproj:61,79`; `Code/DBSrv/DBSrv.vcxproj:61,83`), `sprintf`/`strcpy`/`fopen_s` are used in preference to wide variants, `MessageBoxA` is called explicitly in the DBSrv menu handler (`Code/DBSrv/Server.cpp:1229,1241`), and the window class name and window titles are narrow literals (`"MainClass"`, `"Game Server"`, `"DB Server"`). The icon lookup at `Code/TMSrv/Server.cpp:3401` is written as `LoadIcon(...)` with a narrow literal, which the preprocessor maps to `LoadIconA` precisely because `UNICODE` is absent; in a Unicode build the same source line would require `L"MAINICON"` to compile, and would then also still fail to find the resource (BR-04).

Consistency here matters because it is what makes BR-06's string-named dialog reachable at all in principle: an ANSI consumer passing `"IDD_MAINDLG"` to `DialogBoxA` would match the string-named resource. It is also why the dialog template's `FONT 10, "System", 700, 0, 0x0` line is legal — `"System"` is a raster font name that exists as an ANSI face, and `DS_SETFONT` in an ANSI template stores an 8-bit face name. The remaining windows use a 12-pixel `Fixedsys` font created explicitly at `Code/TMSrv/Server.cpp:3473`, again an ANSI raster face, chosen for monospaced log rendering.

The rule has no current defect, only two latent constraints. First, the component cannot be converted to Unicode without touching the resource scripts (`DIALOGEX` would need `"System"` kept as an 8-bit face or migrated, and the manifest/version situation would need revisiting) and every window-setup call site. Second, because both bundles are ANSI and single-language (BR-03), any future localisation work would face both dimensions at once: a charset change and a language-table change, in a component that currently has no test coverage of any kind (see Section 11).

**Rule workflow**:

```
vcxproj: CharacterSet = MultiByte  -> UNICODE / _UNICODE undefined
      |
      v
compilation unit: LoadIcon(...) -> LoadIconA(...)
                  CreateWindow(...) -> CreateWindowA(...)
                  RegisterClass(...) -> RegisterClassA(...)
      |
      v
.rc authored with Windows.h (ANSI syntax), DIALOGEX + "System" raster font
      |
      v
.res / PE resources are charset-agnostic binaries; the consumer side is ANSI
      |
      v
string-named resources ("MAINICON", "IDD_MAINDLG") compare as 8-bit ANSI names
```

---

## 4. Component Structure

```
Code/
├── TMSrv/
│   ├── TMSrv.rc                   # 69 lines, UTF-16LE+BOM, 3,274 B. Icon only (line 54).
│   │                              #   LANGUAGE pt-BR (19), #include resource.h (3),
│   │                              #   #include Windows.h (10), empty TEXTINCLUDE 3.
│   ├── resource.h                 # 33 lines, VS-generated. IDI_ICON1 = 101 (22).
│   │                              #   _APS_NEXT_* at 28-31.
│   ├── icon1.ico                  # 1,078 B. 2 images: 16x16 + 32x32, both 16-colour/4bpp.
│   ├── TMSrv.vcxproj              # ResourceCompile TMSrv.rc (187); ClInclude resource.h (182);
│   │                              #   None icon1.ico (190); MultiByte (27,34); v143 (28,35)
│   ├── TMSrv.vcxproj.filters      # rc + ico mapped to "Resource Files" (349-356)
│   ├── Server.cpp                 # Consumer: InitApplication (3392), LoadIcon "MAINICON" (3401),
│   │                              #   RegisterClass (3408), InitInstance (3414), CreateMenu (3416),
│   │                              #   CreateWindow (3417), WM_CREATE menus (4055-4079),
│   │                              #   WM_COMMAND (4081-4110)
│   ├── TMSrv/Release/             # nested intermediate tree: TMSrv.res 1,204 B, .obj, .iobj,
│   │                              #   .ipdb, vc143.pdb, .tlog (rc/link/CL)
│   └── Release/                   # second intermediate tree: TMSrv.res 1,204 B
│
└── DBSrv/
    ├── DBSrv.rc                   # 98 lines, UTF-16LE+BOM, 4,482 B.
    │                              #   Icon (54); Dialog IDD_MAINDLG DIALOGEX 415x280 (61-66);
    │                              #   DESIGNINFO RIGHTMARGIN 257 (74-82, APSTUDIO only)
    ├── resource.h                 # 37 lines. IDI_ICON1 = 101 (24); IDD_DIALOG1 = 102 (25);
    │                              #   IDD_MainDlg = 103 (26); _APS_NEXT_* at 32-35
    ├── icon1.ico                  # 1,078 B, byte-identical to TMSrv's (SHA-256 C585D6D5...)
    ├── DBSrv.aps                  # 83,420 B. UNTRACKED, gitignored (.gitignore:77).
    │                              #   VS resource-editor cache; stale abs path
    │                              #   E:\wyd\test-setup\w2pp-agateownz\Code\DBSrv\DBSrv.rc
    ├── DBSrv.vcxproj              # ResourceCompile DBSrv.rc (116); ClInclude resource.h (111);
    │                              #   None icon1.ico (119); MultiByte (27,34); v143 (28,35)
    ├── DBSrv.vcxproj.filters      # rc + ico mapped to "Resource Files" (85-92)
    ├── Server.cpp                 # Consumer: InitApplication (314), LoadIcon "MAINICON" (322),
    │                              #   RegisterClass (329), InitInstance (335), CreateMenu (337),
    │                              #   CreateWindow (339), WM_CREATE menus (1193-1209),
    │                              #   WM_COMMAND (1211-1219)
    ├── DBSrv/Release/             # nested intermediate tree: DBSrv.res 1,320 B + objects
    └── Release/                   # second intermediate tree: DBSrv.res 1,320 B
```

Shared definitions that the component's consumers rely on but does not own:

```
Code/Basedef.h
├── :31-39   IDC_EXIT 9000, IDC_MOBRELOAD 9001, IDC_REBOOT 9002, IDC_SAVEALL 9003,
│            IDC_READGUILD 9004, IDC_READGUILDNAME 9005, IDC_READGAMECONFIG 9006,
│            IDC_SHUTDOWNNP 9050
└── :42       APP_VERSION 6975
```

Toolchain recorded in the build state (`Code/TMSrv/TMSrv/Release/TMSrv.tlog/TMSrv.lastbuildstate`): `PlatformToolSet=v143`, `VCToolsVersion=14.44.35207`, `TargetPlatformVersion=10.0.26100.0`, configuration `Release|Win32`.

---

## 5. Dependency Analysis

### Internal dependencies

```
Code/TMSrv/TMSrv.vcxproj  ──<ResourceCompile>──>  Code/TMSrv/TMSrv.rc
Code/TMSrv/TMSrv.vcxproj  ──<ClInclude>───────>  Code/TMSrv/resource.h
Code/TMSrv/TMSrv.vcxproj  ──<None>────────────>  Code/TMSrv/icon1.ico
Code/TMSrv/TMSrv.rc       ──#include──────────>  Code/TMSrv/resource.h        (TMSrv.rc:3)
Code/TMSrv/TMSrv.rc       ──"icon1.ico"───────>  Code/TMSrv/icon1.ico         (TMSrv.rc:54)
Code/TMSrv/TMSrv.vcxproj.filters ─────────────>  Code/TMSrv/TMSrv.rc, icon1.ico, resource.h

Code/DBSrv/DBSrv.vcxproj  ──<ResourceCompile>──>  Code/DBSrv/DBSrv.rc
Code/DBSrv/DBSrv.vcxproj  ──<ClInclude>───────>  Code/DBSrv/resource.h
Code/DBSrv/DBSrv.vcxproj  ──<None>────────────>  Code/DBSrv/icon1.ico
Code/DBSrv/DBSrv.rc       ──#include──────────>  Code/DBSrv/resource.h        (DBSrv.rc:3)
Code/DBSrv/DBSrv.rc       ──"icon1.ico"───────>  Code/DBSrv/icon1.ico         (DBSrv.rc:54)

Code/TMSrv/Server.cpp     ──string literal────>  "MAINICON"  (UNRESOLVED, see BR-04)
Code/DBSrv/Server.cpp     ──string literal────>  "MAINICON"  (UNRESOLVED, see BR-04)
Code/TMSrv/Server.cpp     ──WM_COMMAND────────>  IDC_* (Code/Basedef.h:31-39)  [not this component]
Code/DBSrv/Server.cpp     ──WM_COMMAND────────>  IDC_* (Code/Basedef.h:31-39)  [not this component]

Code/DBSrv/DBSrv.aps      ──(no edges in or out)──      [gitignored, no build reference]
```

The component has no dependency on any other project source file. It does not include `Basedef.h` or `Server.h`, and no source file `#include`s either `resource.h` — the two `resource.h` files are included exclusively by their own `.rc` (`TMSrv.rc:3`, `DBSrv.rc:3`) and referenced as project items. That is why the `IDD_MainDlg` casing mismatch (BR-06) escaped the compiler: `resource.h` is only ever seen by rc.exe, never by cl.exe, so no C++ code ever had a chance to fail on it.

### External dependencies

| Dependency | Version / identity | Purpose | Evidence |
|------------|--------------------|---------|----------|
| `rc.exe` (Microsoft Resource Compiler) | MSVC 14.44.35207 (VS 2022) | Compiles `.rc` → `.res`; runs the C preprocessor over the script; `/l"0x0409"` default language | `Code/TMSrv/TMSrv/Release/TMSrv.tlog/rc.command.1.tlog`, `TMSrv.lastbuildstate` |
| `link.exe` | MSVC 14.44.35207 | Merges `.res` into the PE `.rsrc`; synthesizes the default `asInvoker` manifest | shipped `TMSrv.exe` RT_MANIFEST (381 B, lang 1033) |
| Windows SDK headers | 10.0.26100.0 | `Windows.h` (`.rc:10`) pulls `WINUSER.RH`, `WINNT.RH`, `COMMCTRL.RH`, `DDE.RH`, `DLGS.H`, `WINVER.H`, `SDV_DRIVERSPECS.H`, `VERRSRC.H`, `SAL.H`, `CONCURRENCYSAL.H`, `SPECSTRINGS.H` | `rc.read.1.tlog` input list |
| `afxres.h` | MFC/ATL (not on the machine's include path, not currently included) | Referenced only as the `TEXTINCLUDE 2` payload text (`TMSrv.rc:34`, `DBSrv.rc:34`); the actual include at line 10 is `Windows.h` | `.rc` lines 10 and 34 |
| Windows shell / `user32` | OS-provided | Shell icon selection from `RT_GROUP_ICON`; `LoadIconA`, `RegisterClassA`, `CreateWindowA`, `SetMenu` at runtime | `Server.cpp` call sites; shipped PE resource tree |

---

## 6. Afferent and Efferent Coupling

Coupling is measured per element of the component (files here; the component has no classes or structs). `Ca` counts distinct elements outside the element that depend on it; `Ce` counts distinct elements the element depends on. Build-tool consumers (`rc.exe`, `link.exe`) are counted once each where applicable, since they are genuine runtime-time coupling to the toolchain rather than to project code.

| Element | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Critical |
|---------|------------------------|------------------------|----------|
| `Code/TMSrv/TMSrv.rc` | 2 — `TMSrv.vcxproj` (`ResourceCompile`, 187), `TMSrv.vcxproj.filters` (349) | 2 — `resource.h` (line 3), `icon1.ico` (line 54) | Low |
| `Code/TMSrv/resource.h` | 2 — `TMSrv.rc` (line 3), `TMSrv.vcxproj` (182) | 0 | Low |
| `Code/TMSrv/icon1.ico` | 2 — `TMSrv.rc` (line 54), `TMSrv.vcxproj` (190, as `<None>`) | 0 | Low |
| `Code/DBSrv/DBSrv.rc` | 2 — `DBSrv.vcxproj` (116), `DBSrv.vcxproj.filters` (85) | 2 — `resource.h` (line 3), `icon1.ico` (line 54) | Medium (contains the casing defect) |
| `Code/DBSrv/resource.h` | 2 — `DBSrv.rc` (line 3), `DBSrv.vcxproj` (111) | 0 | Medium (two unbound IDs) |
| `Code/DBSrv/icon1.ico` | 2 — `DBSrv.rc` (line 54), `DBSrv.vcxproj` (119, as `<None>`) | 0 | Low |
| `Code/DBSrv/DBSrv.aps` | 0 — no build or source reference | 0 | None |
| **Component aggregate** | **2** — `TMSrv.vcxproj`, `DBSrv.vcxproj` | **0** — no outbound dependency on project code | **Low** |

The aggregate figures match the pre-supplied architectural measurement (afferent 2, efferent 0) when afferents are counted at the project-file level, and the component is fully acyclic: nothing it declares is consumed by any `.cpp`. Cohesion is high by construction — each file's single responsibility is unambiguous — but that cohesion is partly accidental, since the only functional contract (the `"MAINICON"` name) is broken and therefore imposes no real coupling.

Two coupling anomalies deserve noting. First, `Code/DBSrv/DBSrv.rc` and `Code/TMSrv/TMSrv.rc` are near-duplicates (identical through line 54) yet fully decoupled; nothing prevents them diverging, and DBSrv already has. Second, the true coupling that matters for BR-04 — `Server.cpp` → `"MAINICON"` — is a *runtime-only, unresolved* dependency that no static analysis of the resource bundle would reveal. The component's measured `Ca = 2` understates its real surface: the two `InitApplication` functions should be afferent edges but are not, because they name a resource that does not exist.

---

## 7. Endpoints

This component exposes no endpoints. It is a static, build-time-only resource artifact: no REST, GraphQL, gRPC, CLI, socket, or file-drop interface is defined in `TMSrv.rc`, `DBSrv.rc`, `resource.h`, `icon1.ico`, or `DBSrv.aps`. Its consumers are the MSVC resource compiler at build time and the Windows loader/shell at process start; neither is an endpoint of the component. The two servers' network endpoints (game port 8281, DB port 7514, admin port 8895 — `Code/Basedef.h:44-46`) are implemented in `Code/TMSrv/Server.cpp` and `Code/DBSrv/Server.cpp` and are outside this component's boundary.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| `rc.exe` (MSVC 14.44.35207) | Build tool | Preprocess and compile `.rc` into `.res`; resolve `icon1.ico` | Command line; `/l"0x0409" /nologo /fo"TMSRV\RELEASE\TMSRV.RES"` | Binary `.res` (Win32 resource format), script input as UTF-16LE+BOM text | No explicit handling; compile errors fail the build. `/l` default silently overridden by `LANGUAGE` (BR-03) |
| `link.exe` (MSVC 14.44.35207) | Build tool | Merge `.res` into PE `.rsrc`; synthesize default manifest | MSBuild `Link` target | PE32 `.rsrc` section | No explicit handling; link errors fail the build |
| Windows loader / `user32!LoadIconA` | OS API (in-process) | Resolve `"MAINICON"` and obtain `HICON` for `WNDCLASS.hIcon` | In-process DLL call | `HICON` (`NULL` on failure) | **None** — return value unchecked at `Code/TMSrv/Server.cpp:3401` and `Code/DBSrv/Server.cpp:322`; failure is silent (BR-04) |
| Windows shell / Explorer | OS component (out-of-process) | Render the `.exe` file icon from the lowest-ID `RT_GROUP_ICON` | Filesystem read of PE `.rsrc` | `RT_GROUP_ICON` 101 → `RT_ICON` 1/2 | OS-internal; not observable from this component |
| `user32!RegisterClassA` | OS API (in-process) | Register `"MainClass"` carrying the (null) icon and null menu name | In-process DLL call | `WNDCLASS` struct | `if (!RegisterClass(&wc)) return RegisterClass(&wc);` — inverted condition, `TMSrv:Server.cpp:3408-3409` / `DBSrv:Server.cpp:329-330`; the retry can only fail again, so the expression returns 0 on failure and `InitApplication` reports failure, but the intent is inverted (see Section 10) |
| `user32!CreateMenu` / `AppendMenu` / `SetMenu` | OS API (in-process) | Build the runtime menu bar with `IDC_*` command IDs | In-process DLL call | `HMENU` tree | No return-value checks at `TMSrv:Server.cpp:3416,4060-4078` / `DBSrv:Server.cpp:337,1198-1208` (BR-05) |
| MFC/ATL `afxres.h` (latent) | Header, referenced as text only | Would be inserted as the `TEXTINCLUDE 2` include if the `.rc` were regenerated by the resource editor | N/A (not currently included) | N/A | Latent: `TMSrv.rc:34` / `DBSrv.rc:34` store `#include "afxres.h"` while line 10 actually uses `Windows.h`; requires MFC installed to regenerate (Section 10) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Declarative resource bundle (Win32 `.rc`) | Per-project `.rc` + `resource.h` pair, compiled to `.res` and linked into `.rsrc` | `Code/TMSrv/TMSrv.rc`, `Code/DBSrv/DBSrv.rc`, both `resource.h` | Ship static binary assets inside the executable with no runtime files |
| Generated symbol header | IDE-maintained `resource.h` with `_APS_NEXT_*` allocation sentinels and `APSTUDIO_INVOKED` guard | `Code/TMSrv/resource.h:28-31`, `Code/DBSrv/resource.h:32-35` | Keep numeric IDs out of source; allow the editor to renumber safely |
| TEXTINCLUDE round-trip blocks | Three `TEXTINCLUDE` resources (`1` = headers, `2` = includes, `3` = trailing text) guarded by `APSTUDIO_INVOKED` | `TMSrv.rc:21-45`, `DBSrv.rc:21-45` | Let the resource editor regenerate the script without losing custom directives |
| Multi-language block template | `#if !defined(AFX_RESOURCE_DLL) || defined(AFX_TARG_PTB)` + `LANGUAGE ...` block | `TMSrv.rc:18-55`, `DBSrv.rc:18-84` | Leave room for a second language table; only pt-BR is populated |
| Two-tier icon composition | `RT_ICON` member images + one `RT_GROUP_ICON` directory | `icon1.ico` → rc.exe → `TMSrv.exe` `.rsrc` (group 101, images 1/2) | Let the shell pick the best-matching size; currently 16x16 and 32x32 only |
| Imperative UI construction (anti-pattern relative to resources) | `CreateMenu`/`CreatePopupMenu`/`AppendMenu`/`SetMenu` in `WM_CREATE`, `switch` in `WM_COMMAND`, `wc.lpszMenuName = NULL` | `TMSrv:Server.cpp:4055-4110`, `DBSrv:Server.cpp:1193-1219` | Deliberately bypass the resource system for menus; keeps menu state in code, at the cost of localisation and IDE support (BR-05) |
| Wizard-template residue | `LoadIcon(hInstance, "MAINICON")` name-based lookup against a script that declares `IDI_ICON1` | `TMSrv:Server.cpp:3401`, `DBSrv:Server.cpp:322` | Historically the VC++ app-wizard convention; now an unresolved contract (BR-04) |
| Duplicated component instances | Two independent, near-identical `.rc`/`resource.h`/`icon1.ico` sets, no shared fragment | `Code/TMSrv/*` vs `Code/DBSrv/*` | Simplest possible per-project isolation; no shared `.rc2`/`.h` fragment and no shared icon |

Architectural decisions worth recording explicitly:

- The bundle is **per-executable, not shared**. There is no common `.rc2` fragment, no shared icon directory, and no common `resource.h`. The two bundles have already drifted (DBSrv has a dead dialog; both have independent sentinels), which is the expected cost of this choice.
- The bundle is **icon-first and nothing-else**. Menus, string tables, accelerators, bitmaps, cursors and version info were all deliberately (or accidentally) left out. Only the two incidental additions — one dead dialog in DBSrv and the linker's manifest — depart from "icon only".
- Resources are **language-tagged at authoring time and never re-tagged**. The `/l 0x0409` build default is inert (BR-03), so the resource language is a property of the script text, edited only by hand.
- The `.rc` files are **UTF-16LE with BOM** (`FF FE` signature on both). This is acceptable to rc.exe but is a common source of tooling friction for anything that reads them as text (the `read` tool in this analysis environment reports them as binary).

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | `TMSrv.rc` / `DBSrv.rc` vs `Server.cpp:3401` / `Server.cpp:322` | `LoadIcon(hInstance, "MAINICON")` names a resource that does not exist; bundles declare only ordinal 101. Shipped `TMSrv.exe` proves it: resources are `RT_ICON` 1/2, `RT_GROUP_ICON` 101, `RT_MANIFEST` 1; `"MAINICON"` appears once as a string literal only | `wc.hIcon = NULL`; both windows show the system default icon in the title bar, taskbar button, Alt-Tab and `WM_GETICON`. The shell file icon still works (BR-02), which masks the bug |
| High | `Code/TMSrv/Server.cpp:3408-3409`, `Code/DBSrv/Server.cpp:329-330` | `if (!RegisterClass(&wc)) return RegisterClass(&wc);` — inverted condition. On success (`!= 0`) the `!` makes the branch false and execution continues (correct by accident); on failure the branch is taken and re-calls `RegisterClass`, which fails again, so the expression yields 0 | Failure is reported correctly but the retry is meaningless and the intent (retry, or report) is inverted. This is the same function that assigns the broken icon, so the whole class-registration path is uncorrected legacy |
| Medium | `Code/DBSrv/DBSrv.rc:61` vs `Code/DBSrv/resource.h:26` | Identifier casing mismatch: script uses `IDD_MAINDLG`, header defines `IDD_MainDlg`. rc.exe's preprocessor is case-sensitive, so the identifier never expands | Dialog ships as a **string-named** resource `"IDD_MAINDLG"` (proven by UTF-16 string in `DBSrv.res`; no ordinal 103 anywhere). `MAKEINTRESOURCE(103)` would not find it. Any future code that trusts the symbolic ID will fail silently |
| Medium | `Code/DBSrv/DBSrv.rc` / `resource.h` | Orphan dialog: `IDD_MAINDLG` (415 x 280 DLU, caption "DBSRV", `"System"` font, empty body) is compiled into `DBSrv.res` but never instantiated — zero `DialogBox`/`CreateDialog`/`EndDialog` call sites in the repository | 116 bytes of dead payload in `DBSrv.exe` (`1,320` vs `1,204` for `TMSrv.res`) plus a misleading symbol in the resource editor. Suggests the DBSrv main window was once dialog-based |
| Medium | Both bundles | No `VERSIONINFO` / `RT_VERSION` resource at all | File Properties show no company, product, file version or description. Any inventory, patch or reputation tooling keyed on `VS_FIXEDFILEINFO` sees nothing. No code depends on it, so the gap is undetected |
| Low | `Code/DBSrv/resource.h:25` | `IDD_DIALOG1 102` is defined with no corresponding resource in `DBSrv.rc`; no ordinal 102 exists in `DBSrv.res` | Dangling symbol. `_APS_NEXT_RESOURCE_VALUE` (103) now exceeds the highest bound ordinal (101), so the editor would allocate 103 and collide with `IDD_MainDlg` |
| Low | `Code/TMSrv/icon1.ico`, `Code/DBSrv/icon1.ico` | Byte-identical files (SHA-256 `C585D6D5...`); both only 16x16 and 32x32 at 4bpp/16 colours | TMSrv and DBSrv are indistinguishable in shell and taskbar; no 48x48, 256x256 or 32-bit alpha, so the icon is dithered and blurry when scaled on modern high-DPI displays |
| Low | `Code/DBSrv/DBSrv.aps` | 83,420-byte VS resource-editor cache present in the working tree; correctly gitignored (`.gitignore:77`) but bound to a stale absolute path `E:\wyd\test-setup\w2pp-agateownz\Code\DBSrv\DBSrv.rc`. No `.aps` for TMSrv | No build impact. Risk is editor-side: the stale symbol table still advertises `IDD_DIALOG1` and `IDD_MainDlg`, the exact two dead IDs from BR-07. Premise that it is "checked in" is incorrect — `git ls-files` and `git check-ignore` both confirm it is untracked |
| Low | `TMSrv.rc:34`, `DBSrv.rc:34` (TEXTINCLUDE 2) | Stored include directive is `#include "afxres.h"` (MFC/ATL) while the live include at line 10 is `Windows.h`. Also `TMSrv.rc:18` / `DBSrv.rc:18` carry an MFC-specific `AFX_RESOURCE_DLL`/`AFX_TARG_PTB` guard | Regenerating either `.rc` from the VS resource editor would rewrite the include to `afxres.h`, requiring MFC/ATL headers on every build machine. Currently latent because both projects build with plain Win32 headers |
| Low | `Code/DBSrv/DBSrv.rc:79` | `GUIDELINES DESIGNINFO` declares `RIGHTMARGIN, 257` for a dialog declared 415 units wide | Editor-guideline only (inside `#ifdef APSTUDIO_INVOKED`, never compiled); stale layout metadata that would mislead a future editor session |
| Low | Build outputs (untracked) | No `ResourceOutputFileName`, so rc.exe writes to a project-relative `/fo"TMSRV\RELEASE\TMSRV.RES"`, producing nested duplicate intermediate trees `Code/TMSrv/TMSrv/Release/` and `Code/DBSrv/DBSrv/Release/` alongside `Code/TMSrv/Release/` and `Code/DBSrv/Release/` | Two parallel intermediate trees per project (objects, `.iobj`, `.ipdb`, `vc143.pdb`, `.tlog`). Ambiguity about which `.res` was actually linked. Untracked, so no repository impact |
| Low | `TMSrv.vcxproj:190`, `DBSrv.vcxproj:119` | `icon1.ico` is declared as `<None>` rather than `<Image>` | The resource editor will not treat the icon as an editable resource. Inclusion still works because rc.exe resolves it relative to the `.rc` and records it in `rc.read.1.tlog`, so incremental builds are correct — but the dependency is implicit and undeclared in the project system |
| Info | `rc.command.1.tlog` | rc.exe is invoked with `/l"0x0409"` while every authored resource is `LANGUAGE ... PORTUGUESE_BRAZILIAN` (0x0416) | The switch is inert. Mixed-language `.rsrc` (1046 authored entries, 1033 linker manifest). Not a defect, but misleading build configuration |
| Info | `TMSrv.res` / `DBSrv.res` (intermediate) | `.res` file format has no self-describing index; the only practical way to confirm contents is PE inspection of a linked image. `DBSrv.exe` was not present on disk, so DBSrv's runtime resource table could not be observed directly | Verification gap for DBSrv: its dialog's string-name binding is proven from `DBSrv.res` (UTF-16 `IDD_MAINDLG` present, ordinal 103 absent) rather than from a linked PE |

---

## 11. Test Coverage Analysis

**Result: there are no tests of any kind for this component, and no tests anywhere in the repository.** This is an observed absence, not an inference.

Evidence:

| Check | Command / method | Result |
|-------|------------------|--------|
| Files whose name matches `test`, `spec`, `fixture`, `mock` (case-insensitive) | recursive file enumeration over the whole project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`, `.git` | **0 files** |
| Directories named `test`, `tests`, `__tests__`, `spec` | recursive directory enumeration | 0 test directories. The only match is `openspec/specs`, an OpenSpec specification directory, not a test suite (and it is outside this component's boundary) |
| Test projects in the solution | `W2PP Code Project.sln` inspection | 3 project entries only: `DBSrv` (`Code\DBSrv\DBSrv.vcxproj`), `TMSrv` (`Code\TMSrv\TMSrv.vcxproj`), and the `Solution Items` folder. No test or benchmark project |
| Test framework references | repository-wide search for gtest / catch / doctest / Boost.Test / MSTest / CppUnit / GoogleTest project properties | none found; no vcpkg/NuGet manifest exists |
| CI or automated verification | recursive search for `*.yml`, `*.yaml`, `*.ps1`, `*.bat`, `*.cmd`, `*.sh`, `Makefile`, `CMakeLists.txt` | **0 files**. No `.github/` directory. Repository root contains only `Code/`, `docs/`, `openspec/`, `.agents/`, `.codegraph/`, `.opencode/`, `.vs/`, `.git/`, `.gitattributes`, `.gitignore`, `license.txt`, `README.md`, `W2PP Code Project.sln` |
| Tracked file count | `git ls-files \| Measure-Object` | 145 files, none of them tests |
| Resource-specific assertions | search for `MAKEINTRESOURCE`, `FindResource`, `LoadResource`, `EnumResourceNames`, `VerQueryValue`, `GetFileVersionInfo` | **0 call sites**. Nothing in the codebase ever validates that these resources exist |

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `Code/TMSrv/TMSrv.rc` | 0 | 0 | 0% | No tests. The only verification is the rc.exe compile gate (fail-on-error). The one functional contract (BR-04, `"MAINICON"` resolution) is untested and broken |
| `Code/TMSrv/resource.h` | 0 | 0 | 0% | No tests. No assertion that every `#define` binds to a resource |
| `Code/TMSrv/icon1.ico` | 0 | 0 | 0% | No tests. No check on image set (sizes/bit depths), no byte-identity check against DBSrv's copy (they are identical, and nothing detects it) |
| `Code/DBSrv/DBSrv.rc` | 0 | 0 | 0% | No tests. The casing defect in BR-06 and the orphan dialog in BR-07 pass through every gate unremarked |
| `Code/DBSrv/resource.h` | 0 | 0 | 0% | No tests. No check that 102 and 103 are bound |
| `Code/DBSrv/icon1.ico` | 0 | 0 | 0% | No tests |
| `Code/DBSrv/DBSrv.aps` | 0 | 0 | N/A | Not a testable artifact; intentionally ignored by SCM |

Structural notes on the coverage gap:

- The component is, in principle, the easiest part of this codebase to test: rc.exe is a deterministic compiler, a `.res` file is a byte-comparable artifact, and a linked PE's resource tree can be asserted without running the program (as this analysis did with `TMSrv.exe`). A single test project comparing the emitted resource inventory against `{RT_ICON x2, RT_GROUP_ICON 101, RT_MANIFEST}` in both languages would have caught BR-04 and BR-08 immediately; a second assertion for `FindResource(hInst, "MAINICON", RT_GROUP_ICON) != NULL` would have caught BR-04 alone.
- The absence of any resource validation is consistent with the project's overall state (145 tracked files, no CI, no test framework). It is not a gap unique to this component.
- Where verification does happen, it is accidental: rc.exe fails the build on syntax errors, and the PE resource table is validated implicitly by the loader at run time for the shell icon path only. Neither of these catches a wrong resource *name*, a missing version block, a dead ID, or an unreferenced dialog.
- The `TMSrv.exe` artifact examined for this report (`Code/release/TMSrv/run/TMSrv.exe`, 509,440 bytes, modified 2026-11-06 17:25:50) predates the 2026-11-14 project-file upgrade commit (`dfe35d0`) and the 2026-11-14 checkout timestamp on the `.rc` files. Its resource table matches the current source, so the conclusions about BR-04 and BR-08 hold for the current source as well as for the artifact, but no `DBSrv.exe` existed on disk to cross-check DBSrv's runtime table.

---

## Appendix A: Verified resource inventory of the shipped binary

`Code/release/TMSrv/run/TMSrv.exe` — PE32, machine 0x14C (i386), 509,440 bytes, SHA-256 `f8c744bd65648c71190abdb18b5c7960f7917b760616179c62917222ccc66199`. Resource directory read statically; 11 directory entries examined, 4 leaf resources.

| Type | Name | Language | Size | Interpretation |
|------|------|----------|------|----------------|
| 3 (`RT_ICON`) | 1 | 1046 (0x0416, pt-BR) | 296 B | 16x16, 16 colours, 4bpp icon image |
| 3 (`RT_ICON`) | 2 | 1046 | 744 B | 32x32, 16 colours, 4bpp icon image |
| 14 (`RT_GROUP_ICON`) | 101 | 1046 | 34 B | Group icon `IDI_ICON1`; 2 entries, both plane=1, 4bpp, declared sizes match the images exactly |
| 24 (`RT_MANIFEST`) | 1 | 1033 (0x0409, en-US) | 381 B | Linker default; `requestedExecutionLevel level='asInvoker' uiAccess='false'`; no DPI awareness, no common-controls dependency |

Absent from the image (verified by type enumeration, not merely by absence in the `.rc`): `RT_VERSION` (16), `RT_DIALOG` (5), `RT_MENU` (4), `RT_STRING` (6), `RT_ACCELERATOR` (9), `RT_BITMAP` (2), `RT_CURSOR` (1), and any named `RT_GROUP_ICON` (i.e. `"MAINICON"`). The identifier string `MAINICON` occurs exactly once in the image, consistent with the `LoadIconA` argument at `Code/TMSrv/Server.cpp:3401` and with no corresponding `.rsrc` name entry.

## Appendix B: Verified resource inventory of the DBSrv intermediate artifact

`Code/DBSrv/Release/DBSrv.res` — 1,320 bytes (identical size in `Code/DBSrv/DBSrv/Release/DBSrv.res`). Contains the same two `RT_ICON` images (296 B and 744 B, matching the byte-identical `icon1.ico`), the group icon, and one additional `RT_DIALOG` whose name is stored as the UTF-16 **string** `IDD_MAINDLG`, immediately preceded by the `0xFFFF, 0x0005` ordinal-type marker for `RT_DIALOG`, and whose template contains the dimensions 415 and 280, the caption `DBSRV`, and the font face `System`. No `0xFFFF, 0x0067` (ordinal 103) and no `0xFFFF, 0x0066` (ordinal 102) occurs anywhere in the file, confirming that both dialog IDs declared in `Code/DBSrv/resource.h` are unbound. `Code/TMSrv/Release/TMSrv.res` — 1,204 bytes — contains no UTF-16 strings at all, consistent with an icon-only bundle.
