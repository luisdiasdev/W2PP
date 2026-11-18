# Component Deep Analysis Report — Basedef Domain Contract

| Attribute | Value |
|---|---|
| Component | Basedef Domain Contract |
| Primary artifact | `Code/Basedef.h` |
| Size / lines | ~60 KB / 2,505 lines |
| Language / toolchain | C++ (MSVC 14.44.35207, Win32/x64), precompiled-free shared header |
| Consumers | TMSrv (game tier), DBSrv (persistence tier), CPSock |
| Paired implementation | `Code/Basedef.cpp` (6,814 lines) — analyzed separately as "Basedef Rules Engine" |
| Analysis scope | Header contract only: constants, structs, message payloads, table externs, prototypes |
| Analysis date | 2026-11-15 22:57:40 |
| Excluded paths | `.agents`, `.codegraph`, `.opencode`, `openspec` |
| Method | Full manual read of all 2,505 lines; static reference counting across all 78 `.cpp` / 21 `.h` translation units; struct layout verified empirically by compiling a layout probe against the real header with the project's toolchain (x86 and x64) |

---

## 1. Executive Summary

`Code/Basedef.h` is not a conventional header. It is the single normative specification of the W2PP domain: 262 named constants for capacities, ports, tuning values and flag bitmasks, plus 7 typed direction flags and 4 state values; 26 domain/persistence structures; 100 wire-message payload structures; 191 numeric message-type identifiers spread across 7 direction flags into 18 distinct namespaces; 101 `BASE_*` function prototypes; 47 `extern` global declarations for content tables. Both executables in the solution (`TMSrv`, `DBSrv`) compile `Basedef.cpp` independently and include this header in 80 translation units, so every rule in it is simultaneously a game rule, a wire-protocol rule and an on-disk storage rule.

Three structural properties define the component and its risk profile:

1. **The header is a frozen binary contract with no version marker.** `STRUCT_ACCOUNTFILE` is written to and read from disk with raw `_read`/`_write` of `sizeof(STRUCT_ACCOUNTFILE)` (verified: 7,808 bytes, offsets `Info`=0, `Char`=224, `Cargo`=3488, `Coin`=4512, `ShortSkill`=4516, `affect`=4580, `mobExtra`=5608, `Donate`=7752, `TempKey`=7756). Nothing in the file records a format version, magic number or length. Four `#pragma pack(push, 1)` regions cover five types (`STRUCT_RANKING`, `MSG_AccountLogin`, `MSG_AccountLogin_HWID`, `MSG_UpdateScore`, `MSG_AttackOne`) while all others use the compiler default packing of 8, so the 7,808-byte layout is an emergent property of field order, natural alignment and hand-written `char EMPTY[n]` filler blocks (`char EMPTY[30]` appears 5 times in `STRUCT_MOBEXTRA` alone, plus `int EMPTY[9]`).

2. **The protocol is uniquely addressable but base IDs are massively reused.** All 191 message constants were evaluated numerically: there are **zero exact-value collisions**, but base IDs are reused across namespaces for 30 distinct base values (e.g. base `1` maps to five different message types: `0x0401`, `0x0801`, `0x0101`, `0x4501`, `0x1001`). Direction is therefore not carried in the payload — it is carried only by the four-to-six high bits of `Type`, and dispatch correctness depends on every participant re-deriving the same flag values. `FLAG_NEW` (`0x4000`) is used by exactly 2 messages and is the only defined bit above `0x2000`; `0x8000` is unallocated and unused.

3. **The contract is declarative but only partially enforced at runtime.** `BASE_CheckPacket` — the only function in the system whose job is to validate that a received buffer's `Size` matches the declared payload structure — has its entire body inside a block comment (`Code/Basedef.cpp:6476-6594`) and now unconditionally returns `FALSE` (`Code/Basedef.cpp:6595`). The only size enforcement that remains is the transport guard `Size > MAX_MESSAGE_SIZE (8192) || Size < sizeof(HEADER)` in `CPSock::ReadMessage` (`Code/CPSock.cpp:397`). Consequently the 100 payload structures are trusted at face value once the outer frame is sane, and the largest declared payload (`MSG_NPAccountInfo`, 7,832 bytes) sits 360 bytes below the transport ceiling with no compile-time assertion protecting that margin.

Key findings: 4 domain structures are dead (`STRUCT_CAMP`, `STRUCT_LEVELBONUS`, and — by type name — `STRUCT_SEALOFSOUL`, `STRUCT_DAM`, both reachable only as embedded members); 55 fields are explicitly unnamed (`Unk*`, `EMPTY`, `Zero`, `Useless*`, `NotUsed`); 3 `TODO` markers sit on contract-level defects (`Code/Basedef.h:1095`, `:1511`, `:1523`); `MOB_PER_ACCOUNT` is fixed at 4 while `STRUCT_SELCHAR` hardcodes the literal `4` in 7 array dimensions; and the "global level" convention is implemented with two different offsets (`+MAX_LEVEL` in 9 sites, `+MAX_CLEVEL` in 4 sites, and `+MAX_CLEVEL + 1` in one site — an off-by-one at `Code/TMSrv/_MSG_AcceptParty.cpp:93`). There are no tests of any kind in the repository.

---

## 2. Data Flow Analysis

### 2.1 Header inclusion flow (compile time)

```
TMSrv/*.cpp (73 files)
  -> ProcessClientMessage.h / Server.h / CUser.h / CMob.h / CItem.h /
     GetFunc.h / SendFunc.h / CReadFiles.h / CNPCGene.h / CCastleZakum.h / CWarTower.h
     -> #include "..\Basedef.h"      (11 aggregator headers, Basedef.h:21 include guard)
  -> all 262 #define constants, 26 STRUCT_*, 100 MSG_* and 191 _MSG_* become TU-local symbols
DBSrv/*.cpp (5 files)
  -> CFileDB.h / CUser.h / CReadFiles.h / CRanking.h
     -> #include "..\Basedef.h"      (4 aggregator headers)
Basedef.cpp, CPSock.cpp
  -> #include "Basedef.h"             (direct)
```

### 2.2 Runtime data flow — inbound client packet through the contract

```
1. CPSock::ReadMessage (CPSock.cpp:353) validates INITCODE, then treats
   HEADER->Size (short at offset 0) as the frame length:
   reject if Size > MAX_MESSAGE_SIZE(8192) or Size < sizeof(HEADER)(12).
2. Header fields decrypted in place from offset 4..Size; CheckSum recomputed
   as Sum2 - Sum1 (CPSock.cpp:425-455).
3. TMSrv ProcessClientMessage (ProcessClientMessage.cpp:38):
   reject if ID < 0 || ID >= MAX_USER (1000)                     -> BR-08
   reject if ServerDown >= 120
   reject if Type == _MSG_Ping (0x0360)                          -> BR-02
   reject if ClientTick == SKIPCHECKTICK and sender is a client   -> BR-12
4. switch(std->Type) over exact 16-bit values (64 cases)          -> BR-02
5. Handler casts the raw buffer to a MSG_* structure from this header
   (no size check; BASE_CheckPacket is disabled)                  -> BR-01, BR-17
6. Handler indexes pUser[ID] (MAX_USER) / pMob[ID] (MAX_MOB) /
   pItem[...] (MAX_ITEM)                                          -> BR-08
7. Persistent mutation is projected into STRUCT_MOB / STRUCT_ITEM /
   STRUCT_MOBEXTRA instances that are byte-copies of on-disk records -> BR-03/04/05
8. Outbound mutation is re-serialized into MSG_UpdateScore (152 B),
   MSG_UpdateCarry (528 B), MSG_CreateMob (232 B), MSG_SetHpMp, etc. -> BR-17
```

### 2.3 Runtime data flow — persistence round trip (DBSrv)

```
1. TMSrv builds MSG_SavingQuit / MSG_DBSaveMob (both 2,696 bytes: _MSG header +
   Slot + STRUCT_MOB + Cargo[128] + Coin + ShortSkill[16] + AccountName +
   STRUCT_MOBEXTRA + affect[32] + Donate)                          -> BR-03
2. DBSrv ProcessClientMessage (DBSrv/Server.cpp:1339) gates on
   (std->Type & FLAG_GAME2DB) && 0 <= ID <= MAX_USER (note: ">", not ">=")  -> BR-02
3. CFileDB::ProcessMessage (CFileDB.cpp:236) switches on exact Type;
   slot validity is re-checked against MOB_PER_ACCOUNT (0..3)      -> BR-07
4. CFileDB::DBReadAccount / DBWriteAccount perform a single
   _read/_write of sizeof(STRUCT_ACCOUNTFILE) = 7,808 bytes at
   ./account/<FirstKey>/<ACCOUNTNAME>                              -> BR-03
5. Legacy tolerance: if _filelength(Handle) < 7,808 only
   memset(file->ShortSkill, 0, 8) is applied (CFileDB.cpp:2573-2574)
6. Rankings read the same file with a size heuristic
   (7,500..7,600 bytes) OR sizeof(STRUCT_ACCOUNTFILE) (CRanking.cpp:175)
```

### 2.4 Content-table flow (header externs)

```
CSV/txt on disk -> BASE_ReadItemListFile / BASE_ReadSkillBin / BASE_InitializeMobname
  -> fixed-size extern arrays declared in Basedef.h:2439-2503
     g_pItemList[MAX_ITEMLIST=6500]      (STRUCT_ITEMLIST, 140 B each)
     g_pSpell[MAX_SKILLINDEX=103]        (STRUCT_SPELL,    84 B each)
     g_pInitItem[MAX_INITITEM=256]       (STRUCT_INITITEM,  8 B each)
     EffectNameTable[MAX_EFFECTINDEX=128][24]
     g_pMessageStringTable[MAX_STRING=2000][128] (XOR 0x5A obfuscated, UI/strdef.bin)
     g_pNextLevel[MAX_LEVEL+2=401], g_pNextLevel_2[MAX_CLEVEL+202=401]
     pSummonBonus[MAX_SUMMONLIST=50], pTransBonus[5], g_pServerList[10][11][64]
  -> consumers index without bounds negotiation; the array dimension IS the rule
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---|---|---|---|
| BR-01 | Protocol invariant | Every wire message begins with a 12-byte header (`Size,KeyWord,CheckSum,Type,ID,ClientTick`); `Size` is the authoritative frame length and must be 12..8192 | `Basedef.h:925-946`; enforced at `CPSock.cpp:397` |
| BR-02 | Protocol invariant | Message identity = `(direction flags) OR base_id`; direction is encoded in the high bits of `Type` and is the only disambiguator of reused base IDs | `Basedef.h:932-941, 976-2325` |
| BR-03 | Persistence | A character file is exactly one 7,808-byte `STRUCT_ACCOUNTFILE`; layout is positional, unversioned and padding-sensitive | `Basedef.h:840-856` |
| BR-04 | Persistence | A character record is exactly 816 bytes (`STRUCT_MOB`) with fixed 16-slot equip and 64-slot carry arrays at fixed offsets 140 and 268 | `Basedef.h:438-481` |
| BR-05 | Encoding | An item is 8 bytes: `short sIndex` plus 3 `{effect,value}` byte pairs, with sanctification/success packed into a single `cValue` byte using two disjoint numeric ranges | `Basedef.h:398-412`; decode at `Basedef.cpp:2100-2285` |
| BR-06 | Capacity/geometry | Carry grid 9x7 = 63 usable cells for 64 slots (slot 63 reserved by `KILL_MARK`); cargo grid 9x14 = 126 cells for 128 slots | `Basedef.h:76-93, 166` |
| BR-07 | Capacity | One account owns at most 4 characters (`MOB_PER_ACCOUNT`); slot must satisfy 0 <= slot < 4 everywhere | `Basedef.h:71`; `CFileDB.cpp:259,865,1036,1142` |
| BR-08 | Index-space | Users occupy indices 0..999 (`MAX_USER`); NPC/mob instances start at index 1000; ground items 0..4999; a DB session index is `server*MAX_USER + slot` with `MAX_DBACCOUNT = 10000` | `Basedef.h:56,60,103,107`; `CFileDB.cpp:247-248` |
| BR-09 | Access control | 10 user slots (`ADMIN_RESERV`) are reserved: a login on `conn >= MAX_USER - ADMIN_RESERV` is refused | `Basedef.h:57`; `_MSG_AccountLogin.cpp:30` |
| BR-10 | Topology | Fixed ports 8281 (game), 7514 (DB), 8895 (admin); at most 10 game servers per DB, 10 server groups, 11 server slots | `Basedef.h:44-50`; `Server.cpp:3634`, `DBSrv/Server.cpp:586-587` |
| BR-11 | Protocol version | Client must send `ClientVersion == APP_VERSION (6975)` or the login is closed with a version-mismatch message | `Basedef.h:42`; `_MSG_AccountLogin.cpp:44-54` |
| BR-12 | Anti-replay | `ClientTick` is validated per action; the sentinel `SKIPCHECKTICK (235543242)` marks internally generated packets that must bypass all tick checks | `Basedef.h:172`; `_MSG_Attack.cpp:59-205,1730` |
| BR-13 | Addressing | `ESCENE_FIELD (30000)` is the reserved `ID` for server-originated notifications (not a user index) | `Basedef.h:170`; `_MSG_Buy.cpp:62` |
| BR-14 | Progression | Mortal/Arch levels are bounded by `MAX_LEVEL` (399) and Celestial levels by `MAX_CLEVEL` (199); Celestial levels are flattened to a global level by adding an offset | `Basedef.h:117-118`; `Basedef.cpp:1184,3476,3996` |
| BR-15 | Classification | `ClassMaster` namespace is MORTAL=2, ARCH=1, CELESTIAL=3, CELESTIALCS=4, SCELESTIAL=5 while playable `Class` is 0..3 (`MAX_CLASS`); the two must not be interchanged | `Basedef.h:115,120,178-182` |
| BR-16 | Bitmask | Status flags are two disjoint namespaces: `RSV_*` (9 bits in the 16-bit `STRUCT_MOB::Rsv`) and `MSV_SNOOP` (bit 0 of `STRUCT_MOB::Merchant`), which is overloaded with a numeric merchant ID | `Basedef.h:184-194, 442, 445` |
| BR-17 | Protocol variant | Payload variants reuse one `Type` and are discriminated by `Size`: `MSG_Attack` (168 B) / `MSG_AttackOne` (68 B) / `MSG_AttackTwo` (80 B) | `Basedef.h:2011-2133`; `_MSG_Attack.cpp:1062` |
| BR-18 | Numeric cap | HP/MP are capped at 2,000,000,000; physical damage at 1,000,000; magic damage at 254; 32 concurrent affects; affect duration uses 450 units per hour and 10,800 per day | `Basedef.h:203-208,122` |
| BR-19 | Content cardinality | Fixed-size content tables are part of the contract; their dimensions are compile-time rules (6500 items, 103 skills, 256 init items, 128 effect names, 2000 strings, 4096 guilds) | `Basedef.h:139-152, 2454-2471` |
| BR-20 | Namespace allocation | NPC-generator ID ranges are allocated by hardcoded numeric defines, some of which deliberately overlap (`*_END` equals the last `*_MOB_n`, `*_BOSS_INITIAL` equals a `*_MOB_n`) | `Basedef.h:278-391` |
| BR-21 | State machine | Ground items and map gates carry a 4-value state (`STATE_NOTHING/OPEN/CLOSED/LOCKED` = 0..3) transported in `MSG_UpdateItem::State` and mirrored in the height grid | `Basedef.h:1834-1837`; `Code/TMSrv/GetFunc.cpp:1220` |

---

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 — 12-byte message header and Size-driven framing

**Overview**
Every packet exchanged between client, game server, database server and NP tool starts with the same 12-byte prologue, injected into every payload structure by the `_MSG` macro. No payload may exist without it, and no transport code may reinterpret it.

**Detailed description**

The macro at `Code/Basedef.h:925-930` expands to six members in a fixed order:

```
short Size;            // offset 0  authoritative frame length
char  KeyWord;         // offset 2  index into the 256-entry substitution table
char  CheckSum;        // offset 3  arithmetic check applied to bytes 4..Size-1
short Type;            // offset 4  message identity = flags | base id
short ID;              // offset 6  connection index, or ESCENE_FIELD
unsigned int ClientTick; // offset 8  sender timestamp / anti-replay token
```

Twelve bytes with no implicit padding under either default packing or `#pragma pack(1)`, because the layout already satisfies 2/4-byte alignment. This was verified empirically: `sizeof(MSG_STANDARD) == 12` on both x86 and x64, and every one of the 100 `MSG_*` structures begins with these twelve bytes at identical offsets.

The transport treats `Size` as the framing length and nothing else. `CPSock::ReadMessage` (`Code/CPSock.cpp:386-411`) first demands at least `sizeof(HEADER)` bytes, then reads the little-endian `short` at the current read position, rejects any value outside `[12, 8192]` by resetting both buffer cursors and raising `ErrorCode = 2`, and finally refuses to consume a partial frame (`if (Size > Rest) return 0`). `CPSock::AddMessage` overwrites the caller's `Size`, `KeyWord`, `CheckSum` and `ClientTick` on the way out (`CPSock.cpp:535-541`), so a producer cannot forge a frame length; `Size` is therefore a transport-owned field that the payload definitions merely reserve room for.

The contract-level enforcement that would compare `Size` against the declared structure is present but inert. `BASE_CheckPacket(MSG_STANDARD*)` at `Code/Basedef.cpp:6475` opens a block comment on line 6476 that contains roughly 90 `if (m->Type == _MSG_X && m->Size != sizeof(MSG_X)) code = 1;` statements, closes it at line 6594, and then executes `return FALSE;` at line 6595 — so the function always reports "invalid" while performing no comparison at all. Its only call site is behind `#ifdef _PACKET_DEBUG` in `CPSock::AddMessage` (`CPSock.cpp:544-552`), i.e. it is a send-side debugging hook, never a receive-side validator, and even there it can only ever log a warning. The consequence for the header is that the 100 payload definitions are read as trusted interpretations of a buffer whose only guarantee is `12 <= Size <= 8192`; a truncated `MSG_Trade` (156 B declared) would be cast to a 156-byte type over a shorter allocation and read past the frame.

This rule also fixes the ceiling relationship that the header must respect: the largest declared payload, `MSG_NPAccountInfo` at 7,832 bytes (12 + 7,808 + 4 + 4 + 4), is within 360 bytes of `MAX_MESSAGE_SIZE` (8,192, defined in `Code/CPSock.h:38`, not in this header). Adding a single field of 361 bytes or more to `STRUCT_ACCOUNTFILE`, or raising `MAX_CARGO`, would silently exceed the transport limit and make the NP account-info path unframeable, with no compile-time or run-time diagnostic at the definition site.

**Rule workflow**

```
1. Producer fills a MSG_* structure, sets Type/ID, leaves Size unspecified.
2. CPSock::AddMessage computes KeyWord = pKeyWord[rand()%256*2], writes
   Size = caller-supplied byte count, ClientTick = CurrentTime, CheckSum = 0.
3. Bytes 4..Size-1 are substituted per (i & 3) and Sum2-Sum1 is written to CheckSum.
4. Receiver accumulates bytes; ReadMessage requires >= 12 bytes buffered.
5. Size is read and validated: 12 <= Size <= 8192, else cursors reset, ErrorCode=2.
6. If Size > bytes remaining, the frame waits for more data.
7. Bytes 4..Size-1 are un-substituted in place and CheckSum verified
   (mismatch sets ErrorCode=1 but the frame is still returned).
8. Dispatcher receives a char* whose Size field is now the only length witness;
   no sizeof comparison is performed (BASE_CheckPacket disabled).
```

**Evidence**: `Basedef.h:925-930` (macro), `Basedef.h:943-974` (the five standard header-bearing structs), `CPSock.h:42-50` (duplicate `HEADER` definition), `CPSock.cpp:386-411` (Size validation), `Basedef.cpp:6475-6560` (disabled size validator).

---

### Business Rule: BR-02 — Flag-partitioned message-type namespace

**Overview**
A message is identified by a single 16-bit `Type` whose value is `base_id | direction_flags`. Seven direction flags partition the identifier space into 6 single-direction and 12 composite namespaces (18 distinct masks); base IDs are reused freely between namespaces.

**Detailed description**

The flag constants at `Code/Basedef.h:932-941` are `const int`, not `#define`, and occupy five contiguous bits plus one outlier:

```
FLAG_GAME2CLIENT 0x0100   FLAG_CLIENT2GAME 0x0200
FLAG_DB2GAME     0x0400   FLAG_GAME2DB     0x0800
FLAG_DB2NP       0x1000   FLAG_NP2DB       0x2000
FLAG_NEW         0x4000
```

