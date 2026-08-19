# Component Deep Analysis Report

**Component:** imple
**File:** legacy/Code/TMSrv/imple.cpp
**Role:** Command / Admin handler (GM command interpreter)
**Analysis Date:** 2026-08-19

---

## 1. Executive Summary

The `imple` component is the administrative command dispatcher of the W2PP legacy game server (TMSrv, the "Total Master" field server). It is implemented as a single translation unit `legacy/Code/TMSrv/imple.cpp` (1,846 lines) exposing two functions: `ProcessImple(int conn, int level, char *str)` — the main command interpreter — and `SaveAll()` — a utility that persists all online players.

The component receives a raw admin command string (typically prefixed with `+`, as the GM chat convention is `+command args`), parses it via `sscanf` into up to eight string parameters and eight numeric parameters, and dispatches it against a tiered privilege model. Access control is enforced through a single `level` integer compared against four thresholds (`>= 8`, `>= 3`, `>= 2`, `>= 1`). The `level` value is derived at the call site as `pMob[conn].MOB.CurrentScore.Level - 1000` (so a character whose level is 1001+ gains access to level-1 commands), or passed directly from the database server (DBSrv) via the `_MSG_MessageDBImple` message channel.

Functionally, the component is a God-object style command switch with more than 90 distinct command handlers spanning: character stat mutation, item granting/equipping, NPC persistence, gate (portal) management, castle/guild war control, event configuration, drop-rate tuning, treasure-table editing, mob spawning/killing, teleportation, user moderation (mute/kick/summon), billing configuration, and live configuration reloads.

Key findings:

- **Tiered privilege gating** is the sole security boundary; it is coarse-grained and many high-impact commands are grouped under low privilege tiers (level >= 1).
- **No input validation on many numeric parameters** — out-of-range values are largely unchecked and can index global arrays (`g_pTreasure[8]`, `g_pGuildZone[5]`, `g_pDropBonus[64]`, `pMob`, `pItem`) directly, creating out-of-bounds risk.
- **Buffer handling via raw `sscanf`/`sprintf`/`strcpy`/`strncpy`** throughout — classic legacy C risks (unbounded writes, format-string style logging of raw admin input).
- **No test files exist** anywhere in the legacy project for this component; testing is effectively non-existent (manual / runtime only).
- **Tight coupling to global mutable state** (`pUser`, `pMob`, `pItem`, `pMobGrid`, `mNPCGen`, `GuildInfo`, `g_pTreasure`, `g_pGuildZone`, `g_pDropBonus`) shared across the entire server process.

---

## 2. Data Flow Analysis

```
1. Command originates from one of two entry points:
   a. GM chat: player types "+..." in whisper to "gm"/"GM"
      → _MSG_MessageWhisper.cpp:791 → level = CurrentScore.Level - 1000
      → ProcessImple(conn, level, m->String)
   b. DB console/remote admin: DBSrv sends _MSG_MessageDBImple
      → ProcessDBMessage.cpp:132 → ProcessImple(0, m->Level, m->String)
2. ProcessImple parses the raw string:
   → sscanf(str+1, "%s ...", cmd, sval1..sval8)  (up to 8 string args)
   → sscanf(sval1..8, "%d"/"%llu", &ival1..&ival8)  (up to 8 int args)
3. Command is logged (audit trail): Log("adm <str>", account, IP)
4. Access control: level thresholds gate each command block:
   → level >= 8 : character/stat/item/gate admin
   → level >= 3 : server timing & billing config
   → level >= 2 : reboot, spawn, gift, event & drop config
   → level >= 1 : weather, teleport, moderation, misc
5. Command handlers mutate global state and/or call helpers:
   → character stats (pMob[conn].MOB.*), items (PutItem/CreateItem),
     NPC persistence (file I/O via _open/_write/_read),
     portals (pItem, UpdateItem, GridMulticast),
     events (evItem/evRate/...), treasure (g_pTreasure), drops (g_pDropBonus)
6. Feedback to the requesting client via SendClientMessage(conn, msg)
   and grid broadcasting via GridMulticast / SendNotice / DBServerSocket
7. Some commands propagate to the DB server or game grid:
   → DBServerSocket.SendOneMessage (notice, guild fame)
   → SendScore / SendEtc / SendEquip / SendItem to refresh client state
```

