# Component Deep Analysis Report - TMSrv CReadFiles Config Loader

- **Component name:** TMSrv CReadFiles Config Loader
- **Primary location:** `Code/TMSrv/CReadFiles.cpp` (904 lines, 18,706 bytes) and `Code/TMSrv/CReadFiles.h` (58 lines, 1,718 bytes)
- **Project scope analysed:** repository root `E:\wyd\test-setup\w2pp` (W2PP - decompiled C/C++ Win32 MSVC MMORPG server emulator for W2)
- **Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec` (also excluded: `.git`, `.vs`, `Debug`, `Release` output trees)
- **Out of scope by instruction:** the separate `Code/DBSrv/CReadFiles.cpp` component
- **Analysis type:** read-only static analysis of source, project files, call sites and Git history. No files were modified, nothing was compiled or executed.
- **Analysis date:** 2026-11-15

---

## 1. Executive Summary

### 1.1 Component purpose

`CReadFiles` is the TMSrv (game/world server) **flat-file configuration loader and persistence helper**. It is a single C++ class whose only responsibilities are:

1. Parse nine fixed external file contracts from disk into process-global tuning tables (sanctification rates, quest rewards, combination/refinement rates, admin IP allow-list, MAC block-list).
2. Parse two per-server world-state files (`Chall_<group>_<index>.txt`, `Guild_<group>_<index>.txt`) into the guild-zone/challenge state consumed by the guild-war state machine.
3. Serialize that guild/challenge state back to disk, plus a derived, write-only projection file (`ChampionCity_<group>_<index>.txt`).

Every function is `static`; the class holds no instance state. The single non-static member is the constructor, which is used purely as a one-shot "load everything" composite (called as `CReadFiles::CReadFiles();` at `Code/TMSrv/Server.cpp:3627`). The only per-instance-free state it owns are three `public static int` quest tables (`QuestExp[5][2]`, `QuestCoin[5]`, `QuestLevel[5][4]`) declared at `Code/TMSrv/CReadFiles.h:42-44` and initialised at `Code/TMSrv/CReadFiles.cpp:46-64`.

### 1.2 Architectural role in the system

The component sits at the boundary between **operator-editable text files** and the **TMSrv in-memory world model**. It is a pure producer for ~20 globals owned elsewhere:

- Shared tables owned by `Code/Basedef.cpp` (`g_pSancRate`, `g_pSancGrade`, `g_pEhreRate`, `g_pOdinRate`, `g_pItemSancRate12`, `g_pItemSancRate12Minus`, `g_pTinyBase`, `g_pShanyBase`, `g_pAilynBase`, `g_pAgathaBase`, `g_pAnctChance`).
- TMSrv world-state owned by `Code/TMSrv/Server.cpp` (`g_pGuildZone[MAX_GUILDZONE]`, `GuildCounter`, `pChallangerMoney[6]`, `pAdminIP[MAX_ADMIN]`, `pMac[MAX_MAC]`, `pMobMerc[MAX_MOB_MERC]`).

In the architecture report vocabulary, TMSrv is the game/world server, DBSrv owns accounts/characters, and `Code/Basedef.*` is the shared kernel. This component is a **leaf adapter**: it is called by the composition root (`Server.cpp`), by the GM/admin command interpreter (`imple.cpp`), by the 500 ms timer (`ProcessSecMinTimer.cpp`), and by gameplay handlers that need to persist guild state (`_MSG_Buy.cpp`, `_MSG_MessageChat.cpp`, `_MSG_MessageWhisper.cpp`). It calls almost nothing inward except `Log()` (`Code/TMSrv/Server.cpp:6701`), `ParseMobMercString()` (`Code/TMSrv/Server.cpp:9286`) and `BASE_GetGuildName()` (`Code/Basedef.cpp:1314`).

Its two structurally important properties are: (a) it is **not a service** - it has no queues, no retries, no validation pipeline, and no error recovery; and (b) it is **the only code in the repository** that reads or writes `Chall_*`, `Guild_*` and `ChampionCity_*` files (verified by repository-wide search, see Section 7).

### 1.3 Key findings

1. **`ReadMobMerc()` is dead code with a latent out-of-bounds write.** The method is declared (`CReadFiles.h:33`) and defined (`CReadFiles.cpp:591-638`) but has **zero call sites** in the entire repository; it is not called by the constructor (`CReadFiles.cpp:66-75`) and `git log -S "ReadMobMerc"` shows the symbol has existed unchanged since the initial import commit `489c4e0`. Because the section index starts at `-1` (`:602`) and is only advanced by a `#` header line, any data line appearing before the first `#` results in `ParseMobMercString(-1, ...)` and writes `pMobMerc[-1]`; the cap check `if (Num >= MAX_MOB_MERC) break;` (`:621`) allows `Num` to reach `128` and then memsets `pMobMerc[128]` (valid range is `0..127`). Consequence: the merchant-stock feature (`pMobMerc` consumed at `_MSG_Buy.cpp:243-255`, `ProcessSecMinTimer.cpp:1341-1348`, `Server.cpp:3001-3008`) is permanently inert because `Server.cpp:3494` only zeroes the array and nothing ever populates it.
2. **The `SancRate.txt` key mapping is internally inconsistent and loses one of the three rate rows.** `"PO"` writes row 0 (`:123`), `"PL"` writes row 1 (`:140`), and `"ÂMAGO"` also writes **row 0** (`:157`) - overwriting the Paladin/PO table - while row 2 remains at its compiled default even though `g_pSancRate[2]` is the live row used by `BASE_GetGrowthRate()` (`Code/Basedef.cpp:2234`) and the row commented `// Amagos` at `Basedef.cpp:65`. The Amagos rate row is therefore not operator-configurable at all.
3. **`ÂMAGO` is an encoding-dependent match key.** The literal is stored as raw UTF-8 (`0xC3 0x82 0x4D 0x41 0x47 0x4F`, verified byte-level) inside a project compiled with `<CharacterSet>MultiByte</CharacterSet>` (`Code/TMSrv/TMSrv.vcxproj:27,34`) and no `/utf-8` switch. Commit `030a0ab` ("Update CReadFiles.cpp", 2018-05-22) converted the file's Portuguese strings from CP1252 to UTF-8, so this branch now only matches a file whose key bytes are the two-byte UTF-8 sequence. Confidence: high for the byte layout, medium for the emitted literal (depends on MSVC source-charset handling of a BOM-less UTF-8 file).
4. **Absent/invalid configuration is handled with blocking modal dialogs on the server's main thread.** Three loaders (`ReadSancRate` `:85`, `ReadQuestsRate` `:282`, `ReadCompRate` `:383`) and `ReadMobMerc` (`:597`) call `MessageBoxA(hWndMain, ...)`, and each invalid record inside `ReadSancRate`/`ReadQuestsRate`/`ReadCompRate` raises *another* modal box (`:113,119,130,136,147,153,164,175,186,197,208,219,230,241,252,263`, `:315,321,333,339,350,361`, `:411,441,453,465,477,489,573`). A file with 50 bad lines requires 50 operator clicks before the server finishes booting (`Server.cpp:3627` runs inside `InitInstance`), and the dialog text is UTF-8 bytes rendered through the ANSI API.
5. **Business-rule conflict on city tax between read and write paths.** `ReadGuild()` normalises `CityTax` to `0..20`, substituting `10` outside that range (`CReadFiles.cpp:811-812`), while the operator command that writes it accepts `0..30` (`_MSG_MessageChat.cpp:76`), and the pricing paths accept `<30` (`_MSG_Buy.cpp:109`, `_MSG_Sell.cpp:184`). A guild master who sets a tax of 21-30 gets the value persisted to disk by `WriteGuild()` (`:860`) and then silently rewritten to `10` at the next `ReadGuild()` (startup, `reloadguild`, menu `ReadGuild`, and - indirectly - every `WriteGuild()` cycle).
6. **Write-back is destructive and non-atomic.** `WriteGuild()` and `WriteChallanger()` open the target with `"wt"` (truncate) and stream integers with no temporary file, no flush check, no `ferror()` inspection, and no rollback (`:751`, `:765`, `:835`, `:847-871`, `:878`, `:898`). `ReadGuild()` sets `GuildCounter = 0` at entry (`:774`) *before* the file is opened; if the open fails, the in-memory counter stays 0 and a later `WriteGuild()` (e.g. the `WM_CLOSE` handler at `Server.cpp:4130`) persists `0`, which permanently disables guild creation (guard at `_MSG_MessageWhisper.cpp:204-208`).
7. **Missing-file semantics are inconsistent across the nine contracts.** Three distinct policies co-exist: modal `MessageBoxA` + defaults retained (Sanc/Quests/Comp/MobMerc); silent `return` with no log at all (Admin `:646-647`, Mac `:684-685`); and fallback to a different, CWD-relative file followed by `Log()` on double failure (Chall `:727-735`, Guild `:784-791`). Nothing reports success, nothing validates that required keys were present, and no loader can distinguish "file absent" from "file empty".
8. **The MAC block-list is re-read from disk roughly twice per second for the entire server lifetime.** `ProcessSecTimer()` increments `SecCounter` and then calls `CReadFiles::ReadMacblock()` on `if (SecCounter % 120)` (`ProcessSecMinTimer.cpp:123-128`) - an inverted guard that skips only one tick out of 120. With `TIMER_SEC` set to 500 ms (`Server.cpp:3631`; 200 ms during shutdown, `ProcessSecMinTimer.cpp:115`), that is ~172,000 file opens and ~172,000 full 200-entry rewrites per day, executed synchronously on the message-pump thread, with no change detection.
9. **No automated tests exist for this component or for the repository.** No test sources, no test project, no test framework, no CI configuration (evidence in Section 10).

---

## 2. Data Flow Analysis

### 2.1 Startup composition flow (single entry point)

```
1. WinMain -> InitInstance (Code/TMSrv/Server.cpp:3414)
2. ReadConfig / ReadLevelItemConfig ; ConfigReady = 1        (Server.cpp:3483-3486)
3. memset(pMac, 0, ...) ; memset(pMobMerc, 0, ...)            (Server.cpp:3493-3494)
4. Reboot() -> BASE_InitializeBaseDef (compiled defaults)     (Server.cpp:3505 -> Basedef.cpp:61-102)
5. CReadFiles::CReadFiles()                                   (Server.cpp:3627)
   5.1 ReadSancRate()   <- ../../Common/Settings/SancRate.txt
   5.2 ReadQuestsRate() <- ../../Common/Settings/QuestsRate.txt
   5.3 ReadCompRate()   <- ../../Common/Settings/CompRate.txt
   5.4 ReadAdmin()      <- ../../DBSRV/Run/Admin.txt
   5.5 ReadMacblock()   <- ../../DBSRV/Run/Mac.txt
   5.6 ReadChallanger() <- ../../Common/Chall_<g>_<i>.txt  (fallback ./Chall.txt)
   5.7 ReadGuild()      <- ../../Common/Guild_<g>_<i>.txt  (fallback ./Guild.txt)
6. CCastleZakum::ReadCastleQuest()                            (Server.cpp:3629)
7. SetTimer(TIMER_SEC, 500 ms) ; SetTimer(TIMER_MIN, 12,000 ms) (Server.cpp:3631-3632)
8. ListenSocket.StartListen(...) ; GuildZoneReport()          (Server.cpp:3634-3636)
```

Load order is significant: `ReadSancRate`/`ReadQuestsRate`/`ReadCompRate` overwrite the compiled defaults established in step 4, and `ReadGuild` overwrites `g_pGuildZone` **after** `GuildZoneReport()` has not yet run - the report is emitted only once at `:3636`, so a successful load is reflected in the first DB report, while a failed load (`GuildCounter == 0`) is reported by `Log()` at `CReadFiles.cpp:823-824` only.

### 2.2 Per-file load flows

**SancRate.txt** (`CReadFiles.cpp:77-272`)

```
1. fopen("../../Common/Settings/SancRate.txt", "r")        :81
2. on NULL -> MessageBoxA + return (defaults retained)     :83-87
3. loop: fgets(temp, 1023, fp)                             :89-94
4. sscanf(temp, "%s %s %s") -> str1(key) str2(idx) str3(val) :101
5. atoi each token; _strupr(str1)                          :103-107
6. dispatch on str1: PO | PL | ÂMAGO | PO_A..PO_E | PL_A..PL_E :109-268
7. range-check idx (0..11) and val (0..100) -> MessageBoxA + continue on violation
8. write g_pSancRate[row][idx] = val or g_pSancGrade[row][n] = idx
9. EOF -> fclose                                           :271
```

**QuestsRate.txt** (`CReadFiles.cpp:274-373`)

```
1. fopen(..., "r")                                          :278 ; NULL -> MessageBoxA :280-284
2. loop fgets(temp, 1023)                                   :286-294
3. sscanf(temp, "%s %s %s %s %s %s") -> 6 tokens            :300
4. atoi x6 ; _strupr(str1)                                  :302-309
5. "EXP"   -> QuestExp[val2][0]=val3 ; QuestExp[val2][1]=val4      :311-327
6. "COIN"  -> QuestCoin[val2]=val3                                  :329-344
7. "LEVEL" -> QuestLevel[val2][0..3] = val3..val6                   :346-369
```

**CompRate.txt** (`CReadFiles.cpp:375-589`)

```
1. fopen(..., "r")                                          :379 ; NULL -> MessageBoxA :381-385
2. loop fgets(temp, 1023)                                   :387-393
3. sscanf(temp, "%s %s %s") -> str1(key) str2(subkey) str3(val) :399
4. atoi x3 ; _strupr(str1) only                             :401-405
5. dispatch on str1: EHRE | TINY | SHANY | AILYN | AGATHA | ODIN | COMPOSITOR
6. within each: range-check val (0..100) -> MessageBoxA + continue
7. second-level exact strcmp on str2 (case-sensitive) -> target global slot
```

**MobMerc.txt** (`CReadFiles.cpp:591-638`, no callers)

```
1. fopen("../../Common/Settings/MobMerc.txt", "rt")         :593 ; NULL -> MessageBoxA :595-600
2. Num = -1 ; tp[1024] zeroed                               :602-606
3. loop fgets(tp, 1024)                                     :608-613
4. tp[0] == 47 ('/')  -> comment, skip                      :616-617
   tp[0] == '#'       -> new merchant: bound-check, Num++, memset(pMobMerc[Num]) :619-627
   tp[0] != 13 (CR)   -> ParseMobMercString(Num, tp)        :629-635
5. fclose                                                   :637
6. ParseMobMercString (Server.cpp:9286) fills MobName / GenerateIndex /
   RenewTime*2 / RebornTime*2 / Carry[ITEM_n] / MaxStock / Stock
```

**Admin.txt** (`CReadFiles.cpp:640-676`)

```
1. fopen("../../DBSRV/Run/Admin.txt", "r")                  :644 ; NULL -> silent return :646-647
2. loop fgets(ttt, 127)                                     :649-657
3. replace '.' with ' ' over the first 255 bytes            :661-663
4. sscanf(ttt, "%d %d %d %d %d") -> idx a b c d             :665
5. ip = (d<<24)|(c<<16)|(b<<8)|a  (dotted-quad, little-endian in memory) :667
6. idx outside 0..MAX_ADMIN-1 -> continue                   :669-670
7. pAdminIP[idx] = ip                                       :672
```

**Mac.txt** (`CReadFiles.cpp:678-715`) - identical pipeline, writes `pMac[idx].Mac[0..3] = a,b,c,d` (`:708-711`), bound `0..MAX_MAC-1` (`:705`).

**Chall_<g>_<i>.txt** (`CReadFiles.cpp:717-741`)

```
1. sprintf(dir, "../../Common/Chall_%2.2d_%2.2d.txt", ServerGroup, ServerIndex) :721
2. fopen(dir, "rt") ; NULL -> fopen("Chall.txt", "rt")      :725-728
3. both NULL -> Log("err,reading chall.txt - can't open.txt","-system",0) + return :730-735
4. for i in 0..ValidGuild-1: fscanf(fp, "%d", &pChallangerMoney[i])  :737-738
5. fclose                                                   :740
```

**Guild_<g>_<i>.txt** (`CReadFiles.cpp:772-825`)

```
1. GuildCounter = 0                                         :774
2. sprintf(dir, "../../Common/Guild_%2.2d_%2.2d.txt", ServerGroup, ServerIndex) :778
3. fopen(dir, "rb") ; NULL -> fopen("Guild.txt", "rb")      :782-785
4. both NULL -> Log(...) + return (GuildCounter stays 0)    :787-791
5. zero ChargeGuild/ChallangeGuild for 5 zones              :793-797
6. fscanf GuildCounter                                     :799
7. fscanf ChargeGuild[0..4]                                :801-802
8. fscanf ChallangeGuild[0..4]                             :804-805
9. fscanf CityTax[0..4] + clamp (0..20 else 10)             :807-813
10. fscanf Clan[0..4]                                      :815-816
11. fscanf Victory[0..4]                                   :818-819
12. fclose ; if GuildCounter == 0 -> Log(...)               :821-824
```

### 2.3 Write-back flows

**Challenge money** (`WriteChallanger`, `:743-770`)

```
1. sprintf(dir, CHALL_PATH, ServerGroup, ServerIndex)        :747
2. fopen(dir, "wt") ; NULL -> fopen("Chall.txt","wt")        :751-754
3. both NULL -> Log("err,writing chall.txt - can't open for write") + return :756-761
4. for i in 0..ValidGuild-1: fprintf("%d ", pChallangerMoney[i]) ; fprintf("\n") :763-767
5. fclose                                                    :769
```

**Guild state + champion-city projection** (`WriteGuild`, `:827-904`)

```
1. sprintf(dir, GUILD_PATH, ...) ; fopen(dir,"wt") ; fallback "Guild.txt" ; Log on double failure :831-845
2. fprintf("%d", GuildCounter) ; " %d" x5 ChargeGuild ; "\n"  :847-852
3. " %d" x5 ChallangeGuild ; "\n"                              :854-857
4. " %d" x5 CityTax ; "\n"                                     :859-862
5. " %d" x5 Clan ; "\n"                                        :864-867
6. " %d" x5 Victory  (no trailing newline)                     :869-871
7. fclose                                                      :872
8. memset(dir) ; sprintf(GUILDCHAMP_PATH, ...) ; fopen(dir,"wt") :875-878
9. NULL -> Log("err,Writting Guild error - can't open GuildChamp.txt") + return (Guild_ file already written) :880-885
10. for i in 0..4: BASE_GetGuildName(ServerGroup, ChargeGuild[i], GuildName) :894
                   sprintf(Guildmark, "b01%06d.bmp", ChargeGuild[i])         :896
                   fprintf("%s %s %s\n", CityName[i], GuildName, Guildmark)  :898-900
11. fclose                                                                   :903
```

