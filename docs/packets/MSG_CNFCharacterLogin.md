# MSG_CNFCharacterLogin

## 1. Summary

| Field | Value |
|---|---|
| Client-facing Type constant | `_MSG_CNFCharacterLogin` = `(20 \| FLAG_GAME2CLIENT)` = `20 \| 0x0100` = **0x114** (Basedef.h:1344) |
| DB-mirror Type constant | `_MSG_DBCNFCharacterLogin` = `(23 \| FLAG_DB2GAME)` = `23 \| 0x0400` = **0x417** (Basedef.h:1111) |
| Direction (client-facing) | GAME2CLIENT (TMSrv → client) |
| Direction (DB-mirror) | DB2GAME (DBSrv → TMSrv) |
| Wire struct sent to client | **MSG_CNFClientCharacterLogin** (Basedef.h:1368) — sizeof **1832** |
| Struct sent DBSrv → TMSrv | **MSG_CNFCharacterLogin** (Basedef.h:1345) — sizeof **2632** |
| Primary handler | `ProcessDBMessage()` case `_MSG_DBCNFCharacterLogin` — TMSrv/ProcessDBMessage.cpp:635–1081 |
| DBSrv producer | `CFileDB.cpp` case `_MSG_DBCharacterLogin` — CFileDB.cpp:1028–1094 |
| Purpose | TMSrv informs the client that the selected character has entered the game world; it is the culmination of the login / character-select flow. Carries the full character state (STRUCT_MOB), equipment, affects, mob-extra (class-master info), position, and the ID the client uses to address itself. |
| Related client-request packet | `_MSG_CharacterLogin` (0x113, CLIENT2GAME) → forwarded as `_MSG_DBCharacterLogin` to DBSrv |

Two distinct structs exist (not aliases):

- **MSG_CNFCharacterLogin** (Basedef.h:1345–1367) — the DBSrv→TMSrv payload. It is **MSG_CNFClientCharacterLogin plus a trailing tail** of `STRUCT_AFFECT affect[32]`, `STRUCT_MOBEXTRA mobExtra`, and `int Donate` (the parts the client must not see).
- **MSG_CNFClientCharacterLogin** (Basedef.h:1368–1386) — the TMSrv→client payload; exactly the leading prefix of MSG_CNFCharacterLogin (identical field-for-field through `Unk2[765]`).

Disambiguation in code: `ProcessDBMessage.cpp:641` does `memcpy(&sm, m, sizeof(MSG_CNFClientCharacterLogin))` — it copies only the client-visible prefix (1832 bytes) out of the incoming MSG_CNFCharacterLogin `m` into a `MSG_CNFClientCharacterLogin sm`, then re-targets `sm.Type` to `_MSG_CNFCharacterLogin` (line 787) and sends `sizeof(MSG_CNFClientCharacterLogin)` (line 984). Because `affect` begins exactly at byte offset 1832 in MSG_CNFCharacterLogin, the copy boundary coincides with the padding after `Unk2`; the two structs are byte-compatible on the leading 1832 bytes.

## 2. Wire Framing

Standard CPSock framing (no per-packet deviation; a plain 12-byte `_MSG` header, CPSock.h:42–50).

**Common header `_MSG`** (Basedef.h:925–930, 12 bytes, LP32 little-endian, MSVC default /Zp8):

| Offset | Size | Type | Field | Semantics |
|---|---|---|---|---|
| 0 | 2 | short | Size | Total message length incl. header |
| 2 | 1 | char | KeyWord | Index into `pKeyWord` table |
| 3 | 1 | char | CheckSum | `Sum2 - Sum1` |
| 4 | 2 | short | Type | Message type constant |
| 6 | 2 | short | ID | Sender/recipient id |
| 8 | 4 | unsigned int | ClientTick | Tick/timestamp |