The two entry points share a single dispatch path; the DB channel uses `conn = 0` (system context) and supplies its own `level`, while the GM-chat channel uses `conn` as the acting player index and derives `level` from character level.

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Security | Command access gated by `level` tiers (>=8, >=3, >=2, >=1) | imple.cpp:89,569,647,1119,1164 |
| Security | `level` derived from character level minus 1000 in GM chat path | _MSG_MessageWhisper.cpp:789 |
| Security | Level-8 commands require valid `conn` in [1, MAX_USER-1] | imple.cpp:91 |
| Validation | `set level` caps value at 2010 | imple.cpp:125 |
| Validation | `timer` value must be >= 5000 ms | imple.cpp:573 |
| Validation | `generate` index must be in [1, mNPCGen.NumList] | imple.cpp:662 |
| Validation | `create` (mob) returns typed result codes (1/0/-1/-2) | imple.cpp:676-683 |
| Validation | `trtarget`/`trsource`/`trrate` bound treasure indices (1-8, 1-5) | imple.cpp:856-916 |
| Validation | `guildday` in [0,6], `guildhour`/`newbiehour` in [0,23] | imple.cpp:802-822 |
| Validation | `drop` bonus rate in [0,8000] with specific position ranges | imple.cpp:1082-1112 |
| Validation | `evon` requires configured item/index/rate before enabling | imple.cpp:998 |
| Validation | `fame` guild id must be in (0, 65536) | imple.cpp:1638 |
| Business Logic | `teleport` converts small (<100) coords to grid cells (x*128+64) | imple.cpp:1794 |
| Business Logic | `weekmode` forces guild mode; -1 reverts to weekly auto | imple.cpp:824-838 |
| Business Logic | NPK O IP binding hardcodes three named accounts | imple.cpp:1674-1687 |
| Business Logic | `set item`/equip auto-initializes "2330-2390" item family | imple.cpp:448-453,483-488 |
| Business Logic | Mob EXP rebalance formulas (waterexp/nigexp/svexp) | imple.cpp:1203-1385 |
| Business Logic | `delayreboot` computes server shutdown offset | imple.cpp:653-659 |
| Authorization | `npko` writes admin IP file to DBSrv Run folder | imple.cpp:1670 |
| Moderation | `kick` cannot kick equal/higher level admins | imple.cpp:1821 |
| Moderation | `mute`/`muteall`/`desmuteall` toggle chat prohibition | imple.cpp:1514-1562 |
| Moderation | `snoop` toggles merchant snoop flag bit 0x01 | imple.cpp:1569 |

## Detailed breakdown of the business rules

### Business Rule: Tiered Command Access Control (level >= 8 / 3 / 2 / 1)

**Overview**
All administrative commands are protected by a coarse privilege tier. The single `level` parameter is compared against four thresholds. A command is only interpreted when the caller's `level` meets or exceeds the tier in which the command block resides. This is the entire authorization model of the component; there is no per-command permission granularity.

**Detailed description**
The `ProcessImple` function encloses command groups in four nested `if (level >= N)` blocks. The highest tier (level >= 8) wraps the bulk of character, item, gate, and NPC file commands (the `set` subcommand with ~40 suboptions, `save`, `read`, `name`). The next tier (level >= 3) contains server timing and billing/global configuration commands (`timer`, `saveall`, `decay`, plus a second `set` block for billing flags). The tier at level >= 2 holds reboot, mob generation/spawning, gifting, player-flag (PK/frag) and event/drop/treasure tuning commands. The lowest tier (level >= 1) holds a wide and high-impact set including weather, teleport, moderation (mute/kick/summon), notice broadcasting, EXP rebalancing, and live world mutation commands. Notably, the privilege levels are ordinal, not cumulative-permission: a level-8 caller can execute all commands, while a level-1 caller can only execute the level>=1 block.

The `level` value is computed at the GM-chat call site as the character's current level minus 1000 (`_MSG_MessageWhisper.cpp:789`). This means a normal player at character level 1001 acquires level-1 command access, level 1002 acquires level-2 access, and so on. In practice this is a hard-coded GM convention where GM characters are given elevated character levels. The DB channel (`ProcessDBMessage.cpp:132`) instead passes `m->Level` verbatim from the DBSrv, which is the more privileged system/console path and does not depend on the character's level.

**Rule workflow**
```
1. Caller computes level (GM chat: CharLevel-1000) or supplies it (DB channel).
2. ProcessImple(conn, level, str) parses cmd + args.
3. Command string logged regardless of level.
4. level >= 8 block evaluated first; if matched, handler runs.
5. Then level >= 3, then level >= 2, then level >= 1 blocks evaluated in order.
6. First matching command handler executes; unmatched commands fall through silently.
```

---

### Business Rule: Character Stat and Item Mutation (set subcommand, level >= 8)

**Overview**
The level >= 8 `set` handler mutates the acting character's `pMob[conn].MOB` structure — base/current scores, experience, coins, merchant flag, learned skills, class master, special bonuses — and can create/equip items and manage gates. After mutating stats it always recomputes and pushes refreshed state to the client.

