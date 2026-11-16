# Component Deep Analysis Report

**Component:** Language Message Catalog
**Primary location:** `Code/TMSrv/Language.h` (18,074 bytes, 550 lines)
**Binding surfaces:** `Code/Basedef.h`, `Code/Basedef.cpp`, `Code/TMSrv/SendFunc.cpp`, `Code/TMSrv/Server.cpp`, `Code/TMSrv/imple.cpp`
**Project:** W2PP (TMSrv game/world server, Win32/MSVC, C++ in C style)
**Analysis date:** 2026-11-15 22:52:52
**Scope:** entire project root `E:\wyd\test-setup\w2pp`
**Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`

---

## 1. Executive Summary

The Language Message Catalog is not an executable module. It is a **compile-time constant namespace** (530 `#define` directives, `Code/TMSrv/Language.h:21-550`) that names 530 integer slots in a **single process-wide string array**:

```c
char g_pMessageStringTable[MAX_STRING][128];   // Basedef.cpp:742, MAX_STRING = 2000 (Basedef.h:150)
```

Every catalog-routed user-visible text emitted by TMSrv is addressed by one of these indices (145 of 671 `SendClientMessage` sites still use hardcoded literals). The array is populated at startup from an external single-byte text resource, `Language.txt`, by `BASE_InitializeMessage()` (`Basedef.cpp:4637-4710`), and read directly by 70 translation units (46 of which reference at least one index). The header itself has **zero includes and zero code** — it is a pure leaf dependency (`#pragma once` at `Language.h:19`, followed only by `#define` lines).

**Measured facts**

| Metric | Value |
|---|---|
| `#define` entries | 530 (lines 21-550, no blank lines between entries) |
| Index range emitted | 1-537 |
| Absent indices inside the emitted range | 7 (`288`, `507`, `508`, `509`, `510`, `511`, `512`) |
| Duplicate index values | 0 |
| Duplicate macro names (case-sensitive) | 0 |
| Symbolic table accesses in code | 653 |
| Raw numeric table accesses (`g_pMessageStringTable[114]` etc.) | 7 |
| Computed-index accesses (`Type + _NN_Only_Level1_Armor`, `messageId + 17`) | 3 |
| Macro names never referenced anywhere | 225 (42.5% of the namespace) |
| Index values with no reachable code path | 215 defined + 7 holes = 222 of 537 (41.3%) |
| Files that include `Language.h` directly | 12 |
| Files that depend on it transitively via `ProcessClientMessage.h:32` | 66 (union: 70 files) |
| Symbolic catalog accesses in code | 653 (631 of them pass the entry as a message or format argument) |
| Hardcoded message literals bypassing the catalog | 145 of 671 `SendClientMessage` sites (21.6%) |
| Parameter-contract deviations detected | 29 of 631 message-argument references (4.6%) |
| Test files for this component | 0 (no test project, framework, fixture or CI pipeline exists in the repository) |

**Key findings**

1. **The index space is frozen and append-only.** Git history shows `Language.h` has only ever changed twice after project import (`489c4e0 Add project files.`, `b030892 License update`, `79b14f7 Update Language.h` — the 2018 commit touched only the license header and the trailing newline). No renumbering has ever occurred, which is consistent with the index being a **wire-level contract shared with the client binary** (client source is absent; `Code/ClientPatch_v7662/` contains only `ClientPatch_v7662.vcxproj.user`).
2. **Two independent contracts are conflated in one identifier**: the integer value (client/display contract) and the two-letter type prefix (parameter contract). The prefix is enforced only by developer discipline and is demonstrably violated in 29 call sites, including 2 sites where a format string receives *fewer* arguments than its prefix declares (`sprintf` under-supply, undefined behaviour).
3. **The loader is a modal, unvalidated, non-atomic text parser.** It uses `MessageBox` for input errors (`Basedef.cpp:4680`, `Basedef.cpp:4701`), never logs a missing resource file (`Basedef.cpp:4643-4644`), performs no duplicate-index or coverage validation, and initialises only 128 of its 256,000 bytes because of an argument-order defect at `Basedef.cpp:4639` and `Basedef.cpp:4934`.
4. **Storage outlives its consumer boundary.** `g_pMessageStringTable` is defined in the shared `Basedef.cpp`, which is compiled into **both** `TMSrv` (`TMSrv.vcxproj:91`) and `DBSrv` (`DBSrv.vcxproj:94`); DBSrv never loads and never reads it, yet carries the 256 KB zeroed array.
5. **Localized text leaks into control flow.** Six whisper-command comparisons use the *translated* string as the command keyword (`_MSG_MessageWhisper.cpp:582, 597, 611, 648, 796, 1291`), so a translation edit changes the server's command surface.

---

## 2. Data Flow Analysis

### 2.1 Startup binding (the only production write path)

```
1. Server startup            Server.cpp:7158  BASE_InitializeBaseDef()
2. Catalog binding begins    Server.cpp:7160  BASE_InitializeMessage("Language.txt")
3. Storage reset             Basedef.cpp:4639 memset(g_pMessageStringTable, MAX_STRING, 128)
                                                (fill byte 0xD0 over 128 bytes only - defect)
4. File open                 Basedef.cpp:4641 fopen("Language.txt", "rt")
                             Basedef.cpp:4643 missing file -> silent return (table stays zeroed)
5. Line read                 Basedef.cpp:4653 fgets(str, 1024, fp)
6. Index parse               Basedef.cpp:4658 sscanf(str, "%d %s", &Index, part1)
7. Index gate                Basedef.cpp:4663 reject Index <= 0 or Index > 2000
8. Delimiter search          Basedef.cpp:4668 scan first 256 bytes for '\t'
                             Basedef.cpp:4678 no tab -> MessageBox "Can't parse String", skip line
9. Text slice                Basedef.cpp:4687 truncate payload at first '\t', '\n' or '\0'
10. Length gate              Basedef.cpp:4699 reject len <= 0 or len >= 128
                             Basedef.cpp:4701 MessageBox "Empty or Long String", skip line
11. Row copy                 Basedef.cpp:4706 strcpy(g_pMessageStringTable[Index], cont)
12. Table live               Basedef.cpp:742  g_pMessageStringTable[2000][128]
```

### 2.2 Read paths at runtime

```
A. Pre-formatted send (majority path, 481 sites)
   handler -> g_pMessageStringTable[IDX] -> SendClientMessage(conn, ptr)
            -> SendFunc.cpp:27  memcpy(String, ptr, 96) + String[94]=0 + String[95]=0
            -> MSG_MessagePanel{ String[128] }  (Basedef.h:1195-1199)
            -> pUser[conn].cSock.AddMessage(...)  (SendFunc.cpp:44)

B. Formatted send (150 sprintf sites)
   handler -> sprintf(temp[4096] (Server.h:382), g_pMessageStringTable[IDX], args...)
            -> SendClientMessage(conn, temp)  -> path A

C. Proximity chat (73 sites)
   handler -> sprintf(temp, ...) -> SendSay(npcId, temp)
            -> SendFunc.cpp:1656 memcpy(String, temp, 96)  [no forced terminator]
            -> MSG_MessageChat{ String[96] } (Basedef.h:1440-1444)
            -> GridMulticast(TargetX, TargetY, ...)  (SendFunc.cpp:1659)

D. Broadcast (12 SendNotice + 10 SendNoticeArea sites)
   handler -> SendNotice(ptr)  (SendFunc.cpp:47)     -> loop over all PLAY users
   handler -> SendNoticeArea(ptr, x1, y1, x2, y2) (SendFunc.cpp:155) -> region fan-out

E. Keyword comparison (6 sites, control-flow read)
   _MSG_MessageWhisper.cpp:582,597,611,648,796,1291
   strcmp(m->MobName, g_pMessageStringTable[_NN_Kingdom])  etc.

F. Bounds-checked accessor (4 sites)
   BASE_GetLanguage(temp, IDX[, args])  (Basedef.cpp:1258-1312)
   -> [UNHANDLED] literal when IDX out of 0..1999

G. Loader write / binary cache (dormant)
   BASE_WriteMessageBin() Basedef.cpp:4903 -> UI/strdef.bin (XOR 0x5A + checksum)
   BASE_ReadMessageBin()  Basedef.cpp:4932 <- UI/strdef.bin (checksum compare commented out)
   Call sites for G: none in this repository.
```

### 2.3 Runtime rebinding (GM command)

```
1. Client sends whisper message with admin keyword
2. _MSG_MessageWhisper.cpp dispatcher
3. ProcessImple(conn, level, m->String)   (imple.cpp)
4. imple.cpp:250  strcmp(sval1,"reloadfile")
5. imple.cpp:252  BASE_InitializeBaseDef()
6. imple.cpp:253  BASE_InitializeMessage("Language.txt")   <- catalog re-read
7. imple.cpp:255-259 ancillary re-reads (sanc/quest/combine/castle)
8. imple.cpp:261  SendClientMessage(conn, "SET RELOAD FILE")
```

Because step 6 re-enters the same loader, the 128-byte `memset` defect and the "missing file returns silently" policy also apply to the reload path: indices removed from `Language.txt` keep their previous text for the life of the process.

### 2.4 Error propagation

The component has no error channel. `BASE_InitializeMessage` returns `void`; the only failure signals are (a) a modal `MessageBox`, and (b) an empty string at the addressed row. Downstream, `SendClientMessage` silently drops messages for out-of-range connections (`SendFunc.cpp:29-30`), so an unbound index produces an empty panel message rather than an error.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Data contract | All display text is addressed by a single flat integer namespace `1..MAX_STRING` (2000), physically allocated `1..537` | `Language.h:21-550`, `Basedef.h:150`, `Basedef.cpp:742` |
| BR-02 | Data contract | Indices are immutable and append-only; existing values must never be renumbered or reused | `Language.h` (git history: 0 renumbering commits), `Basedef.cpp:4663` |
| BR-03 | Validation | A two-letter type prefix declares the message's substitute count and types (`NN`,`SN`,`DN`,`SS`,`DD`,`DS`,`SD`,`SSD`,`SSNN`,`SSDD`,`NS`,`I64D`) | `Language.h` naming convention, consumed at 631 sites |
| BR-04 | Business logic | The catalog is bound at startup from `Language.txt`; nothing else in TMSrv writes rows | `Server.cpp:7160`, `Basedef.cpp:4637-4710` |
| BR-05 | Business logic | The catalog may be re-bound at runtime through the GM `reloadfile` command | `imple.cpp:250-262` |
| BR-06 | Validation | Resource grammar is `<index><TAB><text>`; index parsed as decimal, must satisfy `0 < index <= 2000` | `Basedef.cpp:4658`, `Basedef.cpp:4663` |
| BR-07 | Validation | Message text must be 1..127 bytes and is terminated by the first tab, newline or NUL | `Basedef.cpp:4687-4704` |
| BR-08 | Error policy | Malformed lines trigger a modal `MessageBox` and are skipped; a missing resource file aborts silently | `Basedef.cpp:4643-4644`, `4678-4683`, `4699-4704` |
| BR-09 | Data contract | Duplicate indices are not detected; last definition wins (implicit overwrite) | `Basedef.cpp:4706` |
| BR-10 | Business logic | Every defined index is addressable whether or not `Language.txt` provides it; absence yields an empty message | `Basedef.cpp:4639-4644`, `742` |
| BR-11 | Business logic | Outbound text is truncated to 94 visible characters by the presentation layer | `SendFunc.cpp:39-42`, `Basedef.h:129` |
| BR-12 | Business logic | Substitution is performed in C code (`sprintf`), never in the resource file | 150 `sprintf` sites, e.g. `_MSG_Challange.cpp:56` |
| BR-13 | Business logic | Six admin whisper commands are matched against the localized text itself | `_MSG_MessageWhisper.cpp:582,597,611,648,796,1291` |
| BR-14 | Business logic | Two contiguous index blocks are addressed arithmetically and therefore must stay contiguous | `ProcessSecMinTimer.cpp:108` (17..22), `_MSG_UseItem.cpp:1118,1187` (224..227) |
| BR-15 | Validation | Index-typed access must pass through `BASE_GetLanguage`, which bounds-checks and falls back to `[UNHANDLED]` | `Basedef.cpp:1258-1312` |
| BR-16 | Data contract | Text is byte-transparent: no transcoding, no code-page conversion, MSVC `MultiByte` build | `TMSrv.vcxproj:27,34`, `Basedef.cpp:4706` |
| BR-17 | Business logic | A parallel binary representation (`UI/strdef.bin`, XOR 0x5A + checksum) is defined but never used | `Basedef.cpp:4903-4961` |
| BR-18 | Data contract | Unreferenced definitions are legal; the build and loader never require full coverage | `Language.h` (225 orphans), `Basedef.cpp:4637-4710` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 Single flat index namespace

