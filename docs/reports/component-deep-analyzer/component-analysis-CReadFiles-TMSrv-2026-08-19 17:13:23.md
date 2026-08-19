# Component Deep Analysis Report

**Component:** CReadFiles (TMSrv)
**Project:** W2PP legacy C/C++ codebase
**Scope:** `legacy/Code/TMSrv/CReadFiles.h`, `legacy/Code/TMSrv/CReadFiles.cpp` (plus invocation/consumption sites across the TMSrv project)
**Folders ignored:** `.git`, `.opencode`
**Analysis date:** 2026-08-19 17:13:23

---

## 1. Executive Summary

`CReadFiles` is a **static utility class** in the TMSrv game server that acts as the **data/config-file loading and persistence layer** for the server process. It reads a set of human-editable plain-text configuration files (drop/combination rates, sanctification rates, quest rewards, admin IP allow-list, MAC block-list, mercenary definitions, guild state, and challenge rankings) into a set of process-wide global tables, and writes a subset of those tables back to disk.

The class exposes 11 static methods plus a constructor. All methods are `static`, hold no instance state, and operate exclusively through **shared global variables** declared in `legacy/Code/Basedef.h` and `legacy/Code/TMSrv/Server.h`. The constructor (`CReadFiles::CReadFiles()`) is invoked once during server startup from `Server.cpp:3627`, and the individual read/write methods are re-invoked at runtime through GUI menu commands, in-game administrator commands, periodic timers, and game-event handlers.

Key findings:

- **Central configuration bootstrap**: The class is the single point of load for the server's tunable economic and progression rates (sanc, combine, quest, admin, MAC, guild, challenge). Every rate table it populates (`g_pSancRate`, `g_pEhreRate`, `g_pOdinRate`, `g_pItemSancRate12`, `g_pAnctChance`, `g_pTinyBase`, `g_pShanyBase`, `g_pAilynBase`, `g_pAgathaBase`, `QuestExp`, `QuestCoin`, `QuestLevel`) is consumed by gameplay logic in `_MSG_UseItem.cpp`, `_MSG_CombineItem*.cpp`, `GetFunc.cpp`, and `Basedef.cpp`.
- **Two distinct I/O styles**: text-line parsers with token `sscanf` for rate/block-list files, and sequential `fscanf` binary/text reads for the guild and challenger state files.
- **Dead code**: `ReadMobMerc()` is declared (`CReadFiles.h:33`) and defined (`CReadFiles.cpp:591`) but **never called anywhere** in the TMSrv project. The mercenary data it would load is instead populated only via `ParseMobMercString` in `Server.cpp`.
- **No automated tests**: No test files exist anywhere in the repository, and CodeGraph confirms zero covering tests for all 11 methods. This is a legacy project with no test infrastructure.
- **Error-handling inconsistency**: three distinct failure modes coexist — blocking GUI `MessageBoxA` popups (Sanc/Quests/Comp/MobMerc), silent `return` (Admin/Mac), and `Log()` entries (Challanger/Guild).
- **Invocation bug at call site**: `ProcessSecMinTimer.cpp:127` uses `if (SecCounter % 120)` which evaluates true on **every second except** multiples of 120, causing `ReadMacblock` to run every second rather than every two minutes as the surrounding pattern implies (inverted condition).

---

## 2. Data Flow Analysis

