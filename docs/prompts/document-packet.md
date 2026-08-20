# Prompt Template — Document Packet (MSG_* Spec Generator)

**Purpose:** Generate a complete, byte-exact specification of a single W2PP protocol packet — its binary layout (fields, types, sizes, offsets, padding, alignment) **and** the full server-side mechanics applied to it (dispatch, validation, game logic, side effects).

**How to use:**

1. Copy the prompt body below (everything after the `---` under "PROMPT").
2. Replace `{{PACKET_NAME}}` with the packet to document. Accept either form: `MSG_Attack` or `_MSG_Attack` (the executor must normalize — the struct is `MSG_*`, the type constant is `_MSG_*`).
3. Optionally replace `{{EXTRA_FOCUS}}` with any specific concern (e.g. "focus on anti-cheat checks"), otherwise delete that line.
4. Run it against this repository (one packet per run).
5. The deliverable is written to `docs/packets/MSG_<Name>.md`.

**Deliverable:** one Markdown file per packet under `docs/packets/`.

---

# PROMPT

## Role

You are a Senior Protocol Reverse-Engineer and Game-Server Analyst working on **W2PP**, a legacy Windows C/C++ MMORPG private server (TMSrv = game server, DBSrv = account/character server, NPTool = admin tool). You produce **documentation only** — you must NEVER modify, refactor, or create project source files. The only file you may create is the deliverable Markdown spec.

## Objective

Produce a byte-exact packet specification and complete behavioral analysis for the packet:

**`{{PACKET_NAME}}`**

{{EXTRA_FOCUS}}

Trace the packet end-to-end: wire framing → dispatch → validation → business logic → side effects → any response/confirmation packets → any database-server forwarding. The output must be precise enough to reimplement a compatible client, server handler, or packet sniffer **without reading the source**.

## Authoritative Sources (in priority order)

The **code is the single source of truth**. The pre-generated reports under `docs/reports/` may be used for orientation and hypotheses, but **every claim taken from them must be verified against the actual source before inclusion** — they contain known stale/suspected items. If code and report disagree, the code wins and the discrepancy is documented.

| # | Source | Role |
|---|--------|------|
| 1 | `legacy/Code/Basedef.h` | Type constants (`const short _MSG_*`), packet structs (`MSG_*`), shared header macro (`_MSG`), direction flags, nested `STRUCT_*` domain types, `BASE_*` rule-function prototypes |
| 2 | `legacy/Code/Basedef.cpp` | Implementations of `BASE_*` validation/computation rules applied to the packet |
| 3 | `legacy/Code/TMSrv/ProcessClientMessage.cpp` | Dispatcher: inbound client→game routing, dispatcher-level guards |
| 4 | `legacy/Code/TMSrv/_MSG_*.cpp` | The `Exec_MSG_*` handler(s) implementing the packet's business logic |
| 5 | `legacy/Code/TMSrv/ProcessDBMessage.cpp` | Dispatcher for DB→game responses (for forwarded packets) |
| 6 | `legacy/Code/DBSrv/CFileDB.cpp`, `legacy/Code/DBSrv/Server.cpp` | DBsrv-side handling and file persistence of forwarded packets |
| 7 | `legacy/Code/TMSrv/SendFunc.cpp`, `legacy/Code/TMSrv/GetFunc.cpp` | Outbound packet builders and shared helpers used by the handler |
| 8 | `legacy/Code/CPSock.h`, `legacy/Code/CPSock.cpp` | Wire framing, obfuscation, checksum |
| 9 | `legacy/Code/ItemEffect.h` | `EF_*` item-effect constants (for item-carrying packets) |
| 10 | `docs/reports/` | **Context only** — see verification rule above |

## Embedded Protocol Reference (verified facts — use these, do not re-derive)

### Common header

Every packet begins with the `_MSG` macro (`Basedef.h:925-930`), a **12-byte header**:

| Offset | Field | Type | Size |
|--------|-------|------|------|
| 0 | `Size` | `short` | 2 |
| 2 | `KeyWord` | `char` | 1 |
| 3 | `CheckSum` | `char` | 1 |
| 4 | `Type` | `short` | 2 |
| 6 | `ID` | `short` | 2 |
| 8 | `ClientTick` | `unsigned int` | 4 |

`Size` = total packet size in bytes **including** the header. `Type` = the `_MSG_*` constant value. `ID` = connection/session slot (sender-stamped). `KeyWord`/`CheckSum` are transport fields (see framing).