**Overview**:
Every text shown to a player is selected by an integer into one process-wide array. The header `Code/TMSrv/Language.h` provides 530 symbolic names for those integers; the values run 1..537 with seven holes and point into `char g_pMessageStringTable[2000][128]`.

**Detailed description**:
The namespace is declared by two independent artefacts that must agree: the `#define` list in `Language.h:21-550` and the array extent in `Basedef.h:150` (`#define MAX_STRING 2000`) / `Basedef.cpp:742`. The header publishes 530 values, of which 385 carry the `_NN_` prefix, 54 `_DN_`, 39 `_SN_`, 18 `_DD_`, 15 `_SS_`, 4 `_DS_`, 3 `_SD_`, 3 `_SSD_`, 3 `_SSDD_`, 2 `_ND_`, and one each of `_I64D_`, `_NS_`, `_SSNN_`, `_UN_`. Because macros are preprocessor text, the namespace has no type safety, no scoping and no compiler-enforced range: `g_pMessageStringTable[_NN_Not_Connected]` is indistinguishable from `g_pMessageStringTable[91]` at the point of use.

Allocation behaviour is append-only in practice. Values 1..537 form a dense block except for `288` (between `_SS_Ally_Declared 287` at `Language.h:307` and `_NN_Cant_Declare_Ally 289` at `Language.h:308`) and `507..512` (between `_SN_KINGDOMWAR_DROP_ 506` at `Language.h:525` and `_DN_NO_TOWNSPEOPLE 513` at `Language.h:526`). Both holes are consistent with values that existed in an upstream build and were deleted or never merged, not with deliberate reservations; the loader accepts any index in `1..2000`, so a resource file that defines `288` or `507..512` will bind successfully to text that no server code can ever request. Capacity headroom is `2000 - 537 = 1463` slots, i.e. 73% of the declared namespace is unused.

The namespace is *flat*, not partitioned by domain (quests, combat, economy, administration all interleave), which is why the header reads as an append log: `_NN_Qest06Complete` through `_NN_Qest16Complete` occupy 6..16, the reboot warnings sit at 17..22, guild war results at 135..140, newbie quests at 230..245, auctions at 486..491, and the PIN/security strings at the very end (529..537). Nothing in the representation records which subsystem owns a slot, so an index cannot be validated at its use site.

**Rule workflow**:
```
1. Author picks the next free integer (current maximum 537, next candidate 538)
2. Author adds `#define _XX_Name <value>` at the end of Language.h
3. Translator adds a matching `<value><TAB><text>` line to Language.txt
4. Loader binds the row at startup (Basedef.cpp:4706)
5. Consumer references g_pMessageStringTable[_XX_Name]
6. Client receives the bound text with no knowledge of the index
```

---

### Business Rule: BR-02 Index immutability and append-only allocation

**Overview**:
Once a value is published it is never renumbered, reordered or reused for a different meaning. This is the invariant that makes the catalog safe in a system where the display text lives on the client.

**Detailed description**:
The repository evidence is a single import commit plus a two-commit history for this file (`git log -- Code/TMSrv/Language.h`: `79b14f7 Update Language.h`, `b030892 License update`, `489c4e0 Add project files.`). The 2018 commit `79b14f7` shows `3 insertions(+), 3 deletions(-)` and touches only the copyright banner and the missing trailing newline. No index has ever moved. This matters because TMSrv is a server emulator for a commercial client: the client binary renders parts of its interface from its own compiled string table, and the server refers to the same slot numbers in `MSG_MessagePanel` payloads and in control data. The client source is not in the repository (`Code/ClientPatch_v7662/` holds only a `.vcxproj.user`), so the parity cannot be verified here — it is an implicit contract with a confidence level of **high** for existence and **unverifiable** for content.

The rule is enforced only by convention, never by the code. The loader would happily bind a different text to index 91, and every `#define` could be renumbered without any compiler diagnostic other than "undeclared identifier" at the 653 use sites. Consequently the header behaves as a **frozen ABI**: the value column is append-only, while the name column and the text column are mutable.

Observed reuse pressure is visible in the naming: `_NN_Not_Connected 91` is complemented by `_NN_Not_Connected2 118`, `_SN_Summoned_By_S 52` by `_SN_Summoned_By_S2 98`, `_NN_Dropped_Current_Party_First 121` by `_NN_Dropped_Current_Party_First2 117`. Rather than change index 91, the author added a second slot, which is exactly the behaviour this rule predicts. Two entries also differ only by letter case (`_NN_Only_Guild_Master_can 24` at `Language.h:44` and `_NN_Only_Guild_Master_Can 144` at `Language.h:164`), a case-only collision that is legal in C but hostile to readers.

**Rule workflow**:
```
1. Change request for an existing message -> edit Language.txt text only
2. Change request for a new message     -> append new index 538+, never insert
3. Deletion of a message                -> definition retained (or removed) but value never reused
4. Verification                         -> none automated; relies on git diff review
```

---

### Business Rule: BR-03 Type-prefix parameter contract

**Overview**:
The two letters following the leading underscore declare how many substitute arguments the message takes and of which type, so that a reviewer can validate a call site without opening `Language.txt`.

**Detailed description**:
The observed convention is a sequence of type letters after the first underscore, read left to right as the argument list: `S` = `char *`, `D`/`N` = integral, with `I64D` denoting a 64-bit integral and duplicated letters (or a longer tail) meaning more arguments. The mapping measured across all 631 message-argument references is: `_NN_` 473 references/0 args, `_DN_` 62/1, `_SN_` 42/1, `_SS_` 15/2, `_DD_` 9/2, `_NS_` 6/1, `_SSD_` 3/3, `_DS_` 2/2, `_SD_` 2/2, `_I64D_` 2/1, `_SSNN_` 2/4. Six definitions with undocumented tails (`_ND_GenDarkShadow 444`, `_ND_SERVERCLAN 460`, `_UN_JACKPOT_MESSAGE 386`, `_SSDD_HeavenWarResult 393`, `_SSDD_HeavenWarResult2 396`, `_SSDD_HeavenWarResult3 397`) have no call site, so their argument order is undocumented.

The convention is **not** reliable for ordering. `_SD_Zakum_Quest_by_S_N 222` is invoked as `sprintf(temp, table[_SD_Zakum_Quest_by_S_N], UserNameArea, count - 1)` (`_MSG_Quest.cpp:244`) and as `sprintf(temp, table[_SD_Zakum_Quest_by_S_N], CastleLeader, count)` (`CCastleZakum.cpp:123`) — string first, number second, i.e. the reverse of the `S`→`D` reading. Likewise `_DS_S_cure_price_D 258` is invoked as `sprintf(temp, table[_DS_S_cure_price_D], g_pItemList[mountIndex].Name, price)` (`_MSG_Quest.cpp:187`), again string first. The only defensible reading is "the letters enumerate the parameter *types present*, not their positions", and the human-readable tail (`_by_S_N`, `_S_cure_price_D`) is the actual ordering hint.

Twenty-nine call sites deviate from the declared arity. Three categories were measured: 13 sites pass a parameterized entry straight to a send helper with no formatting at all (for example `SendClientMessage(conn, g_pMessageStringTable[_SN_I_Wwill_Enchant_Your_Weapon])` at `_MSG_Quest.cpp:1603` and `SendNotice(g_pMessageStringTable[_DD_KINGDOMWAR_START])` at `Server.cpp:6624`); 8 sites over-supply arguments that `sprintf` silently discards (`_NN_Version_Not_Match_Rerun` with `APP_VERSION` at `_MSG_AccountLogin.cpp:47`; `_NN_Check_User_Info` with two arguments at `_MSG_MessageWhisper.cpp:1335` and `:1346`; `_NN_Battle_Started` with two at `Server.cpp:8070`); and 2 sites under-supply, which is undefined behaviour because the missing variadic arguments are read from the stack (`_SSD_S_get_S` with two of three arguments at `_MSG_GetItem.cpp:131` and `MobKilled.cpp:1779`). The over-supply cases are the strongest evidence that the *actual text* sometimes contains placeholders even though the tag says `_NN_`, which means the prefix cannot be used as a validation oracle in either direction.

**Rule workflow**:
```
1. Author decides the message needs k substitutes and of which type
2. Author encodes them in the macro tail: _NN_/ _SN_/ _DN_/ _SS_/ _DD_/ _SSD_/ ...
3. Call site must use sprintf (format position), not a send helper
4. Call site must pass exactly k arguments of the declared types
5. Violation is invisible at compile time (variadic + preprocessor) and silent at runtime for k too large
```

---

### Business Rule: BR-04 Startup binding from Language.txt

**Overview**:
The array is populated exactly once during boot, before any handler can run, from a single relative file name `Language.txt`.

**Detailed description**:
`Server.cpp:7160` calls `BASE_InitializeMessage("Language.txt")` immediately after `BASE_InitializeBaseDef()` and `BASE_ApplyAttribute(...)` (`Server.cpp:7158-7160`) and before `mSummon.Initialize()` and the initial item loop. The path is relative to the process working directory (the architecture report notes the executable must see `Language.txt` alongside `gameconfig.txt`, `LevelItem.txt`, `MobName.txt` and the rest). No search path, environment variable, locale suffix or fallback directory is consulted; there is no `_EN`/`_PT` variant mechanism, so a deployment switches language by replacing the single file.

The loader is the only writer in the production binary. A full scan of `Code/**` finds exactly one assignment into a row (`strcpy(g_pMessageStringTable[Index], cont)` at `Basedef.cpp:4706`), one `memset` reset (`Basedef.cpp:4639`), and one defined-but-uncalled pair of binary cache functions. No handler mutates text at runtime, no placeholder substitution is stored back, and no client-supplied text is written into the table — which is what keeps format-string injection limited to the resource file itself.