### 2.4 Reload and refresh flows

| Trigger | Location | Functions invoked |
|---------|----------|-------------------|
| Server boot | `Server.cpp:3627` | constructor: all 7 loaders |
| GM command `+reloadfile` | `imple.cpp:250-262` | `ReadSancRate`, `ReadQuestsRate`, `ReadCompRate` (after `BASE_InitializeBaseDef`, i.e. defaults re-applied first) |
| GM command `+reloadguild` | `imple.cpp:844-848` | `ReadGuild` |
| GM command `+npko` | `imple.cpp:1666-1703` | `ReadAdmin`, then rewrites `../../DBSRV/Run/Admin.txt` and replaces one IP slot |
| Window menu "Sistema > ReadGuild" | `Server.cpp:4073`, `:4101-4103` | `ReadGuild` |
| Every `ProcessSecTimer` tick except multiples of 120 | `ProcessSecMinTimer.cpp:123-128` | `ReadMacblock` |
| Challenge wins (higher fame) | `Server.cpp:2549-2567` | `WriteChallanger` + `WriteGuild` |
| Guild war season end ("decide winner") | `Server.cpp:6557-6564` | `WriteChallanger` |
| New guild created via badge purchase | `_MSG_Buy.cpp:224-231` | `WriteGuild` |
| New guild created via whisper command | `_MSG_MessageWhisper.cpp:261-263` | `WriteGuild` |
| Guild tax change | `_MSG_MessageChat.cpp:88-94` | `WriteGuild` |
| Champion/challenger set by GM | `imple.cpp:1049-1079` | `WriteGuild` |
| Server shutdown (`WM_CLOSE`) | `Server.cpp:4130` | `WriteGuild` |

### 2.5 In-memory state ownership map

| Data written by CReadFiles | Declared/owned at | Declared type/size | Consumers (readers) |
|----------------------------|-------------------|--------------------|---------------------|
| `g_pSancRate[3][12]` | `Basedef.cpp:61` | `int[3][12]` | `Basedef.cpp:2220` (`[OriLacto][sanc+1]`), `:2234` (`[2][sanc]`) |
| `g_pSancGrade[2][5]` | `Basedef.cpp:68` | `int[2][5]` | `_MSG_UseItem.cpp:862` |
| `g_pEhreRate[10]` | `Basedef.cpp:75` | `int[10]` | `_MSG_CombineItemEhre.cpp:113` |
| `g_pOdinRate[12]` | `Basedef.cpp:80` | `int[12]` | `_MSG_CombineItemOdin.cpp:124,357,389,416,448,480,512` |
| `g_pItemSancRate12[11]` / `...Minus[4]` | `Basedef.cpp:97,102` | `int[11]` / `int[4]` | `_MSG_CombineItemOdin.cpp:170-222` |
| `g_pTinyBase` / `g_pShanyBase` / `g_pAilynBase` / `g_pAgathaBase` | `Basedef.cpp:90-93` | `int` | `GetFunc.cpp:214`, `_MSG_CombineItemShany.cpp:89`, `GetFunc.cpp:314-370`, `:424` |
| `g_pAnctChance[3]` | `Basedef.cpp:95` | `int[3]` | `GetFunc.cpp:83,89,95` |
| `QuestExp[5][2]`, `QuestCoin[5]`, `QuestLevel[5][4]` | `CReadFiles.cpp:46-64` | `int[5][2]`, `int[5]`, `int[5][4]` | `_MSG_UseItem.cpp:2245-2246,2260-2263,2277` |
| `pAdminIP[10]` | `Server.cpp:264` | `unsigned int[10]` | **no consumer in TMSrv** (only echoed/rewritten by `imple.cpp:1687-1698`); enforcement lives in `DBSrv/Server.cpp:985-988` |
| `pMac[200]` | `Server.cpp:271` | `STRUCT_BLOCKMAC[200]` | `ProcessDBMessage.cpp:369-383` (login refusal) |
| `pMobMerc[128]` | `Server.cpp:449` | `STRUCT_MERC[128]` | `_MSG_Buy.cpp:243-255`, `ProcessSecMinTimer.cpp:1341-1348`, `Server.cpp:3001-3008` (never populated - see 1.3.1) |
| `GuildCounter` | `Server.cpp:53` (init 1) | `int` | `_MSG_MessageWhisper.cpp:204-210,256`, `_MSG_Buy.cpp:224-231` |
| `g_pGuildZone[5]` | `Server.cpp:490` (`extern`), `Basedef.h:417` | `STRUCT_GUILDZONE[5]` | `Server.cpp:2549-2567,6535-6600`, `_MSG_Buy.cpp:107`, `_MSG_Sell.cpp:182`, `SendFunc.cpp:1408`, `_MSG_MessageChat.cpp:71-94`, `_MSG_SendAutoTrade.cpp:98` |
| `pChallangerMoney[6]` | `Server.cpp:357` | `int[6]` (5 used) | `Server.cpp:2560-2563` |

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Parsing contract | `SancRate.txt` record grammar: `<KEY> <index> <value>`, whitespace separated, key uppercased, one record per line; unknown keys silently ignored | `Code/TMSrv/CReadFiles.cpp:89-269` |
| BR-02 | Validation | SancRate indexed keys (`PO`, `PL`, `ÂMAGO`) require `index` in `0..11` and `value` in `0..100`; grade keys require `index` in `0..9`; violation skips the record with a modal dialog | `Code/TMSrv/CReadFiles.cpp:111-121,128-138,145-155,162-168,173-179,184-190,195-201,206-212,217-223,228-234,239-245,250-256,261-267` |
| BR-03 | Data mapping | Grade keys `PO_A..PO_E` / `PL_A..PL_E` map their `index` token into `g_pSancGrade[0][0..4]` / `[1][0..4]` (the second token is a *value*, not an index) | `Code/TMSrv/CReadFiles.cpp:160-268` |
| BR-04 | Data mapping (defect) | `"PO"` and `"ÂMAGO"` both write `g_pSancRate[0][*]`; row `[2]` (Amagos, used by `BASE_GetGrowthRate`) is not reachable from the file | `Code/TMSrv/CReadFiles.cpp:123,140,157` vs `Code/Basedef.cpp:2234` |
| BR-05 | Parsing contract | `QuestsRate.txt` record grammar: `EXP <tier 0..4> <mortalExp> <archExp>` | `Code/TMSrv/CReadFiles.cpp:311-327` |
| BR-06 | Parsing contract | `QuestsRate.txt` record grammar: `COIN <tier 0..4> <coin>` | `Code/TMSrv/CReadFiles.cpp:329-344` |
| BR-07 | Parsing contract | `QuestsRate.txt` record grammar: `LEVEL <tier 0..4> <minMortal> <maxMortal> <minArch> <maxArch>` | `Code/TMSrv/CReadFiles.cpp:346-369` |
| BR-08 | Validation | Quest tier index must be `0..4`; quest exp/coin must be `0..2,000,000,000`; quest level bounds must be `0..399` | `Code/TMSrv/CReadFiles.cpp:313-323,331-341,348-363` |
| BR-09 | Business logic | Quest rewards are class-dependent: mortal characters consume slot `[0]`/`[0..1]`, arch characters slot `[1]`/`[2..3]`; coin is class-independent | `Code/TMSrv/_MSG_UseItem.cpp:2245-2246,2260-2263` |
| BR-10 | Parsing contract | `CompRate.txt` record grammar: `<GROUP> <SUBKEY> <value 0..100>`, sub-key compared case-sensitively | `Code/TMSrv/CReadFiles.cpp:387-586` |
| BR-11 | Data mapping | `EHRE` group sub-keys map to 7 distinct slots of `g_pEhreRate[10]`; slots 0, 5, 9 are not configurable | `Code/TMSrv/CReadFiles.cpp:407-435` |
| BR-12 | Data mapping | `TINY`/`SHANY`/`AILYN`/`AGATHA` + `CHANCEBASE` map to four scalar base chances | `Code/TMSrv/CReadFiles.cpp:437-483` |
| BR-13 | Data mapping | `ODIN` group sub-keys map to `g_pOdinRate[1,3..11]` and `g_pItemSancRate12[0..10]` and `g_pItemSancRate12Minus[0..3]`; slots `g_pOdinRate[0]` and `[2]` are not configurable | `Code/TMSrv/CReadFiles.cpp:485-567` |
| BR-14 | Data mapping | `COMPOSITOR` sub-keys `ITEM_+7/+8/+9` map to `g_pAnctChance[0..2]` (anvil upgrade bonus) | `Code/TMSrv/CReadFiles.cpp:569-585` |
| BR-15 | Validation | Every `CompRate` value must be `0..100`; violation skips the record with a modal dialog; unknown group/sub-key is silently ignored (no validation, no write) | `Code/TMSrv/CReadFiles.cpp:409-413,439-443,451-455,463-467,475-479,487-491,571-575` |
| BR-16 | Absence handling | Missing `SancRate`/`QuestsRate`/`CompRate`/`MobMerc` -> modal `MessageBoxA` + `return`, compiled defaults remain in force | `Code/TMSrv/CReadFiles.cpp:83-87,280-284,381-385,595-600` |
| BR-17 | Absence handling | Missing `Admin.txt`/`Mac.txt` -> silent `return` (no message, no log); stale in-memory values remain | `Code/TMSrv/CReadFiles.cpp:646-647,684-685` |
| BR-18 | Parsing contract | `Admin.txt`/`Mac.txt` record grammar: `<entryIndex> <a>.<b>.<c>.<d>` ; dots are converted to spaces then five integers are scanned | `Code/TMSrv/CReadFiles.cpp:661-665,699-703` |
| BR-19 | Validation | Admin entry index must be `0..MAX_ADMIN-1` (10) and MAC entry index `0..MAX_MAC-1` (200); out-of-range records are skipped silently; IP octets are **not** range-checked | `Code/TMSrv/CReadFiles.cpp:669-670,705-706` |
| BR-20 | Transformation | Admin dotted-quad is packed as `ip = (d<<24)+(c<<16)+(b<<8)+a`; MAC is stored as four separate `int` octets | `Code/TMSrv/CReadFiles.cpp:667,708-711` |
| BR-21 | Business logic | A MAC entry blocks a login when all four octets match a non-zero entry; all-zero entries are ignored | `Code/TMSrv/ProcessDBMessage.cpp:369-383` |
| BR-22 | Business logic | MAC block-list is refreshed from disk on ~every timer tick (119 of 120) so operator edits take effect without restart | `Code/TMSrv/ProcessSecMinTimer.cpp:123-128` |
| BR-23 | Parsing contract | `MobMerc.txt` is section-based: `#` starts a new merchant record, `/` is a comment, blank lines are ignored, keys are `MOBNAME:`, `GENERATEINDEX:`, `RENEWTIME:`, `REBORNTIME:`, `ITEM_0:`..`ITEM_63:` | `Code/TMSrv/CReadFiles.cpp:608-636`, `Code/TMSrv/Server.cpp:9286-9376` |
| BR-24 | Validation | Merchant count capped at `MAX_MOB_MERC` (128); times are multiplied by 2 on load | `Code/TMSrv/CReadFiles.cpp:621-622`, `Code/TMSrv/Server.cpp:9347,9350` |
| BR-25 | Naming contract | Per-server state files are named with two zero-padded 2-digit fields: `Chall_%2.2d_%2.2d.txt`, `Guild_%2.2d_%2.2d.txt`, `ChampionCity_%2.2d_%2.2d.txt` filled from `ServerGroup`, `ServerIndex` | `Code/TMSrv/CReadFiles.cpp:41-43,721,747,778,831,876` |
| BR-26 | Fallback | If the per-server file cannot be opened, a CWD-relative generic file `Chall.txt` / `Guild.txt` is used instead | `Code/TMSrv/CReadFiles.cpp:727-728,753-754,784-785,837-838` |
| BR-27 | Data contract | `Chall_*.txt` holds exactly `ValidGuild` (5) integers, one per guild zone, in zone order; no range validation | `Code/TMSrv/CReadFiles.cpp:737-738`, `Code/TMSrv/Server.cpp:55` |
| BR-28 | Data contract | `Guild_*.txt` holds 26 integers in 5 groups: counter, 5 charge guilds, 5 challenger guilds, 5 city taxes, 5 clans, 5 victories | `Code/TMSrv/CReadFiles.cpp:799-819,847-871` |
| BR-29 | Validation | `CityTax` outside `0..20` is replaced by `10`; no other guild-zone field is validated | `Code/TMSrv/CReadFiles.cpp:809-813` |
| BR-30 | Business logic | A loaded `GuildCounter` of `0` is treated as an error condition (logged) and disables guild creation; ids are `ServerIndex*4096 + GuildCounter` | `Code/TMSrv/CReadFiles.cpp:823-824`, `_MSG_MessageWhisper.cpp:204-210` |
| BR-31 | Serialization | `WriteChallanger` emits `<value> \n` per zone (trailing space, 5 lines, truncating) | `Code/TMSrv/CReadFiles.cpp:763-767` |
| BR-32 | Serialization | `WriteGuild` emits `Guild_*.txt` as 5 space-prefixed lines then rewrites `ChampionCity_*.txt` as `<CityName> <GuildName> b01%06d.bmp` per zone | `Code/TMSrv/CReadFiles.cpp:847-901` |
| BR-33 | Business logic | Challenge money is raised only when the challenger's fame debt exceeds the stored per-zone record; the record is then persisted | `Code/TMSrv/Server.cpp:2549-2567` |
| BR-34 | Business logic | All guild-zone mutations (`ChargeGuild`, `ChallangeGuild`, `CityTax`) trigger an immediate full rewrite of the guild file | `imple.cpp:1049-1079`, `_MSG_MessageChat.cpp:88-94`, `_MSG_Buy.cpp:224-231`, `_MSG_MessageWhisper.cpp:261-263` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 - SancRate record grammar and key dispatch

**Overview**:
`SancRate.txt` is a line-oriented, whitespace-tokenised key/value file that overrides the compiled sanctification success tables. Parsing is positional and key-driven: token 1 selects the target table row, token 2 the column, token 3 the percentage.

**Detailed description**:
The loader opens the path constant `SANC_RATE_PATH` (`:35`) in text mode (`:81`) and reads the file line by line into the process-global buffer `temp` (`Code/TMSrv/Server.cpp:277`, 4096 bytes) with a **1023-byte** `fgets` limit (`:91`), so any physical line longer than 1022 bytes is delivered as two successive pseudo-records. Each record is re-tokenised with `sscanf(temp, "%s %s %s", str1, str2, str3)` (`:101`) into three 128-byte stack buffers. Tokens are converted with `atoi` (`:103-105`) and only the **key** token is uppercased with `_strupr` (`:107`); the remaining two tokens are compared numerically or, for grade keys, discarded.

The grammar is completely permissive with respect to ordering, repetition and unknown content. Keys may appear in any order and any number of times; because each assignment is an unconditional store (e.g. `g_pSancRate[0][val2] = val3` at `:123`), the **last occurrence wins**. Unknown keys, comment-looking lines beginning with `#`, and any line whose first token is not one of the eleven recognised keys are ignored without diagnostics. There is no count check, no required-key check and no duplicate detection, so a file that defines only `PO` produces a configuration where row 1 keeps its compiled defaults and row 0 is partly overridden - a state the server cannot distinguish from a complete load.

Two structural properties follow. First, the parser does not tolerate intra-line comments or separators: `PO 3 85 ; fire bonus` is accepted, but `PO=3,85` becomes token `PO=3,85` and is ignored. Second, because blank lines produce zero successful `%s` conversions, `str1`, `str2` and `str3` keep their **uninitialised** stack contents (`:96-98` declare them without `memset`), and `atoi`/`_strupr` then operate on indeterminate memory. A blank line in the middle of the file is therefore undefined behaviour in the strict sense, not merely "ignored" - `_strupr` will scan until it happens to find a NUL byte within or beyond the 128-byte object. The same idiom is repeated in BR-05 and BR-10.

**Rule workflow**:

```
fgets(line) -> EOF ? stop
sscanf(line,"%s %s %s") -> (str1, str2, str3) [+ atoi -> val2, val3] [+ _strupr(str1)]
str1 == "PO"      -> validate(val2 in 0..11, val3 in 0..100) -> g_pSancRate[0][val2] = val3
str1 == "PL"      -> validate(same)                          -> g_pSancRate[1][val2] = val3
str1 == "ÂMAGO"   -> validate(same)                          -> g_pSancRate[0][val2] = val3   [see BR-04]
str1 == "PO_A".."PO_E" -> validate(val2 in 0..9)             -> g_pSancGrade[0][0..4] = val2
str1 == "PL_A".."PL_E" -> validate(val2 in 0..9)             -> g_pSancGrade[1][0..4] = val2
otherwise -> no-op (no diagnostic)
```

---

### Business Rule: BR-02 - SancRate index and value bounds

**Overview**:
Every recognised SancRate key is guarded by a two-stage bound check: the column index must address an existing slot and the percentage must be a valid probability-like value.

**Detailed description**:
For the three table keys (`PO`, `PL`, `ÂMAGO`) the index token must satisfy `0 <= val2 < 12` (`:111`, `:128`, `:145`), matching the second dimension of `g_pSancRate[3][12]` (`Code/Basedef.h:2482`). The value token must satisfy `0 <= val3 <= 100` (`:117`, `:134`, `:151`). Note the asymmetry: the index uses an exclusive upper bound (`>= 12`) while the value uses an inclusive one (`> 100`), so 12 is rejected as an index and 100 is accepted as a value. A violating record is **skipped**, never clamped, and the operator is informed through `MessageBoxA(hWndMain, "Índice inválido. (1)", SANC_RATE_PATH, MB_OK)` for index faults and `"(2)"` for value faults.

The consequence of "skip, never clamp" is that a typo silently degrades the configuration instead of rejecting it: if the operator writes `PO 12 90` intending to extend the table, nothing is stored, the previously loaded (or compiled) value for that column survives, and the server continues with a mixture of file and default values. Because the modal box is produced **per bad record**, a systematically malformed file produces a nested sequence of blocking dialogs during `InitInstance` (`Server.cpp:3627`), each of which blocks the message pump - timers already armed are not processed, and no client socket has been created yet, so the process simply waits for a human.

The grade keys (`PO_A..PO_E`, `PL_A..PL_E`) use a different bound: `0 <= val2 <= 9` (`:162`, `:173`, `:184`, `:195`, `:206`, `:217`, `:228`, `:239`, `:250`, `:261`). This is an inclusive bound because the token is a *grade increment* added to sanctification counters rather than an array index; consumers use it as an additive bonus (`_MSG_UseItem.cpp:862`: `sanc += g_pSancGrade[OriLacto][ItemGrade]`). The same dialog text `"Índice inválido. (1)"` is used for grade faults, so an operator cannot tell from the message which of the two constraints failed.