**Detailed description**
The `set` block matches `sval1` against a long list of stat names (`sanc`, `level`, `hp`, `mp`, `ac`, `dam`, `exp`, `str`, `int`, `dex`, `con`, `coin`, `merchant`, `skillbonus`, `special0..3`, `scorebonus`, `attackrun`, `critical`, `learned`, `special`, `classmaster`, `clearquest`, `clearskill`, `buildhtml`, `reloadfile`, gate commands, and `item`). Most assignments are direct scalar writes to fields of `pMob[conn].MOB` (e.g., `BaseScore.Level`, `BaseScore.MaxHp/Hp`, `BaseScore.Str/Int/Dex/Con`, `Coin`, `Merchant`, `SkillBonus`, `ScoreBonus`, `Critical`, `SpecialBonus`, `extra.ClassMaster`). The `set level` rule caps the value at 2010 (`imple.cpp:125`), a hard server maximum. `set hp`/`set mp` write both base and current max and current values so the change is immediately consistent. The `exp` setter additionally triggers `CheckGetLevel()` and `SendEtc` to recompute the level curve. After stat mutation, the code unconditionally (outside any sub-branch, at lines 500-506) recomputes current score, re-sends equipment, score, and etc, and re-sends all 16 equip slots.

**Rule workflow**
```
1. Caller with level >= 8 issues "set <name> <value...>".
2. sval1 matched against stat names; each branch writes pMob[conn].MOB fields.
3. set level clamps value to <= 2010.
4. set exp recomputes level via CheckGetLevel and SendEtc.
5. Post-mutation refresh: GetCurrentScore, SendEquip, SendScore, SendEtc, SendItem x MAX_EQUIP.
6. SendClientMessage confirms to the acting client.
```

---

### Business Rule: Item Granting and Equipping (set item / set equip, level >= 8)

**Overview**
The `set item` subcommand creates a `STRUCT_ITEM` on the acting character's carry (inventory) with configurable item index and up to three effect slots (effect type + value). Separately, a numeric first argument in [1, MAX_EQUIP] is interpreted as an equipment slot, writing an item directly into `pMob[conn].MOB.Equip[slot-1]`.

**Detailed description**
The handler builds a `STRUCT_ITEM` with `sIndex` from `ival2` and fills `stEffect[0..2]` with `(cEffect, cValue)` pairs from `ival3..ival8`. A special family of item indices in the range `2330 <= sIndex < 2390` is pre-initialized with a hard-coded set of effects (`stEffect[0].sValue = 2000`, `stEffect[1] = (120, 60)`), which represents a known class of "special" items that must carry fixed base effects; these are then overlaid with the caller-supplied effects. The resulting item is placed via `PutItem(conn, &Item)`. Separately, if `ival1` is in `[1, MAX_EQUIP]`, the same item-building logic targets `pMob[conn].MOB.Equip[ival1-1]` directly. Both paths write an audit log entry (`Log("-system", ...)`) containing the acting mob name and full item/effect tuple. After equipment mutation, the same post-mutation refresh sequence (GetCurrentScore, SendEquip, SendScore, SendEtc, SendItem) runs.

**Rule workflow**
```
1. level >= 8 caller issues "set item <index> <e0> <v0> <e1> <v1> <e2> <v2>".
2. STRUCT_ITEM built; family 2330-2390 pre-initialized with fixed effects.
3. Effects overlaid from caller args.
4. PutItem adds to carry (inventory).
5. If ival1 in [1,MAX_EQUIP], same item written to Equip[ival1-1].
6. Audit log written; client state refreshed; confirmation sent.
```

---

### Business Rule: Gate (Portal) Lifecycle Management (set gate/destroygate/cgate/ogate/ngate/mgate/closearmia/openarmia)

**Overview**
A set of level >= 8 commands manages map gates/portals represented as `CItem` entries in the global `pItem` array. Commands create, destroy, close, open, move, and toggle gates, always broadcasting the resulting item state to players on the affected grid cell via `GridMulticast` with `MSG_CreateItem`/`MSG_UpdateItem` messages.

**Detailed description**
`set gate <index> <x> <y> <rotate>` calls `CreateItem` to instantiate an item at a grid coordinate, sends a `MSG_CreateItem` to the cell with an `ItemID` encoded as `index + 10000` (an ID-space offset convention distinguishing created items), and aborts if the returned gate id is out of the valid range (`MAX_ITEM` or `<= 0`). `destroygate` retrieves the current item, multicasts it, then zeroes the `CItem`. `cgate`/`ngate` set `pItem[id].Delay` and call `UpdateItem` with `STATE_LOCKED`/`STATE_CLOSED` respectively, then re-broadcast. `ogate` sets `Delay = 1`, calls `UpdateItem(..., STATE_OPEN)`, and broadcasts a `MSG_UpdateItem`. `mgate` moves the hard-coded gate slot 45 to new coordinates. `closearmia`/`openarmia` scan the `pItem` array for the specific item index `4143` (the "Armia" gate) and lock/open it. All variants end with a confirmation `SendClientMessage` and broadcast the mutation to the local grid.