Evaluation of all 191 `const short _MSG_*` declarations in the header yields 18 distinct flag masks and **zero exact-value collisions**, which means the wire identifier space is unambiguous. It also yields 30 base IDs that are reused across namespaces — for example base `1` is `0x0401` (DB→game message panel), `0x0801` (game→DB new account), `0x0101` (game→client message panel), `0x4501` (new ranking data) and `0x1001` (NP credential request); base `31` is `0x041F`, `0x081F` and `0x0D1F`. The flag bits are therefore not documentation — they are the entire disambiguation mechanism, and any consumer that masks or truncates `Type` (for example by comparing `Type & 0xFF`, or by storing it in a `char`) silently aliases unrelated handlers.

Enforcement of the namespace is done independently by each tier, with asymmetric strictness. TMSrv's client dispatcher (`Code/TMSrv/ProcessClientMessage.cpp:42`) validates only the `ID` range and never tests the flag bits, so any `Type` value that happens to equal a case label is accepted regardless of its declared direction; because all 191 values are numerically distinct, this cannot cause cross-handler aliasing today, but it means the direction bits are advisory on the client path and a future duplicate value would be caught by neither side. The DB path is stricter: `Code/DBSrv/Server.cpp:1343` rejects anything without `FLAG_GAME2DB`, and `Code/TMSrv/ProcessDBMessage.cpp:43` rejects anything without `FLAG_DB2GAME`; the NP path rejects anything without `FLAG_NP2DB` and bounds `ID` by `MAX_DBACCOUNT` (`DBSrv/Server.cpp:630,1467`). Because the flags are OR-combined for messages that traverse two hops (e.g. `_MSG_DBNotice = 30 | FLAG_DB2GAME | FLAG_GAME2DB | FLAG_GAME2CLIENT = 0x0D1E`), a multi-hop message must satisfy every hop's mask simultaneously — which is why 58 messages carry the symmetric `0x0300` pair and 6 carry `0x0C00`.

The header also documents a de-facto allocation rule that has never been written down: base IDs 1..62 are used densely in the four main namespaces; 51..229 are used only inside `0x0300`; 222/223 are the only values above 220 in `0x0F00`; and `FLAG_NEW` (`0x4000`) is reserved for the two ranking messages. There is no registry, no free-list and no compile-time uniqueness assertion; uniqueness has so far been maintained by manual care, and the reader can verify it only by recomputing 191 values.

**Rule workflow**

```
1. Author picks a base id and one or more direction flags.
2. Constant is declared as `const short _MSG_Name = (base | FLAG_...);`
   (Basedef.h:976 onward) and a `struct MSG_Name { _MSG; ... };` follows.
3. Producer sets sm.Type = _MSG_Name; ID = target connection.
4. TMSrv inbound path: exact-value switch (no flag test, no base-id test).
5. TMSrv<-DB path: `std->Type & FLAG_DB2GAME` is required before the switch.
6. DB<-TMSrv path: `std->Type & FLAG_GAME2DB` is required before the switch.
7. DB<-NP path: `std->Type & FLAG_NP2DB` and ID < MAX_DBACCOUNT required.
8. Unknown Type falls through the switch with no default case
   (ProcessClientMessage.cpp:311) and is silently discarded.
```

**Evidence**: `Basedef.h:932-941` (flags), `Basedef.h:976-2325` (191 constants), `ProcessClientMessage.cpp:42-66,311`, `DBSrv/Server.cpp:1343`, `ProcessDBMessage.cpp:43`, `DBSrv/Server.cpp:630`.

---

### Business Rule: BR-03 — 7,808-byte unversioned character file

**Overview**
`STRUCT_ACCOUNTFILE` is the atomic unit of persistence. Exactly one instance is written per account per save, as a single raw `_write` of 7,808 bytes, with no header, magic value, version field or checksum.

**Detailed description**

The structure at `Code/Basedef.h:840-856` documents its own offsets in inline comments, and those comments were verified correct by compiling against the header: `Info` at 0 (220 bytes, padded to 224 by the 8-byte alignment of `STRUCT_MOB`), `Char[4]` at 224 (4 x 816 = 3,264), `Cargo[128]` at 3,488 (1,024), `Coin` at 4,512, `ShortSkill[4][16]` at 4,516 (64), `affect[4][32]` at 4,580 (1,024), `mobExtra[4]` at 5,608 (4 x 536 = 2,144), `Donate` at 7,752, `TempKey[52]` at 7,756, total 7,808. The 4 bytes between `Info` ending at 220 and `Char` starting at 224, and the gap between `affect` ending at 5,604 and `mobExtra` starting at 5,608, are implicit padding, not declared filler — a detail that matters because the file writer copies the raw struct including padding.

The format is positional and therefore fragile in a very specific way. `STRUCT_ACCOUNTINFO` ends at 212 but `int Year` is placed at 212 and `int YearDay` at 216, so the 220-byte `Info` block has no slack; `STRUCT_MOB` relies on the compiler inserting 3 bytes between `Quest` (offset 24) and `Coin` (offset 28) and 6 bytes of tail padding after `Resist[4]`; `STRUCT_MOBEXTRA` is built almost entirely out of hand-written filler (`char EMPTY[30]` inside `QuestInfo.Mortal`, `.Arch`, `.Celestial`, inside `SaveCelestial[]`, plus `int EMPTY[9]` at the end) so that future additions can be absorbed inside an existing hole instead of shifting `Donate` and `TempKey`. There is no `static_assert` anywhere in the repository (verified: zero occurrences) and no compile-time or run-time size check on open, so a field insertion anywhere except inside a designated `EMPTY` block silently reinterprets every account file on disk.

Two independent readers compensate for the absence of a format marker by using size heuristics rather than validation. `CFileDB::DBReadAccount` (`Code/DBSrv/CFileDB.cpp:2518-2577`) calls `_filelength`, reads `sizeof(STRUCT_ACCOUNTFILE)` bytes regardless of the actual length, and only if `length < reqsz` performs `memset(file->ShortSkill, 0, 8)` — note that this zeroes 8 of the 64 bytes of `ShortSkill`, apparently a symptom of an older 8-byte field having occupied that address. `CRanking.cpp:175` accepts a file if `(fsize >= 7500 && fsize <= 7600) || fsize == sizeof(STRUCT_ACCOUNTFILE)`, which implies the loader has historically had to accept two vintages. `DBExportAccount` and `DBWriteAccount` (`CFileDB.cpp:2444`, `:2504`) write the whole struct unconditionally.

A second, distinct persistence unit follows the same rule set: `STRUCT_CAPSULE` (`Code/Basedef.h:610-614`, 1,352 bytes = `STRUCT_MOB` 816 + `STRUCT_MOBEXTRA` 536) is read and written the same way (`CFileDB.cpp:1648-1650`, `:1723-1730`, `:1833-1835`), and `STRUCT_SELCHAR` (840 bytes) exists purely as the "character selection screen" projection, built field-by-field from `STRUCT_ACCOUNTFILE` in `DBGetSelChar` (`CFileDB.cpp:2611-2631`) rather than by pointer aliasing.

**Rule workflow**

```
1. Game tier assembles MSG_SavingQuit / MSG_DBSaveMob carrying one STRUCT_MOB,
   Cargo[128], Coin, ShortSkill[16], STRUCT_MOBEXTRA, affect[32], Donate.
2. DB tier validates slot (0..3) and account name, then copies fields into a
   local STRUCT_ACCOUNTFILE (memset 0 first: CFileDB.cpp:110,261,837).
3. DBWriteAccount opens ./account/<FirstKey>/<ACCOUNTNAME> and performs one
   _write of 7,808 bytes; no length prefix, no version, no checksum.
4. On load, DBReadAccount performs one _read of 7,808 bytes; a short file
   leaves the tail as whatever the caller's memset produced.
5. Field-level sanitisation happens after the read: AccountName[15] and [14]
   forced to 0, AccountPass[15] and [14] forced to 0 (CFileDB.cpp:2520-2523),
   and reserved-name prefixes COM0-COM9 / LPT0-LPT9 rejected.
6. `TempKey[52]` is a transfer/session token: it is memcmp'd against
   `MSG_AccountLogin::Zero[52]` (CFileDB.cpp:664-666) and is written from
   `MSG_DBCNFServerChange::Enc[52]` on server transfer (CFileDB.cpp:1984); a
   match or mismatch decides whether the account record is reset.
7. Rankings read the same file with an additional 7,500..7,600 byte tolerance.
```

**Evidence**: `Basedef.h:840-856`, measured layout table (section 6.4), `CFileDB.cpp:2444,2504,2518-2577`, `CRanking.cpp:175-180`, `Basedef.h:610-614`.

---

### Business Rule: BR-04 — 816-byte character record layout

**Overview**
`STRUCT_MOB` is the 816-byte identity of a character or NPC: name, clan, merchant, guild, class, quest byte, coin, 64-bit experience, warp bookmark, two 48-byte score blocks, 16 equipment slots, 64 carry slots, learned-skill bitmap and derived combat fields.

**Detailed description**

Verified offsets on both x86 and x64: `Coin` at 28, `Exp` at 32, `BaseScore` at 44, `CurrentScore` at 92, `Equip[16]` at 140, `Carry[64]` at 268, `LearnedSkill` at 780, `GuildLevel` at 800, `Resist[4]` at 806, total 816. Three 8-byte alignment events (the `long long Exp` at 32, and the struct tail after 810) account for 9 bytes of padding, so the record is not byte-tight and cannot be described by a flat field list.

The dual-score design is the core of the rule: `BaseScore` holds the unmodified allocation (including a `Level` field that is authoritative for Mortal/Arch characters) and `CurrentScore` holds the effective value after equipment, affects, buffs and breeding. `BASE_GetCurrentScore` recomputes `CurrentScore` from `BaseScore` plus derivations. The related tamper check, `BASE_GetMobCheckSum` (`Basedef.cpp:2503-2517`), is designed to sum equipment and carry indices into a single value — but it begins by `memset`-ing `BaseScore` to zero, i.e. it destroys the allocation it is meant to protect, and it has no callers anywhere in the repository, so the checksum is declared and implemented but never used. `Level`, `Ac`, `Damage`, then four unsigned-byte fields (`Merchant`, `AttackRun`, `Direction`, `ChaosRate`), then `MaxHp`, `MaxMp`, `Hp`, `Mp`, then `Str/Int/Dex/Con` and `Special[4]`.

Two field-level defects are baked into the layout. First, `SPX` and `SPY` are both commented "The Y position saved by the stellar gem" (`Basedef.h:452-453`), a copy-paste artefact that makes the X coordinate's documented meaning ambiguous; the same duplication recurs in `STRUCT_MOBEXTRA.SaveCelestial` at `Basedef.h:550-551`. Second, resistances are represented twice: `char Resist[4]` in `STRUCT_MOB` (fire/ice/thunder/magic as signed bytes) and the 16-bit `unsigned short Rsv` bitfield with `RSV_*` bits up to `0x100`; `Resist` is what gets sent to the client in `MSG_UpdateScore`, while `Rsv` is used server-side for immunity checks, so the two can and do diverge.

`LearnedSkill` is a signed 32-bit field used as a skill bitmap with bit index = `(global skill number % MAX_SKILL)`, i.e. 24 usable bits in 4 groups of 8 (`Basedef.cpp:6346`: `int kind = skillnum % MAX_SKILL / 8 + 1`). Specific passive skills are then referenced by literal bit positions rather than named constants — `1 << 15` for the warrior critical armour passive (`Basedef.cpp:3210`), `0x20000`/`0x80000`/`0x200000` for wolf/bear/asta forms (`Basedef.cpp:3439-3441`), `0x100000` for a class-3 passive (`Basedef.cpp:2387`). Bits 24..31 are never used and are the only spare room in the record; there is no `MASK` or enumerator guarding them.

**Rule workflow**

```
1. BASE_ClearMob (Basedef.cpp:2945) memsets the record and re-zeroes BaseScore
   and CurrentScore individually - the only sanctioned initialiser.
2. On login the record is copied from STRUCT_ACCOUNTFILE.Char[slot] verbatim.
3. BASE_GetCurrentScore recomputes CurrentScore from BaseScore + Equip + affects
   + extra (ClassMaster, QuestInfo, SaveCelestial).
4. Equip slot semantics are positional and undocumented: [0] face/mount-model
   (Server.cpp:4202), [14] mount (Server.cpp:4180).
5. Derived combat fields (MaxHp/MaxMp/Damage/Ac) are written into CurrentScore
   and mirrored to the client through MSG_UpdateScore (152 B).
6. On save the record is written back into STRUCT_ACCOUNTFILE.Char[slot]
   with no re-derivation of BaseScore.
```

**Evidence**: `Basedef.h:414-481`, `Basedef.cpp:2503-2517,2945,3210,3439`, `Server.cpp:4180,4202`, measured offsets in section 6.4.

---

### Business Rule: BR-05 — 8-byte item with dual-range sanctification encoding

**Overview**
`STRUCT_ITEM` is 8 bytes: a signed 16-bit index plus three `{unsigned char cEffect; unsigned char cValue;}` pairs, aliased as three `short sValue`. Sanctification level and success count are packed into one of those `cValue` bytes using two disjoint numeric ranges.

**Detailed description**

The union at `Code/Basedef.h:398-412` exposes each effect triple both as `short sValue` and as two bytes. That aliasing is not cosmetic: the persistence layer and the client both read `sValue`, while all server-side interpretation reads `cEffect`/`cValue`. The three slots form an ordered list with first-free-slot semantics — `BASE_SetItemAmount` (`Basedef.cpp:2304-2338`) searches slots 0,1,2 for `EF_AMOUNT`, and if none exists, writes `EF_AMOUNT` into the first slot whose `cEffect == 0`; if all three are occupied the amount is silently dropped. The same first-match pattern governs sanctification, amount, gem and growth-rate lookups (`Basedef.cpp:2100-2237`), so effect ordering is a de facto priority.

The sanctification encoding is the most intricate rule in the header. `BASE_SetItemSanc` (`Basedef.cpp:2239-2285`) writes `cValue = sanc + 10*success` for `sanc` 0..9, but for `sanc` 10..15 it writes `230 + success`, `234 + success`, `238 + success`, `242 + success`, `246 + success`, `250 + success` respectively — six non-overlapping four-wide bands. `BASE_GetItemSanc` (`Basedef.cpp:2100-2141`) inverts this and then remaps the bands to `REF_10..REF_15` = 10, 12, 15, 18, 22, 27 (note: the constant names do not match their values — `REF_11` is 12, `REF_12` is 15), while any value below 230 falls through to `isanc % 10`. `BASE_GetItemGem` returns `(isanc - 230) % 4` and `-1` below 230. Consequently the single byte at `stEffect[n].cValue` encodes two orthogonal quantities, the "level" is nonlinear, and three different accessors decode it three different ways (`% 10`, `% 4`, remap table). `BASE_SetItemSanc` additionally refuses to create a sanctification effect: if none of the three slots already carries `EF_SANC` or an effect in 116..125, it returns FALSE without writing.

The item index itself (`sIndex`, signed 16-bit) is partitioned by numeric range for special families, again without named constants: 2320..2359 is mount territory (`MBabyId = sIndex - 2320` gating 10..39 in `Server.cpp:4211-4214`), 2330..2389 is excluded from sanctification and gem logic entirely, 2360..2389 is the "growth" family, 315..344 is the party face range, and face values 22..25/32 are rewritten to 21 or `MortalFace + 7` on character selection (`CFileDB.cpp:2618-2619`). `MAX_ITEMLIST` is 6,500, so `sIndex` has ~1,500 unused values above the table, and nothing prevents an out-of-table index from being stored and persisted.

**Rule workflow**

```
1. Item created: sIndex <= 0 means empty (BASE_ClearItem memsets 8 bytes).
2. Effects appended in slot order 0,1,2; first free slot wins.
3. Sanctification write: sanc 0..9 -> 10*success + sanc; sanc 10..15 -> 230/234/238/242/246/250 + success.
4. Success count is capped at 20 and sanc outside 0..15 resets to 0.
5. Read-back disambiguation: scan slots 0..2 for cEffect == EF_SANC or 116..125;
   the first match wins; band 230..253 maps to REF_10..REF_15; else value % 10.
6. Client-relevant projections use the raw sValue (MSG_UpdateCarry 528 B,
   MSG_SendItem 32 B, MSG_Trade 156 B).
7. Persistence writes the 8 bytes verbatim into Carry[64]/Cargo[128].
```

**Evidence**: `Basedef.h:398-412`, `Basedef.h:184-192` (`RSV_*` adjacent), `Basedef.cpp:2100-2285,2304-2338`, `CFileDB.cpp:2611-2631`, `Server.cpp:4202-4214`.

---

### Business Rule: BR-06 — Inventory grid geometry and the reserved kill-mark slot

**Overview**
Personal inventory is a 9x7 grid of 64 slots of which only 63 are placeable; warehouse is a 9x14 grid of 128 slots of which 126 are placeable. Carry slot 63 is not an item slot at all.

**Detailed description**

The constants are `CARRYGRIDX 9`, `CARRYGRIDY 7`, `CARGOGRIDX 9`, `CARGOGRIDY 14` (`Basedef.h:90-93`), `MAX_CARRY 64`, `MAX_CARGO 128` and `KILL_MARK 63`. The arithmetic is deliberately lossy: 9*7 = 63 leaves one carry slot unmodelled by the grid, and 9*14 = 126 leaves two cargo slots unmodelled. `BASE_CanCarry` / `BASE_CanCargo` (`Basedef.cpp:4440-4535`) enforce the grid bounds exactly as `xx + tx >= CARRYGRIDX` / `yy + ty >= CARRYGRIDY`, so the 64th slot physically cannot be reached by a grid placement. The missing slot is explained by `KILL_MARK = 63`: `Carry[63]` is repurposed as a persistent player-kill ledger, with `stEffect[0..2].cValue` used as counters and `cEffect` as flags (`GetFunc.cpp:2051-2142`). This means the inventory array in the on-disk record is heterogeneous — 63 real slots plus one counter record — and any generic loop over `MAX_CARRY` that treats slot 63 as an item operates on counter data.

The same file contains a latent defect in the mirror function: `BASE_CanCarry` writes the placement map with `CarryGrid[(yy + ty) * CARRYGRIDX + xx + tx] = i + 1` but tests occupancy with `CarryGrid[(yy + DestY) * CARRYGRIDY + xx + DestX]` (`Basedef.cpp:4449` versus `:4468`), i.e. the row stride switches from 9 to 7 between write and read; `BASE_CanCargo` has the same mismatch (`:4516` stride `CARGOGRIDX` = 9 versus `:4531` stride `CARGOGRIDY` = 14). Since the two constants differ for cargo (9 vs 14) but not for carry (9 vs 7 versus 9), the cargo path is the more clearly wrong one. Both grids are declared as `char CargoGrid[MAX_CARGO]` (128) on the stack, and `memset(CargoGrid, 0, MAX_CARGO-2)` clears only 126 of 128 bytes, leaving the last two cells uninitialised — consistent with the "126 usable cells" rule but achieved by accident. The slot-to-cell conversion in both functions also hardcodes the row width as `int tx = i % 9; int ty = i / 9;` instead of using `CARRYGRIDX`/`CARGOGRIDX`, so the assumed width is a literal even where the named constant exists.

Equipment is a separate flat space, `MAX_EQUIP 16`, with positional meaning that lives outside the header (slot 0 = face, slot 14 = mount). `MAX_TRADE 15` slots per trade window, `MAX_AUTOTRADE 12` listings with `MAX_AUTOTRADETITLE 24`-byte titles, and `MAX_SHOPLIST 27` (making `MSG_ShopList` 236 bytes) follow the same pattern: a capacity constant that only makes sense together with the struct that embeds it.

**Rule workflow**

```
1. Placement request carries (SourType, SourPos) and (GridX, GridY) or a target slot.
2. Type is translated by ITEM_PLACE_EQUIP(0)/CARRY(1)/CARGO(2) in GetItemPointer
   (Basedef.cpp:2340-2365); pos is bounds-checked against MAX_EQUIP/MAX_CARRY/MAX_CARGO.
3. Grid occupancy is built by walking the item's footprint from g_pItemGrid[8][4][2].
4. Each footprint cell must satisfy 0 <= x < CARRYGRIDX/CARGOGRIDX and
   0 <= y < CARRYGRIDY/CARGOGRIDY; the 64th carry / 127th-128th cargo cells are
   therefore unreachable by placement.
5. On conflict the occupied slot index is returned through *error.
6. Slot 63 of the carry array is never treated as an item; it holds kill counters.
```

**Evidence**: `Basedef.h:75-93,166`, `Basedef.cpp:2340-2365,4440-4535`, `GetFunc.cpp:2051-2142`.

---

### Business Rule: BR-07 — Four character slots per account, hardcoded

**Overview**
An account may hold at most four characters. The limit is expressed once as `MOB_PER_ACCOUNT` (`Basedef.h:71`) and re-expressed as the literal `4` in every array of `STRUCT_SELCHAR`.

**Detailed description**