**Rule workflow**:

```
if key in {PO, PL, ÂMAGO}:
    if idx < 0 or idx >= 12 -> MessageBox("Índice inválido. (1)") ; continue
    if val < 0 or val > 100 -> MessageBox("Índice inválido. (2)") ; continue
    store
if key in {PO_A..PL_E}:
    if idx < 0 or idx > 9   -> MessageBox("Índice inválido. (1)") ; continue
    store grade increment
```

---

### Business Rule: BR-03 - Sanctification grade bonus tables

**Overview**:
The ten `*_A..*_E` keys populate a 2x5 grade-bonus matrix used when sanctifying items, one row per lacto/orientation and one column per item grade.

**Detailed description**:
`g_pSancGrade[2][5]` (`Code/Basedef.cpp:68-72`, default all `1`) is indexed by the ore/lacto orientation (`OriLacto`, values 0 and 1 reaching this table) and by the item's `ItemGrade` (0..4). The file contract distinguishes the orientation with a one-letter prefix (`PO_` = 0, `PL_` = 1) and the grade with a one-letter suffix (`_A`..`_E` = columns 0..4). Crucially, the **second token of these lines is not an index**: it is the bonus value itself, which is why its legal range is `0..9` and not `0..11`. The file format is therefore not uniform across the same file, which is a documentation hazard: `PO 3 85` means "row 0, column 3, value 85" while `PO_C 4 0` means "row 0, column 2, value 4".

The matrix is consumed only as an additive bonus to the resulting sanctification counter. In `_MSG_UseItem.cpp:862` the value is added to the item's sanctification level after a successful combination, so a value of 0 disables the bonus and values greater than 1 accelerate progression. Because the matrix is 2x5 and only rows 0 and 1 are addressable, the third orientation (lacto 2, Amagos) never receives a grade bonus regardless of file content - consistent with `BASE_GetSuccessRate` returning a flat 100 for `OriLacto == 2` (`Code/Basedef.cpp:2224`).

The defaults are all `1`, which means an absent or unparsable file still produces a working, if flat, grade system. There is no cross-validation between the two rows and no consistency check against `g_pSancRate`, so an operator can configure a 100% base success rate alongside a 9-point grade bonus and produce success probabilities well above 100%, which downstream code does not clamp.

**Rule workflow**:

```
key == "PO_X" (X in A..E) -> g_pSancGrade[0][ idx_of(X) ] = value   (value 0..9)
key == "PL_X" (X in A..E) -> g_pSancGrade[1][ idx_of(X) ] = value   (value 0..9)
consumers: sanc += g_pSancGrade[OriLacto][ItemGrade]   (_MSG_UseItem.cpp:862)
```

---

### Business Rule: BR-04 - Duplicate SancRate row mapping (Amagos table unreachable)

**Overview**:
`"ÂMAGO"` records write the same row as `"PO"` records, so the row documented as the Amagos table (`g_pSancRate[2]`) cannot be modified from disk.

**Detailed description**:
`Basedef.cpp:61-66` declares three rows with explicit trailing comments: index 0 `// PO`, index 1 `// PL`, index 2 `// Amagos`. The loader maps `"PO"` to row 0 (`:123`) and `"PL"` to row 1 (`:140`), which is consistent. The third branch, keyed `"ÂMAGO"`, also assigns row 0 (`:157`) - it copies the PO branch verbatim except for the key comparison. Two consumers exist: `BASE_GetSuccessRate` indexes `g_pSancRate[OriLacto][sanc + 1]` with `OriLacto` restricted to 0..2 (`Basedef.cpp:2207-2220`), and `BASE_GetGrowthRate` indexes `g_pSancRate[2][sanc]` directly (`Basedef.cpp:2234`).

The practical effect is twofold. First, the Amagos growth-rate curve is frozen at the compiled values `{100,80,60,40,20,10,10,10,5,5,5,5}` and cannot be tuned. Second, and more dangerously, an operator who follows the file's own naming convention and writes `ÂMAGO` records to tune the third row will instead **silently overwrite the PO row**, changing Paladin sanctification probabilities. No dialog, log entry or comment signals this.

Confidence in the defect: high. The evidence is the literal assignment target `g_pSancRate[0][val2]` at `:157` inside the `ÂMAGO` branch, contrasted with the row-2 header comment and the two consumer index expressions. Whether this is a decompilation artefact or an intentional simplification cannot be determined from the repository - the initial import commit `489c4e0` already contains it.

**Rule workflow**:

```
file line "PO 3 85"     -> g_pSancRate[0][3] = 85     (expected)
file line "ÂMAGO 3 85"  -> g_pSancRate[0][3] = 85     (should be row [2])
g_pSancRate[2][*]       -> never written by CReadFiles; stays Basedef.cpp default
```

---

### Business Rule: BR-05 - Quest experience record

**Overview**:
`EXP <tier> <mortalExp> <archExp>` defines the experience reward of each of the five quest difficulty tiers, separately for mortal and arch characters.

**Detailed description**:
`ReadQuestsRate` reads six whitespace tokens per line (`:300`) but the `EXP` branch consumes only four (`:311-327`). The first token must equal `"EXP"` after uppercasing; the second is the tier (0..4) addressing the first dimension of `QuestExp[5][2]`; the third and fourth are the mortal and arch experience amounts. The store is `QuestExp[val2][0] = val3; QuestExp[val2][1] = val4;` (`:325-326`). Defaults are `{1000,500} .. {5000,2500}` (`:48-55`), i.e. arch characters receive half of the mortal reward by default.

The runtime consumer reinforces the column meaning: `_MSG_UseItem.cpp:2260` selects `QuestExp[thisQuest][0]` when `ClassMaster == MORTAL` and `[1]` otherwise, then adds it to `MOB.Exp` and to the daily log, while party members receive `QuestExp / 10` (`:2261,2305,2345`). A malformed or absent `EXP` line therefore leaves either the compiled default or a previously loaded value - the parser never resets the tier, so a `+reloadfile` after an operator edit that deletes a line will retain the old number rather than restoring the default.

Because six tokens are scanned but only four validated, the `LEVEL` branch's extra tokens are read from the same line in a different shape. This overloading means a single file carries three different record schemas distinguishable only by the first token; any line whose first token is a typo (e.g. `EXPP`) is ignored entirely, and the remaining tokens are discarded without a warning. The 128-byte stack buffers for tokens 5 and 6 are uninitialised whenever a shorter record is scanned, so the `LEVEL` path on a four-token line would consume indeterminate values - a defect only avoided because `LEVEL` records in practice carry six tokens.

**Rule workflow**:

```
key == "EXP":
    tier = val2 ; require 0 <= tier < 5
    require 0 <= mortalExp < 2,000,000,000 and 0 <= archExp < 2,000,000,000
    QuestExp[tier][0] = mortalExp ; QuestExp[tier][1] = archExp
consumers: MORTAL -> [0] ; otherwise -> [1] ; party share = value / 10
```

---

### Business Rule: BR-06 - Quest coin record

**Overview**:
`COIN <tier> <amount>` defines the gold reward of each quest tier, shared by both character classes.

**Detailed description**:
The `COIN` branch (`:329-344`) validates the tier with the same `0 <= val2 < 5` constraint and validates the amount with `0 <= val3 <= 2000000000` (`:337`). Note the bound operator differs from the `EXP` branch, which uses `>= 2000000000` (`:319`): a coin value of exactly 2,000,000,000 is accepted while an experience value of exactly 2,000,000,000 is rejected. Both are far inside `INT_MAX`, so the distinction matters only as evidence of ad-hoc validation rather than a designed domain limit.

The award is unconditional and non-clamped at the consumer: `_MSG_UseItem.cpp:2263` performs `pMob[conn].MOB.Coin += CReadFiles::QuestCoin[thisQuest];` with no overflow guard. Since coin is an `int` and the file allows values up to two billion, an operator-supplied large value can overflow a character's coin balance into negative territory in a single quest completion. The default table `{2000,4000,6000,8000,10000}` (`:46`) is monotonic by tier, but the parser imposes no monotonicity on overrides - a file may define tier 0 above tier 4.

Because the value is class-independent there is only one slot per tier, unlike `EXP` and `LEVEL`. This asymmetry is not documented in code or comments; it is only visible by comparing the consumer expressions at `_MSG_UseItem.cpp:2260-2263`.

**Rule workflow**:

```
key == "COIN":
    tier = val2 ; require 0 <= tier < 5
    require 0 <= amount <= 2,000,000,000
    QuestCoin[tier] = amount
consumer: MOB.Coin += QuestCoin[thisQuest]   (no overflow guard)
```

---

### Business Rule: BR-07 - Quest level window record

**Overview**:
`LEVEL <tier> <minMortal> <maxMortal> <minArch> <maxArch>` defines the character-level window in which each quest tier is offered, per class.

**Detailed description**:
The `LEVEL` branch is the only record that consumes all six scanned tokens (`:346-369`). The tier is validated `0..4`; the four level bounds are validated with a single compound condition requiring each of them to satisfy `0 <= x < 400` (`:359`). The bound 400 is the first value outside `MAX_LEVEL` (399, `Basedef.h:117`), so the check is a genuine domain bound rather than an arbitrary number. All four bounds are validated together: a single invalid bound rejects the entire record, leaving all four slots untouched.

Column mapping is `[0]=minMortal, [1]=maxMortal, [2]=minArch, [3]=maxArch` (`:365-368`), and the consumer selects by class exactly as for `EXP` (`_MSG_UseItem.cpp:2245-2246`). Defaults are `{39,115}`, `{115,190}`, `{190,265}`, `{265,320}`, `{320,350}` for both classes (`:57-64`), which partitions the level range 39..350 contiguously. The parser does not enforce the invariants that make that partition work: `min <= max`, contiguity between adjacent tiers, and non-overlap are all unchecked. An operator who inverts a pair (`LEVEL 2 190 115 190 265`) produces a tier that can never be offered, because the consumer rejects levels outside the window; nothing reports the mistake.

Because `LEVEL` shares the six-token scan with `EXP` and `COIN`, a `LEVEL` record with only five tokens would leave `val6` converted from an uninitialised buffer, and the compound validation would reject the record only if the indeterminate value happened to fall outside `0..399` - a non-deterministic outcome. This is the clearest example in the component of validation being applied to unvalidated input.

**Rule workflow**:

```
key == "LEVEL":
    tier = val2 ; require 0 <= tier < 5
    require 0 <= minMortal < 400, 0 <= maxMortal < 400,
            0 <= minArch   < 400, 0 <= maxArch   < 400
    QuestLevel[tier][0..3] = minMortal, maxMortal, minArch, maxArch
consumer: MORTAL -> [0],[1] ; otherwise -> [2],[3]
```

---

### Business Rule: BR-08 - Quest validation summary and failure semantics

**Overview**:
All three quest record types share one failure semantics: reject the record, raise a modal dialog, keep the previous value.

**Detailed description**:
There are exactly two dialog texts: `"Índice inválido. (1)"` for tier faults (`:315,333,350`) and `"Índice inválido. (2)"` for amount/bound faults (`:321,339,361`). Both are `MessageBoxA` calls against `QUESTS_RATE_PATH` as the caption, so the dialog title tells the operator which file is at fault but the message does not say which line or which token. With a 100-record file and several faults, the operator receives a sequence of identical boxes and must correlate them by position.

Failure is per-record, not per-file: a file with one bad `EXP` line still applies the other 4 `EXP` lines, all `COIN` lines and all `LEVEL` lines. Because `QuestExp`/`QuestCoin`/`QuestLevel` are `static` arrays with initialisers (`:46-64`) that persist across calls, a partial reload leaves a hybrid of file values and previously applied values. The GM command that triggers this (`imple.cpp:255-257`) calls `BASE_InitializeBaseDef()` first, which resets the *Basedef* tables but not `QuestExp`/`QuestCoin`/`QuestLevel`, so quest reload is even less deterministic than rate reload.

There is no validation that the three tables are mutually consistent (rewards defined for a tier whose level window is impossible), no check that all five tiers are present, and no summary log of what was loaded. The only observable evidence of a successful load is the absence of dialogs.

**Rule workflow**:

```
for each record:
    if tier invalid  -> MessageBox("Índice inválido. (1)") ; skip record
    if values invalid-> MessageBox("Índice inválido. (2)") ; skip record
    store into QuestExp/QuestCoin/QuestLevel (previous value lost only on success)
```

---

### Business Rule: BR-09 - Class-dependent quest reward selection

**Overview**:
The quest tables are consumed with a hard class split: arch characters use the second column of `EXP` and the last two columns of `LEVEL`; mortal characters use the first.

**Detailed description**:
The split lives entirely in the consumer, not in the loader. `_MSG_UseItem.cpp:2245-2246` computes `minlevel`/`maxlevel` with a ternary on `pMob[conn].extra.ClassMaster == MORTAL`, and `:2260` does the same for experience. This means the loader's `[5][2]` and `[5][4]` shapes are semantic contracts that only exist by convention between the two files - the header (`CReadFiles.h:42-44`) declares bare dimensions with no naming of columns.

For the loader this rule matters in two ways. First, it explains why `EXP` takes two values while `COIN` takes one: coin is class-independent by design. Second, it means the loader cannot reject a file that defines only mortal values - the arch columns simply keep their defaults, producing an asymmetric economy that boots and runs without error.

The ARCH path is reached for any non-MORTAL `ClassMaster` (ARCH = 1, MORTAL = 2, `Basedef.h:178-179`), so future classes silently inherit the arch reward. No bounds check on `ClassMaster` exists at the read site, and no logging records which column was applied.

**Rule workflow**:

```
consumer:
    classMaster == MORTAL ? QuestLevel[t][0], QuestLevel[t][1], QuestExp[t][0]
                          : QuestLevel[t][2], QuestLevel[t][3], QuestExp[t][1]
    QuestCoin[t] used in both cases
```

---

### Business Rule: BR-10 - CompRate record grammar (two-token dispatch)

**Overview**:
`CompRate.txt` records are triples `<GROUP> <SUBKEY> <value>` where both the group and the sub-key must match exactly before the value is stored in a specific global slot.

**Detailed description**:
This is the largest and most complex parser in the component: 215 lines, 56 decision points, 46 `strcmp` calls (`:387-586`). Line reading and tokenising follow the same pattern as BR-01 (`fgets` 1023, `sscanf` three tokens, `atoi` on all three). The difference is that the **second** token is a string used for a nested dispatch that is *not* uppercased: `_strupr` is applied only to `str1` (`:405`), so sub-keys are matched case-sensitively and a file written in lower case silently matches nothing.

Seven top-level groups are recognised: `EHRE` (`:407`), `TINY` (`:437`), `SHANY` (`:449`), `AILYN` (`:461`), `AGATHA` (`:473`), `ODIN` (`:485`) and `COMPOSITOR` (`:569`). The value range check `0 <= val3 <= 100` is duplicated inside each branch rather than performed once before dispatch, which is why the same dialog text appears eight times. A consequence is that an unknown group or an unknown sub-key never reaches a range check and is discarded with no diagnostic at all - a typo such as `ODIN ITEM_CELESTAI 50` is a complete no-op.

The parser has no notion of exclusivity or completeness. Two lines for the same sub-key: last wins. A group with no sub-keys: group is simply absent. `EHRE` with a sub-key belonging to `ODIN`: no-op. This permissiveness is compounded by the fact that the same nine-character global names (`g_pEhreRate`, `g_pOdinRate`, ...) are declared in `Code/Basedef.h:2484-2493` but their slot semantics (which index means which in-game feature) are encoded only as `strcmp` string literals here, e.g. slot 6 of `g_pEhreRate` is bound to the literal `"TRAJE_MONTARIA"` (`:427`) and then consumed generically as `g_pEhreRate[combine]` in `_MSG_CombineItemEhre.cpp:113`.

**Rule workflow**:

```
fgets(line) -> (key, subkey, value)
_strupr(key)                       [subkey untouched]
key == "EHRE"        -> validate(value 0..100) -> subkey dispatch (7 slots)
key == "TINY"        -> validate(value 0..100) -> subkey "CHANCEBASE" -> g_pTinyBase
key == "SHANY"       -> validate(value 0..100) -> subkey "CHANCEBASE" -> g_pShanyBase
key == "AILYN"       -> validate(value 0..100) -> subkey "CHANCEBASE" -> g_pAilynBase
key == "AGATHA"      -> validate(value 0..100) -> subkey "CHANCEBASE" -> g_pAgathaBase
key == "ODIN"        -> validate(value 0..100) -> subkey dispatch (24 targets)
key == "COMPOSITOR"  -> validate(value 0..100) -> "ITEM_+7/+8/+9" -> g_pAnctChance[0..2]
otherwise            -> no-op, no diagnostic
```

---

### Business Rule: BR-11 - Ehre (combination) rate slots

**Overview**:
Seven textual sub-keys of the `EHRE` group bind to seven of the ten slots of `g_pEhreRate`; the remaining three slots are reserved and not configurable.

**Detailed description**:
The mapping is `PACOTE_ORI -> [1]`, `MISTERIOSA -> [2]`, `ESPIRITUAL -> [3]`, `AMUNRA -> [4]`, `TRAJE_MONTARIA -> [6]`, `RETIRAR_TRAJE_MONTARIA -> [7]`, `SOUL -> [8]` (`:415-434`). Slot 0 and slot 5, and slot 9, are never written; their compiled defaults come from `Basedef.cpp:75-78` (`{0,100,100,40,10,10,100,100,100,100}`). Slot 0's default of 0 is deliberately unreachable/unusable, and slots 5 and 9 keep 10 and 100 respectively, so the visible default behaviour of those features is preserved only as long as nobody reloads - a reload cannot change them either, since no key targets them.

At the consumer, `_MSG_CombineItemEhre.cpp:113` reads `g_pEhreRate[combine]` where `combine` is derived from the client's item layout, and combines that with hardcoded guards nearby (`:61` for coin cost on `combine == 6 || 7`; `:69-81` for stat ceilings on `combine == 3 || 4`). Thus the file controls *probability* while the handler controls *eligibility* and *cost*; a file that sets slot 6 or 7 to 0 disables the mount-cost path's success without disabling its 1,000,000 coin cost, which is a business-rule interaction not visible from the config file.

Because the values are probabilities compared against a random roll (as elsewhere in the combine handlers), values above 100 are rejected by BR-15, but the parser has no knowledge of the roll range; correctness depends on the consumer's `rand() %` modulus being compatible with the configured numbers.

**Rule workflow**:

```
EHRE PACOTE_ORI              -> g_pEhreRate[1]
EHRE MISTERIOSA              -> g_pEhreRate[2]
EHRE ESPIRITUAL              -> g_pEhreRate[3]
EHRE AMUNRA                  -> g_pEhreRate[4]
EHRE TRAJE_MONTARIA          -> g_pEhreRate[6]
EHRE RETIRAR_TRAJE_MONTARIA  -> g_pEhreRate[7]
EHRE SOUL                    -> g_pEhreRate[8]
(other EHRE sub-keys)        -> no-op
```

---

### Business Rule: BR-12 - NPC combine base chances

**Overview**:
The four NPC-specific combine handlers each expose a single `CHANCEBASE` scalar that replaces a compiled base probability.

**Detailed description**:
`TINY`, `SHANY`, `AILYN` and `AGATHA` each accept only the sub-key `"CHANCEBASE"` and assign to one scalar (`g_pTinyBase` `:446`, `g_pShanyBase` `:458`, `g_pAilynBase` `:470`, `g_pAgathaBase` `:482`). Only one sub-key is honoured per group, so any other sub-key is a silent no-op. The scalars are *bases*: consumers add item-derived or level-derived terms on top, e.g. `GetFunc.cpp:214` returns `g_pTinyBase + (BASE_GetItemAbility(&Item[1], EF_ITEMLEVEL) * 5)` and `:424` returns `g_pAgathaBase + (grade * 5) + (itemLevel == 5 ? 30 : 1)`. Setting a base to 100 therefore does not guarantee 100% success; it only guarantees the *floor* is 100 while the additions push higher.

`_MSG_CombineItemShany.cpp:89` shows a further interaction: `if (_rand <= g_pShanyBase || LOCALSERVER)`, so the comparison is `<=` (not `<=` on a 0..99 roll with a 0..100 threshold), meaning a configured base of 0 still succeeds when the roll is 0. The same pattern appears for Odin (`_MSG_CombineItemOdin.cpp:124`). The file contract does not document this off-by-one-style edge, and the loader cannot detect it.

Defaults (`Basedef.cpp:90-93`: 15, 35, 10, 15) are all non-zero, so an absent file leaves the four features functional with their original difficulty; a *present* file that omits one group leaves that group at default while the others change, which is the intended partial-override behaviour but is indistinguishable from an operator omission.

**Rule workflow**:

```
GROUP in {TINY, SHANY, AILYN, AGATHA}:
    require value 0..100
    subkey == "CHANCEBASE" -> assign matching scalar (Tiny/Shany/Ailyn/Agatha Base)
    otherwise              -> no-op
consumer: result = Base + level/grade bonuses  (no upper clamp)
```

---

### Business Rule: BR-13 - Odin refine/craft rate slots

**Overview**:
The `ODIN` group is the densest mapping in the component: twenty-four sub-keys addressing three different arrays that together define the Odin refine economy.

**Detailed description**:
The mapping distributes across three globals. `ITEM_CELESTIAL -> g_pOdinRate[1]` (`:494`); `ITEM_12_SECRETA -> g_pItemSancRate12[0]` (`:497`) and `ITEM_12_REF_0..9 -> g_pItemSancRate12[1..10]` (`:500-527`); `ITEM_12_MINUS_12..15 -> g_pItemSancRate12Minus[0..3]` (`:530-539`); and the remaining nine keys (`PISTA`, `DESTRAVE_LV40`, `PEDRA_DA_FURIA`, `SECRETA_AGUA`, `SECRETA_TERRA`, `SECRETA_SOL`, `SECRETA_VENTO`, `SEMENTE_CRISTAL`, `CAPA_CELESTIAL`) map to `g_pOdinRate[3..11]` (`:541-566`). `g_pOdinRate[0]` and `[2]` are never written - default 0 and 100 respectively (`Basedef.cpp:80-83`).

Semantic asymmetry matters here: `g_pOdinRate` slots are *success probabilities* compared against a random roll (`_MSG_CombineItemOdin.cpp:124` and five further sites), whereas `g_pItemSancRate12` slots are *additive modifiers* to a rate (`:170-203`, `rate += ...`) and `g_pItemSancRate12Minus` slots are *subtractive* (`:213-222`, `rate -= ...`). The same file, with the same `0..100` validation, therefore feeds both probability and delta semantics. A value of 100 in `ITEM_12_REF_9` adds 100 percentage points to a refine rate, while a value of 100 in `ITEM_CELESTIAL` means "always succeed"; the loader cannot express or enforce the distinction, and the bounds check is the same for both.

There is also no ordering or coverage rule: `ITEM_12_REF_0..9` need not be monotonic, so an operator can create a rate curve that decreases with refinement level while the `MINUS_*` table simultaneously subtracts more at higher levels. The resulting rate can become negative; `_MSG_CombineItemOdin.cpp` performs no floor clamp before comparing.

**Rule workflow**:

```
ODIN ITEM_CELESTIAL        -> g_pOdinRate[1]              (probability)
ODIN ITEM_12_SECRETA       -> g_pItemSancRate12[0]       (additive)
ODIN ITEM_12_REF_0..9      -> g_pItemSancRate12[1..10]   (additive)
ODIN ITEM_12_MINUS_12..15  -> g_pItemSancRate12Minus[0..3](subtractive)
ODIN PISTA                 -> g_pOdinRate[3]
ODIN DESTRAVE_LV40         -> g_pOdinRate[4]
ODIN PEDRA_DA_FURIA        -> g_pOdinRate[5]
ODIN SECRETA_AGUA/TERRA/SOL/VENTO -> g_pOdinRate[6..9]
ODIN SEMENTE_CRISTAL       -> g_pOdinRate[10]
ODIN CAPA_CELESTIAL        -> g_pOdinRate[11]
g_pOdinRate[0], [2]        -> not addressable from file
```

---

### Business Rule: BR-14 - Compositor (anvil) chances

**Overview**:
`COMPOSITOR <ITEM_+7|ITEM_+8|ITEM_+9> <value>` sets the three anvil-upgrade bonus chances.

**Detailed description**:
The three sub-keys map positionally onto `g_pAnctChance[0..2]` (`:577-584`), whose compiled defaults are `{2,4,10}` (`Basedef.cpp:95`). The consumer at `GetFunc.cpp:83-95` adds the corresponding value to a computed combine rate depending on the target item's current sanctification (`rate = rate + g_pAnctChance[0]`, etc.). So unlike the Odin slots these are strictly *bonuses*, and the file cannot reduce below the computed base - it can only add.

The sub-key literals contain a `+` character (`"ITEM_+7"`), which is not a valid identifier fragment and appears only in this file contract; they are matched after tokenisation by `sscanf`'s `%s`, which stops at whitespace, so the plus sign survives intact. There is no equivalent key for `+10` or higher, and no key to disable a bonus other than writing `0`.

Because only three slots exist and all three have compiled defaults, this group is the least risky of the `CompRate` groups; its main hazard is the shared `0..100` validation, which allows values that push a combine probability above certainty with no downstream clamp.

**Rule workflow**:

```
COMPOSITOR ITEM_+7 -> g_pAnctChance[0]
COMPOSITOR ITEM_+8 -> g_pAnctChance[1]
COMPOSITOR ITEM_+9 -> g_pAnctChance[2]
consumer: rate = rate + g_pAnctChance[n]   (GetFunc.cpp:83,89,95)
```

---

### Business Rule: BR-15 - CompRate value validation

**Overview**:
Every `CompRate` value must lie in `0..100`; the check is duplicated per group, and an unknown group or sub-key bypasses validation entirely.

**Detailed description**:
The identical guard `if (val3 < 0 || val3 > 100) { MessageBoxA(...,"Índice inválido. (1)", COMP_RATE_PATH, MB_OK); continue; }` appears at `:409`, `:439`, `:451`, `:463`, `:475`, `:487` and `:571`. The duplication means the guard executes only after the group key matched; a line whose group is misspelled is discarded before any check, so malformed values in unrecognised groups never produce a diagnostic. Duplicated validation also means a single changed domain limit (for example allowing 200 for a bonus) requires seven edits and, as BR-13 shows, one limit is already wrong for the additive slots.

`continue` semantics reject the whole record - the sub-key dispatch is skipped - so a bad value leaves the target slot at its previous content. There is no clamping anywhere in this parser, unlike `ReadGuild`'s `CityTax` normalisation (BR-29), which is the only clamp in the component.

The observable operator experience is one modal dialog per invalid record, each blocking the server, with a message that identifies neither the line number nor the offending value. Because `CompRate.txt` carries the largest number of records (potentially dozens), the practical failure mode is a long series of dialog boxes at boot with no post-hoc record of which records were accepted.

**Rule workflow**:

```
value outside 0..100 (inside a recognised group)
    -> MessageBoxA("Índice inválido. (1)", COMP_RATE_PATH)
    -> continue (record discarded, slot keeps previous value)
unknown group / unknown sub-key
    -> silently discarded, never validated
```

---

### Business Rule: BR-16 - Absence handling for rate files (blocking dialog, defaults retained)

**Overview**:
If `SancRate.txt`, `QuestsRate.txt`, `CompRate.txt` or `MobMerc.txt` cannot be opened, the loader shows a modal warning and returns without touching any table.

**Detailed description**:
The four guards are identical in shape: `if (fp == NULL) { MessageBoxA(hWndMain, "Não foi possivel encontrar o arquivo.", PATH, MB_OK); return; }` (`:83-87`, `:280-284`, `:381-385`), with `MobMerc` using its own text `"MobMerc.txt não foi encontrado."` (`:595-600`). Because they return before any assignment, every consumer sees the compiled defaults registered in `Basedef.cpp:61-102` (rates, bases, chance bonuses) or the CReadFiles static initialisers (`:46-64`). The server therefore starts and runs - this is a fail-open policy, not fail-fast.

The `MessageBoxA` is critical-path: the constructor runs inside `InitInstance` (`Server.cpp:3627`) before `StartListen` (`:3634`), before timers are useful, and with no window yet accepting client connections beyond the created frame. On a headless or service-style launch the dialog is invisible and the process blocks indefinitely; there is no timeout, no fallback to logging, and no CI/deployment mode that suppresses it. The message text is UTF-8 bytes passed to the ANSI API under a `MultiByte` build (`TMSrv.vcxproj:27,34`), so on a CP1252 system it renders as mojibake - the operator sees a garbled string with the correct file name as the caption.

The policy also means "file missing" and "file unreadable" (locked, permission denied) are indistinguishable, and no log line is emitted in either case. `Log()` is available and is used by the two guild loaders instead - an inconsistency that makes triage of a rate-table problem rely entirely on a human having been present at boot.

**Rule workflow**:

```
fopen(path) == NULL
    -> MessageBoxA(hWndMain, "<pt-BR message>", path, MB_OK)   [blocking]
    -> return                                                   [no table touched]
    -> consumers use Basedef.cpp / CReadFiles.cpp compiled defaults
```

---

### Business Rule: BR-17 - Absence handling for Admin/Mac files (silent)

**Overview**:
`ReadAdmin` and `ReadMacblock` return silently when their file cannot be opened - no dialog, no log entry.

**Detailed description**:
Both functions contain the minimal guard `if (fp == NULL) return;` (`:646-647`, `:684-685`). Nothing is written to `fLogFile` and nothing is shown to the operator, in contrast with the rate loaders (BR-16) and the guild loaders (BR-26). The in-memory arrays (`pAdminIP`, `pMac`) keep whatever they held; at boot that is the zero state established by `Server.cpp:3493` (`memset(pMac, 0, sizeof(pMac))`) or by static initialisation (`pAdminIP[MAX_ADMIN] = {0,};` at `Server.cpp:264`).

For `pMac` the fail-open behaviour is a *security* decision embedded in an absence handler: with no file, no MAC is blocked, and logins proceed normally. There is no "deny by default" mode, no minimum entry count, and no audit trail. Because `ReadMacblock` is also called twice per second (BR-22), a transient sharing violation on a file being edited by an external tool silently clears nothing but also logs nothing - an operator who saves an empty file mid-edit can transiently empty the in-memory block-list without any evidence in the log.

For `pAdminIP` the effect is mitigated by the fact that TMSrv never consults the array for a decision (Section 2.5); enforcement is in DBSrv (`Code/DBSrv/Server.cpp:985-988`), which has its own, equally silent loader (`Code/DBSrv/Server.cpp:361-362`). Both servers therefore share one file with two independent, non-atomic readers and no coordination.

**Rule workflow**:

```
fopen("../../DBSRV/Run/Admin.txt" | "../../DBSRV/Run/Mac.txt") == NULL
    -> return                        [no message, no log, no state change]
    -> prior (zero) contents remain -> no IP allow-list, no MAC blocked
```

---

### Business Rule: BR-18 - Admin/Mac record grammar (dotted quad)

**Overview**:
`Admin.txt` and `Mac.txt` share one grammar: `<slotIndex> <a>.<b>.<c>.<d>`, with the dots rewritten to spaces before numeric scanning.

**Detailed description**:
The reader (`:654-665` and `:692-703`) fetches at most 126 characters per line (`fgets((char*)ttt, 127, fp)`) into a 256-byte buffer, then loops `for (int i = 0; i < 255; i++) if (ttt[i] == '.') ttt[i] = ' ';`. The loop is fixed-bound rather than string-bounded, so it inspects 255 bytes of a 256-byte buffer regardless of the actual line length, converting any dot that happens to occur in uninitialised stack memory - harmless in effect (the scanning stops at the NUL that `fgets` writes) but a clear example of the buffer discipline in this component. The rewrite is also destructive: it prevents any later code from recovering the original text and makes the parser unable to accept alternative separators.

`sscanf(ttt, "%d %d %d %d %d", &idx, &a, &b, &c, &d)` (`:665`, `:703`) then reads five integers: the slot index first, then the four octets. This ordering is the reverse of the human convention "IP then index" and is only discoverable by reading the code; the companion writer in the GM command emits the same order (`imple.cpp:1697`: `fprintf(fp, "%d %d.%d.%d.%d\n", x, cIP[0], cIP[1], cIP[2], cIP[3])`), which confirms the contract by round-trip.

If `sscanf` fails on the first conversion, `idx` keeps its initialiser `-1` (`:659`, `:697`) and the record is dropped by the range check; if it fails on later conversions, `a..d` keep their per-iteration zero initialisation (`:651-652`, `:689-690`), so a truncated line silently yields a zero octet rather than an error. There is no comment support and no blank-line guard: a blank line leaves `ttt` as fetched (just a newline), `sscanf` returns 0, `idx` stays `-1`, and the record is skipped - correct by accident.

**Rule workflow**:

```
fgets(ttt, 127)
replace every '.' in the first 255 bytes with ' '
sscanf("%d %d %d %d %d") -> idx, a, b, c, d   (missing fields default 0; idx defaults -1)
range-check idx -> store
```

---

### Business Rule: BR-19 - Admin/MAC slot index bounds

**Overview**:
Entry indices must address an existing array slot: `0..MAX_ADMIN-1` for Admin, `0..MAX_MAC-1` for MAC. IP octets are not validated.

**Detailed description**:
`if (idx < 0 || idx >= MAX_ADMIN) continue;` (`:669-670`) and `if (idx < 0 || idx >= MAX_MAC) continue;` (`:705-706`) use the constants from `Code/Basedef.h:58` (`MAX_ADMIN = 10`) and `:152` (`MAX_MAC = 200`). Out-of-range records are dropped silently, with no dialog (unlike the rate loaders) and no log - consistent with BR-17's silent policy. Duplicate indices are not detected: the array is assigned unconditionally, so the last record for a slot wins and earlier entries are silently lost.

The octets `a`, `b`, `c`, `d` receive no validation whatsoever, and this is a genuine correctness gap rather than a stylistic omission: `ReadAdmin` packs them with shifts (`ip = (d << 24) + (c << 16) + (b << 8) + a`, `:667`), so an octet of 300 contributes `0x12C` and overflows into the neighbouring byte, producing an IP that neither the operator wrote nor the DBSrv comparison can ever match. Similarly a negative octet propagates sign bits through the shift. `ReadMacblock` stores the four values verbatim into `int Mac[4]` (`:708-711`), where the comparison in `ProcessDBMessage.cpp:374` is exact equality - so an out-of-range octet simply makes the entry inert rather than blocking the wrong client.

Because the dotted-quad is destroyed before parsing (BR-18), a value such as `192.168.1` (three octets) becomes `192 168 1` and yields `d = 0`, i.e. the address `192.168.1.0` - accepted, range-valid, and almost certainly not what the operator meant. Nothing warns.

**Rule workflow**:

```
idx < 0 or idx >= MAX_ADMIN (10)  -> continue  [silent]
idx < 0 or idx >= MAX_MAC (200)   -> continue  [silent]
octets a,b,c,d                    -> not validated (0..255 assumed)
```

---

### Business Rule: BR-20 - IP packing and MAC octet storage

**Overview**:
Admin IPs are packed little-endian from the dotted quad; MACs are stored as four distinct integers.

**Detailed description**:
The Admin pack is `unsigned int ip = (d << 24) + (c << 16) + (b << 8) + a;` (`:667`), so the first written octet occupies the least significant byte. On a little-endian x86 the byte layout in memory then reads `a.b.c.d`, which is what the writer at `imple.cpp:1696-1697` relies on when it re-emits `cIP[0..3]`. This makes the on-disk format and the in-memory format mutually consistent only on little-endian hardware and only because both sides use the same convention; the semantic value (which integer the comparison at `DBSrv/Server.cpp:988` receives) is `a | b<<8 | c<<16 | d<<24`, which is *not* the conventional `inet_addr` order (`a<<24 | b<<16 | c<<8 | d`). The comparison therefore works only against another value produced by the same packing - `pUser[conn].IP`, whose storage convention is set elsewhere in the socket layer.

The MAC is not packed at all: `pMac[idx].Mac[0] = a; ... Mac[3] = d;` (`:708-711`) into `STRUCT_BLOCKMAC { int Mac[4]; }` (`Basedef.h:916-919`). The array is compared element-wise against `pUser[conn].Mac[0..3]` (`CUser.h:93`, `int Mac[4]`), and an entry of all zeros is skipped (`ProcessDBMessage.cpp:371`) so the zero MAC can never be blocked. This is a four-group identity, not a six-byte MAC - the client identity is a 4-integer fingerprint, and the file format reflects that.

The asymmetry between the two files is a maintenance hazard: both share the same line grammar and the same `a.b.c.d` notation, but one is packed and one is not, with no comment marking the difference.

**Rule workflow**:

```
Admin: ip = a + (b<<8) + (c<<16) + (d<<24) ; pAdminIP[idx] = ip
Mac:   pMac[idx].Mac[0]=a ; [1]=b ; [2]=c ; [3]=d
compare: element-wise equality against CUser::Mac[4]
```