**Rule workflow**
```
1. level >= 8 caller issues a gate command with a target item id.
2. For create: CreateItem allocates item; id+10000 encodes the item id.
3. For open/close: pItem[id].Delay and UpdateItem STATE_* are set.
4. GetCreateItem/GetUpdateItem builds the broadcast message.
5. GridMulticast pushes the message to players at the gate cell.
6. Confirmation sent to the acting admin.
```

---

### Business Rule: NPC File Persistence (save / read, level >= 8)

**Overview**
The `save` and `read` commands serialize the acting character's full `STRUCT_MOB` record to and from a binary file under the `./npc/` directory. `save` writes the current mob structure; `read` loads a mob file back into the character, restoring equipment and scores.

**Detailed description**
Both commands construct the path as `"./npc/" + <sval1>` using `strcpy`/`strcat` into a fixed 256-byte `temp` buffer. `save` first recomputes current score via `GetCurrentScore`, then opens the file with `_O_CREAT | _O_RDWR | _O_BINARY` and writes the raw `sizeof(pMob[conn].MOB)` bytes via `_write`. On open failure it sends "fail - save file" and returns. `read` opens read-only, reads the raw struct, closes, re-sends all equipment slots, sets `MobName` to the filename, copies `CurrentScore` into `BaseScore`, recomputes current score, and re-sends score/etc. This implements a save/restore of character (used to author NPCs) in raw binary format with no versioning or validation of the loaded data.

**Rule workflow**
```
1. level >= 8 caller issues "save <file>" or "read <file>".
2. Path built as ./npc/<file>.
3. save: GetCurrentScore then binary-write pMob[conn].MOB.
4. read: binary-read into pMob[conn].MOB, refresh equip/score/etc.
5. Failure to open file returns an error message; success confirms.
```

---

### Business Rule: Billing Mode and Server Timing Configuration (level >= 3)

**Overview**
The level >= 3 block controls global server configuration flags: the timer interval, `saveall`, a decay flag, and a `set` block that configures billing mode, free-exp, char-select billing, potion count/delay, party bonus, and guild-board flags.

**Detailed description**
`timer` sets the main window timer via `SetTimer(hWndMain, TIMER_MIN, ival1, NULL)` but enforces a minimum of 5000 ms (5 seconds) — values below this are rejected with "SET TIMER can't be less than 5 sec". `saveall` iterates all `MAX_USER` users in `USER_PLAY` mode, calls `CharLogOut` for each, and logs "saveall". The billing `set` block sets `BILLING` mode; when mode is 2 or 3 (billing-server-dependent modes) it first verifies `BillServerSocket.Sock != NULL` (must be connected) before allowing the change, otherwise it returns "not connected to billing server.+billconnect first". Other flags (`FREEEXP`, `CHARSELBILL`, `POTIONCOUNT`, `PotionDelay`, `PARTYBONUS`, `GUILDBOARD`) are simple global scalar assignments, each followed by `DrawConfig(1)` to refresh the config UI.

**Rule workflow**
```
1. level >= 3 caller issues "timer <ms>" or "set billmode <mode>" etc.
2. timer enforces >= 5000 ms minimum.
3. billmode 2/3 requires an active billing socket connection.
4. Global flag updated; DrawConfig refreshes the admin UI.
5. Confirmation sent to the caller.
```

---

### Business Rule: Reboot and World Mutation (level >= 2)

**Overview**
The level >= 2 block covers server lifecycle (`reboot`, `delayreboot`), NPC/mob generation and creation (`generate`, `create`), item gifting (`gift`), player flags (`cp`, `frag`), logging restart (`log`), guild battle scheduling (`guildday`, `guildhour`, `newbiehour`, `weekmode`), data reloads (`reloadnpc`, `reloadguild`, `readguildname`), treasure table editing (`trtarget`, `trsource`, `trrate`), and the event/drop tuning `set` subcommands (`statsapphire`, `battleroyal`, `ev*`, `double`, `deadpoint`, `dungeonevent`, `champ`, `chall`, `drop`).