Timing is a hard prerequisite: the table is a zero-initialised global (`Basedef.cpp:742`) before binding, so any consumer invoked earlier would emit empty strings rather than crash. Because the initialisation happens inside the main start-up sequence after socket/`CPSock` construction but before the message pump, the practical ordering is safe; the risk is confined to future code that runs during static initialisation or between socket creation and line 7160.

**Rule workflow**:
```
1. Process start -> globals zero-initialised (g_pMessageStringTable all 0x00)
2. BASE_InitializeBaseDef()                      Server.cpp:7158
3. BASE_InitializeMessage("Language.txt")        Server.cpp:7160
4. fopen rt -> parse -> strcpy per line          Basedef.cpp:4641-4706
5. fclose                                        Basedef.cpp:4709
6. Handlers become reachable; every read is now bound
```

---

### Business Rule: BR-05 Runtime rebinding through reloadfile

**Overview**:
A privileged whisper command re-executes the loader against `Language.txt` on a live server, allowing text changes without a restart.

**Detailed description**:
The path begins in the admin whisper dispatcher, which forwards the message body to `ProcessImple(conn, level, m->String)` (from `_MSG_MessageWhisper.cpp`), and continues at `imple.cpp:250-262` where the token `reloadfile` triggers `BASE_InitializeBaseDef()` and then `BASE_InitializeMessage("Language.txt")` at `imple.cpp:253`, followed by `CReadFiles::ReadSancRate()`, `ReadQuestsRate()`, `ReadCompRate()` and `CCastleZakum::ReadCastleQuest()`, and finally a confirmation message `"SET RELOAD FILE"` (`imple.cpp:261`).

Semantically this is a **partial** rebind. The reset at `Basedef.cpp:4639` is `memset(g_pMessageStringTable, MAX_STRING, 128)` — the fill byte is `MAX_STRING` (2000 → `0xD0`) and the length is 128, so only row 0 is overwritten (with non-ASCII bytes) and rows 1..1999 retain their previous contents. Any index that the new `Language.txt` no longer defines therefore keeps the text from the previous bind for the rest of the process lifetime, and a *shorter* text that overwrites a longer one in the same row leaves no trace because `strcpy` writes a terminator; the asymmetry is invisible to operators.

The rule also lacks any atomicity or concurrency control. The loader runs inside the same thread that processes client messages (the `ProcessImple` call stack), so a rebind cannot interleave with handler reads in this single-threaded design; however, if the file is malformed mid-way, the table is left in a *mixed* state (some rows new, some old) because errors are per-line skips rather than aborts.

**Rule workflow**:
```
1. Client whispers "reloadfile"
2. _MSG_MessageWhisper dispatcher -> ProcessImple(conn, level, body)
3. imple.cpp:250 match -> BASE_InitializeBaseDef()
4. imple.cpp:253 BASE_InitializeMessage("Language.txt")
5. per-line: valid lines overwrite, invalid lines skipped with MessageBox
6. imple.cpp:255-259 ancillary table reloads
7. imple.cpp:261 reply "SET RELOAD FILE"
8. Indices absent from the new file keep stale text (row 0 clobbered with 0xD0)
```

---

### Business Rule: BR-06 Resource grammar and index gate

**Overview**:
Each non-empty line of `Language.txt` must begin with a decimal index followed by a TAB and then the text; the index must satisfy `0 < index <= 2000`.

**Detailed description**:
Parsing is deliberately minimal. `sscanf(str, "%d %s", &Index, part1)` at `Basedef.cpp:4658` extracts the leading integer and a second whitespace-delimited token that is **never used afterwards** (`part1` is a dead variable in this function). `Index` is initialised to `-1` at `Basedef.cpp:4652` for every line, so any line whose first token is not numeric stays `-1` and is discarded by the gate `if(Index <= 0 || Index > MAX_STRING) continue;` (`Basedef.cpp:4663`). The gate is inclusive of `MAX_STRING` (2000) while the array is indexed `0..1999`, so a resource line numbered exactly `2000` passes validation at 4663 and then writes `g_pMessageStringTable[2000]` at `4706` — one row past the end of the array. Reachability of that off-by-one requires a hand-edited resource line, since the header never emits 2000.

The delimiter is a literal tab found by a loop that inspects only the first 256 bytes (`Basedef.cpp:4668-4676`); `TabPos == 0` is treated as "no tab" (`4678`). Because `TabPos` is also used to slice the text at `4685`, a tab located at byte 0 would be indistinguishable from "not found" — harmless in practice because byte 0 holds the index digits. Lines whose payload is empty or whose total length exceeds 127 bytes are rejected (`4699-4704`), which makes it impossible to bind an intentionally blank message: `len <= 0` is an error, not a value.

Neither the grammar nor the gate is documented in the repository: `Language.txt` is absent from the working tree and from git, so this rule is reconstructed entirely from `Basedef.cpp:4650-4707`. Confidence: **high** for the grammar, **medium** for the assumption that real resource files are strictly tab-delimited.

**Rule workflow**:
```
1. fgets 1024-byte line (Basedef.cpp:4653)
2. Index := -1                                    (4652)
3. sscanf("%d %s") -> Index, part1 (part1 unused) (4658)
4. If Index <= 0 or Index > 2000 -> skip line     (4663)
5. Find '\t' within bytes 0..255                   (4668)
6. No tab -> MessageBox "Can't parse String" + skip (4678-4683)
7. Text := str + TabPos + 1, cut at '\t','\n',0    (4685-4695)
8. len := strlen(text); len <= 0 or len >= 128 -> MessageBox "Empty or Long String" + skip (4697-4704)
9. strcpy row                                      (4706)
```

---

### Business Rule: BR-07 Text length gate and byte truncation

**Overview**:
Bound text is limited to 1..127 bytes, measured with `strlen` after tab/newline termination.

**Detailed description**:
The length check at `Basedef.cpp:4697-4704` computes `strlen(cont)` after the payload has already been terminated at the first tab, newline or NUL (`4687-4695`), and rejects `len <= 0` as well as `len >= 128`. The upper bound matches the row width exactly (`char ...[2000][128]`), which is what makes the subsequent `strcpy` at `4706` memory-safe. A 127-byte payload plus terminator fills the row precisely; 128 bytes or more is refused.

The gate is applied *after* the 1024-byte `fgets` buffer has been clamped at bytes 510 and 511 (`Basedef.cpp:4660-4661`), which only guards the tail of the largest possible line and has no effect on the 127-byte limit. Because the measurement is byte-based and the build is `MultiByte` (`TMSrv.vcxproj:27,34`), the effective character count for a multi-byte code page (the code page of the deployed resource is not identifiable from the repository, while `TMSrv.rc:19` declares `LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN` and the source carries Portuguese identifiers) is lower: a CP949 lead byte plus trail byte consumes two of the 127 bytes for one glyph.

The bound is tighter in practice than the declared maximum because the presentation layer truncates at 94 bytes (BR-11). The loader's 127 is therefore an *unreachable* ceiling for `SendClientMessage` traffic, and the difference (33 bytes, up to 16 double-byte glyphs) is silently lost rather than reported. `SendSay` does deliver the full row, but without a forced terminator (BR-11), so the two carriers disagree about what "the message" is.

**Rule workflow**:
```
1. payload sliced at first tab/newline/NUL            (4687-4695)
2. len := strlen(payload)                             (4697)
3. len <= 0            -> MessageBox + skip (empty)   (4699-4703)
4. len >= 128          -> MessageBox + skip (long)    (4699-4703)
5. 1 <= len <= 127     -> strcpy into 128-byte row    (4706)
6. later: SendClientMessage keeps 94 bytes; SendSay keeps 96 bytes
```

---

### Business Rule: BR-08 Error policy: modal dialog and silent abort

**Overview**:
Resource problems are surfaced to the machine's desktop, not to logs; a missing file produces no diagnostic at all.

**Detailed description**:
Three failure modes exist and all of them are human-visible only on the console session: a line without a tab raises `MessageBox(NULL, str, "Can't parse String", MB_OK)` (`Basedef.cpp:4680`), a payload of illegal length raises `MessageBox(NULL, str, "Empty or Long String", MB_OK)` (`Basedef.cpp:4701`), and the analogous checks in the sibling loader raise `"Can't parse String"`, `"Empty or Long String-1st"`, `"Empty or Long String-2nd"` and `"Too much items in mobname.txt"` (`Basedef.cpp:4756, 4787, 4858, 4867, 4875, 4892`). `MessageBox` is a blocking modal call that requires a user click before the loader continues.

For a server process this is a material behaviour: a malformed `Language.txt` at boot stops the boot sequence until an operator interacts with a window that may not exist on a service session. `MB_OK` means the dialog cannot be cancelled and does not abort the load — the offending line is simply skipped. A missing file is worse from an observability standpoint: `if(fp == NULL) return;` (`Basedef.cpp:4643-4644`) leaves the table zeroed and returns to `Server.cpp:7160` with no return code, no `Log()` call and no user-visible text, so the server starts successfully and emits empty messages for every request.

The rest of the codebase uses a `Log()` helper for diagnostics (`SendFunc.cpp:52`, `Server.cpp:7164`, and many handlers); this component does not participate in that convention. The impact is that every catalog failure is either interactive or invisible, never recorded.

**Rule workflow**:
```
1. fopen fails                     -> return (no log, table stays zeroed)          (4643)
2. line has no tab                 -> MessageBox + skip line                       (4680)
3. payload empty or >=128 bytes    -> MessageBox + skip line                       (4701)
4. index out of 1..2000            -> skip line silently                           (4663)
5. any of these during reload      -> process continues with mixed old/new content
```

---

### Business Rule: BR-09 Duplicate indices: last definition wins

**Overview**:
The loader performs no collision detection; two lines with the same index bind the later one and the earlier one is discarded without notice.

**Detailed description**:
The only write is `strcpy(g_pMessageStringTable[Index], cont)` inside the line loop (`Basedef.cpp:4706`) with no record of which indices have already been seen. Duplicate indices in the resource file therefore resolve silently in file order. The `Language.h` header cannot exhibit duplicates today (0 duplicate values across 530 definitions, verified exhaustively), but nothing prevents a *value* collision from being introduced: two different macro names may legally carry the same integer, and the compiler would not object because the macros are never used as identifiers in an arithmetic context that would reveal aliasing.

The practical collision surface is the resource file, which is absent from the repository and therefore not verifiable here. Because the header's value column is hand-maintained and dense (1..537 with 7 holes), an editor appending a new definition by copy-paste is the realistic source of a duplicate value — the failure mode being two subsystems silently sharing one message, which manifests only as wrong text in the UI. Confidence that no duplicate exists *in the header*: **high** (exhaustive scan). Confidence that none exists in any deployed `Language.txt`: **unknown** (file not present).

Two near-collisions are already visible at the name level and are worth recording because they defeat code review: `_NN_Only_Guild_Master_can 24` versus `_NN_Only_Guild_Master_Can 144` (case only), and semantic pairs `_NN_Not_Connected 91` / `_NN_Not_Connected2 118`, `_SN_Summoned_By_S 52` / `_SN_Summoned_By_S2 98`, `_NN_Dropped_Current_Party_First 121` / `_NN_Dropped_Current_Party_First2 117`, `_NN_OnlyVillage 172` / `_NN_Only_Village 217`, `_NN_Item_Arrived 272` / `_SN_Item_Arrived 478`, `_NN_Level_limit 298` / `_NN_Level_Limit2 340` / `_DN_Level_Limit 373`, `_NN_Wrong_Trade_Packet1 80` / `_NN_Wrong_Trade_Packet2 88`.