```
1. Startup: InitApplication() calls CReadFiles::CReadFiles()      (Server.cpp:3627)
2. Constructor orchestrates initial load:
     ReadSancRate()   -> g_pSancRate[3][12], g_pSancGrade[2][5]
     ReadQuestsRate() -> QuestExp[5][2], QuestCoin[5], QuestLevel[5][4]
     ReadCompRate()   -> g_pEhreRate, g_pOdinRate, g_pItemSancRate12,
                         g_pItemSancRate12Minus, g_pAnctChance, g_pTinyBase,
                         g_pShanyBase, g_pAilynBase, g_pAgathaBase
     ReadAdmin()      -> pAdminIP[MAX_ADMIN]
     ReadMacblock()   -> pMac[MAX_MAC]
     ReadChallanger() -> pChallangerMoney[ValidGuild]
     ReadGuild()      -> GuildCounter, g_pGuildZone[5]
3. Runtime consumption (read side):
     g_pSancRate/g_pSancGrade  -> BASE_GetSuccessRate (Basedef.cpp:2220,2234),
                                  _MSG_UseItem.cpp:862 (sanctification)
     g_pEhreRate               -> _MSG_CombineItemEhre.cpp:113
     g_pOdinRate + sanc12      -> _MSG_CombineItemOdin.cpp (lines 124-512)
     g_pTinyBase/Shany/Ailyn/Agatha -> GetFunc.cpp:214,314-424,
                                  _MSG_CombineItemShany.cpp:89
     g_pAnctChance             -> GetFunc.cpp:83-95
     QuestExp/Coin/Level       -> _MSG_UseItem.cpp:2245-2277 (quest reward items)
     pAdminIP                  -> imple.cpp:1687-1696 (admin auth / npko)
     pMac                      -> ProcessDBMessage.cpp:371-374 (MAC block check)
     pMobMerc                  -> _MSG_Buy.cpp:243-255, Server.cpp:3003,
                                  ProcessSecMinTimer.cpp:1341-1348
     pChallangerMoney          -> _MSG_Challange.cpp:123, Server.cpp:2560
     GuildCounter/g_pGuildZone -> _MSG_Buy.cpp, _MSG_MessageWhisper.cpp,
                                  _MSG_MessageChat.cpp, Server.cpp, imple.cpp
4. Runtime re-load triggers:
     In-game admin cmd "+reloadfile"  -> imple.cpp:255-257 (Sanc/Quests/Comp)
     In-game admin cmd "reloadguild"  -> imple.cpp:846 (ReadGuild)
     In-game admin cmd "npko"         -> imple.cpp:1668 (ReadAdmin)
     GUI menu IDC_READGUILD           -> Server.cpp:4102 (ReadGuild)
     Per-second timer                 -> ProcessSecMinTimer.cpp:127 (ReadMacblock)
5. Persistence (write side):
     WriteChallanger() -> Chall_XX_XX.txt (guild challenge prize money)
     WriteGuild()      -> Guild_XX_XX.txt + ChampionCity_XX_XX.txt
     Triggers: Server.cpp:2565-2566,6560,4130 (WM_CLOSE); imple.cpp:1062,1077;
               _MSG_Buy.cpp:231; _MSG_MessageChat.cpp:94; _MSG_MessageWhisper.cpp:263
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Sanc rate index must be in `[0,12)` | CReadFiles.cpp:111,128,145 |
| Validation | Sanc rate value must be in `[0,100]` | CReadFiles.cpp:117,134,151 |
| Validation | Sanc grade index must be in `[0,9]` | CReadFiles.cpp:162-267 |
| Validation | Quest rate index must be in `[0,5)` | CReadFiles.cpp:313,331,348 |
| Validation | Quest EXP/COIN must be `< 2,000,000,000` | CReadFiles.cpp:319,337 |
| Validation | Quest level bounds must be in `[0,400)` | CReadFiles.cpp:359 |
| Validation | Combine rate value must be in `[0,100]` | CReadFiles.cpp:409,439,451,463,475,489,571 |
| Validation | Admin/MAC record index must be in range | CReadFiles.cpp:669,705 |
| Business Logic | Sanc "ÂMAGO" maps to same row as "PO" (row 0) | CReadFiles.cpp:143-158 |
| Business Logic | Guild CityTax clamped to default 10 if outside `[0,20]` | CReadFiles.cpp:811-812 |
| Business Logic | Default quest tables hardcoded in source | CReadFiles.cpp:46-64 |
| File/Format | Per-server file naming via `ServerGroup`/`ServerIndex` | CReadFiles.cpp:721,747,778,831,876 |
| Fallback | Fallback to generic `Chall.txt` / `Guild.txt` if server file missing | CReadFiles.cpp:727-728,784-785,753-754,837-838 |
| Initialization | Guild state zeroed before load | CReadFiles.cpp:774,793-797 |

### Detailed breakdown of the business rules

---

### Business Rule: Sanctification rate and grade configuration loading (ReadSancRate)

**Overview**:
`ReadSancRate()` reads `../../Common/Settings/SancRate.txt`, a three-token-per-line text file that configures the success rates for item sanctification (refinement) and per-grade bonuses. The loaded values are written into the global tables `g_pSancRate[3][12]` and `g_pSancGrade[2][5]` (declared in `Basedef.h:2482-2483`). These tables are subsequently consumed by `BASE_GetSuccessRate` (`Basedef.cpp:2220,2234`) and by the item-refinement flow in `_MSG_UseItem.cpp:862`, which determines whether a sanctification attempt succeeds and by how many levels the item advances.

**Detailed description**:
The file format is line-oriented: each line carries up to three whitespace-separated tokens parsed with `sscanf(temp, "%s %s %s", str1, str2, str3)` (`CReadFiles.cpp:101`). The first token is the row key, the second is the index, and the third is the value. The key is uppercased via `_strupr` before comparison (`CReadFiles.cpp:107`), making matching case-insensitive for the ASCII keys. Three rate keys are recognized: `PO` writes into `g_pSancRate[0][index]`, `PL` writes into `g_pSancRate[1][index]`, and `ÂMAGO` also writes into `g_pSancRate[0][index]` (the same row as `PO`) — a duplication that may be an intentional alias or a defect, and is flagged as ambiguous.

Both the index and value are validated before any write. The index must satisfy `0 <= index < 12` (matching the 12-column width of `g_pSancRate[3][12]`); otherwise a GUI message box "Índice inválido. (1)" is shown and the line is skipped via `continue`. The value must satisfy `0 <= value <= 100` (a percentage); otherwise "Índice inválido. (2)" is shown and the line is skipped (`CReadFiles.cpp:111-121`). Grade keys `PO_A` through `PO_E` and `PL_A` through `PL_E` populate `g_pSancGrade[0][0..4]` and `g_pSancGrade[1][0..4]` respectively; for these keys only the index is range-checked (`0..9`), since the value slot is not used in the assignment. This rule is central to the game economy because refinement success probability directly gates item progression.

**Rule workflow**:
1. Open `SancRate.txt`; if it fails to open, show a blocking `MessageBoxA` and return (`CReadFiles.cpp:81-87`).
2. Read the file line-by-line with `fgets` into the shared `temp` buffer (limit 1023 bytes, safe against the 4096-byte buffer).
3. Tokenize each line; uppercase the key.
4. Dispatch on the key (`PO`/`PL`/`ÂMAGO`/`PO_A..E`/`PL_A..E`).
5. Validate index and value ranges; on failure show a message box and skip the line.
6. On success, write the value into the corresponding global table slot.
7. On EOF, close the file.

---

### Business Rule: Quest reward configuration loading (ReadQuestsRate)

**Overview**:
`ReadQuestsRate()` reads `../../Common/Settings/QuestsRate.txt`, a six-token-per-line file that configures the rewards and level eligibility of five repeatable quest items. It populates the class-static tables `QuestExp[5][2]`, `QuestCoin[5]`, and `QuestLevel[5][4]` (`CReadFiles.cpp:46-64`). These tables are consumed when a player uses the quest reward items (item indices `4117..4121`) in `_MSG_UseItem.cpp:2243-2277` to grant experience, coins, and to enforce level ranges.

**Detailed description**:
Three record keys are recognized after uppercasing the first token (`CReadFiles.cpp:311,329,346`): `EXP`, `COIN`, and `LEVEL`. For `EXP`, the second token is the quest index (validated `0 <= index < 5`), and the third and fourth tokens are the mortal-class and arch-class experience rewards respectively, each validated to be in `[0, 2,000,000,000)`; the two values are written to `QuestExp[index][0]` and `QuestExp[index][1]`. For `COIN`, the third token (coin reward) is validated to be in `[0, 2,000,000,000]` and written to `QuestCoin[index]`. For `LEVEL`, four tokens define two level windows — `minLv`, `maxLv`, `minLv2`, `maxLv2` — each validated to be in `[0, 400)` and written to `QuestLevel[index][0..3]`. The two windows correspond to the mortal and arch class eligibility bands used by `_MSG_UseItem.cpp:2245-2246`. Any validation failure shows a message box ("Índice inválido. (1)" or "(2)") and skips that line. The hardcoded defaults (lines 46-64) remain active for any quest row not overridden by the file. Note the asymmetric bounds: EXP/COIN allow an upper bound of exactly 2,000,000,000 (exclusive for EXP, inclusive for COIN), a minor inconsistency.

**Rule workflow**:
1. Open `QuestsRate.txt`; if missing, show a `MessageBoxA` and return (`CReadFiles.cpp:278-284`).
2. Read lines into `temp`; tokenize up to six tokens.
3. Dispatch on `EXP`/`COIN`/`LEVEL`.
4. Validate quest index (`[0,5)`) and value ranges; on failure show message box and skip.
5. Write values into the class-static quest tables.
6. Close the file at EOF.

---

### Business Rule: Item combination rate configuration loading (ReadCompRate)

**Overview**:
`ReadCompRate()` reads `../../Common/Settings/CompRate.txt`, a three-token-per-line file that configures the success probabilities for the game's item-combination (crafting/enhancing) NPCs. It writes into the global rate tables `g_pEhreRate`, `g_pOdinRate`, `g_pItemSancRate12`, `g_pItemSancRate12Minus`, `g_pAnctChance`, `g_pTinyBase`, `g_pShanyBase`, `g_pAilynBase`, and `g_pAgathaBase` (`Basedef.h:2484-2493`). These tables drive the actual outcome of combine operations in `_MSG_CombineItemEhre.cpp:113`, `_MSG_CombineItemOdin.cpp:124-512`, `_MSG_CombineItemShany.cpp:89`, and `GetFunc.cpp:83-95,214,314-424`.

**Detailed description**:
The parser reads three tokens per line and dispatches on the uppercased first token (`CReadFiles.cpp:399-405`). Seven top-level keys are recognized. `EHRE` uses the second token as a sub-key to select the Ehre combine rate slot: `PACOTE_ORI`→`g_pEhreRate[1]`, `MISTERIOSA`→`[2]`, `ESPIRITUAL`→`[3]`, `AMUNRA`→`[4]`, `TRAJE_MONTARIA`→`[6]`, `RETIRAR_TRAJE_MONTARIA`→`[7]`, `SOUL`→`[8]`. `TINY`, `SHANY`, `AILYN`, and `AGATHA` each accept the sub-key `CHANCEBASE` and write the third token to `g_pTinyBase`, `g_pShanyBase`, `g_pAilynBase`, and `g_pAgathaBase` respectively. `ODIN` is the richest record, mapping many sub-keys to `g_pOdinRate[]` slots (e.g. `ITEM_CELESTIAL`, `PISTA`, `DESTRAVE_LV40`, `PEDRA_DA_FURIA`, `SECRETA_*`, `SEMENTE_CRISTAL`, `CAPA_CELESTIAL`) and to the 11-slot `g_pItemSancRate12[0..10]` and 4-slot `g_pItemSancRate12Minus[0..3]` refinement-rate tables (`ITEM_12_SECRETA`, `ITEM_12_REF_0..9`, `ITEM_12_MINUS_12..15`). `COMPOSITOR` maps `ITEM_+7`/`ITEM_+8`/`ITEM_+9` to `g_pAnctChance[0..2]`. For every record the third-token value is validated to be in `[0,100]` (a percentage) before the write; violations show "Índice inválido. (1)" and skip the line. Unrecognized sub-keys are silently ignored (no warning), so a typo in a sub-key leaves the previous/default value in place.

**Rule workflow**:
1. Open `CompRate.txt`; if missing, show `MessageBoxA` and return (`CReadFiles.cpp:379-385`).
2. Read each line, tokenize into three tokens, uppercase the top-level key.
3. Dispatch on `EHRE`/`TINY`/`SHANY`/`AILYN`/`AGATHA`/`ODIN`/`COMPOSITOR`.
4. Match the sub-key to the corresponding global rate slot.
5. Validate the value in `[0,100]`; on failure show message box and skip.
6. Write the value; unknown sub-keys are ignored silently.
7. Close the file at EOF.

---

### Business Rule: Mercenary mob configuration loading (ReadMobMerc)

**Overview**:
`ReadMobMerc()` is designed to read `../../Common/Settings/MobMerc.txt`, a block-structured file describing special merchant mercenary NPCs (their mob name, spawn/renew timing, and inventory stock). It populates the global array `pMobMerc[MAX_MOB_MERC]` (`Basedef.h:158`, `Server.h:400`) of type `STRUCT_MERC` (`Basedef.h:672-682`). **Critical finding:** this method is never invoked anywhere in the TMSrv project, so the mercenary configuration is dead code within this component; the `pMobMerc` structure is instead populated only by the separate `ParseMobMercString` function in `Server.cpp` (which shares the same data target).

**Detailed description**:
The parser recognizes three line categories based on the first character. A line whose first character is `/` (ASCII 47) is treated as a comment and skipped (`CReadFiles.cpp:616-617`). A line whose first character is `#` starts a new mercenary record: the record counter `Num` is incremented (bounded by `MAX_MOB_MERC` = 128, at which point the loop breaks) and the corresponding `pMobMerc[Num]` struct is zero-initialized (`CReadFiles.cpp:619-627`). Any other non-`\r` line is passed to the external parser `ParseMobMercString(Num, tp)` (`Server.cpp:9300+`), which interprets `MOBNAME:`, `GENERATEINDEX:`, `RENEWTIME:`, `REBORNTIME:`, and `ITEM_N:` directives; the return value is ignored except that a `0` causes the line to be skipped (`CReadFiles.cpp:629-635`). Because the method is never called, its format contract and the `pMobMerc` load path it defines are not exercised at runtime, and any file at the `MobMerc.txt` path is never loaded by this class. This rule is therefore documented as latent/dead functionality.

