# Component Deep Analysis Report

**Component:** CPSock
**Analyzed On:** 2026-08-19 17:13:23
**Project Scope:** legacy (W2PP C/C++ codebase)
**Analysis Location:** `legacy/Code/CPSock.h`, `legacy/Code/CPSock.cpp`

---

## 1. Executive Summary

CPSock is a Windows Winsock-based asynchronous socket and buffering abstraction class written in C++ (legacy codebase, last updated August 2012 per the header comment). It wraps the raw WinSock API (`socket`, `bind`, `listen`, `connect`, `recv`, `send`, `WSAAsyncSelect`, `closesocket`) into a message-oriented transport layer used by both server executables in the project: `TMSrv` (the game/Map server) and `DBSrv` (the database server). It is a shared, project-wide utility component compiled into both binaries through the Visual Studio project files (`legacy/Code/DBSrv/DBSrv.vcxproj` and `legacy/Code/TMSrv/TMSrv.vcxproj`).

The component's central role is to bridge the Winsock message-driven (`WSAAsyncSelect`) event model with the project's custom binary protocol. It provides two independent buffering pipelines (one transmit, one receive), each backed by a heap-allocated byte buffer, plus a per-message framing/obfuscation layer built around a shared 512-byte keyword table (`pKeyWord`) and a custom `HEADER` structure. It also implements an out-of-band, fixed-size "Bill server" protocol (`ReadBillMessage`/`SendBillMessage`) used exclusively for billing server communication.

Key findings:

- **Message framing + integrity:** Each logical message carries a `HEADER` (Size, KeyWord, CheckSum, Type, ID, ClientTick). Payload bytes (from offset 4 onward) are transformed with a per-byte, position-rotating obfuscation keyed off a random keyword index; a 1-byte checksum is derived from the difference between the raw and transformed payload sums and validated on receive.
- **Handshake enforcement:** A 4-byte init code (`INITCODE = 0x1F11F311`) is sent immediately after every outbound connection and must be verified before any framed message is parsed.
- **Two distinct protocols under one class:** (1) the obfuscated, header-framed variable-length protocol used for game/client and game/DB traffic, and (2) a plain fixed-size (196-byte) protocol used for the billing server that performs no framing or obfuscation.
- **Shared global state:** `ConnectPort`, `CurrentTime`, and `LastSendTime` are file-scope / externally-declared globals shared across all socket instances, which introduces concurrency and correctness coupling.
- **Error handling is message-box/log based and inconsistent:** failures surface via `MessageBox`, the external `Log()` function, or numeric error codes threaded through `ReadMessage`/`ReadBillMessage` out-parameters.
- **No automated tests exist anywhere in the project** (no unit, integration, or spec files were found). The correctness of the protocol/obfuscation logic is entirely unverified by test code.

The component is not an endpoint-exposing service; it is a transport/library abstraction consumed by the two server windows-message loops. The report documents its internal structure, protocol rules, data flow, coupling, and risks.

---

## 2. Data Flow Analysis

CPSock is a symmetric transport: inbound data flows from the socket into the receive buffer and is de-framed/decrypted into messages; outbound messages are framed/obfuscated into the send buffer and flushed to the socket. Both directions are driven by `WSAAsyncSelect` window messages delivered to the owning window procedure (`WSA_READ`, `WSA_READDB`, `WSA_READBILL`, `WSA_ACCEPT`, etc.).

### Inbound (receive) data flow

```
1. Kernel notifies via WSAAsyncSelect -> window proc (Server.cpp:3919, WSA_READ)
2. CPSock::Receive() recv()s into pRecvBuffer at nRecvPosition  (CPSock.cpp:336)
3. Loop: CPSock::ReadMessage() validates init code + header      (CPSock.cpp:353)
4. Full message de-framed: header checked, payload de-obfuscated in place, checksum verified
5. pMsg pointer returned; nProcPosition advanced; buffer reset when fully consumed
6. Server dispatches: ProcessClientMessage / ProcessDBMessage / ProcessBILLMessage
```

### Outbound (send) data flow

```
1. Caller builds a binary message struct (e.g. MSG_STANDARD) with Size/Type/ID/ClientTick
2. CPSock::AddMessage() frames + obfuscates payload into pSendBuffer  (CPSock.cpp:513)
3. CPSock::SendMessageA() flushes buffered bytes to the socket         (CPSock.cpp:617)
4. (or CPSock::SendOneMessage() = AddMessage + SendMessageA)           (CPSock.cpp:686)
```

### Receive window-message handling (TMSrv example)

```
1. WSA_READ event -> GetUserFromSocket(wParam) resolves socket to a user slot
2. WSAGETSELECTEVENT(lParam) != FD_READ -> CloseUser (connection closed)
3. pUser[User].cSock.Receive() -> recv into receive buffer
4. On partial/failed receive, a single retry Receive() is attempted, else CloseUser
5. while(1): pUser[User].cSock.ReadMessage(&Error,&ErrorCode) -> ProcessClientMessage(User, Msg)
   - Loop breaks when ReadMessage returns NULL (no complete message) or Error 1/2 (framing/checksum)
```

### Send-side flushing (TMSrv example)

```
1. Business handlers build messages and call pUser[conn].cSock.AddMessage() (bulk buffering)
2. Explicit flush via pUser[conn].cSock.SendMessageA() at logical boundaries
   (e.g. _MSG_CharacterLogin.cpp:42, _MSG_CharacterLogout.cpp:26, SendFunc.cpp:269/283)
3. Many handlers use SendOneMessage() for atomic frame+flush
```

### Buffer compaction

Both buffers support in-place compaction to reclaim consumed space:

```
1. RefreshRecvBuffer(): memcpy remaining (nRecvPosition - nProcPosition) bytes to front,
   reset nProcPosition=0, nRecvPosition -= left          (CPSock.cpp:593)
2. RefreshSendBuffer(): memcpy remaining (nSendPosition - nSentPosition) bytes to front,
   reset nSentPosition=0, nSendPosition -= left           (CPSock.cpp:605)
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Protocol / Handshake | Outbound connections send a 4-byte `INITCODE` (0x1F11F311) immediately after `connect` | CPSock.cpp:249-250 |
| Protocol / Handshake | `ReadMessage` must validate `INITCODE` before parsing any framed message (Init==0 state) | CPSock.cpp:366-383 |
| Validation | Message `Size` must be within `[sizeof(HEADER), MAX_MESSAGE_SIZE]`; otherwise error and buffer reset | CPSock.cpp:397-406 |
| Validation | A full message must have arrived (`Size <= Rest`) before it is returned | CPSock.cpp:408-411 |
| Integrity / Obfuscation | Payload bytes 4..Size-1 are transformed with a position-rotating keyword; checksum = Sum2 - Sum1 | CPSock.cpp:424-466 (recv), 554-584 (send) |
| Integrity / Obfuscation | On checksum mismatch, the packet is still returned but `ErrorCode=1` is set | CPSock.cpp:457-464 |
| Buffering | `AddMessage` rejects writes when the send buffer is full or the socket is invalid, logging the failure | CPSock.cpp:518-533 |
| Buffering | `Receive` returns FALSE when the receive buffer is full (recv filled `Rest` bytes) | CPSock.cpp:341-342 |
| Buffering | Send/recv buffers are compacted in place to reclaim consumed space | CPSock.cpp:593-615 |
| Lifecycle | `CloseSocket` resets all positions and closes the socket handle | CPSock.cpp:84-98 |
| Lifecycle | `StartListen` performs bind/listen/`WSAAsyncSelect(FD_ACCEPT)` with a max of 8 pending connects | CPSock.cpp:128-174 |
| Connection | Client connect binds a local ephemeral port with up to 3 retries (incrementing port by 10) | CPSock.cpp:204-226 |
| Billing | Billing traffic uses a fixed 196-byte (`g_cGame`) un-framed, un-obfuscated protocol | CPSock.cpp:469-511 |
| State / Timestamp | `ClientTick` is stamped with the global `CurrentTime` on every outbound message | CPSock.cpp:541-542 |

### Detailed breakdown of the business rules

---

### Business Rule: Init Code (INITCODE) Handshake

**Overview:**
Every outbound client connection established by CPSock is preceded by a fixed 4-byte binary token (`INITCODE = 0x1F11F311`, defined in `CPSock.h:40`). This acts as a lightweight application-level handshake that the receiving end must verify before it will treat any subsequent bytes as a framed message. The mechanism is asymmetric in practice: the connecting side (`ConnectServer`) writes the token, and the accepting/reading side (`ReadMessage`) enforces it via the per-instance `Init` flag. It is not a cryptographic handshake; it is a fixed magic-number sentinel.

**Detailed description:**
The handshake exists because the wire protocol is otherwise self-describing only through the `HEADER` structure, and the parser needs an unambiguous signal that a stream is a genuine W2PP peer rather than arbitrary data. When `ConnectServer` successfully connects (`CPSock.cpp:247`), it sets `Sock = tSock`, sends the 4 raw bytes of `INITCODE` via a direct `send()` call (`CPSock.cpp:250`), and then sets `Init = 1` (`CPSock.cpp:252`). Notably, the *sending* side sets `Init = 1` unconditionally after sending, so its own flag is not really used as a guard on the outbound path; the enforcement happens entirely on the receiving side.

On the receiving side, `ReadMessage` begins each call by checking `Init == 0` (`CPSock.cpp:366`). While in this uninitialized state, it requires at least 4 bytes to be buffered (`nRecvPosition - nProcPosition < 4` returns NULL, `CPSock.cpp:368-369`). It then reads the next 4 bytes as an unsigned int and compares against `INITCODE`. A mismatch sets `*ErrorCode = 2` and `*ErrorType = InitCode` and returns NULL (`CPSock.cpp:373-379`); the caller (`Server.cpp` WSA handlers) treats error codes 1 and 2 as terminal for that read batch and closes/logs the connection. On a match, `Init` is set to 1 and `nProcPosition` advances past the 4-byte token (`CPSock.cpp:381-382`), after which the normal `HEADER` framing parser takes over. Because `Init` is a per-instance field, each socket object maintains its own handshake state, which matters because `TMSrv` and `DBSrv` each instantiate several independent `CPSock` objects (listen, DB, bill, admin, and per-user sockets).

The business effect is that any stream that does not begin with the exact 4-byte magic is rejected at the first message read, providing a coarse sanity barrier. However, because the token is a public constant compiled into the binary and sent in cleartext, it offers no real security against a determined peer; it is best characterized as a protocol version/identify marker rather than an authentication mechanism.

**Rule workflow:**
```
1. ConnectServer completes connect()
2. send(Sock, &INITCODE, 4, 0)  ; raw 4-byte magic
3. Init = 1 (sending side)
4. Receiving side ReadMessage: Init==0? -> need >=4 bytes buffered
5. Read 4 bytes; compare to INITCODE
6. Mismatch -> ErrorCode=2, ErrorType=badcode, return NULL (connection flagged/logged)
7. Match -> Init=1, nProcPosition+=4, proceed to HEADER parsing
```

---

### Business Rule: Message Framing and Size Validation

**Overview:**
After the init handshake, all non-billing traffic is framed by a 10-byte `HEADER` (Size: short, KeyWord: char, CheckSum: char, Type: short, ID: short, ClientTick: unsigned int — see `CPSock.h:42-50`). `ReadMessage` validates the frame before exposing the payload: it requires at least `sizeof(HEADER)` bytes buffered, verifies the declared `Size` is within allowed bounds, and waits until the full declared frame has arrived before returning the message.

**Detailed description:**
The frame size is bounded by two constants: a lower bound of `sizeof(HEADER)` (10 bytes, since the header is itself considered the minimum message) and an upper bound of `MAX_MESSAGE_SIZE` (8192, `CPSock.h:38`). The check at `CPSock.cpp:386-387` first requires `nRecvPosition - nProcPosition >= sizeof(HEADER)`; if fewer bytes are buffered the parser returns NULL (signalling "not enough data yet"). Next, it reads `Size` (offset 0), `iKeyWord` (offset 2), `CheckSum` (offset 3), and — though unused — reads `SockType` and `SockID` as 4-byte unsigned ints at offsets +4 and +6 (`CPSock.cpp:390-395`). These two reads are effectively dead code: the values are never used anywhere in the function.

The size guard at `CPSock.cpp:397-406` is critical: if `Size > MAX_MESSAGE_SIZE` or `Size < sizeof(HEADER)`, the parser treats the frame as corrupt. In this case it resets both `nRecvPosition` and `nProcPosition` to 0 (discarding the entire receive buffer), sets `*ErrorCode = 2` and `*ErrorType = Size`, and returns NULL. Resetting the whole buffer is a deliberate corruption-recovery measure: an out-of-range `Size` means the stream is unrecoverably desynchronized, so all buffered bytes are dropped rather than attempting partial recovery.

The final framing check (`CPSock.cpp:408-411`) is a partial-packet wait: it computes `Rest = nRecvPosition - nProcPosition` and, if `Size > Rest`, returns NULL so the caller can receive more data and call again. This is what makes the parser incremental — a single `Receive()` may not contain a complete message, and `ReadMessage` returns NULL until the full frame is available. Once the full frame is present, the payload pointer is taken, `nProcPosition` advances by `Size`, and if the buffer is fully consumed both positions reset to 0 (`CPSock.cpp:418-422`).

The business significance is that this is a length-prefixed framing protocol: the receiver relies entirely on the sender's declared `Size` to delimit messages. This creates a trust dependency — a malicious or buggy sender could declare a large `Size`, and while the `MAX_MESSAGE_SIZE` cap bounds the damage, the frame boundaries are not independently verifiable beyond the checksum.

**Rule workflow:**
```
1. Init handshake passed (Init==1)
2. Require >= sizeof(HEADER) bytes buffered; else return NULL
3. Parse Size/KeyWord/CheckSum from header bytes
4. If Size out of [sizeof(HEADER), MAX_MESSAGE_SIZE]: reset buffers, ErrorCode=2, ErrorType=Size, return NULL
5. Rest = nRecvPosition - nProcPosition
6. If Size > Rest: return NULL (incomplete frame; wait for more data)
7. pMsg = pRecvBuffer + nProcPosition; nProcPosition += Size
8. If buffer fully consumed, reset positions to 0
```

---

### Business Rule: Payload Obfuscation and Checksum Integrity

**Overview:**
CPSock implements a proprietary byte-transformation ("obfuscation") scheme over the message payload, driven by a 512-byte static keyword table `pKeyWord` (`CPSock.cpp:29-46`, labeled as "7.xx keys"). Each message selects a random keyword index (`iKeyWord`, 0-255); the table maps each index to two bytes (`pKeyWord[iKeyWord*2]` used as a rotation seed, and `pKeyWord[iKeyWord*2+1]` used as a per-byte transformation operand). Payload bytes from index 4 to `Size-1` are transformed by adding or subtracting shifted forms of the operand based on the byte position mod 4, while a running checksum captures the difference between the raw and transformed sums. On receive, the inverse transform is applied in place and the checksum is re-verified.

**Detailed description:**
The scheme's core is a position-dependent arithmetic transform. In `AddMessage` (`CPSock.cpp:554-584`), for each byte `i` from 4 to `Size-1`, the transform depends on `mod = i & 0x3`:
- `mod == 0`: `out = in + (Trans << 1)`
- `mod == 1`: `out = in - (Trans >> 3)`
- `mod == 2`: `out = in + (Trans << 2)`
- `mod == 3`: `out = in - (Trans >> 5)`

where `Trans = pKeyWord[(pos % 256) * 2 + 1]` and `pos` starts at the chosen `KeyWord = pKeyWord[iKeyWord * 2]` and increments per byte. The keyword index `iKeyWord` is chosen randomly (`rand() % 256`, `CPSock.cpp:535`), and the keyword/seed pair is stored in the header so the receiver can reproduce the transform. `Sum1` accumulates the raw (`pMsg[i]`) bytes, `Sum2` accumulates the transformed (`pSendBuffer[...]`) bytes, and the header's `CheckSum` is stored as `Sum2 - Sum1` (`CPSock.cpp:583-584`).

On receive, `ReadMessage` (`CPSock.cpp:424-466`) applies the inverse: it reads the same `KeyWord` and `Trans` operands, and for `mod` 0/2 it subtracts, for 1/3 it adds (undoing the send-side operation), modifying `pMsg[i]` in place. It separately computes `Sum2` over the raw wire bytes (before transformation) and `Sum1` over the decrypted bytes, then `Sum = Sum2 - Sum1`. Because the wire bytes equal the send-side transformed bytes and the decrypted bytes equal the send-side raw bytes, the receive-side `Sum` should equal the stored `CheckSum`. A mismatch means the frame was corrupted in transit or transformed by an incompatible key set, and `*ErrorCode = 1` is set with `*ErrorType = Size` (`CPSock.cpp:458-463`).

A distinctive design decision is that the checksum failure does **not** drop the packet: the comment at `CPSock.cpp:457` states "return packet, even check_sum not match", and the code indeed returns `pMsg` with `ErrorCode = 1`. The consuming loop (`Server.cpp:3990-3999`) treats error 1 as a reason to break out of the read loop (effectively discarding subsequent processing for that batch) rather than closing the connection outright. This is a fail-soft integrity policy — it surfaces corruption but does not immediately tear down the socket. Note also that the first four header bytes (`Size`, `KeyWord`, `CheckSum`) travel in cleartext (only `memcpy`-copied at `CPSock.cpp:586`), while `Type`, `ID`, and `ClientTick` (offsets 4-9) are obfuscated like the payload.

This is obfuscation, not encryption: the transform is a fixed reversible arithmetic (shift/add/subtract) using a table hard-coded in the binary, so it provides no confidentiality against a reverse engineer but does deter casual packet inspection and catches accidental corruption. The `Sum2 - Sum1` construction means the checksum only validates the transform consistency, not the absolute integrity of the cleartext header fields.

**Rule workflow (send):**
```
1. iKeyWord = rand()%256; KeyWord = pKeyWord[iKeyWord*2]
2. Write header: Size, KeyWord(iKeyWord), CheckSum(0), ClientTick=CurrentTime
3. For i=4..Size-1: transform pMsg[i] by position (mod 4) using pKeyWord; Sum1+=pMsg[i]; Sum2+=transformed
4. CheckSum = Sum2 - Sum1; write back into header
5. memcpy first 4 bytes (Size/KeyWord/CheckSum) into send buffer
```

**Rule workflow (receive):**
```
1. Read KeyWord seed from header; recompute Trans operands from pKeyWord
2. For i=4..Size-1: inverse-transform pMsg[i] in place; Sum2+=raw; Sum1+=decrypted
3. Sum = Sum2 - Sum1
4. If Sum != CheckSum: ErrorCode=1, ErrorType=Size, but still return pMsg (fail-soft)
5. Else return pMsg (valid)
```

---

### Business Rule: Transmit Buffer Management and Socket Validity Guarding

**Overview:**
Outbound messages are not written directly to the socket. `AddMessage` appends framed/obfuscated bytes to a shared `pSendBuffer` (128 KB), and `SendMessageA` flushes buffered bytes to the socket in a single `send()` attempt. Two explicit guards in `AddMessage` reject writes: a buffer-full condition and an invalid-socket condition, each of which produces a diagnostic log via the external `Log()` function.

**Detailed description:**
The send buffer is a heap-allocated `SEND_BUFFER_SIZE` (128 KB, `CPSock.h:36`) byte array. `AddMessage` first checks `if (nSendPosition + Size >= SEND_BUFFER_SIZE)` (`CPSock.cpp:518`) — if appending the new message would overflow the buffer, it formats a diagnostic string including `nSendPosition`, `Size`, the message `Type`, and the socket, logs it via `Log(temp, "-system", 0)`, and returns FALSE (`CPSock.cpp:519-524`). This is a fail-fast policy: the message is not queued, and the caller receives FALSE (many call sites in `SendFunc.cpp` check the return and treat FALSE as a send failure, e.g. `SendFunc.cpp:556`, `799`).

The second guard checks socket validity: `if (Sock <= 0)` (`CPSock.cpp:527`) produces a similar diagnostic log and returns FALSE (`CPSock.cpp:528-533`). This prevents enqueueing data onto a closed or never-opened socket. The combination means CPSock will never grow its fixed send buffer beyond its allocation; instead it silently (well, logged) drops the message and returns an error to the caller.

`SendMessageA` (`CPSock.cpp:617-684`) is the flush path. It first guards `Sock <= 0` (resetting positions and returning FALSE, `CPSock.cpp:623-629`), compacts the buffer if there are previously-sent bytes outstanding via `RefreshSendBuffer()` (`CPSock.cpp:631-632`), and runs two defensive bounds checks on `nSendPosition`/`nSentPosition` (resetting both to 0 on anomalies, `CPSock.cpp:634-656`). It then performs exactly one `send()` of all unsent bytes (`CPSock.cpp:662`). If `send` succeeds, `nSentPosition` advances by the transmitted byte count (`CPSock.cpp:665`); on `SOCKET_ERROR` it records `WSAGetLastError()` (`CPSock.cpp:667`). If all buffered bytes were transmitted, both positions reset to 0 and it returns TRUE (`CPSock.cpp:671-677`); otherwise (partial send or error) it leaves the positions intact so a later flush can retry the remainder, and returns TRUE at `CPSock.cpp:683` unless the buffer overran.

This buffering design supports the project's batching pattern: business handlers call `AddMessage` repeatedly to queue multiple messages, then call `SendMessageA` once to flush them together, improving throughput and enabling grouped multicast/broadcast writes (e.g. `SendFunc.cpp:269-283` flush after adding to many users).

**Rule workflow:**
```
1. AddMessage: if nSendPosition+Size >= SEND_BUFFER_SIZE -> log + return FALSE
2. AddMessage: if Sock <= 0 -> log + return FALSE
3. AddMessage: obfuscate + append Size bytes; nSendPosition += Size; return TRUE
4. SendMessageA: if Sock<=0 -> reset + return FALSE
5. SendMessageA: if nSentPosition>0 -> RefreshSendBuffer (compact)
6. SendMessageA: single send() of (nSendPosition-nSentPosition) bytes
7. Full send -> reset positions, return TRUE; partial/error -> keep positions for retry, return TRUE
```

---

### Business Rule: Receive Buffer Management and Full-Buffer Signaling

**Overview:**
Inbound data is read via `Receive()`, which issues a single blocking-free `recv()` into the receive buffer at the current write position and advances the position by the number of bytes received. The method returns FALSE when the socket returns an error or when the buffer is completely filled by the read, signalling that the buffer has no remaining capacity.

**Detailed description:**
`Receive()` (`CPSock.cpp:336-347`) computes `Rest = RECV_BUFFER_SIZE - nRecvPosition` (the remaining capacity in the 128 KB receive buffer), then calls `recv(Sock, pRecvBuffer + nRecvPosition, Rest, 0)`. The key business condition is at `CPSock.cpp:341-342`: if `tReceiveSize == SOCKET_ERROR` **or** `tReceiveSize == Rest`, it returns FALSE. The second clause is subtle — it treats a read that filled the entire remaining buffer as an error condition rather than a success. The rationale is that filling the buffer exactly leaves no room for the incremental `ReadMessage` parser to grow, and (critically) the server-side handler interprets a FALSE return from `Receive()` as "receive failed" and triggers a retry or a `CloseUser`/connection teardown (`Server.cpp:3940-3956`, `DBSrv/Server.cpp:904-950`). Otherwise, `nRecvPosition` is advanced by `tReceiveSize` and TRUE is returned.

The receive path is complemented by `RefreshRecvBuffer()` (`CPSock.cpp:593-603`), which compacts consumed bytes: it computes `left = nRecvPosition - nProcPosition` (bytes not yet consumed by the parser) and, if positive, memcpy's them to the front of the buffer, resetting `nProcPosition = 0` and `nRecvPosition -= left`. This reclaims the front of the buffer that `ReadMessage` has already de-framed, preventing the write position from marching monotonically toward the end of the fixed allocation over a long-lived connection.

A notable consequence of the full-buffer-as-FALSE policy is that a legitimate flood of data that exactly fills the buffer is indistinguishable from a socket error from the caller's perspective, which in the server handlers leads to a connection close. This is a design trade-off favoring simple error handling over graceful backpressure.

**Rule workflow:**
```
1. Rest = RECV_BUFFER_SIZE - nRecvPosition
2. tReceiveSize = recv(Sock, pRecvBuffer+nRecvPosition, Rest, 0)
3. If tReceiveSize == SOCKET_ERROR OR tReceiveSize == Rest -> return FALSE (signal failure/full)
4. nRecvPosition += tReceiveSize; return TRUE
5. (Compaction) RefreshRecvBuffer: move remaining unprocessed bytes to front; reset positions
```

---

### Business Rule: Billing Server Fixed-Size Protocol

**Overview:**
A separate, simplified code path handles communication with the billing server. Unlike the main framed/obfuscated protocol, the billing path (`ReadBillMessage`/`SendBillMessage`) uses a fixed 196-byte message size (`g_cGame = sizeof(_AUTH_GAME)`, `CPSock.h:132`) with no `HEADER`, no keyword obfuscation, and no checksum. It is a plain block-oriented transfer of fixed-size records.

**Detailed description:**
`SendBillMessage` (`CPSock.cpp:498-511`) first checks `if (nSendPosition + g_cGame >= SEND_BUFFER_SIZE)` (`CPSock.cpp:500`) and returns FALSE if the block would overflow. It then copies the 196 bytes into the send buffer, advances `nSendPosition`, and immediately calls `SendMessage()` (a typo-renamed call that resolves to `SendMessageA`) to flush the block, returning the flush result. `ReadBillMessage` (`CPSock.cpp:469-496`) requires `nRecvPosition - nProcPosition >= g_cGame` (`CPSock.cpp:482`) to have a full block available; it returns NULL if fewer than 196 bytes are buffered. On success it returns a pointer to the block, advances `nProcPosition` by 196, and resets positions when the buffer is fully consumed.

The `_AUTH_GAME` structure is notable: it is currently defined as a single opaque 196-byte `char Unk[196]` field (`CPSock.h:88-91`) with a comment indicating the real layout "NEEDS TO BE FIXED ACCORDING TO WYD 1.2 6.13 SIZE IS 0xC4 (196)". The commented-out `TANTRA` structure below it (`CPSock.h:93-130`) shows the intended fields (Packet_Type, Result, S_KEY, Session, User_CC, User_No, User_ID, User_IP, User_Gender, User_Status, User_PayType, User_Age, Game_No, Bill_PayType, Bill_Method, Bill_Expire, Bill_Remain). This indicates the billing protocol's on-the-wire structure is only partially reverse-engineered; the actual byte layout is treated as opaque and passed through by address.

The billing path is driven by the `WSA_READBILL` window message (`Server.cpp:3731-3810`), which calls `BillServerSocket.ReadBillMessage()` in a loop and dispatches each block to `ProcessBILLMessage()`. Reconnection logic lives in `ProcessSecMinTimer.cpp:130-167`: if `BillServerSocket.Sock == 0` and `BillCounter` counts down, it calls `ConnectBillServer` and, on success, re-sends a billing login block via `SendBilling2(&Unk, 4)`.

**Rule workflow:**
```
SendBillMessage:
1. If nSendPosition + g_cGame >= SEND_BUFFER_SIZE -> return FALSE
2. Copy 196 bytes into send buffer; nSendPosition += 196
3. Flush via SendMessageA(); return flush result

