# Component Deep Analysis Report - UseItem Handler

**Component name:** UseItem Handler
**Primary location:** `Code/TMSrv/_MSG_UseItem.cpp` (160,607 bytes, 5,726 lines)
**Project:** W2PP - decompiled C/C++ (Win32, MSVC / v143) MMORPG server emulator for W2
**Host process:** `TMSrv` (game / world server)
**Analysis date:** 2026-11-15 22:11:21
**Analysis scope:** entire project root (`E:\wyd\test-setup\w2pp`), excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis mode:** read-only static analysis. No project file was modified.

---

## 1. Executive Summary

### 1.1 Purpose

The UseItem Handler is the server-side authority for every "use an item" action in the world. It is implemented as **one free function**, `void Exec_MSG_UseItem(int conn, char *pMsg)`, declared at `Code/TMSrv/ProcessClientMessage.h:75` and defined at `Code/TMSrv/_MSG_UseItem.cpp:41`. It is registered in the client-message dispatcher at `Code/TMSrv/ProcessClientMessage.cpp:210-212` for the single packet type `_MSG_UseItem` (protocol id `115 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME`, `Code/Basedef.h:1821`).

The function is a **flat, sequential dispatch over the item's `EF_VOLATILE` attribute (`Code/ItemEffect.h:77`) followed by a second, smaller dispatch over literal item indexes**. It contains 68 top-level `#pragma region` branch sections (72 region markers, 4 of them nested), 106 distinct volatile codes, 12 item-index-keyed dispatch points, 277 bare `return` statements, 145 `sprintf` calls, 47 `rand()` calls, 227 `SendItem` calls, 148 `SendClientMessage` calls and 119 `ItemLog` calls - all inside one stack frame. It is the largest single function in the code base.

Functionally it covers: HP/MP potions, the full upgrade/refine economy (PO/PL stones, celestial rates, arch stones, earrings, eggs), level/experience items, attribute consumables, class advancement stones, mount feed/growth/catalyst/restorer, hunting and dungeon-entry scrolls, instance tickets (water dungeons, nightmares, secret room), currency bars, grade gems, Mytril stat bars, paint/removal tools, extraction tools for six equipment slots, PvP jewels, and donation packs.

### 1.2 Role in the system

The architectural report classifies this component as a **Command Handler (largest)** with **afferent coupling 1** and **efferent coupling 14** (module granularity). This analysis confirms both figures:

* **Afferent 1** - the only caller is `ProcessClientMessage` (`Code/TMSrv/ProcessClientMessage.cpp:211`). No other translation unit references `Exec_MSG_UseItem`.
* **Efferent 14** - the file includes exactly one aggregate header (`ProcessClientMessage.h`, line 18), whose transitive closure is the 14 TMSrv subsystems (Basedef, CPSock, ItemEffect, Language, CItem, Server, GetFunc, SendFunc, CMob, CUser, CNPCGene, CReadFiles, CCastleZakum, CWarTower).

At symbol granularity the efferent coupling is an order of magnitude higher: **59 distinct server functions** plus 2 socket send methods and roughly 40 global data objects (`pUser[]`, `pMob[]`, `pMobGrid[][]`, `temp[]`, `g_pItemList[]`, `g_pSancRate[][]`, `g_pSancGrade[][]`, `g_pCelestialRate[]`, `g_pSuccessRate[]`, `WaterScrollPosition[][][]`, `WaterClear1[][]`, `PartyPesa[]`, `PesaNPosStandard[][]`, `PesaMPosStandard[][]`, `PesaAPosStandard[][]`, `CartaPos[][]`, `CartaTime`, `CartaSala`, `PotionDelay`, `maxNightmare`, `CastleState`, `LOCALSERVER`, `ServerIndex`, `DBServerSocket`, `mSummon`, `mNPCGen`, `g_pNextLevel[]`, `g_pNextLevel_2[]`, `BaseSIDCHM[][]`, `CReadFiles::QuestExp/QuestCoin/QuestLevel`).

The handler is the **only** place where most item-economy invariants are enforced. It mutates the equipment array, the inventory array, the storage array, the affect array, the score block, the class-master field, the quest flags, the celestial save slots, the coin counter, the fame counter and the donation counter. It is therefore simultaneously the highest-value and highest-risk code path in TMSrv: a single missing guard in any branch is a dupe, an item-mint, a progression skip or a crash.

### 1.3 Key findings

| # | Finding | Severity |
|---|---------|----------|
| 1 | The dead-character branch indexes `MOB.Carry[m->SourPos]`, `pUser[conn].Cargo[m->SourPos]` and `MOB.Equip[m->SourPos]` **directly from unvalidated packet fields** before any `GetItemPointer` validation (`:69-76`). `SourPos` is a client-supplied `int`. Out-of-bounds read of arbitrary length inside the 128 KB receive buffer. | High |
| 2 | `BASE_GetItemAmount` / `BASE_SetItemAmount` are driven by an effect slot, not by a dedicated counter; if the item has no `EF_AMOUNT` the handler falls through to `memset(item, 0, sizeof(STRUCT_ITEM))`. A client that can alter effect slots can therefore duplicate (never decrement) or destroy stackables. | High |
| 3 | Locked-seal refine (`:315`, `:784`) and earring refine (`:784`) call `BASE_ClearItem(dest)` on failure, i.e. **destroy the target equipment**, while every other refine family merely resets sanc. Failure semantics are inconsistent across families and undocumented to the client. | High |
| 4 | `GetCurrentScore()` is recomputed in some branches only. Several branches mutate `MOB.BaseScore`/`ClassMaster` (Ideal Stone `:2978-3052`, Arch Crystals `:3309-3329`, Fury Stone `:3462-3483`) and rely on a later recompute; if any early `return` is taken after the mutation, the client keeps stale stats. | High |
| 5 | Vol 191 quest items compute `thisQuest = item->sIndex - 4117` and then index `CReadFiles::QuestLevel[5][4]`, `QuestExp[5][2]`, `QuestCoin[5]` (`:2243-2246`, `:2263-2264`). Any item whose `EF_VOLATILE` is 191 but whose index is outside `4117..4121` (or below 4117, yielding a negative index) causes an **out-of-bounds read/write** of static tables. | High |
| 6 | `Egg`/`INCUBATE` handling uses `rand() & 0x80000003` with a hand-rolled negative-modulo fixup (`:891-896`, `:952-956`), and `memset(item, 0, 8)` (not `sizeof`) is used on one failure path (`:1588`). Both are correct only for the 32-bit MSVC layout. | Medium |
| 7 | `DoTeleport()` silently does nothing when `GetEmptyMobGrid()` fails (`Server.cpp:8217-8219`), but every caller in this handler still consumes the scroll. On a congested destination the player loses the item without moving. | Medium |
| 8 | Party/instance entry uses **global, non-reentrant** state (`WaterClear1[3][10]`, `PartyPesa[3]`, `CartaTime`, `CartaSala`, `CartaPos`) rather than per-instance objects. Two different parties can interleave and corrupt each other's timers. | Medium |
| 9 | `GetUserInArea()` returns a **count**, and the code compares `UserArea >= 1` then prints `UserArea - 1` (`:1725-1732`). The message reports one fewer player than actually blocks the entry, and a single occupant always blocks. | Medium |
| 10 | Vol 230 / Elixir and Courage Medicine conflict: the guard `Value != 500` / `Value != 2000` compares against a field the handler then writes as `1` (`:3582`, `:3590`, `:3616`, `:3624`). The "already active" detection is therefore permanently true/false depending on the item, not on the buff. | Medium |
| 11 | `LOCALSERVER` (set when `DBServerAddress == "192.168.0.50"`, `Server.cpp:3587-3588`) changes gameplay: mount essence grants **10** levels instead of 1 (`:1603-1604`). A production shard pointed at that address silently runs at 10x mount growth. | Medium |
| 12 | `sprintf` is used 145 times with a global, non-reentrant `char temp[4096]` buffer, including `sprintf(temp, g_pMessageStringTable[...], pMob[...].Name)` patterns where the format string is a runtime table lookup. | Medium |
| 13 | No unknown-volatile default branch: an item whose `EF_VOLATILE` is not dispatched (or is 0) is **silently ignored and not consumed**, giving no client feedback. Failure modes are indistinguishable from packet loss. | Low |
| 14 | `m->GridX`/`m->GridY` are validated against `MAX_GRIDX/MAX_GRIDY` (`:81`) and then **never used**; `m->WarpID` is only read by the hunting-scroll branch. Dead validation. | Low |
| 15 | No test project, no test file, no assertion and no CI configuration exists anywhere in the repository for this or any other component. | High |

---

## 2. Data Flow Analysis

### 2.1 Primary flow (one use action)

```
 1. Client sends _MSG_UseItem (MSG_UseItem: SourType, SourPos, DestType, DestPos, GridX, GridY, WarpID)
 2. CPSock::ReadMessage validates HEADER <= Size <= MAX_MESSAGE_SIZE (8192)          Code/CPSock.cpp:386-410
 3. ProcessClientMessage validates std->ID in [0, MAX_USER) and ServerDown < 120    ProcessClientMessage.cpp:42-54
 4. switch (std->Type) case _MSG_UseItem -> Exec_MSG_UseItem(conn, pMsg)            ProcessClientMessage.cpp:210-212
 5. Handler casts pMsg to MSG_UseItem without checking m->Size                     _MSG_UseItem.cpp:43
 6. Gate: pUser[conn].Mode == USER_PLAY                                            :45
 7. Gate: no open trade (Trade.OpponentID -> RemoveTrade both; TradeMode -> RemoveTrade)   :51-62
 8. Gate: MOB.CurrentScore.Hp != 0 (dead -> resend slot + AddCrackError + return)   :64-79
 9. Gate: GridX < 4096 && GridY < 4096 (log-only, values unused)                    :81-85
10. Resolve source: GetItemPointer(&pMob[conn].MOB, pUser[conn].Cargo, SourType, SourPos)  :86
       -> NULL => ItemLog "err,use item fail source ..." and return                 :88-93
11. Read dispatch keys: Vol = BASE_GetItemAbility(item, EF_VOLATILE)                :94
                        amount = BASE_GetItemAmount(item)                           :95
12. FIRST-LEVEL DISPATCH on Vol (106 codes, 68 regions)                             :97-5724
13. SECOND-LEVEL DISPATCH on item->sIndex for codes not consumed above              :3071-5724
14. Per-branch effect application (score / items / affects / teleport / summon / DB)
15. Per-branch resynchronisation: SendItem / SendScore / SendEtc / SendCarry /
    SendClientMessage / SendEmotion / SendSetHpMp / SendClientSignalParm /
    GridMulticast / cSock.AddMessage
16. Per-branch consumption: amount > 1 ? BASE_SetItemAmount(item, amount-1)
                                      : memset(item, 0, sizeof(STRUCT_ITEM))
17. Per-branch audit: ItemLog("useitem,<verb> ...", AccountName, IP)
```

There is **no single response message and no transactional boundary**. Each branch emits its own sequence of outbound messages, and the item is consumed at a different point of the branch (usually last, sometimes never). Steps 15-17 are therefore repeated 68 times with slightly different orderings.

### 2.2 Dispatch topology

```
Vol (EF_VOLATILE)
 |
 +-- 1  ---------------------------- HP/MP potion           :98
 +-- 4,5 --------------------------- PO / PL refine engine   :150
 |     |-- carry target + locked-seal index list -> refine  :222
 |     |-- MBTYPE==3 (celestial) -----------------> +10..15  :382
 |     |-- sIndex 1752..1759 (arch stones) -------> trans.   :512
 |     |-- sanc 9..REF_14 + slot 8 + nPos 256 ----> earring  :714
 |     +-- default equip target ------------------> PO/PL    :799
 +-- 6 ----------------------------- Pilula Orc              :978
 +-- 7 ----------------------------- Fairy dust (level)      :999
 +-- 8 ----------------------------- Crescent eye (exp)      :1045
 +-- 9 ----------------------------- Adamantite/Beryl        :1080
 +-- 10,52,53,55,56,57,200,201,202 - Kappa-affect potions    :1198
 +-- 11 ---------------------------- Recall scroll           :1301
 +-- 12 ---------------------------- Stellar gem (save)      :1317
 +-- 13 ---------------------------- Portal scroll           :1378
 +-- 14 ---------------------------- DB record ticket        :1411
 +-- 15 ---------------------------- Pet ration              :1434
 +-- 16 ---------------------------- Pet essence             :1520
 +-- 17 ---------------------------- Guild board             :1644
 +-- 19 ---------------------------- Fireworks               :1666
 +-- 21..30 ------------------------ Water scroll M          :1682
 +-- 31..38 ------------------------ Sephira skill books     :1787
 +-- 41..49 ------------------------ Kingdom contracts       :1828
 +-- 58 ---------------------------- Vigor (1h)              :2131
 +-- 63 ---------------------------- Roast chicken           :2200
 +-- 64..66 ------------------------ Divine 7/15/30 days     :2085
 +-- 93 ---------------------------- Mount restorer          :4942
 +-- 94 ---------------------------- Mount catalyst          :4855
 +-- 131..140 ---------------------- Water scroll N          :1876
 +-- 161..170 ---------------------- Water scroll A          :1981
 +-- 173 / 174 / 175 --------------- Nightmare N/M/A tickets :2440/2535/2636
 +-- 180..183 ---------------------- Grade gems (4 colours)  :3768/3846/3924/4002
 +-- 184 --------------------------- War pack (donation)     :5708
 +-- 185 --------------------------- Silver bars             :4117
 +-- 186 --------------------------- Paint / remover         :3646
 +-- 187 --------------------------- Arch crystals           :3252
 +-- 188 --------------------------- Territory entry         :4080
 +-- 190 --------------------------- Class bonus reroll      :4800
 +-- 191 --------------------------- Quest EXP items         :2235
 +-- 194 --------------------------- Molar do Gargula        :2385
 +-- 195 --------------------------- Hunting scroll          :2867
 +-- 198 --------------------------- XP chest                :2165
 +-- 203 --------------------------- Forgiveness scroll      :3743
 +-- 206 --------------------------- Soul seal               :5629
 +-- 211 --------------------------- Ideal stone             :2889
 +-- 212 --------------------------- Nightmare writ          :3178
 +-- 230 (sIndex 647/3378) --------- Elixir of courage      :3612
 +-- 235..238 ---------------------- Mytril bars (4 stats)   :4262/4392/4522/4669
 +-- 242 --------------------------- PvP jewels              :4165
 +-- 243 --------------------------- Storage-recovery jewel  :4228
 |
 +-- (fall-through) item->sIndex dispatch
       +-- 4148 -- Pedra Misteriosa (god2 <-> god3 swap)     :3071
       +-- 4146 -- Selo do Guerreiro (fame + cloak)          :3212
       +-- 3020 -- Pedra da Furia (3 modes)                  :3348
       +-- 4046, 646 -- Courage medicine                     :3578
       +-- 3467 -- Bolsa do Andarilho (+15 carry)            :5664
       +-- 3021..3026 -- Extraction tools (helm/chest/legs/
                        gloves/boots/weapon)                 :5028..5531
       +-- (no match) -> function ends, item untouched
```

### 2.3 Representative branch flows

**Refine a +8 weapon with a PL stone (`Vol == 5`, target `DestType == EQUIP`, `DestPos == 6`)**

```
1. Resolve dest via GetItemPointer(EQUIP, 6)                              :152
2. Reject if dest->EF_VOLATILE != 0 (consumable)                          :176-183
3. Reject if dest->EF_NOSANC != 0                                         :185-192
4. sanc = BASE_GetItemSanc(dest)                                          :194
5. Reject if MBTYPE==5 seal and sanc>=9                                   :224
6. Reject if sanc >= 6 && Vol == 4 (PO cap)                               :374
7. MBTYPE==3? -> celestial path                                           :382
8. sIndex 1752..1759? -> arch-stone path                                  :512
9. sanc 9..22 && slot 8 && nPos 256? -> earring path                      :714
10. Reject if sanc == 9 || sanc >= 12 || sanc < 0 || (sanc>=9 && idx 769) :799
11. sancSuccess = BASE_GetItemSancSuccess(dest)                           :806
12. Ensure sanc effect slot exists (write EF_SANC=43 into first free slot) :811-837
13. Reject egg still incubating (INCUDELAY > 0 && level < 999)            :839-844
14. rate = BASE_GetSuccessRate(dest, Vol-4)                               :847
15. roll rand()%100; success if _rand <= rate && rate != 0                :848-852
16. success: sanc 10 -> +11 with preserved gem; else grade-scaled +1,
    capped at 6 (PO) or 9 (PL)                                            :854-878
17. recompute GetCurrentScore + SendScore + log + msg                     :879-885
18. consume stone, SendItem(dest), SendEmotion(14,3)                      :916-923
19. failure: consume stone, sancSuccess may increment, write back sanc    :926-962
```

**Enter the M water dungeon room 3 (`Vol == 23`)**

```
1. Target position must be inside WaterScrollPosition[1][2] +-8 or +-12   :1689-1704
2. Player must be party leader (Leader == 0 or -1)                        :1713-1718
3. GetUserInArea(room +-8) must return 0                                  :1723-1733
4. WaterClear1[1][2] = 30 (rooms 0..7) or 15 (rooms 8..9)                 :1735
5. DoTeleport(self) + StartTime signal (WaterClear1*2)                    :1737-1738
6. For each party member: DoTeleport + StartTime                          :1740-1749
7. Rooms 0..7: GenerateMob(WATER_M_INITIAL+2) twice + RebuildGenerator()  :1751-1756
8. Consume scroll and log                                                 :1774-1781
```

**Apply a Mytril damage bar to a +10 arch weapon**

```
1. Dest must be EQUIP, slot not 0/1, not 8..14                            :4273-4281
2. sanc >= 10 and EF_MOBTYPE == 1 (arch)                                  :4286-4290
3. current EF_DAMAGE (nosanc) must be in [12,42) for slots 1..5/8.. or [45,81) for 6/7  :4294-4307
4. roll rand()%115 normalised to 0..100; success if <= 18                 :4309-4314
5. success: +6 (or +9 on weapon slots) to the EF_DAMAGE/EF_DAMAGE2 slot, capped at 42/81  :4316-4328
6. recompute score, log, consume bar, SendItem, SendEmotion(14,3)         :4330-4343
7. failure: 1-in-7 chance to subtract the same amount ("fail--"), else no stat change     :4346-4386
```

### 2.4 Data objects touched (write set)