### Type constant encoding and direction flags (`Basedef.h:932-941`)

`_MSG_*` constants are `(sequence | direction flags)`. The low byte is the per-direction sequence number; the high bits encode the path(s) the packet travels:

| Flag | Value | Path |
|------|-------|------|
| `FLAG_GAME2CLIENT` | `0x0100` | TMSrv → game client |
| `FLAG_CLIENT2GAME` | `0x0200` | game client → TMSrv |
| `FLAG_DB2GAME` | `0x0400` | DBSrv → TMSrv |
| `FLAG_GAME2DB` | `0x0800` | TMSrv → DBSrv |
| `FLAG_DB2NP` | `0x1000` | DBSrv → NPTool (admin) |
| `FLAG_NP2DB` | `0x2000` | NPTool → DBSrv |
| `FLAG_NEW` | `0x4000` | protocol extension marker |

⚠️ **Aliasing is real.** Distinct constants share the same low-byte sequence with different direction flags (e.g. `(10 | FLAG_GAME2DB)` vs `(10 | FLAG_DB2GAME)` are different messages), and some constants have **no dedicated struct** — a trailing comment (`// STANDARD`, `//SignalParm`, `// STANDARD PARM`) means they travel as `MSG_STANDARD` / `MSG_STANDARDPARM` / `MSG_STANDARDPARM2` / `MSG_STANDARDSHORTPARM2` / `MSG_STANDARDPARM3`. Some structs have C++ constructors that pre-set `Type`/`ID`/`Size` (e.g. `MSG_SendExpRanking`, `Basedef.h:2196-2233`) — document these. Some structs have variants (e.g. `MSG_AccountLogin` vs `MSG_AccountLogin_HWID`) — document which one is actually sent/received and how the receiver distinguishes them.

### Packing / alignment rule (critical — get this right)

`Basedef.h` is **not** uniformly packed. Explicit `#pragma pack(push, 1)` regions cover **only**:

- lines **808–835** (`STRUCT_RANKING`)
- lines **1212–1246** (`MSG_AccountLogin`, `MSG_AccountLogin_HWID`)
- lines **1465–1492** (`MSG_UpdateScore`)
- lines **2063–2097** (`MSG_AttackOne`)

Everywhere else, **no packing pragma is active**, and neither `TMSrv.vcxproj` nor `DBSrv.vcxproj` overrides struct member alignment — so the **MSVC default (`/Zp8`)** applies: each member is aligned to `min(sizeof(member), 8)`, and the struct size is rounded up to the largest member alignment. You MUST check which region your struct (and every nested `STRUCT_*` it embeds) falls in, and compute offsets accordingly. Watch especially for `long long` members (8-byte aligned under `/Zp8`, 1-byte under pack(1)) and for `short`/`char` tails followed by `int`/`long long`.

Platform: little-endian (x86), Windows LP32 model — `int` = 4 bytes, `short` = 2, `char` = 1, `long long` = 8, pointers never appear on the wire (flag and report any pointer-typed member as a bug/suspicion).

### Transport framing (from `CPSock.cpp` — for the shared preamble)

- Outbound connections open with a 4-byte `INITCODE = 0x1F11F311` magic before any framed message.
- Payload bytes **from offset 4 onward** are obfuscated per byte with a position-rotating XOR-style transform keyed by `KeyWord` (an index into the shared `pKeyWord[512]` table).
- `CheckSum` = difference of raw vs. transformed payload sums (`Sum2 - Sum1`); validated on receive, mismatch sets an error code.
- `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]` or the buffer is reset.
- Billing-server traffic is a separate fixed 196-byte plain protocol — flag it if the packet participates.
- `BASE_CheckPacket` (`Basedef.cpp:6475`) is **disabled** (body commented out, returns `FALSE`) — there is no central release-build packet-size validation. Note this where relevant.

### Dispatch chain (for tracing)

```
client → CPSock.ReadMessage (de-frame/de-obfuscate)
      → Server.cpp (WSA_READ) → ProcessClientMessage(conn, pMsg, FALSE)
      → switch (std->Type) → Exec_MSG_<Name>(conn, pMsg)   [legacy/Code/TMSrv/_MSG_<Name>.cpp]
```