---

### Business Rule: BR-21 - MAC block enforcement at login confirmation

**Overview**:
A client whose four-integer MAC matches any non-zero `pMac` entry is refused during `_MSG_DBCNFAccountLogin` with the localized message `_NN_MAC_Block`.

**Detailed description**:
Enforcement is outside this component (it belongs to the DB-message dispatcher) but the rule is *defined* by the data contract this loader produces. `ProcessDBMessage.cpp:369-383` iterates `x` over `MAX_MAC`, skips all-zero entries, and on a full four-field match sends `_NN_MAC_Block`, flushes the socket and calls `CloseUser` before the account is transitioned to character selection. This is a pre-authentication-path denial implemented as an early exit inside the login confirmation handler.

The dependency on this loader is direct and timing-sensitive: because the block-list is refreshed twice per second (BR-22), an operator adding a MAC entry expects a near-immediate effect for new logins, while an already-connected user is never re-evaluated (the check runs only on this message). Conversely, removing an entry takes effect on the next login attempt, not on a retry within the same second.

The loader's silent absence handling (BR-17) means the enforcement set is empty whenever the file is missing or unreadable, and the permissive grammar (BR-18/BR-19) means a typo in an octet can silently disable a block silently. Neither failure mode produces any log entry from this component, so the only evidence available to an operator is the absence of the `MAC Block` message client-side.

**Rule workflow**:

```
on _MSG_DBCNFAccountLogin (after account-name match):
  for x in 0..MAX_MAC-1:
      if pMac[x] all zero -> skip
      if pMac[x] == pUser[conn].Mac -> send _NN_MAC_Block ; CloseUser ; break
  continue login (IsBillConnect=0, mode USER_SELCHAR)
```

---

### Business Rule: BR-22 - Periodic MAC list refresh

**Overview**:
`ReadMacblock` is invoked from the 500 ms timer on all but one tick out of 120, making operator edits to `Mac.txt` take effect within roughly half a second without a restart.

**Detailed description**:
`ProcessSecTimer` increments `SecCounter` (`ProcessSecMinTimer.cpp:123`) and then executes `if (SecCounter % 120) CReadFiles::ReadMacblock();` (`:127-128`). The condition as written is true for every value except multiples of 120, i.e. it reloads 119 times out of every 120 ticks and skips once - the inverse of the apparent intent ("reload every 120 seconds"). With the timer armed at 500 ms (`Server.cpp:3631`) this is roughly 2 reloads per second, each performing a `fopen`/`fgets` loop/`fclose` over up to 200 entries, synchronously inside the window-procedure message handler.

Three properties follow. First, the cost is paid on the message-pump thread that also services `WSA_READ` and `WSA_READDB`; a slow or contended file (antivirus, network share, editor holding an exclusive lock) delays packet processing for every connected client. Second, writes to `pMac` are unsynchronised with the readers: `ProcessDBMessage` may observe a half-updated array (some slots from the new file, some from the old) because the reload overwrites entries one at a time without clearing first. Third, the once-per-120 skip creates a one-tick window where the array is not refreshed at the very moment a "minute rollover" style operation is happening - harmless in practice but evidence that the guard was written to gate a periodic (not per-tick) action and lost that meaning.

An earlier sibling defect of the same shape is recorded in the project's own server-core report; here the observable consequence is simply that `Mac.txt` behaves as a hot-reloadable list while its counterpart `Admin.txt` is only read at boot and by the `+npko` command - two files in the same directory with different refresh semantics.

**Rule workflow**:

```
ProcessSecTimer (every 500 ms):
    SecCounter++
    if (SecCounter % 120) != 0 -> ReadMacblock()
        fopen("../../DBSRV/Run/Mac.txt","r") -> parse all records -> pMac[0..199]
    else -> skip (one tick in 120)
```

---

### Business Rule: BR-23 - MobMerc section format

**Overview**:
`MobMerc.txt` uses a section-per-merchant format with `#` headers, `/` comments, and `KEY: value` bodies that describe a merchant's mob identity, restock timers and 64 item slots.

**Detailed description**:
`ReadMobMerc` (`:591-638`) is structurally unlike every other loader: state is carried by a section counter (`Num`, starting at `-1` at `:602`), `#` begins a new merchant (`:619-627`), `/` marks a comment (`:616-617`), and any line whose first byte is not `CR` (`13`) is delegated to `ParseMobMercString` (`:629-635`). The header line also performs the record reset: `memset(&pMobMerc[Num], 0, sizeof(STRUCT_MERC))`, so a merchant with no body is a zeroed entry.

`ParseMobMercString` (`Server.cpp:9286-9376`) implements the body grammar: `MOBNAME: <name>` (copied with `strncpy(..., NAME_LENGTH)` = 16 bytes, no explicit terminator), `GENERATEINDEX: <n>` (`:9344`), `RENEWTIME: <n>` and `REBORNTIME: <n>` (both multiplied by 2 at `:9347` and `:9350`), and `ITEM_<0..63>: ...` matched by building `sprintf(temp, "ITEM_%d:", i)` inside a 64-iteration loop (`:9354-9373`). The item branch assigns `sIndex`, three effect pairs and `MaxStock`/`Stock` from eight integers, and returns immediately on the first match.

The parsing of the item *values* is not consistent with the line tokenisation used by the caller: the caller has already split the line into `str1` (key) and `str2`/`str3` (the next two whitespace-separated tokens) via `sscanf(str, "%s %s %s", ...)` (`:9300`), so `str2` is a single token; the subsequent `sscanf(str2, "%s %s %s %s %s %s %s - %s", item_str1..item_str8)` (`:9322`) can only populate `item_str1` and leaves the other seven 256-byte buffers uninitialised before `atoi` reads them (`:9323-9330`). Only `sIndex` is therefore reliably set from a whitespace-separated file. This is strong independent evidence that the whole path is abandoned code (Section 1.3.1): even if it were wired up, the item table would be filled from indeterminate stack memory.

**Rule workflow**:

```
Num = -1
for each line:
    first byte '/'  -> skip (comment)
    first byte '#'  -> if Num >= 128 -> stop ; Num++ ; memset(pMobMerc[Num])
    first byte != CR -> ParseMobMercString(Num, line)
                          MOBNAME:       -> MobName[16]
                          GENERATEINDEX: -> GenerateIndex
                          RENEWTIME:     -> RenewTime = n*2
                          REBORNTIME:    -> RebornTime = n*2
                          ITEM_i:        -> Carry[i], MaxStock[i] = Stock[i] = stock
```

---

### Business Rule: BR-24 - Merchant count cap and timer scaling

**Overview**:
At most 128 merchants are read, and both restock timers are stored at twice the configured value.

**Detailed description**:
The cap is implemented as `if (Num >= MAX_MOB_MERC) break;` *before* the increment (`:621-622`) with `MAX_MOB_MERC = 128` (`Basedef.h:158`) and `pMobMerc[MAX_MOB_MERC]` (`Server.h:400`). Because the check compares against 128 and then increments, the 129th `#` header (when `Num == 127`) passes the check, sets `Num = 128`, and memsets `pMobMerc[128]` - one element past the end of the array. The correct guard would be `>= MAX_MOB_MERC - 1`. The same loop also fails to guard the initial `-1`: a data line before any `#` calls the parser with `Num == -1`.

Timer scaling (`value * 2`) compensates for the fact that the consumer compares against `SecCounter`, which advances every 500 ms rather than every second (`ProcessSecMinTimer.cpp:1344-1348`), so a configured value of 30 becomes 60 ticks = 30 seconds of wall clock. This is an implicit contract between the file's units (seconds) and the timer's actual period, encoded as a magic `* 2` in the parser rather than as a documented conversion.

Restock and reborn use modulo arithmetic (`SecCounter % RenewTime == 0`), and the consumer skips any entry whose `RenewTime`, `RebornTime` or `GenerateIndex` is zero (`:1341`), which is the only protection against a zeroed table - and, given the array is never populated, the reason the feature is silently inert rather than dividing by zero.

**Rule workflow**:

```
on '#': if Num >= 128 -> stop ; Num++ ; zero record
on data: parse into pMobMerc[Num]
times stored as config * 2 (ticks at 500 ms)
consumer skips entries with RenewTime==0 || RebornTime==0 || GenerateIndex==0
```

---

### Business Rule: BR-25 - Per-server file naming

**Overview**:
The three world-state files are named from the server identity: `Chall_<group>_<index>.txt`, `Guild_<group>_<index>.txt`, `ChampionCity_<group>_<index>.txt`, each field formatted with `%2.2d`.

**Detailed description**:
The path constants embed printf placeholders (`:41-43`) and every accessor formats them with the current `ServerGroup` and `ServerIndex` (`:721`, `:747`, `:778`, `:831`, `:876`). Both are globals initialised to `-1` (`Server.cpp:50`, `:52`) and assigned during boot from the server-list resolution; `%2.2d` with `-1` yields the literal `-1`, so a failure to resolve the server identity produces paths such as `../../Common/Chall_-1_-1.txt` rather than an error. The loader never validates that `ServerGroup`/`ServerIndex` are in range, and note that the same identity is used by the guild-id formula (`ServerIndex * MAX_GUILD + GuildCounter`, `_MSG_MessageWhisper.cpp:210`), so an unresolved identity corrupts both the filename and the guild namespace.

Path construction uses `char dir[256]` and `sprintf` (`:719`, `:745`, `:776`, `:829`, `:875`) with no length check; given fixed 20-character prefixes and two 2-3 character numbers the practical risk is nil, but the idiom is unbounded by construction.

The paths are relative (`../../Common/...`), resolved against the process's **current working directory**, which for an MSVC Debug/Release build is the project output directory. This couples the runtime layout to the deployment directory structure (`<run>/<server>/<bin>` style) and is not configurable at runtime: there is no environment-variable override, no registry lookup, and no `config.ini` entry pointing at a data root.

**Rule workflow**:

```
sprintf(dir, "../../Common/Chall_%2.2d_%2.2d.txt", ServerGroup, ServerIndex)
   ServerGroup = -1 -> "Chall_-1_-1.txt"
   ServerGroup = 3, ServerIndex = 7 -> "Chall_03_07.txt"
same pattern for Guild_ and ChampionCity_
```

---

### Business Rule: BR-26 - Generic-file fallback

**Overview**:
When the per-server file cannot be opened, `Chall.txt` / `Guild.txt` in the current working directory is used as a single-server fallback.

**Detailed description**:
Read and write paths both implement a two-step open: the per-server name first, then a bare `"Chall.txt"`/`"Guild.txt"` fallback (`:727-728`, `:753-754`, `:784-785`, `:837-838`). The fallback is a *relative* bare filename, so it resolves against the CWD, which is generally not the `../../Common` directory used by the primary path - meaning the two candidates live in different physical directories and the fallback would only be found for single-server deployments that launch from a directory containing the file.

The fallback is a silent substitution: no log entry records that the primary file was missing and the generic one used. Only if *both* opens fail is a log line produced (`:732`, `:758`, `:789`, `:842`). This makes the deployment mode (per-server vs generic) effectively invisible, and creates a write hazard: a server whose per-server file is missing will *create* `Chall.txt`/`Guild.txt` in its CWD on the next write and never migrate it back. Two servers sharing a working directory would then overwrite each other's guild state.

The read fallback is text/binary asymmetric with the write (`"rt"`/`"rb"` vs `"wt"`), and `ReadGuild` uses `"rb"` while parsing with `fscanf` - harmless on Windows because the mode affects only newline translation of `fgets`/`fwrite`, but it means a file written in text mode and read in binary mode round-trips only because `%d` skips CR.

**Rule workflow**:

```
fopen("../../Common/Guild_<g>_<i>.txt") == NULL
    -> fopen("Guild.txt") == NULL
         -> Log("err,Reading Guild error - can't open Guild.txt") ; return
    -> parse generic file
write path applies the same two-step, creating the fallback file if needed
```

---

### Business Rule: BR-27 - Challenge money data contract

**Overview**:
`Chall_*.txt` holds exactly five integers - the highest challenge bid recorded per guild zone - in zone order, one per `fscanf("%d")`.

**Detailed description**:
The read loop is `for (int i = 0; i < ValidGuild; i++) fscanf(fp, "%d", &pChallangerMoney[i]);` (`:737-738`) with `ValidGuild` initialised to `MAX_GUILDZONE` (5) at `Server.cpp:55` and never reassigned anywhere in the repository (verified by search for `ValidGuild =`). The array itself is `int pChallangerMoney[6]` (`Server.h:304`, `Server.cpp:357`), so the sixth slot exists but is unreachable from the file - a decompilation residue.

Because the `fscanf` return value is discarded, a short file leaves the tail of the array at its previous contents (zero at boot); a file with non-numeric content leaves all five untouched; and because the loop does not check `feof`, a file with more than five numbers silently truncates. There is no validation that bids are non-negative, that they are ordered, or that they relate to the guild ids in `Guild_*.txt`.

The semantic rule is enforced at the single write site: `Server.cpp:2560-2567` only updates the array (and persists both files) when the challenging guild's fame debt `coin` exceeds the stored record for that zone, so the file is an authoritative "highest bid so far per zone" ledger that gates subsequent challenges. Loading a negative or inflated value from disk therefore directly changes who can challenge: a negative value lets any guild challenge; an inflated value blocks challenges until the debt exceeds it.

**Rule workflow**:

```
for i in 0..ValidGuild-1 (5):
    fscanf("%d", &pChallangerMoney[i])   [return ignored; missing values keep old contents]
consumers: Server.cpp:2560 compare ; :2563 assign ; WriteChallanger persists
```

---

### Business Rule: BR-28 - Guild state data contract

**Overview**:
`Guild_*.txt` holds 26 integers in five ordered groups describing the guild-zone occupancy, taxes, clan ownership and war victories.

**Detailed description**:
The exact sequence is: `GuildCounter` (`:799`), then five `ChargeGuild` values (`:801-802`), five `ChallangeGuild` values (`:804-805`), five `CityTax` values enclosed in the clamp (`:807-813`), five `Clan` values (`:815-816`) and five `Victory` values (`:818-819`). All reads use `fscanf(fp, " %d", ...)` (leading space in the later groups, plain `"%d"` for the counter) so any whitespace layout - including the newline-separated layout that `WriteGuild` produces - is accepted. The file is therefore positional and cannot be reordered or partially annotated.

Before reading, the loader zeroes only `ChargeGuild` and `ChallangeGuild` (`:793-797`); `CityTax`, `Clan` and `Victory` are **not** pre-zeroed. On a truncated file the last groups retain whatever they held (zero at boot, previous file contents on reload), so a partially written file is accepted with stale values - and the `CityTax` clamp runs on the stale value, possibly resetting it to 10 because the stale value happens to be out of range. `ChargeGuild`/`ChallangeGuild` behave the opposite way: they are zeroed first and then left at zero if their group is missing, silently declaring "no champion, no challenger" for all zones.

There is no validation that a `ChargeGuild` value corresponds to an existing guild id, no check that champion and challenger differ, and no check that `Clan` matches anything. The values are written straight into `g_pGuildZone`, which the guild-war state machine (`Server.cpp:6535-6600`) and the pricing paths (`_MSG_Buy.cpp:107`, `_MSG_Sell.cpp:182`) consume as authoritative.

**Rule workflow**:

```
zero ChargeGuild[0..4], ChallangeGuild[0..4]
read GuildCounter
read ChargeGuild[0..4]
read ChallangeGuild[0..4]
read CityTax[0..4]      -> clamp (BR-29)
read Clan[0..4]
read Victory[0..4]
if GuildCounter == 0 -> Log
```

---

### Business Rule: BR-29 - City tax normalisation

**Overview**:
`CityTax` values read from disk are clamped to `0..20` with `10` as the substitute, which conflicts with the `0..30` range accepted by the in-game command that writes them.

**Detailed description**:
The rule is implemented inline in the read loop: `fscanf(fp, " %d", &g_pGuildZone[i].CityTax); if (CityTax < 0 || CityTax > 20) CityTax = 10;` (`:809-812`). The default `10` matches `Basedef.cpp`'s ethos of a 10% city tax and is also the value used implicitly by the buy/sell formulas as a "normal" rate (`_MSG_Buy.cpp:113`: `Price += Price * CityTax / 100`).

The conflict with the writer side is concrete and verifiable. `_MSG_MessageChat.cpp:74-89` accepts `0 <= tax <= 30` (with the special case that a non-"0" text whose `atoi` is 0 is rejected), stores it into the same field, and calls `WriteGuild()`. `WriteGuild` (`:860`) persists the value verbatim. On the next `ReadGuild` - which happens at startup, on the `reloadguild` GM command, on the `ReadGuild` menu item, and on **any** `WriteGuild`-then-`ReadGuild` cycle in an operator workflow - values in `21..30` are replaced by `10`. A guild master who sets 25% tax therefore loses the setting on the next server restart without any message, and the guild's income changes silently.

Two further inconsistencies compound this. The consumer-side guards use yet another bound: `_MSG_Buy.cpp:109` treats `CityTax <= 0 || CityTax >= 30` as "no tax" and skips the surcharge, while `_MSG_Sell.cpp:184` accepts `0..30` inclusive. So a persisted 20 (legal, survives the clamp) is applied to selling but a persisted 0 disables buying tax entirely. The loader's clamp is the only place where a value is *changed* rather than rejected, which makes this the one rule in the component that mutates operator intent.

**Rule workflow**:

```
read CityTax
if CityTax < 0 or CityTax > 20 -> CityTax = 10
(WriteGuild persists whatever is in memory, including 21..30 set in-game)
(restart/reload -> 21..30 becomes 10)
```

---

### Business Rule: BR-30 - Guild counter integrity

**Overview**:
`GuildCounter` is a positive, monotonic guild-id allocator; a loaded value of zero is an error condition that disables guild creation.

**Detailed description**:
The counter is declared at `Server.cpp:53` with initialiser `1` and is the low component of every new guild id: `pMob[conn].MOB.Guild = ServerIndex * MAX_GUILD + GuildCounter` (`_MSG_MessageWhisper.cpp:210`), with `MAX_GUILD` = 4096 (`Basedef.h:147`). Two code paths increment it (badge purchase at `_MSG_Buy.cpp:224-231`, which also resets it from `>= 4096` to `2048`, and the whisper guild-creation path at `_MSG_MessageWhisper.cpp:256-263`), and both call `WriteGuild()` immediately after.

`ReadGuild` assigns `GuildCounter = 0` at entry (`:774`), overwrites it with the file value (`:799`), and then logs `"err, Reading Guild error - Guild counter zero"` if the result is 0 (`:823-824`). The `Log` is non-fatal: the server continues with `GuildCounter == 0`, which the creation path explicitly refuses (`_MSG_MessageWhisper.cpp:204-208`, message "Buy Guild item, but, Guild counter zero"). So a zero counter is a *soft lockout* of guild creation rather than a crash, and it is only visible in the log file and the window title.