**Rule workflow**:
```
1. line 1: "91 <TAB>Text A"  -> row 91 = "Text A"
2. line 2: "91 <TAB>Text B"  -> row 91 = "Text B" (no warning, no log)
3. result: all callers of _NN_Not_Connected render "Text B"
```

---

### Business Rule: BR-10 Absent resource equals empty message

**Overview**:
An index is addressable the moment the header compiles, whether or not the resource file provides text for it; the unbound state is an empty C string, not an error.

**Detailed description**:
`g_pMessageStringTable` is a zero-initialised global (`Basedef.cpp:742`), so every row that the loader never touches reads as `""`. `SendClientMessage` then packages an empty `String[128]` into `MSG_MessagePanel` and the client renders nothing (or an empty panel). Because 225 macro names are never referenced and 215 index values have no code path, the plausible population of unbound-but-defined rows is large, but the reverse case — a code path whose index is missing from `Language.txt` — is what produces user-visible blanks.

The inability to bind a deliberately empty message compounds this: `len <= 0` is rejected at `Basedef.cpp:4699`, so "no text" and "missing text" are indistinguishable at runtime. The only place the codebase acknowledges the unbound state is `BASE_GetLanguage`, which substitutes the literal `"[UNHANDLED]"` when the *index* is out of range (`Basedef.cpp:1263, 1271, 1279, 1287, 1295, 1303, 1311`) — a guard against index bugs, not against missing resource rows. That accessor is used at only 4 of 631 message sites (`_MSG_GetItem.cpp:98`, `_MSG_Quest.cpp:1627, 1634, 1731`), so 99.4% of reads have no diagnostic path at all.

The consequence is that resource/code drift is only observable by playing the game. There is no build step, no loader counter and no runtime report of how many of the 530 defined indices were bound, which also means the 7 hole indices (`288`, `507..512`) are indistinguishable from the 215 defined-but-unreachable ones.

**Rule workflow**:
```
1. Global array zero-initialised
2. Loader binds rows present in Language.txt (subset of 1..537)
3. Consumer reads an unbound row -> ""
4. SendClientMessage -> empty MSG_MessagePanel.String
5. Client renders an empty message (no error, no log, no counter)
6. Only BASE_GetLanguage(index out of range) produces "[UNHANDLED]" (4 sites)
```

---

### Business Rule: BR-11 Presentation truncation to 94 characters

**Overview**:
Whatever the resource file declares, the panel-message carrier delivers at most 94 characters; the chat carrier delivers 96 bytes with no guaranteed terminator.

**Detailed description**:
`MESSAGE_LENGTH` is 96 (`Basedef.h:129`). `SendClientMessage` copies exactly 96 bytes from the caller's pointer into `MSG_MessagePanel.String` (`SendFunc.cpp:39`) and then forces bytes 94 and 95 to zero (`SendFunc.cpp:40-42`). Byte 95 would already be the terminator position for a 95-character string, so the *effective* limit is 94 characters. `MSG_MessagePanel.String` is itself `char[128]` (`Basedef.h:1195-1199`), a size chosen separately ("Correct size to fix SendScore Hp Bug" per the inline comment), which means the struct has 32 bytes of slack the carrier never uses but the memcpy cannot overrun.

This produces a three-way disagreement about allowed length: loader ceiling 127 bytes (`Basedef.cpp:4699`), panel carrier 94 bytes (`SendFunc.cpp:40-42`), chat carrier 96 bytes without termination (`SendFunc.cpp:1647-1660`). A localized message of 100 bytes binds cleanly, truncates at 94 for `SendClientMessage` (used by 386 sites), and overruns the chat field for `SendSay` (73 sites). With a multi-byte code page, truncation at a fixed byte offset can split a lead/trail pair and produce a replacement glyph at the end of the line.

For `MSG_MessageChat`, `memcpy(sm_mc.String, Message, MESSAGE_LENGTH)` (`SendFunc.cpp:1656`) is followed by no sentinel writes at all. Callers normally pass the global `temp[4096]` (`Server.h:382`) filled by `sprintf`, which is NUL-terminated, so the common case is safe; the unsafe case is a message whose *formatted* length is 96 bytes or more, which is reachable whenever the resource text alone exceeds ~60 bytes and two substitutions are appended. Confidence in the empty-terminator risk: **high** from code reading; exploitability depends on resource content, which is not present.

**Rule workflow**:
```
1. resource text up to 127 bytes            (Basedef.cpp:4699)
2. caller may prepend/append with sprintf   (e.g. "%s [%s]" at ProcessDBMessage.cpp:272)
3. SendClientMessage: memcpy 96, String[94]=0, String[95]=0   -> 94 chars max
4. SendSay:           memcpy 96, no terminator                -> 96 bytes verbatim
5. SendNotice/Area:   fan-out over users, then path 3
```

---

### Business Rule: BR-12 Substitution happens in code, not in data

**Overview**:
The resource file supplies a `printf` format string; all substitution is performed by C call sites with `sprintf` into the global `temp[4096]` before the carrier is invoked.

**Detailed description**:
150 of the 631 message-argument references place the table entry in the format position of `sprintf` — for example `sprintf(temp, g_pMessageStringTable[_I64D_TOWN_TAX], pMob[target].MOB.Exp)` (`_MSG_Challange.cpp:56`, 2 sites), `sprintf(temp, g_pMessageStringTable[_SSNN_GuildWarResult], GuildChallangeName, GuildChargeName, ChallangeSurvivor, ChargeSurvivor)` (`Server.cpp:6179, 6188`), and `sprintf(cMsg, "%s [%s]", g_pMessageStringTable[_NN_Item_Arrived], g_pItemList[m->Item.sIndex].Name)` (`ProcessDBMessage.cpp:272`, where the entry is an argument, not the format). The remaining 481 references hand the raw row to a carrier.

Because the entry *is* the format string, the resource file is trusted to contain well-formed `printf` conversions. A stray `%` in a translation (for example a percentage sign in "100% damage") becomes a conversion specifier and causes argument mis-consumption — the classic format-string defect, reachable here by editing a text file rather than by an attacker. The reverse hazard is the `_NN_` entry that secretly contains `%s` (evidenced by the two-argument `_NN_Check_User_Info` calls at `_MSG_MessageWhisper.cpp:1335, 1346`): a site that *does* format it produces the substituted text, while a site that passes such an entry raw delivers the literal `%d` or `%s` to the player (the 13 raw-template cases in Appendix B, section A).

Output buffers are inconsistent. Most `sprintf` targets are the global `char temp[4096]` (`Server.cpp:277`, `Server.h:382`), which cannot overflow from a 127-byte format plus short arguments, but three sites use a local `char cMsg[128]` (`ProcessDBMessage.cpp:270, 302`, `Server.cpp:8834`) with the format `"%s [%s]"` and an item name of up to 63 bytes: 127 + 3 + 63 + 1 exceeds 128, so a long localized string plus a long item name overflows the stack buffer. This is the one place where resource length converts directly into memory corruption; confidence: **high** (arithmetic is unconditional, only the resource content is unknown).

**Rule workflow**:
```
1. call site selects index
2. sprintf(temp[4096] or cMsg[128], g_pMessageStringTable[IDX], args...)
3. resource text is interpreted as a printf format
4. extra args ignored; missing args read from stack; stray '%' consumes an argument
5. resulting buffer handed to SendClientMessage / SendSay / SendNotice
6. cMsg[128] path can overflow when (text length + 3 + item name length) > 127
```

---

### Business Rule: BR-13 Localized text doubles as admin command keyword

**Overview**:
Six whisper commands are recognized by comparing the incoming `MobName` field against the *translated* string, with an ASCII fallback.

**Detailed description**:
`_MSG_MessageWhisper.cpp:582` compares `m->MobName` to `g_pMessageStringTable[_NN_Kingdom]` and to the literal `"kingdom"`; the same dual test appears at `:597` (`_NN_King` / `"king"`), `:611` (`_NN_Summon_Guild` / `"summonguild"`), `:648` (`_NN_Summon` / `"summon"`), `:796` (`_NN_Relocate` / `"relo"`), and `:1291` (`_NN_Reply` plus the literals `"r"` and two non-ASCII single-glyph forms). The intent is localization: a player types the translated word, the server matches it. The effect is that the catalog is not merely a presentation concern — it participates in command dispatch.

This creates a bidirectional dependency the component's otherwise-leaf position does not suggest. Editing message 184 (`_NN_Kingdom`), 185 (`_NN_King`), 216 (`_NN_Summon_Guild`), 89 (`_NN_Summon`), 99 (`_NN_Relocate`) or 101 (`_NN_Reply`) changes the accepted command set, and because the English fallback is hardcoded adjacent to each comparison, a translation that collides with another word (for example translating 184 to the same string as 185) makes one branch unreachable — the `else if` chain at 582..796 means the first match wins. No length or charset validation exists on `MobName` beyond the 16-byte field clamp performed upstream.

The same read pattern is used nowhere else: a full scan finds exactly these six `strcmp` sites against the table, so the catalog's control-flow surface is small and enumerable — a useful property for any change-impact analysis, and the reason this rule is stated separately from BR-12.

**Rule workflow**:
```
1. client sends whisper with MobName = "kingdom" or its translation
2. _MSG_MessageWhisper: MobName clamped to NAME_LENGTH-1/-2
3. strcmp(MobName, table[_NN_Kingdom]) || strcmp(MobName, "kingdom")
4. on match -> teleport by Clan (1690,1618 / 1690,1842 / ...)
5. Log("etc,kingdom command %d", Clan)
6. else-if chain continues to the next keyword
```

---

### Business Rule: BR-14 Contiguous index blocks addressed arithmetically

**Overview**:
Two subsystems compute the index instead of naming it, which silently converts "these definitions must stay adjacent and in order" into a runtime requirement.

**Detailed description**:
`ProcessSecMinTimer.cpp:107-108` computes `int messageId = ServerDown / 20;` and reads `g_pMessageStringTable[messageId + 17]`. Given `ServerDown` is set to `1` by the shutdown command (`Server.cpp:4094`, `imple.cpp:651`) or to a negative lead-in `-1 * ServerIndex * (ival1 * 2)` clamped to `>= -1000` (`imple.cpp:655-658`), and given the guard `ServerDown % 20 == 1`, the reachable values are 1, 21, 41, 61, 81, 101 producing `messageId` 0..5 and indices **17..22** — exactly `_NN_ServerRebootAfter_1minute` (17) through `_NN_ServerReboot_Now` (22). Those six definitions are nowhere referenced by name in the codebase, so a static "unused macro" sweep classifies them as dead; they are in fact the live countdown warnings.

`_MSG_UseItem.cpp:1093` computes `int Type = sourIndex - 575;` and `_MSG_UseItem.cpp:1118` and `:1187` read `g_pMessageStringTable[Type + _NN_Only_Level1_Armor]`. The guard at `_MSG_UseItem.cpp:1095` (`if (sourIndex - 575 >= 0 && Type < 4)`) bounds `Type` to 0..3, so indices **224..227** are reachable = `_NN_Only_Level1_Armor` .. `_NN_Only_Level4_Armor`. The base is written as a named macro (224) but the three siblings are not.

