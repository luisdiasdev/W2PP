# MSG_TransperCharacter

## 1. Summary

| Attribute | Value |
|---|---|
| Type constant | `_MSG_TransperCharacter` |
| Base sequence | `169` (`0xA9`) |
| Flags (OR'd) | `FLAG_GAME2CLIENT` (0x0100) · `FLAG_CLIENT2GAME` (0x0200) · `FLAG_DB2GAME` (0x0400) |
| **Type value (authoritative)** | **`0x7A9` (1961)** |
| Alias / dedicated struct | **None.** No `MSG_TransperCharacter` struct exists; only the `const short` type constant is defined. The packet travels as a generic **`MSG_STANDARDPARM2`** (`int Parm1`, `int Parm2`) form — see §3. |
| Wire struct | `MSG_STANDARDPARM2` (Basedef.h:954-959) |
| Wire struct size | **20 bytes** (`sizeof(MSG_STANDARDPARM2)`), no padding (see §3.4) |
| Direction(s) | **DB2GAME** (DBSrv → TMSrv, gated init signal) and **GAME2CLIENT** (TMSrv → client, config-gated producer). Declared also with `CLIENT2GAME` but **no client→TMSrv handler exists** (see §4). |
| Producers | DBSrv/Server.cpp:1173-1174 (DB2GAME init); TMSrv/ProcessDBMessage.cpp:459-470 (GAME2CLIENT, gated on `TransperCharacter`) |
| Consumers | TMSrv/ProcessDBMessage.cpp:61-67 (DB2GAME, `conn==0`); **no client-side / TMSrv-client dispatcher handler** (see §4). |
| Related packets | `_MSG_ReqTransper` (170), `_MSG_DBServerSend1` (43), `_MSG_DBCNFServerChange` (42), `_MSG_DBServerChange` (20) |

**Source:** `const short _MSG_TransperCharacter = (169 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME | FLAG_DB2GAME);` — Basedef.h:2168.

**Flag decode math:** `0xA9 | 0x0100 | 0x0200 | 0x0400 = 0x7A9` (1961). See §9 for a discrepancy with the embedded protocol reference (`0x0BA9`), which double-counts `DB2GAME`.

## 2. Wire Framing (protocol preamble)

Standard CPSock framing applies to this packet — there are **no per-packet deviations**. Facts verified in `CPSock.cpp` / `CPSock.h`:

- **Magic:** `INITCODE = 0x1F11F311` (CPSock.h:40; compared at CPSock.cpp:373).
- **Obfuscation:** payload from offset 4 is XOR-obfuscated by a per-byte position-rotating key derived from `KeyWord` (CPSock.cpp:249, ReadMessage path). The `_MSG` header fields (`Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`) are not obfuscated.
- **CheckSum:** `CheckSum = Sum2 - Sum1` over the body (computed in the CPSock send path).
- **Size validation:** `Size` must satisfy `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE` (CPSock.cpp:397; `MAX_MESSAGE_SIZE = 8192`, CPSock.h:38).
- **Billing:** the separate 196-byte plain billing protocol does not apply here.
- **BASE_CheckPacket** (Basedef.cpp:6475) is **disabled**, so no further checksum/replay gate at dispatch.

Total on-the-wire message: 4-byte `INITCODE` + 20-byte obfuscated `MSG_STANDARDPARM2` payload = **24 bytes** (transport framing; the logical packet `Size` field = 20).

## 3. Binary Layout

Packing context: Basedef.h is **not** uniformly packed. Explicit `#pragma pack(push,1)` regions cover only lines 808-835, 1212-1246, 1465-1492, 2063-2097. `MSG_STANDARDPARM2` lives at lines **954-959**, i.e. **outside** every packed region, so MSVC **default `/Zp8`** applies: each member aligned to `min(sizeof(member), 8)`; struct size rounded up to the largest member alignment (4, driven by the `int`/`unsigned int` members). Little-endian x86, LP32 (4-byte pointers).

There is no dedicated `MSG_TransperCharacter` struct — the wire form is `MSG_STANDARDPARM2` (Basedef.h:954-959), which the producer fills and sends via `SendOneMessage` (ProcessDBMessage.cpp:461-469).

### 3.1 Header (`_MSG`, Basedef.h:925-930) — 12 bytes

| Field | Type | Offset | Size | Notes |
|---|---|---|---|---|
| `Size` | `short` | 0 | 2 | Total struct size incl. header = 20 |
| `KeyWord` | `char` | 2 | 1 | Obfuscation key byte |
| `CheckSum` | `char` | 3 | 1 | `Sum2 - Sum1` |
| `Type` | `short` | 4 | 2 | `_MSG_TransperCharacter` (0x7A9) |
| `ID` | `short` | 6 | 2 | Producer sets `ESCENE_FIELD + 1 = 30001` (TMSrv) / `0` (DBSrv) |
| `ClientTick` | `unsigned int` | 8 | 4 | Client tick; 0/unused by producers here |

Header offset math under `/Zp8`: `short`@0 (2) → `char`@2 (1) → `char`@3 (1) → `short`@4 (2, aligned) → `short`@6 (2) → `unsigned int`@8 (4, aligned). No padding within the header; header size 12.

### 3.2 Payload — `MSG_STANDARDPARM2` (Basedef.h:954-959), 8 bytes

| Field | Type | Offset | Size | Notes |
|---|---|---|---|---|
| `Parm1` | `int` | 12 | 4 | Producer sets `0` |
| `Parm2` | `int` | 16 | 4 | Producer sets `0` |

Payload offset math: after header ends at 12, `int Parm1`@12 (4, aligned) → `int Parm2`@16 (4, aligned). **No padding** anywhere in the struct.

> Note: the embedded protocol reference calls these `Parm1(short)/Parm2(short)`. The source uses the **`int`** parm struct (`MSG_STANDARDPARM2`), not the `short` variant (`MSG_STANDARDSHORTPARM2`, Basedef.h:961-966). Code wins — see §9.

### 3.3 Nested struct expansions

None. This packet has no nested structs — it is a flat `MSG_STANDARDPARM2`.

### 3.4 Size verification

```
Offset  Field         Type          Size
0       Size          short         2
2       KeyWord       char          1
3       CheckSum      char          1
4       Type          short         2
6       ID            short         2
8       ClientTick    unsigned int  4
12      Parm1         int           4
16      Parm2         int           4
                                   ----
Total                             20 bytes
```

- Largest member alignment = 4 (`unsigned int`/`int`). 20 is a multiple of 4 → no trailing padding. **`sizeof(MSG_STANDARDPARM2) = 20`.**
- Producer confirms this: `sm.Size = sizeof(MSG_STANDARDPARM2);` and `SendOneMessage((char*)&sm, sizeof(MSG_STANDARDPARM2));` (ProcessDBMessage.cpp:467,469).
- **Expected `Size` field on the wire: 20.** No mismatch.

## 4. Lifecycle & Flow

This packet has **two** legs, in distinct processes:

### 4.1 Leg A — DB2GAME: DBSrv → TMSrv (mode-init signal)

- **Producer (DBSrv):** when `TransperCharacter` is set (activated on DBSrv because `redirect.txt` present — Server.cpp:602-609 sets `TransperCharacter = 1`), the DB server signals the game server at login handshake:
  `cFileDB.SendDBSignalParm2(User, 0, _MSG_TransperCharacter, 0, 0);` — Server.cpp:1173-1174.
- `SendDBSignalParm2` builds a `MSG_STANDARDPARM2` with `Type=_MSG_TransperCharacter`, `ID=0`, `Parm1=0`, `Parm2=0`, `Size=sizeof(sm)` and sends via `SendOneMessage` — CFileDB.cpp:2137-2151.
- **Consumer (TMSrv):** `ProcessDBMessage.cpp:61-67` — `case _MSG_TransperCharacter` inside the `conn == 0` DB-server branch (ProcessDBMessage.cpp:56-58) sets the game-server global `TransperCharacter = 1` and logs `"TransperCharacter mode"`. (`TransperCharacter` global declared at TMSrv/Server.cpp:382.)

### 4.2 Leg B — GAME2CLIENT: TMSrv → client (character-transfer signal)

- **Producer (TMSrv):** right after account confirmation (`CNFAccountLogin` handling), gated on the config flag:
  - `if (TransperCharacter != 0)` (ProcessDBMessage.cpp:459)
  - builds `MSG_STANDARDPARM2 sm;` → `sm.Type = _MSG_TransperCharacter;` `sm.ID = ESCENE_FIELD + 1;` `sm.Parm1 = 0;` `sm.Parm2 = 0;` `sm.Size = sizeof(MSG_STANDARDPARM2);` then `pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_STANDARDPARM2));` (ProcessDBMessage.cpp:461-469).
  - `ESCENE_FIELD = 30000` (Basedef.h:170); so `ID = 30001`.

### 4.3 Inbound client→TMSrv leg: **NO HANDLER**

- The constant declares `FLAG_CLIENT2GAME`, but there is **no `case _MSG_TransperCharacter` in `ProcessClientMessage.cpp`** (the TMSrv client dispatcher). The dispatcher switch covers 63 `case _MSG_*` labels; `_MSG_TransperCharacter` is not among them.
- Therefore **the client never routes this packet back to TMSrv**, and there is no client-dispatcher consumer. The client is the terminal sink for Leg B.

### 4.4 Sequence diagram

```
DBSrv                        TMSrv (game)                          Client
  |                              |                                    |
  |  Leg A: MSG_STANDARDPARM2    |                                    |
  |  Type=0x7A9 ID=0 Parm1=0     |                                    |
  |  Parm2=0 (CFileDB:2137)      |                                    |
  |----------------------------->| ProcessDBMessage:61  sets          |
  |                              |   TransperCharacter=1 (TMSrv global)|
  |                              |                                    |
  |                              |  Leg B (on account confirm, if     |
  |                              |   TransperCharacter!=0):           |
  |                              |  MSG_STANDARDPARM2 Type=0x7A9      |
  |                              |  ID=ESCENE_FIELD+1=30001           |
  |                              |  Parm1=0 Parm2=0 (PDBMsg:461-469)  |
  |                              |----------------------------------->|
  |                              |                    (client consumes;
  |                              |                     no return packet)
```

## 5. Validation & Guards

- **`TransperCharacter` config gate (TMSrv, Leg B):** the TMSrv→client producer runs only `if (TransperCharacter != 0)` — ProcessDBMessage.cpp:459. When 0, the packet is never sent and no transfer mode signal reaches the client.
- **`TransperCharacter` activation (DB2GAME, Leg A):** DBSrv enables the mode when `redirect.txt` exists (Server.cpp:602-609) and pushes the signal to each game server (Server.cpp:1173-1174). TMSrv sets its own global from that inbound signal (ProcessDBMessage.cpp:61-67).
- **Dispatcher guard (client path):** `ProcessClientMessage` first checks `ID` bounds and `ServerDown >= 120` (ProcessClientMessage.cpp:40-55), but since there is no `case _MSG_TransperCharacter`, these guards never apply to this packet inbound from a client.
- **Framing validation:** generic CPSock `Size` range check `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE` (CPSock.cpp:397); `BASE_CheckPacket` is disabled (Basedef.cpp:6475).
- **No field-level validation:** `Parm1`/`Parm2` are always hardcoded `0`; there is no value-based branch in either consumer.

## 6. Game Mechanics & Business Logic

**Purpose:** `_MSG_TransperCharacter` is a **mode-activation / transfer-mode signal**, not a data-carrying operation.

- **Leg A (DB2GAME)** turns on the "character transfer" mode in the game server: it is the DBSrv telling TMSrv "this is a transfer-mode deployment", flipping `TransperCharacter = 1` (ProcessDBMessage.cpp:64).
- **Leg B (GAME2CLIENT)** is the signal sent to the player right after account confirmation telling the client that **character transfer is available**, so the client begins the transfer flow. It carries no payload data (`Parm1 = Parm2 = 0`); its sole content is the `Type` (0x7A9) and `ID` (`ESCENE_FIELD + 1`).
- The actual client-initiated transfer request uses a sibling packet: `_MSG_ReqTransper` (170, Basedef.h:2169) with the `MSG_ReqTransper` struct (Basedef.h:2170-2177: `Result`, `Slot`, `OldName[NAME_LENGTH]`, `NewName[NAME_LENGTH]`). TMSrv handles it in `ProcessDBMessage.cpp:215-224` (gated on `TransperCharacter == 0` early-return), replying with `ID = ESCENE_FIELD + 1`.

## 7. Side Effects

- **TMSrv (Leg A consumer, ProcessDBMessage.cpp:61-67):**
  - Sets global `TransperCharacter = 1` (the "transfer mode" flag).
  - Logs `"TransperCharacter mode"` to the system log.
- **TMSrv (Leg B producer, ProcessDBMessage.cpp:459-470):**
  - Emits an outgoing packet **to the client itself** (`pUser[conn].cSock.SendOneMessage`) with `ID = ESCENE_FIELD + 1 = 30001`, `Parm1 = 0`, `Parm2 = 0`. This is a self-directed signal message with no reply expected.
  - Does **not** change player state (no `Mode`/`SelChar` mutation) at this point; that happens later via `_MSG_ReqTransper` (ProcessDBMessage.cpp:224 sets `pUser[conn].Mode = USER_SELCHAR`).
- **DBSrv (Leg A producer):** no player-facing side effect; only the outbound handshake signal (Server.cpp:1173-1174).

## 8. Related Packets

| Packet | Constant | Value | Role |
|---|---|---|---|
| `_MSG_ReqTransper` | Basedef.h:2169 | 170 `\|` 0x0100\|0x0200\|0x0400\|0x0800 = `0xFAA` | Client-initiated transfer request (`MSG_ReqTransper` struct); TMSrv handler ProcessDBMessage.cpp:215 |
| `_MSG_DBServerSend1` | Basedef.h:1059 | 43 `\|` DB2GAME\|GAME2CLIENT = `0x0C2B` | Server-change signal (SignalParm) |
| `_MSG_DBCNFServerChange` | Basedef.h:1051 | 42 `\|` DB2GAME\|GAME2CLIENT = `0x0C2A` | Server-change confirmation (SignalParm) |
| `_MSG_DBServerChange` | Basedef.h:1061 | 20 `\|` GAME2DB = `0x814` | Game→DB server-change request |
| `_MSG_DBCNFCharacterLogin` | Basedef.h (search `DBCNFCharacterLogin`) | — | Character-login confirmation in same login handshake |

`_MSG_DBServerSend1` / `_MSG_DBCNFServerChange` are the "server-change" transport signals; `_MSG_TransperCharacter` is the sibling "transfer mode" activation signal in the character-transfer flow.

## 9. Discrepancies & Open Questions

1. **Type value — embedded reference vs source.** The embedded protocol reference lists `0x0BA9`. Recomputing from source: `169 | 0x0100 | 0x0200 | 0x0400 = 0x7A9`. `0x0BA9 - 0x7A9 = 0x0400`, i.e. the reference **double-counts `FLAG_DB2GAME` (0x0400)**. **Authoritative: `0x7A9` (1961)** — code wins.
2. **Parm width — embedded reference says `short`; source uses `int`.** The producer uses `MSG_STANDARDPARM2` (int `Parm1`/`Parm2`, Basedef.h:957-958), giving a **20-byte** packet. The `short` variant (`MSG_STANDARDSHORTPARM2`, 16 bytes) exists but is **not** used here. **Authoritative: `int` parms, 20 bytes.**
3. **Dead direction — `FLAG_CLIENT2GAME` declared, no handler.** `_MSG_TransperCharacter` advertises `CLIENT2GAME`, but there is **no `case _MSG_TransperCharacter` in `ProcessClientMessage.cpp`** and no `Exec_MSG_TransperCharacter` in any `TMSrv/_MSG_*.cpp`. The client is a pure sink. The client→game "transfer" traffic actually flows through `_MSG_ReqTransper` (170).
4. **Unused `Parm1`/`Parm2`:** both are always hardcoded `0` at every producer; the field carries no semantic data (only `Type` + `ID` are meaningful).
5. **Open question:** the client's interpretation of `ID = ESCENE_FIELD + 1` (30001) is not verifiable from server-side sources; it is an outbound signal ID by convention (see `ESCENE_FIELD` usage across `ProcessDBMessage.cpp`, e.g. lines 464, 479).

## 10. Source References

| File | Lines | Content |
|---|---|---|
| legacy/Code/Basedef.h | 170 | `ESCENE_FIELD = 30000` |
| legacy/Code/Basedef.h | 925-930 | `_MSG` header macro |
| legacy/Code/Basedef.h | 932-941 | Direction/`NEW` flags |
| legacy/Code/Basedef.h | 954-959 | `MSG_STANDARDPARM2` struct |
| legacy/Code/Basedef.h | 961-966 | `MSG_STANDARDSHORTPARM2` struct (short-parm variant, not used here) |
| legacy/Code/Basedef.h | 1051, 1059, 1061 | `_MSG_DBCNFServerChange`, `_MSG_DBServerSend1`, `_MSG_DBServerChange` |
| legacy/Code/Basedef.h | 2168 | `_MSG_TransperCharacter` type constant |
| legacy/Code/Basedef.h | 2169-2177 | `_MSG_ReqTransper` + `MSG_ReqTransper` struct |
| legacy/Code/TMSrv/ProcessDBMessage.cpp | 56-67 | DB2GAME consumer (`case _MSG_TransperCharacter`) |
| legacy/Code/TMSrv/ProcessDBMessage.cpp | 215-224 | `_MSG_ReqTransper` handler (transfer request) |
| legacy/Code/TMSrv/ProcessDBMessage.cpp | 459-470 | GAME2CLIENT producer (`TransperCharacter` gate) |
| legacy/Code/TMSrv/ProcessClientMessage.cpp | 38-66 | Client dispatcher guards/switch (no `_MSG_TransperCharacter` case) |
| legacy/Code/TMSrv/Server.cpp | 382 | `int TransperCharacter = 0;` (TMSrv global) |
| legacy/Code/DBSrv/Server.cpp | 54 | `int TransperCharacter = 0;` (DBSrv global) |
| legacy/Code/DBSrv/Server.cpp | 602-609 | `redirect.txt` → `TransperCharacter = 1` |
| legacy/Code/DBSrv/Server.cpp | 1173-1174 | DB2GAME producer (`SendDBSignalParm2(_MSG_TransperCharacter)`) |
| legacy/Code/DBSrv/CFileDB.cpp | 2137-2151 | `SendDBSignalParm2` builder |
| legacy/Code/CPSock.h | 38, 40 | `MAX_MESSAGE_SIZE`, `INITCODE` |
| legacy/Code/CPSock.cpp | 249, 373, 386, 397 | Framing/obfuscation/size checks |
| legacy/Code/Basedef.cpp | 6475 | `BASE_CheckPacket` (disabled) |