**Rule workflow**:
1. Open `MobMerc.txt` in text mode; if missing, show `MessageBoxA` and return (`CReadFiles.cpp:593-600`).
2. Initialize record counter to `-1` and clear a local 1024-byte buffer.
3. Read each line; dispatch on first character (`/` comment, `#` new record, `\r` skip, else parse).
4. For record markers, enforce the `MAX_MOB_MERC` ceiling.
5. Delegate attribute lines to `ParseMobMercString`.
6. Close the file at EOF. (Unreachable at runtime as currently integrated.)

---

### Business Rule: Administrator IP allow-list loading (ReadAdmin)

**Overview**:
`ReadAdmin()` reads `../../DBSRV/Run/Admin.txt`, a file that defines the server's set of privileged administrator IP addresses, and stores them in the global `pAdminIP[MAX_ADMIN]` array (`Server.h:318`, `MAX_ADMIN` = 10 per `Basedef.h:58`). The allow-list is used for administrator authentication and privilege enforcement, notably in the `npko` admin command (`imple.cpp:1668,1687-1696`) which permits/denies kill-all rights by comparing connecting IPs against `pAdminIP`.

**Detailed description**:
Each file line is expected to encode a record index followed by the four octets of an IPv4 address, conventionally as `idx a.b.c.d`. The parser reads up to 127 bytes per line into a local buffer, replaces every `.` character with a space (`CReadFiles.cpp:661-663`), then parses five integers with `sscanf(ttt, "%d %d %d %d %d", &idx, &a, &b, &c, &d)` (`CReadFiles.cpp:665`). The four octets are packed into a single `unsigned int` in network-byte-order-equivalent form via `ip = (d << 24) + (c << 16) + (b << 8) + a` (`CReadFiles.cpp:667`). The record index must satisfy `0 <= idx < MAX_ADMIN` (10); records outside this range are silently skipped (`CReadFiles.cpp:669-670`), and valid records are written to `pAdminIP[idx]`. Unlike the rate loaders, a missing file causes a **silent return with no diagnostics** (`CReadFiles.cpp:646-647`), and a partial/corrupt line yields `a=b=c=d=0` and index `-1` (also silently skipped). There is no deduplication or overlap detection between index slots.