**Preamble / de-framing** (CPSock.cpp):
- The socket session begins with a 4-byte magic `INITCODE = 0x1F11F311` (CPSock.h:40) consumed at CPSock.cpp:382; everything after is obfuscated.
- Per-message framing (ReadMessage, CPSock.cpp:385–467): `Size` read at offset 0; must satisfy `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE` (MAX_MESSAGE_SIZE=8192, CPSock.h:38); payload from byte offset **4** is de-obfuscated in place with a per-byte, position-rotating XOR using the trans key `pKeyWord[rst*2+1]`, `rst = (KeyWord + i) % 256` (CPSock.cpp:430–453). The checksum field (offset 3) must equal `Sum2 - Sum1` over the de-obfuscated bytes.
- Send side (AddMessage, CPSock.cpp:513–591): picks `iKeyWord = rand()%256`, re-obfuscates bytes from offset 4 (inverse transforms, CPSock.cpp:558–581), sets `Size`, `ClientTick = CurrentTime`, and computes `CheckSum = Sum2 - Sum1`.
- `BASE_CheckPacket` (Basedef.cpp:6475) is disabled (only under `_PACKET_DEBUG`, CPSock.cpp:544–552).
- Billing uses a separate plain 196-byte protocol (ReadBillMessage/SendBillMessage, CPSock.cpp:469–511) — irrelevant to 0x114.

**Value used for Size:** the producer sets `sm.Size = sizeof(MSG_CNFCharacterLogin)` = 2632 (CFileDB.cpp:1058) for the DB leg; on the TMSrv→client leg `AddMessage` overwrites `Size` with the passed length `sizeof(MSG_CNFClientCharacterLogin)` = 1832 (ProcessDBMessage.cpp:984 → CPSock.cpp:538). No framing deviation.

## 3. Binary Layout

Packing context: **MSG_CNFCharacterLogin and MSG_CNFClientCharacterLogin fall OUTSIDE every `#pragma pack(push,1)` region** (those cover only STRUCT_RANKING 808–835, MSG_AccountLogin/HWID 1212–1246, MSG_UpdateScore 1465–1492, MSG_AttackOne 2063–2097). So MSVC default **/Zp8** applies: every member aligned to `min(sizeof(member), 8)`, struct size rounded up to the largest member alignment (8, because of `long long`/`time_t` in nested structs). Little-endian x86, LP32 (int=4, short=2, char=1, long long=8; no pointers on the wire).

### 3.1 Header

Identical to §2. The header occupies bytes 0–11 of both structs and is `_MSG` (Basedef.h:925).

| Offset | Size | Align | Field | Notes |
|---|---|---|---|---|
| 0 | 2 | 2 | short Size | total length |
| 2 | 1 | 1 | char KeyWord | |
| 3 | 1 | 1 | char CheckSum | |
| 4 | 2 | 2 | short Type | 0x417 (DB) / 0x114 (client) |
| 6 | 2 | 2 | short ID | DB leg: TMSrv conn id; client leg: `ESCENE_FIELD` or `ESCENE_FIELD+1` |
| 8 | 4 | 4 | unsigned int ClientTick | |

### 3.2 Payload — MSG_CNFClientCharacterLogin (TMSrv → client, sizeof 1832)

| Offset | Size | Align | Type | Field | Semantics |
|---|---|---|---|---|---|
| 12 | 2 | 2 | short | PosX | spawn X |
| 14 | 2 | 2 | short | PosY | spawn Y |
| 16 | 816 | 8 | STRUCT_MOB | mob | full character state |
| 832 | 208 | 1 | char | unk[208] | zeroed (ProcessDBMessage.cpp:801) |
| 1040 | 2 | 2 | unsigned short | Slot | selected char slot |
| 1042 | 2 | 2 | unsigned short | ClientID | = conn |
| 1044 | 2 | 2 | unsigned short | Weather | = CurrentWeather |
| 1046 | 16 | 1 | unsigned char | ShortSkill[16] | |
| 1062 | 2 | 1 | char | Unk[2] | zeroed (line 800) |
| 1064 | 765 | 1 | char | Unk2[765] | zeroed (line 763/799), then `Unk2[448..459]` = account name (12 bytes, line 803) |
| **1829** | **3** | — | **pad** | — | alignment to 8 |
| **1832** | | | | | **struct size = 1832** (align 8) |

`1829 = 12 + 4 + 816 + 208 + 2+2+2 + 16 + 2 + 765`; 1829 is not a multiple of 8 → padded to 1832.

### 3.2 Payload — MSG_CNFCharacterLogin (DBSrv → TMSrv, sizeof 2632)

Same as §3.2 client struct through `Unk2[765]` (offsets 12–1828), then the DB-only tail:

| Offset | Size | Align | Type | Field | Semantics |
|---|---|---|---|---|---|
| 0–1828 | 1829 | — | — | (client prefix, as §3.2) | |
| 1829 | 3 | — | **pad** | — | align affect to 4 |
| 1832 | 256 | 4 | STRUCT_AFFECT | affect[32] | affects (MAX_AFFECT=32, Basedef.h:122) |
| 2088 | 536 | 8 | STRUCT_MOBEXTRA | mobExtra | class-master / quest info |
| 2624 | 4 | 4 | int | Donate | donate coins |
| **2628** | **4** | — | **pad** | — | align to 8 |
| **2632** | | | | | **struct size = 2632** |

`2624 = 1832 + 256 + 536`; 2628 → padded to 2632.

### 3.3 Nested struct expansions

**STRUCT_ITEM** (Basedef.h:398–412) — sizeof **8**, align 2:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 2 | short | sIndex |
| 2 | 2 | short | stEffect[0] (union: sValue / {cEffect,cValue}) |
| 4 | 2 | short | stEffect[1] |
| 6 | 2 | short | stEffect[2] |

**STRUCT_SCORE** (Basedef.h:414–436) — sizeof **48**, align 4:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | int | Level |
| 4 | 4 | int | Ac |
| 8 | 4 | int | Damage |
| 12 | 1 | uchar | Merchant |
| 13 | 1 | uchar | AttackRun |
| 14 | 1 | uchar | Direction |
| 15 | 1 | uchar | ChaosRate |
| 16 | 4 | int | MaxHp |
| 20 | 4 | int | MaxMp |
| 24 | 4 | int | Hp |
| 28 | 4 | int | Mp |
| 32 | 2 | short | Str |
| 34 | 2 | short | Int |
| 36 | 2 | short | Dex |
| 38 | 2 | short | Con |
| 40 | 8 | short[4] | Special |

**STRUCT_MOB** (Basedef.h:438–481) — sizeof **816**, align 8. Math: header-free member offsets accumulate to 810 bytes used; largest alignment is 8 (long long Exp), so rounded to 816. Cross-check: `STRUCT_ACCOUNTFILE.Char[4]` spans 216–3480 = 3264 bytes ⇒ 3264/4 = **816** (Basedef.h:844 comment).

| Offset | Size | Align | Type | Field |
|---|---|---|---|---|
| 0 | 16 | 1 | char[16] | MobName |
| 16 | 1 | 1 | char | Clan |
| 17 | 1 | 1 | uchar | Merchant |
| 18 | 2 | 2 | ushort | Guild |
| 20 | 1 | 1 | uchar | Class |
| 22 | 2 | 2 | ushort | Rsv |
| 24 | 1 | 1 | uchar | Quest |
| 28 | 4 | 4 | int | Coin |
| 32 | 8 | 8 | long long | Exp |
| 40 | 2 | 2 | short | SPX |
| 42 | 2 | 2 | short | SPY |
| 44 | 48 | 4 | STRUCT_SCORE | BaseScore |
| 92 | 48 | 4 | STRUCT_SCORE | CurrentScore |
| 140 | 128 | 2 | STRUCT_ITEM[16] | Equip (MAX_EQUIP=16) |
| 268 | 512 | 2 | STRUCT_ITEM[64] | Carry (MAX_CARRY=64) |
| 780 | 4 | 4 | long | LearnedSkill |
| 784 | 4 | 4 | unsigned int | Magic |
| 788 | 2 | 2 | ushort | ScoreBonus |
| 790 | 2 | 2 | ushort | SpecialBonus |
| 792 | 2 | 2 | ushort | SkillBonus |
| 794 | 1 | 1 | uchar | Critical |
| 795 | 1 | 1 | uchar | SaveMana |
| 796 | 4 | 1 | uchar[4] | SkillBar |
| 800 | 1 | 1 | uchar | GuildLevel |
| 802 | 2 | 2 | ushort | RegenHP |
| 804 | 2 | 2 | ushort | RegenMP |
| 806 | 4 | 1 | char[4] | Resist |
| 810 | 6 | — | **pad** | — |

**STRUCT_AFFECT** (Basedef.h:593–599) — sizeof **8**, align 4: `uchar Type@0, uchar Value@1, ushort Level@2, uint Time@4` → ends 8.