**Detailed description**
`reboot` sets `ServerDown = 1`. `delayreboot` computes `ServerDown = -1 * ServerIndex * (ival1 * 2)` and clamps it to the range `[-1000, 1]`. `generate` validates the generation index against `mNPCGen.NumList` and the connection bounds before calling `GenerateMob` at the target cell. `create` calls `CreateMob` and maps its integer return code to user-facing messages: 1 = success, 0 = no monster file in boss directory, -1 = no empty mob slot, -2 = no empty mob grid. `gift` converts `_` to spaces in the target mob name, searches the global `pMob` array (1..MAX_MOB), and places the built item either into a NPC's carry (finding the first free slot in a shop-list layout) or, for a user, via `PutItem`. `cp`/`frag` call `SetPKPoint`/`SetTotKill` and re-broadcast the mob. Guild scheduling commands enforce ranges: `guildday` in [0,6], `guildhour`/`newbiehour` in [0,23]; `weekmode` accepts [0,5] to force a mode (with `WeekMode = ival1 - 1`, wrapping -1 to 5) or any other value to clear forcing (`ForceWeekMode = -1`). Treasure editing validates `trtarget` indices (room 1-8, target 1-5) and writes `g_pTreasure[idx].Target[tgt]` plus rate fields; `trsource` validates room 1-8. The `drop` subcommand enforces a rate range [0, 8000] and interprets position `ival2` in three bands: 16 applies to all 64 slots, 1-7 applies to an 8-slot block `(ival2-1)*8 + i`, and 8-15 applies to a single slot `56 + (ival2-8)`; invalid positions are rejected. Event tuning subcommands (`evstart`, `evend`, `evitem`, `evrate`, `evindex`, `evdelete`, `evon`, `evnotice`) write event globals and print a status string; `evon` refuses to enable the event unless `evStartIndex`, `evEndIndex`, `evRate`, and `evItem` are all non-zero. `champ`/`chall` validate the zone index against `MAX_GUILDZONE` and write the charged/challenger guild before persisting via `CReadFiles::WriteGuild()`.

**Rule workflow**
```
1. level >= 2 caller issues a world/lifecycle command.
2. Numeric bounds validated where defined (generate, create, guild*, tr*, drop, evon).
3. Global state mutated (ServerDown, g_pTreasure, g_pDropBonus, event globals, guild zones).
4. Where applicable, config persisted (WriteGuild) and UI refreshed (DrawConfig).
5. Confirmation or specific error message returned.
```

---

### Business Rule: Weather, Moderation, Teleport, and Miscellaneous Commands (level >= 1)

**Overview**
The broadest privilege tier (level >= 1) contains the most commonly used GM operations: weather forcing, map attribute inspection, billing connection, treasure/mob EXP rebalancing, notice broadcasting, chat moderation (mute/muteall/desmuteall), snooping, class/skill/buff manipulation, killing mobs, teleportation, and summoning.

**Detailed description**
`weather` sets `ForceWeather`/`CurrentWeather` and calls `SendWeather`. `attmap` reads the map attribute at the target cell and reports it. `billconnect` reads `biserver.txt` for address/port, connects the billing socket via `BillServerSocket.ConnectBillServer`, sets `BILLING = 3`; it refuses if already connected or if the config file is missing. `impost` zeroes the EXP of the five `GuildImpostoID` NPCs. `trn` performs a double teleport (to 4000,4000 then back to target) as a refresh trick. `notice` broadcasts a database notice via `DBServerSocket.SendOneMessage`. `waterexp`/`nigexp` rebalance the EXP of water/nightmare room NPCs by rewriting their `.npc` files with `Exp = xp * Level / FREEEXP`; `svexp` rebalances all NPCs' leader/follower EXP with a formula that scales by `xp * Level / lv` (or `xp` when level < `lv`). `chiefnotice`/`chiefsummon` relay notices/summons to clan chiefs. `mute` looks up a user by name, verifies they are in `USER_PLAY`, and toggles `MuteChat`; `muteall`/`desmuteall` iterate users (skipping those below `USER_SELCHAR` mode) and set/unset `MuteChat`. `snoop` toggles the `MSV_SNOOP` bit (0x01) of the merchant flag and refreshes score. `learn` clears learned skills, `class` sets the character class, `buff`/`nobuff` set/clear affect slots. `fame` validates guild id in (0,65536) and increments guild fame, broadcasting via DB socket. `killkefra`/`createkefra` manage the Kefra boss (by `GenerateIndex == KEFRA_BOSS`), `kill` finds mobs by name (with `_`->space) and kills them via `MobKilled`. `npko` hard-codes three account names (VERBANSKI, MATEUS654, PTR0X) and writes their IPs to `../../DBSRV/Run/Admin.txt`. `teleport` converts coordinates < 100 to grid cells via `x*128 + 64`, bounds-checks against `MAX_GRIDX/MAX_GRIDY`, then calls `DoTeleport`. `kick` finds a user, refuses to kick users of equal or higher level ("Can't kick equal or high level admin"), then `CharLogOut` + `CloseUser`. `gsummon`/`allsummon` summon a guild or the whole server to the acting admin's target cell.

**Rule workflow**
```
1. level >= 1 caller issues a command.
2. Command-specific validation (mute user lookup, fame guild range, kick level guard, teleport bounds).
3. Global or per-user state mutated; file I/O for EXP rebalance.
4. Client/grid/DB feedback via SendClientMessage, SendNotice, DBServerSocket, DoTeleport.
5. Confirmation returned; failures return specific error strings.
```