The dangerous interaction is with the write path. Because the counter is zeroed *before* the open attempt (`:774` precedes `:782`), a failed open leaves it at 0; the next `WriteGuild()` - which fires on shutdown (`Server.cpp:4130`), on a guild tax change, on a champion assignment, and on guild creation - then truncates the real `Guild_*.txt` and writes `0` plus the current zone arrays. A transient file-lock during boot therefore escalates into permanent state loss. A second path to the same outcome is a legitimately zero counter in the file: `ReadGuild` accepts it, logs, and the next write persists it again.

**Rule workflow**:

```
ReadGuild: GuildCounter = 0
   read file value (if open succeeded)
   if value == 0 -> Log("err, Reading Guild error - Guild counter zero")
   guild creation guard: GuildCounter == 0 -> refuse
WriteGuild: write current GuildCounter verbatim (0 possible after failed read)
```

---

### Business Rule: BR-31 - Challenge file serialisation

**Overview**:
`WriteChallanger` writes one `<value> \n` pair per zone, truncating the file, with no atomicity and no verification.

**Detailed description**:
The write loop is `fprintf(fp, "%d ", pChallangerMoney[i]); fprintf(fp, "\n");` for `i < ValidGuild` (`:763-767`), producing five lines such as `1200 \n950 \n...`. The trailing space is harmless because the reader uses `%d` (which skips leading whitespace), so the round-trip is stable; the trailing newline after the last value is *not* emitted, matching the guild file's last group behaviour and producing a file whose last line ends at the digit.

Serialisation is triggered from two places: a successful challenge that beats the record (`Server.cpp:2565`) and the end-of-season reset (`Server.cpp:6560`). Both are infrequent, both are on the main thread, and neither checks the return of `WriteChallanger` (it is `void`). Failure is reported only by `Log("err,writing chall.txt - can't open for write", ...)` (`:758`), which writes to the log file and sets the window title - there is no retry and no user-visible message, so a challenge win during a disk-full condition is lost silently while the in-memory record continues to gate future challenges.

There is no versioning, no header, no checksum, and no backup of the previous file. Because the file is the *only* persistence of the challenge ledger, a corrupted or truncated file silently resets the ledger to zeros on the next boot (short-file reads leave the array at zero), which benefits the next challenger.

**Rule workflow**:

```
trigger -> sprintf(path) -> fopen(path,"wt") [fallback "Chall.txt"] 
        -> per zone: fprintf("%d ", value) ; fprintf("\n")
        -> fclose (no error check)
```

---

### Business Rule: BR-32 - Guild file and champion-city projection serialisation

**Overview**:
`WriteGuild` persists the 26-integer guild state, then regenerates a separate human/external-tool-readable projection of the five city champions.

**Detailed description**:
The primary file is written in five lines: counter plus five charge guilds (line 1), five challenger guilds (line 2), five city taxes (line 3), five clans (line 4), five victories (line 5, no trailing newline) - see `:847-871`. Each subsequent group is prefixed with a leading space (`" %d"`) and terminated with `"\n"`, so line 1 reads `<counter> <c0> <c1> <c2> <c3> <c4>` and lines 2-5 read ` <x0> <x1> ... <x4>`. This is exactly the shape the reader accepts (BR-28), making the round-trip lossless for the six scalar groups - except for `CityTax`, which is not re-validated before writing (BR-29).

After closing that file, `WriteGuild` re-derives the champion-city file (`:875-903`). For each of the five zones it calls `BASE_GetGuildName(ServerGroup, g_pGuildZone[i].ChargeGuild, GuildName)` (`:894`), builds a mark filename with `sprintf(Guildmark, "b01%06d.bmp", ChargeGuild)` (`:896`) - note the hardcoded `b01` prefix and six-digit zero padding - and writes `"<CityName> <GuildName> <Guildmark>\n"`. City names are a hardcoded literal array `{ "Armia", "Arzan", "Erion", "Noatun", "Nipplehein" }` (`:887`) that must stay index-aligned with `g_pGuildZone`.

Two structural risks exist. First, the two files are written sequentially, so a failure on the second open leaves the first updated and the second stale, with `Log` (`:882`) as the only trace; the function then returns without restoring anything. Second, `GuildName` is a 128-byte uninitialised stack buffer and `BASE_GetGuildName` copies `GUILDNAME_LENGTH` (12) bytes without guaranteeing a NUL (`Code/Basedef.cpp:1332`) before calling `BASE_UnderBarToSpace`; if the source name is exactly 12 characters the subsequent `fprintf("%s")` can read past the copied name into uninitialised stack.

The projection has no reader in this repository, so it is a write-only artefact consumed by an out-of-tree tool or website - an interface whose contract is defined solely by this `fprintf` format string.

**Rule workflow**:

```
fopen(Guild_<g>_<i>.txt,"wt") [fallback Guild.txt]
   "<counter> <charge x5>\n" "<challange x5>\n" "<citytax x5>\n" "<clan x5>\n" "<victory x5>"
fclose
fopen(ChampionCity_<g>_<i>.txt,"wt")
   per zone: CityName[i] , BASE_GetGuildName(ChargeGuild) , "b01%06d.bmp"
fclose
```

---

### Business Rule: BR-33 - Challenge ledger update rule

**Overview**:
A guild may capture a zone only if the fame it forfeits exceeds the best previous bid recorded for that zone; on success both the challenge file and the guild file are rewritten.

**Detailed description**:
The rule is implemented in `Server.cpp:2540-2569`. The handler requires the challenging guild's `GuildInfo[Guild].Fame >= 100` (`:2530-2536`), deducts exactly 100 fame (`:2549`), and uses the *pre-deduction* fame as the bid (`:2547`: `coin = GuildInfo[Guild].Fame;` before the subtraction). It then compares `if (pChallangerMoney[ZoneChall] < coin)` (`:2560`) - a strict greater-than, so an equal bid does not take the zone - and on success sets `g_pGuildZone[ZoneChall].ChallangeGuild = Guild`, stores the new record, and calls `WriteChallanger()` followed by `WriteGuild()` (`:2562-2566`).

This is where the loaded `pChallangerMoney` becomes a live rule input: the file's five integers are the price of entry for the next challenger. Loading a zeroed or truncated file lowers every zone's threshold to 0, immediately allowing any guild with 100 fame to take any zone - a state that persists until a winning bid is written. Conversely, an inflated entry (laden from an out-of-range value that the loader did not validate, BR-27) can freeze a zone indefinitely.

The two writes are not transactional: `WriteChallanger` is called before `WriteGuild`, so a failure between them leaves the ledger updated and the zone occupancy stale, or vice versa on a retry. Both failures are logged only through `Log`, and neither function returns a status, so the handler cannot even detect the inconsistency.

**Rule workflow**:

```
on challenge acceptance:
   require GuildInfo[Guild].Fame >= 100
   coin = Fame ; Fame -= 100
   if pChallangerMoney[Zone] < coin:
        g_pGuildZone[Zone].ChallangeGuild = Guild
        pChallangerMoney[Zone] = coin
        WriteChallanger()   [persist ledger]
        WriteGuild()        [persist occupancy]
```

---

### Business Rule: BR-34 - Write-through persistence of guild-zone mutations

**Overview**:
Every mutation of `g_pGuildZone` performed by trusted operator or gameplay code immediately rewrites the entire guild file; there is no batching, debouncing or dirty-flag mechanism.

**Detailed description**:
Six call sites follow the pattern "mutate one field, then `WriteGuild()`": the GM command `+champ` sets `ChargeGuild` for a zone and writes (`imple.cpp:1049-1063`); the GM command `+chall` sets `ChallangeGuild` and writes (`:1065-1078`); the guild master's `guildtax` chat command sets `CityTax`, marks `TaxChanged[i]`, and writes (`_MSG_MessageChat.cpp:71-94`); guild creation via badge purchase increments `GuildCounter` and writes (`_MSG_Buy.cpp:224-231`); guild creation via whisper increments `GuildCounter` and writes (`_MSG_MessageWhisper.cpp:261-263`); and the challenge path writes (`Server.cpp:2565-2566`). The shutdown handler also writes once (`Server.cpp:4130`).

The consequence is that the file's integrity depends on the *caller's* validation, not the loader's: the writer performs no checks at all beyond "can I open it". `WriteGuild` will happily persist a negative `GuildCounter`, an out-of-range `CityTax` (BR-29), a champion guild id that does not exist, or `ChargeGuild == ChallangeGuild`. Only `CityTax` is ever re-validated, and only on the read side, which is how the read/write range conflict becomes observable in production.

Write-through on the message-pump thread also means a user-visible chat command (`guildtax`) performs synchronous disk I/O before the handler returns, and the five-line plus five-line rewrite is repeated for each mutation regardless of how many fields changed - a lost-update-free but I/O-amplifying design.

**Rule workflow**:

```
mutate g_pGuildZone[i] / GuildCounter  (caller has already validated, or not)
    -> WriteGuild()
         -> truncate Guild_<g>_<i>.txt, write 26 ints
         -> truncate ChampionCity_<g>_<i>.txt, write 5 lines
    -> (on failure) Log only; in-memory change remains applied
```

---

## 4. Component Structure

### 4.1 File tree with annotations

```
Code/TMSrv/
├── CReadFiles.h                    58 lines, 1,718 bytes  - class contract, 9 path constants, 3 public tables
│   ├── #include <stdio.h>                                - FILE*, fopen/fgets/fscanf
│   ├── #include "..\Basedef.h"                            - MAX_ADMIN/MAX_MAC/MAX_MOB_MERC, STRUCT_MERC
│   └── class CReadFiles                                   - public ctor; 10 static void loaders/writers;
│                                                          - public static int QuestExp/QuestCoin/QuestLevel;
│                                                          - private static const char* const paths (9)
├── CReadFiles.cpp                 904 lines, 18,706 bytes - implementation
│   ├── :19-33   includes                                  - Windows.h, stdio, fcntl, time, math, io, errno,
│   │                                                        Basedef.h, CPSock.h, ItemEffect.h, CReadFiles.h,
│   │                                                        Server.h, Language.h  (CPSock/ItemEffect/Language unused)
│   ├── :35-43   nine path constants                       - all relative, "../../Common/..." or "../../DBSRV/Run/..."
│   ├── :46-64   three quest tables + defaults             - the only instance-free mutable state owned here
│   ├── :66-75   constructor (composite loader)             - 7 calls, no MobMerc
│   ├── :77-272  ReadSancRate                              - 196 lines, 13 strcmp, 17 MessageBoxA
│   ├── :274-373 ReadQuestsRate                            - 100 lines, 3 strcmp, 7 MessageBoxA
│   ├── :375-589 ReadCompRate                              - 215 lines, 46 strcmp, 8 MessageBoxA (highest complexity)
│   ├── :591-638 ReadMobMerc                               - 48 lines, 0 call sites (dead)
│   ├── :640-676 ReadAdmin                                 - 37 lines, silent failures
│   ├── :678-715 ReadMacblock                              - 38 lines, called ~2x/second
│   ├── :717-741 ReadChallanger                            - 25 lines, no return-value checks
│   ├── :743-770 WriteChallanger                           - 28 lines, truncating writer
│   ├── :772-825 ReadGuild                                 - 54 lines, 6 fscanf
│   └── :827-904 WriteGuild                                - 78 lines, 2 files, 10 fprintf groups
├── Server.cpp (consumer/owner)     - hWndMain:254, temp:277, pAdminIP:264, pMac:271, pMobMerc:449,
│                                     GuildCounter:53, ValidGuild:55, pChallangerMoney:357, g_pGuildZone:490,
│                                     Log:6701, ParseMobMercString:9286, ctor call:3627, menu:4073/4101-4103/4130
├── Server.h (consumer header)      - externs: pMac:65, ServerIndex:202, ServerGroup:204, GuildCounter:205,
│                                     ValidGuild:207, pChallangerMoney:304, pAdminIP:318, temp:382,
│                                     pMobMerc:400, g_pGuildZone:417 ; ParseMobMercString declared :183
├── imple.cpp (GM interpreter)      - :250-262 +reloadfile, :844-848 +reloadguild, :1049-1079 +champ/+chall,
│                                     :1666-1703 +npko
├── ProcessSecMinTimer.cpp          - :123-128 timer-driven ReadMacblock
├── ProcessClientMessage.h          - :41 includes CReadFiles.h (transitive fan-in for all _MSG_*.cpp)
├── _MSG_UseItem.cpp                - :2245-2246, :2260-2263, :2277 read the three quest tables
├── _MSG_Buy.cpp                    - :107-116 tax pricing, :224-231 GuildCounter + WriteGuild, :243-255 pMobMerc
├── _MSG_Sell.cpp                   - :182-192 tax pricing
├── _MSG_MessageChat.cpp            - :62-101 guildtax command -> WriteGuild
├── _MSG_MessageWhisper.cpp         - :204-210, :261-263 guild creation -> WriteGuild
├── _MSG_CombineItemEhre.cpp        - :113 g_pEhreRate[combine]
├── _MSG_CombineItemOdin.cpp        - :124+ g_pOdinRate, :170-222 g_pItemSancRate12[Minus]
├── _MSG_CombineItemShany.cpp       - :89 g_pShanyBase
├── GetFunc.cpp                     - :83-95 g_pAnctChance, :214 g_pTinyBase, :314-370 g_pAilynBase, :424 g_pAgathaBase
├── ProcessDBMessage.cpp            - :369-383 MAC block rule
├── CCastleZakum.cpp                - :38 includes CReadFiles.h (no call)
├── CWarTower.cpp                   - :38 includes CReadFiles.h (no call)
└── TMSrv.vcxproj                   - :97 ClCompile CReadFiles.cpp ; :175 ClInclude CReadFiles.h ;
                                      :27,34 CharacterSet=MultiByte ; :28,35 PlatformToolset=v143
```

External artefacts referenced but **not present in the repository** (no sample data ships with the source, so every format below was reverse-engineered from the parsers):

```
../../Common/Settings/SancRate.txt
../../Common/Settings/QuestsRate.txt
../../Common/Settings/CompRate.txt
../../Common/Settings/MobMerc.txt
../../DBSRV/Run/Admin.txt          (shared with DBSrv)
../../DBSRV/Run/Mac.txt            (no writer in this repository)
../../Common/Chall_<gg>_<ii>.txt    (write: TMSrv only)
../../Common/Guild_<gg>_<ii>.txt    (write: TMSrv only)
../../Common/ChampionCity_<gg>_<ii>.txt  (write-only; no reader in repository)
./Chall.txt, ./Guild.txt           (CWD-relative fallbacks)
```

### 4.2 Class contract

| Member | Kind | Signature / type | Visibility | Notes |
|--------|------|------------------|------------|-------|
| `CReadFiles` | ctor | `CReadFiles()` | public | Composite loader; called as a temporary `CReadFiles::CReadFiles();` |
| `ReadSancRate` | static fn | `void (void)` | public | `:77` |
| `ReadQuestsRate` | static fn | `void (void)` | public | `:274` |
| `ReadCompRate` | static fn | `void (void)` | public | `:375` |
| `ReadMobMerc` | static fn | `void (void)` | public | `:591`; never called |
| `ReadAdmin` | static fn | `void (void)` | public | `:640` |
| `ReadMacblock` | static fn | `void (void)` | public | `:678` |
| `ReadChallanger` | static fn | `void (void)` | public | `:717` |
| `WriteChallanger` | static fn | `void (void)` | public | `:743` |
| `ReadGuild` | static fn | `void (void)` | public | `:772` |
| `WriteGuild` | static fn | `void (void)` | public | `:827` |
| `QuestExp` | static data | `int[5][2]` | public | initialised `:48-55` |
| `QuestCoin` | static data | `int[5]` | public | initialised `:46` |
| `QuestLevel` | static data | `int[5][4]` | public | initialised `:57-64` |
| `SANC_RATE_PATH` .. `GUILDCHAMP_PATH` | static const data | `const char* const` | private | nine constants, `:35-43` |

Design observations about the contract itself: every function is `void` and takes no arguments, so **there is no way for a caller to know whether a load or a write succeeded**. All state transfer happens through globals declared in other headers (`Server.h`, `Basedef.h`), so the class has no data members and cannot be instantiated meaningfully - its constructor is an anti-pattern used as a free function group. The private path constants are the only encapsulated knowledge in the component, and they are `const char* const` (pointer and pointee both const) which is correct but unusual for a decompiled codebase and gives no compile-time length information.

### 4.3 Method inventory and size/complexity metrics

| Method | Lines | Decision points | `strcmp` | `MessageBoxA` | `fscanf`/`fprintf` | External globals written |
|--------|-------|-----------------|----------|---------------|--------------------|--------------------------|
| `CReadFiles` (ctor) | 10 | 0 | 0 | 0 | 0 | (indirect) |
| `ReadSancRate` | 196 | 32 | 13 | 17 | 0 | `g_pSancRate`, `g_pSancGrade` |
| `ReadQuestsRate` | 100 | 12 | 3 | 7 | 0 | `QuestExp`, `QuestCoin`, `QuestLevel` |
| `ReadCompRate` | 215 | 56 | 46 | 8 | 0 | 11 arrays/scalars |
| `ReadMobMerc` | 48 | 8 | 0 | 1 | 0 | `pMobMerc` (via delegate) |
| `ReadAdmin` | 37 | 6 | 0 | 0 | 0 | `pAdminIP` |
| `ReadMacblock` | 38 | 6 | 0 | 0 | 0 | `pMac` |
| `ReadChallanger` | 25 | 3 | 0 | 0 | 1 | `pChallangerMoney` |
| `WriteChallanger` | 28 | 4 | 0 | 0 | 1 | (file) |
| `ReadGuild` | 54 | 10 | 0 | 0 | 6 | `GuildCounter`, `g_pGuildZone` |
| `WriteGuild` | 78 | 9 | 0 | 0 | 10 | (2 files) |

Method count by behaviour: 7 composite-loaders, 2 pure writers, 1 dead loader. Longest method 215 lines (`ReadCompRate`), highest branching 56 decisions / 46 string comparisons in the same method, deepest nesting 4 levels (`ReadCompRate` -> group -> sub-group -> bound check -> assign). There is no shared tokenising helper: each of the three rate parsers re-implements the `fgets`/`sscanf`/`atoi`/`_strupr` sequence with its own array sizes (3, 6 and 3 token buffers respectively).

---

## 5. Dependency Analysis

### 5.1 Internal dependency graph