`STRUCT_ACCOUNTFILE` uses the constant correctly: `STRUCT_MOB Char[MOB_PER_ACCOUNT]`, `unsigned char ShortSkill[MOB_PER_ACCOUNT][16]`, `STRUCT_AFFECT affect[MOB_PER_ACCOUNT][MAX_AFFECT]`, `STRUCT_MOBEXTRA mobExtra[MOB_PER_ACCOUNT]`. `STRUCT_SELCHAR` (`Basedef.h:765-778`) does not: it declares `short SPX[4]; short SPY[4]; char Name[4][16]; STRUCT_SCORE Score[4]; STRUCT_ITEM Equip[4][16]; unsigned short Guild[4]; int Coin[4]; long long Exp[4];` — eight literal `4`s. Both executables then encode the same limit a third time as inline range checks: `if (m->Slot < 0 || m->Slot >= MOB_PER_ACCOUNT)` in `_MSG_CharacterLogin.cpp:25`, and `slot < 0 || slot >= MOB_PER_ACCOUNT` at four sites in `CFileDB.cpp` (259, 865, 1036, 1142), while `CReadFiles.cpp:237,814` compares `Slot <= 3` and `CDBGetSelChar` loops `i < MOB_PER_ACCOUNT`.

The practical consequence is that raising `MOB_PER_ACCOUNT` from 4 to, say, 6 would compile cleanly, produce a larger `STRUCT_ACCOUNTFILE` (changing every offset after `Char`), and then fail to represent slot 4 or 5 on the character-selection screen because `STRUCT_SELCHAR` would still be 840 bytes with four-entry arrays — a silent partial upgrade rather than a compile error. It would also silently invalidate the `Slot <= 3` comparisons in `CReadFiles.cpp` and the `MSG_CNFNewCharacter`/`MSG_CNFDeleteCharacter` payloads, both of which embed `STRUCT_SELCHAR` and would grow as well, changing their wire size without any constant reflecting it.

`STRUCT_CAPSULE` shows the sanctioned pattern for the same problem: it composes `STRUCT_MOB` and `STRUCT_MOBEXTRA` by name rather than restating their layout, so it tracks changes automatically (1,352 = 816 + 536 today). `STRUCT_SELCHAR` is the counter-example: a hand-flattened projection that duplicates the layout of a `STRUCT_MOB` subset.

**Rule workflow**

```
1. Client requests character creation/login with an explicit Slot (int on the wire).
2. TMSrv validates 0 <= Slot < MOB_PER_ACCOUNT before forwarding (msg byte-for-byte rewritten).
3. DB validates the same range again; CReadFiles uses Slot <= 3 instead of the constant.
4. STRUCT_ACCOUNTFILE.Char[Slot] is read/written at 224 + 816*Slot.
5. Selection screen data is projected into STRUCT_SELCHAR (fixed 4 entries) by DBGetSelChar.
6. MSG_CNFNewCharacter / MSG_CNFDeleteCharacter carry that 840-byte projection
   (856 bytes on the wire including the header and tail padding).
```

**Evidence**: `Basedef.h:71,765-778,840-856,1300-1314`; `_MSG_CharacterLogin.cpp:25`; `CFileDB.cpp:259,865,1036,1142,2611-2631`; `CReadFiles.cpp:237,814`.

---

### Business Rule: BR-08 — Index spaces and the MAX_USER boundary

**Overview**
The process maintains five independent flat index spaces whose bounds are all compile-time constants: users 0..999, mobs 0..24999 (mobs above 999 are NPCs), ground items 0..4999, NPC generators 0..8191, and DB sessions 0..9999 addressed as `server * 1000 + user`.

**Detailed description**

`MAX_USER 1000` is described in the header itself as "Max users on CUser pUser and starting index of npcs and mobs" (`Basedef.h:56`), which makes the array `extern CUser pUser[MAX_USER]` and `extern CMob pMob[MAX_MOB]` (25,000) share one numbering scheme: index 500 is always a player, index 1,500 is always an NPC, and the boundary value 1,000 is a semantic constant, not just a size. Code relies on this repeatedly: `_MSG_Attack.cpp:890` distinguishes `idx >= MAX_USER` to decide whether a target is a player or an NPC, and `GetUserInArea` iterates only `0..MAX_USER` (`Server.cpp:4161`). Any change to `MAX_USER` therefore renumbers every persistent reference to NPC instances — including NPC-generator IDs, which are a separate space of 8,192 (`MAX_NPCGENERATOR`) and are addressed by hardcoded numeric literals in the header itself.

The DB tier composes a single global session index from the same constant: `MAX_DBACCOUNT = MAX_USER * MAX_SERVER = 10,000`, and `CFileDB::ProcessMessage` decomposes an incoming `ID` as `int svr = IdxName / MAX_USER; int id = IdxName % MAX_USER;` (`CFileDB.cpp:247-248`, repeated at 2371-2372, and in `CReadFiles.cpp:239,817`, `Server.cpp:650-651`). This arithmetic is only valid if `MAX_USER` is a power-of-ten-friendly divisor; it is an integer division, so changing `MAX_USER` changes the packing of every in-flight session index and the layout of `ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]` and `MSG_GuildReport.ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]` (1,024 bytes of payload, `Basedef.h:1132`).

`MAX_ITEM 5000` bounds the ground-item array (`CItem pItem[MAX_ITEM]`, `Server.h:235`), `MAX_MOB 25000` bounds `CMob pMob[MAX_MOB]`, and `MAX_NPCGENERATOR 8192` bounds the generator table. Combined with the fixed-size grid declarations in `Server.h` (`pMobGrid[MAX_GRIDY][MAX_GRIDX]`, `pItemGrid`, `pHeightGrid` = 32+32+16 MB), these constants are the entire memory budget of the game tier: roughly 80 MB of static grid plus the mob/user/item arrays, with no allocator or pool abstraction anywhere.

**Rule workflow**

```
1. Connection index `conn` is allocated from 0..MAX_USER-1; index 0 is a non-user sentinel (no session state).
2. Any handler that receives an `ID` first checks 0 <= ID < MAX_USER (TMSrv) or
   0 <= ID < MAX_DBACCOUNT (DB/NP paths) - but see R-25: two sites use `ID > MAX_USER`
   and therefore admit exactly one out-of-range value.
3. Index < MAX_USER means player-object; >= MAX_USER means NPC/mob instance.
4. DB session index = server_index * MAX_USER + user_index; decomposed by / and %.
5. Grid coordinates are bounded by MAX_GRIDX/MAX_GRIDY (4096) via g_HeightWidth/Height.
6. Map attribute lookups downsample: g_pAttribute[(y>>2) & 0x3FF][(x>>2) & 0x3FF],
   a 1024x1024 array behind a 4096x4096 coordinate space (Basedef.cpp:2595-2599).
```

**Evidence**: `Basedef.h:56,60,103,107,109`; `Server.h:54-56,64,229-235`; `CFileDB.cpp:247-248,2371-2372`; `Basedef.cpp:2591-2605`; `_MSG_Attack.cpp:890`.

---

### Business Rule: BR-09 — Ten reserved slots for administration

**Overview**
The top ten connection slots (`MAX_USER - ADMIN_RESERV` = 990..999) cannot be used for player login.

**Detailed description**

`ADMIN_RESERV 10` (`Basedef.h:57`) is referenced in exactly one place, the account-login handler: `if (conn <= 0 || (conn >= (MAX_USER - ADMIN_RESERV)))` sends the "reconnect" message, flushes and closes the socket (`_MSG_AccountLogin.cpp:30-39`). Conn 0 is excluded separately because index 0 is a non-user sentinel on both paths: the client dispatcher only maintains session state for `conn > 0` (`ProcessClientMessage.cpp:56`) and the DB dispatcher treats `ID == 0` as a global/broadcast message (`ProcessDBMessage.cpp:54-56`). The rule is a capacity-reservation policy expressed purely as a comparison of two constants; no array is sized by it and no other handler re-checks it, so a connection that becomes a player through another path (for example the internal/imple path at `imple.cpp`) is not constrained by the reservation.

Because the check happens after the socket has already been accepted and after `CPSock` has validated the framing, it is the first *semantic* gate in the login sequence and it runs before the version check, before the `Mode != USER_ACCEPT` check, and before MAC capture. The ordering means a rejected connection on a reserved slot still consumes a full `MSG_AccountLogin` parse and a message-table lookup, and the client is told to reconnect — which it will do into the same reserved slot, producing a loop bounded only by the client's retry logic. `ADMIN_RESERV` is also conceptually paired with `MAX_ADMIN 10` (`Basedef.h:58`, "Max number of NP that can connect to DB server") — the same number for two different reservations, with no shared constant.

**Rule workflow**

```
1. Socket accepted, INITCODE verified, MSG_AccountLogin framed and decrypted.
2. Handler takes conn and compares conn <= 0 || conn >= MAX_USER - ADMIN_RESERV.
3. On failure: format _NN_Reconnect from g_pMessageStringTable, send, flush, CloseUser.
4. On success: continue to version check (BR-11), mode check, MAC capture.
5. Reserved slots 990..999 remain available to the process for non-player sockets
   (admin/NP connections on the DB tier, DB link on the game tier).
```

**Evidence**: `Basedef.h:57-58`, `_MSG_AccountLogin.cpp:30-39`.

---

### Business Rule: BR-10 — Fixed network topology and port numbers

**Overview**
The system's entire deployment topology is five constants: three fixed TCP ports and two server-count limits (plus the derived 11-slot table).

**Detailed description**

`GAME_PORT 8281` ("Game server listening port, client connects to it"), `DB_PORT 7514` ("DB server listening port, Game server connects to it") and `ADMIN_PORT 8895` ("DB server listening port, NPTool connects to it") are bound directly, with no configuration indirection: `ListenSocket.StartListen(hWndMain, *pip, GAME_PORT, WSA_ACCEPT)` (`Server.cpp:3634`), `ListenSocket.StartListen(hWndMain, 0, DB_PORT, WSA_ACCEPT)` and `AdminSocket.StartListen(hWndMain, 0, ADMIN_PORT, WSA_ACCEPTADMIN)` (`DBSrv/Server.cpp:586-587`). Because the constants are compile-time, a deployment that needs to move a port requires a rebuild — there is no override file, environment variable or command-line switch. Port values are also not validated anywhere (no range check, no collision check with the billing port, which is read from configuration into `BillServerPort`).

The fan-in limits are `MAX_SERVER 10` ("Max number of game servers that can connect to DB server"), `MAX_SERVERGROUP 10`, and `MAX_SERVERNUMBER = MAX_SERVER + 1 = 11` (the `+1` accounting for the DB itself or a BISrv, per the comment "DB + TMSrvs + BISrv"). These dimensions feed `g_pServerList[MAX_SERVERGROUP][MAX_SERVERNUMBER][64]` (10x11 entries of 64-byte addresses = 7,040 bytes) and `ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]`. `MAX_SERVER` is also the multiplier in `MAX_DBACCOUNT`, so it simultaneously fixes the connection ceiling at 10,000 and the session-index packing described in BR-08. The header does not record whether a given server's `ServerIndex` is 0-based or 1-based; that convention lives in `WinMain`/`serverlist.txt` handling, and the `MAX_SERVERNUMBER = MAX_SERVER + 1` construction exists precisely to absorb the ambiguity.

**Rule workflow**

```
1. Process start: BASE_InitializeServerList() reads serverlist.txt into
   g_pServerList[group][index][64].
2. Game tier resolves its own group/index (localip.txt + ServerIndex/ServerGroup).
3. Listeners bind GAME_PORT (game) or DB_PORT + ADMIN_PORT (DB).
4. Game tier connects to the DB using the address from g_pServerList.
5. DB accepts at most MAX_SERVER game links and MAX_ADMIN NP links
   (admin checks additionally bounded by MAX_DBACCOUNT).
6. Session identity for every forwarded message is server*MAX_USER + user.
```

**Evidence**: `Basedef.h:44-50,58`; `Server.cpp:3634`; `DBSrv/Server.cpp:586-587`; `Basedef.cpp:2471` (`g_pServerList`).

---

### Business Rule: BR-11 — Protocol version equality gate

**Overview**
A client may only log in if the `ClientVersion` field in `MSG_AccountLogin` equals `APP_VERSION` (6,975). Mismatch results in disconnection with a localized "version not matching, rerun" message.

**Detailed description**

`APP_VERSION 6975` (`Basedef.h:42`) is a bare `#define` with no companion build stamp, and it is compared for strict equality — not a minimum, not a range, not a compatibility set. The comparison lives in `_MSG_AccountLogin.cpp:44`: `if (Size < sizeof(MSG_AccountLogin) || m->ClientVersion != ClientVersion)` where `ClientVersion` was initialised from the constant one line earlier. The failure path formats `g_pMessageStringTable[_NN_Version_Not_Match_Rerun]` with `APP_VERSION` as a `%d` argument, sends it, flushes and closes. Under `_PACKET_DEBUG` the version test is compiled out (`#ifdef` at line 41), leaving only the size test — so a debug build silently accepts any client version, which is a meaningful behavioural divergence between build flavours.

The same constant is duplicated into the build's own identity: the client patch directory is named `ClientPatch_v7662` while `APP_VERSION` is 6975, and no code reconciles the two strings. Since `MSG_AccountLogin` is one of only five structures under `#pragma pack(push, 1)`, its `ClientVersion` sits at a stable offset (12 + 16 + 16 + 52 = 96) that the client must produce exactly; `sizeof(MSG_AccountLogin)` is 120 bytes and includes `int AdapterName[4]` (the MAC quartet) and a 52-byte `char Zero[52]` whose purpose is undocumented. A legacy client that sends a shorter packet is only tolerated in `_PACKET_DEBUG` builds: in release the same `Size < sizeof(MSG_AccountLogin)` condition is part of the rejection test at line 44, so the `0xFF` MAC fallback at lines 67-68 is unreachable outside debug builds. That divergence means a debug server and a release server accept different packet sets for the same endpoint.

**Rule workflow**

```
1. Client sends MSG_AccountLogin: AccountPassword[16], AccountName[16], Zero[52],
   ClientVersion, DBNeedSave, AdapterName[4] (120 bytes, pack 1).
2. Handler rejects conn on a reserved slot first (BR-09).
3. Handler rejects Size < 116 or ClientVersion != 6975 -> send localized message,
   flush, CloseUser.
4. Handler rejects Mode != USER_ACCEPT -> "Login now, wait a moment." + crack log.
5. Handler captures MAC quartet into pUser[conn].Mac (0xFF fill if Size < 116).
6. Handler rewrites Type to _MSG_DBAccountLogin and ID to conn, uppercases the
   account name and forwards to the DB tier.
```

**Evidence**: `Basedef.h:42,1212-1246`; `_MSG_AccountLogin.cpp:26,41-54,65-78`.

---

### Business Rule: BR-12 — ClientTick validation and the SKIPCHECKTICK sentinel

**Overview**
`ClientTick` (header offset 8) is a client-originated timestamp used for rate limiting and sequence validation. The literal `235543242` is a sentinel meaning "internally generated packet — skip every tick check".

**Detailed description**

`SKIPCHECKTICK 235543242` (`Basedef.h:172`) is documented in the header as "base number for the tick checking so people don't get kicked out before doing something". Its use is concentrated in the attack path, where thirteen comparisons test `ClientTick != SKIPCHECKTICK` (or `==`) before applying a rule: minimum 800 ms between attacks (`_MSG_Attack.cpp:59`), attack-speed enforcement (line 68), skill-learned validation (line 184, 205), target-count enforcement (`if (ClientTick != SKIPCHECKTICK && i > maxtarget)`, line 494), and a motion/skill special case that only applies when the sentinel is present (line 533). At line 1730 the presence of the sentinel changes the entire post-processing path (`if (ClientTick == SKIPCHECKTICK)`).

The global gate is in the front dispatcher: `if (isServer == FALSE && std->ClientTick == SKIPCHECKTICK) return;` (`ProcessClientMessage.cpp:63`) — so a *client* cannot use the sentinel to skip checks, but the same dispatcher is used for server-internal calls with `isServer == TRUE`, where the sentinel is honoured. `CPSock::AddMessage` stamps `ClientTick = CurrentTime` on every outgoing packet (`CPSock.cpp:541`), which means the sentinel can only originate from code that constructs a buffer and calls the handler directly rather than going through the socket, or from code that sets it after `AddMessage`. There is no constant for the sentinel's meaning beyond the comment, and the value 235,543,242 is not obviously out-of-band: interpreted in the millisecond unit implied by the 800 ms attack interval, it corresponds to 3 January 1970.

The rule interacts with the `_MSG_Ping` short-circuit immediately above it (`if (std->Type == _MSG_Ping) return;`), which means pings never reach a handler and never refresh `LastReceiveTime` through that path — idle detection is instead driven by `pUser[conn].LastReceiveTime = SecCounter` at line 57, which runs before the ping test. The two mechanisms therefore coexist: every framed packet refreshes liveness, but only non-ping, non-sentinel packets advance any gameplay rule.

**Rule workflow**

```
1. Frame decrypted; Type/ID/ClientTick available.
2. ID range check, ServerDown check, LastReceiveTime refresh.
3. _MSG_Ping returns immediately (no handler, no tick rule).
4. If sender is a client and ClientTick == SKIPCHECKTICK -> discard.
5. Handler-specific tick tests: attack interval >= 800 ms, monotonic ticks,
   target-count cap (MAX_TARGET 13) all bypassed when the sentinel is present.
6. Sentinels are treated as "server-authoritative": line 533 and 1730 take the
   internal code path (e.g. scripted/monster skills with Motion 254 and
   SkillIndex 32/34/36).
```

**Evidence**: `Basedef.h:172`; `ProcessClientMessage.cpp:42-64`; `_MSG_Attack.cpp:59,68,79,104,113,124,184,200,205,494,533,1730`; `CPSock.cpp:541`.

---

### Business Rule: BR-13 — ESCENE_FIELD as a reserved pseudo-connection

**Overview**
`ESCENE_FIELD 30000` is the sentinel written into the `ID` field of packets that are broadcast by the server itself rather than addressed to a connection.

**Detailed description**

The header annotates it "Identifier for the server sent messages" and places it outside every index space (30,000 > `MAX_MOB` 25,000 > `MAX_DBACCOUNT` 10,000 > `MAX_USER` 1,000), which is precisely why it is safe: no bounds check that rejects `ID >= MAX_USER` can accept it by accident, and no array indexing can reach it. It is used as the `id` argument of the signal helpers — `SendClientSignal(conn, ESCENE_FIELD, _MSG_CloseShop)` (`_MSG_Buy.cpp:62`), `SendClientSignalParm(conn, ESCENE_FIELD, _MSG_CombineComplete, 0)` (`_MSG_CombineItem.cpp:41`), and directly as `m->ID = ESCENE_FIELD` for forwarded attack packets (`_MSG_Attack.cpp:25`, `_MSG_Buy.cpp:278`).

The interesting property is that the sentinel is overloaded: it marks "server-originated" for the client's benefit, but the field it occupies is otherwise the connection index, so downstream consumers that need to attribute an action must not use `ID`. `MSG_Attack` therefore carries a separate `unsigned short AttackerID` at a different offset (`Basedef.h:2028`), and the handler writes `m->ID = ESCENE_FIELD` while putting the real actor in `AttackerID`. Nothing in the header documents this division of responsibility; a reader who assumes `ID` is always the actor will mis-attribute every broadcast.

**Rule workflow**

```
1. Server decides to notify clients about an event that no single connection owns.
2. Helper sets sm.Type = the signal constant and sm.ID = ESCENE_FIELD (30000).
3. Packet is queued per recipient through pUser[conn].cSock.AddMessage.
4. Recipient-side code must use payload fields (e.g. AttackerID) for attribution,
   never ID.
5. Any ID-range check (> = MAX_USER) rejects the sentinel, so a reflected
   ESCENE_FIELD packet cannot be processed as a user action.
```

**Evidence**: `Basedef.h:170,2011-2044`; `_MSG_Buy.cpp:62,278`; `_MSG_Attack.cpp:25`; `_MSG_CombineItem.cpp:41`.

---

### Business Rule: BR-14 — Level domains and the global-level offset convention

**Overview**
Levels live in two disjoint domains — Mortal/Arch 1..399 and Celestial 1..199 — and are flattened into one comparable integer by adding an offset. The offset is spelled three different ways in the code.

**Detailed description**

`MAX_LEVEL 399` and `MAX_CLEVEL 199` (`Basedef.h:117-118`) size two experience tables, `g_pNextLevel[MAX_LEVEL + 2]` (401 entries) and `g_pNextLevel_2[MAX_CLEVEL + 202]` (401 entries). The `+2` and `+202` slacks are load-bearing: the lookup path is guarded as `if(level >= MAX_LEVEL) level = MAX_LEVEL;` (`Basedef.cpp:6353-6354`), so index 399 and 400 must exist; for the Celestial table, index `MAX_CLEVEL + 201` must exist because the offset arithmetic can reach it. Neither table is bounds-checked at the point of use beyond that clamp, and the header's `extern` declarations (`Basedef.h:2454-2455`) restate the odd dimensions exactly.