| Object | Mutated in | Nature of mutation |
|---|---|---|
| `pMob[conn].MOB.Equip[0..15]` | refine (753/769/1726/eggs), Ideal Stone, Seal of Warrior, Mystic Stone, extraction tools, Molar, grade gems, Mytril, paint, class reroll | full struct overwrite or per-effect overwrite |
| `pMob[conn].MOB.Carry[0..MaxCarry-1]` | consumption, bag, class reroll, Fury Stone (element removal) | clear / decrement / copy |
| `pUser[conn].Cargo[0..127]` | consumption when `SourType == ITEM_PLACE_CARGO`, class reroll (dest < 60) | clear / decrement |
| `pMob[conn].MOB.CurrentScore` / `.BaseScore` | every score-affecting branch | via `GetCurrentScore()` or direct field writes |
| `pMob[conn].MOB.Exp` | fairy dust, crescent eye, quest items, arch crystals, Fury Stone | add / overwrite / subtract |
| `pMob[conn].MOB.Coin` | quest items (capped 2e9), silver bars (capped 2e9) | add |
| `pMob[conn].MOB.LearnedSkill` | Sephira books, Ideal Stone | bit set / overwrite |
| `pMob[conn].MOB.SkillBonus` / `SpecialBonus` / `ScoreBonus` | Pilula Orc, Ideal Stone | add / overwrite |
| `pMob[conn].MOB.SPX/SPY` | Stellar gem, Mystic Stone (swap) | overwrite |
| `pMob[conn].MOB.Class`, `.Clan`, `.Guild` reads | Ideal Stone, Mystic Stone | read + derive |
| `pMob[conn].Affect[0..31]` | Kappa, Divine, Vigor, XP chest, chicken, courage/elixir, PvP jewels, storage jewel, Mystic Stone (mass clear) | slot allocate / refresh / clear |
| `pMob[conn].extra.ClassMaster` | Ideal Stone, Mystic Stone | 1↔3↔4↔5 transitions |
| `pMob[conn].extra.SaveCelestial[0..1]` | Ideal Stone, Mystic Stone | full snapshot / restore |
| `pMob[conn].extra.Fame` | Seal of Warrior, Fury Stone | add / subtract, capped at 2e9 before add |
| `pMob[conn].extra.NT`, `.LastNT` | Nightmare writ, Nightmare A | +13 / timestamp |
| `pMob[conn].extra.DivineEnd` | Divine 7/15/30 | timestamp + 8/16/31 days |
| `pMob[conn].extra.QuestInfo.*` | Pilula Orc, Fairy dust (Lv90 guard), Molar, Fury Stone, Arch crystals, Nightmare | flags / counters |
| `pMob[conn].extra.DayLog` | Quest EXP items | exp + year-day reset |
| `pUser[conn].Donate` | War pack | add |
| `pUser[conn].PotionTime`, `.UseItemTime`, `.Unk_2688`, `.ReqHp`, `.ReqMp` | potions, Seal of Warrior | timestamps / deferred HP-MP |
| `pMob[conn].MaxCarry` | Traveler's bag | 30 / 45 / 60 |
| `WaterClear1[3][10]`, `PartyPesa[3]`, `CartaTime`, `CartaSala` | water scrolls, nightmares, duel card | global instance state |
| `temp[4096]` | 145 `sprintf` sites | shared scratch buffer |

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| A1 | Access control | Only a connection in `USER_PLAY` mode may use items; otherwise resend HP/mode | `_MSG_UseItem.cpp:45-49` |
| A2 | Concurrency guard | An open player-to-player trade cancels itself before any item use; a trade window alone also cancels | `:51-62` |
| A3 | State validation | A character with `CurrentScore.Hp == 0` cannot use items; the source slot is resent and a crack counter is incremented | `:64-79` |
| A4 | Input validation | `GridX < 4096` and `GridY < 4096`, otherwise only a log line (values are never used afterwards) | `:81-85` |
| A5 | Referential validation | Source must resolve through `GetItemPointer` (type in {0,1,2}, position in range, `sIndex` in `[0,6500)`) | `:86-93` |
| A6 | Dispatch precedence | `EF_VOLATILE` is evaluated before any literal `sIndex` test; `sIndex` branches are only reachable if no volatile branch matched | `:94`, `:3071+` |
| A7 | Fail-closed default | An item with an unhandled volatile (including 0) is neither consumed nor acknowledged | `:5724-5726` |
| B1 | Consumption model | Stacks decrement by one when `EF_AMOUNT > 1`, otherwise the whole 8-byte item is zeroed | 68 sites, e.g. `:139-143` |
| B2 | Atomicity | No transaction: item, world and client state are updated in branch-specific order; no rollback exists | whole function |
| B3 | In-place mutation | All mutations are applied through the pointer returned by `GetItemPointer`; there is no working copy and no re-validation after mutation | `:86`, `:152` |
| C1 | Rate limit | HP/MP potions are throttled by `PotionDelay` (default 500 ms, configurable) | `:102-112` |
| C2 | Deferred HP | HP potions add to `pUser.ReqHp` (capped at MaxHp), not to `CurrentScore.Hp`; applied later by `ApplyHp` in up to 2000-point steps | `:114-123`, `Server.cpp:8591-8623` |
| C3 | Deferred MP | MP potions add to `pUser.ReqMp` (capped at MaxMp), applied later by `ApplyMp` | `:124-135`, `Server.cpp:8625-8657` |
| C4 | Dead-check asymmetry | The HP component is applied only when `Hp > 0 && CurrentScore.Hp > 0`; the MP component has no such guard | `:114`, `:126` |
| D1 | Target restriction | The refine engine accepts only equipment destinations; inventory targets are rejected except for the locked-seal index list | `:166-333`, `:335-343` |
| D2 | Target eligibility | Target must have no `EF_VOLATILE` and no `EF_NOSANC`, and must already expose a `EF_SANC`/116..125 effect slot (created if absent) | `:176-192`, `:353-369` |
| D3 | Locked-seal refine | Seals 1234-1237, 1369-1372, 1519-1522, 1669-1672, 1901-1910, 1714 refine to max +9 using `g_pCelestialRate`; failure destroys the item | `:197-332` |
| D4 | Celestial refine | `EF_MOBTYPE == 3` equipment refines +10..+15 using `g_pCelestialRate[10..14]`; failure resets sanc to 0 | `:382-509` |
| D5 | Arch stone conversion | Stones 1752-1759 convert into 1744-1751 with per-stone probability bands and a hard success cut-off; failure resets sanc | `:512-710` |
| D6 | Earring refine | `sanc in [9,22]`, `DestPos == 8`, `nPos == 256`, `Vol == 5`: flat 15 % chance to add one star; failure destroys the earring | `:714-796` |
| D7 | Refine rate formula | `g_pSancRate[Vol-4][sanc+1] + (sancValue/10)*g_pSuccessRate[sanc+1]`; sanc 10 is hard-coded 15 %; item 4141 forces 100 % | `:847`, `Basedef.cpp:2202-2225` |
| D8 | Refine ceiling | PO (`Vol 4`) caps at +6, PL (`Vol 5`) caps at +9; success at sanc 10 jumps to star 11 preserving gem index | `:854-878`, `:391-396` |
| D9 | Failure counter | A failed refine has a 75 % chance to increment the "failed attempts" sub-counter (PL always, PO only when sanc <= 5), which raises future odds via `g_pSuccessRate` | `:940-960` |
| D10 | Egg incubation | Eggs 2300-2329 are refined like gear; on success a hatch timer (6-9 h) and fed/kill counters are written, and if `sanc >= EF_INCUBATE` the item advances +30 (baby → adult mount) | `:887-915` |
| D11 | Guaranteed stone | Item 4141 always resolves to `OriLacto == 2`, for which `BASE_GetSuccessRate` returns 100 | `:847`, `Basedef.cpp:2215-2224` |
| D12 | Feedback contract | Success emits `SendEmotion(conn, 14, 3)`; failure emits emotion 15 (has face) or 20 (no face); messages come from `g_pMessageStringTable` | `:277-324`, `:955-970` |
| D13 | Audit | Every refine outcome writes an `ItemLog` record containing item index, sanc before, and the three stone effects | `:285`, `:305`, `:464`, `:486`, etc. |
| E1 | Pilula Orc | One-shot quest item: `SkillBonus += 9`, sets `QuestInfo.Mortal.PilulaOrc = 1`; if already set the item is **not** consumed | `:978-996` |
| E2 | Fairy dust | 50 % chance to jump to the next level threshold (`Exp = g_pNextLevel[Level+1]`), level cap depends on class master (399 / 199) | `:999-1042` |
| E3 | Crescent eye | Mortal-only, `Exp += 2500`; on level-up runs `SetCircletSubGod` and `DoItemLevel` | `:1045-1077` |
| E4 | Legendary upgrade | Stones 575-578 promote matching unique armour of grade 1..3 to `g_pItemList[].Extra` on a 51 % roll | `:1080-1195` |
| E5 | Kappa potions | Nine indexes map to affect `Type = 4` with value 1/2/3 and duration 80, 225, 450, 450 or 9000 ticks | `:1198-1298` |
| E6 | Recall scroll | `DoRecall()` to city spawn, guild zone or rookie area | `:1301-1314` |
| E7 | Stellar gem | Saves `TargetX/TargetY` into `SPX/SPY`; blocked inside war arenas, villages, and level-gated map attributes | `:1317-1375` |
| E8 | Portal scroll | Teleports to saved `SPX/SPY`; blocked when the saved point is in the sanctuary maps (9,1) or (8,2) | `:1378-1408` |
| E9 | DB record ticket | Emits `_MSG_MessageDBRecord` to DBSrv with `TCK` line and consumes itself | `:1411-1431` |
| E10 | Guild board / fireworks | Guild board opens `_MSG_GuildBoard` **without consuming**; fireworks emit emotion 100 | `:1644-1679` |
| E11 | Vigor / XP chest / chicken | Affect 35 (1 h), affect 39 (+2 h, cap 324000), affect 30 (`Level 1000`, 4 h) | `:2131-2232` |
| E12 | Courage items | Affect 30 with `Level = 500` (+500 damage, 100 ticks) or `Level = 2000` (1 h / 30 d) | `:3578-3643` |
| E13 | Forgiveness scroll | `SetPKPoint(conn, 150)` and broadcasts the new appearance | `:3743-3765` |
| E14 | Soul seal (empty) | `SCELESTIAL` only: logout, then `_MSG_DBPutInCapsule` with the character slot | `:5629-5661` |
| E15 | War pack | `pUser.Donate += EF_DONATE` with no upper bound | `:5708-5724` |
| E16 | Mystic stone | `CELESTIALCS ↔ SCELESTIAL` swap: snapshot/restore score, skills, soul, short-skills, position; clears most affects; requires being inside a city | `:3071-3175` |
| E17 | Seal of the Warrior | `Fame += 10` (cap applied before the add); below-354 mortals also receive cloak 3191/3192/3193 | `:3212-3249` |
| E18 | Traveler's bag | Adds +15 carry capacity, writing a 30-day dated item into slot 60 then 61; refuses when both are occupied (item kept) | `:5664-5705` |
| F1 | Water scroll M | Rooms 0..9 of the "M" dungeon: position window, party-leader-only, single-party lock, 30/15 unit clear timer, 2 mobs per room | `:1682-1784` |
| F2 | Water scroll N | Same as F1 for the "N" dungeon (rooms 0..9), no `RebuildGenerator()` | `:1876-1978` |
| F3 | Water scroll A | Same as F1 for the "A" dungeon, no `RebuildGenerator()` | `:1981-2082` |
| F4 | Nightmare N ticket | Region (19,15) only, party-leader-only, blocked minutes 4-19/24-39/44-59, celestial>=180 excluded, `PartyPesa[0] < maxNightmare` | `:2440-2532` |
| F5 | Nightmare M ticket | Region (16,16), windows 5-8/25-28/45-48, honours saved (8,2) position | `:2535-2633` |
| F6 | Nightmare A ticket | Region (19,13), windows 10-13/30-33/50-53, consumes one `NT` per celestial participant | `:2636-2756` |
| F7 | Nightmare writ | 20-hour cooldown (`LastNT + 72000`), then `NT += 13` | `:3178-3209` |
| F8 | Duel card | Tor-Alter coordinate gate, party-leader-only, area must be empty, 60-unit timer, wipes mobs in the arena, spawns one of three secret rooms | `:2759-2864` |
| F9 | Hunting scroll | Indexes 3432-3437 select one of six 10-entry coordinate tables; `WarpID` must be 1..10 | `:2867-2886` |
| F10 | Territory entry | Index 4111/4112 = mortal/arch territory; 4113+ = celestial territory; teleports with ±3 jitter | `:4080-4114` |
| G1 | Pet ration | `DestPos == 14` only, mount index 2330-2389, ration family must match mount family, `+5000` HP (cap 30000), `+2` feed (cap 100) | `:1434-1517` |
| G2 | Pet essence | `DestPos == 14`, HP > 0, essence family must match, sets HP 20000 / feed 100, then adds 1 level (10 on `LOCALSERVER`) | `:1520-1641` |
| G3 | Pet evolution | Baby→adult at level 25 (2330), 50 (2331), 100 (2332-2359): index +30, life = `rand()%20 + level`, level reset | `:1610-1622` |
| G4 | Mount catalyst | Index 3344-3350 selects a mount family; matching mount advances +30 (adult) and rolls its life value | `:4855-4939` |
| G5 | Mount restorer | Index 3351-3357; requires mount life in (5,50) and HP > 0; adds 1-2 life points | `:4942-5025` |
| H1 | Sephira books | Vol 31..38 maps to learned-skill bit `1 << (Vol-7)`; refuses duplicates without consuming | `:1787-1825` |
| H2 | Kingdom contracts | Vol 41..49 maps to summon id `Vol-41+28`; clan 7 cannot use 46-49, clan 8 cannot use 41-45; requires `CastleState != 0` | `:1828-1873` |
| H3 | Quest EXP items | Vol 191: class gate (MORTAL/ARCH), per-quest level window, coin + exp, daily-log accounting, 10 % party share | `:2235-2382` |
| H4 | Molar do Gargula | Mortal only, level 199..253, one-shot flag, raises all armour pieces below +6 to +6 | `:2385-2437` |
| H5 | Ideal stone (arch) | Level >= 355, empty helmet, arch quest >= 99; converts ARCH→CELESTIAL, rebuilds stats, sets helmet 3500/3501/3502 and clan cloak | `:2897-3072` |
| H6 | Ideal stone (celestial) | Level >= 120 and helmet 1760-1763; converts CELESTIAL→CELESTIALCS and snapshots into `SaveCelestial[1]` | `:2912-2976` |
| H7 | Arch crystals | Sequential quests 1..4 (index 4106+), each granting fixed stat increments and subtracting 100,000,000 exp | `:3252-3345` |
| H8 | Fury stone | Three modes: celestial>=199 arcana quest (4 element items), celestial reset (< 3), mortal level-90 quest | `:3348-3575` |
| H9 | Divine 7/15/30 | Affect 34 with `Time = 2e9` and `DivineEnd += 8/16/31 days`; refuses if a type-34 affect already exists | `:2085-2128` |
| I1 | Extraction tools | Six tools (3021-3026) copy the tool's effect slots 1-2 onto the worn piece; requires `sanc == 12` and matching item level/type | `:5027-5627` |
| I2 | Paint / remover | Vol 186: writes effect `116+color` into the first free effect slot; index 3417 strips it back to `EF_SANC` | `:3646-3741` |
| I3 | Grade gems | 180-183 shift the item's `Grade` field by -3..+3 and rewrite sanc with gem index 0..3 | `:3768-4078` |
| I4 | Mytril bars | 235-238 add +6/+9 (damage), +2/+4 (magic), +5/+10 (AC), +10 (crit) with per-type caps and an 18 %/50 % roll | `:4262-4798` |
| I5 | Class reroll | Vol 190 re-rolls the two bonus effects of a mortal/normal item via `SetItemBonus2` if `EF_ITEMLEVEL` matches the stone | `:4800-4852` |
| I6 | PvP jewels | Vol 242 sets/ORs affect type 8 with bit `1 << jewel` and 1 h duration | `:4165-4225` |
| I7 | Storage-recovery jewel | Vol 243 / index 3203 clears affect types 1,3,5,7,10,12,20,32 | `:4228-4259` |
| I8 | Silver bars | Six bar indexes convert to 1M..1G coin, refused if the sum would exceed 2,000,000,000 | `:4117-4162` |

### 3.2 Detailed breakdown of the business rules

---

#### A1 - Session-mode gate

**Overview**: The handler refuses to process any use request unless the connection slot is in the in-game state.

**Detailed description**: `pUser[conn].Mode` is a session state machine (`Code/TMSrv/CUser.h:36`, `USER_PLAY = 22`). A connection passes through login, character selection and a database handshake before reaching `USER_PLAY`; during those phases `pMob[conn]` is either stale or not yet populated. The rule therefore protects every downstream branch from operating on a half-initialised character.

The failure action is not an error message but `SendHpMode(conn)` (`SendFunc.cpp:1622`), which pushes `_MSG_SetHpMode` carrying the current HP and the current mode. This is effectively a client resynchronisation hint: the client is told "you are not in play state, here is the authoritative mode". It is used twice in the handler - here and in the dead-character branch - which suggests it is the generic "you cannot act" signal of this protocol.

Note that `SendHpMode` itself re-checks `Mode == USER_PLAY` and the socket before sending (`SendFunc.cpp:1627-1631`), so in the failing case the message is silently dropped and the client receives nothing. The gate is safe but gives no diagnostic feedback.

**Rule workflow**:
```
if (pUser[conn].Mode != USER_PLAY) { SendHpMode(conn); return; }
```

---

#### A2 - Open-trade cancellation

**Overview**: Item use is mutually exclusive with trading; an open or half-open trade is torn down instead of being ignored.

**Detailed description**: Two distinct trade states are checked. `pUser[conn].Trade.OpponentID` identifies an active bidirectional trade window; `pUser[conn].TradeMode` is a broader flag that also covers auto-trade stalls. In both cases the handler calls `RemoveTrade()` - on the opponent first, then on the caller - and returns without touching the item.

This is a defensive invariant rather than a player-facing rule: the client is expected to close the trade window before using an item, but the server enforces it. `RemoveTrade` (`Server.cpp:7319`) zeroes the trade struct, resets all `InvenPos` to -1, clears the auto-trade buffer and sends signal 900 to the client.

The important consequence is that a use request received during a trade **consumes no item and applies no effect**, and the trade itself is destroyed. A malicious client cannot use this to duplicate (the trade is cancelled, not committed), but a race between "confirm trade" and "use item" results in the trade being lost - a player-visible behaviour that is not documented anywhere.

**Rule workflow**:
```
if (Trade.OpponentID) { RemoveTrade(OpponentID); RemoveTrade(conn); return; }
if (TradeMode)        { RemoveTrade(conn); return; }
```

---

#### A3 - Dead-character gate

**Overview**: A character at zero HP cannot use items; the server resends the source slot and increments the anti-cheat counter.

**Detailed description**: `pMob[conn].MOB.CurrentScore.Hp == 0` is the canonical "dead" test throughout TMSrv. The handler responds with `SendHpMode`, `AddCrackError(conn, 1, 15)`, then an explicit resend of the source slot so the client's view of that slot is refreshed.

The resend is implemented as an `if/else if/else` chain over `SourType` and - critically - it indexes the arrays **before** validating `SourPos`:

```c
if (m->SourType == ITEM_PLACE_CARRY)      SendItem(conn, m->SourType, m->SourPos, &pMob[conn].MOB.Carry[m->SourPos]);
else if (m->SourType == ITEM_PLACE_CARGO) SendItem(conn, m->SourType, m->SourPos, &pUser[conn].Cargo[m->SourPos]);
else                                      SendItem(conn, m->SourType, m->SourPos, &pMob[conn].MOB.Equip[m->SourPos]);
```

`m->SourPos` is a 32-bit signed integer taken directly from the packet, and neither `MaxCarry` nor `MAX_EQUIP`/`MAX_CARGO` is consulted. `SendItem` copies `sizeof(STRUCT_ITEM)` (8 bytes) from the computed address into the outbound message (`SendFunc.cpp:1079`), so a negative or large `SourPos` reads 8 bytes from an arbitrary offset relative to the global arrays. The same eight-line block is duplicated verbatim in `_MSG_PutoutSeal.cpp:41-48`, confirming a copy-paste origin.

`AddCrackError(conn, 1, 15)` accumulates into `pUser[conn].NumError` and only forces a logout at 2,000,000,000 (`Server.cpp:684-706`), so a single occurrence has no consequence.

**Rule workflow**:
```
if (Hp == 0) {
    SendHpMode(conn);
    AddCrackError(conn, 1, 15);
    SendItem(conn, SourType, SourPos, &array[SourPos]);   // no bounds check
    return;
}
```

---

#### A4 - Grid-coordinate sanity

**Overview**: The packet's `GridX`/`GridY` must be below `MAX_GRIDX`/`MAX_GRIDY` (4096); violations only produce a crack-log entry.

**Detailed description**: `MSG_UseItem` carries `unsigned short GridX, GridY` (`Basedef.h:1830`) which the client populates with the ground target of the use action. The handler validates both against the world grid size before doing anything else, which limits the value to 0..4095.

However, after this check the two fields are **never read again anywhere in the 5,726 lines**. The validation is therefore vestigial: it protects nothing and rejects only impossible values that an `unsigned short` can still represent (4096..65535). No error message is sent; only `CrackLog(conn, " Drop grid, out of range")` is written, which appends to the shared `temp` buffer and a log file (`Server.cpp:6671`).