```
Callers -> CReadFiles -> Collaborators -> Globals consumed

Server.cpp:3627  --(ctor)--> CReadFiles::CReadFiles
                                 |-- ReadSancRate   --> MessageBoxA, fopen/fgets/sscanf/atoi/_strupr --> g_pSancRate, g_pSancGrade
                                 |-- ReadQuestsRate --> MessageBoxA, fopen/fgets/sscanf/atoi/_strupr --> QuestExp/QuestCoin/QuestLevel
                                 |-- ReadCompRate   --> MessageBoxA, fopen/fgets/sscanf/atoi/_strupr --> 11 globals
                                 |-- ReadAdmin      --> fopen/fgets/sscanf/sprintf           --> pAdminIP
                                 |-- ReadMacblock   --> fopen/fgets/sscanf/sprintf           --> pMac
                                 |-- ReadChallanger --> sprintf, fopen/fscanf, Log            --> pChallangerMoney
                                 '-- ReadGuild      --> sprintf, fopen/fscanf, Log            --> GuildCounter, g_pGuildZone

Server.cpp:4102  --(ReadGuild)----------------------------^
imple.cpp:846    --(ReadGuild)----------------------------^
imple.cpp:255-257--(ReadSancRate, ReadQuestsRate, ReadCompRate)
imple.cpp:1668   --(ReadAdmin)
ProcessSecMinTimer.cpp:128 --(ReadMacblock)

Server.cpp:2565  --(WriteChallanger)--> sprintf, fopen/fprintf, Log --> Chall_*.txt
Server.cpp:6560  --(WriteChallanger)
Server.cpp:2566, 4130 ; imple.cpp:1062, 1077 ; _MSG_Buy.cpp:231 ;
_MSG_MessageChat.cpp:94 ; _MSG_MessageWhisper.cpp:263  --(WriteGuild)
      --> sprintf/fopen/fprintf/fclose, Log, BASE_GetGuildName
      --> Guild_*.txt, ChampionCity_*.txt

ReadMobMerc (no caller) --> MessageBoxA, ParseMobMercString (Server.cpp:9286) --> pMobMerc
```

### 5.2 Internal dependencies in detail

| Dependency | Direction | Contract used | Coupling kind | Evidence |
|------------|-----------|---------------|---------------|----------|
| `Log(char*, char*, unsigned)` | efferent | Writes a timestamped line to `fLogFile` and to the window title | Function call, process-global side effect | `Server.cpp:6701-6727`; called at `CReadFiles.cpp:732,758,789,842,882` |
| `ParseMobMercString(int, char*)` | efferent | Parses one MobMerc body line into `pMobMerc[Num]` | Function call + shared global | `Server.cpp:9286-9376`; called at `CReadFiles.cpp:631` |
| `BASE_GetGuildName(int, unsigned short, char*)` | efferent | Resolves guild id -> display name (12 bytes max) | Function call, cross-module (Basedef) | `Basedef.cpp:1314-1355`; called at `CReadFiles.cpp:894` |
| `hWndMain` | efferent | Owner window for modal dialogs | Shared global (`HWND`) | `Server.cpp:254`; used at `:85,113,...,597` |
| `temp` | efferent | 4096-byte scratch line buffer shared by the whole server | Shared mutable global | `Server.cpp:277`, `Server.h:382`; used at `:91,288,389` |
| `ServerGroup`, `ServerIndex` | efferent | Filename fields | Shared globals | `Server.cpp:50,52`; used at `:721,747,778,831,876` |
| `ValidGuild` | efferent | Loop bound for challenge records | Shared global (constant 5 in practice) | `Server.cpp:55`; used at `:737,763` |
| `g_pGuildZone`, `GuildCounter` | efferent+afferent | Guild-zone state and id allocator | Bidirectional shared state | `Server.cpp:490`, `Server.h:417`; read/written `:774-819`, `:847-871` |
| `pChallangerMoney`, `pAdminIP`, `pMac`, `pMobMerc` | efferent | Target arrays | Shared globals | `Server.h:304,318,65,400` |
| `g_pSancRate` ... `g_pAnctChance` (11 objects) | efferent | Tuning tables | Shared globals, cross-module (Basedef) | `Basedef.h:2482-2493` |
| `MessageBoxA` | efferent | 34 modal dialogs total (33 in loaders, 1 in MobMerc) | Win32 API | `:85,113,119,130,136,147,153,164,175,186,197,208,219,230,241,252,263,282,315,321,333,339,350,361,383,411,441,453,465,477,489,573,597` |

### 5.3 External dependencies

| Dependency | Type | Version/notes | Purpose | Failure behaviour |
|------------|------|---------------|---------|-------------------|
| MSVCRT (`fopen`,`fgets`,`fscanf`,`fprintf`,`fclose`,`sscanf`,`sprintf`,`atoi`,`memset`,`_strupr`) | C runtime | Static/dynamic per configuration (`MultiThreadedDebug` / `MultiThreadedDebugDLL`, `TMSrv.vcxproj:62,80`) | File and string handling | No error handling; return values mostly ignored |
| `Windows.h` / `MessageBoxA` / `SetWindowTextA` (via `Log`) | Win32 API | `PlatformToolset v143`, `CharacterSet MultiByte` | Modal error reporting, log rendering | Blocks caller until dismissed |
| `time.h`, `math.h`, `io.h`, `fcntl.h`, `errno.h` | C runtime headers | - | Included but unused by this TU (no `time()`, `rand()`, `_open()` calls) | n/a |
| `../../Common/Settings/SancRate.txt` | Flat file | Not in repo | Sanctification success tables | `MessageBoxA` + defaults |
| `../../Common/Settings/QuestsRate.txt` | Flat file | Not in repo | Quest exp/coin/level tables | `MessageBoxA` + defaults |
| `../../Common/Settings/CompRate.txt` | Flat file | Not in repo | Combine/refine chances (11 globals) | `MessageBoxA` + defaults |
| `../../Common/Settings/MobMerc.txt` | Flat file | Not in repo | Merchant stock definitions | `MessageBoxA` + inert arrays |
| `../../DBSRV/Run/Admin.txt` | Flat file, shared with DBSrv | Written by TMSrv `+npko` and by DBSrv's own tooling | Admin IP allow-list | Silent return |
| `../../DBSRV/Run/Mac.txt` | Flat file, external writer | No writer in this repository | MAC block-list | Silent return |
| `../../Common/Chall_<gg>_<ii>.txt` | Flat file, TMSrv-owned | Round-trip | Challenge ledger | `Log` only |
| `../../Common/Guild_<gg>_<ii>.txt` | Flat file, TMSrv-owned | Round-trip | Guild zone/war state | `Log` only |
| `../../Common/ChampionCity_<gg>_<ii>.txt` | Flat file, TMSrv-owned, write-only | No reader in repo | City champion projection for external tools | `Log` only (after primary file already written) |
| `./Chall.txt`, `./Guild.txt` | Flat file fallbacks | CWD-relative | Single-server fallback | `Log` only |

### 5.4 Compile-time dependency notes

`CReadFiles.h` includes only `<stdio.h>` and `../Basedef.h`, but it is included by `Server.cpp:38`, `ProcessClientMessage.h:41`, `CCastleZakum.cpp:38` and `CWarTower.cpp:38`. Because `ProcessClientMessage.h` is included by most `_MSG_*.cpp` handlers, **every client-message translation unit transitively recompiles when `CReadFiles.h` changes**. Two of the four includers (`CCastleZakum.cpp`, `CWarTower.cpp`) make no use of the class at all - stale includes left by the decompilation.

`CReadFiles.cpp` includes three headers it does not use: `../CPSock.h` (no `CPSock` symbol referenced), `../ItemEffect.h` (no effect constant referenced) and `Language.h` (all operator-facing text is hardcoded Portuguese, never `g_pMessageStringTable`). This is the one place in TMSrv where user-facing strings bypass the localization table used everywhere else.

---

## 6. Afferent and Efferent Coupling

Because the component is a single C++ class with static members, the coupling census is taken at member granularity (C++ classes/functions as the component unit) and at translation-unit granularity for the file contracts.

### 6.1 Afferent coupling (who depends on this component)

| Component (member) | Afferent (call sites) | Distinct dependent TUs | Critical | Notes |
|--------------------|-----------------------|------------------------|----------|-------|
| `CReadFiles::CReadFiles` | 1 | 1 (`Server.cpp`) | High | Single boot-time composition point |
| `ReadSancRate` | 2 | 2 (`Server.cpp` ctor, `imple.cpp:255`) | Medium | GM reload path exists |
| `ReadQuestsRate` | 2 | 2 (`Server.cpp` ctor, `imple.cpp:256`) | Medium | - |
| `ReadCompRate` | 2 | 2 (`Server.cpp` ctor, `imple.cpp:257`) | High | Feeds 11 gameplay globals |
| `ReadMobMerc` | 0 | 0 | Low | Dead code (Section 1.3.1) |
| `ReadAdmin` | 2 | 2 (`Server.cpp` ctor, `imple.cpp:1668`) | Low | No enforcement consumer in TMSrv |
| `ReadMacblock` | 2 | 2 (`Server.cpp` ctor, `ProcessSecMinTimer.cpp:128`) | High | Security + 2 Hz I/O |
| `ReadChallanger` | 1 | 1 (`Server.cpp` ctor) | Medium | No hot-reload |
| `WriteChallanger` | 2 | 1 (`Server.cpp:2565, 6560`) | Medium | Ledger durability |
| `ReadGuild` | 3 | 3 (`Server.cpp:4102`, `imple.cpp:846`, ctor) | High | Overwrites live war state |
| `WriteGuild` | 7 | 6 (`Server.cpp:2566,4130`; `imple.cpp:1062,1077`; `_MSG_Buy.cpp:231`; `_MSG_MessageChat.cpp:94`; `_MSG_MessageWhisper.cpp:263`) | High | Most-used member; two files per call |
| `QuestExp` / `QuestCoin` / `QuestLevel` | 6 | 1 (`_MSG_UseItem.cpp`) | Medium | Public static data, read-only use |
| **Total (members)** | **30 call edges** | **7 TUs** | | plus 3 header-only includers |

Header-level fan-in (compile-time): `CReadFiles.h` is included by 4 files directly (`Server.cpp`, `ProcessClientMessage.h`, `CCastleZakum.cpp`, `CWarTower.cpp`) and, through `ProcessClientMessage.h`, by the whole `_MSG_*.cpp` family.

### 6.2 Efferent coupling (what this component depends on)

| Target | Kind | Count |
|--------|------|-------|
| C runtime file/string functions | Function calls | 10 distinct (`fopen`, `fgets`, `fscanf`, `fprintf`, `fclose`, `sscanf`, `sprintf`, `atoi`, `memset`, `_strupr`) |
| Win32 `MessageBoxA` | Function call | 33 sites in 4 loaders |
| `Log` (Server.cpp) | Function call | 6 sites |
| `ParseMobMercString` (Server.cpp) | Function call | 1 site |
| `BASE_GetGuildName` (Basedef.cpp) | Function call | 1 site |
| Shared globals read | Data dependency | 6 (`ServerGroup`, `ServerIndex`, `ValidGuild`, `hWndMain`, `temp`, `g_pGuildZone` for write projection) |
| Shared globals written | Data dependency | 20 objects (11 Basedef tables + `QuestExp/QuestCoin/QuestLevel` + `pAdminIP`, `pMac`, `pMobMerc`, `pChallangerMoney`, `GuildCounter`, `g_pGuildZone`) |
| File contracts | I/O | 9 paths + 2 CWD fallbacks |

### 6.3 Coupling assessment

| Dimension | Observation |
|-----------|-------------|
| Cohesion | **Functional for a loader, low within methods.** All public functions are "load a config file"; but there is no shared tokeniser, no shared error policy, and three different failure semantics (dialog / silent / log+fallback). The class is a namespace, not an abstraction. |
| Afferent/efferent ratio | 30 afferent call edges vs ~17 efferent call edges + 20 written globals. Efferent *data* coupling dominates: the component's real interface is the set of globals it mutates, not its six functions. |
| Instability | I = Ce / (Ca + Ce) is low (~0.4 counting data deps), consistent with a leaf adapter. Not a stability problem per se - but it means every contract change ripples outward into 20 globals with no encapsulation. |
| Data coupling | **Extreme.** Two shared scratch buffers (`temp`, `hWndMain`) and 20 globals; no return values anywhere, so correctness is unobservable to callers. |
| Control coupling | Minimal - no flags or modes are passed; behaviour is decided entirely by file content and compile-time path constants. |
| Content coupling | Present via `temp` (CReadFiles writes the same buffer `Server.cpp` uses for `sprintf`-based logging) and via `memset(dir, 0, ...)`/`sprintf` on locals (benign). The `temp` sharing is a genuine re-entrancy hazard if a loader is ever invoked from inside a `Log`-heavy path. |
| Temporal coupling | Explicit and load-bearing: the constructor must run after `Reboot()`/`BASE_InitializeBaseDef` and before `GuildZoneReport()`/`StartListen()` (`Server.cpp:3505-3636`). Nothing enforces that order at compile time. |
| Contract coupling to other servers | `Admin.txt` and `Mac.txt` live under `../../DBSRV/Run/` and are read by two processes with independent, non-coordinated loaders (`Code/DBSrv/Server.cpp:353-391` vs `CReadFiles.cpp:640-715`); `Mac.txt` has no writer in either. |

---

## 7. Integration Points

**No endpoints are exposed.** The component contains no network protocol handling, no message dispatch, no RPC surface and no command registration table: it is a synchronous, in-process flat-file adapter. Its functions are not reachable from a client packet - they are reachable only from the server's own composition root, the GM command interpreter (`imple.cpp`, itself reachable only through an authenticated `Imple` channel), the 500 ms timer, and gameplay handlers that persist state.

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| `../../Common/Settings/SancRate.txt` | File (read, in-process) | Sanctification tables | `fopen("r")` + `fgets` + `sscanf` | Whitespace-delimited text, `<KEY> <idx> <val>`, UTF-8 key bytes for `ÂMAGO` | Modal `MessageBoxA` per fault; record skipped; defaults retained |
| `../../Common/Settings/QuestsRate.txt` | File (read) | Quest reward/level tables | Same | `<EXP|COIN|LEVEL> <tier> <n...>` (4 or 6 tokens) | Modal `MessageBoxA` per fault; record skipped |
| `../../Common/Settings/CompRate.txt` | File (read) | Combine/refine chances | Same | `<GROUP> <SUBKEY> <val 0..100>`, sub-key case-sensitive | Modal `MessageBoxA` per fault; unknown keys ignored |
| `../../Common/Settings/MobMerc.txt` | File (read, **unused**) | Merchant stock | Same + section parser | `#` sections, `/` comments, `KEY: value` | Modal `MessageBoxA` on open failure; parser return value ignored by caller except zero |
| `../../DBSRV/Run/Admin.txt` | File (read + rewrite) | Admin IP allow-list | Same | `<idx> <a>.<b>.<c>.<d>` | Silent return on open failure; out-of-range idx skipped |
| `../../DBSRV/Run/Mac.txt` | File (read, ~2 Hz) | MAC block-list | Same | `<idx> <a>.<b>.<c>.<d>` | Silent return; per-entry overwrite without clearing |
| `../../Common/Chall_<gg>_<ii>.txt` | File (read + write) | Per-zone challenge ledger | `fopen("rt"/"wt")`, `fscanf`/`fprintf` | 5 integers, one per line (`"%d \n"`), no header | `Log()` on double open failure; `fscanf` result unchecked |
| `../../Common/Guild_<gg>_<ii>.txt` | File (read + write) | Guild zone/war state | `fopen("rb"/"wt")`, `fscanf`/`fprintf` | 26 integers in 5 newline-separated groups | `Log()` on open failure; `CityTax` clamped; other fields unvalidated |
| `../../Common/ChampionCity_<gg>_<ii>.txt` | File (write only) | City champion projection for external tools | `fopen("wt")`, `fprintf` | `<CityName> <GuildName> b01NNNNNN.bmp` x5 | `Log()` only; primary file already committed |
| `./Chall.txt`, `./Guild.txt` | File (fallback) | Single-server fallback | Same | Same as above | `Log()` when both opens fail |
| `Log()` (in-process) | Function call | Report persistent failures | Direct call | `"err,<text>"`, `"-system"`, 0 | Non-blocking; writes `fLogFile` + window title |
| `MessageBoxA()` (Win32) | OS dialog | Report load failures | Modal Win32 | UTF-8 bytes via ANSI API (mojibake under CP1252) | Blocks caller indefinitely |
| `DBSrv` (cross-process, indirect) | Other process | Shared `Admin.txt` ownership | File system | Text | No coordination, no locking, no reload protocol |
| External MAC tooling (indirect) | Other process | Produces `Mac.txt` | File system | Text | None in this repository |