ReadBillMessage:
1. If nProcPosition >= nRecvPosition -> reset positions, return NULL
2. If nRecvPosition - nProcPosition < g_cGame -> return NULL (incomplete block)
3. Return block pointer; nProcPosition += 196; reset positions if buffer fully consumed
```

---

### Business Rule: Connection Lifecycle (Listen, Connect, Close)

**Overview:**
CPSock manages the full Winsock connection lifecycle. `StartListen` sets up a listening socket; `ConnectServer` and `ConnectBillServer` establish outbound client connections (with local-port binding fallback logic); `CloseSocket` tears down a socket and resets all buffer state. All sockets are registered for asynchronous event notification via `WSAAsyncSelect`, tying the socket to the owning window handle `hWnd`/`hWndMain` and a designated `WSA_*` window message.

**Detailed description:**
`StartListen` (`CPSock.cpp:128-174`) creates a TCP socket, binds it to the supplied IP (or INADDR_ANY via the caller) and port, calls `listen()` with `MAX_PENDING_CONNECTS` (8, `CPSock.h:33`) backlog, and registers `WSAAsyncSelect(tSock, hWnd, WSA, FD_ACCEPT)`. Each step checks the relevant error return and, on failure, pops a `MessageBox` and closes the socket, returning FALSE/0. On success it stores the socket in `Sock` and returns it. Callers pass the WSA message constant (e.g. `WSA_ACCEPT`) and the window handle; `TMSrv` calls `ListenSocket.StartListen(hWndMain, *pip, GAME_PORT, WSA_ACCEPT)` (`TMSrv/Server.cpp:3634`) and `DBSrv` calls it twice (`DB_PORT`, `ADMIN_PORT` — `DBSrv/Server.cpp:586-587`).

`ConnectServer` (`CPSock.cpp:176-255`) is the outbound client path. It resets all positions, closes any existing socket, creates a socket, and binds it to the local IP. Binding uses a retry ladder: if the first bind fails it increments the global `ConnectPort` by 10 and rebinds with `port = ConnectPort + 5000`, retrying up to three times before giving up with a `MessageBox` (`CPSock.cpp:208-226`). After binding it calls `connect()`, and on success registers `WSAAsyncSelect(tSock, hWndMain, WSA, FD_READ | FD_CLOSE)` (`CPSock.cpp:239`). It then sends the 4-byte `INITCODE` and sets `Init = 1` (see the handshake rule). `ConnectBillServer` (`CPSock.cpp:257-334`) mirrors this but uses a different port offset (`ConnectPort + 6000`), does not send `INITCODE`, logs bind/connect diagnostics via `sprintf` into a local `msg` buffer (a suspicious dead-log — the buffer is populated but the contents are never written to any sink), and registers `WSAAsyncSelect` for `FD_READ | FD_CLOSE`.

`CloseSocket` (`CPSock.cpp:84-98`) resets all four position counters and `Init` to 0, calls `closesocket(Sock)` if the socket handle is non-zero, and zeroes `Sock`. This is the canonical teardown used by all the server handlers on connection loss (e.g. `Server.cpp:3737`, `3823`, and the retry/reconnect paths).

The lifecycle is entirely event-driven: after `StartListen`/`ConnectServer`, subsequent I/O is driven by `WSAAsyncSelect` window messages (`WSA_ACCEPT`, `WSA_READ`, `WSA_READDB`, `WSA_READBILL`, `WSA_READADMIN`, `WSA_READADMINCLIENT`) that the server window procedure routes to the corresponding `Receive`/`ReadMessage`/`ReadBillMessage` loops.

**Rule workflow:**
```
StartListen:
1. socket(AF_INET, SOCK_STREAM) -> bind -> listen(backlog=8) -> WSAAsyncSelect(FD_ACCEPT)
2. Any failure -> MessageBox + closesocket + return FALSE