---

### Business Rule: Audit Logging of Admin Actions

**Overview**
Every admin command is logged before execution, and several high-impact mutations produce additional targeted audit records. This provides a basic accountability trail for GM actions.

**Detailed description**
At the top of `ProcessImple`, the raw command string is formatted as `"adm %s"` and written via `Log(logtemp, pUser[conn].AccountName, pUser[conn].IP)` when a real connection is present (`conn != 0`), or `Log(logtemp, "system", 0)` for the system/DB channel. Additional logs are emitted for item/equip mutations (`set item`, `set equip`, `gift`) via `Log("-system", "<MobName> - Set ... [effects]", pUser[conn].IP)`. The EXP-rebalance commands log file-save failures via `Log("-system", "fail - save npc", 0)`. `SaveAll` logs each saved user with `Log("saveall", AccountName, IP)`. This means all admin activity, including the raw parameters, is written to the server log, which doubles as both an audit trail and a potential injection vector since raw input is logged verbatim.

**Rule workflow**
```
1. Command received → "adm <str>" logged with actor identity.
2. High-impact mutations additionally log item/effect tuples.
3. File I/O failures logged under "-system".
4. Logs persist to the server log store for operational review.
```

---

## 4. Component Structure

The component is a single-file command dispatcher. Internal organization is by privilege tier, not by subsystem:

```
legacy/Code/TMSrv/
├── imple.cpp                    # ProcessImple + SaveAll (the entire component)
└── (header contracts declared in:)
    ├── Server.h:169,190         # SaveAll / ProcessImple prototypes
    ├── GetFunc.h                # GetCreateItem, GetCreateMob, SetPKPoint, SetTotKill
    ├── SendFunc.h               # SendNoticeChief, SendSummonChief, SendEmotion, ...
    ├── ProcessClientMessage.h   # message types (_MSG_*)
    └── ProcessDBMessage.h       # DB message types (_MSG_MessageDBImple, ...)
```

Within `imple.cpp`, the structure is:

```
ProcessImple(int conn, int level, char *str)
├── Variable declarations (cmd, sval1..8, ival1..8, dval2)
├── sscanf parsing (strings + integers)
├── Audit log of raw command
├── if (level >= 8)          # character/item/gate/NPC admin
│   ├── conn bounds check
│   ├── set <stat> (sanc, level, hp, mp, ac, dam, exp, str, int, dex, con,
│   │             coin, merchant, skillbonus, special0-3, scorebonus,
│   │             attackrun, critical, learned, special, classmaster,
│   │             reloadfile, gate, destroygate, cgate, closearmia,
│   │             openarmia, ogate, ngate, mgate, clearcarry, item, noatum)
│   ├── equip-set branch (ival1 in [1,MAX_EQUIP])
│   ├── post-mutation state refresh
│   ├── save <file>          # NPC binary save
│   ├── read <file>          # NPC binary load
│   └── name <name>          # rename + rebroadcast mob
├── if (level >= 3)          # server timing & billing config
│   ├── timer / saveall / decay
│   └── set (billmode, billfree, charselbill, potioncount, potiondelay,
│            partybonus, guildboard)
├── if (level >= 2)          # reboot, spawn, gift, event, drop tuning
│   ├── reboot / delayreboot
│   ├── generate / create / gift / cp / frag
│   ├── log / guildday / guildhour / newbiehour / weekmode
│   ├── reloadnpc / reloadguild / readguildname
│   ├── trtarget / trsource / trrate
│   ├── statsapphire / battleroyal
│   └── set (evstart, evend, evitem, evrate, evindex, evdelete, evon,
│            evnotice, double, deadpoint, dungeonevent, champ, chall, drop)
└── if (level >= 1)          # weather, moderation, teleport, misc
    ├── weather / attmap / billconnect
    ├── impost / trn / notice
    ├── waterexp / testegrid / svexp / nigexp
    ├── chiefnotice / chiefsummon / rebuild
    ├── mute / muteall / desmuteall / snoop
    ├── event / learn / class / buff / nobuff / soul / fame
    ├── killkefra / npko / kill / createkefra
    ├── partydif / rvrhour / gtorrehour / dropitem / maxnightmare
    ├── emotion / teleport / kick
    └── gsummon / allsummon
SaveAll()
└── iterate pUser[0..MAX_USER); USER_PLAY → CharLogOut + log
```

---

## 5. Dependency Analysis

### Internal Dependencies (functions/globals within TMSrv)