The flattening convention is that a Celestial level is made comparable to a Mortal/Arch level by adding `MAX_LEVEL` (or `MAX_CLEVEL`) to it. Nine sites add `MAX_LEVEL`: hit/HP/MP derivations (`Basedef.cpp:1184,1196`), sanctification scaling (`:3476`), mana (`:3531`), damage (`:3996`). Four sites add `MAX_CLEVEL`: party-level comparisons in `_MSG_AcceptParty.cpp:94` and `_MSG_SendReqParty.cpp:68-69`. One site adds `MAX_CLEVEL + 1` for the leader only (`_MSG_AcceptParty.cpp:93`), producing an asymmetry of exactly one level between the leader's computed level and the member's computed level when both are Celestial — the pragmatic effect is that a Celestial leader at level N is treated as N+200 while a Celestial member at level N is treated as N+199. Because 399 and 199 differ, `+MAX_LEVEL` and `+MAX_CLEVEL` give different global levels for the same Celestial character; the two conventions therefore cannot both be right, and the code does not indicate which is authoritative.

`extra.ClassMaster` determines which domain applies, and `MORTAL`/`ARCH` are treated as the low domain in every branch (`ClassMaster == MORTAL || ClassMaster == ARCH ? Level : Level + offset`). Here `MORTAL` is 2 and `ARCH` is 1, i.e. Mortal has the numerically larger identifier than Arch, which inverts the intuitive ordering and is only discoverable from the `#define` list. `MAX_CLEVEL`, unusually, is also used as a *comparison* bound (`pMob[conn].MOB.CurrentScore.Level <= MAX_CLEVEL`, `_MSG_CombineItemEhre.cpp:149`) and as a *loop bound* (`for(int i = 0; i < MAX_CLEVEL; i++)`, `_MSG_CombineItemEhre.cpp:154`, `_MSG_UseItem.cpp:3466`), where it stands in for the domain maximum. `_MSG_UseItem.cpp:1002` selects `maxlevel = (MORTAL || ARCH) ? MAX_LEVEL : MAX_CLEVEL`, confirming the two-domain intent.

**Rule workflow**

```
1. Character loads with BaseScore.Level in its own domain and extra.ClassMaster
   identifying the domain (MORTAL/ARCH = low, >= CELESTIAL = high).
2. Experience gain compares Exp against g_pNextLevel[level] or
   g_pNextLevel_2[level] depending on domain (BR-19 tables).
3. Level-up clamps at MAX_LEVEL (399) for the low domain; Celestial clamps
   via MAX_CLEVEL comparisons and 199-entry loops.
4. Cross-domain comparisons (party level, hit rate, drop) add MAX_LEVEL or
   MAX_CLEVEL to the high-domain level.
5. Item level requirements compare against the flattened value.
6. Client is told the domain-specific level in MSG_UpdateScore.Score.Level (48-byte STRUCT_SCORE).
```

**Evidence**: `Basedef.h:117-118,178-182,2454-2455`; `Basedef.cpp:607,656,1184,1196,3476,3531,3996,6353`; `_MSG_AcceptParty.cpp:93-94`; `_MSG_SendReqParty.cpp:68-69`; `_MSG_UseItem.cpp:1002,3466`; `_MSG_CombineItemEhre.cpp:149,154`.

---

### Business Rule: BR-15 — ClassMaster versus Class: two different namespaces

**Overview**
Two integer classifications coexist: `STRUCT_MOB::Class` (0..3, the playable archetype) and `STRUCT_MOBEXTRA::ClassMaster` (1..5, the progression tier). They have overlapping value ranges with different meanings and must never be substituted for one another.

**Detailed description**

`MAX_CLASS 4` governs `Class`, indexed as 0..3 and mapped in `g_pIncrementHp[4] = {3,1,1,2}` / `g_pIncrementMp[4] = {1,3,2,1}` with the comment "TK, FM, BM, HT" (`Basedef.cpp:56-57`), and validated as `if(cls < 0 || cls >= MAX_CLASS)` before use (`Basedef.cpp:832,862,1178`, `Server.cpp:6893`, `CFileDB.cpp:1446`). `MAX_CLASSMASTER 4` is declared (`Basedef.h:120`) but referenced nowhere in the repository — it is a dead constant, and the actual `ClassMaster` domain is 1..5 as enumerated by the `MORTAL`/`ARCH`/`CELESTIAL`/`CELESTIALCS`/`SCELESTIAL` defines.

The collision is concrete: `ARCH` is 1 and `Class` 1 is a valid archetype; `MORTAL` is 2 and `Class` 2 is another valid archetype. Code that reads the wrong field therefore still produces a plausible value. This is visible in defensive patterns such as `pMob[i].extra.ClassMaster == MORTAL || ... == ARCH ? ... : Level + MAX_LEVEL`, which is written out longhand instead of as a predicate, because a bare `<= 2` test would also match `ClassMaster == 1` and, if applied to `Class`, would silently classify two of the four archetypes as "low tier". `DBGetSelChar` uses the distinction to rewrite the face index: `sel->Equip[i][0].sIndex = file->mobExtra[i].ClassMaster == MORTAL ? 21 : file->mobExtra[i].MortalFace + 7` (`CFileDB.cpp:2619`), so the *visual identity of a character on the selection screen* is a direct function of `ClassMaster` equality with the constant 2.

`STRUCT_MOBEXTRA` also carries `SaveCelestial[2]`, a two-entry history of previous Celestial incarnations (each 136 bytes, with its own `Class`, `Exp`, `SPX/SPY`, `BaseScore`, `LearnedSkill`, bonus points and skill bars). The `2` in `SaveCelestial[2]` is literal and unconnected to any constant, and `MAX_CLASSMASTER 4` — the only constant that hints at a master-tier count — is unused, so the intended cardinality of the celestial save history is undocumented.

**Rule workflow**

```
1. Character creation fixes MOB.Class in 0..3 (validated against MAX_CLASS) and
   extra.ClassMaster to MORTAL(2) or ARCH(1).
2. Advancement rewrites extra.ClassMaster to CELESTIAL(3), CELESTIALCS(4),
   SCELESTIAL(5) and archives the old state into extra.SaveCelestial[0..1].
3. Every derivation that needs "is this a high tier" tests ClassMaster against
   MORTAL and ARCH by name, never by numeric range.
4. Every derivation that needs "which archetype" indexes tables by MOB.Class
   after an explicit 0 <= cls < MAX_CLASS check.
5. Face selection for the character list branches on ClassMaster == MORTAL.
```

**Evidence**: `Basedef.h:115,120,178-182,544-567`; `Basedef.cpp:56-57,832,862,1178`; `CFileDB.cpp:1446,2619`; `Server.cpp:6893`.

---

### Business Rule: BR-16 — Two disjoint status-flag namespaces on overloaded fields

**Overview**
Status effects are encoded as bitmasks on two fields that also carry numeric data: `RSV_*` (nine bits, into the 16-bit `STRUCT_MOB::Rsv`) and `MSV_SNOOP` (bit 0, into the byte `STRUCT_MOB::Merchant`, which is simultaneously a merchant/NPC identifier and a two-bit city field).

**Detailed description**

`RSV_FROST 0x01`, `RSV_DRAIN 0x02`, `RSV_VISION 0x04`, `RSV_PARRY 0x08`, `RSV_HIDE 0x10`, `RSV_HASTE 0x20`, `RSV_CAST 0x40`, `RSV_BLOCK 0x80`, `RSV_IMUNIDADE 0x100` (`Basedef.h:184-192`) fit cleanly in the `unsigned short Rsv` field at `Basedef.h:445`; the ninth bit at `0x100` is why the field is 16 bits rather than 8. Only two of the nine are actually written by the codebase — `RSV_FROST` and `RSV_DRAIN` (`Basedef.cpp:3569,3574`), read back in `_MSG_Attack.cpp:1450,1464` — while `RSV_IMUNIDADE`, `RSV_PARRY`, `RSV_HIDE`, `RSV_HASTE`, `RSV_CAST`, `RSV_BLOCK` and `RSV_VISION` are declared and never referenced by name. This is a namespace defined in the contract but only 22% implemented, and the unused bits are indistinguishable from "features disabled" versus "features removed".

`MSV_SNOOP 0x01` is far more dangerous because it shares a byte with numeric semantics. `MOB.Merchant` is (a) an NPC/merchant type compared against literal ids 1, 4, 43, 100 (`_MSG_Attack.cpp:340`), 1 (`_MSG_Buy.cpp:55`), 0 (`_MSG_REQShopList.cpp:39`), (b) a city selector packed into bits 6-7 (`pMob[conn].MOB.Merchant &= 0x3F; pMob[conn].MOB.Merchant |= city << 6;`, `_MSG_ChangeCity.cpp:30-31`), (c) a zone offset derived by subtraction (`int zone = pMob[target].MOB.Merchant - 6;`, `_MSG_ChallangeConfirm.cpp:30`), and (d) a bitfield where bit 0 means "snoop/GM invisibility" (`imple.cpp:1569,1576`). Because `0x01` is also the numeric value of "merchant type 1", the tests `Merchant == 1` and `Merchant & MSV_SNOOP` are the same test on odd values; `Merchant != 0` (used at `_MSG_Attack.cpp:890` to decide whether an NPC is a valid attack target) is true for every odd merchant id regardless of snoop state. The header's own comment on the field is "The mob's merchant ID" (`Basedef.h:442`), with no mention of flags.

`STRUCT_SCORE::Merchant` is a *different* byte with no documented relation to `STRUCT_MOB::Merchant`; `Basedef.cpp:4017` composes them (`MOB.CurrentScore.Merchant = (MOB.CurrentScore.Merchant & 240) | MOB.Merchant`), i.e. the high nibble of the score copy is preserved and the low nibble is overwritten by the mob's value — a fourth interpretation of the same concept. `SOUL_*` (`Basedef.h:210-232`, 18 values 0..17) is a third, clean namespace stored in `char Soul` and read by a long `if/else if` chain in `Basedef.cpp:3011-3054`; it has no overlap problem because it occupies a dedicated byte.

**Rule workflow**

```
1. Immunity: server sets MOB.Rsv |= RSV_FROST / RSV_DRAIN when an item grants it.
2. Combat: attacker tests (target.MOB.Rsv & RSV_FROST) to redirect or nullify damage.
3. City change: Merchant bits 6-7 are rewritten, bits 0-5 preserved.
4. NPC identity: Merchant is compared against literal ids to select shop/quest behaviour.
5. Snoop: Merchant bit 0 toggled by the /snoop implementation path.
6. Client projection: Resist[4] (not Rsv) is what MSG_UpdateScore carries, so Rsv is server-only.
```

**Evidence**: `Basedef.h:184-194,210-232,420,442,445,479`; `Basedef.cpp:3011-3054,3569-3574,4017`; `_MSG_Attack.cpp:340,890,1450,1464`; `_MSG_ChangeCity.cpp:30-31`; `_MSG_ChallangeConfirm.cpp:30`; `imple.cpp:1569-1576`.

---

### Business Rule: BR-17 — Size as payload-variant discriminator

**Overview**
Some logical messages have several physical layouts sharing one `Type`. The receiver selects the layout by comparing `Size` against the candidate `sizeof`s, in descending order, which makes the payload structures in this header part of the protocol negotiation.

**Detailed description**

`MSG_Attack` (168 bytes, `Dam[13]`), `MSG_AttackOne` (68 bytes, `Dam[1]`, `#pragma pack(1)`) and `MSG_AttackTwo` (80 bytes, `Dam[2]`) all travel as `_MSG_Attack` (`0x0367`). The consumer at `_MSG_Attack.cpp:1062` decides per iteration:

```
for (q = 0; q < skill_target &&
     ((q < MAX_TARGET && m->Size >= sizeof(MSG_Attack)) ||
      (q < 1        && m->Size >= sizeof(MSG_AttackOne)) ||
      (q < 2        && m->Size >= sizeof(MSG_AttackTwo))); q++)
```

so the number of damage entries that may be read is derived from the frame length, and `MAX_TARGET 13` is the ceiling only when the largest variant is present. The three structures are *not* prefix-compatible in a naive sense: `MSG_Attack` carries `int CurrentHp` at one offset while `MSG_AttackOne` carries `int CurrentMp` there and `CurrentHp` later, and `MSG_AttackTwo` mirrors `MSG_AttackOne`'s ordering with a two-entry array. The declared sizes confirm the deliberate asymmetry (168 / 68 / 80, and `MSG_AttackTwo` is not `MSG_AttackOne + 8` because of packing), so a receiver cannot simply read `Dam[0]` at a fixed offset across variants.

The same size-as-discriminator pattern appears in the login pair: `MSG_CNFCharacterLogin` (2,632 bytes, includes `affect[32]`, `mobExtra`, `Donate`) and `MSG_CNFClientCharacterLogin` (1,832 bytes) share `_MSG_CNFCharacterLogin` (`0x0114`); the client-variant omits the trailing 800 bytes. `MSG_AccountLogin` (120) versus `MSG_AccountLogin_HWID` (170) is a third instance, discriminated by `Size < sizeof(MSG_AccountLogin)` at `_MSG_AccountLogin.cpp:42,67` — except that both must first pass the earlier `Size < sizeof(MSG_AccountLogin)` rejection, so the HWID variant is only reachable on the transmit side. `MSG_CreateMob` (232) versus `MSG_CreateMobTrade` (252) is a fourth: the trade variant appends `char Desc[24]` and drops the trailing `int Hold`.

Because `BASE_CheckPacket` is disabled (BR-01), none of these discriminators are validated centrally; each handler re-implements the comparison, and a frame whose `Size` matches no variant falls through to the smallest interpretation. The header provides no `union`, no tag field and no comment block describing the variants — the only record of the relationship is the convention that the `MSG_*` names share a prefix and the dispatcher's `case` labels are shared.

**Rule workflow**

```
1. Sender (server) chooses the variant that fits the number of targets/fields.
2. CPSock stamps the exact byte count into Size.
3. Receiver reads Size and selects the widest matching layout:
   Size >= 168 -> MSG_Attack (up to 13 entries)
   Size >= 80  -> MSG_AttackTwo (2 entries)
   Size >= 68  -> MSG_AttackOne (1 entry)
4. Entry count is min(skill_target, variant capacity) and additionally capped at MAX_TARGET.
5. Fields are read through the chosen interpretation; fields present only in the
   largest variant are ignored for smaller ones.
```

**Evidence**: `Basedef.h:1364-1386,1511-1520,1553-1578,2011-2133,2063-2097`; `_MSG_Attack.cpp:1062,1087-1093`; `_MSG_AccountLogin.cpp:42,67`; measured sizes in section 6.4.

---

### Business Rule: BR-18 — Numeric saturation limits and affect duration units

**Overview**
Five constants bound the arithmetic domain: HP and MP saturate at 2,000,000,000, physical damage at 1,000,000, magic damage at 254, concurrent affects at 32, and affect duration is expressed in two units — 450 per hour and 10,800 seconds.

**Detailed description**

`MAX_HP 2000000000` and `MAX_MP 2000000000` (`Basedef.h:203-204`) are chosen just below `INT_MAX` (2,147,483,647) so that intermediate additions cannot overflow a signed 32-bit `int`, which is the type of `STRUCT_SCORE::MaxHp/Hp/MaxMp/Mp`. `MAX_DAMAGE 1000000` bounds the per-hit physical value and `MAX_DAMAGE_MG 254` bounds the magic value — and 254 is not a round number in base 10: it is the largest value that fits in an `unsigned char` below the 255 sentinel, so `MAX_DAMAGE_MG` is really encoding "one byte, reserving 0xFF". `MSG_Attack::Dam[MAX_TARGET].Damage` is declared `int` while the magic path is byte-limited, so the two damage domains have different widths in the same structure.

`MAX_AFFECT 32` sizes five distinct things simultaneously: `STRUCT_AFFECT` arrays in `STRUCT_ACCOUNTFILE` (`affect[4][32]` = 1,024 bytes), in `MSG_DBSaveMob`/`MSG_SavingQuit` (`affect[32]`), in `MSG_CNFCharacterLogin`, the `unsigned short Affect[32]` mirrors in `MSG_UpdateScore` (64 bytes) and `MSG_CreateMob`/`MSG_CreateMobTrade` (64 bytes), and the full `STRUCT_AFFECT Affect[32]` in `MSG_SendAffect` (256 bytes). The same slot count is thus represented in two incompatible encodings — a 2-byte packed id/level in `MSG_UpdateScore` versus an 8-byte `{Type,Value,Level,Time}` in `MSG_SendAffect` — and both are indexed by affect slots, so a consumer that receives one and expects the other reads garbage rather than failing.

`AFFECT_1H 450` and `AFFECT_1D 10800` (`Basedef.h:207-208`) are the duration conversion: 450 units per hour and 10,800 units per day (24 x 450), i.e. the same duration expressed in two bases. In practice only `AFFECT_1H` is used, and always as a multiplier or divisor (`AFFECT_1H`, `AFFECT_1H/2`, `AFFECT_1H*2`, `AFFECT_1H*4`, `AFFECT_1H*20` in `_MSG_UseItem.cpp:1219-2214` and `_MSG_Quest.cpp:2497`), which implies a tick granularity of 8 seconds; `AFFECT_1D` is declared and never referenced. The header does not name the base unit, and `STRUCT_AFFECT::Time` is `unsigned int`, so an effect's remaining time is a raw count in whichever base its writer chose.

**Rule workflow**

```
1. Damage computed and clamped to MAX_DAMAGE (physical) or MAX_DAMAGE_MG (magic).
2. HP/MP recomputed and saturated at MAX_HP/MAX_MP before being written to
   STRUCT_SCORE and mirrored to the client.
3. Affect slots allocated from the fixed 32-slot space; allocation failure means
   the effect is dropped (no eviction policy is declared in the contract).
4. Duration converted with AFFECT_1H / AFFECT_1D depending on the caller.
5. Client projections: MSG_UpdateScore carries packed Affect[32] (2 bytes each),
   MSG_SendAffect carries full STRUCT_AFFECT[32] (8 bytes each).
6. On save, affect[4][32] is copied verbatim into the account file (1,024 bytes).
```

**Evidence**: `Basedef.h:122,203-208,593-599,1465-1492,2186-2191`; `Basedef.h:850,995,1176,1364`.

---

### Business Rule: BR-19 — Fixed-size content tables as compile-time contract

**Overview**
All content lives in statically sized globals declared `extern` in this header. Their dimensions are the contract: 6,500 items, 103 skills, 256 initial items, 128 effect names, 2,000 localized strings, 4,096 guilds, 50 summon bonuses, 401 experience entries.

**Detailed description**

The 47 `extern` declarations at `Basedef.h:2439-2503` are the complete cross-tier shared-state surface of the header. Several are sized by a constant that is *also* used as a loop bound in the loader: `g_pItemList[MAX_ITEMLIST]` (6,500 x 140 bytes = 910 KB) with `BASE_ReadItemListFile`, `g_pSpell[MAX_SKILLINDEX]` (103 x 84 = 8.6 KB) with `BASE_ReadSkillBin`, `g_pInitItem[MAX_INITITEM]` (256 x 8) with `BASE_ReadInitItem`, `EffectNameTable[MAX_EFFECTINDEX][24]` (3,072 bytes) with `BASE_InitializeEffectName`, `g_pMessageStringTable[MAX_STRING][128]` (256 KB) with `BASE_ReadMessageBin`. Because the loader writes into a fixed array without resizing, a content file with more rows than the constant silently truncates; a file with fewer rows leaves zeroed entries that are still resolvable by index.

The string table has an additional invariant that is invisible in the declaration: `BASE_ReadMessageBin` reads exactly `sizeof(char) * MAX_STRING * 128` bytes, then XORs every byte with `0x5A` in place (`Basedef.cpp:4934-4960`), and the appended checksum is read but its verification is commented out. The lookup helpers clamp with `if (idx >= 0 && idx < MAX_STRING)` before formatting (`Basedef.cpp:1260-1308`), so a bad string index yields an empty string rather than an overrun. `g_pGuildName[10][16][MAX_GUILD][GUILDNAME_LENGTH]` (10 server groups x 16 servers x 4,096 guilds x 12 chars = 7.9 MB) uses the literals 10 and 16 rather than `MAX_SERVERGROUP`/`MAX_SERVERNUMBER`, so changing the topology constants does not resize it — a divergence that would become a buffer-size mismatch the moment `MAX_SERVER` were increased.

`g_pAttribute[1024][1024]` is the clearest example of a table whose dimension is *not* the coordinate constant: the map grid is 4,096 x 4,096 (`MAX_GRIDX/Y`, `g_HeightWidth=4096`), and attribute lookups downsample with `(x >> 2) & 0x3FF` (`Basedef.cpp:2595-2597`). The 1024 is therefore a resolution choice (4x downsampled, wrapping) rather than the map size, and it is written as a literal in both the definition and the `extern` declaration, independent of `MAX_GRIDX`.