**STRUCT_MOBEXTRA** (Basedef.h:483–591) — sizeof **536**, align 8 (long long/time_t). time_t is 64-bit (8 bytes) in MSVC (no `_USE_32BIT_TIME_T` defined). Math:

| Offset | Size | Align | Type | Field |
|---|---|---|---|---|
| 0 | 2 | 2 | short | ClassMaster |
| 2 | 1 | 1 | char | Citizen |
| 4 | 4 | 4 | int | Fame |
| 8 | 1 | 1 | char | Soul |
| 10 | 2 | 2 | short | MortalFace |
| 12 | 144 | 2 | struct | QuestInfo |
| 160 | 272 | 8 | struct[2] | SaveCelestial (align 8; 12+144=156→pad→160) |
| 432 | 8 | 8 | time_t | LastNT |
| 440 | 4 | 4 | int | NT |
| 444 | 4 | 4 | int | KefraTicket |
| 448 | 8 | 8 | time_t | DivineEnd |
| 456 | 4 | 4 | unsigned int | Hold |
| 464 | 16 | 8 | struct | DayLog {long long Exp; int YearDay;} |
| 480 | 16 | 8 | struct | DonateInfo {time_t LastTime; int Count;} |
| 496 | 36 | 4 | int[9] | EMPTY |
| 532 | 4 | — | **pad** | align 8 |
| **536** | | | | **sizeof** |

`QuestInfo` (Basedef.h:494–542) = 144 bytes: `Mortal`(34) @12–45, `Arch`(35) @46–80, `Celestial`(44) @81–124, `Circle` @125, `EMPTY[30]` @126–155.
`SaveCelestial` (Basedef.h:544–567) = 136 bytes each: `int Class@0, [pad4], long long Exp@8, short SPX@16, short SPY@18, STRUCT_SCORE BaseScore@20(48)→68, long LearnedSkill@68, ushort ScoreBonus@72, ushort SpecialBonus@74, ushort SkillBonus@76, uchar SkillBar1[4]@78, uchar SkillBar2[16]@82, char Soul@98, char EMPTY[30]@99` → ends 129 → align 8 = 136.

### 3.4 Size verification

| Check | Expression | Expected | Source |
|---|---|---|---|
| DBSrv sets Size | `sm.Size = sizeof(MSG_CNFCharacterLogin)` | 2632 | CFileDB.cpp:1058 |
| DBSrv sends | `SendOneMessage(&sm, sizeof(MSG_CNFCharacterLogin))` | 2632 | CFileDB.cpp:1080 |
| TMSrv copies prefix | `memcpy(&sm, m, sizeof(MSG_CNFClientCharacterLogin))` | 1832 | ProcessDBMessage.cpp:641 |
| TMSrv sends | `SendOneMessage(&sm, sizeof(MSG_CNFClientCharacterLogin))` | 1832 | ProcessDBMessage.cpp:984 |
| STRUCT_MOB | cross-check via `Char[4] @216–3480` | 816 | Basedef.h:844 |

All consistent. No mismatch found. The two structs share an identical 1832-byte prefix because `affect` in MSG_CNFCharacterLogin begins exactly at the padded end of `Unk2`. Members not referenced by either handler (`unk`, `Unk`, `Unk2`, weather semantics) are documented as zeroed/opaque rather than assumed.

## 4. Lifecycle & Flow

**Producer — DBSrv** (`CFileDB.cpp:1028–1094`, case `_MSG_DBCharacterLogin`):
1. Validate `Slot` in `[0, MOB_PER_ACCOUNT)` (1036) and `SecurePass != -1` (1045).
2. `pAccountList[Idx].Slot = Slot` (1052).
3. Build `MSG_CNFCharacterLogin sm`; set `Type=_MSG_DBCNFCharacterLogin`, `ID=m->ID` (the TMSrv conn), `Size`, `Slot` (1054–1060).
4. Fill `sm.mob` from `File.Char[Slot]`, `sm.Donate`, `ShortSkill`, `affect`, `mobExtra` (1062–1067).
5. Reject empty `MobName[0]==0` (1073–1078).
6. `SendOneMessage(&sm, sizeof(MSG_CNFCharacterLogin))` (1080); then ranking bookkeeping (1083–1092).