- Dispatcher guards (`ProcessClientMessage.cpp:38-66`): `std->ID ∈ [0, MAX_USER)`, `ServerDown >= 120` shutdown guard, `ClientTick == SKIPCHECKTICK` anti-spoof guard. **There is no `default:` case — unknown types are silently dropped.** If your packet has no case, say so and find where (if anywhere) it is handled.
- DB forwarding pattern: handler rewrites `m->Type` to a `_MSG_DB*` constant, sets `m->ID = conn`, sends via `DBServerSocket.SendOneMessage(...)`, and transitions the user's `USER_*` mode. DB→game responses come back through `ProcessDBMessage.cpp`; persistence lives in `DBSrv/CFileDB.cpp`.
- Session modes encountered in guards: `USER_ACCEPT`, `USER_LOGIN`, `USER_SELCHAR`, `USER_WAITDB`, `USER_PLAY`.
- Standard anti-cheat vocabulary: `AddCrackError(conn, n, code)`, `CrackLog`, tick-window checks, speed caps, `memcmp` of client-supplied item snapshots vs. server state, the 2,000,000,000 coin cap.
- Standard side-effect vocabulary: `SendClientMessage`, `SendHpMode`, `SendScore`, `SendItem`, `SendEtc`, `SendCargoCoin`, `pUser[conn].cSock.AddMessage/SendOneMessage`, `GridMulticast` (area broadcast), `SaveUser`, `Log`/`ItemLog`, mode transitions.

## Procedure

### Step 1 — Resolve the message type

1. Locate the `const short _MSG_<Name>` definition(s) in `Basedef.h` (cite `file:line`). Decode: full hex value, sequence number (low byte), direction flag(s).
2. List **all aliases**: other constants with the same numeric value, and other constants sharing the same struct.
3. Resolve the struct actually on the wire: the dedicated `MSG_<Name>` struct, a variant (`_HWID`, `Ex`, …), or a `// STANDARD`-style shared form per the trailing comment. If a constructor pre-sets header fields, record the exact values.
4. If multiple candidate structs exist and the code disambiguates (e.g. by `Size`), document the disambiguation rule. If you cannot resolve it from code, STOP and report — do not guess.

### Step 2 — Compute the exact binary layout

1. Determine the packing context of the struct **and of every nested `STRUCT_*` it embeds** (see the packing rule above). State it explicitly.
2. Build the full field table (header first, then payload). For each field: offset, size, C type, alignment requirement, padding inserted before it (if any), and semantic meaning. Show the offset math; mark explicitly computed padding bytes as their own rows.
3. Expand nested `STRUCT_*` fields recursively — either inline (indented) or in an appendix table — until every byte is accounted for by primitive fields. Compute each nested struct's total size the same way.
4. Compute total `sizeof(struct)` and state the expected value of the `Size` header field. Cross-check against any `sizeof(MSG_*)` usage in send code and against hardcoded sizes in the handler. Report mismatches.
5. Identify variable-length tails (e.g. `STRUCT_DAM Dam[1]`-style trailing arrays): document the length formula and how `Size` is computed at runtime.
6. Record signedness, and mark every unknown/reserved/padding member (`Unk*`, `Zero*`, `Rsv`, `EMPTY*`, unnamed gaps) as `UNKNOWN` — never invent semantics.

### Step 3 — Wire framing (shared preamble + per-packet note)

Include the standard framing preamble (INITCODE, header, obfuscation, checksum, size bounds — see the reference above) as a compact fixed section, then add per-packet notes: anything this packet does that deviates (e.g. billing protocol, oversized payloads, constructor-framed headers, skipped obfuscation).

### Step 4 — Trace the lifecycle per direction

For **each** direction flag on the constant, trace the path with `file:line` citations: who builds the packet, who receives it, which dispatcher case routes it, which handler/function processes it. Cover: client→TMSrv, TMSrv→client (incl. `GridMulticast` broadcasts), TMSrv→DBSrv forwarding (the `Type` rewrite and mode transition), DBSrv processing and persistence (`CFileDB.cpp`), DBSrv→TMSrv response, and NPTool paths if flagged. Note ports only if the code ties the packet to a specific socket.

### Step 5 — Extract mechanics, validation, and side effects

From the handler(s) and everything they call (follow into `BASE_*`, `SendFunc`, `GetFunc`, `CMob`/`CUser`/`CItem` methods as needed):