**Rule workflow**

```
1. Startup: BASE_InitializeBaseDef -> BASE_ReadItemList / ReadSkillBin /
   ReadInitItem / ReadMessageBin / InitializeEffectName / InitializeServerList.
2. Each loader fills a fixed array; row count beyond the constant is discarded.
3. Runtime lookups index the array by id from the wire or from a CSV-defined value;
   only the string table performs an explicit range clamp.
4. Item metadata (price, requirements, static effects, grade) is read from
   STRUCT_ITEMLIST (140 bytes) per lookup, not copied into the item instance.
5. The tables are process-private copies (both executables define their own).
```

**Evidence**: `Basedef.h:139-152,882-906,2454-2503`; `Basedef.cpp:4932-4961,1260-1308,2591-2605,2736-2756`.

---

### Business Rule: BR-20 — NPC-generator identifier ranges, including deliberate overlaps

**Overview**
The header allocates specific NPC-generator id ranges for quest content and event rooms using ~90 numeric `#define`s. Several ranges intentionally alias their own boundaries, so a single id participates in two different roles.

**Detailed description**

Three families dominate. The secret-room family allocates three parallel corridors, each with an `_INITIAL`, ten `SALA*_MOB_*` slots, an `_END` and a `_BOSS_INITIAL`: `SECRET_ROOM_N_INITIAL 2395` with mobs 2395..2404, `SECRET_ROOM_N_END 2402` and `SECRET_ROOM_N_BOSS_INITIAL 2403`. The `_END` value (2402) equals `SECRET_ROOM_N_SALA4_MOB_2`, and `_BOSS_INITIAL` (2403) equals `SALA4_MOB_3`, so the same generator index is simultaneously "end of the normal room" and "a normal room mob", and simultaneously "boss start" and "fourth-room mob 3". The M and A corridors repeat the pattern exactly (`M_END 2412` = `M_SALA4_MOB_2`, `M_BOSS_INITIAL 2413` = `M_SALA4_MOB_3`; `A_END 2422`, `A_BOSS_INITIAL 2423`). Any loop written as `for (i = INITIAL; i <= END; i++)` therefore re-processes boss generators, and any loop `for (i = BOSS_INITIAL; i <= ...)` starts inside the normal range.

The Nightmare family is contiguous and non-overlapping (`NIGHTMARE_N_INITIAL 2368`/`_END 2375`, `M` 2377/2384, `A` 2385/2394), which shows that the overlap in the secret-room family is a choice rather than a constraint. The rune-quest family allocates six level bands with arbitrary boundaries (`RUNEQUEST_LV1_MOB_INITIAL 5709`/`_END 5764`, `LV2` 5790/5848 with `BOSS 5789`, `LV3` split into `SULRANG` 5972..5975 and `BOSS` 5948..5955, `LV4` 5854..5898 with `BOSS 5849`, `LV5 BOSS 5899`, `LV6` 5775..5785 plus 5768..5774 plus `BOSS 5767`) — note `LV4_MOB_BOSS 5849` immediately precedes `LV4_MOB_INITIAL 5854`, and `LV5_MOB_BOSS 5899` immediately follows `LV4_MOB_END 5898`, so boss ids are allocated out-of-band from their band. `KEFRA_BOSS 396` similarly precedes `KEFRA_MOB_INITIAL 397..400`.

The quest-NPC family (`Basedef.h:240-274`, 35 values, 1..35) and the single-NPC family (`ZAKUM_LORD 4`, `ORC_GUERREIRO 3`, `REI_HARABARD 8`, `REI_GLANTUAR 9`, `TORRE_NOATUM1..3` 23..25, `WATER_M/N/A_INITIAL` 10/171/183, `GTORRE 1078`, `RVRTORRE_1/2` 4236/4237) are keyed by generator index and used with literal comparisons in the quest handler. The names are Portuguese-influenced and, in the quest-NPC family, are assigned to *quest script slots* rather than to generator ids — `QST_AMUMISTICO 1` is declared separately from `AMU_MISTICO 2`, proving that two different numbering schemes (quest ids and NPC ids) coexist in the same `#define` namespace with adjacent values.

**Rule workflow**

```
1. spawn configuration selects an NPC generator by index.
2. Content code compares the generator index against these #defines to decide
   room membership, quest assignment or boss identity.
3. Range iteration (INITIAL..END) includes generators that are also named by
   BOSS_INITIAL/_END aliases, so aliased ids are processed twice where loops overlap.
4. Quest ids (1..35) are matched against the quest byte / NPC script id, not
   against the generator index.
```

**Evidence**: `Basedef.h:234-274,278-391`.

---

### Business Rule: BR-21 — Four-value state machine for ground items and gates

**Overview**
A single `int State` carried in `MSG_UpdateItem` and stored on `CItem` encodes whether a door, gate, chest or placed object is inert, open, closed or locked. The four values are declared in this header, not in the message block, and are the only `const short` values in the file that are not message types.

**Detailed description**

`STATE_NOTHING 0`, `STATE_OPEN 1`, `STATE_CLOSED 2`, `STATE_LOCKED 3` (`Basedef.h:1834-1837`) sit in the middle of the client/server message block, immediately after `MSG_UseItem` and before `MSG_UpdateItem`, which is where the reader naturally looks for a payload rather than an enumeration. They are typed `const short`, matching the message ids around them, so nothing at the type level distinguishes "state value" from "message type" — a misunderstanding that is only caught by the fact that no `case STATE_OPEN:` exists in any dispatcher.

The state is produced and consumed in four distinct flows. Gate control passes the state as the second argument of `UpdateItem(gateid, STATE_OPEN, &height)` (`_MSG_UpdateItem.cpp:98`, `CCastleZakum.cpp:200`, `imple.cpp:363,378`), and the lock/unlock admin path uses `STATE_LOCKED` (`imple.cpp:315,339`) then `STATE_CLOSED` (`imple.cpp:400,415`). Ground-item creation writes `sm->State = STATE_CLOSED` into the `MSG_UpdateItem`/`MSG_CreateItem` projection (`GetFunc.cpp:1220`) and the same value is later tested to decide interaction (`if (sm->State == STATE_OPEN)`, `GetFunc.cpp:1232`). Door timers re-derive state from the height grid (`ProcessSecMinTimer.cpp:2157,2179`), so the persisted `State` and the collision/height data must agree. `STATE_NOTHING` is never used, which means "no state" and "state 0" are indistinguishable from an uninitialised record.

Because the value is an `int` on the wire while the stored `CItem::State` is whatever type `CItem` declares, and because `MSG_UpdateItem` also carries an `int ItemID` whose valid range is `MAX_ITEM` (5,000), the two integers are easily transposed by a handler; the header provides no `enum class` or named wrapper to prevent it. The practical invariant is: `State` in 1..3 means a live gate/object, `State` 0 means the record is not a gate at all.

**Rule workflow**

```
1. Map load places a gate/item with State = STATE_CLOSED and its height footprint applied.
2. Interaction (skill, key item, admin command) sends MSG_UpdateItem with ItemID + State.
3. Handler validates ItemID < MAX_ITEM, then calls UpdateItem(id, newState, &height).
4. UpdateItem writes the height for the footprint (BASE_UpdateItem / UpdateItem2) and,
   on failure (out of bounds), returns without changing State.
5. The authoritative state is re-broadcast to nearby clients via MSG_UpdateItem.
6. Timers may force STATE_OPEN -> STATE_CLOSED transitions for castle/arena doors.
7. LOCKED is terminal until an explicit unlock; no automatic path returns to STATE_NOTHING.
```

**Evidence**: `Basedef.h:1834-1837,1839-1847`; `_MSG_UpdateItem.cpp:98`; `CCastleZakum.cpp:200`; `imple.cpp:315,339,363,378,400,415`; `GetFunc.cpp:1220,1232`; `ProcessSecMinTimer.cpp:2157,2179`.

---

### 3.3 Implicit rules with lower confidence

| ID | Implicit rule | Confidence | Evidence |
|---|---|---|---|
| IR-01 | `STRUCT_MOB::Equip[0]` is the face/visual slot and `Equip[14]` the mount slot | High | `Server.cpp:4180,4202`; `Basedef.cpp:3433` |
| IR-02 | `Carry[63]` (`KILL_MARK`) is a kill-counter record, never a container | High | `GetFunc.cpp:2051-2142` |
| IR-03 | `MSG_NPAccountInfo` must remain below `MAX_MESSAGE_SIZE` (360-byte headroom) | High | measured 7,832 vs `CPSock.h:38` |
| IR-04 | `Effect[0]` has priority over `Effect[1]` over `Effect[2]` for any effect lookup | High | `Basedef.cpp:2102-2119,2291-2299` |
| IR-05 | `sIndex` ranges 2320-2359 = mounts, 2330-2389 = no-sanctification, 2360-2389 = growth items | Medium | `Server.cpp:4211`, `Basedef.cpp:2145,2179,2229` |
| IR-06 | The 52-byte blob is a cross-structure session/transfer token: `STRUCT_ACCOUNTFILE::TempKey`, `MSG_AccountLogin::Zero` and `MSG_DBCNFServerChange::Enc` are compared and copied as one value | High | `CFileDB.cpp:664-672,1984` |
| IR-07 | `STRUCT_MOB::Quest` is a single quest-progress byte that is written to 0 once and never read | High (for "unused"), Low for its intended semantics | `imple.cpp:115` alone |
| IR-08 | `STRUCT_SCORE::ChaosRate`/`Merchant`/`Direction` are legacy client fields | Low | `Basedef.h:420-423`, comment "UNK" |

---

## 4. Component Structure

```
Code/
├── Basedef.h                                # THIS COMPONENT - 2,505 lines, single file
│   ├── [21-22]   include guard _BASEDEF_
│   ├── [24-25]   <time.h>, <Windows.h>      # only two dependencies
│   ├── [27-394]  #pragma region Defines
│   │   ├── [31-39]   IDC_* dialog control ids (8)
│   │   ├── [42-61]   APP_VERSION, GAME_PORT, DB_PORT, ADMIN_PORT,
│   │   │             MAX_SERVER, MAX_SERVERGROUP, MAX_SERVERNUMBER,
│   │   │             MAX_IP_LENGTH, SECSTANDINGBY, SECBATTLE, MAX_USER,
│   │   │             ADMIN_RESERV, MAX_ADMIN, MAX_DBACCOUNT, MAX_RANK_INDEX
│   │   ├── [63-73]   Account-related lengths (ACCOUNTNAME_LENGTH ... MOB_PER_ACCOUNT)
│   │   ├── [75-173]  Inventory, grid, level, message and content capacities
│   │   ├── [175-208] Nations, progression tiers, RSV_* / MSV_* flags,
│   │   │             REF_10..15, MAX_HP/MP/DAMAGE, AFFECT_1H/1D
│   │   ├── [210-232] SOUL_* namespace (18 values)
│   │   ├── [234-274] Quest NPC ids (1..35)
│   │   └── [278-391] NPC-generator id allocations (secret rooms, nightmare,
│   │                 rune quests, towers, kefra)
│   ├── [396-836] #pragma region Structures
│   │   ├── [398-412]  STRUCT_ITEM           8 B   (sIndex + 3 effect pairs)
│   │   ├── [414-436]  STRUCT_SCORE         48 B
│   │   ├── [438-481]  STRUCT_MOB          816 B
│   │   ├── [483-591]  STRUCT_MOBEXTRA     536 B
│   │   ├── [593-599]  STRUCT_AFFECT         8 B
│   │   ├── [601-608]  STRUCT_LEVELBONUS    10 B   (dead)
│   │   ├── [610-614]  STRUCT_CAPSULE     1352 B
│   │   ├── [616-635]  STRUCT_BEASTBONUS    68 B
│   │   ├── [637-644]  STRUCT_TREASURE      52 B
│   │   ├── [646-656]  STRUCT_PISTA         84 B
│   │   ├── [658-670]  STRUCT_CASTLEQUEST  564 B
│   │   ├── [672-682]  STRUCT_MERC         796 B
│   │   ├── [684-707]  STRUCT_CAMP         428 B   (dead)
│   │   ├── [709-712]  STRUCT_ITEMLOG        4 B
│   │   ├── [714-744]  STRUCT_GUILDZONE     84 B
│   │   ├── [746-763]  STRUCT_SEALOFSOUL    60 B
│   │   ├── [765-778]  STRUCT_SELCHAR      840 B   (4 hardcoded dimensions x8)
│   │   ├── [780-795]  STRUCT_ACCOUNTINFO  220 B
│   │   ├── [796-806]  STRUCT_GUILDINFO      8 B   (typedef, style outlier)
│   │   └── [808-835]  STRUCT_RANKING       52 B   (pack 1, C++ ctors)
│   ├── [838-921] #pragma region File Structures
│   │   ├── [840-856]  STRUCT_ACCOUNTFILE 7808 B   (offset comments 0..4580)
│   │   ├── [858-880]  STRUCT_SPELL         84 B
│   │   ├── [882-906]  STRUCT_ITEMLIST     140 B
│   │   ├── [908-914]  STRUCT_INITITEM       8 B
│   │   └── [916-919]  STRUCT_BLOCKMAC     16 B
│   ├── [923-2327] #pragma region Messages defines and structures
│   │   ├── [925-930]  #define _MSG       12-byte header macro
│   │   ├── [932-941]  7 FLAG_* direction constants (const int, not #define)
│   │   ├── [943-974]  5 standard-header structs (MSG_STANDARD..MSG_STANDARDPARM3)
│   │   ├── [976-1133] DB2GAME namespace (28 constants)
│   │   ├── [1137-1186] GAME2DB "TM > DB" block (16 constants + 5 structs)
│   │   ├── [1188-1938] Client <> TM block (93 constants, ~40 structs)
│   │   ├── [1943-2002] combine / capsule / delete / split id block (14 constants)
│   │   ├── [2004-2233] attack, trade, ranking, castle, FLAG_NEW block
│   │   └── [2236-2325] NP (DB2NP / NP2DB) block (15 constants)
│   ├── [2329-2437] 101 BASE_* prototypes (4 overload families)
│   ├── [2439-2503] 47 extern content-table declarations
│   └── [2505]      #endif
├── Basedef.cpp                              # out of scope (Rules Engine component)
├── CPSock.h/.cpp                            # duplicates the 12-byte HEADER layout,
│                                            # owns MAX_MESSAGE_SIZE / INITCODE
├── ItemEffect.h                             # EF_* effect ids referenced by item rules
└── TMSrv/ | DBSrv/                          # 80 TUs consuming this header
```

Internal organisation is by `#pragma region` only (7 regions); there are no nested directories, no namespace, no partial files and no generated sections. The file is order-dependent in one important respect: `_MSG` (a macro) is defined at line 925, after all constant and domain structs that its expansions use, and every `MSG_*` structure depends on `STRUCT_*`, `NAME_LENGTH`, `MAX_*` being declared above it. Constants and structures therefore cannot be moved across the region boundary without breaking compilation, and the file cannot be split without introducing explicit includes.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (efferent — what this component depends on)

```
Basedef.h
  -> <time.h>      (time_t in STRUCT_MOBEXTRA: LastNT, DivineEnd, DonateInfo.LastTime)
  -> <Windows.h>   (no direct use; required transitively for the MIN/MAX-style types
                    and to match the MSVC project's precompiled environment)
  -> NOTHING else  (no project header is included; strncpy used by the inline
                    STRUCT_RANKING ctor is resolved only because <Windows.h>
                    pulls in <string.h> on MSVC)
```

Efferent coupling is therefore **1** in architectural terms (system libraries only) and **0** to any project component. This is what allows the header to be included by both executables without pulling in `Server.h`, `CPSock.h` or `ItemEffect.h` — and it is why `BASEDEF` can be compiled twice, once per tier, without a shared library.

### 5.2 Internal dependencies (afferent — who depends on this component)

```
TMSrv (73 .cpp, 14 .h)
  header aggregators: Server.h, ProcessClientMessage.h, ProcessDBMessage.h,
                      CUser.h, CMob.h, CItem.h, CNPCGene.h, CReadFiles.h,
                      GetFunc.h, CCastleZakum.h, CWarTower.h   (11 direct includes)
  58 handler TUs (_MSG_*.cpp) -> ProcessClientMessage.h -> Basedef.h
  15 core TUs (Server, SendFunc, GetFunc, imple, MobKilled, ProcessSecMinTimer,
               ProcessClientMessage, ProcessDBMessage, C*.cpp)
DBSrv (5 .cpp, 7 .h)
  header aggregators: CFileDB.h, CUser.h, CReadFiles.h, CRanking.h (4 direct includes)
  all 5 TUs
Shared
  Basedef.cpp (implements the 101 prototypes + defines the 47 externs)
  CPSock.cpp (uses MSG_STANDARD-independent HEADER, but calls BASE_CheckPacket)
```

Measured totals: **80 translation units** include the header directly or transitively; **15 headers** name it directly.

### 5.3 Outbound runtime dependencies (via the contract, not via include)

| Contract element | Consumed by | Effect |
|---|---|---|
| `_MSG` 12-byte header layout | `CPSock.h::HEADER` (duplicate definition), both dispatchers | Layout duplication: two definitions must stay byte-identical |
| 191 `_MSG_*` constants | `ProcessClientMessage.cpp` (64 cases), `ProcessDBMessage.cpp`, `DBSrv/CFileDB.cpp`, `SendFunc.cpp`, `CFileDB.cpp` send helpers | Any value change breaks both tiers simultaneously (no negotiation) |
| 100 `MSG_*` payloads | every handler cast | Wire compatibility between server build and client build |
| `STRUCT_ACCOUNTFILE` | `CFileDB.cpp`, `CRanking.cpp`, `DBSrv/CReadFiles.cpp`, `DBSrv/Server.cpp` | On-disk compatibility across all historical account files |
| `STRUCT_MOB`/`STRUCT_MOBEXTRA` | `CMob`, `CUser`, all handlers, DB store | In-memory entity + wire + disk all share one layout |
| 47 content externs | `Basedef.cpp` loaders, all rule consumers | Single content plane, two processes |
| `MAX_*` capacities | `Server.h` array declarations, all loop bounds | Memory footprint and index spaces |

### 5.4 External dependencies

None. The component performs no I/O, has no third-party dependency and no build-time code generation. The only externality is the MSVC ABI itself, on which the 7,808-byte persistence layout silently depends (alignment of `long long`/`time_t`, packing defaults).

---

## 6. Afferent and Efferent Coupling

### 6.1 Component-level

| Component | Afferent (Ca) | Efferent (Ce) | Criticality |
|---|---|---|---|
| Basedef Domain Contract (`Basedef.h`) | 80 TUs + 15 headers | 2 (system headers) | Critical — single point of truth for wire + disk |
| Basedef Rules Engine (`Basedef.cpp`) | 49 TUs | 3 | High |
| CPSock Transport | 46 call sites for `AddMessage`, 2 for `ReadMessage` | 3 (incl. Basedef for `BASE_CheckPacket`) | High |

Architecture-report estimate was ~95 afferent; the measured value in this analysis is 80 translation units (+15 headers) after excluding `release/` build artefacts.

### 6.2 Type-level coupling (measured by counting TUs that name the type)