**Dispatch — TMSrv**: `DBServerSocket.ReadMessage` (Server.cpp:3889) → `ProcessDBMessage(Msg)` (Server.cpp:3914). Entry guards (ProcessDBMessage.cpp:43): `Type & FLAG_DB2GAME` must be set and `ID in [0, MAX_USER)`; `conn = std->ID` (54). `_MSG_DBCNFCharacterLogin` (0x417) matches case at ProcessDBMessage.cpp:635.

**Re-target + send — TMSrv** (`ProcessDBMessage.cpp:635–1081`): builds `sm` (client struct), populates state, sets `sm.Type=_MSG_CNFCharacterLogin` (787), `sm.ID = ESCENE_FIELD` (or `ESCENE_FIELD+1` if `NewbieEventServer==1`) (789–792), spawn coords, `ClientID=conn`, `Weather` (794–797), and `SendOneMessage(&sm, sizeof(MSG_CNFClientCharacterLogin))` (984). Then broadcasts the character to the world (see §7).

```
 client                TMSrv                          DBSrv
   | 0x113 MSG_CharacterLogin |                             |
   |------------------------->| 0x211 MSG_DBCharacterLogin |
   |                          |---------------------------->|
   |                          |   0x417 MSG_DBCNFCharacterLogin (2632B)  |
   |                          |<-----------------------------|
   |                          | case 635: memcpy prefix 1832B|
   |                          | re-target Type=0x114, ID=ESCENE_FIELD|
   | 0x114 MSG_CNFClientCharacterLogin (1832B) |             |
   |<-------------------------|                             |
   |                          | then follow-ons (§7):       |
   |<--- CreateMob, PKInfo, GridMob, etc -------------------|
```

Session mode transitions (CUser.h): `USER_SELCHAR`(11) → `USER_CHARWAIT`(12) set by `Exec_MSG_CharacterLogin` before forwarding to DB (_MSG_CharacterLogin.cpp:76); → `USER_PLAY`(22) set at ProcessDBMessage.cpp:906.

## 5. Validation & Guards

In execution order within `case _MSG_DBCNFCharacterLogin`:

| # | Guard | Action on fail | Source |
|---|---|---|---|
| 1 | `conn == 0` | `CrackLog(0," CNFCharLogin")`, `CloseUser(conn)`, break | ProcessDBMessage.cpp:643–650 |
| 2 | (ProcessDBMessage entry) `Type & FLAG_DB2GAME`; `ID in [0,MAX_USER)` | log & return | ProcessDBMessage.cpp:43–52 |
| 3 | Equip/Carry `sIndex` in `[1, MAX_ITEMLIST)` for damage normalization | (silently skip item) | :654–696 |
| 4 | `m->mobExtra.ClassMaster == MORTAL/ARCH/CELESTIAL/SCELESTIAL/CELESTIALCS` | equip fixups per class (no explicit else) | :706–726 |
| 5 | `m->mob.CurrentScore.Hp <= 0` | clamp `Hp = 2` | :730–731 |
| 6 | `MobCLS < 0 || MobCLS > MAX_CLASS-1` | `Log("err,login Undefined class",…)`, `CloseUser`, break | :859–866 |
| 7 | `GetEmptyMobGrid(conn,&tx,&ty) == 0` | `Log("Can't start can't get mobgrid",…)`, `CloseUser`, break | :887–896 |
| 8 | `mobExtra.ClassMaster==ARCH && MortalSlot in [0,3)` | recompute MortalLevel else set 99 | :767–771 |
| 9 | `NewbieEventServer==1` | `sm.ID = ESCENE_FIELD+1` else `ESCENE_FIELD` | :789–792 |
| 10 | `mob.Carry[KILL_MARK].sIndex == 0` | seed kill-mark item 547 | :809–817 |
| 11 | `pMob[conn].MOB.Guild` non-zero | guild-zone spawn & mantle re-dye | :872–975 |

Upstream producer guards (DBSrv): slot range (CFileDB.cpp:1036), secure-pass (1045), mobname empty (1073). Client-side dispatcher guards for the login request: `ID` range (ProcessClientMessage.cpp:42), `ServerDown>=120` (53), `ClientTick==SKIPCHECKTICK` (63).

## 6. Game Mechanics & Business Logic