```
ProcessImple
├── Character state:  pMob[], pUser[], pItem[], pMobGrid[], mNPCGen
├── GetFunc:          GetCreateItem, GetCreateMob, SetPKPoint, SetTotKill
├── SendFunc:         SendNoticeChief, SendSummonChief, SendEmotion,
│                     SendWeather, SendClientMessage, SendScore, SendEtc,
│                     SendEquip, SendItem
├── Server.h helpers: SaveAll, BuildList, CreateItem, CreateMob, GenerateMob,
│                     PutItem, DoTeleport, MobKilled, CharLogOut, CloseUser,
│                     GetUserByName, SummonGuild, SummonServer, RebuildGenerator,
│                     GuildZoneReport, StartLog, StartChatLog, StartItemLog
├── CReadFiles:       ReadSancRate, ReadQuestsRate, ReadCompRate, ReadGuild,
│                     ReadAdmin, ReadNPCGenerator, WriteGuild
├── CReadFiles/CCastleZakum: ReadCastleQuest
├── Basedef globals:  g_pTreasure[], g_pGuildZone[], g_pDropBonus[64],
│                     GuildInfo[], pAdminIP[], BillServerSocket, DBServerSocket
├── BASE_*:           BASE_InitializeBaseDef, BASE_InitializeMessage,
│                     BASE_InitializeGuildName, BASE_ClearItem
└── Win32:            SetTimer, hWndMain, _open/_close/_read/_write, fopen/fscanf
```

### External Dependencies

| Dependency | Type | Purpose |
|-----------|------|---------|
| Win32 API (`windows.h`) | Platform | Timers (`SetTimer`, `hWndMain`), file descriptors |
| CRT file I/O (`_open`/`_read`/`_write`/`fopen`) | Standard library | NPC binary persistence, config file reads |
| DBSrv (database server) | Internal service (cross-process) | `_MSG_MessageDBImple` inbound; notice/fame outbound via `DBServerSocket` |
| Billing server | External service | `BillServerSocket.ConnectBillServer`, `biserver.txt` config |
| Filesystem (`./npc/`, `../../DBSRV/Run/Admin.txt`) | External storage | NPC data files, admin IP whitelist |

The component is entirely self-contained within the TMSrv process and depends heavily on global mutable singletons, making its call graph wide but its module boundary flat.

---

## 6. Afferent and Efferent Coupling

The component's "classes" are the functions and data structures it interacts with (C language; functions + global structs are the coupling units).

| Component (Function/Struct) | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|-------------------|
| ProcessImple | 2 | ~45 distinct callees / globals | High |
| SaveAll | 1 | 2 (CharLogOut, Log) | Low |
| pMob (global mob/char state) | High (mutated by many commands) | — | High |
| pItem (gate/item state) | High (gate/item commands) | — | High |
| mNPCGen (NPC generator list) | Medium (EXP rebalance, generate, reloadnpc) | — | Medium |
| g_pTreasure / g_pDropBonus / g_pGuildZone | Medium (tuning commands) | — | High (unbounded indexing) |

`ProcessImple` is the central hub: only two callers (`_MSG_MessageWhisper.cpp:791` and `ProcessDBMessage.cpp:132`) depend on it (low afferent coupling), but it fans out to the majority of the server's subsystems (very high efferent coupling). This is the classic God-object / high-fan-out pattern and represents a significant maintainability and testability burden.

---

## 7. Endpoints

This component does not expose network endpoints of its own. It is an in-process command dispatcher reached through two internal message/call entry points:

| Entry Point | Source | Mechanism | Description |
|-------------|--------|-----------|-------------|
| GM chat command `+...` to "gm"/"GM" | `_MSG_MessageWhisper.cpp:791` | In-process function call | Player-initiated admin command; `level = CharLevel - 1000` |
| `_MSG_MessageDBImple` DB message | `ProcessDBMessage.cpp:132` | Cross-process socket message | System/console admin command; `level` from DBSrv |

Outbound, the component drives messages through `DBServerSocket.SendOneMessage` (DB server), `BillServerSocket` (billing server), and `GridMulticast`/`SendNotice` (game clients), but it does not itself define HTTP/REST/gRPC/GraphQL endpoints.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv (database server) | Internal service | Inbound admin commands (`_MSG_MessageDBImple`); outbound notices (`_MSG_DBNotice`), guild fame (`_MSG_GuildInfo`) | Custom TCP socket (`CPSock`/`DBServerSocket`) | Binary structs (`MSG_*`) | None in component; socket assumed live |
| Billing server | External service | Billing mode 2/3 connectivity | Custom TCP (`BillServerSocket.ConnectBillServer`) | Text config (`biserver.txt`) | Refuses if already connected or no config file; requires connected socket before billing mode change |
| NPC file storage | Filesystem | `save`/`read`/EXP-rebalance NPC persistence | Binary file I/O (`_open`/`_write`/`_read`) | Raw `STRUCT_MOB` binary | Returns error message on open failure |
| Admin IP whitelist file | Filesystem | `npko` writes `../../DBSRV/Run/Admin.txt` | Text file write | `"%d %d.%d.%d.%d\n"` rows | Skips zero entries; no-op on NULL fp |
| Game clients (grid) | Internal | Broadcast gate/mob/item state, weather, notices | In-process `GridMulticast`/`SendNotice`/`SendWeather` | Binary `MSG_*` structs | None; best-effort multicast |