| Type | Ca (TUs) | Ce (types it embeds) | Criticality |
|---|---|---|---|
| `MSG_STANDARD` | 27 | 0 (only `_MSG` macro) | Critical (every cast) |
| `STRUCT_ITEM` | 36 | 0 | Critical |
| `MSG_STANDARDPARM` | 21 | 0 | High |
| `MSG_CreateMob` | 12 | `STRUCT_SCORE`, `_MSG` | High |
| `STRUCT_MOB` | 8 | `STRUCT_SCORE` x2, `STRUCT_ITEM` x2 | Critical |
| `STRUCT_MOBEXTRA` | 5 | `STRUCT_SCORE`, `time_t` x3 | Critical (persisted) |
| `MSG_Attack` | 5 | `STRUCT_DAM` | High |
| `STRUCT_ACCOUNTFILE` | 4 | `STRUCT_ACCOUNTINFO`, `STRUCT_MOB`, `STRUCT_ITEM`, `STRUCT_AFFECT`, `STRUCT_MOBEXTRA` | Critical (persisted) |
| `STRUCT_SCORE` | 3 | 0 | High |
| `STRUCT_AFFECT` | 3 | 0 | High |
| `STRUCT_RANKING` | 3 | 0 | Medium |
| `MSG_AccountLogin` | 3 | 0 | High |
| `MSG_DBSaveMob` | 3 | `STRUCT_MOB`, `STRUCT_ITEM`, `STRUCT_AFFECT`, `STRUCT_MOBEXTRA` | High |
| `MSG_CNFCharacterLogin` | 3 | same as above | High |
| `MSG_Trade` | 3 | `STRUCT_ITEM` | Medium |
| `STRUCT_SELCHAR` | 2 | `STRUCT_SCORE`, `STRUCT_ITEM` | Medium (hardcoded dims) |
| `STRUCT_ITEMLIST` | 2 | 0 | Medium |
| `STRUCT_GUILDZONE` | 2 | 0 | Medium |
| `STRUCT_MERC` | 2 | `STRUCT_ITEM` | Medium |
| `STRUCT_ITEMLOG` | 2 | 0 | Low |
| `STRUCT_GUILDINFO` | 2 | 0 | Low |
| `MSG_NPAccountInfo` | 2 | `STRUCT_ACCOUNTFILE` | High (largest payload) |
| `MSG_UpdateScore` | 2 | `STRUCT_SCORE` | High |
| `MSG_UseItem` | 2 | 0 | Medium |
| `STRUCT_SEALOFSOUL` | 1 (only via `MSG_CNFDBCapsuleInfo`) | 0 | Low |
| `STRUCT_DAM` | 0 by name (embedded in `MSG_Attack*`) | 0 | High in practice |
| `STRUCT_CAPSULE` | 1 | `STRUCT_MOB`, `STRUCT_MOBEXTRA` | Medium (persisted) |
| `STRUCT_CASTLEQUEST` | 1 | `STRUCT_ITEM` | Medium |
| `STRUCT_BEASTBONUS` | 1 | 0 | Low |
| `STRUCT_PISTA` | 1 | 0 | Low |
| `STRUCT_TREASURE` | 1 | `STRUCT_ITEM` | Low |
| `STRUCT_BLOCKMAC` | 1 | 0 | Low |
| `STRUCT_ACCOUNTINFO` | 1 | 0 | High (part of persisted record) |
| `STRUCT_SPELL` | 1 | 0 | Medium |
| `STRUCT_INITITEM` | 1 | 0 | Low |
| `STRUCT_CAMP` | 0 | 0 | Dead |
| `STRUCT_LEVELBONUS` | 0 | 0 | Dead |

### 6.3 Cohesion assessment

The header exhibits **low internal cohesion by design and high external coupling by necessity**. It is a "god header": 2,505 lines covering three unrelated concerns (content tuning, binary persistence, wire protocol) plus a function-prototype block and a global-variable block. Within it, cohesion is high per region — all persistence structs are together, all message constants are grouped by direction with banner comments — but there is no boundary between the three concerns, so a consumer that only needs `MAX_EQUIP` still transitively acquires the whole persistence layout and all 191 message types.

Two measurable cohesion violations stand out. First, `STRUCT_MOBEXTRA` mixes eight unrelated concerns (class master, quest history, two archived celestial records, login timestamps, a ticket counter, a currency "hold", a daily exp log, a donation log) into one 536-byte record, which is why it needs `char EMPTY[30]` five times and `int EMPTY[9]`. Second, the constant block mixes physical constants (`MAX_GRIDX`), policy constants (`ADMIN_RESERV`), protocol identifiers (`APP_VERSION`), gameplay tuning (`REF_15`, `AFFECT_1H`) and content identifiers (35 quest-NPC ids, 90 NPC-generator ids) without any delimiter other than `#pragma region`, and the content-identifier block alone is 113 lines (35% of the defines region).

### 6.4 Verified layout table (empirical, MSVC 14.44.35207, x86 == x64)

| Struct | Size | Align | Struct | Size | Align |
|---|---|---|---|---|---|
| `STRUCT_ITEM` | 8 | 2 | `STRUCT_ACCOUNTFILE` | 7,808 | 8 |
| `STRUCT_SCORE` | 48 | 4 | `STRUCT_SPELL` | 84 | 4 |
| `STRUCT_MOB` | 816 | 8 | `STRUCT_ITEMLIST` | 140 | 4 |
| `STRUCT_MOBEXTRA` | 536 | 8 | `STRUCT_INITITEM` | 8 | 2 |
| `STRUCT_AFFECT` | 8 | 4 | `STRUCT_BLOCKMAC` | 16 | 4 |
| `STRUCT_LEVELBONUS` | 10 | 2 | `MSG_STANDARD` | 12 | 4 |
| `STRUCT_CAPSULE` | 1,352 | 8 | `MSG_STANDARDPARM` | 16 | 4 |
| `STRUCT_BEASTBONUS` | 68 | 4 | `MSG_STANDARDPARM2` | 20 | 4 |
| `STRUCT_TREASURE` | 52 | 2 | `MSG_STANDARDSHORTPARM2` | 16 | 4 |
| `STRUCT_PISTA` | 84 | 4 | `MSG_STANDARDPARM3` | 24 | 4 |
| `STRUCT_CASTLEQUEST` | 564 | 4 | `MSG_NPAccountInfo` | 7,832 | 8 |
| `STRUCT_MERC` | 796 | 4 | `MSG_DBSaveMob` / `MSG_SavingQuit` | 2,696 | 8 |
| `STRUCT_CAMP` | 428 | 4 | `MSG_CNFCharacterLogin` | 2,632 | 8 |
| `STRUCT_ITEMLOG` | 4 | 4 | `MSG_CNFClientCharacterLogin` | 1,832 | 8 |
| `STRUCT_GUILDZONE` | 84 | 4 | `MSG_DBCNFAccountLogin` | 1,928 | 8 |
| `STRUCT_SEALOFSOUL` | 60 | 4 | `MSG_UpdateCarry` | 528 | 4 |
| `STRUCT_SELCHAR` | 840 | 8 | `MSG_ShopList` | 236 | 4 |
| `STRUCT_ACCOUNTINFO` | 220 | 4 | `MSG_CreateMob` | 232 | 4 |
| `STRUCT_GUILDINFO` | 8 | 2 | `MSG_CreateMobTrade` | 252 | 4 |
| `STRUCT_RANKING` (pack 1) | 52 | 1 | `MSG_Attack` | 168 | 8 |
| `MSG_AccountLogin` (pack 1) | 120 | 1 | `MSG_AttackOne` (pack 1) | 68 | 1 |
| `MSG_AccountLogin_HWID` (pack 1) | 170 | 1 | `MSG_AttackTwo` | 80 | 8 |
| `MSG_UpdateScore` (pack 1) | 152 | 1 | `MSG_Trade` | 156 | 4 |
| `MSG_SendExpRanking` | 172 | 4 | `MSG_SendAutoTrade` | 196 | 4 |
| `MSG_UpdateExpRanking` | 64 | 4 | `MSG_CombineItem` | 84 | 4 |
| `MSG_UseItem` | 36 | 4 | `MSG_CreateItem` | 32 | 4 |

`STRUCT_ACCOUNTFILE` field offsets: `Info` 0, `Char` 224, `Cargo` 3488, `Coin` 4512, `ShortSkill` 4516, `affect` 4580, `mobExtra` 5608, `Donate` 7752, `TempKey` 7756, size 7808.
`STRUCT_MOB` field offsets: `Coin` 28, `Exp` 32, `BaseScore` 44, `Equip` 140, `Carry` 268, `Resist` 806.
`STRUCT_MOBEXTRA` field offsets: `QuestInfo` 12, `SaveCelestial` 160, `LastNT` 432, `KefraTicket` 444, `Hold` 456, `EMPTY` 496; `QuestInfo` 146 bytes, `SaveCelestial[0]` 136 bytes.

---

## 7. Protocol Endpoints

This component exposes no HTTP, GraphQL or gRPC surface. Its endpoints are the 191 wire message types, which are the only externally reachable entry points of both servers. They are listed here because the header — not any handler file — is the registry that defines them. `Type` values are shown in hex; base id is the low byte(s).

### 7.1 Game to client — `FLAG_GAME2CLIENT` (0x0100), 27 endpoints

| Endpoint (message) | Type | Base | Payload | Line |
|---|---|---|---|---|
| `_MSG_MessagePanel` | 0x0101 | 1 | `MSG_MessagePanel` (140) | 1194 |
| `_MSG_MessageBoxOk` | 0x0102 | 2 | `MSG_MessageBoxOk` (116) | 1201 |
| `_MSG_CNFAccountLogin` | 0x010A | 10 | `MSG_DBCNFAccountLogin` (1,928) | 1211 |
| `_MSG_CNFNewCharacter` | 0x0110 | 16 | `MSG_CNFNewCharacter` (856) | 1300 |
| `_MSG_CNFDeleteCharacter` | 0x0112 | 18 | `MSG_CNFDeleteCharacter` (856) | 1308 |
| `_MSG_CNFCharacterLogin` | 0x0114 | 20 | `MSG_CNFCharacterLogin` (2,632) / `MSG_CNFClientCharacterLogin` (1,832) | 1344 |
| `_MSG_CNFCharacterLogout` | 0x0116 | 22 | `MSG_STANDARD` (12) | 1389 |
| `_MSG_NewAccountFail` | 0x0117 | 23 | `MSG_STANDARD` | 1390 |
| `_MSG_CharacterLoginFail` | 0x0119 | 25 | `MSG_STANDARD` | 1392 |
| `_MSG_NewCharacterFail` | 0x011A | 26 | `MSG_STANDARD` | 1393 |
| `_MSG_DeleteCharacterFail` | 0x011B | 27 | `MSG_STANDARD` | 1394 |
| `_MSG_AlreadyPlaying` | 0x011C | 28 | `MSG_STANDARD` | 1395 |
| `_MSG_StillPlaying` | 0x011D | 29 | `MSG_STANDARD` | 1396 |
| `_MSG_RemoveMob` | 0x0165 | 101 | `MSG_RemoveMob` (16) | 1582 |
| `_MSG_PKInfo` | 0x0166 | 102 | `MSG_STANDARDPARM` | 1590 |
| `_MSG_DecayItem` | 0x016F | 111 | `MSG_DecayItem` (16) | 1609 |
| `_MSG_CNFGetItem` | 0x0171 | 113 | `MSG_CNFGetItem` (28) | 1629 |
| `_MSG_CNFDropItem` | 0x0175 | 117 | `MSG_CNFDropItem` (28) | 1850 |
| `_MSG_ShopList` | 0x017C | 124 | `MSG_ShopList` (236) | 1650 |
| `_MSG_SetHpMp` | 0x0181 | 129 | `MSG_SetHpMp` (28) | 1659 |
| `_MSG_SendItem` | 0x0182 | 130 | `MSG_SendItem` (24) | 1672 |
| `_MSG_UpdateCarry` | 0x0185 | 133 | `MSG_UpdateCarry` (528) | 1455 |
| `_MSG_SetHpDam` | 0x018A | 138 | `MSG_SetHpDam` (20) | 1684 |
| `_MSG_UpdateWeather` | 0x018B | 139 | `MSG_UpdateWeather` (16) | 1793 |
| `_MSG_ReqChallange` | 0x018D | 141 | `MSG_STANDARD` | 1803 |
| `_MSG_SetClan` | 0x0193 | 147 | `MSG_STANDARDPARM` | 1398 |
| `_MSG_CloseShop` | 0x0196 | 150 | `MSG_STANDARD` | 1819 |

### 7.2 Client to game — `FLAG_CLIENT2GAME` (0x0200), 28 endpoints

| Endpoint | Type | Base | Payload | Line |
|---|---|---|---|---|
| `_MSG_AccountLogin` | 0x020D | 13 | `MSG_AccountLogin` (120) / `_HWID` (170) | 1210 |
| `_MSG_CreateCharacter` | 0x020F | 15 | `MSG_CreateCharacter` (36) | 1259 |
| `_MSG_DeleteCharacter` | 0x0211 | 17 | `MSG_DeleteCharacter` (48) | 1269 |
| `_MSG_CharacterLogin` | 0x0213 | 19 | `MSG_CharacterLogin` (20) | 1335 |
| `_MSG_CharacterLogout` | 0x0215 | 21 | `MSG_STANDARD` | 1387 |
| `_MSG_CreateItem` | 0x026E | 110 | `MSG_CreateItem` (32) | 1592 |
| `_MSG_GetItem` | 0x0270 | 112 | `MSG_GetItem` (28) | 1618 |
| `_MSG_DropItem` | 0x0272 | 114 | `MSG_DropItem` (32) | 1638 |
| `_MSG_ApplyBonus` | 0x0277 | 119 | `MSG_ApplyBonus` (20) | 1769 |
| `_MSG_REQShopList` | 0x027B | 123 | `MSG_REQShopList` (16) | 1779 |
| `_MSG_Restart` | 0x0289 | 137 | `MSG_STANDARD` | 1787 |
| `_MSG_Quest` | 0x028B | 139 | `MSG_STANDARDPARM2` | 1801 |
| `_MSG_Deprivate` | 0x028C | 140 | `MSG_STANDARDPARM` | 1802 |
| `_MSG_Challange` | 0x028E | 142 | `MSG_STANDARDPARM` | 1804 |
| `_MSG_ChallangeConfirm` | 0x028F | 143 | `MSG_STANDARD` | 1805 |
| `_MSG_ReqTeleport` | 0x0290 | 144 | `MSG_STANDARD` | 1806 |
| `_MSG_ChangeCity` | 0x0291 | 145 | `MSG_STANDARD` | 1807 |
| `_MSG_SetHpMode` | 0x0292 | 146 | `MSG_SetHpMode` (20) | 1811 |
| `_MSG_CombineItemLindy` | 0x02C3 | 195 | `MSG_CombineItem` (84) | 1945 |
| `_MSG_CombineItemShany` | 0x02C4 | 196 | `MSG_CombineItem` | 1946 |
| `_MSG_CapsuleInfo` | 0x02CD | 205 | `MSG_STANDARDPARM` | 1947 |
| `_MSG_CombineItemOdin` | 0x02D2 | 210 | `MSG_CombineItem` | 1948 |
| `_MSG_CombineItemEhre` | 0x02D3 | 211 | `MSG_CombineItem` | 1952 |
| `_MSG_CombineItemExtracao` | 0x02D4 | 212 | `MSG_CombineItem` | 1949 |
| `_MSG_CombineItemAlquimia` | 0x02E1 | 225 | `MSG_CombineItem` | 1950 |
| `_MSG_CombineItemOdin2` | 0x02E2 | 226 | `MSG_CombineItem` | 1951 |
| `_MSG_DeleteItem` | 0x02E4 | 228 | `MSG_DeleteItem` (20) | 1982 |
| `_MSG_SplitItem` | 0x02E5 | 229 | `MSG_SplitItem` (24) | 1992 |

### 7.3 Bidirectional client/game — `FLAG_GAME2CLIENT | FLAG_CLIENT2GAME` (0x0300), 58 endpoints

Base ids: 51, 52, 54, 55, 56, 57, 99, 100, 102, 103, 104, 105, 106, 107, 108, 115, 116, 118, 120, 121, 122, 125, 126, 127, 131, 132, 134, 135, 136, 151, 152, 153, 154, 155, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 171, 172, 173, 176, 180, 181, 185, 186, 187, 192, 204, 213 (lines 1439-2200).

Representative rows:

| Endpoint | Type | Base | Payload | Line |
|---|---|---|---|---|
| `_MSG_MessageChat` | 0x0333 | 51 | `MSG_MessageChat` (108) | 1439 |
| `_MSG_MessageWhisper` | 0x0334 | 52 | `MSG_MessageWhisper` (128) | 1447 |
| `_MSG_UpdateScore` | 0x0336 | 54 | `MSG_UpdateScore` (152, pack 1) | 1466 |
| `_MSG_UpdateEtc` | 0x0337 | 55 | `MSG_UpdateEtc` (48) | 1494 |
| `_MSG_CNFMobKill` | 0x0338 | 56 | `MSG_CNFMobKill` (32) | 1511 |
| `_MSG_UpdateCargoCoin` | 0x0339 | 57 | `MSG_STANDARDPARM` | 1523 |
| `_MSG_CreateMobTrade` | 0x0363 | 99 | `MSG_CreateMobTrade` (252) | 1525 |
| `_MSG_CreateMob` | 0x0364 | 100 | `MSG_CreateMob` (232) | 1553 |
| `_MSG_Action2` | 0x0366 | 102 | `MSG_Action` (52) | 1694 |
| `_MSG_Attack` | 0x0367 | 103 | `MSG_Attack` (168) / `AttackOne` (68) / `AttackTwo` (80) | 2011 |
| `_MSG_Action3` | 0x0368 | 104 | `MSG_Action` | 1695 |
| `_MSG_NoViewMob` | 0x0369 | 105 | `MSG_STANDARD` | 1580 |
| `_MSG_Motion` | 0x036A | 106 | `MSG_Motion` (20) | 1710 |
| `_MSG_UpdateEquip` | 0x036B | 107 | `MSG_UpdateEquip` (60) | 1720 |
| `_MSG_Action` | 0x036C | 108 | `MSG_Action` (52) | 1693 |
| `_MSG_UseItem` | 0x0373 | 115 | `MSG_UseItem` (36) | 1821 |
| `_MSG_UpdateItem` | 0x0374 | 116 | `MSG_UpdateItem` (20) | 1839 |
| `_MSG_TradingItem` | 0x0376 | 118 | `MSG_TradingItem` (20) | 1728 |
| `_MSG_SetShortSkill` | 0x0378 | 120 | `MSG_SetShortSkill` (32) | 1741 |
| `_MSG_Buy` | 0x0379 | 121 | `MSG_Buy` (24) | 1749 |
| `_MSG_Sell` | 0x037A | 122 | `MSG_Sell` (20) | 1760 |
| `_MSG_CNFAddParty` | 0x037D | 125 | `MSG_CNFAddParty` (40) | 1861 |
| `_MSG_RemoveParty` | 0x037E | 126 | `MSG_RemoveParty` (16) | 1898 |
| `_MSG_SendReqParty` | 0x037F | 127 | `MSG_SendReqParty` (48) | 1878 |
| `_MSG_Trade` | 0x0383 | 131 | `MSG_Trade` (156) | 2046 |
| `_MSG_QuitTrade` | 0x0384 | 132 | `MSG_STANDARD` | 2059 |
| `_MSG_CNFCheck` | 0x0386 | 134 | `MSG_STANDARD` | 2060 |
| `_MSG_Withdraw` | 0x0387 | 135 | `MSG_STANDARDPARM` | 1788 |
| `_MSG_Deposit` | 0x0388 | 136 | `MSG_STANDARDPARM` | 1789 |
| `_MSG_SendAutoTrade` | 0x0397 | 151 | `MSG_SendAutoTrade` (196) | 1908 |
| `_MSG_ReqBuy` | 0x0398 | 152 | `MSG_ReqBuy` (36) | 1925 |
| `_MSG_PKMode` | 0x0399 | 153 | `MSG_STANDARDPARM` | 1790 |
| `_MSG_ReqTradeList` | 0x039A | 154 | `MSG_STANDARDPARM` | 1791 |
| `_MSG_ItemSold` | 0x039B | 155 | `MSG_STANDARDPARM2` | 1940 |
| `_MSG_AttackOne` | 0x039D | 157 | `MSG_AttackOne` (68, pack 1) | 2062 |
| `_MSG_AttackTwo` | 0x039E | 158 | `MSG_AttackTwo` (80) | 2099 |
| `_MSG_ReqRanking` | 0x039F | 159 | `MSG_STANDARDPARM2` | 2135 |
| `_MSG_Ping` | 0x03A0 | 160 | `MSG_STANDARD` | 2136 |
| `_MSG_StartTime` | 0x03A1 | 161 | `MSG_STANDARD` | 2138 |
| `_MSG_EnvEffect` | 0x03A2 | 162 | `MSG_EnvEffect` (24) | 2140 |
| `_MSG_SoundEffect` | 0x03A3 | 163 | `MSG_STANDARDPARM` | 2154 |
| `_MSG_GuildDisable` | 0x03A4 | 164 | `MSG_STANDARD` | 2155 |
| `_MSG_GuildBoard` | 0x03A5 | 165 | `MSG_STANDARDPARM2` | 2156 |
| `_MSG_CombineItem` | 0x03A6 | 166 | `MSG_CombineItem` (84) | 1943 |
| `_MSG_CombineComplete` | 0x03A7 | 167 | `MSG_STANDARDPARM` | 1965 |
| `_MSG_SendWarInfo` | 0x03A8 | 168 | `MSG_STANDARD` | 2157 |
| `_MSG_AcceptParty` | 0x03AB | 171 | `MSG_AcceptParty` (32) | 2159 |
| `_MSG_SendCastleState` | 0x03AC | 172 | `MSG_STANDARD` | 2179 |
| `_MSG_SendCastleState2` | 0x03AD | 173 | `MSG_STANDARD` | 2180 |
| `_MSG_MobLeft` | 0x03B0 | 176 | `MSG_STANDARDPARM` | 2182 |
| `_MSG_SendArchEffect` | 0x03B4 | 180 | `MSG_STANDARDPARM` | 2184 |
| `_MSG_CombineItemAilyn` | 0x03B5 | 181 | `MSG_CombineItem` | 1953 |
| `_MSG_SendAffect` | 0x03B9 | 185 | `MSG_SendAffect` (268) | 2186 |
| `_MSG_CombineItemAgatha` | 0x03BA | 186 | `MSG_CombineItem` | 1954 |
| `_MSG_MobCount` | 0x03BB | 187 | `MSG_STANDARDPARM2` | 2194 |
| `_MSG_CombineItemTiny` | 0x03C0 | 192 | `MSG_CombineItem` | 1944 |
| `_MSG_PutoutSeal` | 0x03CC | 204 | `MSG_PutoutSeal` (52) | 1968 |
| `_MSG_InviteGuild` | 0x03D5 | 213 | `MSG_STANDARDPARM2` | 1966 |