**Rule workflow**:
1. Open `Admin.txt`; if missing, return silently (`CReadFiles.cpp:644-647`).
2. Read each line (up to 127 bytes).
3. Replace `.` with spaces; parse index + 4 octets.
4. Pack octets into a 32-bit integer.
5. If the index is in `[0, MAX_ADMIN)`, store into `pAdminIP[idx]`; otherwise skip.
6. Close the file at EOF.

---

### Business Rule: MAC-address block-list loading (ReadMacblock)

**Overview**:
`ReadMacblock()` reads `../../DBSRV/Run/Mac.txt`, which holds the server's block-list of hardware MAC addresses. It populates the global array `pMac[MAX_MAC]` of `STRUCT_BLOCKMAC` (`Basedef.h:916-919`, `MAX_MAC` = 200 per `Basedef.h:152`). The block-list is consulted during client authentication in `ProcessDBMessage.cpp:371-374`, where a connecting user's MAC address is compared against `pMac` and rejected on a match.

**Detailed description**:
The file format mirrors `ReadAdmin`: each line encodes a record index followed by four MAC octets. The parser reads up to 127 bytes per line, replaces `.` characters with spaces, and parses `idx` plus four octets via `sscanf` (`CReadFiles.cpp:692-703`). Unlike `ReadAdmin`, the four octets are **not packed** into an integer; they are stored verbatim into `pMac[idx].Mac[0..3]` (`CReadFiles.cpp:708-711`) for byte-wise comparison by the login flow. Records whose index falls outside `[0, MAX_MAC)` are silently skipped (`CReadFiles.cpp:705-706`), and a missing file causes a silent return (`CReadFiles.cpp:684-685`). The method is invoked from the per-second server timer at `ProcessSecMinTimer.cpp:128`. Notably, the guard at `ProcessSecMinTimer.cpp:127` is `if (SecCounter % 120)`, which is true on every tick except multiples of 120, so in practice `ReadMacblock` executes roughly every second rather than the two-minute cadence the naming/pattern implies — an inverted-condition issue at the call site that is documented as a risk.

**Rule workflow**:
1. Open `Mac.txt`; if missing, return silently (`CReadFiles.cpp:682-685`).
2. Read each line (up to 127 bytes).
3. Replace `.` with spaces; parse index + 4 octets.
4. If the index is in `[0, MAX_MAC)`, store the four octets into `pMac[idx]`; else skip.
5. Close the file at EOF.
6. Invoked each server tick by `ProcessSecMinTimer.cpp:127-128` (see risk on inverted condition).

---