Error handling across integrations is minimal — mostly boolean "failed, returning" checks (file open, socket connect, mob lookup) with no retry, circuit-breaker, or resilience patterns.

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Command Dispatcher | `ProcessImple` `strcmp` chain over `cmd`/`sval1` | imple.cpp:46 | Centralized admin command routing |
| Privilege Tiering | Nested `if (level >= N)` blocks | imple.cpp:89-1164 | Coarse-grained access control |
| God Object / Procedural Switch | Single flat function with 90+ branches | imple.cpp (entire) | Admin command handling |
| Global Singleton State | `pMob`, `pUser`, `pItem`, `mNPCGen`, `g_pTreasure` etc. | Basedef.h | Shared mutable world state |
| Binary Serialization | `save`/`read` raw `STRUCT_MOB` to disk | imple.cpp:508-555 | NPC persistence |
| Config-flag mutation + UI refresh | `DrawConfig()` after global changes | imple.cpp (many) | Live config updates |
| Audit logging | `Log("adm <cmd>", ...)` before dispatch | imple.cpp:81-87 | Accountability |

The architecture is a flat, procedural command interpreter with no layering, no dependency injection, and no data-access abstraction. All state is accessed through process-global arrays, and command handlers inline both business logic and persistence directly.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Command parsing (imple.cpp:68-78) | Unbounded `sscanf` into fixed 128-byte buffers (`cmd`, `sval1..8`) and `%s` — no length limits | Buffer overflow / stack corruption from long admin input |
| High | Array indexing (trtarget/trsource/trrate/drop/champ/chall) | Numeric args used to index `g_pTreasure[8]`, `g_pGuildZone[5]`, `g_pDropBonus[64]` with partial bounds checks | Out-of-bounds read/write if bounds are missed |
| High | Global mutable state | Commands mutate `pMob`/`pItem`/`g_p*` with no locking, transactions, or rollback | Data inconsistency / corruption under concurrent access |
| High | Audit log (imple.cpp:81) | Raw admin input logged verbatim via `Log` | Log injection / potentially misleading audit records |
| Medium | `npko` (imple.cpp:1674) | Hard-coded account names (VERBANSKI, MATEUS654, PTR0X) and writes outside server dir | Fragile hardcoded identity; path coupling to DBSrv |
| Medium | Privilege model (level >= 1) | High-impact commands (teleport, kick, kill, summon, drop-rate, event) gated only by level >= 1 | Over-broad access if a single low-tier GM is compromised |
| Medium | Duplication (waterexp/nigexp/svexp) | Near-identical EXP-rebalance loops duplicated three times | Maintenance burden, drift risk |
| Medium | `save`/`read` binary I/O | No versioning, checksum, or validation of loaded `STRUCT_MOB` | Loading malformed/stale NPC files corrupts state |
| Low | `decay` (imple.cpp:586) | Handler is a no-op stub (assignment commented out) | Dead/misleading command |
| Low | Dead command `learned` (imple.cpp:230) | Computes bit and sends integer but does not mutate state | Confusing/incomplete feature |

---

## 11. Test Coverage Analysis

**No test files exist for this component or anywhere in the legacy codebase.** A project-wide search for test/spec files (excluding the ignored `.opencode` node_modules) returned zero results.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| ProcessImple (all 90+ commands) | 0 | 0 | 0% | None |
| SaveAll | 0 | 0 | 0% | None |
| Exp-rebalance (waterexp/nigexp/svexp) | 0 | 0 | 0% | None |
| Gate/item/char mutation handlers | 0 | 0 | 0% | None |

Test file locations: none found (searched `legacy/` for `*test*`/`*spec*`). The absence of automated tests is a material risk given the component's direct manipulation of shared global state, filesystem writes, and cross-process socket traffic. All behavior is validated only at runtime by operators.

---

## 12. Methodology Notes

- Component scope: the `imple` component is defined as `legacy/Code/TMSrv/imple.cpp` (the only file matching the `imple` name) plus its header contracts (`Server.h`).
- Folders ignored per parameters: `.git`, `.opencode` (both excluded from all searches).
- No project files were modified during this analysis.
- Confidence on implicitly documented rules (privilege thresholds, hard-coded accounts, magic item ranges) is flagged where inference was required; the tier thresholds and level-derivation are explicitly present in the source.