- **Damage-effect normalization** (ProcessDBMessage.cpp:654–696): for every Equip (0..MAX_EQUIP) and Carry (0..MAX_CARRY-1) item whose `g_pItemList[sIndex].nPos` is 64 or 192 (weapon slots), `stEffect[*].cEffect` of `EF_DAMAGE2`(73) or `EF_DAMAGEADD`(67) is rewritten to `EF_DAMAGE`(2) — normalizes damage-type effects to a canonical one on login (ItemEffect.h:4,107,115).
- **Event-delete purge** (698–705): if `evDelete`, any `Carry[j].sIndex` in `[470,500]` is zeroed (removes event items).
- **Equip-effect fixups by class master** (706–726): for `MORTAL`/`ARCH`, `Equip[0].stEffect[1] = {98,0}` and `stEffect[2] = {106, (uchar)Equip[0].sIndex}`; for `CELESTIAL/SCELESTIAL/CELESTIALCS` the same but `stEffect[1].cValue = 3`.
- **Stats recomputation on login**: `BASE_GetHpMp` (773, Basedef.cpp:1174), `GetCurrentScore` (775, 911), `GetGuild` (777, GetFunc.cpp:1862), `BASE_GetBonusSkillPoint` (779, Basedef.cpp:810), `BASE_GetBonusScorePoint` (780, Basedef.cpp:858).
- **MaxCarry** (751–757): base 30; +15 for each of `Carry[60]`/`Carry[61]` equal to item 3467.
- **Spawn-point selection** (848–899): starts from saved `SPX/SPY`; derives `CityID = (Merchant & 0xC0) >> 6`; city spawn `g_pGuildZone[CityID].CitySpawnX/Y + rand()%15`; overridden by matching `MobGuild == g_pGuildZone[n].ChargeGuild` → `GuildSpawnX/Y`; newbie mortal (`Level < FREEEXP(=35, Server.cpp:43)`, ClassMaster==MORTAL) forced to `(2112,2042)`; final placement via `GetEmptyMobGrid` (GetFunc.cpp:1901).
- **Kill-mark item 547** (809–817): if empty, `Carry[63]` (KILL_MARK=63, Basedef.h:166) seeded with `EF_CURKILL`(75), `EF_LTOTKILL`(76), `EF_HTOTKILL`(77) effects.
- **Guild mantle re-dye** (915–975): when `MOB.Clan != GuildInfo[usGuild].Clan`, `Equip[15]` (mantle) is remapped by guild clan (7=blue/8=red) and class-master tier (celestial → 3197/3198, else 543–549/3191–3196 bands).
- **Exp segment** (1015–1054): for `curlvl < max_level-1`, splits `[curexp, nextexp]` into 4 segments (using `g_pNextLevel` for MORTAL/ARCH at MAX_LEVEL=399, `g_pNextLevel_2` for celestial tiers at MAX_CLEVEL=199); sets `pMob[conn].Segment` = 3/2/1/0 based on current `Exp`; resets `Unk_2736=0`; bills if `Level>=FREEEXP` and `CHARSELBILL==0` (SendBilling).
- **Admin flag** (1004–1005): `Level >= 999` → `pUser[n].Admin=1` (note: uses loop var `n`, see §9).
- **Billing/free-exp logic in the login request** lives in `Exec_MSG_CharacterLogin` (_MSG_CharacterLogin.cpp:21–110) and gates whether the DB request is even made.

## 7. Side Effects

State mutated in `pMob[conn]` / `pUser[conn]` before/at send:
- `pMob[conn].MOB = m->mob` (733), `pMob[conn].extra` copied (742), `pMob[conn].Affect` copied (749), `pMob[conn].Mode = MOB_USER` (782), `MaxCarry` (751), `ProcessorCounter=1`, `QuestFlag=0`, `LastReqParty=0` (738–740), `LastTime/LastX/LastY/TargetX/TargetY` (805–807, 901–904), `GuildDisable=0` (913), `Segment` (1038–1044), `Tab/Snd` cleared (760–761).
- `pUser[conn]`: `Donate` (728), `Message/UseItemTime/PotionTime=0` (735–737), `CharShortSkill` (747), `Mode=USER_PLAY` (906), `Slot` (821), `NumError=0`, `LastMove=0`, `LastAction=_MSG_Action`, last ticks=`SKIPCHECKTICK` (822–828), `RankingTarget/RankingType=0` (829–830), `CastleStatus=0` (831), `Unk9=-1` (834), `Trade`/`AutoTrade` cleared with `-1` inventories (835–843), `TradeMode=0`, `PKMode=0` (845–846), `cProgress=0`, `ReqHp/ReqMp`, `Unk_2688=0` (979–982), `Unk_2708=0` (819), `LastChat[0]=0` (820).
- Grid: `pMobGrid[PosY][PosX] = conn` (997).