### Business Rule: Guild challenge prize-money load/persist (ReadChallanger / WriteChallanger)

**Overview**:
`ReadChallanger()` and `WriteChallanger()` load and persist the accumulated challenge prize money for each guild zone, stored in `pChallangerMoney[ValidGuild]` (`Server.h:304`, `ValidGuild` = `MAX_GUILDZONE` = 5 per `Server.cpp:55`). The money is used to gate and score guild challenges (`_MSG_Challange.cpp:123`, `Server.cpp:2560-2563`) and is refreshed after challenge outcomes.

**Detailed description**:
Both methods format a server-specific file path from the `CHALL_PATH` template `../../Common/Chall_%2.2d_%2.2d.txt` using `ServerGroup` and `ServerIndex` (`CReadFiles.cpp:721,747`), producing filenames like `Chall_01_02.txt`. If the server-specific file cannot be opened, both methods fall back to a generic `Chall.txt` in the current working directory (`CReadFiles.cpp:727-728,753-754`). `ReadChallanger` reads exactly `ValidGuild` (5) integers from the file into `pChallangerMoney[0..4]` using `fscanf`, with no per-read success checking (`CReadFiles.cpp:737-738`); on total open failure it logs `"err,reading chall.txt - can't open.txt"` and returns. `WriteChallanger` writes the same 5 values, one per line, in text mode (`CReadFiles.cpp:763-767`), logging `"err,writing chall.txt - can't open for write"` on failure. This persistence keeps the challenge prize money stable across server restarts. `WriteChallanger` is invoked after a successful challenge in `Server.cpp:2565` and after weekly challenge resolution in `Server.cpp:6560`.

**Rule workflow (Read)**:
1. Format the server-specific path; open for reading.
2. On failure, fall back to `Chall.txt`; on failure log and return.
3. Read `ValidGuild` integers into `pChallangerMoney` without checking each read.
4. Close the file.

**Rule workflow (Write)**:
1. Format the server-specific path; open for writing.
2. On failure, fall back to `Chall.txt`; on failure log and return.
3. Write `ValidGuild` integers, one per line.
4. Close the file.

---

### Business Rule: Guild zone state load/persist (ReadGuild / WriteGuild)

**Overview**:
`ReadGuild()` and `WriteGuild()` manage the persistent guild-zone state of the five castle/city zones, including the currently charging (ruling) guild, the challenging guild, city tax rate, clan, and victory count, plus the global `GuildCounter` used to allocate guild IDs. This state is stored in `g_pGuildZone[MAX_GUILDZONE]` (`STRUCT_GUILDZONE`, `Basedef.h:714-744`) and `GuildCounter` (`Server.cpp:53`). The data is central to guild ownership, city tax collection, and challenge logic (`Server.cpp:2545-2567`, `_MSG_MessageChat.cpp:88-94`, `_MSG_Buy.cpp`, `_MSG_MessageWhisper.cpp`).

**Detailed description**:
`ReadGuild()` formats a server-specific path from `GUILD_PATH` (`../../Common/Guild_%2.2d_%2.2d.txt`) using `ServerGroup`/`ServerIndex`, with a fallback to `Guild.txt` (`CReadFiles.cpp:778-785`). Before reading, it resets `GuildCounter = 0` and zeroes `ChargeGuild` and `ChallangeGuild` for all five zones (`CReadFiles.cpp:774,793-797`). It then reads, in fixed sequence via `fscanf`: the `GuildCounter`; five `ChargeGuild` values; five `ChallangeGuild` values; five `CityTax` values (each validated: if outside `[0,20]`, it is clamped to the default `10` at `CReadFiles.cpp:811-812`); five `Clan` values; and five `Victory` values (`CReadFiles.cpp:799-819`). If `GuildCounter` reads as `0`, it logs `"err, Reading Guild error - Guild counter zero"` (`CReadFiles.cpp:823-824`). On open failure it logs and returns. `WriteGuild()` writes the same state back in text format (`CReadFiles.cpp:847-872`), then additionally writes a companion champion-city file via the `GUILDCHAMP_PATH` template `../../Common/ChampionCity_%2.2d_%2.2d.txt` (`CReadFiles.cpp:876-903`). That file enumerates the five cities (`Armia`, `Arzan`, `Erion`, `Noatun`, `Nipplehein`), resolving each ruling guild's name via `BASE_GetGuildName` and a guild-mark bitmap filename `b01%06d.bmp`. `ReadGuild` is triggered by the GUI `IDC_READGUILD` menu (`Server.cpp:4102`) and the `reloadguild` command (`imple.cpp:846`); `WriteGuild` is triggered by guild events (`_MSG_Buy.cpp:231`, `_MSG_MessageWhisper.cpp:263`), tax changes (`_MSG_MessageChat.cpp:94`), admin commands (`imple.cpp:1062,1077`), challenge updates (`Server.cpp:2566`), and server shutdown (`Server.cpp:4130`).

**Rule workflow (Read)**:
1. Reset `GuildCounter` and zero zone charge/challenge fields.
2. Format server-specific path; open for reading (`rb`); fallback to `Guild.txt`.
3. On total failure log and return.
4. Sequentially read `GuildCounter`, 5×`ChargeGuild`, 5×`ChallangeGuild`, 5×`CityTax`, 5×`Clan`, 5×`Victory`, clamping `CityTax` to 10 if outside `[0,20]`.
5. Log a warning if `GuildCounter` is zero; close the file.

**Rule workflow (Write)**:
1. Format server-specific path; open for writing; fallback to `Guild.txt`.
2. On failure log and return.
3. Write `GuildCounter` and the five zone arrays in the same fixed sequence.
4. Open the `ChampionCity` file and write the five city entries (city name, guild name, guild-mark bitmap), then close.

---