Ownership boundary notes: `Chall_*`, `Guild_*` and `ChampionCity_*` are written **exclusively** by this component (verified repository-wide: no other writer). `Admin.txt` has two writers (TMSrv `+npko` at `imple.cpp:1670-1699`, and DBSrv's own tooling) and two readers. `Mac.txt` has no writer anywhere in the repository, so its producer is out-of-tree.

---

## 8. Design Patterns & Architecture

| Pattern / decision | Implementation | Location | Purpose and assessment |
|--------------------|----------------|----------|------------------------|
| Static utility class / namespace surrogate | `class CReadFiles` with only `static` members and one ctor | `CReadFiles.h:25-56` | Groups related free functions under a name. The ctor-as-composite is an anti-pattern: it hides sequencing inside a constructor-like call and returns no status. |
| Composite / façade loader | ctor calls 7 loaders in fixed order | `CReadFiles.cpp:66-75` | One call site for boot (`Server.cpp:3627`). The façade is incomplete: `ReadMobMerc` is omitted, so the "load everything" contract is false. |
| Table-driven dispatch (long if/else-if chain) | 13 / 3 / 46 `strcmp` chains | `:109-268`, `:311-369`, `:407-585` | Chosen over a key->slot table, probably a decompilation artefact. Produces 215-line methods, duplicated validation, and no way to enumerate valid keys. |
| Key-offset map (implicit) | Sub-key literals bound to fixed array indices (`ITEM_12_REF_0..9` -> `[1..10]`) | `:497-527` | The mapping exists only in code. A table of `{ "ITEM_12_REF_0", &g_pItemSancRate12[0] }` would be equivalent and enumerable; the current form makes the contract discoverable only by reading 215 lines. |
| Positional record layout (implicit schema) | 26 integers read in fixed order | `:799-819` | `Guild_*.txt` has no keys, no header, no version. Any reorder silently corrupts state. |
| Section-based format (only for MobMerc) | `#` section counter + `KEY:` body | `:608-636` | The only self-describing format in the component - and the only one that is dead code. |
| Fallback chain (two-tier path resolution) | per-server path, then bare CWD filename | `:727-728`, `:753-754`, `:784-785`, `:837-838` | Resilience by substitution; hides which file was used and where writes land. |
| Fail-open with operator notification | `MessageBoxA` then `return`, defaults intact | `:83-87` etc. | Keeps the server bootable with no config; the cost is a blocking modal in the boot path. |
| Silent degradation | `if (fp == NULL) return;` | `:646-647`, `:684-685` | Chosen for the two security-adjacent files - the opposite of fail-safe. |
| Write-through persistence (no cache/dirty flag) | every mutation immediately calls `WriteGuild` | 6 call sites (BR-34) | Simplest consistency model; amplified I/O and no transaction boundary across the two files. |
| Truncate-and-rewrite serialisation | `fopen(..., "wt")` + `fprintf` | `:751`, `:835`, `:878` | No temp-file + rename, so a crash mid-write leaves a partial file which the readers accept (BR-28). |
| Derived-file projection | `ChampionCity_*` regenerated from `g_pGuildZone` on every guild write | `:875-901` | Denormalised read model for external tools; consistency is only as good as the last write. |
| Shared global state as the data bus | 20 globals written, zero return values | throughout | The de facto architecture: the component is a "writer of globals" rather than a provider of a typed configuration object. |
| Magic numbers extracted as named constants | `SANC_RATE_PATH`..`GUILDCHAMP_PATH`, `MAX_ADMIN`/`MAX_MAC`/`MAX_MOB_MERC` | `:35-43`, `Basedef.h:58,152,158` | Good. But domain limits inside the parsers (12, 100, 5, 400, 9, 20, 10, 2,000,000,000, `"b01"`) are inline literals. |
| Copy-paste derivation | `ReadAdmin` and `ReadMacblock` are the same 37 lines with a different target | `:640-676` vs `:678-715`; also `ReadSancRate`/`ReadCompRate` gate blocks | The duplication is the reason the two Admin/Mac functions cannot diverge safely; the `CityTax`/tax-range conflict is a *non*-duplicated divergence. |
| Encoding-as-interface | UTF-8 key literals in a MultiByte build | `:143` (`C3 82`), `:85,113,...` | Accidental architectural decision: the file contract now depends on source encoding (`030a0ab`). |

Architectural assessment in one line: the component is an **importer/exporter at the edge of the process**, with no domain model of its own, whose entire contract is expressed as (a) magic path strings, (b) positional text formats, and (c) a fixed list of global symbols it is allowed to overwrite. That design maximises boot-time simplicity and minimises testability - there is no seam at which a fake file system or a fake `MessageBoxA` could be injected.

---

## 9. Technical Debt & Risks

### 9.1 Risk register

| Risk level | Component area | Issue | Impact | Evidence |
|------------|----------------|-------|--------|----------|
| Critical | `ReadMobMerc` | Dead code with two out-of-bounds write paths (`Num == -1` before first `#`; `Num == 128` after 128 headers) | If ever wired up: heap/static corruption of `pMobMerc` | `CReadFiles.cpp:602,621-626,631` |
| Critical | `ReadGuild` / `WriteGuild` | `GuildCounter = 0` before open; failed read + any later write persists 0 | Permanent loss of guild-id allocator; guild creation disabled; existing zone data overwritten | `:774,782,823-824`, `Server.cpp:4130`, `_MSG_MessageWhisper.cpp:204-208` |
| High | `ReadSancRate` | `ÂMAGO` branch writes row 0, overwriting Paladin rates; Amagos row not configurable | Silent corruption of sanctification balance; operator intent inverted | `:157` vs `Basedef.cpp:65,2234` |
| High | `ReadSancRate` / `ReadCompRate` / `ReadQuestsRate` | Blank/short lines parse uninitialised stack buffers (`atoi`, `_strupr`, `sscanf`) | Undefined behaviour: non-deterministic values, potential out-of-bounds scan of `str1` | `:96-107`, `:293-309`, `:394-405`, `Server.cpp:9311-9330` |
| High | `ReadGuild` vs `_MSG_MessageChat` | City tax read clamp `0..20` vs write acceptance `0..30` | Guild masters silently lose 21-30% tax settings on restart/reload | `:811-812` vs `_MSG_MessageChat.cpp:76` |
| High | `ProcessSecTimer` | Inverted modulo guard reloads the MAC list 119/120 ticks (~2 Hz) on the message-pump thread | Continuous synchronous disk I/O; unsynchronised mutation of a security table; window for torn reads | `ProcessSecMinTimer.cpp:127-128`, `Server.cpp:3631` |
| High | All loaders | `MessageBoxA` in the boot path and per invalid record | Server blocks on human presence; 50 bad lines = 50 dialogs; unusable on headless/hosted deployments | `:85,113,...,597`, called from `Server.cpp:3627` |
| High | `WriteGuild` / `WriteChallanger` | Truncate-and-rewrite with no temp file, no `ferror`, no return status, no backup | Partial write after crash/disk-full is accepted by the reader as valid state | `:751,835,878`, `:847-871`, `:898` |
| High | `WriteGuild` | Two sequential file writes; second failure leaves primary committed | `Guild_*` and `ChampionCity_*` can permanently disagree | `:872-885` |
| Medium | `ReadAdmin` | Loaded `pAdminIP` has no consumer in TMSrv; enforcement is in DBSrv reading the same file with its own parser | Divergent admin allow-lists between processes; TMSrv load is wasted work | `:640-676`; no read of `pAdminIP` in TMSrv |
| Medium | `ReadAdmin` / `ReadMacblock` | Octets `a..d` unvalidated before shifting/packing | Out-of-range octets produce unmatchable IPs / inert MAC entries, silently | `:667,708-711` |
| Medium | `ReadAdmin` / `ReadMacblock` | Dots destroyed in-place over a fixed 255-byte loop on a 256-byte buffer | Inspects uninitialised bytes; any future consumer cannot see the original text | `:661-663,699-701` |
| Medium | `ReadChallanger` | `fscanf` return ignored; no range checks; `pChallangerMoney[6]` with 5 used | Truncated file silently retains stale bids; negative bids allow unlimited challenges | `:737-738`, `Server.h:304` |
| Medium | `ReadGuild` | `CityTax`/`Clan`/`Victory` not pre-zeroed; only `ChargeGuild`/`ChallangeGuild` are | Truncated file mixes new and stale values undetectably | `:793-797` |
| Medium | `WriteGuild` | `GuildName[128]` uninitialised; `BASE_GetGuildName` copies 12 bytes without guaranteed NUL | Possible read past initialised stack in `fprintf("%s")` | `:891-898`, `Basedef.cpp:1332-1334` |
| Medium | `ReadMacblock` | `pMac` entries overwritten one-by-one with no clearing | Torn read: a login may see a mixed old/new block-list | `:708-711` |
| Medium | `CReadFiles.cpp` | `temp` (4096 B, `Server.h:382`) shared with `Server.cpp` logging; `fgets` limited to 1023 | Cross-module buffer aliasing; long lines split into pseudo-records | `:91,288,389`, `Server.cpp:277` |
| Medium | All | No status/return values; callers cannot detect failure | `+reloadfile`, `+reloadguild`, menu actions and shutdown writes cannot report success | entire header |
| Medium | `ReadSancRate`/`ReadQuestsRate`/`ReadCompRate` | `int val1 = atoi(str1);` computed and never used in all three parsers | Dead computation; sign of incomplete decompilation; warning-suppression noise | `:103,302,401` |
| Medium | `ReadCompRate` | Single 215-line method, 46 `strcmp`, 56 decisions, validation duplicated 7x | High change risk; one domain limit change needs 7 edits (already inconsistent for additive slots) | `:375-589` |
| Low | `ReadQuestsRate` | `>= 2000000000` (EXP) vs `> 2000000000` (COIN) inconsistency; `atoi` overflow undefined | Boundary value 2e9 accepted for coin, rejected for exp; overflow behaviour implementation-defined | `:319,337` |
| Low | `ReadSancRate`/`ReadCompRate` | `// char str4[128];` commented-out declarations | Decompilation residue | `:99,397` |
| Low | `CReadFiles.cpp` | Unused includes (`CPSock.h`, `ItemEffect.h`, `Language.h`); unused `<time.h>`, `<math.h>`, `<io.h>` | Compile-time coupling, slower builds; localization bypass | `:19-33` |
| Low | `CCastleZakum.cpp`, `CWarTower.cpp` | Include `CReadFiles.h` with no usage | Spurious recompilation fan-out | `:38` in both |
| Low | `ReadMobMerc` | `memset(tp, 0, 1023)` on a 1024-byte buffer (last byte untouched) | Cosmetic; `fgets` terminates anyway | `:604-606` |
| Low | `WriteGuild` | Hardcoded city-name literals and `"b01%06d.bmp"` mark format | Localisation/tooling coupling in code | `:887,896` |
| Low | All writers | Output formats have no version and no header | Any future field addition breaks positional readers silently | `:847-871`, `:763-767` |
| Low | Documentation | No format documentation, no sample files in the repository | Every contract must be reverse-engineered from code (as done in Section 3) | no `.txt` under `Code/` |

### 9.2 Code smells detected

| Smell | Occurrences | Example |
|-------|-------------|---------|
| Duplicated code (near-identical methods) | 2 pairs | `ReadAdmin` vs `ReadMacblock` (`:640-676` / `:678-715`); `ReadSancRate` vs `ReadCompRate` gate blocks |
| Long method | 3 | `ReadCompRate` 215 lines; `ReadSancRate` 196; `ReadQuestsRate` 100 |
| Long parameter-free void function hiding 20 outputs | 10 | all `void (void)` members |
| Magic numbers | 20+ | `12`, `100`, `5`, `400`, `9`, `20`, `10`, `2`, `2000000000`, `127`, `255`, `1023`, `-1`, `47`, `13`, `"b01"`, `6` (digits) |
| Commented-out code | 4 | `:99`, `:397`, `:258` (`// CEncampment::ReadCamp();` in caller) |
| Dead code | 1 method | `ReadMobMerc` (48 lines) |
| Dead stores | 6+ | `val1` (`:103,302,401`), `int pars` result discarded except `== 0` continue (`:631-634`) |
| Unused includes | 6 | `:28,29,33` and `:22-25` |
| Inconsistent error policy | 3 policies | dialog / silent / log+fallback |
| Stringly-typed dispatch | 46+13+3 comparisons | Section 8 |
| Uninitialised locals | 3 families | token buffers, `GuildName`, `dir` is initialised only by `sprintf` |
| Fixed-bound loop over a string | 2 | `for (i = 0; i < 255; i++)` on `char[256]` |
| Ignored return values | 12+ | `fscanf` x7, `fgets` handled, `fprintf` x10, `fopen` fallbacks handled |
| Primitive obsession / global data bus | 20 globals | Section 6.2 |

---

## 10. Test Coverage Analysis

### 10.1 Result: no tests exist

| Check | Method | Result |
|-------|--------|--------|
| Test source files (`*test*`, `*Test*`, `*spec*`) anywhere in the repo | Recursive file-name scan excluding `.agents`, `.codegraph`, `.opencode`, `openspec`, `.git`, `.vs` | **0 files** |
| Test frameworks (gtest, Catch2, doctest, CppUnit, Boost.Test, GoogleMock) | Content search for framework headers/macros | **0 matches** |
| Test projects in the solution | `W2PP Code Project.sln` project list | **2 projects only**: `DBSrv`, `TMSrv` (both application `.vcxproj`) |
| Test build configurations | `TMSrv.vcxproj` configurations | `Debug|Win32`, `Release|Win32`, `Debug|x64`, `Release|x64` - no test target |
| Test/CI scripts | `.yml`, `.yaml`, `CMakeLists.txt`, `Makefile`, `*.mk`, `.github/`, `appveyor*`, `.gitlab-ci*` | **0 files** |
| Fixture/sample config data | Search for `SancRate`, `QuestsRate`, `CompRate`, `MobMerc`, `Admin.txt`, `Mac.txt`, `Chall_`, `Guild_`, `ChampionCity_` as files | **0 data files**; only code references |
| Assertions coverage | Search for `assert(`, `ASSERT`, `_ASSERT`, `static_assert` | none in this component |

### 10.2 Per-function coverage assessment

| Member | Unit tests | Integration tests | Coverage | Test quality |
|--------|-----------|-------------------|----------|--------------|
| `CReadFiles` (ctor) | 0 | 0 | 0% | No verification that all 7 loaders run, no ordering test |
| `ReadSancRate` | 0 | 0 | 0% | 13 branches, 12 bound checks, 17 dialogs - all unverified. The `ÂMAGO`/row-0 defect and the encoding dependency would both be caught by a single table-driven test |
| `ReadQuestsRate` | 0 | 0 | 0% | 3 record shapes, 7 dialogs; the 4-token `LEVEL` uninitialised-read case untested |
| `ReadCompRate` | 0 | 0 | 0% | 46 key comparisons, 24 slot mappings - the highest-value target in the component; completely unverified |
| `ReadMobMerc` | 0 | 0 | 0% | Would immediately expose both out-of-bounds writes if exercised (`Num == -1`, `Num == 128`) |
| `ReadAdmin` | 0 | 0 | 0% | Little-endian packing and idx bounds unverified; no round-trip test against `imple.cpp:1697` |
| `ReadMacblock` | 0 | 0 | 0% | Security-relevant; no test that zero entries are ignored, no test of torn reload |
| `ReadChallanger` | 0 | 0 | 0% | Short-file and non-numeric-file behaviour unverified |
| `WriteChallanger` | 0 | 0 | 0% | No round-trip test (`Write` -> `Read` -> compare) |
| `ReadGuild` | 0 | 0 | 0% | 26-field positional schema, `CityTax` clamp, `GuildCounter == 0` path - all unverified |
| `WriteGuild` | 0 | 0 | 0% | Two-file output; no golden-file test of either format |
| `QuestExp`/`QuestCoin`/`QuestLevel` consumers | 0 | 0 | 0% | `_MSG_UseItem.cpp` class-split selection untested |

### 10.3 Why this is a risk here specifically

The absence of tests is not unusual for a decompiled 2015-era codebase, but this component has an unusually poor risk/effort ratio. All eleven entry points are **pure functions of a file path**: they take no arguments, they can be exercised with a temporary directory and a text file, and their observable output is a set of globals in `Basedef.h`/`Server.h`. A table-driven harness that writes a small fixture file, calls one function, and asserts on 3-10 globals would cover BR-01 through BR-34 with no mocking beyond stubbing `MessageBoxA` and `Log`. The two known defects (BR-04 row collision, BR-29 tax range) are exactly the class of defect such a test detects in one assertion.

The countervailing constraint is that the code is not testable as-is without a seam: `MessageBoxA(hWndMain, ...)` is called directly, paths are compile-time constants (not parameters), and outputs are globals with no accessor. That is an architectural property of the component, not a missing test file - and it is the reason the component will remain unverified while remaining load-bearing for 20 gameplay globals.

---

## Appendix A - Assumption & ambiguity register

| # | Item | Status | Confidence | Basis |
|---|------|--------|-----------|-------|
| A1 | `Admin.txt`/`Mac.txt` record order is `<index> <a.b.c.d>`, not IP-first | Resolved | High | `sscanf("%d %d %d %d %d", &idx, &a, ...)` at `:665,703` plus the writer `imple.cpp:1697` |
| A2 | `Guild_*.txt` field order (counter, charge, challenge, tax, clan, victory) | Resolved | High | Read `:799-819` and write `:847-871` are mutually consistent |
| A3 | `QuestExp` column 0 = mortal, column 1 = arch | Resolved | High | `_MSG_UseItem.cpp:2260` ternary on `MORTAL` |
| A4 | `g_pSancRate` row semantics (0=PO, 1=PL, 2=Amagos) | Resolved | High | Trailing comments at `Basedef.cpp:63-65` plus consumers `:2220,2234` |
| A5 | `ÂMAGO` literal bytes are UTF-8 `C3 82 4D 41 47 4F` | Resolved | High (file bytes) / Medium (compiled literal) | Byte dump of `CReadFiles.cpp`; no BOM; `CharacterSet=MultiByte`; MSVC source-charset behaviour not compiled/tested here |
| A6 | `SancRate.txt`, `QuestsRate.txt`, `CompRate.txt`, `MobMerc.txt` actual on-disk layouts | **Unverifiable** | - | No sample data exists in the repository; formats were derived exclusively from the parsers |
| A7 | `MobMerc.txt` item-line tokenisation (whether values are whitespace-separated or colon-packed) | **Ambiguous** | Low | `ParseMobMercString` re-parses a single `%s` token (`Server.cpp:9300,9322`), which cannot yield 8 values; the intended format is not recoverable from the repository. Call path is dead, so no runtime evidence exists |
| A8 | `ChampionCity_*.txt` consumer and whether the `b01` mark prefix is a hard requirement | **Unknown** | - | No reader in the repository; format inferred from the single writer at `:887-898` |
| A9 | `Mac.txt` producer (external tool identity, write cadence, locking) | **Unknown** | - | No writer in the repository; only consumed here and by `ProcessDBMessage` |
| A10 | Working directory at runtime (drives all `../../` path resolution) | **Assumed** | Medium | Paths are CWD-relative constants; the VS project does not set a debugging working directory, and `.vcxproj.user` contents were not part of the analysed scope |
| A11 | `ValidGuild` is constant 5 | Resolved | High | Initialised `Server.cpp:55`; no assignment anywhere else in the repository |
| A12 | `pChallangerMoney[6]` sixth slot is unused | Resolved | High | Loops bounded by `ValidGuild` (5): `:737,763` |
| A13 | `ReadMobMerc` was never called at any point in history | Resolved | High | `git log -S "ReadMobMerc"` returns only the initial import `489c4e0`; no call site exists in any revision of the file |
| A14 | Whether BR-04 (`ÂMAGO` -> row 0) is a decompilation artefact or an authored simplification | **Unknown** | - | Present in the initial import commit; no earlier history available |
| A15 | Whether the 33 modal dialogs are intentional operator-facing UX | **Unknown** | - | No comments, no design notes in the repository |
| A16 | `int` size assumptions for `pMac[idx].Mac[0..3]` (four 32-bit groups vs four 8-bit octets) | Resolved | High | `STRUCT_BLOCKMAC { int Mac[4]; }` at `Basedef.h:916-919`; compared with `CUser::Mac[4]` (`CUser.h:93`) |
| A17 | Endianness dependency of the Admin IP pack | Resolved | High | `:667` packs octet 0 into the low byte; `imple.cpp:1696-1697` re-reads via `unsigned char*` - consistent only on little-endian x86 |

---

*End of report. No project files were modified during this analysis.*