**Outgoing packets sent AFTER the 0x114 send (line 984)** in the same handler (follow-ons):
- `MSG_CreateMob sm2` via `GetCreateMob` (990–992, GetFunc.cpp:947), `CreateType=2` (994), `GridMulticast(PosX,PosY,&sm2,0)` (999).
- `SendPKInfo(conn, conn)` (1001, SendFunc.cpp:1737)
- `SendGridMob(conn)` (1002, SendFunc.cpp:560)
- `MountProcess(conn,0)` (1007, Server.cpp:4178)
- `SendWarInfo(conn, g_pGuildZone[4].Clan)` (1008, SendFunc.cpp:1416)
- `_MSG_SendCastleState` via `SendClientSignalParm` if `CastleState != 0` (1010–1011)
- `ClearCrown(conn)` (1013, Server.cpp:494)
- `SendEtc(conn)` (1075, SendFunc.cpp:1195 → MSG_UpdateEtc)
- `SendScore(conn)` (1076, SendFunc.cpp:1136 → MSG_UpdateScore + SendAffect)

Logs (format strings):
- `Log("err,login Undefined class", acct, ip)` (861)
- `Log("Can't start can't get mobgrid", acct, ip)` (891)
- `Log("sta,Login char:%s exp:%llu level:%d conn:%d money:%d, store:%d", …)` (1078)
- DBSrv: `Log("err,charlogin slot illegal", …)` (1038), `Log("err,charlogin secure illegal", …)` (1047), `Log("err,charlogin mobname empty", …)` (1075).
- Kefra/RvR notices: `_SN_End_Khepra` (1058–1061), `_SN_KINGDOMWAR_DROP_` (1064–1073).

Related follow-on helper details: `GetCreateMob` embeds kill/guilty/pk points into `MobName[12..15]` (GetFunc.cpp:957–976); `SendScore` broadcasts `MSG_UpdateScore` and calls `SendAffect` (SendFunc.cpp:1136–1193); `SendGridMob` streams the surrounding mob grid (SendFunc.cpp:560).

## 8. Related Packets

| Packet | Type | Direction | Role |
|---|---|---|---|
| `_MSG_CharacterLogin` (MSG_CharacterLogin) | 0x113 | CLIENT2GAME | request login of slot; forwarded as `_MSG_DBCharacterLogin` (0x211) to DBSrv (_MSG_CharacterLogin.cpp:73–81) |
| `_MSG_DBCNFCharacterLogin` (MSG_CNFCharacterLogin) | 0x417 | DB2GAME | response w/ full char state; producer CFileDB.cpp:1054 |
| `_MSG_CNFCharacterLogin` (MSG_CNFClientCharacterLogin) | 0x114 | GAME2CLIENT | this packet, to client |
| `_MSG_CharacterLoginFail` | 0x119 | GAME2CLIENT | on DB failure (ProcessDBMessage.cpp:1084–1096) |
| `_MSG_CharacterLogout` / `_MSG_DBCharacterLogout` | 0x115 / — | CLIENT2GAME | teardown |
| `_MSG_CreateMob` (MSG_CreateMob) | — | GAME2CLIENT | follow-on, broadcast character to world |
| `_MSG_UpdateScore` / `_MSG_UpdateEtc` / `_MSG_SendAffect` / `_MSG_SendCastleState` | — | GAME2CLIENT | follow-on state sync |
| `_MSG_DBCharacterLogin` (DB request) | 0x211 | GAME2DB | producer trigger |

## 9. Discrepancies & Open Questions