The rule is therefore: *a block referenced as `base + n` must remain physically contiguous and ordered*, and it must be renumbered as a block if the base ever changes. Since insertion at the base would silently shift meaning (index 225 would render "Level 1" text for a level-2 item), this is the one place where BR-02's "never renumber" rule is load-bearing for correctness rather than for client compatibility. A name-based static sweep reports 225 unreferenced names; that figure overstates dead text by exactly these nine indices (17..22 and 225..227).

**Rule workflow**:
```
A. shutdown countdown
   1. ServerDown set to 1 (or negative lead-in)
   2. each tick: if (ServerDown % 20 == 1) messageId = ServerDown / 20
   3. index = messageId + 17  -> 17,18,19,20,21,22
   4. SendNotice(index text) to all PLAY users
B. armor refinement rejection
   1. Type := item->sIndex - 575
   2. guard: Type >= 0 && Type < 4
   3. index = Type + 224      -> 224,225,226,227
   4. SendClientMessage(conn, text)
```

---

### Business Rule: BR-15 Bounds-checked accessor contract

**Overview**:
When an index is a variable rather than a literal, the codebase provides `BASE_GetLanguage` overloads that validate the range and emit `[UNHANDLED]` instead of reading out of bounds.

**Detailed description**:
`Basedef.cpp:1258-1312` defines seven overloads, one per argument shape: `(char *str, int idx)`, `(char *, int, char *)`, `(char *, int, char *, char *)`, `(char *, int, int)`, `(char *, int, int, int)`, `(char *, int, char *, int)`, `(char *, int, int, char *)`. Each performs `if (idx >= 0 && idx < MAX_STRING)` and then `sprintf(str, g_pMessageStringTable[idx], ...)`; on failure it writes the literal `"[UNHANDLED]"`. Declarations are at `Basedef.h:2337-2343`. The overload set is a genuine facade: it caps the index and documents (by its existence) that variable indices are a supported case.

In practice the facade is nearly unused: only 4 call sites exist (`_MSG_GetItem.cpp:98`, `_MSG_Quest.cpp:1627`, `:1634`, `:1731`), against 653 direct `g_pMessageStringTable[...]` accesses. The direct sites rely on the compiler to fold a literal index, which is always in range (maximum referenced literal is 537, below `MAX_STRING`), plus the two arithmetic sites whose guards were examined under BR-14. So the bounds-checked path is not load-bearing for current correctness — but it is the only place where the "index out of namespace" condition has a defined representation.

Note the asymmetry: the accessor guards *index range* but not *row content* and not *argument count*, and it uses the row as a format string just like the direct sites (same `%`-in-translation hazard as BR-12). It also `sprintf`s into a caller-provided buffer with no length parameter, so a 127-byte text into a small caller buffer is a caller-side risk.

**Rule workflow**:
```
1. caller has a non-constant index (rare: 4 sites)
2. BASE_GetLanguage(str, idx[, parms...])
3. if 0 <= idx < 2000 -> sprintf(str, table[idx], parms...)
4. else                -> strcpy(str, "[UNHANDLED]")
5. direct g_pMessageStringTable[...] sites bypass this guard entirely (653 sites)
```

---

### Business Rule: BR-16 Byte-transparent encoding, single locale

**Overview**:
Text is copied as raw bytes; there is no transcoding, no BOM handling, no per-locale file, and the build is `MultiByte`.

**Detailed description**:
`TMSrv.vcxproj:27` and `:34` set `<CharacterSet>MultiByte</CharacterSet>` for both configurations, and `Basedef.cpp:4706` uses `strcpy` on `char` buffers. `fopen(file, "rt")` (`Basedef.cpp:4641`) opens in text mode, which on Windows translates CRLF to LF; the parser then treats `\n` as a terminator (`4689`), so CRLF-delimited resources work. A UTF-8 BOM at the head of the file would be part of the first line's leading token and would make `sscanf("%d")` fail for line 1 only — an off-by-one-entry failure rather than a parse abort.

No code page conversion occurs anywhere in the path, so the bytes in `Language.txt` reach the client verbatim inside `MSG_MessagePanel.String` / `MSG_MessageChat.String`. The client must interpret them with the same code page. `TMSrv.rc:19` declares `LANGUAGE LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN`, the source contains both Portuguese identifiers (`_MSG_CombineItemExtracao.cpp`, `_MSG_CombineItemAlquimia.cpp`, log strings such as `"useitem,olho_crescente"`) and Korean-origin strings (the two non-ASCII `Reply` aliases at `_MSG_MessageWhisper.cpp:1292`), so a deployment may carry CP949 or CP1252 bytes with no marker either way.

The interaction with BR-07/BR-11 is the practical consequence: all length limits are byte-based, so a double-byte locale loses roughly half its character budget, and truncation at 94 or 127 bytes can bisect a glyph because nothing checks for a trailing lead byte. There is also no validation that the resource is in the expected encoding, so a UTF-8 file loaded into a CP949 client yields deterministic mojibake rather than a diagnostic.

**Rule workflow**:
```
1. Language.txt opened "rt" (CRLF -> LF)
2. bytes copied 1:1 into char[128] rows
3. bytes copied 1:1 into MSG_MessagePanel.String[128] / MSG_MessageChat.String[96]
4. no conversion, no validation, no BOM handling
5. client must decode with the matching code page
```

---

### Business Rule: BR-17 Dormant binary cache (UI/strdef.bin)

**Overview**:
A second, binary representation of the same table is implemented with XOR obfuscation and a checksum, but no code in the repository calls it.

**Detailed description**:
`BASE_WriteMessageBin()` (`Basedef.cpp:4903-4930`) XORs all `2000 * 128` bytes with `0x5A`, writes them to `UI/strdef.bin`, appends `BASE_GetSum(...)` over the pre-XOR buffer, then XORs the in-memory table back. `BASE_ReadMessageBin()` (`Basedef.cpp:4932-4961`) reads the payload, reads the stored checksum, and then — critically — the comparison is **commented out** (`Basedef.cpp:4951-4954`), so the function always returns `TRUE` and the checksum has no effect. Both functions are declared at `Basedef.h:2397-2398` and neither is called anywhere in `Code/**` (verified across `TMSrv`, `DBSrv`, `Basedef`).

The pair represents a historical design in which the catalog was shipped as a client-side binary blob with integrity checking. Its presence in the component boundary matters for three reasons: it documents an intended obfuscation contract (`XOR 0x5A`, fixed 256,000-byte stride, trailing 4-byte sum) that a client or patch tool may still expect; it creates a second potential source of truth (`Language.txt` vs `UI/strdef.bin`) whose precedence is decided only by which function the caller chooses; and its disabled checksum means that if it were ever re-enabled, tampered content would be accepted. The `.gitignore` and resource layout contain no `UI/` directory, and the architecture report lists `UI/strdef.bin` as an expected runtime artefact ("Server both consumes and generates binary caches").

**Rule workflow**:
```
A. write (uncalled)
   1. XOR 256000 bytes in place with 0x5A
   2. fwrite to UI/strdef.bin
   3. fwrite 4-byte checksum
   4. XOR back (restore in-memory text)
B. read (uncalled)
   1. fread 256000 bytes
   2. fread stored checksum
   3. compute sum (result unused, comparison commented out)
   4. XOR 0x5A in place -> table becomes plaintext
   5. return TRUE unconditionally
```

---

### Business Rule: BR-18 Orphan definitions are legal

**Overview**:
A definition with no call site is a normal state; neither the compiler nor the loader requires the index space and the code to agree.

**Detailed description**:
225 of the 530 macro names are referenced nowhere in `Code/**` (42.5%), and 215 distinct values have no code path at all once raw literals and the two arithmetic blocks are accounted for. The distribution is not uniform: the tail of the file is heavily orphaned (auction 486..491, guild zone 503..505, encampment 537), and whole themed groups are dead (lotto 332..343, quiz 318..324, billing notices 123..129 / 159..160, Heaven War results 393..397). Conversely, some "dead" names are live through other mechanisms: 17..22 via `messageId + 17`, 225..227 via `Type + 224`, 114 via the literal `g_pMessageStringTable[114]` (`_MSG_Buy.cpp:183, 188`), 169 via the literal `[169]` (`_MSG_MessageChat.cpp:91`), and 66/67/68/71 via `BASE_GetLanguage`, 89/99/184/185/216 via whisper keywords.

The rule matters because orphan tolerance is what allows the catalog to be a shared, versioned artefact: entries can be retired from code while their index and client-side text survive, and text can be added ahead of the code that will use it. It also means static "unused" analysis is unsafe as a deletion criterion — the two arithmetic blocks prove that a name-based sweep would delete live server behaviour (reboot countdown and armor-level rejection). Confidence that the 225-name figure is complete for *name* references: **high**; confidence that a given orphan is truly unreachable: **medium**, because raw literals and arithmetic are outside a name-based scan.

**Rule workflow**:
```
1. author adds definition (possibly ahead of the consuming code)
2. translator adds the row
3. no consumer yet -> text never displayed
4. static scan reports "unused" -> classification is advisory only
5. exceptions: arithmetic blocks, raw literals, BASE_GetLanguage, keyword compares
```

---

## 4. Component Structure