ConnectServer:
1. Reset positions; close existing socket if any
2. socket + bind(local ip, port 0); on bind failure retry with ConnectPort+5000 (+10 per retry, up to 3)
3. connect(remote) -> WSAAsyncSelect(FD_READ|FD_CLOSE)
4. send INITCODE; Init=1

CloseSocket:
1. Reset positions + Init=0
2. closesocket(Sock); Sock=0
```

---

### Business Rule: Global Timestamp Stamping

**Overview:**
Every outbound message queued via `AddMessage` is stamped with the project-global `CurrentTime` value into the header's `ClientTick` field, and the global `LastSendTime` is updated to the same value. This couples the socket layer to two externally-declared time globals.

**Detailed description:**
`AddMessage` sets `pSMsg->ClientTick = CurrentTime` and `LastSendTime = CurrentTime` (`CPSock.cpp:541-542`). `CurrentTime` is declared as `extern unsigned int CurrentTime;` (`CPSock.cpp:49`) and is maintained by the servers (e.g. `ProcessSecMinTimer.cpp:125` sets `CurrentTime = timeGetTime()`). `LastSendTime` (`CPSock.cpp:50`) is likewise a shared global. This rule means the socket layer does not own its clock; it relies on the host process to keep `CurrentTime` current. The `ClientTick` field is part of the `HEADER`/`_MSG` layout and is transmitted (obfuscated) to the peer, serving as a client timestamp the receiving application can use for latency/ordering analysis. The practical coupling risk is that `CurrentTime` is only refreshed periodically by the server timer (`ProcessSecMinTimer`), so `ClientTick` values reflect the last timer tick rather than a true per-message timestamp, and any thread that calls `AddMessage` concurrently with the timer thread would read/write unsynchronized globals.

**Rule workflow:**
```
1. AddMessage receives a message with header
2. pSMsg->ClientTick = CurrentTime (global, maintained by host timer)
3. LastSendTime = CurrentTime (global "last activity" marker)
4. Message is obfuscated (including ClientTick at offset 8-9) and queued
```

---

## 4. Component Structure

CPSock is a two-file component located at the shared project root `legacy/Code/`, compiled into both server projects:

```
legacy/Code/
├── CPSock.h                  # Class declaration, wire constants, HEADER struct, _AUTH_GAME struct
├── CPSock.cpp                # Implementation (693 lines), pKeyWord table, globals, all methods
└── (consumers)
    ├── TMSrv/                # Game/Map server — includes ../CPSock.h in many files
    └── DBSrv/                # Database server — includes ../CPSock.h in many files