1. **Validation & guards** — every check applied to the packet or its fields, in execution order: session-mode gates, HP/liveness checks, index/bounds checks, tick/timing windows, snapshot `memcmp` integrity checks, range/cap checks (with exact constants), ownership/permission checks. For each: the condition, the failure behavior (drop, corrective packet, `AddCrackError`, disconnect), and `file:line`.
2. **Game mechanics / business logic** — the actual rules the packet triggers: formulas (with constants and table names, e.g. `g_pSancRate`, `BaseSIDCHM`), state machines, economy rules, success rolls (`rand()%…` thresholds), cooldowns, level/class gates. Describe each rule in enough detail to reimplement it; include a numbered workflow per rule.
3. **Side effects** — exhaustive list, each with `file:line`: global-state mutation (`pUser[]`, `pMob[]`, `pItem[]`, grids, guild tables), outgoing packets (to self / target / grid / all — name the `MSG_*` or builder), DB messages (the `_MSG_DB*` type and mode transition), persistence (`SaveUser`, file writes), logs (`Log`, `ItemLog` — include the log format string), timers/counters updated.
4. **Related packets** — confirmation/response packets (`_MSG_CNF*`), failure variants, the DB-side mirror types, and the aliased types from Step 1.
5. **Discrepancies & open questions** — dead code paths, `TODO` comments on the definition, suspected bugs, fields whose purpose couldn't be determined, report-vs-code conflicts.

### Step 6 — Write the deliverable

Create `docs/packets/` if needed and write **`docs/packets/MSG_<Name>.md`** using EXACTLY this structure:

```markdown
# MSG_<Name>

## 1. Summary
| Property | Value |
|---|---|
| Type constant | `_MSG_<Name>` = `(<seq> | <FLAGS>)` = `0x<hex>` |
| Sequence ID | <n> |
| Direction(s) | <decoded flags> |
| Wire struct | `MSG_<Name>` (or shared form + why) |
| Total size | <n> bytes (fixed) / <formula> (variable) |
| Packing | pack(1) explicit / compiler default (/Zp8) |
| Handler | `Exec_MSG_<Name>` @ `<file:line>` (or "none — <explanation>") |
| Aliases | <constants sharing value/struct> |
| Related | <CNF/DB-mirror packets> |

## 2. Wire Framing (protocol preamble)
<compact standard preamble> + per-packet deviations

## 3. Binary Layout
### 3.1 Header (12 bytes, `_MSG` macro)
| Offset | Size | Field | Type | Description |
### 3.2 Payload
| Offset | Size | Field | Type | Align | Pad | Description |
<one row per field; explicit padding rows; UNKNOWN marked>
### 3.3 Nested struct expansions
<appendix tables for each embedded STRUCT_*>
### 3.4 Size verification
<offset math, sizeof total, expected Size value, cross-checks>

## 4. Lifecycle & Flow
<per-direction trace with file:line; ascii sequence diagram>

## 5. Validation & Guards
| # | Check | Condition | On failure | Location |
<numbered table, execution order> + short detail per non-obvious check

## 6. Game Mechanics & Business Logic
### Rule: <name>
**Overview / Detailed description / Workflow** — repeat per rule

## 7. Side Effects
| Effect | Target | Mechanism | Location |
<state mutation | outgoing packet | DB forward | persistence | log | mode transition>

## 8. Related Packets
<aliases, CNF/fail variants, DB mirrors — with their constants>

## 9. Discrepancies & Open Questions
<or "None found">

## 10. Source References
<grouped file:line list of every citation used>
```

## Rules

- **Read-only.** Never modify project sources. The only file created is `docs/packets/MSG_<Name>.md`.
- **Cite everything.** Every structural and behavioral claim carries a `file:line` reference. No uncited assertions.
- **No guessing.** Unknown fields are `UNKNOWN`; unresolved behavior goes to "Discrepancies & Open Questions". If the packet/struct cannot be resolved from code, STOP and report what is missing.
- **Verify the reports.** Anything sourced from `docs/reports/` must be confirmed against the code first; conflicts are documented in §9 with code winning.
- **Show the math.** Offsets, padding, and totals are computed explicitly per the packing rule — never asserted without the derivation.
- **Code over comments.** Comments (incl. Portuguese/Korean ones) may inform semantics but behavior is read from the code.
- **Follow the calls.** Handler logic that delegates to `BASE_*`/`SendFunc`/`GetFunc`/class methods must be followed far enough to document the rule — don't stop at the call site.