The field is instead used indirectly: several branches teleport or spawn based on `pMob[conn].TargetX/TargetY` (the server's authoritative position), and `WarpID` - not `GridX`/`GridY` - selects hunting-scroll destinations. This is a case where the protocol contract and the implementation diverged.

**Rule workflow**:
```
if (GridX >= 4096 || GridY >= 4096) { CrackLog(conn, " Drop grid, out of range"); return; }
```

---

#### A5 - Source item resolution

**Overview**: The source item must resolve to a real slot in the caller's own equipment, inventory or storage; failures are audited and abort.

**Detailed description**: `GetItemPointer` (`Basedef.cpp:2340-2372`) is the only bounds-checking primitive in this path. It maps `type` 0/1/2 to `MOB.Equip`, `MOB.Carry` and `pUser.Cargo`, rejects any other type, rejects `pos` outside `MAX_EQUIP` (16), `MAX_CARRY` (64) or `MAX_CARGO` (128) respectively, and rejects `sIndex` outside `[0, MAX_ITEMLIST)` (6500).

Because the pointers are taken into the *caller's* arrays, there is no cross-player forgery: the client can only name a slot of its own character. There is also no ownership check for storage beyond the array identity, so items stored in `Cargo` can be used directly (this is intended - potions can be consumed from storage).

On failure the handler writes `err,use item fail source %d %d` with `SourType`/`SourPos` and the pseudo-account `"-system"`, and returns. The audit trail therefore records the offending packet values, which is the closest thing to protocol forensics in this component.

**Rule workflow**:
```
item = GetItemPointer(&pMob[conn].MOB, pUser[conn].Cargo, m->SourType, m->SourPos);
if (!item) { sprintf(temp,"err,use item fail source %d %d", ...); ItemLog(temp,"-system",0); return; }
```

---

#### A6 - Volatile-first dispatch precedence

**Overview**: `EF_VOLATILE` is the primary discriminator; all `sIndex`-keyed branches sit after the volatile chain and are reachable only by items that no volatile branch claimed.

**Detailed description**: `EF_VOLATILE` (id 38, `ItemEffect.h:77`) is the item-list column that classifies an item's use behaviour ("0: none, 1: potion, 2: ?, 3: ?, 4: PO, 5: PL, 6: ?"). The handler evaluates `Vol` once (`:94`) and then runs equality tests for 59 individual codes plus 5 contiguous range tests that cover a further 47 codes - 106 volatile values in total - in a fixed textual order.

Because the tests are `if (Vol == N) { ... return; }` and every branch returns, the semantics are those of a `switch`. The ordering matters only for the *fall-through* code after the volatile chain: `sIndex == 4148` (Mystic stone), `4146` (Seal of the Warrior), `3020` (Fury stone), `4046/646` (Courage medicine), `3467` (Traveler's bag) and `3021..3026` (extraction tools) are only reachable if the item's volatile is one the chain does not handle (0, or 2/3/18/... which have no branch).

The consequence is a hidden data dependency: the item database (`../../Common/itemlist.csv`, loaded into `g_pItemList`) must assign non-conflicting volatile values to these items. If, for example, the extraction tools were assigned volatile 1, they would act as potions and the extraction code would be unreachable. Nothing in the source enforces or documents this contract, and the CSV is not committed to the repository, so the invariant cannot be verified statically.

**Rule workflow**:
```
Vol = BASE_GetItemAbility(item, EF_VOLATILE);
if (Vol == 1) {...return;} if (Vol == 4 || Vol == 5) {...return;} ... (106 codes)
if (item->sIndex == 4148) {...return;} ... // only for unhandled volatiles
```

---

#### A7 - Fail-closed default (unhandled volatile)

**Overview**: Items whose volatile code has no branch are silently ignored: no message, no consumption, no log.

**Detailed description**: The function ends at `:5726` immediately after the last `if` block, with no `else` and no default handling. An item whose `EF_VOLATILE` is 0, 2, 3, 18, 40, 50, 51, 54, 59-62, 67-92, 95-130, 141-160, 171-172, 176-179, 189, 192-193, 196-197, 199, 204-205, 207-210, 213-229, 231-234, 239-241 or 244+ reaches the end of the function and returns.

From the player's perspective the item "does nothing". From the server's perspective nothing changed at all - the item is still in the slot, no audit line is written, no emotion is shown. This is the safest possible failure mode for data integrity, but it is the worst for observability: an operator cannot distinguish "wrong volatile in the item CSV" from "client never sent the packet" or "packet lost".

The rule also means that a **partially implemented item** (e.g. an item added to the CSV with a new volatile) silently becomes inert rather than producing an error, which is why the 106-code table is effectively the authoritative implementation manifest of the item economy.

**Rule workflow**:
```
... all branches ...
}   // end of Exec_MSG_UseItem - no default branch, no log, item untouched
```

---

#### B1 - Stack consumption model

**Overview**: Consumption is `amount > 1 ? amount-1 : memset(item, 0, sizeof(STRUCT_ITEM))`, where `amount` comes from an effect slot.

**Detailed description**: `BASE_GetItemAmount` (`Basedef.cpp:2287-2302`) does not read a dedicated count field. It scans the three effect slots for `EF_AMOUNT` (id 61) and returns that slot's `cValue`, or 0 if absent. `BASE_SetItemAmount` (`:2304-2338`) either updates the existing `EF_AMOUNT` slot or **creates one in the first free effect slot**.

This design means the stack size is part of the item's effect payload, competing with real effects for three 16-bit slots. It also means an item with `amount == 0` (no `EF_AMOUNT`) is destroyed by `memset` on first use, while an item with `amount == 1` is also destroyed - the two cases are indistinguishable to the player. `amount` is captured **once** at `:95` and reused in every branch, so a branch that mutates effect slots before consuming (e.g. the refine success path writing `EF_SANC`) can invalidate the cached value.

The 8-byte `memset` is correct for this struct (`short sIndex` + 3 × `short`-union effects = 8 bytes, `Basedef.h:398-412`) but is written as `memset(item, 0, 8)` in one place (`:1588`) - a magic number that breaks silently if `STRUCT_ITEM` grows.

**Rule workflow**:
```
amount = BASE_GetItemAmount(item);          // EF_AMOUNT payload or 0
...
if (amount > 1) BASE_SetItemAmount(item, amount - 1);
else            memset(item, 0, sizeof(STRUCT_ITEM));
```

---

#### B2 - Non-transactional mutation

**Overview**: Each branch performs its world mutations and its consumption in a different order, with no rollback path.

**Detailed description**: The dominant pattern is "mutate target → notify → consume source → notify". For example the generic refine writes the new sanc, calls `GetCurrentScore`/`SendScore`, sends the client messages, then consumes the stone, then sends the destination item (`:877-923`). A crash between step 1 and step 4 leaves the target upgraded and the stone intact - a free upgrade after a restart. Conversely, the water-scroll branch consumes the scroll *after* generating mobs and teleporting the whole party (`:1735-1781`), so a failure after teleport would consume the scroll but leave the instance half-created.

No branch takes a copy of the pre-state, and no branch re-validates the item pointer after a `SendItem` call that could - in theory - trigger client-driven state changes. The world is mutated through raw pointers into global arrays (`pMob[conn].MOB.Equip`, `pUser[conn].Cargo`), so any later `return` simply leaves the partially applied state in place.

Because TMSrv is a single-threaded cooperative loop, the practical exposure is process termination (crash, operator kill, reboot) rather than interleaving. `SaveUser` is only called on logout/timer, so the mutation window is up to one save interval.

**Rule workflow**:
```
apply mutation -> SendScore/SendItem -> ItemLog -> consume source -> SendItem(source slot)
(crash anywhere in this sequence leaves the prefix applied)
```

---

#### B3 - In-place aliasing of the source item

**Overview**: All operations act on the live slot; there is no defensive copy, and no branch re-reads the item after mutating it.

**Detailed description**: `item` and `dest` are raw pointers into the global arrays. Several branches write to `dest` and then read `item` (the stone) to build log strings, and a few read `item->sIndex` *after* consuming it. The clearest example is the hunting-scroll log at `:2883`, which formats `item->sIndex` into the audit line **after** `memset(item, 0, sizeof(STRUCT_ITEM))` at `:2881` - the logged index is therefore always 0 on the last unit of a stack. The cart room log at `:2860` has the same defect.

Aliasing also creates a self-interaction hazard: if the source and destination resolve to the same slot (possible for `Vol 186`, `Vol 190` where the destination may be storage, or for the bag at slot 60/61), the branch will overwrite the very item it is about to consume, and the subsequent `amount`-based decrement then operates on the mutated struct.

**Rule workflow**:
```
item = pointer(SourType, SourPos);  dest = pointer(DestType, DestPos);
// same-slot aliasing not checked; item may be consumed before it is logged
```

---

#### C1 - Potion rate limit

**Overview**: HP/MP potions (`Vol == 1`) are limited to one per `PotionDelay` milliseconds per connection.

**Detailed description**: `PotionDelay` is a global initialised to 500 ms (`Server.cpp:340`) and overridable at runtime through the `gameconfig.txt` mechanism (`Server.cpp:1087-1112`, admin command `potiondelay` in `imple.cpp:625-628`). The check compares `GetTickCount() - pUser[conn].PotionTime` against it.

The timestamp is written **after** the delay check but **before** any effect is applied, so a rejected potion does not extend the lockout and a successful potion always resets it. On rejection the handler resends the source slot (`SendItem`) so the client can restore the icon it optimistically removed, and returns without a message.

`GetTickCount()` is a 32-bit wrapping counter; the subtraction is unsigned and wraps correctly, so a wrap does not create a permanent lockout. The counter is per connection slot, and `ProcessDBMessage.cpp:736-737` clears both `UseItemTime` and `PotionTime` on character login, which prevents a slot-reuse lockout.

**Rule workflow**:
```
if (PotionTime != 0 && GetTickCount() - PotionTime < PotionDelay) { SendItem(...); return; }
PotionTime = GetTickCount();
... apply HP/MP ...
```

---

#### C2 - Deferred HP application

**Overview**: An HP potion adds its `EF_HP` value to a *pending* HP counter, clamped to max HP, not to the current HP.

**Detailed description**: `pUser[conn].ReqHp` is the server's "HP the player should have" value, reconciled against `CurrentScore.Hp` later by `ApplyHp` (`Server.cpp:8591-8623`), which applies at most 2000 points per call and clamps to `MaxHp` and to 2,000,000,000. The potion branch adds to `ReqHp` and clamps it to `MOB.CurrentScore.MaxHp` (`:119-122`), so overheal is discarded at the moment of use rather than at application.

This indirection exists to smooth client-side HP display and to let the server batch regeneration; it also means a potion does **not** immediately change `CurrentScore.Hp`. The client is informed by `SendSetHpMp(conn)` (`:137`), which transmits the pending values (see `SendFunc.cpp:1590-1620`, which pushes `ReqHp`/`ReqMp`).

The write is guarded by `if (Hp && pMob[conn].MOB.CurrentScore.Hp > 0)`, i.e. zero-effect potions are ignored and the dead cannot heal - although the dead-character gate at A3 already prevents reaching here with 0 HP.

**Rule workflow**:
```
Hp = BASE_GetItemAbility(item, EF_HP);
if (Hp && CurrentScore.Hp > 0) { ReqHp = min(ReqHp + Hp, MaxHp); }
```

---

#### C3 - Deferred MP application

**Overview**: An MP potion (`EF_MP`) adds to `pUser.ReqMp`, clamped to max MP, then triggers the same client sync.

**Detailed description**: The MP path mirrors C2 but omits the "target alive" test: `if (Mp)` is the only guard (`:126`). Functionally this is harmless because A3 already rejects dead characters, but the asymmetry is a latent inconsistency if the gate is ever moved.

`ApplyMp` (`Server.cpp:8625-8657`) applies at most 2000 points per call, clamps to `MaxMp` and to 2,000,000,000, exactly like `ApplyHp`. The same `SendSetHpMp` covers both, and `pUser[conn].Unk_2688 = 1` (`:136`) marks the pair as "pending synchronisation"; the flag is cleared by the timer (`ProcessSecMinTimer.cpp:607`) and on login (`ProcessDBMessage.cpp:982`).

A potion may carry both `EF_HP` and `EF_MP` in different effect slots; both are applied in the same use and the item is consumed once. The order is HP first, then MP, and both clamps use the *current* max values - which are not recomputed here, so a potion used in the same tick as an equipment change uses the stale maximum.

**Rule workflow**:
```
Mp = BASE_GetItemAbility(item, EF_MP);
if (Mp) { ReqMp = min(ReqMp + Mp, MaxMp); }
Unk_2688 = 1; SendSetHpMp(conn); consume;
```

---

#### C4 - Potion consumption ordering

**Overview**: The potion is consumed only after both components have been staged and the client has been synchronised.

**Detailed description**: The branch stages HP, stages MP, sets the sync flag, sends `SendSetHpMp`, and only then decrements/destroys the item (`:136-143`). The item slot is **not** resent after consumption - the client is expected to decrement its own stack, which is the opposite of the refine branches (which resend everything).

This mixed contract is a recurring theme: some branches resend the source slot after consuming (e.g. `:108`, `:314`, `:330`), others rely on the client's optimistic update. A desynchronised client therefore shows a potion that no longer exists, and only a relog or a later `SendCarry` corrects it. The dead-character branch (A3) exists precisely to repair this class of mismatch.

**Rule workflow**:
```
apply HP -> apply MP -> Unk_2688=1 -> SendSetHpMp -> consume (no SendItem for source)
```

---

#### D1 - Refine destination restriction

**Overview**: The refine engine accepts equipment slots only; inventory destinations are rejected except for a whitelist of "locked seal" item indexes.

**Detailed description**: The destination is resolved with `GetItemPointer(..., m->DestType, m->DestPos)` (`:152`). If the client targeted the inventory (`ITEM_PLACE_CARRY`) **and** `DestPos < MaxCarry`, a separate block runs that (a) allows refinement only when `dest->sIndex` is one of 1234-1237, 1369-1372, 1519-1522, 1669-1672, 1901-1910 or 1714 (`:197-198`), and (b) otherwise emits "Only to equips" and returns. That block always returns, so carry refinement is exclusively the locked-seal path.

The equipment path is guarded by a compound condition (`:335`) which, by C precedence (`&&` before `||`), evaluates as:

```
(DestType == EQUIP && DestPos == 0)                       -> reject (face slot)
|| DestType != 0                                          -> reject (not equipment)
|| (DestPos > 11 && DestPos < 15 && dest->sIndex not in {753,769,1726} && !Egg)
                                                          -> reject (slots 12-14 whitelist)
```

So slots 1-11 are always eligible, and slots 12/13/14 only for three specific accessory indexes or for mount eggs (2300-2329). Slot 15 (cloak) is never eligible, which is consistent with the fact that cloaks are granted, not refined.

The rule encodes an economy decision: upgrade stones work on worn gear only (plus a handful of seals in the bag and the mount eggs), never on arbitrary inventory items. This prevents "refine-then-sell" laundering and removes any need for the client to send a free inventory slot.

**Rule workflow**:
```
dest = pointer(DestType, DestPos);
if (DestType == CARRY && DestPos < MaxCarry) { locked-seal only; always return; }
if (DestPos == 0 || DestType != 0 || (12<=DestPos<=14 && idx not whitelisted && !Egg)) reject;
```

---

#### D2 - Target eligibility and sanc-slot materialisation

**Overview**: The target must be a plain, refinable piece of equipment, and the sanc value must live in a dedicated effect slot which is created if missing.

**Detailed description**: Two rejections apply before any roll: `EF_VOLATILE != 0` ("the target is itself a consumable") and `EF_NOSANC != 0` ("the target is explicitly marked unrefinable", effect id 126). Both are item-list-driven flags, checked identically in the carry and equipment paths (`:176-192`, `:353-369`).

The sanc value is stored as `cValue` of an effect whose `cEffect` is `EF_SANC` (43) or one of 116-125 (`Basedef.cpp:2095-2141`). If `sanc == 0` the handler scans the three effect slots and, if none of them carries a "protected" effect (43 or 116-125), it **writes** `cEffect = 43, cValue = 0` into the first free slot, or refuses with "Cant Refine More" if all three slots hold real effects (`:238-263`, `:398-424`, `:811-837`).

That refusal is a hard inventory-of-effects limit: a three-effect item cannot receive a refine unless one of its effects is already the sanc channel. It is also why the "paint" system (I2) deliberately writes into 116+ slots - the paint and the sanc share the same channel.

The three identical copies of this block (locked seal, celestial, generic) are a maintenance hazard; the generic copy is the only one that later uses `BASE_GetItemSancSuccess`.

**Rule workflow**:
```
if (dest->EF_VOLATILE) reject; if (dest->EF_NOSANC) reject;
if (sanc == 0) place/patch an EF_SANC slot, else reject if all slots occupied;
```

---

#### D3 - Locked-seal refine (destructive)

**Overview**: Soul-bound "seal" items stored in the inventory upgrade up to +9 with the celestial rate table, and are **destroyed** on failure.

**Detailed description**: The whitelist at `:197` identifies six families of sealed equipment (indexes 1234-1237, 1369-1372, 1519-1522, 1669-1672, 1901-1910 and the single index 1714). For these, `itemtype` is forced to 5 (`:198`), which routes the branch into the `if (itemtype == 5)` block (`:222-328`) regardless of the target's real `EF_MOBTYPE`.

The rate is `g_pCelestialRate[sanc]` with a normalised `rand()%115` roll, and the ceiling is sanc 9 (`:224`). On success the sanc is written as `ref+1` with zero gem sub-level (`:280`), the character is rescored and the client receives message + emotion 14. On failure the stone is consumed, `BASE_ClearItem(dest)` zeroes the entire target (`:315`) and the client sees emotion 15 (has a face) or 20 (no face).

Destroying equipment on a failed upgrade is by far the harshest rule in the handler. Combined with the fact that these are inventory items (not worn gear), it means a failed seal refine permanently removes an item the player was not even using. There is no confirmation handshake and no way for the client to know the risk from the server side.

**Rule workflow**:
```
if (sanc >= 9) reject;
if (sanc == 0) materialise sanc slot;
roll = rand()%115 normalised; if (roll <= g_pCelestialRate[sanc]) { sanc+1; success } 
else { consume; BASE_ClearItem(dest); emotion; }
```

---

#### D4 - Celestial refine

**Overview**: Equipment with `EF_MOBTYPE == 3` uses the celestial rate table and can reach star 15; failure resets sanc to 0 but keeps the item.

**Detailed description**: The celestial block (`:382-509`) is reached from the generic equipment path after the volatile/NOSANC/PO-cap checks. Its gate is `itemtype == 3`, i.e. `BASE_GetItemAbility(dest, EF_MOBTYPE) == 3`, and a hard ceiling at `sanc >= REF_15` (27) (`:384`).

The rate lookup requires translating the *encoded* sanc into a dense 0..14 index, because `BASE_GetItemSanc` returns the sparse encoding `10, 12, 15, 18, 22, 27` for stars 10-15 (`Basedef.cpp:2123-2138`). The handler does this with a six-way `if` chain (`:433-449`) and then indexes `g_pCelestialRate[ref]`, whose values are `100,100,100,100,100,100,100,100,70,50,40,40,40,10,5` (`Basedef.cpp:85-88`). Practical consequence: +0..+7 are guaranteed, +8 is 70 %, +9 is 50 %, +10..+12 are 40 %, +13 is 10 %, +14 is 5 %.

Failure uses `BASE_SetItemSanc(dest, 0, 0)` (`:496`), not `BASE_ClearItem`, so the item survives at +0 while the stone is consumed. Success sets `ref+1`, which is a *dense* value passed to `BASE_SetItemSanc` and therefore maps back into the 230/234/238/242/246/250 encoding.

The same normalised `rand()%115` roll is used as in D3, but the comparison is `<=`, giving 101 effective outcomes (0..100) rather than 115.

**Rule workflow**:
```
if (MBTYPE != 3) skip; if (sanc >= 27) reject;
dense = decode(sparse sanc); roll = normalise(rand()%115);
if (roll <= g_pCelestialRate[dense]) { BASE_SetItemSanc(dense+1); score; consume; } 
else { consume; BASE_SetItemSanc(0); }
```

---

#### D5 - Arch stone conversion

**Overview**: Stones 1752-1759 convert into one of eight "pedra" items (1744-1751) using per-stone probability bands and a per-stone success cut-off.

**Detailed description**: Each source stone owns a band table expressed as cumulative `_rd` thresholds. For 1752 (Orc Trooper) the bands are `<56 → 1744`, `<80 → 1745`, `<90 → 1746`, `<93 → 1747`, with `RateSucess = 93`; 1759 (DemonLord) uses `<2 → 1748`, `<5 → 1749`, `<10 → 1750`, `<60 → 1751` with `RateSucess = 60` (`:512-656`). The first four stones produce the "intelligence/sagacity/mercy/abyss" family, the last four the "beauty/victory/originality/kingdom" family - so the stone tier also selects which attribute family the result belongs to.

Success (`_rd <= RateSucess`) writes `dest->sIndex = NextPedra` (`:663`) - a wholesale item substitution that keeps the effect payload - then rescoring, logging, consumption and emotion 14. Failure sets sanc to 0 and consumes the stone (`:697`), leaving the item's index untouched.

Two subtleties: the assignment happens before the item is logged, so the log's `dest->sIndex` is the *new* index while the printed sanc is the *old* one (`:668`); and because `NextPedra` is initialised to 1744 and only overwritten inside the band tests, any `_rd` value above the last band but below the success cut-off (impossible here, since the last band equals the cut-off) would produce 1744. The tables are therefore consistent only by construction.

**Rule workflow**:
```
roll = normalise(rand()%115);
switch (dest->sIndex) { case 1752: pick band; Rate = 93; ... case 1759: Rate = 60; }
if (roll <= Rate) { dest->sIndex = NextPedra; success } else { sanc = 0; fail }
```

---

#### D6 - Earring refine (destructive)

**Overview**: Earrings at star 9..14, in equipment slot 8, refined with a PL stone, have a flat 15 % chance per attempt and are destroyed on failure.

**Detailed description**: The gate is a five-part conjunction (`:714`): `sanc >= 9 && sanc <= REF_14 (22) && Vol == 5 && DestPos == 8 && g_pItemList[dest->sIndex].nPos == 256`. `nPos == 256` is the item-class marker for earrings, and slot 8 is the earring slot - so the rule is explicit about both.

The rate is a literal: `_rand = rand() % 100; if (_rand <= 15)` (`:717`, `:744`). Because the comparison is `<=`, the effective probability is 16/100, not 15/100. The dense-index conversion chain is computed but unused for the rate (`ref` is only used for `ref+1` on success), so the branch is effectively "flat chance, no scaling with current star".

Failure calls `BASE_ClearItem(dest)` (`:784`), destroying the earring exactly like D3. Success writes `ref+1` (which for sanc 22 becomes 23 → `BASE_SetItemSanc` clamps >15 to 0, i.e. a "reset to +0" edge case at the top of the range) and consumes the stone. This is the only refine branch where reaching the documented maximum can produce a *downgrade*, and it is a concrete defect (confidence: high, from `Basedef.cpp:2243-2244`).

**Rule workflow**:
```
if (sanc in [9,22] && Vol==5 && slot==8 && nPos==256) {
    if (rand()%100 <= 15) { BASE_SetItemSanc(ref+1); succeed }
    else { BASE_ClearItem(dest); fail }
}
```

---

#### D7 - Generic PO/PL refine rate

**Overview**: Below star 10, success probability is table-driven per stone type and per current star, plus a bonus from accumulated failures.

**Detailed description**: `OriLacto = Vol - 4` selects the table row: 0 = PO (`Vol 4`), 1 = PL (`Vol 5`). `BASE_GetSuccessRate(dest, OriLacto)` (`Basedef.cpp:2202-2225`) returns 15 for star 10 with a PO/PL stone, 100 for star 10 with `OriLacto == 2`, and otherwise `g_pSancRate[OriLacto][sanc+1] + succ * g_pSuccessRate[sanc+1]`, where `succ = rawSancChar / 10` (`BASE_GetItemSancSuccess`).

The base tables (`Basedef.cpp:61-66`) are `PO = {100,100,100,85,70,40,10,0,...}` and `PL = {100,100,100,100,100,80,70,70,50,60,0,0}`, indexed by `sanc+1` - so the effective per-star rates are: PO `+0 = 100, +1 = 100, +2 = 85, +3 = 70, +4 = 40, +5 = 10, +6 = 0`; PL `+0..+3 = 100, +4 = 80, +5 = 70, +6 = 70, +7 = 50, +8 = 60`. The failure bonus uses `g_pSuccessRate = {5,5,5,5,4,4,3,3,2,1,0}` (`:104`) with the same `sanc+1` offset, i.e. each stored failure adds 5 percentage points at low stars, decaying to 1 point at star 9.

Item 4141 is special-cased to `OriLacto == 2` (`:847`), for which the function returns 100 unconditionally - a "guaranteed refine" cash item. Note that `BASE_GetSuccessRate` returns `FALSE` (0) for any `OriLacto` outside {0,1,2}, so the function is only safe because the enclosing block already restricts `Vol` to 4 or 5.

**Rule workflow**:
```
OriLacto = Vol - 4;
rate = (sIndex == 4141) ? BASE_GetSuccessRate(dest, 2) : BASE_GetSuccessRate(dest, OriLacto);
if (rand()%100 <= rate && rate) success else fail;
```

---

#### D8 - Refine success progression and ceilings

**Overview**: A successful refine either advances to star 11 (from star 10, preserving the gem) or adds 1, capped at +6 for PO and +9 for PL, with grade-based scaling.

**Detailed description**: At star 10 (`sanc == REF_10`), success calls `BASE_SetItemSanc(dest, 11, BASE_GetItemGem(dest))` (`:854-855`), which encodes `234 + gem` - the item keeps its gem sub-level while gaining a star. This is the only path that can cross the 10→11 boundary, and it is what makes gems meaningful: `BASE_GetItemGem` returns `(rawSanc - 230) % 4`, i.e. which of the four sub-levels of star 10 the item had (`Basedef.cpp:2143-2175`).

For all other stars, `ItemGrade = BASE_GetItemAbility(dest, EF_ITEMLEVEL)` (effect 87) is inspected. When it is 1..5 the grade is decremented and `g_pSancGrade[OriLacto][ItemGrade]` is added to sanc; the shipped table is all-ones (`Basedef.cpp:68-72`), but `../../Common/Settings/SancRate.txt` can override each cell through `CReadFiles::ReadSancRate` keys `PO_A..PO_E` / `PL_A..PL_E` (`CReadFiles.cpp:160-268`). This is the mechanism by which "grade A/B/C/D/E" gear gets a better refine curve.

The clamps are strict: `if (sanc >= 6 && Vol == 4) sanc = 6; else if (sanc > 9) sanc = 9;` (`:865-869`). PO therefore cannot exceed +6 even via a high-grade bonus, and PL cannot exceed +9; stars 10+ are reachable only through the D4/D6/D11 paths. The related `sancsucess = 0` reset clears the accumulated failure bonus on every success.

**Rule workflow**:
```
if (sanc == 10) { BASE_SetItemSanc(11, gem); }
else { grade = EF_ITEMLEVEL; if (1..5) { grade--; sanc += g_pSancGrade[row][grade]; }
       if (Vol==4 && sanc>=6) sanc=6; else if (sanc>9) sanc=9;
       sancSuccess = 0; BASE_SetItemSanc(sanc, 0); }
```

---

#### D9 - Failure sub-counter

**Overview**: A failed refine has a 75 % chance to record the failure inside the sanc encoding, which raises the next attempt's probability.

**Detailed description**: The sanc slot has a dual purpose: `cValue` holds `sanc + 10*success` for stars 0-9 (`Basedef.cpp:2250`), so the tens digit of the raw value stores up to 20 accumulated failures while the units digit stores the star. `BASE_GetItemSancSuccess` extracts that tens digit (`:2177-2200`), and `BASE_GetSuccessRate` multiplies it by `g_pSuccessRate[sanc+1]`.

On failure the handler rolls `rand() % 4 <= 2` (75 %) and increments the counter only under two conditions: `OriLacto == 1` (PL, any star) or `sanc <= 5 && !OriLacto` (PO at low stars) (`:940-947`). PO at +6 and above therefore never accumulates a pity bonus, which is exactly where it would matter most - a deliberate or accidental asymmetry.

The write-back is skipped for star 10 (`if (sanc != REF_10) BASE_SetItemSanc(dest, sanc, sancsucess)`, `:959-960`) to avoid destroying the 230+gem encoding. All other failures rewrite the slot with the (unchanged) star and the (possibly incremented) counter, and then resend the item so the client sees the updated "success rate" indicator.

**Rule workflow**:
```
if (sanc != 10) {
   if (rand()%4 <= 2) { if (Vol==5) ++sancSuccess; else if (sanc<=5 && Vol==4) ++sancSuccess; }
   BASE_SetItemSanc(dest, sanc, sancSuccess);
}
```

---

#### D10 - Egg incubation and hatching

**Overview**: Mount eggs are refined like equipment; success stamps a hatch delay and feed counters, and when sanc reaches the egg's `EF_INCUBATE` threshold the egg becomes an adult mount (+30 index).

**Detailed description**: An egg is any target with `sIndex` in `[2300, 2330)` (`:163-164`), which also makes it one of the few indexes allowed into equipment slots 12-14 by D1. Before the roll, an egg that already carries `EF_INCUDELAY > 0` and whose owner is below level 999 is refused with "Incu Wait More" (`:839-844`) - the "level < 999" clause means high-level characters bypass the incubation wait entirely.

On success the handler writes `stEffect[2].cEffect = EF_INCUDELAY (84)` and `stEffect[2].cValue = 6..9`, where the value is produced by `rand() & 0x80000003` followed by a negative-modulo fixup (`:891-896`) - effectively `rand() % 4 + 6`. It then compares sanc against `BASE_GetBonusItemAbility(dest, EF_INCUBATE)`; when the threshold is met the item's index advances by 30 (2300-2329 → 2330-2359), `stEffect[0].sValue` becomes 20000 (mount HP), `stEffect[1]` becomes level 1 with `rand()%20 + 10` life, `stEffect[2]` becomes feed 30 / kills 1. The client is told "INCUBATED" and `MountProcess` is called to spawn the mount entity.

On failure the same `EF_INCUDELAY`/`cValue` is written with the raw `rand() % 4` (no `+6`) (`:948-957`), which still blocks the next attempt by the `INCUDELAY > 0` test - so a failed incubation imposes a wait but no progress. Eggs therefore behave as a slot machine whose "cost" is the wait timer, not the item.

**Rule workflow**:
```
if (Egg && level<999 && INCUDELAY>0) reject;
roll -> success/fail;
write INCUDELAY and 6..9 (success) or 0..3 (fail);
if (success && sanc >= INCUBATE) { idx += 30; HP=20000; level=1; life=rand%20+10; feed=30; kills=1; MountProcess(); }
```

---

#### D11 - Guaranteed refine stone (4141)

**Overview**: Index 4141 always succeeds because it is routed to the `OriLacto == 2` rate row, which returns 100.

**Detailed description**: The ternary at `:847` is the only place in the file that special-cases an item index inside the refine engine. It substitutes `OriLacto = 2` for the rate lookup, which in `BASE_GetSuccessRate` short-circuits at three points: star 10 returns 100, any other star returns 100, and the table row is never consulted (`Basedef.cpp:2212-2224`).

The stone still respects every other restriction: target eligibility (D1/D2), ceilings (D4/D8) and consumption. It cannot push past +9 with PO semantics (`OriLacto` is 2, so neither the +6 nor the +9 clamp applies; the `sanc > 9 → 9` clamp does). It also cannot reach star 11 unless the item is exactly at star 10, because only that branch writes `11`.

The effect is a paid "no-fail" upgrade path for stars 0-9. Because `g_pSancRate[2]` is also mutated by the `SancRate.txt` key `ÁMAGO` (mangled to row 0 due to a copy-paste at `CReadFiles.cpp:157`), the shipped configuration cannot accidentally lower the guaranteed rate.

**Rule workflow**:
```
rate = (item->sIndex == 4141) ? BASE_GetSuccessRate(dest, 2) : BASE_GetSuccessRate(dest, Vol-4);
// OriLacto 2 => 100 for every star, incl. star 10
```

---

#### D12 - Refine feedback contract

**Overview**: Success and failure are signalled through client messages from the string table plus emotion animations, with a face/no-face branch.

**Detailed description**: Every refine family ends with either `SendClientMessage(conn, g_pMessageStringTable[_NN_Refine_Success])` or `...[_NN_Fail_To_Refine]`, plus `SendEmotion`. On success the emotion is `(14, 3)`; on failure it is `(15, 0)` when `pMob[conn].MOB.Equip[0].sIndex / 10` is non-zero and `(20, 0)` otherwise (`:318-324`, `:964-970`).

`Equip[0].sIndex / 10` is the face/class-derived sprite index, so a character without a face (index < 10) gets a different failure animation. This idiom is repeated in most branches that report failure, and it means the face item is implicitly required for correct emotion selection.

The locked-seal and earring branches also emit the emotion **after** the item has been cleared, so the client receives "fail" plus a resend of an empty slot. No branch sends a structured result code; the client infers everything from the text and the subsequent `_MSG_SendItem`.

**Rule workflow**:
```
success: SendClientMessage(_NN_Refine_Success); SendEmotion(conn, 14, 3);
failure: SendClientMessage(_NN_Fail_To_Refine); SendEmotion(conn, face?15:20, 0);
```

---

#### D13 - Refine audit trail

**Overview**: Each attempt writes one `ItemLog` line containing the destination index, sanc before, and the three effects of the stone.

**Detailed description**: The format is `useitem,refine success %d+%d - (%d,%d,%d)` (or `refine fail`), with `dest->sIndex`, `sanc` and the stone's three `cEffect` bytes (`:285`, `:305`, `:464`, `:486`, `:668`, `:687`, `:755`, `:774`, `:882`, `:931`). `ItemLog(str1, str2, ip)` (`Server.cpp:9264`) writes to the item log file with account and IP.

Because the log is written **after** the mutation in the success path, the destination index is the post-refine index (matters for D5 and D10), while `sanc` is the pre-refine value captured at `:194`. The stone effects are logged from `item`, i.e. the stone, before its `memset`.

Nineteen of the 119 `ItemLog` calls in the file are refine-related; the rest cover teleports, tickets, quests, gems and mounts. There is no structured/csv audit and no correlation id, so reconstructing an economy incident requires text parsing.

**Rule workflow**:
```
sprintf(temp, "useitem,refine success %d+%d - (%d,%d,%d)", dest->sIndex, sanc, eff0, eff1, eff2);
ItemLog(temp, pUser[conn].AccountName, pUser[conn].IP);
```

---

#### E1 - Pilula Orc (one-shot quest)

**Overview**: Vol 6 grants 9 skill points exactly once per character; a second use is refused and the item is **kept**.

**Detailed description**: The branch checks `pMob[conn].extra.QuestInfo.Mortal.PilulaOrc` and returns immediately if set - without consuming (`:980-981`). Otherwise it adds 9 to `MOB.SkillBonus`, sets the flag, emits emotion 14, calls `SendEtc` to refresh the character sheet, then consumes and logs.

Because the flag lives in `STRUCT_MOBEXTRA.QuestInfo.Mortal`, which is part of the persisted character record, the rule is permanent. There is no class-master gate: a CELESTIAL who somehow obtained the item still gets 9 skill points, and the flag is stored in the Mortal sub-struct regardless.

The "return without consuming" behaviour is user-hostile: the player keeps a permanently unusable item with no message. It is the only branch in the handler that both refuses and refunds.

**Rule workflow**:
```
if (PilulaOrc) return;                       // item kept, silent
SkillBonus += 9; PilulaOrc = 1; SendEmotion; SendEtc; consume; log;
```

---

#### E2 - Fairy dust (level jump)

**Overview**: Vol 7 has a 50 % chance to set the experience to exactly the next level threshold; it is consumed either way.

**Detailed description**: The level ceiling is class-master dependent: `MAX_LEVEL` (399) for MORTAL and ARCH, `MAX_CLEVEL` (199) for the celestial family (`:1002`). The roll is `rand() % 2`, so 50 %; a level at or above the ceiling, a negative level, or a losing roll produces emotion 15/20 plus "Fail To Level Up" (`:1004-1014`).

On success it **overwrites** `MOB.Exp` with `g_pNextLevel[Level+1]` (or `g_pNextLevel_2` for celestials) rather than adding to it (`:1017`). This is a strict "jump to the threshold" semantic: excess experience is discarded, and a character already close to the next level gains almost nothing. `CheckGetLevel()` is then called, which increments the level and may return 4 (level-up occurred); only when it returns true is the client told "Level UP", rescored, and - for MORTAL - given the level item via `DoItemLevel`.

Consumption is unconditional (`:1033-1037`) and the audit line records the class master and level (`:1039`).

**Rule workflow**:
```
if (level >= max || level < 0 || rand()%2) fail-message;
else { Exp = g_pNextLevel[level+1]; if (CheckGetLevel()) { msg; emotion; SendScore; SendEtc; if(MORTAL) DoItemLevel(); } }
consume; log;
```

---

#### E3 - Crescent eye (flat experience)

**Overview**: Vol 8 gives 2500 experience to MORTAL characters only; non-mortals keep the item.

**Detailed description**: `if (pMob[conn].extra.ClassMaster != MORTAL) return;` (`:1047-1048`) precedes any mutation, so the item is not consumed for the celestial/arch families. Mortals receive `Exp += 2500`, the exact amount is echoed to the client via `_NS_GETEXP`, and emotion 14 is emitted.

Level-up handling is gated on `CheckGetLevel() == 4` (`:1057`), i.e. only when the call actually performed a level-up. In that case `SetCircletSubGod` is invoked (which can grant permanent helmet bonuses at levels 120/150/180/199 for SCELESTIAL), the client is told "Level Up", rescored, and `DoItemLevel` may push the level reward item. Because a single 2500-exp grant can at most produce one level, the `== 4` test is sufficient here - unlike the quest branch (H3), which can cross several levels and therefore only reacts to the last one.

**Rule workflow**:
```
if (ClassMaster != MORTAL) return;            // item kept
Exp += 2500; msg; emotion;
if (CheckGetLevel() == 4) { SetCircletSubGod; msg; SendScore; if (MORTAL) DoItemLevel; }
SendEtc; consume; log;
```

---

#### E4 - Legendary (LE) upgrade stones

**Overview**: Vol 9 (Adamantite/Beril and siblings) promotes a matching-grade armour piece to its `Extra` index on a 51 % roll.

**Detailed description**: `Type = item->sIndex - 575` must be 0..3 (`:1093-1095`), so exactly four stone indexes are recognised (575-578). The destination's `nUnique` is mapped to a required `UniqueType` through a 5-line table (`:1100-1113`): `5/14/24/34 → 0`, `6/15/25/35 → 1`, `7/16/26/36 → 2`, `8/17/27/37 → 3`, and `10/20/30/40 → 3`. `nUnique == -1` means "not classifiable" and produces the message `_NN_Only_Level1_Armor + Type` (the message table has four consecutive entries, one per tier).

If `UniqueType == Type`, the destination's `Grade` must be 1..3 (`:1126`). The roll is `rand() % 100 <= 50`, i.e. 51 %. Success writes `dest->sIndex = g_pItemList[destIndex].Extra` (`:1134`) - the item-list "upgrade target" column - which is how the LE family is chained in data. Failure only emits emotion 15/20; the destination is untouched and the stone is consumed.

Note the mapping collision: `nUnique` 10/20/30/40 maps to `UniqueType 3` as well, so the fourth tier accepts two different unique families. This is either intentional ("abyss" and "kingdom" both refine with the same stone) or a copy-paste defect; the item list is not available to confirm (confidence: low).

**Rule workflow**:
```
Type = sIndex - 575; if (Type not in 0..3) refund;
UniqueType = map(g_pItemList[dest].nUnique);
if (UniqueType != Type) { refund; msg "Only LevelN Armor"; }
if (Grade not in 1..3) { refund; "Cant Refine More"; }
if (rand()%100 <= 50) dest->sIndex = g_pItemList[dest].Extra; else emotion-fail;
consume;
```

---

#### E5 - Kappa potions (affect type 4)

**Overview**: Nine potion indexes install or refresh an `Affect` of `Type = 4` with a class value (1 Kappa, 2 Combatente, 3 Mental) for a duration that varies from ~11 minutes to 20 hours.

**Detailed description**: The branch is entered for `Vol` in {10, 52, 53, 55, 56, 57, 200, 201, 202} and then maps `item->sIndex` to a `(value, tempo)` pair (`:1205-1255`): 787 → (1, 80), 1764 → (2, 80), 1765 → (3, 80), 3312 → (3, 450), 3311 → (2, 450), 3310 → (1, 225), 3321/3320/3319 → (3/2/1, 9000). Any other index inside those volatiles silently gets `value = 0` and still installs an affect.

The affect allocator is a two-pass search: first look for an existing `Type == 4 && Value == value` slot (refresh in place, so durations do not stack), then for the first empty slot (`:1259-1275`). When found, the slot is written with `Level = 0`, `Value`, `Time = tempo` and the score is recomputed. When **not** found (all 32 slots occupied by other affects), the potion is still consumed with no effect (`:1289-1293`).

Duration units are 8-second affect ticks: the timer loop decrements `Affect[i].Time` for one sixteenth of the players per 500 ms tick (`ProcessSecMinTimer.cpp:1184-1187`, `Server.cpp:3631`, `Server.cpp:5377-5378`), so `AFFECT_1H = 450` ticks = 3600 s. The branch therefore provides 640 s (default 80), 1800 s (`AFFECT_1H/2`), 3600 s (`AFFECT_1H`) and 72000 s (20 h) variants.

**Rule workflow**:
```
value/tempo = map(sIndex);
slot = find(Type==4 && Value==value) ?? find(Type==0);
if (slot >= 0) { Type=4; Level=0; Value=value; Time=tempo; GetCurrentScore; SendScore; }
consume; log;
```

---

#### E6 - Recall scroll

**Overview**: Vol 11 teleports the character to its home city, guild zone or rookie area.

**Detailed description**: `DoRecall(conn)` (`Server.cpp:7558-7602`) computes the destination from three sources in priority order: the guild zone of the character's guild (`g_pGuildZone[i].ChargeGuild`), then the city spawn derived from `MOB.Merchant & 0xC0` (`CitySpawnX/Y`), and finally - overriding both - the rookie area (2112, 2042) for MORTAL characters below `FREEEXP`.

The handler itself performs no validation: no combat check, no map restriction, no cooldown. Any character in `USER_PLAY` with the scroll and a live trade-free state teleports. `DoRecall` uses `GetEmptyMobGrid` and, unlike `DoTeleport`, does **not** check the return value before calling `GetAction`/`GridMulticast`, so a fully occupied destination still moves the character.

The scroll is consumed after the teleport, and the audit line is the constant string `useitem,recall` with no coordinates.

**Rule workflow**:
```
DoRecall(conn); consume; ItemLog("useitem,recall", ...);
```

---

#### E7 - Stellar gem (save point)

**Overview**: Vol 12 stores the character's current coordinates into `SPX/SPY` unless the position is a sanctuary, a war arena or a village.

**Detailed description**: The gem reads `pMob[conn].TargetX/TargetY` (the authoritative position) and `GetAttribute()` (the map-attribute byte). Two map regions bypass all checks and jump to the save (`:1321-1325`): `(TargetX/128, TargetY/128) == (9,1)` and `(8,2)` - these are the nightmare-ticket staging areas, so players can save next to an instance entrance.

Otherwise three rejections apply: `map_att & 4` (a "no-teleport" attribute) for characters below level 1000 (`:1327-1332`), `BASE_GetArena(...) < MAX_GUILDZONE` (inside a guild war area) and `BASE_GetVillage(...) < MAX_GUILDZONE` (inside a city). Both helpers return `5` when the coordinate is outside all five zones (`Basedef.cpp:4247-4267`), so `< 5` means "inside". The level-1000 exemption is the GM/admin level boundary used throughout the server.

On success the handler emits an emotion chosen by `Equip[0].sIndex / 10` (0 → 23, 1/2/3 → 15), writes SPX/SPY, sends `_NN_Set_Warp`, consumes and logs the coordinates.

**Rule workflow**:
```
if (region == (9,1) || region == (8,2)) goto save;
if (attr&4 && level<1000) reject; if (Arena<5 || Village<5) reject;
save: emotion; SPX=TargetX; SPY=TargetY; msg; consume; log;
```

---

#### E8 - Portal scroll (return to saved point)

**Overview**: Vol 13 teleports to the saved `SPX/SPY`, refusing when the saved point is in the sanctuary regions or the destination forbids teleport.

**Detailed description**: The first check inspects the **saved** point, not the current one: if `SPX/128, SPY/128` is `(9,1)` or `(8,2)` the scroll is refused with "Cant Use That Here" and refunded (`:1380-1385`). This symmetrically closes the loophole that E7 opens - you may save there, but you may not return there with a scroll.

The second check inspects the **current** position's map attribute (`map_att & 4` and level < 1000), so a player standing in a no-teleport zone cannot jump out. Note that `GetAttribute` is called with `TargetX/TargetY` (current position), while the teleport itself goes to `SPX/SPY`.

`DoTeleport` (`Server.cpp:8212-8230`) is then used; it calls `GetEmptyMobGrid` and silently does nothing when no free cell is found. The scroll is consumed regardless, and the log line records the *current* coordinates rather than the destination (`:1398`) - an audit defect.

**Rule workflow**:
```
if (saved region is sanctuary) refund;
if (attr(TargetX,TargetY)&4 && level<1000) refund;
DoTeleport(conn, SPX, SPY); consume; log(current position);
```

---

#### E9 - DB record ticket

**Overview**: Vol 14 forwards an audit record to DBSrv and consumes itself.

**Detailed description**: The branch builds a `MSG_MessageDBRecord` on the stack, zeroes it, sets `Type`, `ID = 0` and `Size`, formats `"TCK %d %s %d %d %d %d"` with `ServerIndex`, the account name, the three effect bytes and the item index, and sends it through `DBServerSocket.SendOneMessage` (`:1413-1422`). The item is then consumed.

This is the only branch in the handler that communicates with another process. It is fire-and-forget: no acknowledgement, no retry, and the item is consumed immediately after the send call - if the DB link is down the record is lost and the item is gone. There is also no local `ItemLog` line, so the only trace is on the DB side.

The message id `_MSG_MessageDBRecord` and the `TCK` prefix suggest a "ticket" handed to customer support; the record content is fully controlled by the item's own effect bytes, which are set at creation time by the item list.

**Rule workflow**:
```
sm_dbrec.Type = _MSG_MessageDBRecord; sm_dbrec.ID = 0; sm_dbrec.Size = sizeof(...);
sprintf(sm_dbrec.Record, "TCK %d %s %d %d %d %d", ServerIndex, AccountName, eff0, eff1, eff2, sIndex);
DBServerSocket.SendOneMessage(&sm_dbrec, sizeof(...)); consume;
```

---

#### E10 - Guild board and fireworks (non-consuming / cosmetic)

**Overview**: Vol 17 opens the guild board without consuming the item; Vol 19 plays a random firework animation and consumes.

**Detailed description**: The guild board branch reads the guild id encoded in the item itself (`BASE_GetGuild(item)`, which decodes `EF_HWORDGUILD`/`EF_LWORDGUILD`, `Basedef.cpp:4207-4240`) and, when it is positive, queues a `MSG_STANDARDPARM2` with `Type = _MSG_GuildBoard`, `ID = ESCENE_FIELD`, `Parm1 = guild`, `Parm2 = GUILDBOARD` directly onto the socket (`:1646-1660`). The function then returns **without consuming** - the item is a reusable guild furniture/board.

The fireworks branch calls `SendEmotion(conn, 100, rand() % 5)` (`:1668`), i.e. one of five animation variants, consumes the item and logs a constant string. No state is mutated and no broadcast is performed: only the user sees the effect (the emotion packet is targeted at `conn`).

These two branches are the only purely client-facing ones; they demonstrate that the handler doubles as a "remote control" for client UI features, not only as an item effect processor.

**Rule workflow**:
```
if (Vol == 17) { if (guild>0) queue _MSG_GuildBoard; return; }   // no consumption
if (Vol == 19) { SendEmotion(conn, 100, rand()%5); consume; log; }
```

---

#### E11 - Vigor, XP chest and roast chicken (affect 35/39/30)

**Overview**: Three timed consumables install affects 35 (vigor, 1 h), 39 (XP bonus, +2 h capped at 30 days) and 30 (damage, 4 h) with different allocators.

**Detailed description**: Vigor (`Vol 58`) calls `GetEmptyAffect(conn, 35)`, which returns the existing type-35 slot if present or the first empty slot, and rejects with `_NN_CantEatMore` only when it returns -1 (`:2133-2140`). It then writes `Type 35, Level 1, Value 0, Time = AFFECT_1H` and recomputes HP/MP and score. Re-use therefore refreshes rather than extends.

The XP chest (`Vol 198`) uses the same allocator with type 39 but **adds** to `Time` (`:2179`) instead of assigning, capped at 324000 ticks (30 days). Because the allocator returns an empty slot whose `Time` is normally 0 after expiry, the additive semantic only matters when the slot is reused while still active - in which case it stacks. The cap is applied after the addition.

Roast chicken (`Vol 63`) targets affect 30 with `Level = 1000`, `Time = AFFECT_1H * 4` (4 h) and the same 324000 cap. Its guard is the oddest in the file: `if (sAffect == -1 || (Affect[sAffect].Value != 2000 && Affect[sAffect].Time == 30))` (`:2204`) - it rejects only when the existing slot holds `Value 2000` **and** `Time 30`, a combination no branch in the file produces. Effectively the check never fires.

**Rule workflow**:
```
slot = GetEmptyAffect(conn, type); if (slot == -1) { msg; refund; }
Type/Level/Value/Time = per-item; if (Time >= 324000) Time = 324000;
BASE_GetHpMp; GetCurrentScore; SendScore; consume; log;
```

---

#### E12 - Courage medicine and Elixir of Courage (affect 30)

**Overview**: Two consumables add flat damage through affect 30: +500 for 100 ticks, or +2000 for 1 h / 30 days.

**Detailed description**: Both branches allocate affect 30 with `Level` as the damage bonus (`500` and `2000`) and `Value = 1`, and both set `Time` to a fixed value: 100 ticks for the medicine (`:3592`) and `AFFECT_1H` or `AFFECT_1D * 30` for the elixir depending on whether the index is 3378 (`:3626`).

Their duplicate-detection guards are broken by design. The medicine rejects when `Value != 500` (`:3582`) but writes `Value = 1`, so the condition is always true for a medicine-placed affect and the next use is always refused - while an elixir-placed affect (`Value = 1`, `Level = 2000`) satisfies `Value != 500` and also blocks the medicine. The elixir has the mirror problem: it rejects when `Value != 2000`, a value never written.

Net effect: after the first courage item, affect 30 is permanently occupied and every later courage item is refused with "CantEatMore" and **refunded**. Because affect 30 shares the slot with roast chicken, using chicken first also blocks courage items. This is a functional defect rather than an intended rule (confidence: high, from direct comparison of the written and compared values).

**Rule workflow**:
```
slot = GetEmptyAffect(conn, 30);
if (slot == -1 || (Value != <500|2000> && ...)) { msg; refund; }
Type=30; Value=1; Level=<500|2000>; Time=<100|450|324000>; clamp; rescore; consume;
```

---

#### E13 - Forgiveness scroll (PK point reset)

**Overview**: Vol 203 sets the kill-mark counter to 150 and rebroadcasts the character sprite.

**Detailed description**: `SetPKPoint(conn, 150)` (`GetFunc.cpp:2145-2159`) clamps the value to 1..150 and writes it into `MOB.Carry[KILL_MARK].stEffect[0].cEffect` (`KILL_MARK = 63`), i.e. the PK counter is an effect on a reserved inventory slot. Setting it to 150 is the maximum - which is the *worst* state in this encoding, not a reset; the log line even reads `useitem,perdao new cp 75` while the code passes 150 (`:3755`).

The branch then rebuilds the character's appearance (`GetCreateMob`) and broadcasts it to the surrounding grid, so other players see the new "killer" sprite. Emotion 14 signals success and the scroll is consumed.

This is one of the few branches whose *intent* cannot be inferred from the code with confidence: the item is named "forgiveness" (`perdão`) but writes the maximum guilt value. Either the encoding is inverted elsewhere, or the item is a self-inflicted punishment. Documented as ambiguous.

**Rule workflow**:
```
SetPKPoint(conn, 150); GetCreateMob; GridMulticast; SendEmotion(14,3); consume; log("new cp 75");
```

---

#### E14 - Soul seal (empty)

**Overview**: Vol 206 with an empty first effect captures the character into the capsule system; a filled seal does nothing.

**Detailed description**: `if (item->stEffect[0].cEffect == 0)` distinguishes an empty seal. The branch requires `ClassMaster == SCELESTIAL` (the third-tier "god" class), otherwise it emits `_NN_GOD2CHARONLY_` and refunds (`:5633-5638`).

When allowed, the handler consumes the seal first, then calls `CharLogOut(conn)` and sends `_MSG_DBPutInCapsule` with `Parm = pUser[conn].Slot` (`:5640-5657`). Consuming before the logout is deliberate: the item must be gone from the persisted character before the capsule snapshot is taken. The capsule message is processed by DBSrv, which serialises the current character (`STRUCT_CAPSULE` = `STRUCT_MOB` + `STRUCT_MOBEXTRA`) into a slot.

A **filled** seal (`cEffect != 0`) falls through the `if` and returns without consuming - presumably the restore path is handled by a different handler (`_MSG_PutoutSeal`), which is outside this component.

**Rule workflow**:
```
if (seal empty) { if (ClassMaster != SCELESTIAL) { msg; refund; }
                  consume; CharLogOut; send _MSG_DBPutInCapsule(slot); }
return;   // filled seal: no-op
```

---

#### E15 - War pack (donation)

**Overview**: Vol 184 adds the item's `EF_DONATE` value to the session donation counter with no ceiling.

**Detailed description**: `donate = BASE_GetItemAbility(item, EF_DONATE)` reads effect id 91 from the item's static/effect payload; `pUser[conn].Donate += donate` accumulates it in the session object (`CUser`), then the client is told `_NN_Cash_ChargeOk`, emotion 14 is emitted and the pack is consumed (`:5710-5722`).

There is no bound, no persistence call and no per-account rate limit inside the handler. `pUser.Donate` lives in the per-connection session, so the counter is lost on disconnect unless some other component persists it (the `DonateInfo.LastTime/Count` pair in `STRUCT_MOBEXTRA` exists for that purpose but is not touched here). The item can therefore be used repeatedly to inflate the session counter; whether that is exploitable depends entirely on how `pUser.Donate` is consumed elsewhere.

The absence of an audit `ItemLog` line in this branch is notable given that every other value-bearing branch logs.

**Rule workflow**:
```
donate = EF_DONATE; Donate += donate; SendClientMessage(_NN_Cash_ChargeOk); emotion;
consume; (no ItemLog);
```

---

#### E16 - Mystic stone (CelestialCS ↔ SCELESTIAL swap)

**Overview**: Index 4148 switches a character between the two "god 2" sub-classes, snapshotting one and restoring the other.

**Detailed description**: The branch requires `ClassMaster` to be `CELESTIALCS` (4) or `SCELESTIAL` (5) and the character to be standing inside a city (`BASE_GetVillage` in 0..4, `:3079-3086`). It then computes `cl` (the class being saved) and `ncl` (the class being restored) as opposites, and flips `ClassMaster`.

The snapshot copies the entire `BaseScore` block, class, exp, learned skills, `ScoreBonus`/`SkillBonus`/`SpecialBonus`, `SPX/SPY`, the soul byte, and both skill bars (`SkillBar1` 4 bytes and `SkillBar2` 16 bytes) into `extra.SaveCelestial[cl]` (`:3101-3114`). Restoration is the mirror image into the live `MOB` (`:3116-3128`) plus a `_MSG_SetShortSkill` push to the client.

Before restoring, all affects are cleared except types 4, 8, 29, 30, 34, 35 and 39 (`:3130-3136`) - i.e. paid/potion buffs survive the class swap while spell buffs do not. The face sprite is rebuilt as `extra.MortalFace + 5 + MOB.Class`, then bonus points, HP/MP and the full score are recomputed, guild membership is refreshed and `SendScore`/`SendEtc` are emitted. The stone is consumed and the log line is `useitem,mistery stone change god2god`.

**Rule workflow**:
```
if (ClassMaster not in {4,5}) refund; if (Village not in 0..4) { msg; refund; }
cl = (ClassMaster==4)?0:1; ncl = 1-cl;
SaveCelestial[cl] = live; ClassMaster = other; live = SaveCelestial[ncl];
clear affects except whitelist; rebuild face; recompute; SendScore/SendEtc; consume; log;
```

---

#### E17 - Seal of the Warrior (fame and cloak)

**Overview**: Index 4146 grants 10 fame and, for mortals at level 354+ without a cloak, a clan-coloured cloak.

**Detailed description**: The branch sets `pUser[conn].UseItemTime` (the only place the timestamp is written, and it is not read anywhere since the cooldown block at `:206-219` is commented out). It then applies `if (Fame >= 2000000000) Fame = 2000000000;` **before** `Fame += 10` (`:3216-3219`), so the effective cap is 2,000,000,010 - a classic off-by-operation.

The cloak grant requires four conditions: `ClassMaster == MORTAL`, `CurrentScore.Level >= 354`, and `Equip[15].sIndex` not already one of 3191-3196. It then zeroes slot 15 and writes 3191 (clan 7), 3192 (clan 8) or 3193 (any other clan), resending the equipment (`:3223-3237`). The 3194-3196 exclusion suggests those are higher-tier cloaks that must not be overwritten.

There is no class gate for the fame gain, and consumption is unconditional. The item is therefore a repeatable fame source whose only limit is the drop rate.

**Rule workflow**:
```
UseItemTime = tick; if (Fame >= 2e9) Fame = 2e9; Fame += 10; emotion;
if (MORTAL && level>=354 && Equip[15] not in 3191..3196) { Equip[15] = per-clan cloak; SendItem; }
consume; log;
```

---

#### E18 - Traveler's bag (carry capacity)

**Overview**: Index 3467 adds 15 inventory slots by occupying reserved slots 60/61 with a dated bag item.

**Detailed description**: The capacity model is implicit: `MaxCarry` starts at 30, and the bag branch recomputes it as `30 + 15*(bag in slot 60) + 15*(bag in slot 61)` (`:5688-5694`). The branch first refuses when both reserved slots already hold 3467 (`:5666-5670`) - note that in this case the item is **not** consumed.

Otherwise it writes 3467 into slot 60 if empty, else into slot 61, stamps a 30-day date with `BASE_SetItemDate` (`Basedef.cpp:6732-6760`, which encodes `EF_WDAY`/`EF_WMONTH`), pushes the whole inventory with `SendCarry`, recomputes `MaxCarry`, consumes the source and pushes `SendCarry` again.

Two consequences: `MaxCarry` is *recomputed from scratch*, so any other mechanism that raised `MaxCarry` is overwritten; and the bag items themselves are ordinary dated items in the inventory, so they can be traded or dropped - at which point neither this handler nor the expiry timer (`ProcessSecMinTimer.cpp:571-600`, which recomputes `MaxCarry` only when a dated bag is actually cleared in slot 60/61) sees them, and the extra slots remain until the next bag use or relog (`ProcessDBMessage.cpp:751` resets to 30).

**Rule workflow**:
```
if (Carry[60]==3467 && Carry[61]==3467) { msg; return; }        // no consume
Carry[60 or 61] = 3467 with date+30; SendCarry; MaxCarry = 30 + 15*count;
consume; SendCarry;
```

---

#### F1 - Water scroll M (dungeon M, rooms 0-9)

**Overview**: Vol 21-30 teleports the leader and party into one of ten rooms of the "M" water dungeon, arms a room timer and spawns the room's mobs.

**Detailed description**: The room is `Vol - 21`, and the destination is `WaterScrollPosition[1][room]` - the middle of the three 10-entry coordinate tables (`Server.cpp:56-94`). Entry requires the leader to be standing within ±8 of any of the ten room centres (or ±12 for rooms 8-9, `:1689-1704`), or at `(TargetX/4, TargetY/4) == (491, 443)` for any scroll except `Vol 29`. That second clause is a lobby coordinate where scrolls may be used freely.

Three further gates: the caller must be the party leader (`Leader == 0 || Leader == -1`, `:1713-1718`), the target room must be empty of players (`GetUserInArea` must return 0, `:1723-1733`), and the room timer array entry `WaterClear1[1][room]` is set to 30 ticks for rooms 0-7 and 15 for rooms 8-9. `WaterClear1` is decremented one unit every two seconds (`ProcessSecMinTimer.cpp:1108-1130`, gated on `SecCounter % 4 == 0` with `SecCounter++` per 500 ms tick at `:123`) and, when it reaches 1, teleports everyone in the room back to (1965, 1769). The "30/15" values are therefore 60 s and 30 s of room time, which matches the `WaterClear1*2` seconds sent to the client as `_MSG_StartTime`.

The teleport covers the leader and every party member in `USER_PLAY`, each receiving `_MSG_StartTime = WaterClear1*2` (seconds). Then rooms 0-7 spawn `WATER_M_INITIAL + room` twice and call `RebuildGenerator()`; room 9 spawns one of four bosses with a 4/1/1/4 distribution; room 8 spawns nothing.

**Rule workflow**:
```
room = Vol-21; inside = position near centre (r=8 or 12) or lobby;
if (!inside) { msg; refund; } if (Leader) { msg; refund; }
if (GetUserInArea(room) >= 1) { msg; refund; }
WaterClear1[1][room] = room<=7 ? 30 : 15;
DoTeleport(self + party); StartTime signals;
if (room<=7) { 2x GenerateMob(room+10); RebuildGenerator(); } else if (room==9) { 1 boss by roll; }
consume; log;
```

---

#### F2 - Water scroll N (dungeon N, rooms 0-9)

**Overview**: Vol 131-140 is the "N" variant of F1; identical rules, different table, no generator rebuild.

**Detailed description**: The branch is a near-verbatim copy of F1 with `Vol - 131` as the room index, `WaterScrollPosition[0]` as the table and `WaterClear1[0]` as the timer store (`:1876-1978`). The lobby coordinate clause uses `Vol == 139` instead of `Vol == 29`.

The only behavioural differences are that rooms 0-7 spawn `WATER_N_INITIAL + room` twice **without** calling `RebuildGenerator()` (the call is commented out at `:1949`), and the audit string identifies the dungeon as `water scroll N`.

The duplication means any future fix to F1 (e.g. the `GetUserInArea` off-by-one or the missing re-check after teleport) must be applied three times. It also means the three dungeons share the same global `WaterClear1` timer array but different rows, which is the only thing preventing cross-dungeon interference.

**Rule workflow**:
```
identical to F1 with table index 0, timer row 0, base Vol 131, lobby check Vol==139;
spawn 2x mobs for rooms 0-7 without RebuildGenerator; one boss for room 9;
```

---

#### F3 - Water scroll A (dungeon A, rooms 0-9)

**Overview**: Vol 161-170 is the "A" variant of F1; identical rules, third table, no generator rebuild.

**Detailed description**: Room = `Vol - 161`, table `WaterScrollPosition[2]`, timer row `WaterClear1[2]`, lobby clause `Vol == 169`, mob base `WATER_A_INITIAL` (`:1981-2082`). As with F2, rooms 0-7 spawn two mobs and skip `RebuildGenerator()` (commented at `:2054`), and room 9 rolls one of four bosses with the same 4/1/1/4 distribution.

Because only the M dungeon calls `RebuildGenerator()`, the generator counters for N and A rooms are not recomputed after spawning; `RebuildGenerator` recomputes `NumOld[]` from live mobs and is the mechanism that prevents over-spawning on the next regeneration tick. Its absence in two of the three dungeons is a probable source of mob-count drift (confidence: medium).

All three water branches share the same latent defect: `GetUserInArea` returns the number of players in the room and the code treats `>= 1` as "occupied", so a **solo** player standing in the room is counted as an obstruction; the printed message then shows `UserArea - 1 = 0` other players.

**Rule workflow**:
```
room = Vol-161; table 2; timer row 2; lobby Vol==169;
same gates and teleports as F1; 2x GenerateMob(room+WATER_A_INITIAL); boss for room 9;
```

---

#### F4 - Nightmare ticket N

**Overview**: Vol 173 admits the leader and party to the "N" nightmare only in the 20-minute window at the start of each half-hour, and only below the configured party limit.

**Detailed description**: Three positional gates come first: the player must stand in region `(19,15)` (`TargetX/128`, `TargetY/128`), must be the party leader, and must not be a celestial-class character at level 180 or above (`:2445-2463`) - that last check refunds the ticket silently.

The time gate is the core rule: `if (tm_min in 4..19 || 24..39 || 44..59)` the ticket is refunded without a message (`:2470-2476`). Entry is therefore allowed only during minutes 0-3, 20-23 and 40-43 - a four-minute window every twenty minutes, matching a 15-minute instance plus a 5-minute cooldown.

Party handling depends on the item index: `sIndex == 3324` marks a party ticket, which increments `PartyPesa[0]` and is refused when `PartyPesa[0] >= maxNightmare` (default 3, configurable via `imple.cpp:1775-1781`) (`:2478-2490`). The individual ticket skips that counter entirely. `NigthTime` is computed as 240 minus the elapsed seconds inside the window, so a party entering at :00 gets the full 240 and one entering at :03 gets 60.

Teleport uses `PesaNPosStandard[i]` - all 13 rows are the same coordinate (1304, 335), so the party stacks on one cell and `DoTeleport`'s `GetEmptyMobGrid` spreads them.

**Rule workflow**:
```
if (region != (19,15)) { msg; refund; } if (Leader) { msg; refund; }
if (celestial && level>=180) refund;
if (minute not in [0-3]|[20-23]|[40-43]) refund;
if (party ticket) { if (PartyPesa[0] >= maxNightmare) { msg; refund; } PartyPesa[0]++; }
NigthTime = 240 - ((min-20|40|0)*60 + sec);
DoTeleport(self, party); StartTime(NigthTime); consume; log;
```

---

#### F5 - Nightmare ticket M

**Overview**: Vol 174 is the "M" nightmare: region (16,16), windows at :05-:08, :25-:28 and :45-:48, and it honours a saved celestial position.

**Detailed description**: The gates mirror F4 with a different region and a different minute set (`:2540-2571`), and the same celestial-180 exclusion. The party counter is `PartyPesa[1]` and the party ticket index is 3325.

The distinguishing rule is the destination choice: if the character's saved `SPX/SPY` region is `(8,2)` - one of the sanctuary regions from E7 - the teleport goes to that saved point instead of `PesaMPosStandard`, and the same substitution is applied per party member using **that member's** saved point (`:2598-2601`, `:2614-2617`). This lets a party stage inside the instance area and re-enter at their own saved spot.

As in F4, all 13 rows of `PesaMPosStandard` are identical (1083, 308).

**Rule workflow**:
```
region must be (16,16); leader only; celestial>=180 refund;
minute must be in [5-8]|[25-28]|[45-48];
party ticket (3325) -> PartyPesa[1] limit; NigthTime = 240 - elapsed;
destination = (saved region == (8,2)) ? saved : PesaMPosStandard[i];
teleport self+party; StartTime; consume;
```

---

#### F6 - Nightmare ticket A

**Overview**: Vol 175 is the "A" nightmare: region (19,13), windows at :10-:13, :30-:33, :50-:53, and it spends one `NT` per celestial participant.

**Detailed description**: The branch is structurally different from F4/F5 in three ways. First, the party-counter increment is **unconditional**: `PartyPesa[2]++` happens before the `isParty` test (`:2688-2695`), so individual tickets consume the shared party budget too. Second, celestial-class characters must have `extra.NT > 0` and are decremented once per use, and each celestial party member is checked and decremented individually (`:2681-2686`, `:2730-2736`); a member without `NT` is skipped. Third, the destination jitter `+rand()%1` is always zero (`:2714`, `:2717`), so the "random spread" is dead code and all members land on the same coordinate unless `GetEmptyMobGrid` moves them.

The celestial-180 exclusion is present twice - before the time gate and inside the party loop (`:2661`, `:2725`) - so high-level celestials are filtered out of both the entry and the party teleport.

`NT` is the counter granted by the Nightmare writ (F7), so the "A" nightmare is effectively a two-resource instance.

**Rule workflow**:
```
region (19,13); leader only; celestial>=180 refund; minute in [10-13]|[30-33]|[50-53];
if (celestial && NT<=0) refund;
if (PartyPesa[2] >= maxNightmare) { msg; refund; } PartyPesa[2]++;
if (celestial) NT--;
teleport self (+jitter 0) and party (each celestial must have NT>0 and is decremented);
consume; log;
```

---

#### F7 - Nightmare writ (NT grant)

**Overview**: Vol 212 grants 13 `NT` charges but only once per 20 hours.

**Detailed description**: The branch compares `time(NULL)` against `extra.LastNT + 72000` (`:3184`). When the cooldown is active it computes the remaining hours and minutes and sends `_DD_NightmareTime` with both, then refunds the writ (`:3186-3193`).

Otherwise it stamps `extra.LastNT = now` and adds 13 to `extra.NT` (`:3196-3197`), consumes and logs the new total. There is no cap on `NT`, so writs can be stockpiled one per 20 hours; the cooldown is per character (`LastNT` lives in `STRUCT_MOBEXTRA`), not per account, and it is a wall-clock timestamp so server restarts do not reset it.

The 72000-second constant is exactly 20 hours, which - combined with F6's 13 charges - sets the intended Nightmare A throughput at 13 entries per 20 hours per celestial character.

**Rule workflow**:
```
if (time < LastNT + 72000) { compute remaining h/m; msg; refund; }
LastNT = time; NT += 13; consume; log(NT);
```

---

#### F8 - Duel card (secret room)

**Overview**: Vol 20 opens one of three secret rooms from the Tor-Alter position, after clearing the arena and checking that nobody is inside.

**Detailed description**: The position gate is `(TargetX/4, TargetY/4) == (261, 422)` (`:2765`), a single 4×4 map cell. The caller must be party leader and the rectangular area `(774,3593)-(892,3702)` must contain no player (`GetUserInArea`, `:2781-2789`).

The branch then sets the **global** instance state `CartaTime = 60` and `CartaSala = 1`, teleports the leader and party to `CartaPos[0]` and sends `_MSG_StartTime = 120`. It then deletes every mob in the rectangle `x 767..896, y 3582..3711` (`:2808-2819`) - a hard-coded wipe that includes any mob whose id is `>= MAX_USER`, i.e. all non-players.

Room selection uses `cartaindex = item->sIndex - 3171`: `1` opens the N room (mob range `SECRET_ROOM_N_INITIAL..END` twice each plus one boss), `0` opens the M room, anything else opens the A room (`:2821-2853`). The boss roll is `rand() % 1`, which is always 0 - the "random boss" is a single fixed boss.

`CartaTime`/`CartaSala` are decremented by the minute timer (`ProcessSecMinTimer.cpp:1132-1167`) which advances through four stages; `MobKilled.cpp:1094-1350` also advances them on boss kills. Any second party using a card while a run is active overwrites the globals.

**Rule workflow**:
```
if (cell != (261,422)) { msg; refund; } if (Leader) { msg; refund; }
if (GetUserInArea(774,3593,892,3702)) { msg; refund; }
CartaTime=60; CartaSala=1; teleport self+party to CartaPos[0]; StartTime 120;
delete all mobs in [767..896]x[3582..3711];
rooms by sIndex-3171 (1=N, 0=M, else A) + boss; consume; log;
```

---

#### F9 - Hunting scroll

**Overview**: Vol 195 maps six scroll indexes to six 10-entry coordinate tables and teleports to the entry selected by `WarpID`.

**Detailed description**: The table `HuntingScrolls[6][10][2]` is declared at the top of the file (`:20-39`) with six named regions in comments: Armia, Dungeon, SubMundo, Kult, Kefra and Nipple. Each row has ten coordinates, addressed by `m->WarpID - 1`.

Two bounds checks precede the teleport: `sIndex` must be in `3432..3437` and `WarpID` must be in `1..10`, otherwise the function returns **without consuming and without a message** (`:2869-2873`). This is the only branch that consumes the packet field `WarpID`; a client that sends 0 silently loses the action.

The teleport itself does not check the map attribute or combat state, so a hunting scroll works anywhere, including inside another instance. The item is consumed and the log records index, warp id and destination.

**Rule workflow**:
```
if (sIndex not in 3432..3437) return; if (WarpID not in 1..10) return;
DoTeleport(conn, HuntingScrolls[sIndex-3432][WarpID-1][0], [...][1]);
consume; log(index, warpid, x, y);
```

---

#### F10 - Territory entry ticket

**Overview**: Vol 188 teleports into one of three territory areas, gated by class master.

**Detailed description**: `territorio = sIndex - 4111` selects the destination: 0 → (3639,3639), 1 → (3782,3527), 2+ → (3911,3655), each with ±3 jitter (`:4095-4102`).

The class gate is split: indexes 4111/4112 (`territorio <= 1`) require MORTAL or ARCH; indexes 4113+ require CELESTIAL, SCELESTIAL or CELESTIALCS (`:4084-4093`). Mismatches are refunded silently (no message), so a mortal holding a celestial territory ticket gets no feedback.

There is no level, guild, party or timer check, and no occupancy check: two players using the same ticket land on the same cell. The item is consumed after the teleport and logged with the territory index.

**Rule workflow**:
```
territorio = sIndex-4111;
if (territorio<=1 && ClassMaster not in {MORTAL,ARCH}) refund;
else if (territorio>=2 && ClassMaster not in {CELESTIAL,SCELESTIAL,CELESTIALCS}) refund;
teleport to per-territory point + jitter; consume; log;
```

---

#### G1 - Pet ration (mount feeding)

**Overview**: Vol 15 feeds the mount in slot 14: +5000 HP (cap 30000) and +2 feed (cap 100), but only when the ration family matches the mount family.

**Detailed description**: The destination must be exactly `DestType == 0 && DestPos == 14` (`:1436-1440`); any other destination refunds silently, which is the correct UX for "drop the ration on the pet". The mount must exist in the 2330-2389 range (`:1444-1448`).

The family mapping is the heart of the rule. `mount = (dest->sIndex - 2330) % 30` is normalised to seven families: 0-5 (and 6-15, 27) collapse to 6, 19 → 7, 20 → 8, 21/22/23/28 → 9, 24/25/26 → 10, 29 → 19 (`:1450-1468`). The ration side computes `racid = sIndex >= 3367 ? sIndex - 3367 : sIndex - 2420` and `racao = racid % 30`. Matching families allow the feed.

Feeding adds 5000 to `stEffect[0].sValue` capped at 30000, increments `stEffect[2].cEffect` by 2 capped at 100, then calls `MountProcess` for a baby/mount and `ProcessAdultMount` for an adult, plays sound effect 270, resends the mount and consumes. A mount with `sValue == 0` (dead/unsummoned) is not fed and the ration is refunded (`:1476`, `:1506`).

**Rule workflow**:
```
if (DestType!=0 || DestPos!=14) refund; if (mount idx not in 2330..2389) refund;
mount family = normalise((idx-2330)%30); ration family = (racid)%30;
if (family mismatch) { msg; refund; }
if (HP <= 0) refund;
HP = min(HP+5000, 30000); feed = min(feed+2, 100);
MountProcess / ProcessAdultMount; sound 270; SendItem; consume; log;
```

---

#### G2 - Pet essence (mount levelling)

**Overview**: Vol 16 sets the mount to full HP and feed, adds one level (ten on `LOCALSERVER`), and evolves the mount at its family threshold.

**Detailed description**: The gates are: destination must be slot 14, mount index 2330-2389, and mount HP > 0 (`:1522-1534`). Family matching is simpler than G1: `mountIndex = (idx-2330)%30` with 28→21 and 27→10 remapped, compared against `amgIndex = (essence-2390)%30` (`:1536-1550`). Mismatch refunds with "Mount Not Match".

On success it sets HP to 20000 and feed to 100 (`:1552-1553`), then handles adults specially: an adult at level >= 120 is refused ("Cant Upgrade More") and refunded. For adults below 120, `BASE_GetGrowthRate` (which reads `g_pSancRate[2][level/10]`) is rolled against `rand()%101`; failure emits the fail message, consumes the essence, and has a 1-in-5 chance to **subtract** one level (`:1590-1591`).

Success adds `levelUP` (1, or 10 when `LOCALSERVER != 0`, `:1603-1604`) to the level and sets `stEffect[2].cValue = 1`. Evolution is then tested: 2330 at level 25, 2331 at 50, 2332-2359 at 100 (`:1610-1612`). On evolution the index advances by 30, the life value becomes `rand()%20 + level`, and level/feed are zeroed.

**Rule workflow**:
```
if (slot != 14 || mount invalid || HP<=0) refund;
if (family mismatch) { msg; refund; }
HP=20000; feed=100;
if (adult && level>=120) { msg; refund; }
if (adult) { if (rand()%101 > growthRate) { fail; consume; maybe level--; return; } }
level += LOCALSERVER ? 10 : 1;
if (threshold reached) { idx += 30; life = rand%20 + level; level = 0; feed = 0; msg; MountProcess; }
emotion; MountProcess/ProcessAdultMount; SendItem; consume;
```

---

#### G3 - Mount catalyst (forced evolution)

**Overview**: Vol 94 advances any baby mount by 30 indexes (into adulthood) once, if the catalyst family matches.

**Detailed description**: The destination must be slot 14, the mount must be a baby (`idx - 2333` mapped into seven families), and the catalyst `sIndex - 3344` must resolve to the same family (`:4876-4908`). The mapping is identical to G1's, so a catalyst is the "family key" of a baby mount.

The transformation is unconditional once the family matches: `dest->sIndex += 30`, `stEffect[1].cValue = rand()%20 + stEffect[1].cEffect`, `stEffect[1].cEffect = 0`, `stEffect[2].cValue = 0`, then "Mount Growth", `MountProcess`, resend and consume (`:4910-4936`).

Two oddities: the branch emits the **failure** emotions (15 with face, 20 without) on this success path (`:4921-4926`), and it does not cap the level or check whether the mount is already adult - an adult would compute `mount` in the 27+ range, fail the family comparison and be refunded.

**Rule workflow**:
```
if (DestType!=0 || DestPos!=14) { msg; refund; }
mount family = normalise(idx-2333); catalyst family = sIndex-3344;
if (mismatch) { msg; refund; }
idx += 30; life = rand%20 + level; level = 0; feed-kills = 0;
msg; MountProcess; emotion(15/20); SendItem; consume;
```

---

#### G4 - Mount restorer (life repair)

**Overview**: Vol 93 restores 1-2 life points on an adult mount whose life is between 6 and 49 and whose HP is above zero.

**Detailed description**: The mapping is the adult variant (`idx - 2363`, family vs `sIndex - 3351`) with the same seven-family normalisation (`:4963-4995`). The additional gate is `stEffect[1].cValue >= 50 || <= 5 || stEffect[0].sValue <= 0` → reject and refund (`:4996-5002`), i.e. life must be in the open interval (5,50) and the mount must be alive.

The effect is `dest->stEffect[1].cValue += rand() % 2 + 1`, so +1 or +2 life per restorer (`:5003`). There is no cap applied on write, but the pre-check at 50 makes overflow impossible in practice. The branch then logs, emits emotion (success emotions 14/3 here, unlike G3's failure emotions), resends the mount and consumes.

The economy reading is that mount life is consumed by combat and can only be restored while the mount is neither fresh (<=5) nor nearly new (>=50) - a deliberately narrow band.

**Rule workflow**:
```
if (slot != 14) { msg; refund; }
if (family mismatch) { msg; refund; }
if (life >= 50 || life <= 5 || HP <= 0) { log; refund; }
life += rand()%2 + 1; emotion(15/20); SendItem; consume;
```

---

#### H1 - Sephira skill books

**Overview**: Vol 31-38 teaches one of eight "sephira" skills permanently, encoded as a bit in `MOB.LearnedSkill`.

**Detailed description**: The skill bit is `1 << (Vol - 7)`, giving bits 24..31 (`:1798`) - the four high bits of the 32-bit `LearnedSkill` long. Because `LearnedSkill` is declared `long` (`Basedef.h:461`), a shift into bit 31 is implementation-defined/UB in C but yields `0x80000000` on MSVC, which is exactly what the celestial class code expects (`Ideal Stone` writes `1073741824` = bit 30).

Two refusals: `ItemID` outside `1..MAX_ITEMLIST` (an impossible state for a resolved item, so this is defensive) and a set bit, which refunds with "Already Learned It" (`:1800-1805`). On success the bit is OR-ed in, the client receives `_SN_Learn_Sephera` with the item's name, `SendEtc` refreshes the skill list, and `SetAffect(conn, 44, 20, 20)` applies spell 44 (a short visual/learning buff) with 20 % of its nominal duration.

This is the only branch that grants skills, and it bypasses all class/level prerequisites: any class can learn any sephira bit provided it holds the book.

**Rule workflow**:
```
bit = 1 << (Vol-7);
if (LearnedSkill & bit) { refund; msg; }
LearnedSkill |= bit; msg(item name); SendEtc; SetAffect(44,20,20); consume; log;
```

---

#### H2 - Kingdom contracts (summons)

**Overview**: Vol 41-49 summons one of nine kingdom guardians, restricted by clan (7 or 8) and to castle-war time.

**Detailed description**: `Evock = Vol - 41` selects the summon id `Evock + 28` (`:1830`, `:1862`). The clan gate is two-sided: clan 7 (blue) cannot use contracts 46-49 and clan 8 (red) cannot use contracts 41-45 (`:1839-1851`), so each kingdom has exactly four exclusive summons plus a shared fifth. Characters in any other clan (including clan 0) are rejected by the first test.

`CastleState == 0` rejects the use entirely (`:1854-1859`): contracts only work while a castle siege is running. `CastleState` is a global set by the castle scheduler (`Server.cpp:6225-6286`) and by the `castle` admin command (`imple.cpp:473`).

`GenerateSummon(conn, id, NULL, 1)` is called with `Num = 1`, so exactly one guardian is created, at the caster's position, subject to `GetEmptyNPCMob()` availability and the summon-list conflict checks in `Server.cpp:2572-2660`. The return value is ignored, so a failed summon still consumes the contract.

**Rule workflow**:
```
Evock = Vol-41;
if (Clan not in {7,8}) { msg; refund; }
if (Evock<5 && Clan==8) { msg; refund; } if (Evock>=5 && Clan==7) { msg; refund; }
if (CastleState == 0) { msg; refund; }
GenerateSummon(conn, Evock+28, 0, 1); consume; log;
```

---

#### H3 - Quest experience items

**Overview**: Vol 191 grants coin and experience per quest tier, with per-class level windows, a daily log, and a 10 % party share.

**Detailed description**: `thisQuest = item->sIndex - 4117` indexes three 5-entry tables: `QuestLevel[5][4]`, `QuestExp[5][2]` and `QuestCoin[5]` (`CReadFiles.cpp:46-64`, overridable from `../../Common/Settings/QuestsRate.txt` keys `EXP`/`COIN`/`LEVEL`). Columns 0/1 are the mortal min/max level and 2/3 the arch/celestial window; `QuestExp[q][0]` is mortal and `[1]` arch.

The class gate requires MORTAL or ARCH (`:2237-2242`); note this excludes the celestial family from quest items even though the table has a second window. The level window is half-open: `Level < min || Level >= max` (`:2248`).

Rewards: `Coin += QuestCoin[q]` then clamped to 2,000,000,000; `Exp += QuestExp`; and the daily log (`extra.DayLog.Exp`) is reset when `tm_yday` changes and then incremented - a per-day exp tracker used elsewhere for anti-farm limits (not enforced here). `CheckGetLevel() == 4` triggers `SetCircletSubGod`, the level message, rescore and `DoItemLevel`.

Party sharing is the most intricate part. The leader is resolved as `Leader > 0 && Leader < MAX_USER ? Leader : conn`; if the leader is a different, MORTAL/ARCH character below level 350 with `OnlyTrade == 0`, it receives `QuestExp/10`. Then every player in the leader's `PartyList` with the same filters receives `QuestExp/10` as well - including characters who are not in the same area, and including the leader a second time if the leader also appears in its own party list (a duplicate-credit risk, confidence medium).

**Rule workflow**:
```
if (ClassMaster not in {MORTAL,ARCH}) { msg; refund; }
q = sIndex-4117; check Level within [min,max) for the class;
Coin = min(Coin + QuestCoin[q], 2e9); Exp += QuestExp[q]; update DayLog;
msg; emotion; if (CheckGetLevel()==4) { SetCircletSubGod; msg; SendScore; DoItemLevel; }
SendEtc;
leader = valid Leader ? Leader : conn;
if (leader != conn && eligible) { leader.Exp += QuestExp/10; DayLog; msg; emotion; level-check; }
for each PartyList member: if valid, playing, != conn, eligible -> same 10 % grant;
consume; log;
```

---

#### H4 - Molar do Gargula

**Overview**: Vol 194 is a one-shot mortal quest item that raises every worn armour piece below +6 to +6.

**Detailed description**: Three gates: `ClassMaster == MORTAL`, `CurrentScore.Level` in `[199, 254)`, and `QuestInfo.Mortal.MolarGargula == 0` (`:2387-2406`). Each failure has its own message ("Level Limit 2", "Level Limit", "Youve Done It Already") and refunds the item.

The reward loop iterates equipment slots 1..5 (armour, shield, legs, gloves, boots) and, for each non-empty slot whose sanc is below 6, writes sanc 6 and resends that slot (`:2408-2420`). Slots 0 (face), 6/7 (weapons), 8..15 are deliberately excluded, so the item is an armour-only floor.

After the loop the flag is set, the score is recomputed and a quest-completion message is sent. The audit line records the flag value. Because the flag is in the persisted quest struct, the effect cannot be repeated; the item is consumed.

**Rule workflow**:
```
if (ClassMaster != MORTAL) { msg; refund; } if (Level < 199 || Level >= 254) { msg; refund; }
if (MolarGargula) { msg; refund; }
for (slot 1..5) if (item && sanc < 6) { sanc = 6; SendItem; }
MolarGargula = 1; GetCurrentScore; msg; consume; log;
```

---

#### H5 - Ideal stone (ARCH → CELESTIAL)

**Overview**: Vol 211 converts an arch character into a celestial, resetting stats and granting a tiered helmet.

**Detailed description**: The outer gate requires `ClassMaster` to be ARCH or CELESTIAL (`:2891-2895`); for ARCH the further requirements are level >= 355, `Equip[1].sIndex == 0` (helmet must be removed, else "Cant with armor") and `QuestInfo.Arch.MortalLevel >= 99` (`:2897-2910`).

The conversion computes a `ptlevel` band from the level (355-369 → 1, 370-379 → 2, 380-397 → 3, 398 → 4, 399+ → 5) and stores it in `QuestInfo.Celestial.ArchLevel`. It then writes a helmet by level band (`<380 → 3500`, `<399 → 3501`, `<400 → 3502`), sets `ClassMaster = CELESTIAL`, and rebuilds the score block from `BaseSIDCHM[MOB.Class][0..5]`: level 0, Str/Int/Dex/Con from the class table, all four Specials 0, `Ac = 230`, `Damage = 0`, HP/MP = the class base, `SpecialBonus = 855`, and `LearnedSkill = 1073741824` (bit 30 only, wiping all other skills) (`:3010-3033`).

It also rewrites `Equip[15]` with the clan cloak (3197/3198/3199), stamps `Equip[0]` effects `(98,3)` and `(106, sIndex)` - the visual "god" markers - sends the blessing message, consumes, and finally calls `CharLogOut` followed by `_MSG_SendArchEffect` (`:3063-3065`). The logout is required because the client must re-enter with the new class and silhouette.

**Rule workflow**:
```
if (ClassMaster not in {ARCH,CELESTIAL}) refund;
if (ARCH && (level<355 || Equip[1] || MortalLevel<99)) { msg; refund; }
ptlevel = band(level); Equip[1] = 3500|3501|3502; ArchLevel = ptlevel; ClassMaster = CELESTIAL;
BaseScore = class table with level 0, Ac 230, Damage 0; SpecialBonus = 855; LearnedSkill = bit30; Exp = 0;
Equip[15] = clan cloak; Equip[0] effects 98/106; msg; consume; log; CharLogOut; SendArchEffect;
```

---

#### H6 - Ideal stone (CELESTIAL → CELESTIALCS)

**Overview**: The same stone, used by a CELESTIAL at level 120+ with a tier-1 helmet, snapshots the character into `SaveCelestial[1]` and switches to `CELESTIALCS`.

**Detailed description**: The branch requires `Equip[11].sIndex` in `1760..1763` (the four "god" helmets) and level >= 120 (`:2912-2922`). It then sets `ClassMaster = CELESTIALCS` and writes the class byte as `Equip[11].sIndex - 1760` into `SaveCelestial[1].Class`.

The snapshot fills `SaveCelestial[1]` with the current `BaseScore` (level forced to 0, all attributes from `BaseSIDCHM[MOB.Class]`, specials 0, `Ac = 230`, `Damage = 0`, HP/MP from the helmet's row), `LearnedSkill & 0xFF000000` (only the high byte of skills survives), `SpecialBonus = 855`, `Exp = 0`, and `Soul`. `Equip[11]` is then cleared and resent (`:2924-2963`).

This is a *destructive* snapshot: the live character is not reset here (unlike H5), but the helmet is removed and the saved copy is what a later Mystic stone swap (E16) will restore. The item is consumed and the log records the conversion.

**Rule workflow**:
```
if (ClassMaster == CELESTIAL) {
   if (Equip[11] not in 1760..1763 || level < 120) refund;
   ClassMaster = CELESTIALCS; SaveCelestial[1] = base+helmet-derived snapshot;
   clear Equip[11]; consume; msg; return;
}
```

---

#### H7 - Arch crystals (sequential arch quests)

**Overview**: Vol 187 consumes four crystal items in strict order, each granting fixed stats and subtracting 100,000,000 experience.

**Detailed description**: `quest = item->sIndex - 4106 + 1` gives 1..4 for indexes 4106..4109 (`:3268`). Three ordering rules are enforced: the same quest cannot be repeated, `Cristal >= quest` refuses (no going back), and `Cristal != quest - 1` refuses with "Need Before Quest" (`:3270-3289`). The gate also requires ARCH and level >= 355.

Rewards are quest-specific: 1 → `MaxMp += 80`; 2 → `Ac += 30`; 3 → `MaxHp += 80`; 4 → `MaxHp += 60, MaxMp += 60, Ac += 20` (`:3293-3307`). Then `Exp -= 100000000` (unconditional, and not clamped at zero) and the level is recomputed by scanning `g_pNextLevel[0..MAX_LEVEL]` for the band containing the new exp - a linear scan that overwrites `BaseScore.Level` with the last matching index.

The score is then rebuilt (`CheckGetLevel`, `SetCircletSubGod` if a level was crossed, `BASE_GetBonusScorePoint`, `GetCurrentScore`, `SendEtc`, `SendScore`) and the crystal is consumed. Because `Exp` can go negative, the scan finds no band and `newlevel` stays 0 - a character can be reset to level 0 by using crystals while poor.

**Rule workflow**:
```
if (ClassMaster != ARCH) refund; if (level < 355) { msg; refund; }
q = sIndex-4106+1; if (Cristal == q || Cristal >= q) { msg; refund; }
if (Cristal != q-1) { msg; refund; }
Cristal = q; apply stat bonus by q; Exp -= 1e8; recompute level by table scan;
CheckGetLevel; SetCircletSubGod if 4; GetBonusScorePoint; GetCurrentScore; SendEtc; SendScore;
emotion; msg; consume; log;
```

---

#### H8 - Fury stone (three-mode quest item)

**Overview**: Index 3020 is a multi-purpose item: celestial "Arcana" quest, celestial reset, or mortal level-90 quest, selected by class and level.

**Detailed description**: Mode 1 - celestial class at level >= 199 (`:3350`): requires `Fame >= 500`; if `QuestInfo.Circle == 0`, requires one each of items 5334/5335/5336/5337 (water/earth/sun/wind) in the inventory, removes exactly one of each, subtracts 500 fame, rolls `rand()%115` normalised and, on `< 30`, writes `Equip[1].sIndex = 3507`, sets `Circle = 1` and announces "Cythera Arcana" (`:3358-3452`). On failure it announces failure but still consumes the stone and the elements.

Mode 2 - `Circle != 0` and `Celestial.Reset < 3` (`:3457`): increments `Reset`, subtracts 500 fame and 20,000,000 exp, recomputes the level by scanning `g_pNextLevel_2` over `MAX_CLEVEL`, and rescoring (`:3459-3499`). Note this mode does **not** require the four elements.

Mode 3 - any other case (`:3502-3574`): requires `ClassMaster == CELESTIAL` (else refund), level >= 89, `Lv90 == 0`, and `Fame >= 500`; subtracts 500 fame and, on `rand()%115 < 60`, sets `Lv90 = 1`, announces "Lv90" and puts item 3502 into the first free inventory slot via `PutItem`. Failure only announces.

The mode selection is purely positional: the first `if` requires celestial && level>=199, so a celestial at level 100 falls to mode 2 only if `Circle != 0`, otherwise mode 3 and then fails the `ClassMaster == CELESTIAL`... which is true for CELESTIAL, so it runs the Lv90 quest.

**Rule workflow**:
```
if (celestial && level>=199) {
   if (Fame<500) refund;
   if (Circle==0) { require 4 element items; remove them; Fame-=500; roll<30 ? (Equip[1]=3507; Circle=1; msg) : fail-msg; consume; return; }
   else if (Circle && Reset<3) { Reset++; Fame-=500; Exp-=2e7; recompute; consume; return; }
}
if (ClassMaster != CELESTIAL) refund; if (level<89) { msg; refund; } if (Lv90) { msg; refund; }
if (Fame<500) refund; roll = normalise(rand()%115); Fame -= 500;
if (roll<60) { Lv90=1; msg; prize 3502 via PutItem; } else fail-msg;
consume;
```

---

#### H9 - Divine 7/15/30 day items

**Overview**: Vol 64-66 grant a 7/15/30-day "divine" affect backed by an absolute expiry timestamp.

**Detailed description**: The allocator is `GetEmptyAffect(conn, 34)` and the guard is unusual: it rejects when the allocator returns -1 **or** when the returned slot already has `Type == 34` (`:2089`). Since the allocator returns the existing type-34 slot whenever one exists, an active divine buff always blocks a new item - divine durations cannot be extended, only re-applied after expiry.

The duration is stored twice, deliberately: `Affect[sAffect].Time = 2000000000` (the "infinite" sentinel) and `extra.DivineEnd = now + 60*60*24*8 | *16 | *31` for the three items (`:2096-2111`). The extra day is a grace period, and `SendFunc.cpp:1784-1806` recognises the sentinel and derives the client-visible remaining time from `DivineEnd` in days; when less than one hour remains it switches the affect to a normal 450 ticks so the client can count down.

`BASE_GetHpMp` and `GetCurrentScore` are called because divine typically changes HP/MP multipliers, then the item is consumed. Consumption happens after the affect is installed, so a crash in between duplicates the buff (safe direction).

**Rule workflow**:
```
slot = GetEmptyAffect(conn,34);
if (slot==-1 || Affect[slot].Type==34) { msg; refund; }
time(&DivineEnd); DivineEnd += 8|16|31 days;
Affect[slot] = {Type 34, Level 1, Value 0, Time 2000000000};
BASE_GetHpMp; GetCurrentScore; SendScore; log; consume;
```

---

#### I1 - Extraction tools (six slots)

**Overview**: Items 3021-3026 overwrite the two bonus effect slots of a worn helmet/chest/legs/gloves/boots/weapon with the tool's own payload, if the target is at +12 and matches the tool's level and type.

**Detailed description**: Each of the six branches is a copy of the same 100-line block with a different equipment slot (1..6) and a different `sIndex` (`:5028`, `:5127`, `:5225`, `:5323`, `:5421`, `:5531`). The tool's `EF_ITEMLEVEL` (`extra`) decides the mode: when `extra <= 5` the target must be a mortal/normal item (`MOBTYPE` 0 or 2) whose own `EF_ITEMLEVEL` matches; when `extra > 5` the target must be an arch item (`MOBTYPE == 1`) and, for the helmet only, must be one of 3500..3507.

Every branch requires the target's sanc to be **exactly** `REF_11`, which is the raw encoding `234..237` (logical star 11 / the first gem sub-level), and refuses otherwise with `_DN_SANCREQ` formatted with 11 (`:5051-5057`). It then copies the tool's `stEffect[1]` and `stEffect[2]` (effect+value pairs) verbatim onto the target and resends it (`:5059-5065`).

The boots branch adds a correction after copying: if a copied effect is `EF_DAMAGE` and the boot has a static damage ability, that static value is subtracted from the copied value (`:5458-5462`, and again for slot 2). This is the only branch that adjusts the payload, and it exists because boots have an intrinsic damage stat that would otherwise double-count.

**Rule workflow**:
```
extra = EF_ITEMLEVEL(tool); slot = fixed per index;
if (Equip[slot] empty) refund;
if (extra<=5) { require dest extra in {0,5} and dest extra == extra; require MOBTYPE in {0,2}; }
else { helmet: dest idx in 3500..3507; others: MOBTYPE == 1; }
if (sanc != REF_11) { msg(_DN_SANCREQ,11); refund; }
Equip[slot].eff1 = tool.eff1; Equip[slot].eff2 = tool.eff2; (boots: subtract static damage)
SendItem; log; consume; emotion;
```

---

#### I2 - Paint and remover

**Overview**: Vol 186 writes a colour effect (`116 + (sIndex - 3407)`) into the first free effect slot, or strips it when the index is the remover (3417).

**Detailed description**: The destination must be equipment, in slots 1..5 only (`DestType != 0 || DestPos >= 6 && < 16 || DestPos == 0` rejects everything else, `:3657`), and the target must already have `sanc >= 1` (`:3669`) - painted items must be upgraded first.

`color = item->sIndex - 3407` (`:3675`). When `color == 10` (index 3417, the remover) the handler finds the first effect slot whose `cEffect` is either 0 or outside 116..125 and sets it to `EF_SANC` (43) **without touching its value** (`:3681-3697`). Otherwise it writes `cEffect = 116 + color` into the first slot that is either 0, already in 116..125, or (for the non-remover path) not `EF_SANC` (`:3701-3717`).

The 116..125 window is the same channel `BASE_GetItemSanc` reads, so colours 1..9 are indistinguishable from sanc levels 1..9 to the sanc reader - which is exactly how the client renders "+N" tinted gear. Colours above 9 produce codes 126+, which collide with `EF_NOSANC` (126) and `EF_NOTRADE` (127) - a data-range problem that the item list must avoid (confidence: medium, since the CSV is absent).

On success the handler rescoring, logs, `SendEquip(conn,0)` (full appearance refresh), resends the slot and emits emotion 14.

**Rule workflow**:
```
if (DestType!=0 || (DestPos>=6 && DestPos<16) || DestPos==0) reject;
if (sanc < 1) refund;
color = sIndex-3407;
if (color==10) write EF_SANC into first free effect slot;      // remover, value kept
else write 116+color into first free/overwritable effect slot;
msg success; GetCurrentScore; SendScore; log; consume; SendEquip; SendItem; emotion;
```

---

#### I3 - Grade gems (diamond, emerald, coral, garnet)

**Overview**: Vol 180-183 rewrite the target's `Grade` and sanc gem index, shifting a piece of gear within its four-grade family.

**Detailed description**: All four branches share the same skeleton: destination must be equipment in slots 1..7 (`DestPos >= 8 && < 16` and `DestPos == 0` rejected), the target must be either at star 10+ or of `Grade >= 5` (`sanc < REF_10 && Grade < 5` refuses, `:3792`).

Each gem then applies a grade delta by inspecting `g_pItemList[ItemId].Grade`: diamond (180) moves 6→5, 7→5?... the code is `Grade==6 → -1, 7 → -2, 8 → -3`; emerald (181) `5 → +1, 7 → -1, 8 → -2`; coral (182) `5 → +2, 6 → +1, 8 → -1`; garnet (183) `5 → +3, 6 → +2, 7 → +1` (`:3798-3805`, `:3876-3883`, `:3954-3961`, `:4032-4039`). The net effect is that diamond normalises toward grade 5 and the other three promote toward grade 8, with each stone covering a different slice of the 5..8 range.

The sanc is then rewritten with the gem index 0..3 (`BASE_SetItemSanc(dest, star, gemIndex)` for stars 10..15), where diamond writes 0, emerald 1, coral 2, garnet 3 (`:3809-3825`, `:3887-3903`, etc.). This encodes "which gem colour" into the low two bits of the 230/234/238/242/246/250 byte, and is the mechanism the refine code reads back with `BASE_GetItemGem`.

All four branches recompute the score, log `change jewel success`, consume and resend. None of them rolls a random number: the operation is deterministic, which makes these gems a pure currency for grade manipulation.

**Rule workflow**:
```
if (dest not equip slots 1..7) { msg; refund; }
if (sanc < 10 && Grade < 5) refund;
ItemId +=/-= delta(Grade) per gem colour; dest->sIndex = ItemId;
for star in 10..15: if (sanc == star) BASE_SetItemSanc(star, gemIndex);
GetCurrentScore; SendScore; log; consume; SendItem; emotion;
```

---

#### I4 - Mytril bars (damage, magic, defence, critical)

**Overview**: Vol 235-238 add flat stat values to arch equipment at +10 or better, with per-slot floors and ceilings and an 18 % (or 50 %) success rate.

**Detailed description**: The four branches share a gate set: destination must be equipment, excluding slot 0 and the weapon slots where irrelevant (`DestPos >= 8 && < 16` rejected for damage/magic/crit; `>= 6` rejected for defence, plus slot 0 and 1 for all four). The target must be at `sanc >= REF_10` and `EF_MOBTYPE == 1` (arch) (`:4286`, `:4416`, `:4552`, `:4693`).

Each branch reads the current value with `BASE_GetItemAbilityNosanc` (damage, magic, critical) or scans the effect slots (defence, which must find `EF_AC` or `EF_ACADD2`). The floor/ceiling pair depends on whether the slot is a weapon: damage 12/42 for armour vs 45/81 for weapons 6/7; magic 6/14 vs 24/36; defence 15/40 (AC) or 15/70 (ACADD2); critical 50/90 for non-weapons and **0/0 for weapons**, which makes the critical bar unusable on weapons (any `dam >= 0` fails `dam >= max_add`).

Success rate is a literal `18` for damage/magic/critical and `50` for defence, rolled as normalised `rand()%115`. On success the value is increased by 6/2/5/10 (or 9/4/10/1 on weapon slots) and capped at the ceiling, except critical which caps at 90 only for non-weapons.

Failure is two-tiered: `(rand() % 7) == 0` (1 in 7) subtracts the same increment ("fail--"), otherwise the stat is untouched ("fail"). Both consume the bar, log a distinct verb and emit emotion 14 on the destructive variant or 15/20 on the plain failure.

**Rule workflow**:
```
if (dest invalid slot) { msg; refund; }
if (sanc < 10 || MOBTYPE != 1) refund;
stat = read current (nosanc); [min,max] = per stat and weapon-slot;
if (stat < min || stat >= max) refund;
if (normalise(rand()%115) <= rate) { stat += inc, capped; success; }
else if (rand()%7 == 0) { stat -= inc; "fail--"; } else "fail";
consume;
```

---

#### I5 - Class reroll (Vol 190)

**Overview**: Vol 190 re-rolls the two bonus effects of a normal/mortal item whose `EF_ITEMLEVEL` matches the stone.

**Detailed description**: The destination must be in a **non-equipment** location: the guard `if (!m->DestType || m->DestPos >= 60)` requires `DestType != 0` and `DestPos < 60`, i.e. the inventory (0..59) or storage (0..59) - never the equipment slots and never the last four inventory cells.

The target must have `sanc <= 9` and `MOBTYPE` 0 or 2 (`:4824`), which restricts the operation to mortal/normal gear that has not entered the arch upgrade path. The stone's tier is `replation = sIndex in 4016..4020 ? sIndex-4015 : sIndex-4020` and must equal the target's `EF_ITEMLEVEL` (`:4830-4836`), so each stone tier only works on gear of the same tier.

On success `SetItemBonus2(dest)` (`Server.cpp:2335+`) chooses a random bonus row from `g_pBonusValue2/3/4` based on the item's `nPos` (helmet, chest/legs, gloves, boots...) and writes both bonus effects; it also ensures the first effect slot is the sanc channel, adding `rand()%2` stars when the item is below +6. The item is then resent, emotion 14 emitted, the operation logged and the stone consumed.

Because the reroll is destructive, the "class" (in the sense of bonus table) of an item is not stable: repeated use cycles through bonus pairs within the same tier. There is no confirmation and no preview.

**Rule workflow**:
```
if (DestType == 0 || DestPos >= 60) { msg; refund; }
if (sanc > 9 || MOBTYPE not in {0,2}) refund;
replation = tier(sIndex); if (EF_ITEMLEVEL(dest) != replation) refund;
SetItemBonus2(dest); SendItem; emotion; log; consume;
```

---

#### I6 - PvP jewels

**Overview**: Vol 242 adds one PvP attribute bit to affect 8, refreshable to a one-hour duration.

**Detailed description**: Eight indexes map to `joia` 0..7 (3200, 3201, 3202, 3204, 3205, 3206, 3208, 3209 - note 3203 and 3207 are skipped, because they belong to the storage-recovery jewel family, see I7). The allocator is `GetEmptyAffect(conn, 8)` and an allocation failure refunds with "CantEatMore".

If the slot is not already type 8, the branch writes `Type 8, Level = 1 << joia, Value 0`; if it is, it ORs the bit into `Level` (`:4202-4210`). Either way `Time = AFFECT_1H` (450 ticks = 1 h), so using a second jewel resets the timer for **all** previously accumulated bits.

The bitmask values are therefore 1..128 in `Affect.Level` with the type fixed at 8. `GetCurrentScore` is recomputed so the resistance/attack bonuses apply, and the jewel is consumed. A character can hold all eight bits simultaneously as long as each reuse happens within the hour of the previous one.

**Rule workflow**:
```
joia = index->bit map (0..7);
slot = GetEmptyAffect(conn,8); if (slot==-1) { msg; refund; }
if (Type != 8) { Type=8; Level = 1<<joia; Value=0; } else Level |= 1<<joia;
Time = AFFECT_1H; GetCurrentScore; SendScore; consume; log;
```

---

#### I7 - Storage-recovery jewel

**Overview**: Vol 243 / index 3203 removes a fixed set of negative affect types.

**Detailed description**: The branch acts only when `item->sIndex == 3203`; any other index carrying volatile 243 is still consumed but has no effect. It scans all 32 affect slots and clears those whose `Type` is one of 1, 3, 5, 7, 10, 12, 20, 32 (`:4237-4242`), setting a flag if anything was cleared.

If the flag is set the score is recomputed and pushed, because these affect types are stat modifiers (speed, defence, shields). The item is then consumed and logged with the standard `joia_pvp joia:%d` text - a copy-paste from I6 that mislabels the audit trail.

The hard-coded type list is a policy decision that cannot be configured: new debuff types are not removed until this list is edited.

**Rule workflow**:
```
if (sIndex == 3203) { for each affect: if Type in {1,3,5,7,10,12,20,32} clear; if changed rescore; }
consume; log;
```

---

#### I8 - Silver bars and war pack (currency)

**Overview**: Vol 185 converts six bar indexes into 1M..1G coin, refusing when the result would exceed 2,000,000,000.

**Detailed description**: The mapping is `4010 → 100,000,000`, `4011 → 1,000,000,000`, `4026 → 1,000,000`, `4027 → 5,000,000`, `4028 → 10,000,000`, `4029 → 50,000,000`; any other index yields `gold = 0` (`:4121-4137`) and the bar is consumed for nothing.

The addition is computed in an `unsigned int` (`fGold = Coin + gold`) and compared against 2,000,000,000 **before** assignment (`:4139-4146`); if it exceeds, the client is told `_NN_Cant_get_more_than_2G` and the bar is refunded. Otherwise `Coin = fGold`, `SendEtc` refreshes the currency display, emotion 14 is emitted, the bar is consumed and the log records both the value and the new total.

Because the check uses an unsigned sum, a `Coin` already above 2e9 (possible through quest items, which clamp at exactly 2e9) cannot wrap unless `Coin + gold` overflows 2^32 - which the 2e9 pre-check prevents. The rule is therefore safe but relies on the caller never exceeding the cap elsewhere.

**Rule workflow**:
```
gold = map(sIndex); fGold = (unsigned)(Coin + gold);
if (fGold > 2000000000) { msg; refund; }
Coin = fGold; SendEtc; emotion; log(value,total); consume;
```

---

### 3.3 Cross-cutting observations

1. **Nine branches mutate the character's fundamental identity** (`ClassMaster`, `Class`, `BaseScore`, `LearnedSkill`, `SaveCelestial`) - H5, H6, H7, H8, E16, E4, I3, I5, D10. Only H5 ends with a forced logout; the others rely on the client tolerating an in-place identity change.
2. **Consumption is used as a success flag in some branches and as an unconditional cost in others.** Refines always consume; water/nightmare/duel tickets consume on success; quest items consume on success; a handful of branches refund deliberately (Pilula when already used, Crescent eye for non-mortals, bag when full, courage when affect 30 is occupied).
3. **Twelve different random distributions** are used: `rand()%100` with `<=`, `rand()%115` normalised with `<=` or `<`, `rand()%101` with `>`, `rand()%7`, `rand()%4`, `rand()%2`, `rand()%5`, `rand()%20`, `rand() & 0x80000003`, `rand()%1` (always 0), and `rand()%10`. The normalisation `if (x > 100) x -= 15` maps 0..114 onto 0..100 but **not uniformly** (values 86..100 are twice as likely), which inflates every rate that uses it by roughly 15 % relative.
4. **Three global instance-state families** (`WaterClear1`, `PartyPesa`, `Carta*`) are shared by all players and have no ownership, reference count or reset on crash.

---

## 4. Component Structure

### 4.1 File layout

```
Code/TMSrv/
├── _MSG_UseItem.cpp        # 5,726 lines - the entire component
│   ├── :1-17               # GPL header
│   ├── :18                 # #include "ProcessClientMessage.h"  (single include; 14 subsystems via aggregate)
│   ├── :20-39              # int HuntingScrolls[6][10][2]  (only file-scope data; 6 regions x 10 warps)
│   └── :41-5726            # void Exec_MSG_UseItem(int conn, char *pMsg)
│       ├── :43-95          #   prologue: cast, gates, resolve source, read Vol/amount
│       ├── :97-5724        #   68 branch regions (106 volatile codes + 12 index keys)
│       └── :5726           #   implicit end (no default branch)
├── ProcessClientMessage.h  # :75  declaration of Exec_MSG_UseItem
└── ProcessClientMessage.cpp# :210-212 dispatch entry
```

### 4.2 Internal organisation

The function has no helpers, no macros, no static locals and no error type. Its only structure is the sequence of `if (...) { ... return; }` blocks, each delimited by a `#pragma region` comment naming the item family. Because every branch returns, the regions behave as `case` labels; because they are evaluated in textual order, moving a region changes behaviour.

| Region block | Lines | Size | Vol / key | Returns |
|---|---|---|---|---|
| POT HP / MP | 97-148 | 52 | 1 | 1 |
| PO / PL (incl. REFINE_LOCKED_SEAL, celestial, arch stones, earring) | 149-976 | 828 | 4, 5 | 18 |
| Pilula Orc | 977-997 | 21 | 6 | 1 |
| Poeira de Fada | 998-1043 | 46 | 7 | 1 |
| Olho Crescente | 1044-1078 | 35 | 8 | 1 |
| Adamantita/beril (LE) | 1079-1196 | 118 | 9 | 6 |
| KAPPA_POTION | 1197-1299 | 103 | 10,52,53,55,56,57,200,201,202 | 1 |
| Retorno | 1300-1315 | 16 | 11 | 1 |
| Gema Estelar | 1316-1376 | 61 | 12 | 5 |
| Portal | 1377-1409 | 33 | 13 | 3 |
| DBRecord | 1410-1432 | 23 | 14 | 1 |
| PET_RATION | 1433-1518 | 86 | 15 | 6 |
| PET_ESSENCE | 1519-1642 | 124 | 16 | 6 |
| GUILDBOARD | 1643-1664 | 22 | 17 | 2 |
| Fogos de Artificio | 1665-1680 | 16 | 19 | 1 |
| Pergaminho da agua M | 1681-1785 | 105 | 21-30 | 5 |
| Livros Sephira | 1786-1826 | 41 | 31-38 | 4 |
| Contratos do reino | 1827-1874 | 48 | 41-49 | 5 |
| Pergaminho da agua N | 1875-1979 | 105 | 131-140 | 5 |
| Pergaminho da agua A | 1980-2083 | 104 | 161-170 | 5 |
| Divina 7/15/30 | 2084-2129 | 46 | 64-66 | 2 |
| Vigor 1 Hora | 2130-2163 | 34 | 58 | 2 |
| Bau de XP | 2164-2198 | 35 | 198 | 2 |
| Frango Assado | 2199-2233 | 35 | 63 | 2 |
| Itens de quests | 2234-2383 | 150 | 191 | 5 |
| Molar do Gargula | 2384-2438 | 55 | 194 | 4 |
| Pesadelo N | 2439-2533 | 95 | 173 | 7 |
| Pesadelo M | 2534-2634 | 101 | 174 | 7 |
| Pesadelo A | 2635-2757 | 123 | 175 | 7 |
| Carta de Duelo | 2758-2865 | 108 | 20 | 5 |
| HUNTING_SCROLL | 2866-2887 | 22 | 195 | 3 |
| Pedra Ideal | 2888-3069 | 182 | 211 | 9 |
| Pedra Misteriosa | 3070-3176 | 107 | idx 4148 | 4 |
| Escritura do Pesadelo | 3177-3210 | 34 | 212 | 2 |
| Selo do Guerreiro | 3211-3250 | 40 | idx 4146 | 1 |
| Cristal (arch quests) | 3251-3346 | 96 | 187 | 6 |
| Pedra da Furia | 3347-3576 | 230 | idx 3020 | 10 |
| COURAGE_MEDICINE | 3577-3610 | 34 | idx 4046/646 | 2 |
| Elixir da Coragem | 3611-3644 | 34 | 230 + idx 647/3378 | 2 |
| REMOVE_MAGIC_BEAN | 3645-3741 | 97 | 186 | 6 |
| FORGIVENESS_SCROLL | 3742-3766 | 25 | 203 | 1 |
| Gema (diamond/emerald/coral/garnet) | 3767-4078 | 312 | 180-183 | 5 each |
| Entrada do Territorio | 4079-4115 | 37 | 188 | 3 |
| Barra de Prata | 4116-4163 | 48 | 185 | 2 |
| PVP_JEWELS | 4164-4226 | 63 | 242 | 3 |
| STORAGE_RECOVERY_JEWELS | 4227-4260 | 34 | 243 | 1 |
| Barra Mytril (Dano/Magia/Defesa/Critico) | 4261-4798 | 538 | 235-238 | 5 each |
| Classe | 4799-4853 | 55 | 190 | 5 |
| Catalizadores | 4854-4940 | 87 | 94 | 5 |
| Restaurador | 4941-5026 | 86 | 93 | 5 |
| Helm/Chest/Legs/Gloves/Boots/Weapon extraction | 5027-5627 | 601 | idx 3021-3026 | 6 each |
| Selo das Almas | 5628-5662 | 35 | 206 | 2 |
| Bolsa do Andarilho | 5663-5706 | 44 | idx 3467 | 3 |
| War Pack | 5707-5725 | 19 | 184 | 1 |

### 4.3 Volatile dispatch map (item category → branch)

| Vol | Category | Branch behaviour | Consumes |
|---|---|---|---|
| 1 | HP/MP potion | adds to ReqHp/ReqMp, throttled | yes |
| 4 | PO refine stone | refine to +6 max, rate table row 0 | yes |
| 5 | PL refine stone | refine to +9 max, rate table row 1, also drives earring path | yes |
| 6 | Orc pill | +9 skill points once | yes (not if already used) |
| 7 | Fairy dust | 50 % level jump | yes |
| 8 | Crescent eye | +2500 exp, mortal only | yes (not for non-mortal) |
| 9 | Legendary stones | 51 % grade→extra promotion | yes |
| 10, 52, 53, 55, 56, 57, 200, 201, 202 | Kappa potions | affect 4 with class value and duration | yes (even if no slot) |
| 11 | Recall scroll | DoRecall | yes |
| 12 | Stellar gem | save SPX/SPY | yes |
| 13 | Portal scroll | teleport to SPX/SPY | yes |
| 14 | DB record ticket | send TCK record to DBSrv | yes |
| 15 | Pet ration | mount feed +5000/+2 | yes |
| 16 | Pet essence | mount HP/feed reset + level | yes |
| 17 | Guild board | open UI | **no** |
| 19 | Fireworks | emotion 100 | yes |
| 20 | Duel card | secret room instance | yes |
| 21-30 | Water scroll M | instance M rooms 0-9 | yes |
| 31-38 | Sephira books | learn skill bit 24..31 | yes |
| 41-49 | Kingdom contracts | summon 28..36 | yes |
| 58 | Vigor | affect 35, 1 h | yes |
| 63 | Roast chicken | affect 30 level 1000, 4 h | yes |
| 64-66 | Divine 7/15/30 | affect 34 + DivineEnd | yes |
| 93 | Mount restorer | +1..2 life | yes |
| 94 | Mount catalyst | baby→adult | yes |
| 131-140 | Water scroll N | instance N rooms 0-9 | yes |
| 161-170 | Water scroll A | instance A rooms 0-9 | yes |
| 173 / 174 / 175 | Nightmare N/M/A | instance teleport + timer | yes |
| 180-183 | Grade gems | grade shift + gem index | yes |
| 184 | War pack | +Donate | yes |
| 185 | Silver bars | +Coin (cap 2e9) | yes |
| 186 | Paint / remover | effect 116+color / EF_SANC | yes |
| 187 | Arch crystals | sequential stat quests, -1e8 exp | yes |
| 188 | Territory entry | teleport | yes |
| 190 | Class reroll | SetItemBonus2 on inventory item | yes |
| 191 | Quest exp items | coin + exp + party share | yes |
| 194 | Molar do Gargula | armour sanc floor +6 | yes |
| 195 | Hunting scroll | teleport by index+warp | yes |
| 198 | XP chest | affect 39 +2 h | yes |
| 203 | Forgiveness scroll | SetPKPoint(150) | yes |
| 206 | Soul seal | capsule capture (SCELESTIAL) | yes (empty seal only) |
| 211 | Ideal stone | ARCH→CELESTIAL / CELESTIAL→CELESTIALCS | yes |
| 212 | Nightmare writ | NT += 13 with 20 h cooldown | yes |
| 230 (+647/3378) | Elixir of courage | affect 30 level 2000 | yes |
| 235-238 | Mytril bars | flat stat add | yes |
| 242 | PvP jewels | affect 8 bitmask | yes |
| 243 | Storage-recovery jewel | clear 8 affect types | yes |
| other | unhandled | nothing | **no** |

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
ProcessClientMessage
  └── Exec_MSG_UseItem
        ├── GetItemPointer ............................ Basedef (bounds-checked slot resolution)
        │     └── g_pItemList[] ....................... item database (6500 entries)
        ├── BASE_GetItemAbility / ...Nosanc ........... Basedef (effect arithmetic; consults g_pItemList)
        │     ├── BASE_GetItemSanc / Gem / SancSuccess  Basedef (sanc encoding 0-9 and 230..253)
        │     ├── BASE_GetItemAmount / SetItemAmount ... Basedef (EF_AMOUNT in effect slots)
        │     └── BASE_SetItemSanc ..................... Basedef (writes 230..253 encoding)
        ├── BASE_GetSuccessRate ── g_pSancRate[][] ── g_pSuccessRate[]
        ├── BASE_GetGrowthRate  ── g_pSancRate[2][]
        ├── BASE_GetBonusItemAbility .................. EF_INCUBATE / EF_INCUDELAY only
        ├── BASE_SetItemDate ────────────────────────── EF_WDAY / EF_WMONTH / EF_YEAR
        ├── CMob::GetCurrentScore ── BASE_GetMobAbility, BASE_GetBonusScorePoint
        │     └── BASE_GetHpMp, BASE_GetBonusSkillPoint
        ├── CMob::CheckGetLevel ── g_pNextLevel[] / g_pNextLevel_2[] / g_pIncrementHp/Mp
        ├── DoTeleport ── GetEmptyMobGrid ── pMobGrid[][]
        ├── DoRecall ── g_pGuildZone[] / GetEmptyMobGrid
        ├── MountProcess ── GenerateSummon / DeleteMob / BASE_GetItemAbility(EF_MOUNTHP)
        ├── ProcessAdultMount ── mSummon.Mob[] / SendEquip / SendItem
        ├── SetItemBonus2 ── g_pBonusValue2/3/4[][]
        ├── SetAffect ── g_pSpell[] / GetEmptyAffect
        ├── DoItemLevel ── LevelItem[class][type][level] / pUser.Cargo
        ├── SetCircletSubGod ── extra.QuestInfo.Celestial.Add120/150/180/200
        ├── GenerateSummon ── mSummon.Mob[] / GetEmptyNPCMob
        ├── GenerateMob / RebuildGenerator ── mNPCGen
        ├── GetUserInArea ── pUser[] / pMob[]
        ├── SetPKPoint ── MOB.Carry[63].stEffect[0]
        ├── GetGuild ── GuildInfo[] / pMob[]
        ├── PutItem ── pMob.Carry[]
        ├── CharLogOut ── DB write path
        └── DBServerSocket.SendOneMessage ── TCP 7514
```

### 5.2 External dependencies (modules)

| Module | Symbols used | Purpose |
|---|---|---|
| `Code/Basedef.cpp` | 20 `BASE_*` functions | item arithmetic, sanc encoding, rates, dates, tables |
| `Code/TMSrv/GetFunc.cpp` | `GetAttribute`, `GetEmptyAffect`, `GetItemPointer` (Basedef), `GetCreateMob`, `SetPKPoint` | map attributes, affect allocation, DTO construction |
| `Code/TMSrv/SendFunc.cpp` | `SendItem`, `SendScore`, `SendEtc`, `SendCarry`, `SendEquip`, `SendClientMessage`, `SendClientSignalParm`, `SendEmotion`, `SendSetHpMp`, `SendHpMode`, `SendNotice` | outbound projection |
| `Code/TMSrv/Server.cpp` | `DoTeleport`, `DoRecall`, `MountProcess`, `ProcessAdultMount`, `GenerateSummon`, `GenerateMob`, `RebuildGenerator`, `SetItemBonus2`, `DoItemLevel`, `SetCircletSubGod`, `PutItem`, `CharLogOut`, `RemoveTrade`, `GetUserInArea`, `CrackLog`, `ItemLog`, `AddCrackError` | world services |
| `Code/TMSrv/CMob.cpp` | `CMob::GetCurrentScore`, `CMob::CheckGetLevel` | derived stats, level-up |
| `Code/TMSrv/CReadFiles.cpp` | `QuestExp[5][2]`, `QuestCoin[5]`, `QuestLevel[5][4]` | quest rewards (static tables) |
| `Code/TMSrv/CUser.h` | `pUser[]`, `USER_PLAY` | session state |
| `Code/TMSrv/Server.h` | ~20 externs | global world state |
| `Code/TMSrv/Language.h` | ~45 `_NN_*` / `_SN_*` / `_DN_*` / `_DD_*` indices | localised text |
| `Code/Basedef.h` | `STRUCT_ITEM`, `STRUCT_MOB`, `STRUCT_MOBEXTRA`, `STRUCT_AFFECT`, `MSG_UseItem`, `MAX_*`, `REF_*`, `AFFECT_*`, `MORTAL/ARCH/CELESTIAL/CELESTIALCS/SCELESTIAL`, `ITEM_PLACE_*` | contracts and constants |
| `Code/ItemEffect.h` | `EF_*` ids | effect taxonomy |
| `Code/CPSock.cpp` | `pUser[conn].cSock.AddMessage`, `DBServerSocket.SendOneMessage` | TCP 8281 / 7514 |
| Win32 / CRT | `GetTickCount`, `rand`, `srand` (implicit), `time`, `localtime`, `memset`, `memcpy`, `sprintf` | time, randomness, formatting |

### 5.3 External data dependencies

| File | Loader | Used by |
|---|---|---|
| `../../Common/itemlist.csv` → `g_pItemList[6500]` | `Basedef.cpp` loader | every `BASE_*` call, `nPos`, `Grade`, `Extra`, `nUnique`, `Name` |
| `../../Common/Settings/SancRate.txt` | `CReadFiles::ReadSancRate` (`:77-272`) | `g_pSancRate`, `g_pSancGrade` |
| `../../Common/Settings/QuestsRate.txt` | `CReadFiles::ReadQuestsRate` (`:274-373`) | `QuestExp`, `QuestCoin`, `QuestLevel` |
| `gameconfig.txt` | `ReadConfig` / `DrawConfig` (`Server.cpp:1087-1114`) | `PotionDelay`, `maxNightmare` |
| `LevelItem.txt` | `Server.cpp` loader | `DoItemLevel` rewards |
| `Language.txt` | `Basedef` loader | `g_pMessageStringTable[]` |

---

## 6. Afferent and Efferent Coupling

### 6.1 Component-level summary

In this C codebase the "components" are translation units, functions and structs. The UseItem Handler is a single function, so class-style metrics must be projected onto its internal branch families.

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Instability (Ce/(Ca+Ce)) | Criticality |
|---|---|---|---|---|
| `Exec_MSG_UseItem` (whole component) | 1 | 59 functions + 40 globals + 2 socket methods | 0.98 | High |
| `HuntingScrolls[6][10][2]` | 1 | 0 | 0.00 | Low |
| Refine engine (D1-D13) | 0 (internal) | 9 (`GetItemPointer`, `BASE_GetItemSanc`, `SetItemSanc`, `SancSuccess`, `Gem`, `GetSuccessRate`, `ClearItem`, `GetBonusItemAbility`, `GetCurrentScore`) | 1.00 | High |
| Mount family (G1-G4) | 0 (internal) | 6 (`MountProcess`, `ProcessAdultMount`, `GetGrowthRate`, `BASE_GetItemAbility`, `SendClientSignalParm`, `GenerateSummon` indirectly) | 1.00 | High |
| Instance/ticket family (F1-F10) | 0 (internal) | 8 (`DoTeleport`, `GenerateMob`, `RebuildGenerator`, `GetUserInArea`, `DeleteMob`, `SendClientSignalParm`, `time`, `localtime`) | 1.00 | High |
| Class progression (H5-H8, E16) | 0 (internal) | 9 (`GetCurrentScore`, `CheckGetLevel`, `SetCircletSubGod`, `DoItemLevel`, `BASE_GetBonusScorePoint`, `BASE_GetBonusSkillPoint`, `BASE_GetHpMp`, `CharLogOut`, `GetGuild`) | 1.00 | High |
| Extraction tools (I1) | 0 (internal) | 5 (`BASE_GetItemAbility`, `GetSanc`, `GetStaticItemAbility`, `SendItem`, `ItemLog`) | 1.00 | Medium |
| Simple consumables (E1-E15) | 0 (internal) | 7 (`GetEmptyAffect`, `SendScore`, `PutItem`, `SetPKPoint`, `SendEmotion`, `SendEtc`, `ItemLog`) | 1.00 | Medium |

### 6.2 Afferent detail

There is exactly one incoming dependency, at `Code/TMSrv/ProcessClientMessage.cpp:211`. Nothing else calls, includes or links against `Exec_MSG_UseItem`; there is no unit-test caller, no admin caller, no timer caller and no server-injected invocation (unlike `Exec_MSG_Attack`, which is re-entered from `Server.cpp` AI code). This makes the component **trivially isolated at the call-graph level but impossible to isolate at the state level**: it reaches 40 globals directly.

### 6.3 Efferent detail (per callee, with call count)

| Callee | Calls | Notes |
|---|---|---|
| `SendItem` | 227 | every branch resyncs at least one slot |
| `SendClientMessage` | 148 | text feedback |
| `ItemLog` | 119 | audit |
| `memset` | 105 | 68 consumption sites + init |
| `sprintf` | 145 | log/format strings |
| `BASE_GetItemAbility` | 46 | dominant read primitive |
| `BASE_SetItemSanc` | 30 | refine/gem/quest writes |
| `BASE_GetItemSanc` | 21 | gates and logs |
| `GetCurrentScore` | 20 | stat recomputation |
| `SendScore` | 19 | client stat sync |
| `SendEmotion` | 18 | success/failure animation |
| `SendItem`-family writes to equip | ~40 | extraction, gems, mounts, paint |
| `DoTeleport` | 9 | scrolls, instances, territories |
| `BASE_GetItemAmount` / `SetItemAmount` | 2 / 68 | consumption |
| `DoItemLevel` | 5 | level rewards |
| `SetCircletSubGod` | 5 | sub-god bonuses |
| `GetEmptyAffect` | 7 | timed buffs |
| `MountProcess` / `ProcessAdultMount` | 8 / 4 | mount projection |
| `GenerateMob` | 14 | instance spawns |
| `GenerateSummon` | 1 | kingdom contracts |
| `DeleteMob` | 1 | duel-card arena wipe |
| `CharLogOut` | 2 | Ideal stone, soul seal |
| `PutItem` | 1 | Fury stone Lv90 prize |
| `DBServerSocket.SendOneMessage` | 1 | DB record ticket |
| `pUser[conn].cSock.AddMessage` | 2 | guild board, short-skill push |

---

## 7. Endpoints

This component exposes **no network endpoint** - it opens no listener, registers no route and publishes no RPC. Its only inbound contract is a single protocol message consumed through the game dispatcher. The table documents that contract because it is the component's de-facto public interface.

| Operation | Direction | Message | Payload | Transport |
|---|---|---|---|---|
| Use item | client → server | `_MSG_UseItem` (115, both flags) | `MSG_UseItem` (`Basedef.h:1822-1832`): `SourType`, `SourPos`, `DestType`, `DestPos`, `GridX`, `GridY`, `WarpID` | TCP 8281, custom header + obfuscation + 8-bit checksum |

**Field contract as actually enforced**

| Field | Type | Valid range enforced | Used by |
|---|---|---|---|
| `SourType` | `int` | 0 (equip), 1 (carry), 2 (cargo) - via `GetItemPointer` | A5, A3 |
| `SourPos` | `int` | 0..15 / 0..63 / 0..127 for the three types - via `GetItemPointer`; **unchecked** in A3 | A5, A3 |
| `DestType` | `int` | per-branch conjunctions (see D1, I1-I5) | refine, gems, extraction |
| `DestPos` | `int` | per-branch conjunctions | refine, gems, extraction, mounts |
| `GridX` | `u16` | `< 4096`, then unused | A4 only |
| `GridY` | `u16` | `< 4096`, then unused | A4 only |
| `WarpID` | `u16` | 1..10 for hunting scrolls, ignored elsewhere | F9 only |

The packet `Size` field is **not** validated by the handler; `pMsg` is cast and read on the assumption that at least `sizeof(MSG_UseItem)` bytes are present. `CPSock.cpp:397` guarantees only `sizeof(HEADER) <= Size <= 8192`, so a short `_MSG_UseItem` is read past the received bytes.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| W2 game client | Inbound/outbound session | Receive use requests, push item/score/affect updates | TCP 8281 (WinSock 1.1, async select) | packed binary `MSG_*` | none - invalid state returns silently or refunds |
| `DBSrv` (`DBServerSocket`) | Outbound one-way | `_MSG_MessageDBRecord` for Vol 14 tickets | TCP 7514 | packed `MSG_MessageDBRecord` (fixed 4 `int` + record string) | fire-and-forget; item consumed regardless |
| `DBSrv` (capsule) | Outbound one-way | `_MSG_DBPutInCapsule` for the soul seal | TCP 7514 | `MSG_STANDARDPARM` | after `CharLogOut`; no ack |
| Item database (`g_pItemList`) | In-memory table | Item identity, `nPos`, `Grade`, `Extra`, `nUnique`, static effects | loaded from CSV at startup | `STRUCT_ITEMLIST[6500]` | absent/bad index returns 0 from `BASE_*`; `GetItemPointer` rejects `sIndex >= 6500` |
| `SancRate.txt` | Startup configuration | Refine rates and grade bonuses | text (`PO n v`, `PL n v`, `PO_A..E`, `PL_A..E`, `ÁMAGO n v`) | `g_pSancRate[3][12]`, `g_pSancGrade[2][5]` | MessageBox + skip line on invalid index/range |
| `QuestsRate.txt` | Startup configuration | Quest exp/coin/level windows | text (`EXP q v1 v2`, `COIN q v`, `LEVEL q min max min2 max2`) | static arrays of size 5 | MessageBox + skip on out-of-range |
| `gameconfig.txt` | Runtime configuration | `PotionDelay`, `maxNightmare` | text ints, re-read on `Rebuild` | globals | none - defaults 500 / 3 |
| NPC generator (`mNPCGen`) | In-process | Spawn instance mobs, rebuild generator counters | direct calls | `NPCGENLIST` | `GenerateMob` guards index internally; return ignored |
| Summon list (`mSummon.Mob[50]`) | In-process | Kingdom contracts, mounts | direct calls | `STRUCT_MOB` | `GenerateSummon` returns 0 on no free slot; return ignored |
| World grid (`pMobGrid[4096][4096]`) | In-process | Teleports, area occupancy, mob wipe | direct calls | `unsigned short` | `DoTeleport` silently no-ops; `GetUserInArea` scans all 1000 players |
| Affect array (`pMob.Affect[32]`) | In-process | Timed buffs | direct writes | `STRUCT_AFFECT` | `GetEmptyAffect` returns -1; some branches still consume |
| Item log (`./Log/TM_*.txt`) | File (append) | Audit trail | `fprintf` via `ItemLog` | free text with account/IP | no error handling; global `temp` buffer |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / assessment |
|---|---|---|---|
| Command Handler (message router target) | `Exec_MSG_UseItem` invoked from a `switch` on `std->Type` | `ProcessClientMessage.cpp:210` | Uniform handler signature `(conn, pMsg)`; no return value, no error channel |
| Tagged dispatch (volatile tag) | 59 equality tests + 5 range tests over `EF_VOLATILE` | `:97-5724` | Data-driven item taxonomy; ordering is load-bearing and undocumented |
| Fall-through secondary dispatch (index tag) | 12 `item->sIndex ==` / range tests after the volatile chain | `:3071-5724` | Handles items with no volatile class; silent skip if unmatched |
| Chain of Responsibility (implicit) | Each branch returns; no shared post-processing | whole function | Equivalent to a `switch`, but written as sequential guards |
| Template Method (copy-paste variant) | Three water dungeons, four grade gems, four Mytril bars, six extraction tools | `:1682/1876/1981`, `:3768-4078`, `:4262-4798`, `:5027-5627` | Same algorithm, different constants; ~1,500 duplicated lines |
| Value-object mutation in place | `GetItemPointer` returns a pointer into `pMob`/`pUser`; branch writes through it | `:86`, `:152` | Zero-copy, but no rollback and no re-validation |
| Two-phase commit (prototype) | Deferred HP/MP via `ReqHp`/`ReqMp` reconciled by `ApplyHp`/`ApplyMp` | `:114-137`, `Server.cpp:8591-8657` | Smooths client display; introduces a desync window |
| Snapshot / restore | `SaveCelestial[2]` used by Ideal Stone and Mystic Stone | `:2924-2963`, `:3101-3128` | Full character-state swap between class masters |
| Lazy materialisation | sanc effect slot created on first refine | `:238-263`, `:811-837` | Allows 8-byte items to carry an upgrade level without a dedicated field |
| Pity counter embedded in the value | `sanc + 10*failCount` inside a single byte | `Basedef.cpp:2239-2285` | Clever, but invisible to the client and fragile at the 230..253 encoding |
| Data-driven tables | `g_pSancRate`, `g_pSancGrade`, `g_pCelestialRate`, `g_pSuccessRate`, `g_pBonusValue*`, `HuntingScrolls`, `WaterScrollPosition`, `Pesa*PosStandard`, `CartaPos` | `Basedef.cpp:61-104`, `Server.cpp:56-152`, `_MSG_UseItem.cpp:20-39` | Content tuning without recompilation (partially) |
| Singleton-ish global state | `pUser[]`, `pMob[]`, `WaterClear1`, `PartyPesa`, `CartaTime`, `CartaSala`, `CastleState`, `LOCALSERVER` | throughout | Single-process assumptions; prevents multi-instance safety |
| Audit-log side channel | `ItemLog` on almost every branch | 119 sites | Text-only, per-branch format, no schema |
| Guard-clause style | 277 bare `return`s | throughout | Flat control flow; 277 distinct exits to reason about |

**Architectural decisions observable from the code**

1. **One handler per message family, not per item.** The volatile tag is the only abstraction; adding a new item family means editing this 5,726-line function.
2. **Server-authoritative rolls.** All randomness is server-side (`rand()`), and the client only sends intent (source, destination, warp id). No client-provided success flag is trusted.
3. **Item identity drives behaviour, not item instance.** Rules are keyed on `sIndex` and `EF_VOLATILE`, never on a per-item script or a GUID. This makes the item list CSV effectively executable configuration.
4. **Refunds instead of error codes.** Failure paths call `SendItem(SourType, SourPos, item)` to restore the client's optimistic removal, then return. There is no negative acknowledgement.
5. **Instance state as globals.** Dungeons are simulated by three fixed-size arrays plus two scalars, not by instance objects, which caps concurrency at one run per dungeon.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|---|---|---|---|
| High | Dead-character branch (`:69-76`) | Three array indexes from `m->SourPos` with no bounds check; 8-byte read through `SendItem` | Out-of-bounds read; potential info leak into an outbound packet; crash if the address is unmapped |
| High | Consumption model (`:95`, `:139-143`) | Stack size is an effect slot; `amount == 0` destroys the item; cached `amount` can be stale after effect writes | Duplication or loss of stackables; ambiguous behaviour for zero/one counts |
| High | Refine failure semantics (`:315`, `:784`) | Two families call `BASE_ClearItem` on failure, destroying equipment; others only reset sanc | Permanent, unannounced loss of high-value items; inconsistent player expectations |
| High | Quest items (`:2243-2246`) | `sIndex - 4117` used as index into 5-element static arrays with no validation | Out-of-bounds read/write if the item CSV assigns Vol 191 to any other index |
| High | `LOCALSERVER` debug flag (`:1603-1604`) | Mount essence grants 10 levels instead of 1 when `DBServerAddress == "192.168.0.50"` | Silent 10x progression on a misconfigured shard |
| High | No tests (`*`) | No unit/integration/smoke tests, no harness, no CI | Any change to this file is unverifiable except by manual play |
| High | Identity mutations without finalisation (`:2978-3052`, `:3309-3329`, `:3462-3483`) | `ClassMaster`/`BaseScore` written mid-branch; only H5 forces a logout | Client desync or stale stats until relog |
| Medium | Volume of duplicated logic (three water branches, four gems, four bars, six extraction tools, ~1,500 lines) | Fixes must be applied 3-6 times; divergence already visible (N/A dungeons skip `RebuildGenerator`) | Divergent behaviour between nominally identical items |
| Medium | Global instance state (`WaterClear1`, `PartyPesa`, `CartaTime/CartaSala`) | One run per dungeon supports exactly one party; second use overwrites timers | Corrupted timers, duplicated or missing spawns, unfair instance resets |
| Medium | `GetUserInArea` off-by-one (`:1725-1732`, `:2781-2789`) | Returns a count, compared to `>= 1`, message prints `count-1` | Any single player blocks entry; message reports one fewer occupant |
| Medium | Courage/Elixir guards (`:3582`, `:3616`) | Compares `Value` to 500/2000 but writes 1 | Buff cannot be refreshed; item permanently refunded after first use |
| Medium | Fury stone reset mode (`:3457-3499`) | Does not require the four element items and decrements fame/exp on `Circle != 0` | Unbounded fame/exp drain while `Reset < 3` if the item is spammed |
| Medium | `sprintf` with runtime format (`:1052`, `:1727`, `:2277`, `:2424`, `:3333`, `:3429`, `:3541`, `:3871`...) | Table-supplied format strings; 145 call sites into a shared `temp[4096]` | Format-string corruption if the language table is edited; buffer overflow on long names |
| Medium | Packet size not validated (`:43`) | `pMsg` cast to `MSG_UseItem` without checking `m->Size >= sizeof(MSG_UseItem)` | Reads past the received frame for truncated packets |
| Medium | Dead randomness (`:2831`, `:2842`, `:2852`, `:2714`, `:2717`, `:2741`) | `rand() % 1` is always 0 | "Random" boss/position selection is deterministic; misleading intent |
| Medium | `party` loop duplicate credit (`:2294-2372`) | Leader grant plus per-member grant without excluding the leader from `PartyList` | Possible double 10 % exp to the leader |
| Medium | Roll normalisation (46 sites) | `if (x > 100) x -= 15` makes 86..100 twice as likely as 0..85 | Effective success rates up to ~15 % higher than the nominal table |
| Low | Unused packet fields (`GridX/GridY`) | Validated then ignored | Misleading contract; wasted check |
| Low | Unused `UseItemTime` (`:3214`; cooldown block commented `:206-219`) | Timestamp written but never read | Dead anti-spam mechanism |
| Low | Audit gaps (E15 war pack, E10 guild board) | No `ItemLog` for donation or guild-board use | Untraceable value creation |
| Low | Log uses post-mutation values (`:2860`, `:2883`) | `item->sIndex` logged after `memset` | Audit lines record index 0 |
| Low | `memset(item, 0, 8)` (`:1588`) | Magic size instead of `sizeof(STRUCT_ITEM)` | Breaks silently if the struct grows |
| Low | `g_pSancGrade` row mix-up (`CReadFiles.cpp:157`) | `ÁMAGO` key writes `g_pSancRate[0]` (PO row) instead of row 2 | Configuration surprise; row 2 is effectively fixed |
| Low | Naming/log inconsistencies (`:3755` "new cp 75" for 150; `:4256` logs "joia_pvp" for a recovery jewel) | Copy-paste log text | Misleading operations data |
| Low | No `default:` for unhandled volatiles (`:5724-5726`) | Silent no-op | Indistinguishable from packet loss during incident triage |

---

## 11. Test Coverage Analysis

### 11.1 Test inventory

A full repository scan was performed for test projects, test sources, fixtures, mocks, assertions and CI definitions (patterns `test`, `spec`, `fixture`, `mock` in file names; `gtest`, `catch2`, `assert`, `TEST(`, `TEST_F(` in contents; `.github`, `azure-pipelines`, `.gitlab-ci` in paths), excluding `.agents`, `.codegraph`, `.opencode` and `openspec`.

**Result: no test artefacts exist anywhere in the repository.**

| Evidence | Observation |
|---|---|
| `W2PP Code Project.sln` | Exactly two projects: `TMSrv` and `DBSrv`. No test project, no test target, no `x64` mapping. |
| File-name scan (`*test*`, `*spec*`, `*fixture*`, `*mock*`) | Zero matches outside build artefacts and `.git`. |
| `Code/TMSrv/*.vcxproj` | No `<ClCompile>` entry for a test file, no test framework include path, no `PostBuild` test step. |
| Source scan for `assert`/`TEST(`/`gtest` | No assertion macro and no test framework symbol in the TMSrv or DBSrv sources. |
| CI configuration | None. The only YAML in scope is `openspec/config.yaml`, which is excluded by the analysis parameters and is not a CI definition. |
| Harness / fake socket | None. `CPSock` is used directly; there is no interface seam to substitute a transport. |

### 11.2 Per-area coverage assessment

Coverage is estimated structurally: a branch is "covered" only if some automated test would exercise it. Since no tests exist, every figure is 0 %, and the column instead records the **observability** of the branch (how easily a developer could build a test around it).

| Area | Unit tests | Integration tests | Coverage | Testability assessment |
|---|---|---|---|---|
| A. Entry gates (A1-A7) | 0 | 0 | 0 % | Low: needs `pUser`/`pMob` fixtures and a fake socket; gates themselves are simple |
| B. Consumption (B1-B3) | 0 | 0 | 0 % | Low: `BASE_SetItemAmount` is directly callable and would be the only easy unit under test |
| C. Potions (C1-C4) | 0 | 0 | 0 % | Medium: pure arithmetic on `pUser.ReqHp` if globals can be seeded |
| D. Refine engine (D1-D13) | 0 | 0 | 0 % | Low-Medium: 6 rate tables and a 2-phase roll; high value, high branch count (>120 paths) |
| E. Consumables (E1-E15) | 0 | 0 | 0 % | Low: each branch mutates a different subsystem |
| F. Instances/tickets (F1-F10) | 0 | 0 | 0 % | Very low: depends on wall-clock (`localtime`), global timers and grid occupancy |
| G. Mounts (G1-G4) | 0 | 0 | 0 % | Low: depends on `mSummon` and entity creation |
| H. Progression (H1-H9) | 0 | 0 | 0 % | Low: full character-state transitions, DB logout, static quest tables |
| I. Equipment modification (I1-I8) | 0 | 0 | 0 % | Medium: deterministic effect copies are the easiest candidates for a harness |
| Language/format strings | 0 | 0 | 0 % | Medium: `g_pMessageStringTable` bounds are checkable statically |

### 11.3 Testability blockers

1. **No dependency injection.** Every dependency is a global or a free function; there is no seam to replace `SendItem`, `GetTickCount`, `rand`, `time` or `localtime`.
2. **`main`-less library.** TMSrv is a Win32 GUI application (`WinMain` + message loop); the handler cannot be linked into a console harness without the whole host.
3. **Time and randomness are ambient.** Eight branches depend on `localtime`, six on `rand`, two on `GetTickCount`. Deterministic testing requires either link-time interception or a refactor that is out of scope for this analysis.
4. **Hidden global coupling.** `pMob[conn]` doubles as the player entity, so every test would need a fully populated `STRUCT_MOB` plus a `CUser` with a live socket object.
5. **No assertions, no invariants.** The code never validates its own post-conditions (e.g. "sanc must be <= 15 after a refine"), so a harness could only compare against externally written expectations - which do not exist.

### 11.4 Coverage risk statement

The component implements the entire item economy through 68 branches, 106 volatile codes and 12 index keys, with about 120 decision paths in the refine engine alone and 19 distinct probability distributions. None of it is exercised by any automated test, and four of the five highest-severity findings in this report (A3, B1, D3, H3) are in paths that a single table-driven test would have caught. The absence of tests is therefore the single largest quality risk of this component, larger than any individual defect.

---

## 12. Appendix - Branch-to-Language-String Map (sample)

| Branch | Success message | Failure message |
|---|---|---|
| Refine (all families) | `_NN_Refine_Success` | `_NN_Fail_To_Refine` |
| Refine, target invalid | - | `_NN_Only_To_Equips` |
| Refine, cap reached | - | `_NN_Cant_Refine_More` |
| Egg incubation | `_NN_INCUBATED` | `_NN_Incu_Wait_More` |
| Fairy dust | `*** Level UP !!! ***` | `_NN_Fail_To_Level_Up` |
| Kappa / Vigor / XP chest / chicken / courage | - | `_NN_CantEatMore` |
| Water / nightmare / duel / territory | `_MSG_StartTime` signal | `_NN_Cant_Use_That_Here`, `_NN_Party_Leader_Only`, `_NN_Someone_is_on_quest` |
| Nightmare | - | `_NN_Night_Limited` |
| Mounts | `_NN_Mount_Growth` | `_NN_Mount_Not_Match`, `_NN_Cant_Upgrade_More` |
| Ideal stone | `_NN_My_King_Bless1` | `_NN_Cant_with_armor` |
| Mystic stone | (score/affect sync only) | `_NN_OnlyVillage`, `_NN_GOD2CHARONLY_` |
| Quest items | `_NS_GETEXP`, `_NN_Level_Up` | `_NN_Level_Limit`, `_NN_Level_Limit2` |
| Arch crystals | `_DN_Play_Quest` | `_NN_Youve_Done_It_Already`, `_NN_Need_BeforeQuest` |
| Extraction | emotion 14 | `_DN_SANCREQ` (sanc requirement) |
| Silver bars | `_NN_Cash_ChargeOk` / emotion | `_NN_Cant_get_more_than_2G` |
| Bag | - | `_NN_MAX_BAG` |

---

*End of report. Read-only static analysis; no project file was created, modified or deleted outside this report.*