```

### CPSock.h structure (`legacy/Code/CPSock.h`)

```
CPSock.h
├── Include guard _CPSOCK_ (line 20)
├── #include <Windows.h> (line 23)
├── Window message constants (lines 25-31):
│     WSA_READ, WSA_READDB, WSA_ACCEPT, WSA_READBILL,
│     WSA_ACCEPTADMIN, WSA_READADMIN, WSA_READADMINCLIENT
├── MAX_PENDING_CONNECTS = 8 (line 33)
├── RECV_BUFFER_SIZE = 128*1024 (line 35)   # comment says 64k (stale)
├── SEND_BUFFER_SIZE = 128*1024 (line 36)   # comment says 64K (stale)
├── MAX_MESSAGE_SIZE = 8192 (line 38)       # comment says 4K (stale)
├── INITCODE = 0x1F11F311 (line 40)
├── typedef struct _HEADER (lines 42-50): Size, KeyWord, CheckSum, Type, ID, ClientTick
├── class CPSock (lines 53-86):
│     Public data: Sock, pSendBuffer, pRecvBuffer,
│                  nSendPosition, nRecvPosition, nProcPosition, nSentPosition, Init
│     Public methods: constructor/destructor, CloseSocket, WSAInitialize, StartListen,
│                     ConnectServer, ConnectBillServer, Receive, ReadMessage,
│                     AddMessage, SendMessageA, SendOneMessage, RefreshRecvBuffer,
│                     RefreshSendBuffer, SendBillMessage, ReadBillMessage
├── struct _AUTH_GAME (lines 88-91): opaque 196-byte char Unk[196]
├── (commented-out TANTRA _AUTH_GAME / _AUTH_GAME2 reference layouts, lines 93-130)
└── g_cGame = sizeof(_AUTH_GAME) = 196 (line 132)
```

### CPSock.cpp structure (`legacy/Code/CPSock.cpp`)

```
CPSock.cpp (693 lines)
├── Includes: <Windows.h>, <stdio.h>, <stdlib.h>, "CPSock.h", "Basedef.h" (lines 20-25)
├── int ConnectPort = 0; (line 27)  # global local-port offset counter
├── unsigned char pKeyWord[512] (lines 29-46)  # static obfuscation keyword table
├── extern HWND hWndMain; extern unsigned int CurrentTime; extern unsigned int LastSendTime;
│     extern void Log(...); (lines 48-51)
├── CPSock::CPSock() (62-73)         # allocates both buffers, zeroes positions
├── CPSock::~CPSock() (75-82)        # frees both buffers
├── CPSock::CloseSocket() (84-98)
├── CPSock::WSAInitialize() (109-117)
├── CPSock::StartListen() (128-174)
├── CPSock::ConnectServer() (176-255)
├── CPSock::ConnectBillServer() (257-334)
├── CPSock::Receive() (336-347)
├── CPSock::ReadMessage() (353-467)  # init check + framing + de-obfuscation + checksum
├── CPSock::ReadBillMessage() (469-496)
├── CPSock::SendBillMessage() (498-511)
├── CPSock::AddMessage() (513-591)   # framing + obfuscation + checksum (send side)
├── CPSock::RefreshRecvBuffer() (593-603)
├── CPSock::RefreshSendBuffer() (605-615)
├── CPSock::SendMessageA() (617-684) # flush path
└── CPSock::SendOneMessage() (686-693) # AddMessage + SendMessageA
```

---

## 5. Dependency Analysis

### Internal Dependencies (within the component and its consumers)

```
Component-internal:
CPSock::SendOneMessage  ->  AddMessage, SendMessageA
CPSock::SendMessageA    ->  RefreshSendBuffer
CPSock::AddMessage      ->  Log, rand(), pKeyWord, CurrentTime, LastSendTime, BASE_CheckPacket (debug)
CPSock::ReadMessage     ->  pKeyWord, extern globals
CPSock::Receive         ->  recv
CPSock::ReadBillMessage ->  g_cGame