```
Code/
├── Basedef.h                                   # Storage + accessor contract (declarations)
│   ├── MAX_STRING               = 2000         # Basedef.h:150   array rows
│   ├── MESSAGE_LENGTH           = 96           # Basedef.h:129   carrier field width
│   ├── BASE_GetLanguage (7 overloads)          # Basedef.h:2337-2343
│   ├── BASE_InitializeMessage(char *file)      # Basedef.h:2395
│   ├── BASE_WriteMessageBin() / ReadMessageBin() # Basedef.h:2397-2398
│   ├── MSG_MessagePanel { char String[128] }   # Basedef.h:1195-1199
│   ├── MSG_MessageChat  { char String[96]  }   # Basedef.h:1440-1444
│   └── extern char g_pMessageStringTable[MAX_STRING][128]  # Basedef.h:2461
│
├── Basedef.cpp                                 # Storage + loader + accessors (definitions)
│   ├── char g_pMessageStringTable[MAX_STRING][128]        # :742  (256,000 bytes)
│   ├── BASE_GetLanguage x7 (bounds-checked, [UNHANDLED])  # :1258-1312
│   ├── BASE_InitializeMessage(char *file)                 # :4637-4710
│   │   ├── memset(g_pMessageStringTable, MAX_STRING, 128) # :4639  (defective reset)
│   │   ├── fopen(file, "rt") / silent return if NULL      # :4641-4644
│   │   ├── fgets loop + sscanf("%d %s")                   # :4650-4658
│   │   ├── index gate 0 < Index <= MAX_STRING             # :4663
│   │   ├── tab search in first 256 bytes + MessageBox     # :4668-4683
│   │   ├── payload slice at tab/newline/NUL               # :4685-4695
│   │   ├── length gate 1..127 + MessageBox                # :4697-4704
│   │   └── strcpy(g_pMessageStringTable[Index], cont)     # :4706
│   ├── BASE_WriteMessageBin()  XOR 0x5A -> UI/strdef.bin  # :4903-4930 (uncalled)
│   └── BASE_ReadMessageBin()   UI/strdef.bin -> XOR       # :4932-4961 (uncalled, checksum disabled :4951-4954)
│
└── TMSrv/
    ├── Language.h                              # THE COMPONENT (18,074 bytes, 550 lines)
    │   ├── :1-18    GPLv3 banner (author removed in 79b14f7)
    │   ├── :19      #pragma once
    │   ├── :21-550  530 #define directives, values 1..537, 7 holes
    │   └── (no #include, no code, no types, no functions)
    │
    ├── ProcessClientMessage.h:32               # #include "Language.h"  (transitive hub, 66 TUs)
    │
    ├── Binding / lifecycle
    │   ├── Server.cpp:7158-7160                # BASE_InitializeBaseDef + BASE_InitializeMessage("Language.txt")
    │   ├── imple.cpp:250-262                   # GM "reloadfile" re-bind
    │   └── Server.cpp:46                       # int ServerDown = -1000 (drives index block 17..22)
    │
    ├── Presentation carriers
    │   ├── SendFunc.cpp:27-45                  # SendClientMessage: memcpy 96, force [94]=[95]=0
    │   ├── SendFunc.cpp:47-62                  # SendNotice: fan-out to all PLAY users
    │   ├── SendFunc.cpp:155-166                # SendNoticeArea: region fan-out
    │   ├── SendFunc.cpp:1647-1660              # SendSay: memcpy 96, no terminator
    │   └── SendFunc.cpp:179                    # SendClientMessageOk (arguments unused)
    │
    └── Consumers (46 translation units referencing at least one index)
        ├── _MSG_UseItem.cpp      147 table refs (highest)
        ├── _MSG_Quest.cpp        134
        ├── _MSG_MessageWhisper.cpp 67 (incl. 6 keyword strcmp)
        ├── Server.cpp             61
        ├── _MSG_Trade.cpp         27
        ├── ProcessDBMessage.cpp   23
        ├── _MSG_CombineItemOdin.cpp 18
        ├── MobKilled.cpp          17
        ├── _MSG_Attack.cpp        12
        ├── _MSG_ApplyBonus.cpp    10
        ├── CCastleZakum.cpp       10
        ├── SendFunc.cpp           10
        ├── _MSG_AcceptParty.cpp    9
        └── ... 33 further handler TUs with 1-8 refs each
```

**Composition notes**

- The component is **header-only**: `Language.h` declares nothing, includes nothing and compiles to nothing. Its precondition (`g_pMessageStringTable` being bound) lives in a different file (`Basedef.cpp`).
- `Code/TMSrv/Language.h` is listed in the project as `<ClInclude Include="Language.h" />` (`TMSrv.vcxproj:179`); no `ClCompile` entry, consistent with a pure constant header.
- Formatting inconsistencies inside the header: one entry is tab-indented (`Language.h:542`, `#define\t_NN_PINCODE_INVALID\t529`) while the other 529 use a space; the file had no trailing newline until commit `79b14f7`.
- No data file ships with the repository: `Language.txt` (input) and `UI/strdef.bin` (dormant cache) are both absent from the working tree and from git.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies

```
Language.h  (leaf: zero outgoing dependencies, zero #include)
     ^
     |  #include "Language.h"                     12 files
     |  #include "ProcessClientMessage.h"         66 files (line 32 of that header)
     |
     +-- Consumed value: preprocessor integers only
     
g_pMessageStringTable  (storage, Basedef.cpp:742 / Basedef.h:2461)
     ^
     +-- written by  BASE_InitializeMessage       1 site  (Basedef.cpp:4706)
     +-- reset by    memset                       1 site  (Basedef.cpp:4639)
     +-- read by     653 symbolic + 7 literal + 3 computed accesses across 48 files
     +-- read by     BASE_GetLanguage x7          (Basedef.cpp:1261-1309)
     +-- read/written by BASE_Write/ReadMessageBin (0 call sites)

BASE_InitializeMessage  (Basedef.cpp:4637)
     +-- called by Server.cpp:7160   (boot)
     +-- called by imple.cpp:253     (GM reloadfile)
     +-- depends on: C runtime fopen/fgets/sscanf/strcpy/strlen/memset
     +-- depends on: Win32 user32 MessageBox

SendClientMessage  (SendFunc.cpp:27)
     +-- called by 386 catalog sites + 145 hardcoded-literal sites
     +-- depends on: MSG_MessagePanel (Basedef.h:1195), pUser[], CPSock::AddMessage
```

Notes on direction: the catalog depends on **nothing** in the source tree, which is why it is recorded as efferent 0. The dependency from `Basedef` to the catalog is inverted relative to normal layering: the *data* lives in the shared base layer (`Code/Basedef.cpp`), while the *names* live in the game-server layer (`Code/TMSrv/Language.h`). A `DBSrv`-only build therefore still compiles the 256 KB array (`DBSrv.vcxproj:94`) without ever binding or reading it.

### 5.2 External dependencies

| Dependency | Version / Form | Purpose | Evidence |
|---|---|---|---|
| `Language.txt` | Runtime data file, tab-delimited text, CWD-relative | Supplies the 530 row texts | `Basedef.cpp:4641`, `Server.cpp:7160`, `imple.cpp:253` |
| C runtime (`fopen`, `fgets`, `sscanf`, `strcpy`, `strlen`, `memset`, `fread`, `fwrite`) | MSVC CRT, `MultiThreaded(Debug)` static / DLL | File I/O and buffer copies | `TMSrv.vcxproj:62,80` |
| Win32 `user32!MessageBox` / `MessageBoxA` | OS | Surfaces parse errors modally | `Basedef.cpp:4680, 4701, 4756, 4787, 4858, 4867, 4875, 4892` |
| `UI/strdef.bin` | Runtime data file, 256,004 bytes, XOR 0x5A + checksum | Dormant binary cache | `Basedef.cpp:4913, 4940` (0 call sites) |
| Client string table (implied) | Out of repository — `Code/ClientPatch_v7662/` contains only `ClientPatch_v7662.vcxproj.user` | Index parity for display and command names | Absence of client source; architecture report notes client source absent |
| MSVC toolset | v143 / Visual Studio 2022 | `MultiByte` charset, `_CRT_SECURE_NO_WARNINGS`, `WarningLevel 3` | `TMSrv.vcxproj:27,34,59,61,73,79` |

### 5.3 Build-time dependency facts

- `TMSrv.vcxproj` has 75 `ClCompile` entries, all product sources; `Language.h` appears once as `ClInclude` (`:179`).
- `DBSrv.vcxproj:94` compiles `..\Basedef.cpp`, so `g_pMessageStringTable` exists twice in a full deployment (once per process) with only TMSrv's copy populated.
- `_CRT_SECURE_NO_WARNINGS` is defined in both build configurations (`TMSrv.vcxproj:61` Debug, `:79` Release), which suppresses the C4996 warnings that would otherwise flag `strcpy`/`sprintf`/`sscanf` at the loader and every consumer.

---

## 6. Afferent and Efferent Coupling

Components in this codebase are C-style files and globals rather than classes; the table below therefore treats each named symbol (macro namespace, global array, function) as the unit of coupling. "Afferent" counts dependent code units that reference the symbol; "efferent" counts code units the symbol itself depends on.