### Business Rule: Hardcoded default quest reward tables

**Overview**:
The class statically initializes default values for the quest reward tables (`CReadFiles.cpp:46-64`): `QuestCoin[5] = {2000, 4000, 6000, 8000, 10000}`, `QuestExp[5][2]` with escalating mortal/arch EXP pairs (`{1000,500}` … `{5000,2500}`), and `QuestLevel[5][4]` with five ascending level windows (`{39,115,…}` … `{320,350,…}`).

**Detailed description**:
These static member initializers serve as the compiled-in baseline for the five quest tiers. They take effect immediately at process start and remain authoritative for any tier not overridden by `QuestsRate.txt`. The level windows are symmetric between the mortal and arch columns (columns 0/1 equal columns 2/3 in every default row), meaning both classes share identical eligibility bands by default; `ReadQuestsRate` may make them diverge per class. The EXP pairs provide a 2:1 mortal-to-arch reward ratio at every tier. This rule is consumed by `_MSG_UseItem.cpp:2245-2277` when a player uses quest reward items, where the effective level bounds and rewards are selected based on the player's `ClassMaster` (MORTAL vs ARCH). Because they are plain static data with no validation at initialization, any file override is the only mechanism that changes them after compile time.

**Rule workflow**:
1. Initialize `QuestCoin`, `QuestExp`, `QuestLevel` from static initializers at load time.
2. Apply any overrides from `ReadQuestsRate`.
3. Expose the tables to quest reward consumption in `_MSG_UseItem.cpp`.

---

## 4. Component Structure

The component is a single static utility class implemented across two files in `legacy/Code/TMSrv/`.

```
legacy/Code/TMSrv/
├── CReadFiles.h          # Class declaration (58 lines)
│   ├── CReadFiles()      # Constructor
│   ├── ReadSancRate()    # Load sanctification rates/grades
│   ├── ReadQuestsRate()  # Load quest EXP/COIN/LEVEL tables
│   ├── ReadCompRate()    # Load combine rates
│   ├── ReadMobMerc()     # Load mercenary defs (dead code - never called)
│   ├── ReadAdmin()       # Load admin IP allow-list
│   ├── ReadMacblock()    # Load MAC block-list
│   ├── ReadChallanger()  # Load challenge prize money
│   ├── WriteChallanger() # Persist challenge prize money
│   ├── ReadGuild()       # Load guild zone state
│   ├── WriteGuild()      # Persist guild zone state + champion city file
│   ├── QuestExp[5][2]    # Public static data
│   ├── QuestCoin[5]      # Public static data
│   └── QuestLevel[5][4]  # Public static data
└── CReadFiles.cpp        # Implementation (904 lines)
```

All methods are `static`; the class holds no instance members and is used purely as a namespace for configuration I/O. Shared configuration state lives in process-global arrays declared in `legacy/Code/Basedef.h` and `legacy/Code/TMSrv/Server.h`.

---

## 5. Dependency Analysis

### Internal Dependencies (relationship chains)

```
CReadFiles (TMSrv) --reads/writes--> global tables declared in Basedef.h / Server.h:
    g_pSancRate[3][12], g_pSancGrade[2][5]
    g_pEhreRate[10], g_pOdinRate[12], g_pItemSancRate12[11],
    g_pItemSancRate12Minus[4], g_pAnctChance[3]
    g_pTinyBase, g_pShanyBase, g_pAilynBase, g_pAgathaBase
    pMobMerc[MAX_MOB_MERC] (STRUCT_MERC)
    pAdminIP[MAX_ADMIN], pMac[MAX_MAC] (STRUCT_BLOCKMAC)
    pChallangerMoney[ValidGuild], GuildCounter, g_pGuildZone[5]
    ServerGroup, ServerIndex, ValidGuild, hWndMain, temp

CReadFiles --calls--> Log()                    (Server.cpp)
CReadFiles --calls--> ParseMobMercString()     (Server.cpp) - used by ReadMobMerc (dead)
CReadFiles --calls--> BASE_GetGuildName()      (Basedef.cpp)

Callers of CReadFiles:
    Server.cpp            (constructor:3627; ReadGuild:4102; WriteGuild:4130,2566;
                           WriteChallanger:2565,6560)
    imple.cpp             (ReadSancRate/Quests/Comp:255-257; ReadGuild:846;
                           WriteGuild:1062,1077; ReadAdmin:1668)
    ProcessSecMinTimer.cpp (ReadMacblock:128)
    _MSG_Buy.cpp          (WriteGuild:231)
    _MSG_MessageChat.cpp  (WriteGuild:94)
    _MSG_MessageWhisper.cpp (WriteGuild:263)
    _MSG_UseItem.cpp      (reads QuestLevel/QuestExp/QuestCoin:2245-2277)
    ProcessClientMessage.h (includes CReadFiles.h)
    CCastleZakum.cpp / CWarTower.cpp (include CReadFiles.h only)

Consumers of tables loaded by CReadFiles (downstream data flow):
    Basedef.cpp:2220,2234           (g_pSancRate -> BASE_GetSuccessRate)
    _MSG_UseItem.cpp:862,2245-2277  (g_pSancGrade, Quest* tables)
    _MSG_CombineItemEhre.cpp:113    (g_pEhreRate)
    _MSG_CombineItemOdin.cpp:124-512(g_pOdinRate, g_pItemSancRate12*)
    _MSG_CombineItemShany.cpp:89    (g_pShanyBase)
    GetFunc.cpp:83-95,214,314-424   (g_pAnctChance, g_pTinyBase/Ailyn/Agatha)
    ProcessDBMessage.cpp:371-374    (pMac -> MAC block check)
    _MSG_Challange.cpp:123, Server.cpp:2560-2563 (pChallangerMoney)
    _MSG_Buy.cpp:243-255, Server.cpp:3003-3006,
    ProcessSecMinTimer.cpp:1341-1348 (pMobMerc)
```