1. **`pMob[n]` / `pUser[n]` bug** (ProcessDBMessage.cpp:1004–1005): after the guild loop, `n` holds `MAX_GUILDZONE` (5) when no guild matched (loop ends), so `pMob[n]`/`pUser[n]` index a non-player slot rather than `conn`. The level-999 admin check reads the wrong mob.
2. **STRUCT_MOBEXTRA = 536 is computed, not verified against a fixed constant** — depends on `time_t` being 64-bit (8 bytes) in the MSVC build; no `_USE_32BIT_TIME_T` define found. If a 32-bit `time_t` build were used, sizes would differ and the DB↔TMSrv structs would desync.
3. **`GetGuild(conn)` (777) is effectively a no-op** — GetFunc.cpp:1862–1867 reads `Equip[12]` and `Guild` into locals and returns; guild state actually comes from `GuildInfo[m->mob.Guild].Citizen` (744–745).
4. **Dead code `sm.mob.SPX = sm.mob.SPX`** (785–786) — self-assignment, no effect.
5. **`unk`/`Unk`/`Unk2` semantics mostly UNKNOWN** — only `Unk2[448..459]` is used (account name, 803). The rest are opaque zeroed buffers; client interpretation not derivable from server source.
6. **`sm.Size` not set explicitly on the TMSrv leg** — `AddMessage` overwrites it (CPSock.cpp:538); the value copied from the DB message (2632) is discarded. Safe only because `Size` is the first field and the client-visible size (1832) is what matters.
7. **`break` vs `return`** at 649/865/895: commented `// TODO: compile and check if it's break or return` — current code falls through to subsequent DB message cases on failure paths (CloseUser already called).
8. **`evDelete`, `NewbieEventServer`, `KefraLive`, `RvRBonus`, `CastleState`, `CurrentWeather`, `CHARSELBILL`** are runtime/server-config globals (Server.cpp) not constants; behavior varies by server settings.

## 10. Source References

- Basedef.h: `_MSG_CNFCharacterLogin` 1344; `MSG_CNFCharacterLogin` 1345–1367; `MSG_CNFClientCharacterLogin` 1368–1386; `_MSG_DBCNFCharacterLogin` 1111; `_MSG` macro 925–930; flags 932–941; STRUCT_ITEM 398–412; STRUCT_SCORE 414–436; STRUCT_MOB 438–481; STRUCT_MOBEXTRA 483–591; STRUCT_AFFECT 593–599; STRUCT_SELCHAR 765–778; STRUCT_ACCOUNTFILE 840–856 (size cross-check 844); pack regions 808/1212/1465/2063; constants ESCENE_FIELD 170, SKIPCHECKTICK 172, MAX_AFFECT 122, MAX_EQUIP 75, MAX_CARRY 76, MAX_CLASS 115, MAX_LEVEL 117, MAX_CLEVEL 118, MAX_GUILDZONE 148, KILL_MARK 166, MORTAL/ARCH/CELESTIAL/CELESTIALCS/SCELESTIAL 178–182, MAX_USER 56, g_pNextLevel/g_pNextLevel_2 2454–2455.
- ProcessDBMessage.cpp:39–52 (dispatch guards); 635–1081 (primary handler); 1084–1096 (fail path).
- CFileDB.cpp:1028–1094 (producer).
- _MSG_CharacterLogin.cpp:21–110 (request handler, mode transition 76).
- ProcessClientMessage.cpp:38–66 (client dispatcher guards).
- CPSock.cpp:378–467 (read/framing), 513–591 (add/obfuscation); CPSock.h:38–50.
- GetFunc.cpp:947 (GetCreateMob), 1862 (GetGuild), 1901 (GetEmptyMobGrid).
- SendFunc.cpp:560 (SendGridMob), 620/843 (GridMulticast), 1136 (SendScore), 1195 (SendEtc), 1416 (SendWarInfo), 1737 (SendPKInfo), 208 (SendClientSignalParm).
- Server.cpp:43 (FREEEXP=35), 494 (ClearCrown), 3889–3914 (DB read+dispatch), 4178 (MountProcess).
- Basedef.cpp:810 (BASE_GetBonusSkillPoint), 858 (BASE_GetBonusScorePoint), 1174 (BASE_GetHpMp), 6475 (BASE_CheckPacket, disabled).
- ItemEffect.h:4 (EF_DAMAGE=2), 67 (EF_DAMAGEADD), 73 (EF_DAMAGE2), 75–77 (EF_CURKILL/LTOTKILL/HTOTKILL).
- CUser.h:26–36 (session modes USER_SELCHAR/CHARWAIT/PLAY).