| Component / Symbol | Afferent Coupling | Efferent Coupling | Critical | Notes |
|---|---|---|---|---|
| `Language.h` (macro namespace) | 70 files (12 direct `#include`, 66 via `ProcessClientMessage.h:32`; union 70) | 0 | High | Leaf header; referenced by 46 TUs at value level |
| `g_pMessageStringTable` (global array) | 48 files (46 consumer TUs + `Basedef.cpp` + `Basedef.h`) | 0 | High | 663 total accesses (653 symbolic, 7 literal, 3 computed) |
| Individual hot indices (e.g. `_NN_Cant_Refine_More` 75) | 1-2 files, up to 16 call sites | 0 | Low-Medium | 304 values reachable by name; maximum single-index fan-in is 16 |
| `BASE_InitializeMessage` | 2 call sites (`Server.cpp:7160`, `imple.cpp:253`) | 6 CRT + 1 Win32 | High | Sole writer; void return, no error channel |
| `BASE_GetLanguage` (7 overloads) | 4 call sites | 1 global array | Low | Facade exists but is bypassed by 653 direct accesses |
| `BASE_ReadMessageBin` / `WriteMessageBin` | 0 | 3 CRT + 1 helper (`BASE_GetSum`) | None (dormant) | Reachable only by future call sites |
| `SendClientMessage` (catalog's egress) | 531 call sites (386 catalog + 145 literal) | `MSG_MessagePanel`, `pUser[]`, `CPSock` | High | Also the truncation point (94 chars) |
| `SendSay` | 73 catalog sites | `MSG_MessageChat`, `pUser[]`, `GridMulticast` | Medium | No forced NUL terminator |

**Interpretation**

- The header's afferent count (70) is close to the total server TU count, because `ProcessClientMessage.h` includes it for every handler. The **value-level** coupling is lower (46 TUs actually name an index). The architectural report's figure of ~52 afferent is consistent with counting direct includes plus explicit consumers.
- Efferent is genuinely 0: the header has no `#include`, no external type, no function. This is the strongest structural property of the component and the reason it compiles into any TU without risk of include cycles.
- Coupling is *wide but shallow*: 70 dependents, each touching only an integer constant or a global array element. There is no encapsulation (`g_pMessageStringTable` is a raw global with no accessor on the hot path), so cohesion is declarative rather than enforced: the names group semantically (quests, trade, war) but the storage and the loader know nothing about those groups.
- The one place where coupling becomes semantic rather than structural is BR-13 (six `strcmp` sites) and BR-14 (two arithmetic blocks). In both, a change to a *value* or to a *text* changes control flow, which is a materially tighter coupling than the counts suggest.

**Per-index concentration (top consumers)**

| Index | Value | References | Files |
|---|---|---|---|
| `_NN_Cant_Refine_More` | 75 | 16 | `_MSG_UseItem.cpp` (15), `_MSG_CombineItemOdin.cpp` (1) |
| `_NN_Processing_Complete` | 158 | 16 | 8 combine-handler TUs |
| `_NN_Only_To_Equips` | 74 | 16 | `_MSG_UseItem.cpp` (16) |
| `_NN_Cant_Use_That_Here` | 96 | 16 | `_MSG_UseItem.cpp` (10), `_MSG_MessageWhisper.cpp` (5), `_MSG_Attack.cpp` (1) |
| `_NN_Not_Connected` | 91 | 15 | `_MSG_MessageWhisper.cpp` (10), `_MSG_AcceptParty.cpp` (2), 3 others |
| `_NN_CombineFailed` | 269 | 15 | combine-handler TUs |
| `_NN_Level_Limit2` | 340 | 15 | `_MSG_Quest.cpp` (12), `_MSG_UseItem.cpp` (3) |
| `_DN_SANCREQ` | 520 | 12 | `_MSG_UseItem.cpp` (12) |

---

## 7. Integration Points

> The component exposes no network endpoints (no REST, GraphQL, gRPC, socket listener or command registration of its own). Its integration surface is a data file, a shared global, an optional runtime rebind, and the client protocol message types that carry its text.

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| `Language.txt` (CWD) | File system resource | Text binding for all 530 indices | `fopen("rt")`, line-oriented | `<decimal index><TAB><ANSI bytes>` | Missing file: silent return, table stays zeroed (`Basedef.cpp:4643`); malformed line: modal `MessageBox` + skip (`:4680`, `:4701`) |
| `UI/strdef.bin` | File system resource | Dormant binary cache of the same table | `fopen("wb"/"rb")`, fixed 256,004-byte stride | 2000x128 bytes XOR `0x5A` + 4-byte additive checksum | Checksum comparison commented out (`:4951-4954`); returns `TRUE` unconditionally; never called |
| `g_pMessageStringTable` | In-process shared global | Catalog storage | Direct memory reads | `char[2000][128]`, NUL-terminated rows | No bound checking on 653 direct accesses; `BASE_GetLanguage` guards 4 |
| GM `reloadfile` command | In-process control path | Live rebind | Whisper message -> `ProcessImple` -> string compare | ASCII token `reloadfile` | No confirmation of success/failure beyond a fixed reply string (`imple.cpp:261`) |
| `MSG_MessagePanel` (`_MSG_MessagePanel = 1 | FLAG_GAME2CLIENT`) | Game-to-client packet | Delivers 386 catalog texts | `CPSock::AddMessage` on the user socket | Connection range checked (`SendFunc.cpp:29`); text truncated to 94 chars, no NUL guarantee needed because `String[128]` |
| `MSG_MessageChat` (`_MSG_MessageChat`) | Game-to-client packet | Delivers 73 `SendSay` texts | `GridMulticast` around the NPC | 96-byte `String` | No terminator forced (`SendFunc.cpp:1656`); no length validation |
| `SendNotice` / `SendNoticeArea` fan-out | In-process broadcast | 12 + 10 catalog texts to all or regional users | Repeated `SendClientMessage` | Same as panel | Silently skips non-`USER_PLAY` users |
| Diagnostic `Log()` | In-process log | Used by every subsystem *except* this one | File/log sink | Text | Not used by loader or carriers for catalog failures |
| Client message table (implied) | External binary | Renders indices the client owns | Unknown (out of repo) | Unknown | Unverifiable; `Code/ClientPatch_v7662/` has no sources |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Macro-as-enum catalog | 530 `#define NAME <int>` | `Language.h:21-550` | Compile-time constant namespace with zero runtime cost and zero type safety |
| Frozen ABI / append-only registry | Fixed values, never renumbered (git evidence) | `Language.h`, `Basedef.cpp:4663` | Preserve parity with an already-shipped client |
| Global shared table (service-locator style) | `extern char g_pMessageStringTable[MAX_STRING][128]` | `Basedef.h:2461`, `Basedef.cpp:742` | Single point of text storage readable from any TU without plumbing |
| Data-driven binding (external resource) | `BASE_InitializeMessage(char *file)` | `Basedef.cpp:4637-4710` | Localization without recompilation |
| Facade / overload set | 7 `BASE_GetLanguage` overloads with bounds check and `[UNHANDLED]` fallback | `Basedef.cpp:1258-1312` | Safe access for variable indices (used at 4 of 631 sites) |
| Format-string composition (variadic contract) | `sprintf(dst, g_pMessageStringTable[idx], args...)` | 150 sites, e.g. `_MSG_Challange.cpp:56` | Parameter substitution in code, types declared by naming convention |
| Transparent no-abstraction egress | `SendClientMessage` copies caller text into a fixed packet | `SendFunc.cpp:27-45` | Uniform client presentation; also the truncation boundary |
| Obfuscated binary cache (dormant) | XOR `0x5A` over 256,000 bytes + additive checksum | `Basedef.cpp:4903-4961` | Historical client-side distribution and tamper detection |
| Two-phase lifecycle (boot + hot reload) | `Server.cpp:7160` and `imple.cpp:253` | `Basedef.cpp:4637` | Operators can change text without restarting |
| Conflation of presentation and control data | 6 `strcmp` against localized rows | `_MSG_MessageWhisper.cpp:582,597,611,648,796,1291` | Localized admin keywords |

**Architectural decisions observed (evidence-based)**

1. *Indices are the ABI, names are convenience.* The value column is immutable; the name column is mutable. Evidence: 2015-lineage file with a frozen value space, plus newer names appended at 529..537 (`_NN_PINCODE_INVALID` ... `_NN_ENCAMPMENTBORN`) using uppercase naming that differs from the older lower-case-mixed style.
2. *Text lives in a shared base layer, not in the server layer.* Storage and loader are in `Code/Basedef.cpp`, shared with DBSrv, while the names are TMSrv-specific. This inverts the usual dependency direction and is why DBSrv carries an unused 256 KB array.
3. *Single-locale deployment.* One file name, no locale suffix, no environment override, `MultiByte` build, byte-transparent copies. A deployment changes language by replacing `Language.txt`; there is no runtime concept of a locale set.
4. *Fail-open on data, fail-loud on syntax.* A missing file is ignored (server runs with blank text); a syntactically bad line blocks on a modal dialog. The two policies are inconsistent with each other and with the rest of the codebase's `Log()` convention.
5. *No schema validation.* There is no manifest, no coverage check, no duplicate check, no type-to-format check, no encoding check. Correctness is maintained by developer discipline (the prefix convention) and by client parity.

---

## 9. Technical Debt & Risks

| Risk | Component Area | Issue | Impact |
|------|----------------|-------|--------|
| High | `ProcessDBMessage.cpp:270, 302`, `Server.cpp:8834` | `char cMsg[128]` receives `sprintf("%s [%s]", <row up to 127 bytes>, <item name up to 63 bytes>)` | Stack buffer overflow reachable by resource/translation length; arithmetic bound is 127+3+63+1 = 194 > 128 |
| High | `_MSG_GetItem.cpp:131`, `MobKilled.cpp:1779` | `sprintf(temp, table[_SSD_S_get_S], name, itemName)` supplies 2 of 3 declared arguments | Undefined behaviour: third variadic read takes stack garbage; formatted output can contain arbitrary values; potential access violation |
| High | Loader (`Basedef.cpp:4680, 4701`) | `MessageBox` used as the error channel from inside server start-up and hot reload | Unattended/service deployments hang until a human dismisses a modal window that may not be visible |
| High | Parameter contract (BR-03) | 29 of 631 message sites deviate: 13 raw parameterized sends, 8 ignored extra args, 2 under-supplies, 6 literal indices | Placeholders rendered literally (`%d`, `%s`) to players; UB in 2 sites; convention is not a reliable validator |
| High | `_MSG_MessageWhisper.cpp:582,597,611,648,796,1291` | Admin commands matched against localized text with ASCII fallbacks in an `else if` chain | Translation edit silently removes or shadows a command; collisions make branches unreachable |
| Medium | `Basedef.cpp:4639`, `Basedef.cpp:4934` | `memset(g_pMessageStringTable, MAX_STRING, 128)` — fill byte 2000 (`0xD0`), length 128 | Only row 0 is reset (with non-ASCII bytes); rows 1..1999 survive a reload, so deleted indices keep stale text |
| Medium | `Basedef.cpp:4663` vs `Basedef.cpp:4706` | Index gate allows `Index == MAX_STRING` (2000) while the array is `0..1999` | One-row out-of-bounds write if a resource line is numbered 2000 |
| Medium | `SendFunc.cpp:1647-1660` | `SendSay` copies 96 bytes with no terminator forcing, unlike `SendClientMessage:40-42` | 96+ byte localized text leaves the chat field unterminated; client-side overread / protocol desync |
| Medium | Contradiction across `Basedef.cpp:4699`, `SendFunc.cpp:40-42`, `SendFunc.cpp:1656` | Loader accepts 127 bytes, panel emits 94, chat emits 96 without terminator | Silent loss of up to 33 bytes; truncation can split a double-byte glyph |
| Medium | 225 unused macro names / 215 dead values / 7 holes (`288`, `507..512`) | No coverage or dead-entry policy; static "unused" scans are unsafe because 9 values are reached arithmetically | Deletion risk of live behaviour (reboot countdown 17..22, armor rejection 225..227); review noise |
| Medium | `_MSG_Buy.cpp:183,188`, `_MSG_GetItem.cpp:194`, `_MSG_MessageChat.cpp:91`, `Server.cpp:8148,8158` | 7 accesses use numeric literals (`[114]`, `[169]`, `[194]`, `[273]`) | Meaning is invisible at the call site; a value-space change would not be caught by name-level review |
| Medium | `Basedef.cpp:4951-4954` | Checksum verification for `UI/strdef.bin` is commented out; both cache functions are uncalled | Integrity control is nominally present but inert; two potential sources of truth with undefined precedence |
| Medium | `Basedef.cpp:4643-4644` | Missing `Language.txt` returns silently; no `Log()`, no counter, no return value | Server starts successfully, emits empty messages; failure is invisible in logs |
| Low | 145 of 671 `SendClientMessage` sites | Hardcoded English literals bypass the catalog (21.6%) | Localization is partial; language switching leaves mixed-language output |
| Low | Header naming | Case-only near-collision `_NN_Only_Guild_Master_can 24` / `_NN_Only_Guild_Master_Can 144`; typo families (`Qest`, `Challange`, `Allready`, `Enchanted`), semantic near-duplicates in 12 groups | Review confusion; wrong-index selection risk |
| Low | `DBSrv.vcxproj:94` | DBSrv compiles `Basedef.cpp`, instantiating a 256 KB zeroed array it never binds or reads | Wasted per-process memory; false impression that DBSrv localizes text |
| Low | `Language.h:542` and header hygiene | Tab-indented entry, no blank-line grouping by domain, no ownership annotation per index | Maintainability; index ownership must be inferred from the name |
| Low | Build configuration | `_CRT_SECURE_NO_WARNINGS` + `WarningLevel 3` suppress the CRT deprecation warnings that would surface the `strcpy`/`sprintf` sites above | Defects are invisible to the compiler; no static analysis in the build |

---

## 10. Test Coverage Analysis

**Result: no tests exist for this component, and no test infrastructure exists in the repository.**

Evidence gathered across the full project root (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`):

1. **No test files.** A recursive scan for paths matching `*test*`, `*spec*`, `*_t.cpp`, `*_unittest*` returns nothing outside `openspec/` (which is specification tooling, not tests). No `.cpp` file in `Code/**` contains a test harness, a `main` unrelated to the servers, or assertion macros.
2. **No test project.** The solution `W2PP Code Project.sln` contains only `TMSrv` and `DBSrv` (`TMSrv.vcxproj` has 75 `ClCompile` entries, all product sources; `DBSrv.vcxproj` similarly). There is no third project, no `gtest`/`catch2`/`CppUnit` reference, and no `packages.config`/`vcpkg.json` pulling a test framework.
3. **No test fixtures or golden data.** `Language.txt` — the only input the component consumes — is absent from the working tree and from git, so there is not even a sample resource to assert against. `UI/strdef.bin` is likewise absent.
4. **No CI/CD pipeline.** No `.github/`, `.gitlab-ci.yml`, `azure-pipelines.yml`, `appveyor.yml`, `Jenkinsfile` or `CMakeLists.txt` exists; the only build path is a Visual Studio solution opened locally.
5. **No runtime self-check in the component.** `BASE_InitializeMessage` returns `void`, exposes no count of bound rows, and performs no format/duplicate/coverage validation that a test could observe. `BASE_ReadMessageBin` returns `TRUE` unconditionally (`Basedef.cpp:4960`).

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `Language.h` index namespace | 0 | 0 | 0% | Not applicable — preprocessor constants; no assertions possible in-repo |
| `BASE_InitializeMessage` (loader) | 0 | 0 | 0% | No tests for grammar, index gate, length gate, tab requirement, duplicate handling, missing file |
| `BASE_GetLanguage` (accessor overloads) | 0 | 0 | 0% | Bounds-check branch and `[UNHANDLED]` fallback untested |
| `BASE_WriteMessageBin` / `BASE_ReadMessageBin` | 0 | 0 | 0% | Dead code, therefore also untested; XOR round-trip and checksum path unverified |
| `SendClientMessage` truncation (94-byte rule) | 0 | 0 | 0% | No test for boundary lengths, UTF-8/CP949 split, empty text, or long text |
| `SendSay` termination (96-byte, unterminated) | 0 | 0 | 0% | No test for 96+ byte text |
| Parameter-prefix contract (631 sites) | 0 | 0 | 0% | No compile-time or runtime check; 29 deviations undetected |
| Emitted-message integration (client rendering) | 0 | 0 | 0% | Client sources absent (`Code/ClientPatch_v7662/` holds only a `.vcxproj.user`) |

**How correctness is actually maintained today**

- Manual, environment-level verification: place a valid `Language.txt` next to the executables (required by the architecture report's "runtime layout" list) and observe in-game text.
- Implicit contract testing through the client: index parity is validated only by the shipped client rendering the expected string.
- Static review of the naming convention: the only defence against a wrong argument count is a developer noticing the `_SN_`/`_DN_` prefix.

The absence of automated verification is why these defects remain undetected: the two under-supplied `sprintf` sites, the 94-vs-127 truncation mismatch, the `memset` argument-order defect and the `Index == MAX_STRING` off-by-one would each be exposed by a fixture-driven test of the loader and the two carriers.

---

## Appendix A — Index namespace inventory

**Emitted values:** 1..537 (530 definitions, 0 duplicates)

**Absent values inside the range:** `288`, `507`, `508`, `509`, `510`, `511`, `512`

**Unallocated headroom:** 538..2000 (1463 slots, 73% of `MAX_STRING`)

**Value 0:** not defined, not referenced, rejected by the loader gate (`Basedef.cpp:4663`)

**Prefix distribution (definitions):**

| Prefix | Count | Declared arity | Observed call behaviour |
|---|---|---|---|
| `_NN_` | 385 | 0 | 465 calls with 0 args; 8 calls with 1-2 args (extra args ignored) |
| `_DN_` | 54 | 1 numeric | 55 with 1 arg; 7 sent raw via send helpers |
| `_SN_` | 39 | 1 string | 41 with 1 arg; 1 sent raw |
| `_DD_` | 18 | 2 numeric | 6 with 2 args; 3 sent raw |
| `_SS_` | 15 | 2 strings | 12 with 2 args (one site, `_MSG_UseItem.cpp:468`, is commented out); 3 sent raw |
| `_DS_` | 4 | string + numeric (order per name tail) | 2 sites, 2 args each (`_DS_S_Challanged`, `_DS_S_cure_price_D`) |
| `_SD_` | 3 | string + numeric (order per name tail) | 2 with 2 args (`_SD_Zakum_Quest_by_S_N`) |
| `_SSD_` | 3 | 2 strings + 1 numeric | 1 with 3 args; **2 with 2 args (UB)** |
| `_SSDD_` | 3 | 2 strings + 2 numeric | 0 call sites |
| `_SSNN_` | 1 | 2 strings + 2 numeric | 2 with 4 args (`_SSNN_GuildWarResult`) |
| `_ND_` | 2 | undocumented | 0 call sites |
| `_UN_` | 1 | undocumented | 0 call sites |
| `_NS_` | 1 | 1 numeric (observed) | 6 with 1 arg (`_NS_GETEXP`) |
| `_I64D_` | 1 | 1 int64 | 2 with 1 arg (`_I64D_TOWN_TAX`) |

**Reachability summary**

| Category | Count |
|---|---|
| Defined macro names | 530 |
| Referenced by name in code | 305 |
| Never referenced by name | 225 |
| Values reachable via symbolic name | 304 |
| Values reachable only arithmetically (`17..22`, `225..227`) | 9 |
| Values reachable only via numeric literal (`114`, `169`) | 2 |
| Total reachable values | 315 of 537 (58.7%) |
| Defined but unreachable values | 215 |
| Undefined holes in range | 7 |

---

## Appendix B — Parameter-contract deviations (complete list)

**A. Raw parameterized entry passed to a carrier with no formatting (13 sites)**

| File:Line | Index | Prefix | Carrier |
|---|---|---|---|
| `_MSG_CombineItemOdin.cpp:133` | `_SS_Combin_12Succ` (462) | SS | `SendNotice` |
| `_MSG_CombineItemOdin.cpp:264` | `_SS_Combin_12Succ` (462) | SS | `SendNotice` |
| `_MSG_MessageWhisper.cpp:44` | `_DN_NO_TOWNSPEOPLE` (513) | DN | `SendClientMessage` |
| `_MSG_MessageWhisper.cpp:194` | `_DN_NO_TOWNSPEOPLE` (513) | DN | `SendClientMessage` |
| `_MSG_Quest.cpp:1603` | `_SN_I_Wwill_Enchant_Your_Weapon` (61) | SN | `SendClientMessage` |
| `_MSG_Quest.cpp:2435` | `_DD_JOINTOWNPEP` (427) | DD | `SendClientMessage` |
| `_MSG_Quest.cpp:2486` | `_DN_TOWN_SOUL_BUFF` (516) | DN | `SendClientMessage` |
| `_MSG_Quest.cpp:2511` | `_DN_TOWN_SOUL_NOBUFF` (517) | DN | `SendClientMessage` |
| `_MSG_Quest.cpp:2560` | `_DN_TOWN_SORRY` (518) | DN | `SendClientMessage` |
| `CWarTower.cpp:60` | `_DN_BASEWORSTART` (425) | DN | `SendNotice` |
| `Server.cpp:2509` | `_DN_ANOTHER_TOWNSPEOPLE` (514) | DN | `SendSay` |
| `Server.cpp:6624` | `_DD_KINGDOMWAR_START` (433) | DD | `SendNotice` |
| `Server.cpp:6643` | `_DD_KINGDOMWAR_END` (434) | DD | `SendNotice` |

**B. Extra arguments silently ignored by `sprintf` (8 sites)**

| File:Line | Index | Prefix | Args supplied |
|---|---|---|---|
| `_MSG_AccountLogin.cpp:47` | `_NN_Version_Not_Match_Rerun` (36) | NN | 1 (`APP_VERSION`) |
| `_MSG_CombineItemTiny.cpp:112` | `_NN_Processing_Complete` (158) | NN | 2 (mob name, item name) |
| `_MSG_MessageWhisper.cpp:1335` | `_NN_Check_User_Info` (435) | NN | 2 (citizen, fame) |
| `_MSG_MessageWhisper.cpp:1346` | `_NN_Check_User_Info` (435) | NN | 2 (citizen, fame) |
| `_MSG_UseItem.cpp:1727` | `_NN_Someone_is_on_quest` (295) | NN | 2 (user, area) |
| `_MSG_UseItem.cpp:1921` | `_NN_Someone_is_on_quest` (295) | NN | 2 (user, area) |
| `_MSG_UseItem.cpp:2026` | `_NN_Someone_is_on_quest` (295) | NN | 2 (user, area) |
| `Server.cpp:8070` | `_NN_Battle_Started` (194) | NN | 2 (two mob names) |

**C. Missing arguments — undefined behaviour (2 sites)**

| File:Line | Index | Prefix | Args supplied / required |
|---|---|---|---|
| `_MSG_GetItem.cpp:131` | `_SSD_S_get_S` (198) | SSD | 2 / 3 |
| `MobKilled.cpp:1779` | `_SSD_S_get_S` (198) | SSD | 2 / 3 |

**D. Numeric literal indices instead of symbolic names (7 accesses, 6 message sites + 1 argument site)**

| File:Line | Literal | Symbolic equivalent | Form |
|---|---|---|---|
| `_MSG_Buy.cpp:183` | `114` | `_NN_Only_Guild_Master_Can_Buy_It` | message argument |
| `_MSG_Buy.cpp:188` | `114` | `_NN_Only_Guild_Master_Can_Buy_It` | message argument |
| `_MSG_GetItem.cpp:194` | `273` | `_NN_Cant_get_more_than_2G` | message argument |
| `_MSG_MessageChat.cpp:91` | `169` | `_DD_Set_Guild_Tax` | format position (2 args) |
| `Server.cpp:8148` | `194` | `_NN_Battle_Started` | message argument |
| `Server.cpp:8158` | `194` | `_NN_Battle_Started` | message argument |
| `_MSG_CombineItem.cpp:136` | `269` | `_NN_CombineFailed` | `%s` argument |

---

## Appendix C — Method and limitations

**Method**

- Exhaustive scan of `Code/**` (`*.cpp`, `*.h`, `*.c`) for `g_pMessageStringTable[...]` accesses, macro-name references and call-site argument counts. Multi-line statements were flattened with a position-to-line map so reported line numbers match the physical file.
- Argument counting distinguishes the *message argument* (format position for `sprintf`; second/first argument for `SendClientMessage`/`SendSay` and `SendNotice`/`SendNoticeArea`) from the entry used as a `%s` argument, so that sites like `sprintf(cMsg, "%s [%s]", table[...], name)` are not miscounted as contract violations.
- Reachability was computed by value, combining symbolic references, `BASE_GetLanguage` index arguments, whisper-keyword comparisons, raw numeric literals and the two arithmetic blocks.
- Git history, project files (`.vcxproj`, `.sln`, `.rc`) and the existing architectural report (`docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md`) were used for build-configuration and role context.

**Limitations and ambiguities**

1. **The resource file is absent.** `Language.txt` is not in the working tree or in git, so the actual text, the actual `%` conversions, duplicate resource indices, encoding (CP949 vs CP1252 vs UTF-8) and message lengths cannot be verified. Every statement about text content is therefore inferred from call sites and naming.
2. **The client is absent.** `Code/ClientPatch_v7662/` contains only `ClientPatch_v7662.vcxproj.user`. Index parity with the client's own string table is an *implied* contract (confidence: high for existence, unverifiable for content).
3. **Prefix ordering is ambiguous.** `_SD_` and `_DS_` both carry "string + number" at their two call sites, in string-first order, contrary to a literal left-to-right reading. `_NS_`, `_ND_` and `_UN_` have no call sites, so their semantics are undocumented.
4. **"Unused" is not the same as "unreachable".** Nine values are reached only arithmetically and two only through numeric literals; a name-based sweep (including the one in this report's raw measurement) understates live text by exactly those eleven.
5. **Two suspected text-level decisions cannot be confirmed**: whether `_NN_Check_User_Info` and `_NN_Someone_is_on_quest` actually contain `%` conversions (which would make their `_NN_` tag wrong), and whether any localized string contains a stray `%` that `sprintf` would consume.