### External Dependencies

| Dependency | Type | Purpose | Notes |
|------------|------|---------|-------|
| C standard library (`stdio.h`) | System library | File I/O (`fopen`, `fgets`, `fscanf`, `fprintf`, `fclose`, `sscanf`) | Core I/O primitive |
| C standard library (`stdlib.h`) | System library | `atoi` (string-to-int) | Indirect via includes |
| Windows API (`Windows.h`) | OS API | `MessageBoxA` (blocking error dialogs), `hWndMain` window handle | Platform-specific; renders config errors as modal GUI dialogs |
| MSVC string (`_strupr`) | Compiler runtime | In-place uppercase conversion for key matching | Case-insensitive config keys |

Note: All file-path constants and I/O are Windows-oriented (backslashes in `../..\Basedef.h` includes, `MessageBoxA`, `_strupr`). No external network/database/third-party library is used by this component; all "integration" is through shared in-process global state and the filesystem.

---

## 6. Afferent and Efferent Coupling

The unit of "component" here is the `CReadFiles` class (static utility). The table below reports coupling at the class level and per public method.

| Component / Method | Afferent Coupling | Efferent Coupling | Critical |
|--------------------|-------------------|-------------------|----------|
| `CReadFiles` (class) | 8 calling files + 2 include-only | ~22 global symbols + 3 functions + stdio/Windows | High |
| `CReadFiles()` constructor | 1 (Server.cpp:3627) | 7 reads (Sanc/Quests/Comp/Admin/Mac/Challanger/Guild) | High |
| `ReadSancRate()` | 2 (constructor, imple.cpp:255) | g_pSancRate, g_pSancGrade, MessageBoxA, temp, hWndMain | High |
| `ReadQuestsRate()` | 2 (constructor, imple.cpp:256) | QuestExp/Coin/Level, MessageBoxA, temp, hWndMain | High |
| `ReadCompRate()` | 2 (constructor, imple.cpp:257) | 9 rate globals, MessageBoxA, temp, hWndMain | High |
| `ReadMobMerc()` | 0 (never called) | pMobMerc, ParseMobMercString, MessageBoxA | Low (dead) |
| `ReadAdmin()` | 2 (constructor, imple.cpp:1668) | pAdminIP, temp | Medium |
| `ReadMacblock()` | 2 (constructor, ProcessSecMinTimer.cpp:128) | pMac, temp | Medium |
| `ReadChallanger()` | 1 (constructor) | pChallangerMoney, ServerGroup/Index, Log | Medium |
| `WriteChallanger()` | 3 (Server.cpp:2565,6560; imple.cpp via state) | pChallangerMoney, ServerGroup/Index, Log | Medium |
| `ReadGuild()` | 3 (constructor, Server.cpp:4102, imple.cpp:846) | GuildCounter, g_pGuildZone, Log | High |
| `WriteGuild()` | 6 (Server.cpp:2566,4130; imple.cpp:1062,1077; _MSG_Buy:231; _MSG_MessageChat:94; _MSG_MessageWhisper:263) | GuildCounter, g_pGuildZone, BASE_GetGuildName, ServerGroup/Index | High |

Afferent coupling counts distinct calling translation units; `WriteGuild` is the most heavily depended-upon method (7 call sites across 6 files). Efferent coupling is high because the class touches a large number of process-global tables and shared buffers, making it a tightly bound config hub.

---

## 7. Endpoints

This component **does not expose any network endpoints** (no REST, GraphQL, gRPC, or socket protocol of its own). The TMSrv server is a socket-based game server (`CPSock`), and `CReadFiles` is an internal, in-process configuration loader invoked only through local triggers. Its invocation points (not network endpoints) are:

| Invocation Point | Kind | Location |
|------------------|------|----------|
| Server startup | Programmatic | Server.cpp:3627 (constructor) |
| GUI menu `IDC_READGUILD` | GUI command | Server.cpp:4102 |
| Server shutdown `WM_CLOSE` | GUI event | Server.cpp:4130 |
| In-game admin `+reloadfile` | In-game command | imple.cpp:255-257 |
| In-game admin `reloadguild` | In-game command | imple.cpp:846 |
| In-game admin `npko` | In-game command | imple.cpp:1668 |
| Per-second timer | Timer | ProcessSecMinTimer.cpp:127-128 |

---

## 8. Integration Points