### 7.4 DB to game — `FLAG_DB2GAME` (0x0400), 28 endpoints

Base ids 1, 2, 10, 11, 14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39 (lines 976-1133). Notable: `_MSG_DBCNFServerChange 0x0542` and `_MSG_DBServerSend1 0x0543` carry an extra `FLAG_GAME2CLIENT` bit (composite namespace 0x0500).

### 7.5 Game to DB — `FLAG_GAME2DB` (0x0800), 16 endpoints

Base ids 1, 2, 3, 4, 5, 6, 7, 9, 10, 12, 13, 20, 21, 22, 23, 31 (lines 978-1163). Composite 0x0C00 (both DB directions): `_MSG_MessageDBImple` (11), `_MSG_DBSendItem` (15), `_MSG_DBSendDonate` (17), `_MSG_DBCapsuleInfo` (60), `_MSG_DBPutInCapsule` (61), `_MSG_DBOutCapsule` (62). Composite 0x0D00 (+game→client): `_MSG_MagicTrumpet` (29), `_MSG_DBNotice` (30), `_MSG_CNFDBCapsuleInfo` (31). Composite 0x0E00 (+client→game): `_MSG_War` (14), `_MSG_GuildAlly` (18), `_MSG_GuildInfo` (19). Composite 0x0700: `_MSG_TransperCharacter` (169). Composite 0x0F00 (all four): `_MSG_AccountSecure` (222), `_MSG_AccountSecureFail` (223), `_MSG_ReqTransper` (170).

### 7.6 NP (admin tool) endpoints — 0x1000 / 0x2000 families, 15 endpoints

| Endpoint | Type | Base | Direction | Payload | Line |
|---|---|---|---|---|---|
| `_MSG_NPReqIDPASS` | 0x1001 | 1 | DB→NP | `MSG_STANDARD` | 2243 |
| `_MSG_NPIDPASS` | 0x2002 | 2 | NP→DB | `MSG_NPIDPASS` (48) | 2244 |
| `_MSG_NPReqAccount` | 0x2003 | 3 | NP→DB | `MSG_NPReqAccount` (44) | 2254 |
| `_MSG_NPNotFound` | 0x1004 | 4 | DB→NP | `MSG_STANDARD` | 2262 |
| `_MSG_NPAccountInfo` | 0x1005 | 5 | DB→NP | `MSG_NPAccountInfo` (7,832) | 2264 |
| `_MSG_NPReqSaveAccount` | 0x2006 | 6 | NP→DB | `MSG_NPAccountInfo` | 2273 |
| `_MSG_NPDisable` | 0x3007 | 7 | bidirectional | `MSG_STANDARD` | 2275 |
| `_MSG_NPEnable` | 0x3008 | 8 | bidirectional | `MSG_NPEnable` (36) | 2277 |
| `_MSG_NPNotice` | 0x3409 | 9 | NP/DB/game | `MSG_NPNotice` (132) | 2286 |
| `_MSG_NPState` | 0x300A | 10 | bidirectional | `MSG_STANDARDPARM` | 2296 |
| `_MSG_NPCreateCharacter` | 0x300B | 11 | bidirectional | `MSG_NPCreateCharacter` (848) | 2298 |
| `_MSG_NPCreateCharacter_Reply` | 0x300C | 12 | bidirectional | `MSG_NPCreateCharacter_Reply` (52) | 2307 |
| `_MSG_NPDonate` | 0x300D | 13 | bidirectional | `MSG_NPDonate` (32) | 2317 |
| `_MSG_NPAppeal` | 0x1C10 | 16 | NP/DB/game | `MSG_STANDARDPARM` | 2325 |

### 7.7 Ranking namespace — `FLAG_NEW` (0x4000), 2 endpoints

| Endpoint | Type | Payload | Line |
|---|---|---|---|
| `_MSG_GrindRankingData` | 0x4501 | `MSG_SendExpRanking` (172, C++ ctor sets Type/ID/Size) | 2196 |
| `_MSG_UpdateExpRanking` | 0x4D02 | `MSG_UpdateExpRanking` (64, C++ ctor) | 2217 |

### 7.8 Dispatch coverage

Of the 93 constants carrying `FLAG_CLIENT2GAME`, **64** appear as `case` labels in `TMSrv/ProcessClientMessage.cpp`. The remaining 29 are declared inbound-capable but never dispatched (for example `_MSG_UpdateEtc`, `_MSG_CNFMobKill`, `_MSG_UpdateCargoCoin`, `_MSG_CreateMob`, `_MSG_CreateMobTrade`, `_MSG_UpdateEquip`, `_MSG_Ping`, `_MSG_EnvEffect`, `_MSG_SendAffect`). The switch has **no `default` label** and returns silently at line 311-313, so an undispatched inbound message is discarded with no log entry and no crack counter increment.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / Format | Error Handling |
|---|---|---|---|---|
| Client socket (game tier) | External network | All gameplay traffic; 6,975-version gate | TCP, 12-byte header + payload, port 8281 | Frame size 12..8192; `ClientTick` sentinel; per-handler `AddCrackError`; no payload validation |
| DB socket (game tier, outbound) | Internal service | Persistence and cross-server messages | TCP, port 7514, `FLAG_GAME2DB`/`FLAG_DB2GAME` | `Type & FLAG_DB2GAME` gate; `ID < MAX_USER`; no retry, no timeout in the dispatcher |
| DB listener (DB tier) | Internal service | Accepts up to `MAX_SERVER` (10) game servers | TCP, port 7514 | `Type & FLAG_GAME2DB`; `ID <= MAX_USER`; logs and drops |
| Admin listener (DB tier) | External tool | NP tool, up to `MAX_ADMIN` (10) sessions | TCP, port 8895, `FLAG_DB2NP`/`FLAG_NP2DB` | `Type & FLAG_NP2DB`; `ID < MAX_DBACCOUNT` |
| Account store | Filesystem | One 7,808-byte record per account | Raw `_read`/`_write`, path `./account/<FirstKey>/<ACCOUNTNAME>` | `_filelength < 7808` -> partial `memset`; `errno` logged for EINVAL/EMFILE only |
| Ranking store | Filesystem | Reads the same account records for ranking | Raw `fread`, size heuristic 7,500..7,600 or exact `sizeof` | Falls through if size does not match either condition |
| Message string table | Filesystem | Localized strings, 2,000 x 128 | `UI/strdef.bin`, XOR 0x5A, trailing checksum | Checksum verification commented out (`Basedef.cpp:4951-4954`) |
| Item/skill/init-item tables | Filesystem | Content plane | CSV text -> binary caches (`ItemList.bin`, `SkillData.bin`, `InitItem.bin`), single-record `_read` of `sizeof(STRUCT_ITEMLIST)+2` | `sz < sizeof+2` rejected (`Basedef.cpp:2871-2873`) |
| Attribute map | Filesystem | Collision mask | `AttributeMap.dat`, 1024x1024 bytes | None; `fread(g_pAttribute, 1024, 1024, fp)` result ignored |
| Guild zone config | Filesystem | 5 zones, tax/clan/victory | `Guild.txt` / `GUILD_PATH` `fscanf` | Out-of-range tax clamped to 10 (`CReadFiles.cpp:811-812`); zero guild counter only logged |
| Billing server | External network | Optional payment/authorisation | TCP, port from config (`BillServerPort`) | Custom `WSA_READBILL` handler, `BillCounter = 360` cooldown |
| C++ runtime ABI | Toolchain | Struct layout underpinning wire + disk formats | MSVC default packing 8, `pack(1)` in 4 regions, `time_t` 8 bytes | None — mismatch is undetectable at runtime |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Shared-contract header (single source of truth) | `Basedef.h` compiled into both executables, no shared library | whole file | Guarantees TMSrv and DBSrv agree on 191 message identifiers, 100 payloads and the 7,808-byte disk layout without a linkage step |
| Macro-injected base class | `#define _MSG` expands a 12-byte header into every payload | `Basedef.h:925-930` | Structural inheritance in C: 100 payloads share an identical prologue by construction, enabling untyped dispatch |
| Tagged namespace over a flat integer | `Type = base_id | direction_flags`, 7 flags, 18 masks | `Basedef.h:932-941, 976-2325` | Routes 191 logical endpoints through one 16-bit field with no collision |
| Positional binary serialisation (flat-file record) | Raw `_read`/`_write` of `sizeof(STRUCT_ACCOUNTFILE)` | `Basedef.h:840-856`; `CFileDB.cpp:2444,2562` | Zero-copy persistence with no parser; trades schema evolution for speed |
| Reserved-hole forward compatibility | `char EMPTY[30]` x5, `int EMPTY[9]`, `char Unk2[765]`, `char Zero[52]` | `Basedef.h:503,516,536,540,566,590,1362` | Absorbs future fields without moving later offsets |
| Packed-wire layout pattern | `#pragma pack(push,1)` for `STRUCT_RANKING`, `MSG_AccountLogin`, `MSG_AccountLogin_HWID`, `MSG_UpdateScore`, `MSG_AttackOne` | `Basedef.h:808-835,1212-1246,1465-1492,2063-2097` | Locks exact client-visible byte counts where natural alignment would add padding |
| Bitmask namespaces on shared storage | `RSV_*` (16-bit `Rsv`), `MSV_SNOOP` (bit 0 of `Merchant`), `LearnedSkill` bits | `Basedef.h:184-194,445,461` | Packs nine status flags and 24 skills into existing fields |
| Sentinel-value protocol | `SKIPCHECKTICK`, `ESCENE_FIELD`, `MAX_DAMAGE_MG = 254`, `sanc` band 230..253 | `Basedef.h:166,172,206` | Encodes out-of-band semantics in values outside the legitimate range |
| Variant-by-size polymorphism | `MSG_Attack` / `AttackOne` / `AttackTwo`; `MSG_CNFCharacterLogin` / `MSG_CNFClientCharacterLogin`; `MSG_AccountLogin` / `_HWID` | `Basedef.h:1364-1386,2011-2133` | One `Type`, several layouts, selected by frame length |
| Table-driven content | 47 `extern` fixed arrays filled by CSV/bin loaders | `Basedef.h:2439-2503`; `Basedef.cpp` loaders | Data-driven tuning without recompilation |
| Compile-time capacity contract | `MAX_*` constants size both arrays and validation ranges | whole defines region | Capacity is a build-time constant, not a runtime configuration |
| Layered (partially enforced) | `MainWndProc` -> `ProcessClientMessage` -> `_MSG_*` handler -> `Basedef` rules -> `CPSock` | `Code/TMSrv` | Conventional only; handlers reach globals directly and the "layer" boundary is not enforced by types |

Architectural decisions visible in the header, with their consequences:

- **No versioning of anything.** `APP_VERSION` versions the client protocol, but the disk format, the payload layouts and the constant set are unversioned. A single build mismatch between client and server is detected (`BR-11`), a mismatch between server and existing account files is not (`BR-03`).
- **Two-tier compilation of one source file.** `Basedef.cpp` is a `ClCompile` item in both `TMSrv.vcxproj:91` and `DBSrv.vcxproj:94`, producing duplicate copies of 47 tables in two processes. This is a deliberate simplification (no DLL, no IPC for content) at the cost of ~10 MB of duplicated static data and a hard requirement that both projects build from the same commit.
- **Header as wiring hub.** Because `Server.h` includes `Basedef.h` (line 34) and `ProcessClientMessage.h` includes it (line 29), all 58 handler TUs acquire the entire domain surface plus all 101 prototypes. This is the mechanism behind the uniformly high efferent coupling of the handler layer reported by the architecture component.
- **Numeric literals over enumerations.** No `enum` is used anywhere in the file; every namespace (`SOUL_*`, `RSV_*`, `FLAG_*`, `REF_*`) is a sequence of `#define` or `const int`/`const short`. Only `const int`/`const short` are typed; the rest are untyped preprocessor substitutions, so type checking cannot catch a misuse such as passing a `MAX_*` where a flag is expected.

---

## 10. Technical Debt & Risks

| Risk | Risk Level | Component Area | Issue | Impact |
|---|---|---|---|---|
| R-01 | High | Contract enforcement | `BASE_CheckPacket` body is inside a block comment (`Basedef.cpp:6476-6594`) and the function unconditionally returns FALSE (`:6595`); no receive-side size validation exists for any of the 100 payload structures | Any handler can read past a short frame; the only guard is the 12..8192 transport window |
| R-02 | High | Persistence | 7,808-byte account record has no magic, version or length field; correctness depends on field order, natural alignment and `EMPTY` holes | A single field insertion silently reinterprets every existing account file; discovered only by corrupted data |
| R-03 | High | Persistence | Two readers disagree on acceptable file size: `DBReadAccount` accepts any length and zeroes 8 bytes; `CRanking` accepts 7,500..7,600 or exactly 7,808 | Rankings and login can interpret the same file differently |
| R-04 | High | Protocol sizing | `MSG_NPAccountInfo` is 7,832 bytes against `MAX_MESSAGE_SIZE` 8,192 (360-byte margin), with no `static_assert` (0 in repository) | Adding 361 bytes anywhere in `STRUCT_ACCOUNTFILE` makes the NP path unframeable with no diagnostic |
| R-05 | High | Bitmask aliasing | `STRUCT_MOB::Merchant` is simultaneously a merchant id (compared to 1/4/43/100), a 2-bit city field (bits 6-7) and `MSV_SNOOP` (bit 0) | `Merchant == 1` and `Merchant & MSV_SNOOP` coincide; NPC identity and GM invisibility can toggle each other |
| R-06 | High | Inventory geometry | `BASE_CanCarry`/`BASE_CanCargo` build the occupancy map with stride `*GRIDX` but test it with stride `*GRIDY` (`Basedef.cpp:4449` vs `:4468`; `:4516` vs `:4531`) | Placement checks consult the wrong cells; cargo stride 9 vs 14 is the most divergent case |
| R-07 | Medium | Level arithmetic | Global-level flattening uses `+MAX_LEVEL` (9 sites) and `+MAX_CLEVEL` (4 sites), plus `+MAX_CLEVEL + 1` for party leaders only (`_MSG_AcceptParty.cpp:93`) | Cross-domain comparisons are inconsistent by up to 200 levels; leader/member asymmetry of 1 |
| R-08 | Medium | Capacity coupling | `MOB_PER_ACCOUNT` (4) is duplicated as eight literal `4`s in `STRUCT_SELCHAR` and as `Slot <= 3` in `CReadFiles.cpp:237,814` | Raising the constant compiles but silently truncates the selection screen and skips the new slots |
| R-09 | Medium | Capacity coupling | `g_pGuildName[10][16][...]` and `g_pClientGuildName[16][...]` hardcode 10/16 instead of `MAX_SERVERGROUP`/`MAX_SERVERNUMBER` | Topology constants and guild storage can diverge without a compile error |
| R-10 | Medium | Dead surface | `STRUCT_CAMP` (428 B) and `STRUCT_LEVELBONUS` (10 B) are declared and referenced by no TU; `BASE_GetMobCheckSum` is implemented and never called; `MAX_CLASSMASTER` is declared and never referenced | Contract size without function; readers must reason about types and functions that carry no behaviour |
| R-11 | Medium | Dead flags | 7 of 9 `RSV_*` bits (`VISION`, `PARRY`, `HIDE`, `HASTE`, `CAST`, `BLOCK`, `IMUNIDADE`) are never referenced | Protocol surface that cannot be distinguished from "disabled feature" |
| R-12 | Medium | Namespace drift | `REF_11` = 12, `REF_12` = 15, `REF_13` = 18, `REF_14` = 22, `REF_15` = 27 — name and value disagree by construction | Index/level mapping errors are easy and invisible; `sanc % 10` produces 0,2,5,8,2,7 |
| R-13 | Medium | Over-broad direction flags | 58 constants claim `FLAG_GAME2CLIENT | FLAG_CLIENT2GAME`; only 64 of 93 inbound-capable ids are dispatched, and the switch has no `default` | Undispatched inbound traffic is silently dropped with no log and no crack counter |
| R-14 | Medium | Numeric literals | ~90 NPC-generator ids, 35 quest-NPC ids, mount ranges 2320/2330/2360/2390 and face 22-25/32 are all bare literals in code and header | Content changes require code edits; no single registry |
| R-15 | Medium | Overlapping id ranges | `SECRET_ROOM_*_END` equals a `SALA4_MOB_2` id and `*_BOSS_INITIAL` equals `SALA4_MOB_3` (three corridors) | Range loops double-process generators; boss spawn may be treated as a normal room mob |
| R-16 | Medium | Field overloading | `LearnedSkill` bit positions 15/17/19/20/21 are used as named flags with no constants; `CurrentScore.Merchant` is a partial copy of `MOB.Merchant` | Skill-table edits can alias a passive; two "Merchant" fields with different semantics |
| R-17 | Medium | Mixed affect encodings | `MSG_UpdateScore` carries packed `unsigned short Affect[32]` while `MSG_SendAffect` carries `STRUCT_AFFECT Affect[32]` | Same slot index, two incomparable encodings on the same direction |
| R-18 | Medium | Header/transport duplication | The 12-byte layout is defined twice: `_MSG` macro (`Basedef.h:925`) and `HEADER` struct (`CPSock.h:42`) | Any divergence breaks framing across both tiers with no compile error |
| R-19 | Low | Documentation defects | `SPX`/`SPY` both commented "The Y position" (`Basedef.h:452-453`, `550-551`); `STRUCT_SCORE::Merchant` commented "UNK"; `MAX_MESSAGE_SIZE` comment says "4K" for 8192 | Misreading leads to coordinate bugs and to assuming a 4 KB limit |
| R-20 | Low | Placeholders | 55 fields named `Unk*/unk*/EMPTY/Zero/Useless*/NotUsed`, 3 `TODO` at contract level (`:1095`, `:1511`, `:1523`) | Unverifiable protocol semantics; `MSG_CNFMobKill` layout explicitly unconfirmed |
| R-21 | Low | Style inconsistency | `STRUCT_GUILDINFO` is the only `typedef struct {...}` (every other type is `struct STRUCT_X`); `const int` flags mixed with `#define` constants | Inconsistent inclusion/linkage expectations; macro flags are untyped |
| R-22 | Low | Portability | Layout assumes 8-byte `time_t`, 8-byte `long long` alignment and default packing 8; only 4 regions use `pack(1)` | Verified identical on MSVC x86/x64, but not portable to another ABI without changing all four persisted structures |
| R-23 | Low | Unused payload variants | `MSG_AccountLogin_HWID` referenced once; `MSG_CNFClientCharacterLogin` referenced 4 times; both shadow larger/smaller siblings of the same `Type` | Two live layouts per endpoint with no central documentation of the discriminator |
| R-24 | Low | Legacy tolerance code | `CFileDB.cpp:2573-2574` zeroes only 8 of 64 `ShortSkill` bytes on short files; `CRanking.cpp:175` accepts a 7,500..7,600 window | Suggests a historical layout change that is not documented anywhere in the header |
| R-25 | Low | Range-check inconsistency | The same bound is written three ways against the same constant: `ID >= MAX_USER` (reject), `ID > MAX_USER` (accept), and `ID > MAX_MOB` (accept); `DBSrv/Server.cpp:1343` and `ProcessDBMessage.cpp:202` therefore accept `ID == MAX_USER` (1000), a value no `pUser[MAX_USER]` slot can hold | Latent off-by-one; currently harmless only because the downstream index is bounded by `MAX_DBACCOUNT` (10,000) instead |

---

## 11. Test Coverage Analysis

### 11.1 Test inventory: none

A repository-wide search (excluding `.git`, `.codegraph`, `.agents`, `.opencode`, `openspec`, `.vs`) was performed for every conventional test indicator:

| Indicator searched | Result |
|---|---|
| File or directory names matching `test`, `spec`, `gtest`, `catch`, `doctest`, `CMakeLists` | 0 matches |
| `.sln` projects | 2 only: `DBSrv`, `TMSrv` (neither is a test project) |
| `Test` / `gtest` / `unittest` occurrences in `TMSrv.vcxproj`, `DBSrv.vcxproj` | 0 matches |
| `assert(` occurrences in `Code/**.cpp|**.h` | 0 |
| `static_assert` occurrences anywhere in `Code/` | 0 |
| CI configuration (`.yml`, `.yaml`, `Makefile`, `CMakeLists.txt`, `.bat`, `.ps1`) | 0 matches |
| Property/golden files, fixtures, recorded packets | 0 matches (only `.obj`, `.tlog`, `.pdb`, `.ilk`, `.idb` build artefacts exist) |