Header dependency: CPSock.h <-> Basedef.h (HEADER vs _MSG macro layouts; MSG_STANDARD, BASE_CheckPacket)

Consumer -> CPSock (afferent):
TMSrv/Server.cpp              -> CPSock instances: ListenSocket, DBServerSocket, BillServerSocket
TMSrv/CUser.h                 -> CUser::cSock (CPSock member)
TMSrv/SendFunc.cpp            -> pUser[].cSock (AddMessage/SendMessageA/SendOneMessage)
TMSrv/_MSG_*.cpp (many)       -> pUser[].cSock, DBServerSocket
TMSrv/ProcessSecMinTimer.cpp  -> BillServerSocket.ConnectBillServer/CloseSocket
TMSrv/ProcessClientMessage.cpp / ProcessDBMessage.cpp -> include CPSock.h
DBSrv/Server.cpp              -> CPSock instances: ListenSocket, AdminSocket, AdminClient
DBSrv/CUser.h                 -> CUser::cSock (CPSock member)
DBSrv/CFileDB.cpp             -> extern CPSock AdminClient; pUser[].cSock
DBSrv/CRanking.cpp            -> pUser[tmsrvId].cSock.SendOneMessage
DBSrv/CReadFiles.cpp          -> pUser[svr].cSock.SendOneMessage
```

### External Dependencies

| Dependency | Type | Purpose |
|-----------|------|---------|
| WinSock 1.1 (WSAStartup MAKEWORD(1,1)) | OS API | Socket transport (`CPSock.cpp:113`) |
| Windows.h | OS API | HWND, MessageBox, WM_* window messaging, closesocket |
| rand() (stdlib) | C runtime | Random keyword index selection for obfuscation (`CPSock.cpp:535`) |
| malloc/free (stdlib) | C runtime | Send/recv buffer allocation (`CPSock.cpp:66-67, 78-81`) |
| Basedef.h | Project shared header | MSG_STANDARD / _MSG layout, BASE_CheckPacket, message-type constants |
| Host process globals | Project | `hWndMain`, `CurrentTime`, `LastSendTime`, `Log()` (defined in Server.cpp / Basedef.cpp) |
| Visual Studio projects | Build | CPSock.cpp compiled into TMSrv.vcxproj and DBSrv.vcxproj |

Note: `WSAInitialize` is declared but never called by the consumers surveyed; the servers rely on Winsock being initialized elsewhere or implicitly. This is an unverified/mostly-dead API.

---

## 6. Afferent and Efferent Coupling

Coupling is analyzed at the CPSock public-method level (the "components" in this C++ class context), with afferent coupling measured by the number of distinct consumer call sites across the two server modules, and efferent coupling measured by the number of external/global dependencies each method touches.

| Component (CPSock method) | Afferent Coupling (call sites) | Efferent Coupling | Critical |
|---------------------------|-------------------------------|-------------------|----------|
| SendOneMessage | ~60 (DBSrv + TMSrv) | 2 (AddMessage, SendMessageA) | High |
| AddMessage | ~70 (SendFunc.cpp, _MSG_*.cpp) | 5 (Log, rand, pKeyWord, CurrentTime, LastSendTime) | High |
| SendMessageA | ~12 | 2 (RefreshSendBuffer, send) | High |
| ReadMessage | 3 (TMSrv Server) + 3 (DBSrv Server) | 2 (pKeyWord, globals) | High |
| Receive | 5 (WSA handlers) | 1 (recv) | High |
| ConnectServer | 3 | 4 (socket/bind/connect/WSAAsyncSelect/send, ConnectPort) | Medium |
| ConnectBillServer | 2 | 4 (socket/bind/connect/WSAAsyncSelect, ConnectPort) | Medium |
| StartListen | 3 | 3 (socket/bind/listen/WSAAsyncSelect) | Medium |
| CloseSocket | 8+ (disconnect/reconnect paths) | 1 (closesocket) | Medium |
| SendBillMessage | 1 | 2 (AddMessage-like copy, SendMessageA) | Low |
| ReadBillMessage | 1 | 1 (g_cGame) | Low |
| RefreshRecvBuffer / RefreshSendBuffer | internal (2) | 1 (memcpy) | Low |
| WSAInitialize | 0 (declared, unused) | 1 (WSAStartup) | Low |

Consumer-module afferent view (files that embed/use CPSock): TMSrv/Server.cpp, TMSrv/CUser.h, TMSrv/SendFunc.cpp, ~20 TMSrv/_MSG_*.cpp files, TMSrv/ProcessSecMinTimer.cpp, TMSrv/ProcessClientMessage.cpp, TMSrv/ProcessDBMessage.cpp, DBSrv/Server.cpp, DBSrv/CUser.h, DBSrv/CFileDB.cpp, DBSrv/CRanking.cpp, DBSrv/CReadFiles.cpp. The class is therefore a shared, widely-invoked foundational transport with high fan-in.

---

## 7. Endpoints

CPSock is a transport/library abstraction and does not expose application-level endpoints (no REST, GraphQL, gRPC, or HTTP). It provides Winsock-based socket connection primitives (`StartListen`, `ConnectServer`, `ConnectBillServer`) and window-message events (`WSA_ACCEPT`, `WSA_READ`, `WSA_READDB`, `WSA_READBILL`, `WSA_READADMIN`, `WSA_READADMINCLIENT`) consumed by the server window procedures. Per the analysis guidelines, the Endpoints section is omitted because the component does not expose endpoints.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Game clients (TMSrv) | External clients | Client <-> game server traffic | TCP / WSAAsyncSelect (WSA_READ) | Obfuscated header-framed binary (HEADER + payload) | Receive retry once, then CloseUser; ReadMessage Error 1/2 break loop |
| DBSrv (TMSrv) | Internal server link | Game <-> DB server messages | TCP / WSAAsyncSelect (WSA_READDB) | Obfuscated header-framed binary | On FD_CLOSE, reconnect up to 2 attempts, else PostQuitMessage |
| Bill server (TMSrv) | External service | Billing/auth (SendBilling2) | TCP / WSAAsyncSelect (WSA_READBILL) | Fixed 196-byte blocks, no framing/obfuscation | On receive failure, CloseSocket + BillCounter=360 countdown reconnect |
| Admin clients (DBSrv) | External clients | Admin console traffic | TCP / WSAAsyncSelect (WSA_READADMIN / WSA_READADMINCLIENT) | Obfuscated header-framed binary | Receive retry once, else close admin user |
| OS Winsock (all) | System API | Transport | WinSock 1.1 | Raw bytes | SOCKET_ERROR -> WSAGetLastError -> retry/close |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Message-Oriented Transport / Facade | CPSock wraps raw Winsock into framed message send/recv | CPSock.cpp (all methods) | Hide socket complexity behind a message API |
| Event-driven async I/O | WSAAsyncSelect + window messages | CPSock.h:25-31, CPSock.cpp:163,239,323 | Non-blocking socket notifications routed to window proc |
| Framing / Length-prefixed protocol | HEADER.Size + MAX_MESSAGE_SIZE bound | CPSock.cpp:390-411 | Delimit variable-length messages on a byte stream |
| Obfuscation + integrity check | pKeyWord table + Sum2-Sum1 checksum | CPSock.cpp:424-466, 554-584 | Deter casual inspection; detect corruption |
| Buffer pool / ring-with-compaction | Fixed 128KB buffers + Refresh*Buffer compaction | CPSock.cpp:593-615 | Reuse fixed buffers without unbounded growth |
| Handshake/state flag | Init flag + INITCODE sentinel | CPSock.cpp:249-252, 366-383 | Gate protocol parsing until peer identified |
| Data-transfer-object pattern (implicit) | Callers pass binary structs (MSG_STANDARD, etc.) as opaque byte payloads | Consumers (_MSG_*.cpp) | Standardized message structs shared via Basedef.h |

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | AddMessage/ReadMessage | No input size checks on `recv`/buffer math beyond MAX_MESSAGE_SIZE; relies entirely on peer-declared Size for framing | Malicious/corrupt peer could declare large sizes; only bounded by 8192 cap, but frame boundaries not independently verifiable |
| High | ReadMessage (CPSock.cpp:394-395) | `SockType`/`SockID` read as 4-byte unsigned ints at offsets +4/+6 but never used (dead code / likely left-over debug) | Obscures intent; misaligned reads of the HEADER fields |
| High | SendBillMessage (CPSock.cpp:508) | Calls `SendMessage()` (undecorated) which only compiles because it maps to `SendMessageA`; fragile naming | Compile-time portability risk; unclear intent |
| High | Obfuscation (pKeyWord) | Static table and transform hard-coded in binary; "obfuscation" provides no real confidentiality | Not a security boundary; easily reverse-engineered |
| Medium | Global state (ConnectPort, CurrentTime, LastSendTime) | Shared non-atomic globals across all socket instances and threads | Race conditions; cross-instance port allocation coupling; stale timestamps |
| Medium | ConnectBillServer (CPSock.cpp:259,300,311,320) | `char msg[256]` populated via sprintf but never written to a log/sink | Dead logging code; diagnostics lost |
| Medium | Stale/contradictory comments | RECV_BUFFER_SIZE/SEND_BUFFER_SIZE comment "64k" vs value 128KB; MAX_MESSAGE_SIZE comment "4K" vs 8192; header comment "Last updated august 2012" | Misleading documentation |
| Medium | Receive() full-buffer policy (CPSock.cpp:341) | `tReceiveSize == Rest` treated as FALSE/error | Legitimate full-buffer reads are indistinguishable from errors -> connection close |
| Medium | Error handling inconsistency | Mix of MessageBox, Log(), and out-parameter error codes; some paths MessageBox on server (blocking UI) | Non-uniform failure handling; UI popups in server context |
| Medium | _AUTH_GAME opaque (CPSock.h:88-91) | Billing record defined as 196 opaque bytes; real layout commented out | Billing protocol only partially reverse-engineered; risky if layout wrong |
| Medium | WSAInitialize declared but unused | Dead public API | Dead code; Winsock init assumed elsewhere |
| Low | No per-instance buffer bounds on init | Constructor allocates both buffers with malloc without checking NULL | Unchecked allocation failure (minor in practice) |
| Low | CloseSocket double-close risk | Callers may call CloseSocket after socket already closed; relies on Sock==0 guard (present) | Mostly mitigated by guard |

---

## 11. Test Coverage Analysis

No automated test files were found anywhere in the project. A comprehensive search for test artifacts (files/directories named `*test*`, `*spec*`, `*unittest*`, and test references in the Visual Studio project files) returned no results. The `.vcxproj` files (`legacy/Code/TMSrv/TMSrv.vcxproj`, `legacy/Code/DBSrv/DBSrv.vcxproj`) contain only the production source files (including the shared `..\CPSock.cpp` / `..\CPSock.h`).

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CPSock | 0 | 0 | 0% (none; not instrumented) | N/A — no test suite exists |

Consequences:
- The framing/obfuscation round-trip (AddMessage -> SendMessageA -> Receive -> ReadMessage), which is the correctness-critical symmetric transform, has zero automated verification.
- The checksum logic (`Sum2 - Sum1` both directions) and the `INITCODE` handshake are unverified.
- The partial-packet incremental parser behavior (ReadMessage returning NULL until a full frame arrives) is untested.
- The fixed-size billing protocol (SendBillMessage/ReadBillMessage) is untested.
- The buffer-compaction routines (RefreshRecvBuffer/RefreshSendBuffer) are untested.
- No mock/stub layers exist for the Winsock API, so even the error paths (SOCKET_ERROR handling, full-buffer FALSE) are not exercised by tests.

This is a significant risk for a component that underpins all inter-process and client-server communication in both server binaries. The absence of tests means protocol changes, key-table updates, or buffer-size modifications cannot be safely validated.

---

## 12. Notes on Analysis Boundaries

- Ignored folders (`.git`, `.opencode`) were excluded from the analysis, per the task parameters.
- Analysis scope was restricted to the CPSock component (`legacy/Code/CPSock.h`, `legacy/Code/CPSock.cpp`) and its direct consumers (`legacy/Code/TMSrv/*`, `legacy/Code/DBSrv/*`).
- Some behaviors are implicit or inferred from the surrounding server code (window-message handlers, reconnect logic) because CPSock itself is a transport library; confidence levels for inferred business rules are noted inline.
- The component was analyzed as-is; no source files were modified.

---

*End of Component Deep Analysis Report.*