This component integrates with the surrounding system through shared in-process global state and the local filesystem; there are no external service/API/database integrations.

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Config files under `../../Common/Settings/` | Filesystem | Load tunable rates (sanc/quests/comp/mobmerc) | File I/O (`fopen`/`fgets`/`sscanf`) | Line-oriented text, token-separated | `MessageBoxA` dialog + skip line / silent return |
| `../../DBSRV/Run/Admin.txt` | Filesystem | Admin IP allow-list | File I/O | `idx a.b.c.d` per line | Silent return on missing file; skip invalid index |
| `../../DBSRV/Run/Mac.txt` | Filesystem | MAC block-list | File I/O | `idx a.b.c.d` per line | Silent return; skip invalid index |
| `../../Common/Chall_%2.2d_%2.2d.txt` | Filesystem | Challenge prize-money persistence | File I/O (`fscanf`/`fprintf`) | Space-separated ints | `Log()` on open failure; fallback to `Chall.txt` |
| `../../Common/Guild_%2.2d_%2.2d.txt` | Filesystem | Guild zone state persistence | File I/O (`fscanf`/`fprintf`) | Fixed-sequence ints | `Log()` on failure; CityTax clamped; fallback `Guild.txt` |
| `../../Common/ChampionCity_%2.2d_%2.2d.txt` | Filesystem | Champion city summary export | File I/O (`fprintf`) | `city guildname b01NNNNNN.bmp` | `Log()` on failure |
| Shared global tables (`Basedef.h`, `Server.h`) | In-process | Publish loaded config to gameplay modules | Static memory | `int` arrays / structs | None (no transactional or consistency guarantees) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Static Utility / Namespace class | All members static, no instance state | CReadFiles.h:25-56 | Group related file-I/O routines without object state |
| Registry / Configuration Table | Loads file values into process-global arrays consumed elsewhere | CReadFiles.cpp:123,325-368,416-584 | Publish tunable game rates to the rest of the server |
| Default-fallback initialization | Hardcoded static arrays as compiled-in defaults | CReadFiles.cpp:46-64 | Guarantee valid values before/without file override |
| Template-based file naming | `sprintf` into path constants with `ServerGroup`/`ServerIndex` | CReadFiles.cpp:721,747,778,831,876 | Per-server-instance configuration files |
| Fallback resource resolution | Fallback to generic `Chall.txt`/`Guild.txt` when server file missing | CReadFiles.cpp:727-728,784-785 | Graceful degradation when per-server files absent |
| Delegated line parsing | Attribute lines delegated to `ParseMobMercString` | CReadFiles.cpp:631 | Reuse of shared parser for `pMobMerc` |
| Centralized constructor orchestration | Constructor chains all initial loads | CReadFiles.cpp:66-75 | Single startup entry point for config bootstrap |

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | ProcessSecMinTimer.cpp:127 (call site) | `if (SecCounter % 120)` is an inverted condition: `ReadMacblock` runs every second except multiples of 120, instead of every 2 minutes | Unnecessary per-second disk reads of `Mac.txt`; likely a logical bug |
| High | All read methods | Return-type `void`, no error/success propagation; partial reads (`fscanf` unchecked) can leave tables silently corrupt | Undetectable misconfiguration; bad data flows into gameplay |
| High | ReadChallanger/ReadGuild | `fscanf` return values never checked; fixed read sequence with no field count validation | Truncated/corrupt files yield garbage state or out-of-bounds `GuildCounter` |
| Medium | ReadMobMerc | Method defined and declared but never called (dead code) | Mercenary config at `MobMerc.txt` is never loaded by this path; confusing/unreachable logic |
| Medium | All rate loaders | Blocking `MessageBoxA` modal dialogs on missing files/invalid data, requiring `hWndMain` GUI | Server cannot run unattended/headless; a modal dialog can halt the server |
| Medium | ReadSancRate | `ÂMAGO` maps to the same `g_pSancRate[0]` row as `PO` | Ambiguous alias — possibly an unintended overwrite of PO rates |
| Medium | Path constants | Hardcoded relative paths (`../../Common/...`, `../../DBSRV/...`) and Windows backslashes | Fragile CWD-dependent startup; not portable; failure if run from a different working directory |
| Medium | ReadGuild | `fopen(dir, "rb")` (binary) combined with text-mode `fscanf` | Mode mismatch; line-ending/whitespace handling may be inconsistent across platforms |
| Medium | Config consistency | No locking, validation of cross-table invariants, or checksum around shared global writes | Concurrent/multiple writers can leave partially persisted state |
| Medium | ReadAdmin/ReadMacblock | Silent return on missing file; indices beyond range silently dropped; no count of applied records | Admin/MAC protections may be silently absent (fail-open) |
| Low | All parsers | `val1` (first token) parsed via `atoi` but never used in most methods; magic numbers (5, 12, 0-100, 400, 2e9) scattered | Dead computation; maintenance and correctness risk on tuning changes |
| Low | Quest validation | EXP/COIN bounds are inconsistent (EXP `< 2e9`, COIN `<= 2e9`) | Minor validation asymmetry; edge-case coin value accepted while EXP is rejected |
| Low | Code organization | Nearly all state lives in process globals declared far from the class (`Basedef.h`/`Server.h`) | Poor encapsulation; high efferent coupling; harder to test or reuse |

---

## 11. Test Coverage Analysis

No automated tests exist for this component, and none exist anywhere in the repository. A full search for test files/directories (`test`, `tests`, `*test*`, `*spec*`) under the project root returned zero results (excluding `.git` and `.opencode`), and the CodeGraph index reports **no covering tests** for every method (`ReadChallanger`, `ReadGuild`, `ReadSancRate`, `ReadCompRate`, etc.).

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CReadFiles (all methods) | 0 | 0 | 0% | N/A — no test infrastructure present |
| Config parsing logic | 0 | 0 | 0% | N/A |

**Risk statement**: The parsing and validation logic in this component (tokenizing, range checking, file fallback, clamping) is entirely untested and is exercised only through manual server operation. Given the many validation branches, fallback paths, and the identified inverted-condition bug, the absence of tests is a significant coverage gap. There are no test fixtures, mocks, or test harnesses available; the only observable verification is runtime behavior of the live server.

---

## 12. Notes on Scope and Ambiguity

- The TMSrv project has its own `CReadFiles`; a **separate, distinct** `CReadFiles` implementation also exists in `legacy/Code/DBSrv/CReadFiles.cpp`/`.h` for the DB server. This report analyzes **only the TMSrv variant** as specified. The two classes are independent and should not be conflated.
- Several business rules are implicit in the code (e.g., the "ÂMAGO"/"PO" alias, the `ValidGuild` loop bounds, the CityTax default of 10, and the 2:1 EXP ratio) and are documented here with the confidence of the source evidence, not from external specification.
- The `ReadMacblock` cadence discrepancy originates at the call site (`ProcessSecMinTimer.cpp:127`) rather than inside the component, but is included because it directly affects how frequently the component's I/O runs.

---

*End of report.*