There is no test harness, no test project, no assertion facility and no CI. Verification of this component is therefore entirely by execution: a contract change is validated only by running both servers against a real client and observing behaviour.

### 11.2 Coverage by artefact

| Artefact | Unit tests | Integration tests | Coverage | Assessment |
|---|---|---|---|---|
| Constants (`#define`, 262 + 191 message ids + 7 flags + 4 states) | 0 | 0 | 0% | Uniqueness of the 191 `Type` values is maintainable only by manual recomputation; this analysis had to script it |
| `STRUCT_*` layouts (26 types) | 0 | 0 | 0% | Layout is unverified at build time; verified here only by an external layout probe (x86 == x64) |
| `MSG_*` payloads (100 types) | 0 | 0 | 0% | `BASE_CheckPacket` would have been the assertion mechanism and is disabled |
| Header prototypes (101 `BASE_*`) | 0 | 0 | 0% | Exercised indirectly by handler code paths only |
| Extern tables (47) | 0 | 0 | 0% | Loader bounds are implicitly tested by running against real content files |

### 11.3 Nearest-to-a-test artefacts (not tests)

| Artefact | Location | What it verifies | Why it is not a test |
|---|---|---|---|
| `BASE_CheckPacket` | `Basedef.cpp:6475-6560` | Intended: `Size == sizeof(MSG_X)` for ~90 message types | Entire body commented out; only called under `#ifdef _PACKET_DEBUG` on the *send* path |
| Size-discriminator loops | `_MSG_Attack.cpp:1062,1087-1093` | Variant selection by `m->Size >= sizeof(...)` | Runtime input validation, not a test |
| `BASE_GetMobCheckSum` | `Basedef.cpp:2503-2517` | Detects equipment divergence (but zeroes `BaseScore` as a side effect) | Tamper check, not a test |
| File-size heuristic | `CRanking.cpp:175` | Accepts 7,500..7,600 or exactly 7,808 | Defensive tolerance, masks rather than detects format drift |
| `_PACKET_DEBUG` logging | `CPSock.cpp:544-551`, `Basedef.cpp:6482-6560` | Logs packet type/size mismatches | Observability hook, disabled in release |

### 11.4 Coverage gaps ranked by contract risk

1. **No layout assertion.** `static_assert(sizeof(STRUCT_ACCOUNTFILE) == 7808)` and equivalents for `STRUCT_MOB` (816), `STRUCT_MOBEXTRA` (536), `MSG_NPAccountInfo < MAX_MESSAGE_SIZE` would convert R-02/R-04 from latent corruption into compile errors. Zero exist.
2. **No message-id uniqueness check.** A script or `constexpr` set asserting all 191 values are distinct would have caught the 30 base-id reuses as intentional rather than accidental.
3. **No payload round-trip test.** No test builds each of the 100 `MSG_*` structures, serialises, deserialises and compares — which is the only cheap way to detect a `pack` or field-order regression.
4. **No file-format regression fixture.** Even a single 7,808-byte golden account file checked byte-for-byte on load and save would have prevented the historical drift implied by R-03 and R-24.
5. **No dispatch coverage check.** 29 inbound-capable message types are never dispatched and are dropped silently (R-13); no test enumerates the declared-vs-handled sets.

---

## Appendix A - Complete constant inventory (264 `#define` directives + 7 `const int` flags + 195 `const short` values)

### A.1 Non-protocol constants (262 `#define` constants, by block)

- Dialog/menu ids (8): `IDC_EXIT` 9000, `IDC_MOBRELOAD` 9001, `IDC_REBOOT` 9002, `IDC_SAVEALL` 9003, `IDC_READGUILD` 9004, `IDC_READGUILDNAME` 9005, `IDC_READGAMECONFIG` 9006, `IDC_SHUTDOWNNP` 9050
- Build/network (7): `APP_VERSION` 6975, `GAME_PORT` 8281, `DB_PORT` 7514, `ADMIN_PORT` 8895, `MAX_SERVER` 10, `MAX_SERVERGROUP` 10, `MAX_SERVERNUMBER` 11 (derived)
- Session/capacity (6): `MAX_IP_LENGTH` 16, `SECSTANDINGBY` 8, `SECBATTLE` 8, `MAX_USER` 1000, `ADMIN_RESERV` 10, `MAX_ADMIN` 10, `MAX_DBACCOUNT` 10000 (derived), `MAX_RANK_INDEX` 500
- Account (7): `ACCOUNTNAME_LENGTH` 16, `ACCOUNTPASS_LENGTH` 16, `REALNAME_LENGTH` 24, `EMAIL_LENGTH` 48, `ADDRESS_LENGTH` 78, `TELEPHONE_LENGTH` 16, `MOB_PER_ACCOUNT` 4
- Inventory (9): `MAX_EQUIP` 16, `MAX_CARRY` 64, `MAX_CARGO` 128, `MAX_TRADE` 15, `MAX_AUTOTRADE` 12, `MAX_AUTOTRADETITLE` 24, `MAX_SHOPLIST` 27, `MAX_GROUNDMASK` 6, `ITEM_PLACE_EQUIP/CARRY/CARGO` 0/1/2
- Grid (10): `CARGOGRIDX` 9, `CARGOGRIDY` 14, `CARRYGRIDX` 9, `CARRYGRIDY` 7, `VIEWGRIDX` 33, `VIEWGRIDY` 33, `HALFGRIDX` 16, `HALFGRIDY` 16, `MAX_GRIDX` 4096, `MAX_GRIDY` 4096
- Content capacities (20): `MAX_ITEM` 5000, `MAX_SUMMONLIST` 50, `MAX_MOB` 25000, `MAX_NPCGENERATOR` 8192, `MAX_SKILL` 24, `MAX_COMBINE` 8, `MAX_CLASS` 4, `MAX_LEVEL` 399, `MAX_CLEVEL` 199, `MAX_CLASSMASTER` 4, `MAX_AFFECT` 32, `MAX_ROUTE` 24, `MAX_TRANS` 20480, `MAX_ITEMLIST` 6500, `MAX_SKILLINDEX` 103, `MAX_INITITEM` 256, `MAX_EFFECTINDEX` 128, `ITEMNAME_LENGTH` 64, `MAX_STATICEFFECT` 12, `MAX_MOB_MERC` 128
- Text/misc lengths (9): `MAX_NOTIFY_LENGTH` 96, `MESSAGE_LENGTH` 96, `MESSAGEWHISPER_LENGTH` 100, `NAME_LENGTH` 16, `MH` 8, `TIMER_SEC` 0, `TIMER_MIN` 1, `GUILDNAME_LENGTH` 12, `MAX_STRING` 2000, `MAX_ENGLISH` 600, `MAX_MAC` 200, `MAX_ACCOUNTMAC` 2, `MAX_PIN_LENGTH` 37
- Guild/castle/party (9): `MAX_GUILD` 4096, `MAX_GUILDZONE` 5, `MAX_CASTLE_POS` 9, `MAX_CASTLE_QUEST` 64, `MAX_CAMP_QUEST` 64, `MAX_CAMP_PRIZE` 12, `KILL_MARK` 63, `MAX_PARTY` 12, `MAX_TARGET` 13
- Protocol sentinels (3): `ESCENE_FIELD` 30000, `SKIPCHECKTICK` 235543242, `_MSG` (macro)
- Nations/tiers (7): `REINO_RED` 8, `REINO_BLUE` 7, `MORTAL` 2, `ARCH` 1, `CELESTIAL` 3, `CELESTIALCS` 4, `SCELESTIAL` 5
- Flags (10): `RSV_FROST` 0x01, `RSV_DRAIN` 0x02, `RSV_VISION` 0x04, `RSV_PARRY` 0x08, `RSV_HIDE` 0x10, `RSV_HASTE` 0x20, `RSV_CAST` 0x40, `RSV_BLOCK` 0x80, `RSV_IMUNIDADE` 0x100, `MSV_SNOOP` 0x01
- Refinement (6): `REF_10` 10, `REF_11` 12, `REF_12` 15, `REF_13` 18, `REF_14` 22, `REF_15` 27
- Limits (6): `MAX_HP` 2000000000, `MAX_MP` 2000000000, `MAX_DAMAGE` 1000000, `MAX_DAMAGE_MG` 254, `AFFECT_1H` 450, `AFFECT_1D` 10800
- Souls (18): `SOUL_NULL` 0, `SOUL_KIBITA` 1, `SOUL_F` 2, `SOUL_I` 3, `SOUL_D` 4, `SOUL_C` 5, `SOUL_FI` 6, `SOUL_FD` 7, `SOUL_FC` 8, `SOUL_IF` 9, `SOUL_ID` 10, `SOUL_IC` 11, `SOUL_DF` 12, `SOUL_DI` 13, `SOUL_DC` 14, `SOUL_CF` 15, `SOUL_CI` 16, `SOUL_CD` 17
- Quest NPC ids (36): `QST_AMUMISTICO` 1, `GOLD_DRAGON`..`GUARDA_REAL_EVT1` 1..35
- NPC-generator ids (~90): secret rooms 2395..2424 (+ aliases 2402/2403, 2412/2413, 2422/2423), nightmare 2368..2394, rune quests 5653..5975, `RVRTORRE_1/2` 4236/4237, `GTORRE` 1078, `KEFRA_BOSS` 396, `KEFRA_MOB_INITIAL/END` 397/400, `ZAKUM_LORD` 4, `ORC_GUERREIRO` 3, `REI_HARABARD` 8, `REI_GLANTUAR` 9, `TORRE_NOATUM1-3` 23/24/25, `WATER_M/N/A_INITIAL` 10/171/183

### A.2 Protocol constants (191 `const short`, 18 flag masks)

| Mask | Direction semantics | Count | Base id range |
|---|---|---|---|
| 0x0100 | game to client | 27 | 1..150 |
| 0x0200 | client to game | 28 | 13..229 |
| 0x0300 | client and game (both) | 58 | 51..213 |
| 0x0400 | DB to game | 28 | 1..39 |
| 0x0500 | DB to game, game to client | 2 | 42, 43 |
| 0x0700 | client, DB, game (three-way) | 1 | 169 |
| 0x0800 | game to DB | 16 | 1..31 |
| 0x0C00 | game and DB (both) | 6 | 11..62 |
| 0x0D00 | DB, game, game to client | 3 | 29..31 |
| 0x0E00 | client, DB, game to DB | 3 | 14..19 |
| 0x0F00 | all four main directions | 3 | 170, 222, 223 |
| 0x1000 | DB to NP | 3 | 1, 4, 5 |
| 0x1C00 | DB, game, DB to NP | 1 | 16 |
| 0x2000 | NP to DB | 3 | 2, 3, 6 |
| 0x3000 | NP and DB (both) | 6 | 7..13 |
| 0x3400 | NP, DB, DB to game | 1 | 9 |
| 0x4500 | NEW, DB to game, game to client | 1 | 1 |
| 0x4D00 | NEW, DB, game to DB, game to client | 1 | 2 |

Exact-value collisions: **0**. Base ids reused across namespaces: **30** (highest reuse: base 1 and base 2 at 5 namespaces each; base 10, 11, 13, 15, 16, 17, 18, 19, 20, 22, 23 at 3 each).

### A.3 State constants (4 `const short`, lines 1834-1837)

| Constant | Value | Meaning | Representative use |
|---|---|---|---|
| `STATE_NOTHING` | 0 | no state / not applicable | declared only |
| `STATE_OPEN` | 1 | gate or container open | `_MSG_UpdateItem.cpp:98`, `imple.cpp:363`, `GetFunc.cpp:1232` |
| `STATE_CLOSED` | 2 | gate or container closed | `GetFunc.cpp:1220`, `imple.cpp:400` |
| `STATE_LOCKED` | 3 | gate or container locked | `imple.cpp:315,339`, `ProcessSecMinTimer.cpp:2179` |

These are `const short` (not `#define`) and share the file with the message ids, but they are not message types: they populate `MSG_UpdateItem::State` and `CItem::State`. See BR-21.

## Appendix B — Verified reference sites for cross-cutting constants

| Constant | Definition | Representative uses |
|---|---|---|
| `APP_VERSION` | `Basedef.h:42` | `_MSG_AccountLogin.cpp:26,44,47` |
| `GAME_PORT` | `Basedef.h:44` | `Server.cpp:3634` |
| `DB_PORT` | `Basedef.h:45` | `DBSrv/Server.cpp:586` |
| `ADMIN_PORT` | `Basedef.h:46` | `DBSrv/Server.cpp:587` |
| `MAX_USER` | `Basedef.h:56` | `ProcessClientMessage.cpp:42`, `CFileDB.cpp:247-248`, `Server.cpp:4161` |
| `ADMIN_RESERV` | `Basedef.h:57` | `_MSG_AccountLogin.cpp:30` |
| `MAX_DBACCOUNT` | `Basedef.h:60` | `CFileDB.cpp:59,204,2339`, `DBSrv/Server.cpp:630,1467` |
| `MOB_PER_ACCOUNT` | `Basedef.h:71` | `_MSG_CharacterLogin.cpp:25`, `CFileDB.cpp:259,865,1036,1142` |
| `SKIPCHECKTICK` | `Basedef.h:172` | `ProcessClientMessage.cpp:63`, `_MSG_Attack.cpp:59-205,1730` |
| `ESCENE_FIELD` | `Basedef.h:170` | `_MSG_Attack.cpp:25`, `_MSG_Buy.cpp:62,278`, `_MSG_CombineItem.cpp:41` |
| `MAX_TARGET` | `Basedef.h:173` | `_MSG_Attack.cpp:297,1056,1062,1740`, `GetFunc.cpp:1579` |
| `MAX_LEVEL` | `Basedef.h:117` | `Basedef.cpp:1184,3476,6353`, `_MSG_Action.cpp:76` |
| `MAX_CLEVEL` | `Basedef.h:118` | `_MSG_AcceptParty.cpp:93-94`, `_MSG_SendReqParty.cpp:68-69`, `_MSG_UseItem.cpp:1002` |
| `MORTAL`/`ARCH` | `Basedef.h:178-179` | `_MSG_AcceptParty.cpp:93`, `CFileDB.cpp:2619`, `Basedef.cpp:1184` |
| `MSV_SNOOP` | `Basedef.h:194` | `imple.cpp:1569,1576` |
| `RSV_FROST`/`RSV_DRAIN` | `Basedef.h:184-185` | `Basedef.cpp:3569,3574`, `_MSG_Attack.cpp:1450,1464` |
| `KILL_MARK` | `Basedef.h:166` | `GetFunc.cpp:2051-2142` |
| `CARRYGRIDX/Y`, `CARGOGRIDX/Y` | `Basedef.h:90-93` | `Basedef.cpp:4440-4535` |
| `REF_10`..`REF_15` | `Basedef.h:196-201` | `Basedef.cpp:2126-2136,2212-2215`, `_MSG_CombineItemOdin.cpp:73,206` |
| `MAX_ITEMLIST` etc. | `Basedef.h:139-144` | `Basedef.cpp:2736,2756,2830,2844,5148`, `Basedef.h:2464-2467` |

---

## Appendix C — Measured size of every declared payload (authoritative)

All 100 `MSG_*` structures measured with MSVC 14.44.35207 (x86 and x64 identical). Sizes are the total frame length including the 12-byte `_MSG` header.

| Payload | Bytes | Payload | Bytes | Payload | Bytes | Payload | Bytes |
|---|---|---|---|---|---|---|---|
| `MSG_STANDARD` | 12 | `MSG_STANDARDPARM` | 16 | `MSG_STANDARDPARM2` | 20 | `MSG_STANDARDSHORTPARM2` | 16 |
| `MSG_STANDARDPARM3` | 24 | `MSG_DBSaveMob` | 2,696 | `MSG_DBSavingQuit` | 32 | `MSG_MessageDBRecord` | 268 |
| `MSG_GuildZoneReport` | 32 | `MSG_DBClientMessage` | 108 | `MSG_DBCheckPrimaryAccount` | 44 | `MSG_DBCNFServerChange` | 80 |
| `MSG_DBServerChange` | 20 | `MSG_DBActivatePinCode` | 104 | `MSG_DBPrimaryAccount` | 32 | `MSG_DBCNFAccountLogin` | 1,928 |
| `MSG_GuildReport` | 212 | `MSG_NewAccount` | 232 | `MSG_DBCreateArchCharacter` | 40 | `MSG_SavingQuit` | 2,696 |
| `MSG_MessageDBImple` | 112 | `MSG_MessagePanel` | 140 | `MSG_MessageBoxOk` | 116 | `MSG_AccountLogin` | 120 |
| `MSG_AccountLogin_HWID` | 170 | `MSG_AccountSecure` | 32 | `MSG_CreateCharacter` | 36 | `MSG_DeleteCharacter` | 48 |
| `MSG_DBSendItem` | 40 | `MSG_DBSendDonate` | 36 | `MSG_CNFNewCharacter` | 856 | `MSG_CNFDeleteCharacter` | 856 |
| `MSG_GuildAlly` | 20 | `MSG_GuildInfo` | 24 | `MSG_CharacterLogin` | 20 | `MSG_CNFCharacterLogin` | 2,632 |
| `MSG_CNFClientCharacterLogin` | 1,832 | `MSG_MagicTrumpet` | 108 | `MSG_DBNotice` | 108 | `MSG_DBOutCapsule` | 56 |
| `MSG_CNFDBCapsuleInfo` | 76 | `MSG_MessageChat` | 108 | `MSG_MessageWhisper` | 128 | `MSG_UpdateCarry` | 528 |
| `MSG_UpdateScore` | 152 | `MSG_UpdateEtc` | 48 | `MSG_CNFMobKill` | 32 | `MSG_CreateMobTrade` | 252 |
| `MSG_CreateMob` | 232 | `MSG_RemoveMob` | 16 | `MSG_CreateItem` | 32 | `MSG_DecayItem` | 16 |
| `MSG_GetItem` | 28 | `MSG_CNFGetItem` | 28 | `MSG_DropItem` | 32 | `MSG_ShopList` | 236 |
| `MSG_SetHpMp` | 28 | `MSG_SendItem` | 24 | `MSG_SetHpDam` | 20 | `MSG_Action` | 52 |
| `MSG_Motion` | 20 | `MSG_UpdateEquip` | 60 | `MSG_TradingItem` | 20 | `MSG_SetShortSkill` | 32 |
| `MSG_Buy` | 24 | `MSG_Sell` | 20 | `MSG_ApplyBonus` | 20 | `MSG_REQShopList` | 16 |
| `MSG_UpdateWeather` | 16 | `MSG_SetHpMode` | 20 | `MSG_UseItem` | 36 | `MSG_UpdateItem` | 20 |
| `MSG_CNFDropItem` | 28 | `MSG_CNFAddParty` | 40 | `MSG_SendReqParty` | 48 | `MSG_RemoveParty` | 16 |
| `MSG_SendAutoTrade` | 196 | `MSG_ReqBuy` | 36 | `MSG_CombineItem` | 84 | `MSG_PutoutSeal` | 52 |
| `MSG_DeleteItem` | 20 | `MSG_SplitItem` | 24 | `MSG_Attack` | 168 | `MSG_Trade` | 156 |
| `MSG_AttackOne` | 68 | `MSG_AttackTwo` | 80 | `MSG_EnvEffect` | 24 | `MSG_AcceptParty` | 32 |
| `MSG_ReqTransper` | 52 | `MSG_SendAffect` | 268 | `MSG_SendExpRanking` | 172 | `MSG_UpdateExpRanking` | 64 |
| `MSG_NPIDPASS` | 48 | `MSG_NPReqAccount` | 44 | `MSG_NPAccountInfo` | 7,832 | `MSG_NPEnable` | 36 |
| `MSG_NPNotice` | 132 | `MSG_NPCreateCharacter` | 848 | `MSG_NPCreateCharacter_Reply` | 52 | `MSG_NPDonate` | 32 |

Largest five: `MSG_NPAccountInfo` 7,832; `MSG_DBSaveMob` / `MSG_SavingQuit` 2,696; `MSG_CNFCharacterLogin` 2,632; `MSG_DBCNFAccountLogin` 1,928; `MSG_CNFClientCharacterLogin` 1,832. Transport ceiling is `MAX_MESSAGE_SIZE` = 8,192 (`Code/CPSock.h:38`), leaving 360 bytes of headroom on the largest payload.

---

*End of report. No project file was modified during this analysis. The layout values in section 6.4 and Appendix C were obtained by compiling standalone probes against the unmodified header in a temporary directory outside the repository, using the project's own toolchain (MSVC 14.44.35207) for both x86 and x64.*

