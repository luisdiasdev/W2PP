# Component Deep Analysis Report - CPSock Transport

| Field | Value |
|---|---|
| Component | CPSock Transport |
| Primary files | `Code/CPSock.h` (134 lines), `Code/CPSock.cpp` (693 lines) |
| Secondary contract | `Code/Basedef.h` (`_MSG` macro `:925-930`, `MSG_STANDARD` `:943-946`), `Code/Basedef.cpp` (`BASE_CheckPacket` `:6475-6596`) |
| Consumers analyzed | `Code/TMSrv/Server.cpp`, `Code/TMSrv/CUser.cpp`, `Code/TMSrv/ProcessSecMinTimer.cpp`, `Code/DBSrv/Server.cpp`, `Code/DBSrv/CUser.cpp`, `Code/TMSrv/ProcessClientMessage.cpp` |
| Project | W2PP (decompiled W2 MMORPG server emulator, Win32 / MSVC) |
| Build target | `Win32` only; compiled into both `TMSrv` (`Code/TMSrv/TMSrv.vcxproj:92`) and `DBSrv` (`Code/DBSrv/DBSrv.vcxproj:95`) |
| Analysis date | 2026-11-15 22:56:02 |
| Exclusions | `.agents`, `.codegraph`, `.opencode`, `openspec` (not read) |

---

## 1. Executive Summary

`CPSock` is the single network transport abstraction of the W2PP emulator. It is a small, non-polymorphic C++ class (32 bytes of instance state plus two heap buffers) that wraps Winsock 1.1 in the legacy `WSAAsyncSelect` message-driven model. Every byte that crosses a TCP connection in either executable passes through it: game clients on `:8281`, the TMSrv-to-DBSrv link on `:7514`, the DBSrv admin channel on `:8895`, and the outbound billing/side-channel links. It is compiled twice from the same source into two binaries with different Winsock header orders and different CRT settings.

The component defines the entire wire contract of the emulator: a 12-byte header (`Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`), a 512-byte static obfuscation table, a per-byte additive transform keyed by a message-local random byte, an 8-bit additive checksum over the transformed body, a one-dword `INITCODE` handshake, and fixed 128 KiB send/receive buffers that are never resized. There is no schema, no version negotiation beyond the dword, and no encryption.

Key findings, in order of consequence:

1. **Integrity verification is advisory, not enforcing.** `ReadMessage` returns the parsed message pointer even when the checksum does not match; it only sets `*ErrorCode = 1` (`Code/CPSock.cpp:457-464`). Callers must choose to stop, and they choose inconsistently.
2. **A failed `send()` is reported as success.** `SendMessageA` captures `WSAGetLastError()` into a local `err` that is never used, then falls through to `return TRUE` for both partial sends and `SOCKET_ERROR` (`Code/CPSock.cpp:658-683`). The only reachable `FALSE` is `Sock <= 0`.
3. **`SendOneMessage` silently discards messages when the queue is full.** It ignores the `AddMessage` result and returns `SendMessageA()`'s status (`Code/CPSock.cpp:686-692`), so a dropped DB-save packet is indistinguishable from a sent one.
4. **`Receive` classifies a completely filled buffer as an error and loses the bytes.** `if (tReceiveSize == SOCKET_ERROR || tReceiveSize == Rest) return FALSE;` (`Code/CPSock.cpp:341-342`) leaves `nRecvPosition` unchanged, so the retry overwrites the bytes that were just read - a permanent stream desync on that connection.
5. **All five async-arming calls check the wrong success value.** `WSAAsyncSelect` returns `0` on success and `SOCKET_ERROR` on failure; the code tests `> 0` (`Code/CPSock.cpp:163`, `:239`, `:323`; `Code/TMSrv/CUser.cpp:61`; `Code/DBSrv/CUser.cpp:49`), so arming failure is undetectable and the socket is adopted anyway.
6. **No per-message validation exists.** `ReadMessage` enforces only `sizeof(HEADER) <= Size <= 8192` (`:397`). The intended schema check, `BASE_CheckPacket`, has its whole body commented out and returns `FALSE` (`Code/Basedef.cpp:6475-6596`), and its `CPSock` call site is inside `#ifdef _PACKET_DEBUG` (`Code/CPSock.cpp:544-552`), which is defined only in the Debug configuration (`Code/TMSrv/TMSrv.vcxproj:61`).
7. **The billing channel is a different, unframed protocol.** `SendBillMessage`/`ReadBillMessage` treat the stream as a sequence of exactly 196-byte `_AUTH_GAME` records with no header, keyword, checksum or obfuscation (`Code/CPSock.cpp:469-511`, `Code/CPSock.h:88-91,132`). `ReadBillMessage` always reports `ErrorCode = 0` (`:471-472`) and the only inbound consumer, `ProcessBILLMessage`, is an empty function (`Code/TMSrv/Server.cpp:4300-4302`).
8. **The `INITCODE` gate is asymmetric and sticky.** Outbound connections send the magic dword and immediately set `Init = 1` (`:249-252`); inbound connections require it (`:366-383`). On mismatch the code returns without consuming the four bytes or closing, so every subsequent read of that connection reproduces the same error.
9. **Resource cost is fixed and large.** Each instance allocates 262,144 bytes in its constructor with no `NULL` check (`:66-67`). `CUser pUser[MAX_USER]` (`Code/TMSrv/Server.cpp:269`, `MAX_USER = 1000`) therefore commits approximately 250 MiB at static-initialisation time, before `main`.
10. **There is no automated test infrastructure anywhere in the repository.** No test project, no test source, no assertion framework, no CI definition; the prebuilt `Code/release/TMSrv.exe` (5,094,440 bytes, 2026-11-06) is the only executable evidence of behaviour.

---

## 2. Data Flow Analysis

### 2.1 Position semantics

Four integers define the component's state machine (`Code/CPSock.h:56-63`):

| Field | Meaning |
|---|---|
| `nRecvPosition` | End of valid received bytes in `pRecvBuffer` (append offset) |
| `nProcPosition` | Start of the next unconsumed frame |
| `nSendPosition` | End of queued bytes in `pSendBuffer` |
| `nSentPosition` | Start of bytes not yet accepted by `send()` |

Invariants intended by the code: `nProcPosition <= nRecvPosition <= RECV_BUFFER_SIZE`, `nSentPosition <= nSendPosition <= SEND_BUFFER_SIZE`. The first invariant is deliberately violated by the sticky-init-error path (nothing consumed, nothing reset), and the second is violated transiently whenever `AddMessage` is called with a negative `Size`.

### 2.2 Inbound client message (TMSrv, `:8281`)

```
 1. TCP bytes arrive; Winsock posts WM_USER+100 (WSA_READ) with wParam = socket, lParam = FD_READ|FD_CLOSE
 2. Code/TMSrv/Server.cpp:3921   GetUserFromSocket(wParam)            -> session index, 0 => closesocket
 3. Code/TMSrv/Server.cpp:3929   FD_CLOSE (event != FD_READ)          -> CloseUser
 4. Code/TMSrv/Server.cpp:3938   pUser[User].cSock.Receive()          -> recv into pRecvBuffer + nRecvPosition
 5. Code/TMSrv/Server.cpp:3975   pUser[User].cSock.ReadMessage(&Error,&ErrorCode)  (loop)
 6. Code/CPSock.cpp:366-383      if Init == 0: require 4-byte INITCODE, consume 4 bytes, set Init = 1
 7. Code/CPSock.cpp:386-387      require >= sizeof(HEADER) bytes buffered
 8. Code/CPSock.cpp:390-395      read Size, KeyWord index, CheckSum, Type, ID (Type/ID read only at :394-395)
 9. Code/CPSock.cpp:397-406      reject Size outside [12, 8192]; reset both positions; ErrorCode = 2
10. Code/CPSock.cpp:408-411      if Size > buffered remainder -> return 0 (wait for more bytes)
11. Code/CPSock.cpp:414-422      advance nProcPosition by Size; reset to (0,0) when fully drained
12. Code/CPSock.cpp:424-455      de-obfuscate bytes [4,Size) in place; recompute 8-bit checksum
13. Code/CPSock.cpp:457-464      checksum mismatch -> ErrorCode = 1, still return the message
14. Code/TMSrv/Server.cpp:3990    Error 1 or 2 -> log and break (socket left open)
15. Code/TMSrv/ProcessClientMessage.cpp:38-66  cast to MSG_STANDARD, validate ID range, switch on Type
16. Handler (_MSG_*.cpp) reads payload at fixed offsets without any size check
```

### 2.3 Inbound DB message (TMSrv `WSA_READDB`)

Same as 2.2 without the per-session lookup: `DBServerSocket.Receive()` (`Code/TMSrv/Server.cpp:3850`) then `DBServerSocket.ReadMessage(...)` in a loop (`:3889`), then `ProcessDBMessage(Msg)` (`:3914`). On `Receive() == FALSE` the socket is closed and reconnected up to two times with `Sleep(200)` between attempts (`:3854-3881`); if reconnect fails, `PostQuitMessage` terminates the process. `INITCODE` is sent by this side in `ConnectServer` and validated by DBSrv's accepted socket.

### 2.4 Inbound admin message (DBSrv `WSA_READADMIN` / `WSA_READADMINCLIENT`)

`pAdmin[User].cSock.Receive()` (`Code/DBSrv/Server.cpp:825`) then `ReadMessage` in a loop (`:840`) then `ProcessAdminMessage` (`:863`). The redirect client uses the same framing over `AdminClient` (`:780`). Rejection of an unknown source IP closes the socket (`:816-821`, `:896-899`).

### 2.5 Outbound client message (TMSrv -> client)

```
 1. Handler builds MSG_* on the stack (Code/TMSrv/SendFunc.cpp etc.)
 2. AddMessage(msg, sizeof(MSG_*))                Code/CPSock.cpp:513
 3.   capacity guard nSendPosition + Size < 131072 (:518)
 4.   Sock == 0 guard                               (:527)
 5.   iKeyWord = rand() % 256; KeyWord = pKeyWord[iKeyWord*2]  (:535-536)
 6.   stamp Size, KeyWord, CheckSum=0, ClientTick=CurrentTime  (:538-542); LastSendTime = CurrentTime
 7.   transform payload into pSendBuffer, accumulate Sum1/Sum2  (:554-581)
 8.   CheckSum = Sum2 - Sum1; copy first 4 header bytes          (:583-586)
 9.   nSendPosition += Size                                      (:588)
10. SendMessageA()                                 Code/CPSock.cpp:617
11.   RefreshSendBuffer if nSentPosition > 0                    (:631-632)
12.   one send() of [nSentPosition, nSendPosition)              (:658-667)
13.   success -> reset both positions; partial or error -> keep, return TRUE  (:669-683)
```

A 1 Hz sweep retries pending client writes: `if (pUser[bb].cSock.nSendPosition) SendMessageA()` (`Code/TMSrv/ProcessSecMinTimer.cpp:205-219`). No equivalent sweep exists for `DBServerSocket`, `BillServerSocket`, `AdminClient`, or `AdminSocket`.

### 2.6 Billing outbound / inbound

```
out: SendBilling2 (_AUTH_GAME*) -> CPSock::SendBillMessage -> memcpy 196 bytes -> SendMessageA
     Code/TMSrv/Server.cpp:1382-1391, Code/CPSock.cpp:498-511
in:  FD_READ -> BillServerSocket.Receive -> ReadBillMessage (196-byte pop) -> ProcessBILLMessage (empty)
     Code/TMSrv/Server.cpp:3745, 3793, 3806
```

`SendBilling` (the per-session billing disconnect) is a stub returning `TRUE` (`Code/TMSrv/Server.cpp:1377-1380`).

### 2.7 Accept path

```
listener armed for FD_ACCEPT (StartListen)
 -> WM_USER+3 (WSA_ACCEPT) -> GetEmptyUser -> pUser[User].AcceptUser(ListenSocket.Sock)
 -> accept(); WSAAsyncSelect(tSock, hWndMain, WSA_READ, FD_READ|FD_CLOSE); cSock.Sock = tSock
 -> Code/TMSrv/CUser.cpp:51-79
DBSrv additionally redirects the accepted handle into a pre-allocated slot without moving the buffers:
 -> Code/DBSrv/Server.cpp:1152-1157 (Sock + three positions), Code/DBSrv/CUser.cpp:39-65
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|---|---|---|---|
| BR-01 | Platform | Winsock must be initialised as version 1.1 before any socket call; failure aborts server start | `Code/CPSock.cpp:109-117`; callers `Code/TMSrv/Server.cpp:3496`, `Code/DBSrv/Server.cpp:520` |
| BR-02 | Networking | Listener binds a caller-supplied IPv4 address and port with backlog 8; any bind/listen failure aborts start with a modal dialog | `Code/CPSock.cpp:128-174`, `Code/CPSock.h:33` |
| BR-03 | Eventing | A socket is armed with `WSAAsyncSelect` for exactly the events the caller names; inbound data is delivered as a posted window message | `Code/CPSock.cpp:163,239,323`; `Code/TMSrv/CUser.cpp:61`; `Code/DBSrv/CUser.cpp:49` |
| BR-04 | Resilience | Outbound connect first binds an ephemeral port; on bind failure it retries twice from a monotonic global port cursor (+5000 for game, +6000 for billing) | `Code/CPSock.cpp:208-226`, `:281-298`, `:27` |
| BR-05 | Handshake | Every outbound server-to-server connection sends the 4-byte `INITCODE` and marks itself authenticated | `Code/CPSock.cpp:249-252` |
| BR-06 | Handshake | Every inbound connection must begin with the 4-byte `INITCODE`; a mismatch is reported as error 2 and the connection is not advanced | `Code/CPSock.cpp:366-383` |
| BR-07 | Framing | A frame is valid only when `12 <= Size <= 8192`; anything else resets the receive buffer and reports error 2 | `Code/CPSock.cpp:397-406`, `Code/CPSock.h:38` |
| BR-08 | Framing | An incomplete frame is retained; no data is consumed until `Size` bytes are buffered | `Code/CPSock.cpp:408-411` |
| BR-09 | Wire format | Header is 12 bytes: `Size(2)`, `KeyWord(1)`, `CheckSum(1)`, `Type(2)`, `ID(2)`, `ClientTick(4)`, little-endian, unpacked | `Code/CPSock.h:42-50`; `Code/Basedef.h:925-930` |
| BR-10 | Obfuscation | The message keyword is a random index into the even bytes of a 512-byte table; the receiver re-derives it from the transmitted index byte | `Code/CPSock.cpp:535-536`, `:390-392`; table `:29-46` |
| BR-11 | Obfuscation | Each payload byte is shifted by a table byte selected by ring position and shifted by `index & 3`; the transform is its own inverse | `Code/CPSock.cpp:430-453`, `:558-581` |
| BR-12 | Integrity | `CheckSum` is the 8-bit difference between summed ciphertext and summed plaintext over bytes `[4,Size)`; a mismatch does not suppress delivery | `Code/CPSock.cpp:424-466`, `:554-584` |
| BR-13 | Flow control | A message is refused when it would not fit strictly below `SEND_BUFFER_SIZE` (131072) or when the socket handle is 0 | `Code/CPSock.cpp:518-533` |
| BR-14 | Stamping | Enqueue overwrites the caller's `Size`, `KeyWord`, `CheckSum` and `ClientTick` and records `LastSendTime` | `Code/CPSock.cpp:538-542` |
| BR-15 | Transmission | One `send()` attempt per call; unsent remainder stays queued and is flushed by a later call; no write-readiness event is registered | `Code/CPSock.cpp:631-683`, `:239`, `:323` |
| BR-16 | Reception | One `recv()` per call into the remaining buffer space; a full buffer or a socket error is reported as failure without advancing | `Code/CPSock.cpp:336-347` |
| BR-17 | Buffer lifecycle | Positions are reset when the receive buffer drains or when a fatal frame error occurs; compaction helpers exist but the receive one is never called | `Code/CPSock.cpp:357-363`, `:418-422`, `:593-615` |
| BR-18 | Billing framing | Billing traffic is fixed 196-byte records with no header, keyword, checksum or obfuscation; read errors are never reported | `Code/CPSock.cpp:469-511`, `Code/CPSock.h:88-91,132` |
| BR-19 | Billing connect | The billing connector uses the same bind fallback but does not send `INITCODE`, does not set `Init`, and does not reset `nSentPosition` | `Code/CPSock.cpp:257-334` |
| BR-20 | Lifecycle | Closing zeroes all four positions and `Init`, closes the handle if non-zero, and always reports success | `Code/CPSock.cpp:84-98` |
| BR-21 | Resource | Each instance owns two 128 KiB `malloc` buffers for its entire life; the destructor frees them; the class has no copy semantics | `Code/CPSock.cpp:62-82`, `Code/CPSock.h:53-86` |
| BR-22 | Error contract | `ReadMessage` reports `0` (ok), `1` (checksum) or `2` (frame/init); `ErrorType` carries `Size` or the offending dword; the component never disconnects | `Code/CPSock.cpp:353-467` |
| BR-23 | Concurrency | All I/O and all state mutation occur on the thread that owns `hWndMain`; there are no locks and no background threads | `Code/CPSock.cpp:163-169`, `:239-245`; `Code/TMSrv/Server.cpp:3650-3654` |
| BR-24 | Diagnostics | Schema validation, if compiled in, is gated by `_PACKET_DEBUG` and delegated to a validator that always returns false | `Code/CPSock.cpp:544-552`; `Code/Basedef.cpp:6475-6596` |
| BR-25 | Addressing | The `ip` argument is a 4-byte network-order address; `0` means `INADDR_ANY` for listeners | `Code/CPSock.cpp:144`, `:205`; `Code/TMSrv/Server.cpp:367,3526-3529`; `Code/DBSrv/Server.cpp:586-587` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 Process-wide Winsock initialisation gate

**Overview**

`CPSock::WSAInitialize()` is the only place in the codebase that calls `WSAStartup`. It requests version 1.1 and returns a boolean that gates server startup.

**Detailed description**

The function declares a local `WSADATA` and calls `WSAStartup(MAKEWORD(1,1), &WSAData)` (`Code/CPSock.cpp:111-113`). A non-zero return maps to `FALSE`; anything else maps to `TRUE`. The `WSAData` content (negotiated version, maximum sockets, buffer sizes) is discarded, so the process never learns whether the stack actually granted 1.1 or returned a higher compatible version, and it never inspects `iMaxSockets` or `iMaxUdpDg`.

Although the method is declared as an instance member, `WSAStartup` is a process-global operation. Both executables call it exactly once, through an arbitrary instance used as a bootstrap object: `ListenSocket.WSAInitialize()` at `Code/TMSrv/Server.cpp:3496` and at `Code/DBSrv/Server.cpp:520`. No reference counting exists, and no per-socket initialisation is performed; every `CPSock` constructed before that call is inert but already has its buffers allocated.

The counterpart `WSACleanup()` is called once from the window-destruction path (`Code/TMSrv/Server.cpp:4145-4148`, `Code/DBSrv/Server.cpp:1327`), not from `CPSock`. Lifetime is therefore process-scoped, not object-scoped. Requesting 1.1, rather than 2.2, also determines which header the translation unit sees: `CPSock.cpp` includes only `<Windows.h>` (`:20`), which pulls the 1.1 `winsock.h` in TMSrv, while `Code/DBSrv/Server.cpp:21` includes `<winsock2.h>` first. The same shared source is therefore compiled against two different socket headers in one solution.

**Rule workflow**

```
server start
  -> ListenSocket.WSAInitialize()
       -> WSAStartup(MAKEWORD(1,1), &WSAData)
            != 0 -> return FALSE -> caller logs "err,wsainitialize fail" and returns FALSE
            == 0 -> return TRUE
  -> later: ListenSocket.StartListen(...) / ConnectServer(...)
process shutdown (WM_DESTROY)
  -> WSACleanup()
```

---

### Business Rule: BR-02 Listener creation, bind and backlog

**Overview**

`StartListen` creates the passive socket, binds it to a supplied address and port, starts listening with a backlog of 8, and arms accept notification. Any failure produces a modal message box and returns 0.

**Detailed description**

The sequence is `socket(AF_INET, SOCK_STREAM, 0)` (`:133`), `bind` (`:147`), `listen(tSock, MAX_PENDING_CONNECTS)` (`:155`) and `WSAAsyncSelect(tSock, hWnd, WSA, FD_ACCEPT)` (`:163`). Each step has its own `MessageBox` string: "Initialize socket fail", "Binding fail", "Listen fail", "WSAAsyncSelect fail". The function returns `FALSE` (0) for the first three and, for the fourth, would return 0 only if the test could ever fire - see BR-03.

Three callers exist, each with a distinct address policy: TMSrv game listener `StartListen(hWndMain, *pip, GAME_PORT, WSA_ACCEPT)` (`Code/TMSrv/Server.cpp:3634`), DBSrv game-server listener `StartListen(hWndMain, 0, DB_PORT, WSA_ACCEPT)` (`Code/DBSrv/Server.cpp:586`) and DBSrv admin listener `StartListen(hWndMain, 0, ADMIN_PORT, WSA_ACCEPTADMIN)` (`:587`). Passing `0` binds to all interfaces; TMSrv passes the four bytes of `LocalIP` reinterpreted as an `int` (`Code/TMSrv/Server.cpp:3560`), which for a little-endian host is exactly the network-order address `a.b.c.d` and is therefore a bind to one specific interface.

Two incidental details matter operationally. First, `gethostname(Temp, 256)` (`:142`) is called and the result is never used, so its only effect is a possible DNS-less hostname lookup on the UI thread. Second, `SOCKADDR_IN local_sin` is not zeroed before use, so `sin_zero` carries stack garbage into `bind`; `bind` ignores those bytes for `AF_INET`, making this benign in practice but non-deterministic. The function also adopts `Sock = tSock` unconditionally at the end (`:171`), overwriting any previously stored handle without closing it, so a second `StartListen` on the same object leaks the first listener.

**Rule workflow**

```
StartListen(hWnd, ip, port, wsaMsg)
  -> socket(AF_INET, SOCK_STREAM, 0)
       INVALID_SOCKET -> MessageBox("Initialize socket fail") -> return 0
  -> gethostname(Temp,256)                       (result unused)
  -> sin_family=AF_INET, sin_addr=ip, sin_port=htons(port)
  -> bind()
       SOCKET_ERROR -> MessageBox("Binding fail") -> closesocket -> return 0
  -> listen(backlog = 8)
       < 0 -> MessageBox("Listen fail") -> closesocket -> return 0
  -> WSAAsyncSelect(tSock, hWnd, wsaMsg, FD_ACCEPT)   [test is `> 0`, see BR-03]
  -> Sock = tSock; return tSock
```

---

### Business Rule: BR-03 Async event registration and event codes

**Overview**

`WSAAsyncSelect` converts every socket into a message source for the server window. The component defines the message identifiers and imposes the guarantee that all socket state changes are serialised on that window's owning thread.

**Detailed description**

The message identifiers are fixed constants in the header: `WSA_READ (WM_USER+100)`, `WSA_READDB (+2)`, `WSA_ACCEPT (+3)`, `WSA_READBILL (+4)`, `WSA_ACCEPTADMIN (+5)`, `WSA_READADMIN (+6)`, `WSA_READADMINCLIENT (+7)` (`Code/CPSock.h:25-31`). Listeners are armed with `FD_ACCEPT` only; connected sockets with `FD_READ | FD_CLOSE` only (`Code/CPSock.cpp:163`, `:239`, `:323`; `Code/TMSrv/CUser.cpp:61`; `Code/DBSrv/CUser.cpp:49`). `FD_WRITE` is never requested. The absence of `FD_WRITE` is the structural reason for BR-15: the transport has no event that would tell it the socket has become writable again.

The success test itself is wrong in every one of the five arming sites. `WSAAsyncSelect` documents `0` for success and `SOCKET_ERROR` (-1) for failure, and the code writes `if (WSAAsyncSelect(...) > 0)` and treats that as the failure branch. The branch is therefore unreachable: success (0) and failure (-1) both fall through to "armed". The consequence is not a crash but a silent degradation - a socket that could not be armed is stored in `Sock`, returned to the caller as a valid handle, and will never deliver a single message. In `CUser::AcceptUser` this means a client is accepted, counted, and then never spoken to.

Event decoding happens in the consumers, not in `CPSock`. Each handler uses `WSAGETSELECTEVENT(lParam)` and treats anything that is not `FD_READ` as a close (`Code/TMSrv/Server.cpp:3929-3936`, `Code/DBSrv/Server.cpp:810-821`, `:890-900`). Because `FD_CLOSE` is the only other event requested, that test is currently equivalent to "connection closed", but it also silently absorbs any future event added to the mask.

**Rule workflow**

```
arm(socket, hwnd, wsaMsg, events)
  -> WSAAsyncSelect(...)
       returns 0        -> branch `> 0` false -> treated as success
       returns SOCKET_ERROR -> branch `> 0` false -> treated as success
  -> caller stores handle and proceeds

data arrives
  -> Windows posts wsaMsg to hwnd with wParam = SOCKET, lParam = MAKEWORD(event, error)
  -> WndProc: WSAGETSELECTEVENT(lParam) == FD_READ ? read : treat as close
```

---

### Business Rule: BR-04 Outbound source-port fallback ladder

**Overview**

`ConnectServer` and `ConnectBillServer` bind the outbound socket to an explicit local port. If that fails, they retry from a monotonically advancing global cursor before giving up.

**Detailed description**

`ConnectServer` zeroes both `SOCKADDR_IN` structures and first binds with `sin_port = 0`, which asks the stack for any ephemeral port (`:206-208`). If that bind fails, the global `ConnectPort` (`Code/CPSock.cpp:27`, defined outside the class, external linkage) is increased by 10 and the code tries `ConnectPort + 5000`; on a second failure it increases by another 10 and tries again; a third failure produces "Binding fail" and a closed socket (`:208-226`). `ConnectBillServer` follows the same shape but with a different ladder: first retry uses `ConnectPort + 6000` *without* incrementing, second retry increments by 10 and reuses `+6000`; a successful billing connection then performs `ConnectPort++` (`:281-303`).

The cursor is never reset to zero, not on success, not on failure, and not on `CloseSocket`. It is shared between the game-server connection and the billing connection, so a single billing connect permanently shifts every later game-server fallback by one. Fallback ports are chosen from 5000-65535/6000-65535 without checking whether another process or another `CPSock` in the same process already owns them; a collision simply fails the bind again. The business effect is a deterministic "try three times, then abort" policy whose third attempt is at a port derived from history rather than configuration.

For the normal path (bind to port 0 succeeds) the cursor is untouched and the source port is whatever the OS assigns. DBSrv's outbound `AdminClient` and TMSrv's `DBServerSocket` both benefit from this path in ordinary operation; the ladder only matters in environments where the OS refuses the ephemeral bind, which is why it exists (port-exhaustion or restricted-port conditions on the original deployment).

**Rule workflow**

```
ConnectServer(host, port, ip, wsaMsg)
  -> positions zeroed; CloseSocket() if Sock != 0
  -> socket(); bind(ip, 0)
       fail -> ConnectPort += 10; bind(ip, ConnectPort+5000)
                 fail -> ConnectPort += 10; bind(ip, ConnectPort+5000)
                           fail -> MessageBox("Binding fail"), closesocket, return 0
  -> connect(host,port)
       < 0 -> closesocket, Sock = 0, return 0
  -> WSAAsyncSelect(... FD_READ|FD_CLOSE)
  -> Sock = tSock; send(INITCODE,4); Init = 1; return tSock
```

---

### Business Rule: BR-05 Outbound INITCODE preface

**Overview**

Immediately after a successful `connect`, the client side writes a fixed 32-bit constant to the stream and marks its own state as authenticated. Nothing is expected back.

**Detailed description**

The constant is `INITCODE 0x1F11F311` (`Code/CPSock.h:40`) stored in a local `unsigned int` and sent as four raw bytes (`Code/CPSock.cpp:249-250`). On a little-endian host the octets on the wire are `11 F3 11 1F`. The `send` return value is ignored entirely: a zero-length or partial write, or `SOCKET_ERROR`, still results in `Init = 1` and a returned "successful" socket. The receiver's mandatory dword (BR-06) is what gives this preface meaning.

The handshake is unidirectional on purpose. `ConnectServer` marks itself `Init = 1` so that its own `ReadMessage` skips the magic check for traffic coming back from the peer; only accepted sockets start at `Init = 0` and perform the check. This is consistent for every server-to-server link in the project: TMSrv's `DBServerSocket` (`Code/TMSrv/Server.cpp:3562`) and DBSrv's `AdminClient` (`Code/DBSrv/Server.cpp:599`) both send the dword, and DBSrv's accepted game-server and admin sockets both require it.

The practical consequence is a hard interoperability requirement that is not documented in any protocol file: any peer that connects to a W2PP listener - including the closed-source game client and any external admin or billing tool - must begin the TCP stream with those four bytes, or it will be permanently rejected by BR-06. `ConnectBillServer` deliberately does not participate (BR-19), which is why the billing path is the one exception.

**Rule workflow**

```
connect() succeeds
  -> InitCode = 0x1F11F311
  -> send(tSock, &InitCode, 4, 0)        [return ignored, partial write not repaired]
  -> Init = 1
  -> return tSock
peer side:
  -> first 4 bytes of its stream must equal 0x1F11F311 (BR-06)
```

---

### Business Rule: BR-06 Inbound INITCODE gate

**Overview**

An accepted connection is not trusted as a message source until the first four bytes equal `INITCODE`. On mismatch the component reports error 2 with the offending dword and does not consume or reset anything.

**Detailed description**

`ReadMessage` begins by handling the drained-buffer case, then checks `if (Init == 0)` (`Code/CPSock.cpp:366`). If fewer than four bytes are buffered it returns 0 (not an error: "come back later"), preserving both positions. If four bytes are available it reinterprets them as `unsigned int` and compares against `INITCODE` (`:371-373`). Equality sets `Init = 1` and advances `nProcPosition` by 4 (`:381-382`), after which normal header parsing continues at the new offset in the same call.

Mismatch is the interesting path. The code writes `*ErrorCode = 2` and `*ErrorType = InitCode` and returns 0 (`:375-379`) **without advancing `nProcPosition` and without clearing the buffer**. Since `Init` remains 0, the next invocation re-reads the same four bytes and reproduces the same error. The client-side handlers react by logging and breaking out of the message loop (`Code/TMSrv/Server.cpp:3990-3999`, `Code/DBSrv/Server.cpp:785-791`, `:855-861`, `:938-944`) but none of them closes the socket on error 2. The connection therefore parks in a state where every `FD_READ` produces one log line and no data is consumed.

This rule is also the reason the receive path cannot resynchronise: there is no search for a later `INITCODE`, no timeout, and no close. Combined with the fact that `ProcessClientMessage` is never reached, the result is a logged, resource-consuming zombie connection until the peer disconnects or an idle timer fires (`CheckIdle`, `Code/TMSrv/Server.cpp:4304-4319`).

**Rule workflow**

```
ReadMessage()
  -> if nProcPosition >= nRecvPosition: reset (0,0), return 0
  -> if Init == 0:
       available < 4          -> return 0 (wait)
       dword == INITCODE      -> Init = 1; nProcPosition += 4; continue to header parse
       dword != INITCODE      -> ErrorCode = 2; ErrorType = dword; return 0
                                 (positions unchanged, Init unchanged -> repeats forever)
```

---

### Business Rule: BR-07 Frame size window

**Overview**

A frame is accepted only if its declared `Size` lies between 12 (`sizeof(HEADER)`) and 8192 (`MAX_MESSAGE_SIZE`). A violation is fatal for the current buffer contents: both receive positions are reset to zero.

**Detailed description**

The declared size is read as an `unsigned short` from the first two bytes of the candidate frame (`Code/CPSock.cpp:390`). The window check is `if (Size > MAX_MESSAGE_SIZE || Size < sizeof(HEADER))` (`:397`). The lower bound protects the header parse itself; the upper bound is the only global defence against a peer announcing an enormous frame. `MAX_MESSAGE_SIZE` is defined as `8192` with a comment that says "4K" (`Code/CPSock.h:38`), and the neighbouring buffer comments claim 64 KiB for 128 KiB buffers (`:35-36`) - the comments and the values disagree, and the values are authoritative.

On violation the code performs `nRecvPosition = 0; nProcPosition = 0;` and reports error 2 with `ErrorType = Size` (`:399-404`). This is a deliberate resynchronisation-by-discard policy: everything buffered, including complete valid frames that followed the bad one, is dropped. Because the caller only breaks the loop on error, the connection stays open and the next `FD_READ` starts from an empty buffer. A single malformed 2-byte length field therefore costs at most one buffer of data and one log line, at the price of arbitrary loss of queued messages.

The upper bound is not mirrored on the send path (BR-13 checks only the 128 KiB buffer), and the declared size is a `short` when written (`:538`). A caller that enqueues more than 32,767 bytes writes a negative size into the header; a caller that enqueues fewer than 12 bytes writes a frame the peer will reject and treat as a resynchronisation event. Both are reachable from existing call sites: `Code/TMSrv/_MSG_Action.cpp:272` and `:278` pass `sizeof(m)` where `m` is a `MSG_Action *`, i.e. 4, and the receiving peer discards its buffer on that frame.

**Rule workflow**

```
header bytes available (>= 12)
  -> Size = *(unsigned short*)(buffer + nProcPosition)
  -> Size > 8192  or  Size < 12
       -> nRecvPosition = 0; nProcPosition = 0
       -> ErrorCode = 2; ErrorType = Size
       -> return 0                      (caller logs and breaks; socket stays open)
  -> otherwise continue to BR-08
```

---

### Business Rule: BR-08 Partial-frame retention

**Overview**

If fewer than `Size` bytes are buffered, the frame is not consumed and the partial data is kept at its current offset so the next receive can append to it.

**Detailed description**

After the header has been read, the code computes `Rest = nRecvPosition - nProcPosition` and returns 0 when `Size > Rest` (`Code/CPSock.cpp:408-411`). Neither position changes, so `Receive` will append at `nRecvPosition`, immediately after the partial frame. This is the mechanism that makes TCP segmentation invisible to the application: a 4 KiB frame split across three segments is assembled without copying or re-reading.

Retention is bounded by BR-07: the largest possible partial frame is 8191 bytes, and the header of the next frame cannot begin until that is satisfied. Consequently the receive buffer cannot be driven to overflow by framing alone; the worst case is one pending frame plus a drained prefix, which is far below 131072. There is no timeout and no per-frame deadline: a peer may hold a partial frame indefinitely, and the buffer will simply stop making progress while other events (close, idle check) decide the connection's fate.

The rule depends on `nProcPosition` never being advanced for an incomplete frame, which holds for the normal path. It does not hold after a checksum failure: positions have already advanced past the frame by the time the checksum is compared, so a corrupt frame is consumed and the caller is told via `ErrorCode = 1` (BR-12). It also does not hold after an init mismatch, where the same four bytes are re-examined forever (BR-06).

**Rule workflow**

```
Rest = nRecvPosition - nProcPosition
  -> Size > Rest:
       return 0            (positions unchanged; data retained)
  -> Size <= Rest:
       pMsg = &pRecvBuffer[nProcPosition]
       nProcPosition += Size
       if nRecvPosition <= nProcPosition -> both = 0
       continue to de-obfuscation / checksum
```

---

### Business Rule: BR-09 Header field layout and byte order

**Overview**

Every framed message starts with a fixed 12-byte header. The layout is duplicated in two places - `HEADER` in `CPSock.h` and the `_MSG` macro in `Basedef.h` - and is never validated for equivalence.

**Detailed description**

`HEADER` declares `short Size; char KeyWord; char CheckSum; short Type; short ID; unsigned int ClientTick;` (`Code/CPSock.h:42-50`). With MSVC's default 8-byte packing and 2-byte alignment for `short`, the fields land at offsets 0, 2, 3, 4, 6 and 8, giving 12 bytes total. `Basedef.h` re-declares the same six fields as the `_MSG` macro (`:925-930`) used at the head of every `MSG_*` structure, and `MSG_STANDARD` (`:943-946`) is the bare form. Both layouts coincide because every field before `ClientTick` is naturally aligned; no `#pragma pack` is involved.

All multi-byte fields are read by direct pointer cast (`*((unsigned short*)(pRecvBuffer + nProcPosition))`, `*((unsigned int*)(pRecvBuffer + nProcPosition + 8))`), which makes the wire format host-endian. On x86 the stream is little-endian; the component cannot be recompiled for a big-endian target without breaking the protocol, and it cannot be moved to a 64-bit build without changing `unsigned int Sock` (BR-25) and every `int` field in the contract.

`Type` and `ID` are read at `:394-395` but never used inside `CPSock`; they exist only so the component could log them in the `_PACKET_DEBUG` block (which uses a separate `MSG_STANDARD*` cast instead). `Size` and `CheckSum` are the only header fields the transport itself consumes. The duplication of the layout across two headers means a change to one is not detected by the compiler for the other; the only enforcement is that both are 12 bytes by coincidence of alignment.

**Rule workflow**

```
offset 0  : Size      (unsigned short, validated BR-07)
offset 2  : KeyWord   (unsigned char, index into pKeyWord even entries)
offset 3  : CheckSum  (unsigned char, plaintext, verified BR-12)
offset 4  : Type      (short, consumed by dispatchers only)
offset 6  : ID        (short, consumed by dispatchers only)
offset 8  : ClientTick(unsigned int, stamped by sender, checked by dispatcher)
offset 12 : payload   (transformed per BR-11, covered by checksum)
```

---

### Business Rule: BR-10 Keyword derivation and table indexing

**Overview**

Each message carries a random index byte. Both sides use it to select one table entry from the even positions of the 512-byte obfuscation table; the selected byte seeds the per-byte transform.

**Detailed description**

The sender draws `iKeyWord = rand() % 256` at enqueue time (`Code/CPSock.cpp:535`), stores it in the header (`:539`) and derives `KeyWord = pKeyWord[iKeyWord * 2]` (`:536`). The receiver applies the mirror operation: it reads the header byte and looks up the even entry (`:391-392`). Because the index byte is transmitted, the receiver needs no shared state and no handshake; the "secret" is only the table itself, which is a compile-time array present in both the server and the closed-source client.

The table (`pKeyWord[512]`, `:29-46`, annotated "7.xx keys") is a non-`const`, externally linked global that no other translation unit declares. Its even bytes (0, 2, 4, ... 510) are keyword values in the observed range `0x02-0xF6`; its odd bytes are transform parameters. Only the even half is used as a keyword; only the odd half is used as a shift source. Nothing validates the table at runtime, and nothing prevents another module from writing to it through an `extern` declaration, since it is not `static`.

The randomness source is the C runtime `rand()`. No `srand` call exists anywhere in the repository (verified by exhaustive search), so the sequence is the implementation's deterministic default seed. Every process therefore generates the same keyword sequence from the same number of prior calls, and because `rand()` is also used elsewhere in the codebase, the position in the sequence depends on startup path. This makes the keyword statistically predictable but does not by itself weaken the framing, since the receiver is told the index explicitly.

**Rule workflow**

```
send:
  iKeyWord = rand() % 256
  KeyWord  = pKeyWord[iKeyWord * 2]
  header[2] = iKeyWord
receive:
  iKeyWord = header[2]                 (0..255, always a valid even-index input)
  KeyWord  = pKeyWord[iKeyWord * 2]
  pos = KeyWord
  for i in [4, Size):
     T = pKeyWord[(pos % 256) * 2 + 1]
     ... transform byte i ...
     pos++
```

---

### Business Rule: BR-11 Payload transform

**Overview**

Bytes from offset 4 to `Size-1` are transformed with one of four arithmetic operations, selected by `i & 3`, using a table byte selected by a 256-entry ring position. The same arithmetic with reversed signs recovers the plaintext.

**Detailed description**

The forward transform in `AddMessage` is (`Code/CPSock.cpp:568-578`): `i%4==0 -> plain + (T<<1)`, `i%4==1 -> plain - (T>>3)`, `i%4==2 -> plain + (T<<2)`, `i%4==3 -> plain - (T>>5)`, where `T = pKeyWord[(pos % 256) * 2 + 1]` and `pos` starts at the message keyword and increments once per byte. The inverse in `ReadMessage` (`:440-450`) uses `cipher - (T<<1)`, `cipher + (T>>3)`, `cipher - (T<<2)`, `cipher + (T>>5)`. All operations are byte-wide with wraparound, so the pair is exact for every possible `T` and every byte value.

`i` is the offset from the start of the frame, not from the start of the buffer, because `pMsg` points at the frame (`:414`). This makes the transform alignment independent of where in the 128 KiB buffer the frame happens to sit, and it is why the receive path can parse at arbitrary `nProcPosition` values without desynchronising the cipher. The ring position, by contrast, is continuous across the message and wraps mod 256, so a frame longer than 256 bytes reuses table entries in a fixed cycle.

Two properties follow. First, the transform is not compression, not encryption, and not authenticated: anyone with the 512-byte table (embedded in both binaries) and the index byte (transmitted in clear) recovers the plaintext in one pass. Its function is to prevent trivial protocol inspection and casual packet editing, not to provide confidentiality. Second, the arithmetic is performed on `char`, which is signed under MSVC; the additions and subtractions are computed in `int` and truncated back, which is well-defined under MSVC's two's-complement but implementation-defined in ISO C++ for out-of-range narrowing. The sender and receiver both compile with the same toolset, so they agree.

**Rule workflow**

```
for i = 4 .. Size-1:
    rst = (KeyWord + (i-4)) % 256
    T   = pKeyWord[rst*2 + 1]
    m   = i & 3
    send: m==0: b += T<<1 ; m==1: b -= T>>3 ; m==2: b += T<<2 ; m==3: b -= T>>5
    recv: m==0: b -= T<<1 ; m==1: b += T>>3 ; m==2: b -= T<<2 ; m==3: b += T>>5
```

---

### Business Rule: BR-12 8-bit additive checksum

**Overview**

`CheckSum` is a one-byte additive difference between the transformed payload and the plaintext payload. The receiver recomputes it and, on mismatch, reports error 1 while still returning the message.

**Detailed description**

The sender accumulates `Sum1` over the plaintext bytes and `Sum2` over the bytes it writes into the send buffer, then stores `Sum2 - Sum1` (mod 256) at header offset 3 (`Code/CPSock.cpp:554-584`). The receiver accumulates `Sum2` over the ciphertext as received and `Sum1` over the same bytes after inverse transform, then compares `Sum2 - Sum1` with the header byte (`:425-455`). Both accumulators are `unsigned char`, so the arithmetic is modulo 256 and order-independent. The first four header bytes, including the checksum itself, are excluded from the sum.

The check is deliberately non-blocking. When the comparison fails the code sets `*ErrorCode = 1`, `*ErrorType = Size`, and returns `pMsg` anyway (`:457-464`). It does not zero the pointer, does not mark the frame as suspect in any out-of-band way, and does not prevent a caller from processing it. The only defence is caller discipline: TMSrv's client loop checks `if (Error == 1 || Error == 2)` after the null test and breaks (`Code/TMSrv/Server.cpp:3990`), as do DBSrv's loops (`Code/DBSrv/Server.cpp:855`, `:938`) and the admin-client loop (`:785`). A caller that omits that check would consume corrupted data as if it were valid.

Detection strength is 1/256 against random corruption and zero against an adversary: an attacker who knows the table (it is in the client binary) can produce any plaintext and compute the matching checksum, because the checksum key is derived from the same public transform. The rule's actual value is detecting truncation and misalignment - a frame consumed at the wrong offset produces a large `Sum2 - Sum1` difference with overwhelming probability.

**Rule workflow**

```
send:
  Sum1 = Sum2 = 0
  for i in [4,Size): Sum1 += plain[i]; buf[i] = transform(plain[i]); Sum2 += buf[i]
  header[3] = Sum2 - Sum1
receive:
  for i in [4,Size): Sum2 += raw[i]; plain[i] = inverse(raw[i]); Sum1 += plain[i]
  if (Sum2 - Sum1) != header[3]: ErrorCode = 1; ErrorType = Size; return pMsg
  return pMsg
```

---

### Business Rule: BR-13 Send queue capacity guard

**Overview**

A message is accepted for transmission only when it fits strictly below the 131,072-byte send buffer and the socket handle is non-zero. Refusal is logged through the host server's `Log`.

**Detailed description**

`AddMessage` first tests `if (nSendPosition + Size >= SEND_BUFFER_SIZE)` (`Code/CPSock.cpp:518`). The strict `>=` means the final byte of the buffer is never used, so the maximum post-enqueue `nSendPosition` is 131,071. On refusal it formats `"err,add buffer full %d %d %d %d"` with the current position, the requested size, the message type and the socket handle, calls `Log`, and returns `FALSE` (`:520-523`). The message is discarded; there is no queue, no retry and no back-pressure signal to the producer beyond the boolean.

The second guard is `if (Sock <= 0)` (`:527`), which for an `unsigned int` handle is exactly `Sock == 0`. It logs `"err,add buffer invalid ..."` and returns `FALSE` (`:529-532`). Together these are the only two conditions under which enqueue fails. Notably absent: any check that `Size >= sizeof(HEADER)`, any check that `Size <= MAX_MESSAGE_SIZE`, any check that `pMsg != NULL`, and any check that `Size` is non-negative. Since `Size` is an `int` and is added to `nSendPosition` before the guard, a negative `Size` passes the capacity test, produces an empty transform loop, copies four header bytes, and moves `nSendPosition` backwards - leaving the buffer in a state where the next message overwrites earlier bytes.

The 128 KiB budget is shared by all pending traffic for one connection and is never trimmed by anything except successful transmission (BR-15). With client socket send buffers typically far smaller, the queue absorbs bursts and converts them into deferred writes. When the queue is full, the failure is reported to `SendOneMessage`, which ignores it (see BR-15 and Section 10, TD-03), so at most a log line records the loss.

**Rule workflow**

```
AddMessage(pMsg, Size)
  -> if nSendPosition + Size >= 131072:
        Log("err,add buffer full ..."); return FALSE
  -> if Sock == 0:
        Log("err,add buffer invalid ..."); return FALSE
  -> stamp header (BR-14), transform (BR-11), compute checksum (BR-12)
  -> copy 4 header bytes; nSendPosition += Size; return TRUE
```

---

### Business Rule: BR-14 Outbound header stamping and caller-buffer mutation

**Overview**

Enqueue rewrites the caller's header in place: `Size`, `KeyWord`, `CheckSum` and `ClientTick` are overwritten, and the process-global `LastSendTime` is updated. The payload is never modified.

**Detailed description**

The function casts the caller's buffer to `HEADER *` and writes `pSMsg->Size = Size`, `pSMsg->KeyWord = iKeyWord`, `pSMsg->CheckSum = 0`, `pSMsg->ClientTick = CurrentTime` (`Code/CPSock.cpp:538-541`), then later writes the computed checksum (`:584`). `Type` and `ID` are left as the caller set them. Because the cast is applied to a `MSG_*` structure whose first twelve bytes are the `_MSG` macro, the write lands on the caller's own header fields; stack-allocated messages in `SendFunc.cpp` are mutated as a side effect of sending.

`ClientTick` is not an arbitrary diagnostic field. The dispatcher compares it against `SKIPCHECKTICK` and silently drops the packet when a client-forged value equals the sentinel (`Code/TMSrv/ProcessClientMessage.cpp:62-64`), and several handlers read it for pacing. Because `CPSock` overwrites whatever the caller wrote with `CurrentTime`, outbound timestamps are always server-generated; the field is authoritative on send and untrusted on receive. `LastSendTime` (`Code/CPSock.cpp:50`, defined at `Code/TMSrv/Server.cpp:282` and `Code/DBSrv/Server.cpp:56`) is set at *queue* time, not at transmit time, so it measures "last time a message was enqueued for this object" and is a global, not per-socket, value - the last writer wins across all connections.

The mutation has one observable interaction with multicast helpers. `SyncMulticast` and friends pass the same `MSG_STANDARD*` to several `AddMessage` calls (`Code/TMSrv/SendFunc.cpp:266`, `:280`, `:714`, `:968`, `:1021`). Each call re-randomises the keyword and recomputes the checksum over the still-plaintext payload, so per-recipient frames differ correctly; the caller's header ends up carrying the values of the last enqueue. That is safe for the payload but means a caller cannot inspect `m->Size`/`m->KeyWord` after a multicast and expect the first recipient's values.

**Rule workflow**

```
AddMessage(pMsg, Size)
  -> pSMsg = (HEADER*)pMsg
  -> pSMsg->Size = (short)Size
  -> pSMsg->KeyWord = rand()%256
  -> pSMsg->CheckSum = 0
  -> pSMsg->ClientTick = CurrentTime
  -> LastSendTime = CurrentTime
  -> transform payload; pSMsg->CheckSum = Sum2 - Sum1
  -> memcpy(pSendBuffer + nSendPosition, pMsg, 4)
```

---

### Business Rule: BR-15 Single-attempt transmit and deferred flush

**Overview**

`SendMessageA` performs at most one `send()` per invocation. A partially accepted write is retained and reported as success; the remainder is flushed only by a later call. No write-readiness event exists to trigger that call.

**Detailed description**

The function first returns `FALSE` and zeroes both positions when `Sock == 0` (`Code/CPSock.cpp:623-629`). It then compacts the buffer if any bytes were already accepted (`if (nSentPosition > 0) RefreshSendBuffer()`, `:631-632`), validates the position pair, and enters `for (int i = 0; i < 1; i++)` - a loop that executes exactly once by construction (`:658`). Inside, it computes `Left = nSendPosition - nSentPosition`, calls `send(Sock, pSendBuffer + nSentPosition, Left, 0)`, advances `nSentPosition` by the returned count, and on `SOCKET_ERROR` stores `WSAGetLastError()` in a local `err` that is never read (`:660-667`).

The exit logic is `if (nSentPosition < nSendPosition || tSend == SOCKET_ERROR) continue; else { reset; return TRUE; }` (`:669-677`). A `continue` on the last iteration terminates the loop, after which the function checks only `nSendPosition > SEND_BUFFER_SIZE` and otherwise `return TRUE` (`:680-683`). Three distinct outcomes - complete write, partial write, and hard socket error - therefore collapse into the same `TRUE` result. The false return path is effectively unreachable in normal operation because `AddMessage` caps `nSendPosition` at 131,071.

Because only `FD_READ | FD_CLOSE` are registered (BR-03), there is no `FD_WRITE` notification when the peer's window reopens. Client sessions are rescued by a 1 Hz sweep that calls `SendMessageA()` whenever `nSendPosition != 0` (`Code/TMSrv/ProcessSecMinTimer.cpp:201-221`). The DB socket, billing socket and admin sockets have no such sweep; their pending tail is flushed only when the application happens to enqueue another message on the same object. A connection that goes idle with a partially written tail stays partially written indefinitely, and a subsequent write appends after the unsent bytes, preserving order but compounding latency.

**Rule workflow**

```
SendMessageA()
  -> Sock == 0 -> zero positions; return FALSE
  -> nSentPosition > 0 -> RefreshSendBuffer()
  -> invalid indices -> zero positions
  -> one send(nSendPosition - nSentPosition)
       full write           -> zero positions; return TRUE
       partial write        -> keep positions; return TRUE   (flush deferred)
       SOCKET_ERROR         -> keep positions; err unused; return TRUE
```

---

### Business Rule: BR-16 Reception: one read, and full-buffer treated as failure

**Overview**

`Receive` performs one `recv` into the unused tail of the receive buffer. It reports failure both for a socket error and for the case where the read exactly filled the remaining space - which loses those bytes.

**Detailed description**

The function computes `Rest = RECV_BUFFER_SIZE - nRecvPosition` and calls `recv(Sock, pRecvBuffer + nRecvPosition, Rest, 0)` (`Code/CPSock.cpp:338-339`). It then tests `if (tReceiveSize == SOCKET_ERROR || tReceiveSize == Rest) return FALSE;` (`:341-342`). In the normal case it advances `nRecvPosition` by the returned count and returns `TRUE` (`:344-346`). There is no `WSAGetLastError` capture, no distinction between `WSAEWOULDBLOCK` and a hard error, and no handling of `tReceiveSize == 0`.

The `tReceiveSize == Rest` clause is the most consequential. Filling the remaining space is a legitimate outcome, not an error; by returning `FALSE` without advancing `nRecvPosition`, the function discards the count while the kernel has already copied the bytes into `pRecvBuffer + nRecvPosition`. The caller's retry calls `recv` again at the same offset with the same length, overwriting those bytes with the *next* bytes of the stream. The result is a hole in the byte stream: every frame that spanned it is either rejected by the size window or delivered with corrupted content and a failing checksum. TMSrv reacts to `FALSE` by retrying once and then closing the user (`Code/TMSrv/Server.cpp:3940-3956`), so the user is disconnected - but only after the peer may have processed a desynchronised exchange. DBSrv logs and resets the positions without closing (`Code/DBSrv/Server.cpp:904-916`), leaving its stream desynchronised rather than torn down.

The zero-byte case is a separate gap. A graceful peer close makes `recv` return 0; the code treats that as success and returns `TRUE` with no position change. Nothing in `Receive` can therefore detect half-close. In practice `FD_CLOSE` arrives as a separate message and the consumers check `WSAGETSELECTEVENT(lParam) != FD_READ` before calling `Receive` (`Code/TMSrv/Server.cpp:3929`), so the close is handled by the event path; the silent zero-read is only reachable if a close races behind a queued `FD_READ`, in which case one wasted event loop iteration occurs. `Rest == 0` (buffer full) makes `recv` return 0, which also falls into the "success, no progress" path and would spin if a partial frame ever left the buffer full.

**Rule workflow**

```
Receive()
  -> Rest = 131072 - nRecvPosition
  -> n = recv(Sock, pRecvBuffer + nRecvPosition, Rest, 0)
       n == SOCKET_ERROR -> return FALSE          (positions unchanged)
       n == Rest         -> return FALSE          (bytes already copied are lost)
       n == 0            -> nRecvPosition += 0; return TRUE (no progress)
       otherwise         -> nRecvPosition += n; return TRUE
```

---

### Business Rule: BR-17 Buffer lifecycle and reset discipline

**Overview**

The receive buffer is reset to `(0,0)` when it drains or when a size violation occurs; the send buffer resets when a write completes. Compaction helpers exist, but the receive one is never invoked.

**Detailed description**

`ReadMessage` begins with `if (nProcPosition >= nRecvPosition) { nRecvPosition = 0; nProcPosition = 0; return 0; }` (`Code/CPSock.cpp:357-363`), which recycles the buffer whenever the previous call consumed everything. It repeats the reset after advancing past a frame when `nRecvPosition <= nProcPosition` (`:418-422`). A size violation performs an unconditional reset (`:399-400`). `SendMessageA` resets both send positions on a completed write and on detected index corruption (`:641-642`, `:654-655`, `:673-674`). `CloseSocket` resets all four plus `Init` (`:86-90`).

`RefreshRecvBuffer` (`:593-603`) moves the unconsumed tail `[nProcPosition, nRecvPosition)` to offset 0 and rebases both positions; `RefreshSendBuffer` (`:605-615`) does the same for `[nSentPosition, nSendPosition)`. `RefreshSendBuffer` is called from `SendMessageA` (`:632`). `RefreshRecvBuffer` is public but has no call site anywhere in the repository (verified by exhaustive search: two textual occurrences, the declaration and the definition). Receive-side compaction is therefore dead capability: the buffer only ever returns to offset 0 by fully draining or by discarding everything.

The asymmetry is largely masked by BR-07 and BR-08. Since no frame can exceed 8,192 bytes, the unconsumed region is at most one pending frame, and the buffer cannot be driven to exhaustion through ordinary framing. It becomes relevant in two states: after a checksum error the caller breaks with a partially drained buffer, so `nProcPosition` sits mid-buffer and the next event appends after `nRecvPosition`; and after the sticky init error, where `nProcPosition` stays at 0 while data accumulates. Neither state compacts, so both rely on the eventual `(0,0)` reset to recover.

**Rule workflow**

```
after each frame:
  nProcPosition += Size
  if nRecvPosition <= nProcPosition -> nRecvPosition = 0; nProcPosition = 0
after complete send:
  nSendPosition = 0; nSentPosition = 0
before next send with nSentPosition > 0:
  RefreshSendBuffer()   (memcpy tail to 0, rebase)
RefreshRecvBuffer():
  defined, public, zero call sites
```

---

### Business Rule: BR-18 Billing fixed-record framing

**Overview**

Traffic on the billing socket is a raw sequence of 196-byte `_AUTH_GAME` records. It bypasses the header, keyword, transform and checksum entirely, and its read path never reports an error.

**Detailed description**

`_AUTH_GAME` is declared as `char Unk[196]` with a comment stating the real structure still needs to be reverse-engineered (`Code/CPSock.h:88-91`), and `g_cGame` is defined as `sizeof(_AUTH_GAME)` (`:132`). `SendBillMessage` refuses to append when `nSendPosition + 196 >= 131072`, copies 196 bytes verbatim, advances the position, and calls `SendMessageA()` (`Code/CPSock.cpp:498-511`). `ReadBillMessage` checks that at least 196 bytes are buffered, returns a pointer to the current offset, advances by 196, and resets when drained (`:469-496`). It sets `*ErrorCode = 0` and `*ErrorType = 0` at entry and never modifies them (`:471-472`), so its declared error outputs are constant.

The result is a second, completely independent protocol on the same class. Billing records have no length field, no type, no integrity check and no obfuscation; a one-byte slip in the stream permanently misaligns every subsequent record, and the component has no way to detect it. There is also no `Init` handshake on this path (BR-19), so the first 196 bytes a billing peer sends are consumed as a record regardless of content. The only validation is `nRecvPosition - nProcPosition < g_cGame -> return 0`, which waits for a full record.

Consumption is entirely delegated. In TMSrv the only reader is the `WSA_READBILL` handler, which calls `ReadBillMessage` in a loop and forwards each record to `ProcessBILLMessage` (`Code/TMSrv/Server.cpp:3791-3808`). That function has an empty body (`:4300-4302`). Inbound billing data is therefore parsed, framed, and thrown away. Outbound, `SendBilling2` stamps a server index into the record and calls `SendBillMessage` (`:1382-1391`), ignoring the boolean result; `SendBilling` is a stub that returns `TRUE` without touching the socket (`:1377-1380`).

**Rule workflow**

```
send:
  if nSendPosition + 196 >= 131072 -> return FALSE
  copy 196 bytes at nSendPosition
  nSendPosition += 196
  return SendMessageA()
receive:
  ErrorCode = 0; ErrorType = 0
  if drained -> reset (0,0); return NULL
  if available < 196 -> return NULL
  pMsg = &pRecvBuffer[nProcPosition]
  nProcPosition += 196
  if drained -> reset (0,0)
  return pMsg            (never reports an error)
```

---

### Business Rule: BR-19 Billing connection variant

**Overview**

`ConnectBillServer` shares the bind-fallback pattern but deliberately differs from `ConnectServer`: it sends no `INITCODE`, never sets `Init`, does not reset `nSentPosition`, and keeps dead diagnostic code.

**Detailed description**

The function zeroes only three positions (`nRecvPosition`, `nSendPosition`, `nProcPosition`) and calls `CloseSocket()` if a previous handle exists (`Code/CPSock.cpp:265-270`). Because `CloseSocket` zeroes `nSentPosition` as well, the omission is only observable when `Sock` was already 0 - a state in which `nSentPosition` can still hold a stale value from a connection that was closed by direct handle manipulation. `SendMessageA` tolerates that: a positive `nSentPosition` with `nSendPosition == 0` makes `RefreshSendBuffer` compute a negative length and return early, then the index-validation branch resets both positions (`:607-609`, `:647-656`).

The bind ladder uses `ConnectPort + 6000` on the first retry without incrementing, then increments by 10 for the second (`:281-298`), and performs `ConnectPort++` after every attempt (`:303`). This makes the billing cursor advance by one per connection attempt, while the game cursor advances by ten per *failure*. The two cursors are the same variable, so billing activity perturbs the game fallback sequence.

Three `sprintf` calls build a descriptive message containing socket handle, IP octets and ports (`:300-301`, `:311-312`, `:320-321`); the resulting `msg[256]` is never used, logged or returned. The values it formats are also misleading: `local_sin.sin_addr` was assigned from `ip`, and `%d` of `sin_port` prints the network-order integer, not the port number. After `connect` succeeds the function arms `WSAAsyncSelect` and stores `Sock`, but does not send `INITCODE` and leaves `Init == 0`. The billing side only ever calls `ReadBillMessage`, which ignores `Init` entirely, so the zero value is harmless; it would matter if billing ever used `ReadMessage`.

**Rule workflow**

```
ConnectBillServer(host, port, ip, wsaMsg)
  -> reset recv/send/proc (not nSent); CloseSocket if Sock != 0
  -> socket(); bind(ip, 0)
       fail -> bind(ip, ConnectPort+6000)
                 fail -> ConnectPort += 10; bind(ip, ConnectPort+6000)
                           fail -> MessageBox("Binding fail"); closesocket; return 0
  -> sprintf(unused diagnostic)
  -> ConnectPort++
  -> connect(host, port)
       < 0 -> closesocket; Sock = 0; return 0
  -> WSAAsyncSelect(... FD_READ|FD_CLOSE)
  -> Sock = tSock; return tSock        (no INITCODE, Init stays 0)
```

---

### Business Rule: BR-20 Close and reset semantics

**Overview**

Closing a connection is a pure state reset plus `closesocket`. It always succeeds, never frees buffers, and leaves the object reusable for a new connection.

**Detailed description**

`CloseSocket` sets `nSendPosition`, `nSentPosition`, `nRecvPosition`, `nProcPosition` and `Init` to zero, calls `closesocket(Sock)` only when `Sock != 0`, then sets `Sock = 0`, and returns `TRUE` unconditionally (`Code/CPSock.cpp:84-98`). The two 128 KiB buffers are preserved, which is what allows a `CUser` slot to accept a new client without reallocating. The `Sock != 0` guard means a double close is harmless at this layer, but it also means that a handle already closed elsewhere (several consumers call `closesocket(wParam)` directly, e.g. `Code/TMSrv/Server.cpp:3924`, `Code/DBSrv/Server.cpp:803`, `:883`) is left non-zero in the object until `CloseSocket` runs; the stale value can be matched by `GetUserFromSocket` if the OS reuses the handle before the slot is cleaned.

The `Init` reset is the important semantic. It is what guarantees the next accepted connection on a reused slot performs the `INITCODE` gate (BR-06). The only path that bypasses it is DBSrv's accept redirect, which copies `Sock` and the three positions into `pUser[User].cSock` but not `Init` (`Code/DBSrv/Server.cpp:1152-1157`); it relies on the destination slot having been through `CloseSocket`, which is true for every slot that was previously used because `CUser::CloseUser` calls it (`Code/DBSrv/Server.cpp:896`, `:816`). Nothing in the accept path asserts it.

`CloseSocket` does not call `shutdown`, does not linger, and does not remove the socket from `WSAAsyncSelect`; `closesocket` implicitly cancels the registration, and any already-posted message for that handle will be delivered with an error code that `WSAGETSELECTEVENT` reports as something other than `FD_READ`, whereupon the consumer closes again or, for unknown handles, calls `closesocket` on a possibly recycled value.

**Rule workflow**

```
CloseSocket()
  -> nSendPosition = nSentPosition = nRecvPosition = nProcPosition = 0
  -> Init = 0
  -> if Sock != 0: closesocket(Sock)
  -> Sock = 0
  -> return TRUE
```

---

### Business Rule: BR-21 Instance memory ownership

**Overview**

Each instance owns exactly two heap blocks of 128 KiB, allocated in the constructor without a failure check and released in the destructor. The class has no copy constructor and no assignment operator.

**Detailed description**

The constructor sets `Sock = 0`, `Init = 0`, `malloc(131072)` for the send buffer, `malloc(131072)` for the receive buffer, and zeroes the four positions (`Code/CPSock.cpp:62-73`). Neither `malloc` result is tested. The class size is 32 bytes on Win32 (`unsigned Sock`, two pointers, five `int`), so the per-instance overhead is dominated by the two blocks: 262,144 bytes.

The aggregate cost is significant because the transport is embedded by value in session records. `CUser` contains `CPSock cSock` (`Code/TMSrv/CUser.h:47`), and `CUser pUser[MAX_USER]` is a global array of 1,000 elements (`Code/TMSrv/Server.cpp:269`). All 1,000 constructors run during static initialisation, so the process commits roughly 250 MiB of heap before `main` executes and before any client connects. DBSrv's arrays are much smaller (`MAX_SERVER`), but `TempUser` and the admin slots add a few more blocks. The design trades memory for zero allocation churn at connection time and for the ability to keep partially sent/received data across events.

The destructor frees both pointers after a `NULL` test (`:75-82`). Because the compiler-generated copy constructor and assignment operator copy the two pointers verbatim, any by-value copy of a `CPSock` (or of a `CUser`, which contains one) produces two owners of the same buffers and a double free plus a use-after-free of the socket buffers. The codebase avoids this by convention, not by construction: DBSrv's accept path copies `Sock` and the three positions field-by-field into the destination object rather than assigning the object (`Code/DBSrv/Server.cpp:1152-1157`, `:1038-1044`), which preserves the destination's own buffers. No `memcpy` or `memset` of a whole `CUser` or `pUser` array exists (verified by search). `pAdmin`, `ListenSocket`, `AdminSocket`, `DBServerSocket`, `BillServerSocket`, `AdminClient` and `TempUser` are all distinct objects with distinct buffers.

**Rule workflow**

```
CPSock():
  Sock = 0; Init = 0
  pSendBuffer = malloc(131072)      [unchecked]
  pRecvBuffer = malloc(131072)      [unchecked]
  nSend = nSent = nRecv = nProc = 0
~CPSock():
  free(pSendBuffer); free(pRecvBuffer)
copy: implicitly shallow (two pointers shared) - not used by the codebase
```

---

### Business Rule: BR-22 Error reporting contract and ownership of disconnect

**Overview**

`ReadMessage` classifies outcomes as `0` (ok), `1` (checksum mismatch), `2` (frame or init violation) and places a numeric detail in `ErrorType`. The component never closes a connection; every disconnect decision belongs to the caller.

**Detailed description**

`*ErrorCode = 0` is written at entry (`Code/CPSock.cpp:355`). Error 2 is produced by an `INITCODE` mismatch with `ErrorType = InitCode` (`:375-377`) and by a size-window violation with `ErrorType = Size` (`:402-403`). Error 1 is produced by a checksum mismatch with `ErrorType = Size` (`:460-461`). `ErrorType` is therefore overloaded: its meaning depends on `ErrorCode`, and a caller that logs it without checking first can print a size as if it were a magic dword or vice versa. In the checksum case the message pointer is still non-null; in both error-2 cases the return is null.

Caller behaviour is not uniform, and this is where the rule's consequences live. TMSrv's client loop logs and breaks without closing (`Code/TMSrv/Server.cpp:3990-3999`); the DB loop does the same (`:3907-3912`); the billing loop does the same (`:3798-3804`) even though billing can never set 1 or 2; DBSrv's game-server loop breaks without closing (`Code/DBSrv/Server.cpp:938-944`); the admin loops break without closing (`:785-791`, `:855-861`). In every case the socket remains registered and the next `FD_READ` re-enters the same state. Only a transport-level `Receive` failure or an `FD_CLOSE` causes closure, and those are different conditions.

Two caller-side branches are unreachable and one is misleading. `Receive()` is a `BOOL`, so `ret == -1` can never be true, yet TMSrv tests it twice and, in those branches, performs a raw `recv` into the whole receive buffer before closing (`Code/TMSrv/Server.cpp:3757-3786`, `:3957-3968`). The raw `recv` writes up to 131,072 bytes starting at offset 0 before the close, which is harmless only because the socket is closed immediately afterwards. Meanwhile the `ret = WSAGetLastError()` assignment inside the `ret == FALSE` branch overwrites the boolean with an error code, after which the code retries `Receive()` - correct in effect, but the logged value is the C runtime's last error only if no intervening call changed it, and `sprintf`/`Log` in between can.

**Rule workflow**

```
ReadMessage(&err, &type):
  err = 0
  drained                    -> return NULL
  init mismatch              -> err = 2, type = dword, return NULL
  size out of [12,8192]      -> err = 2, type = size, return NULL
  partial frame              -> return NULL
  checksum mismatch          -> err = 1, type = size, return pMsg
  valid                      -> return pMsg
caller contract:
  NULL  -> stop draining this event
  err 1/2 -> log; either break (all current callers) or disconnect (none do)
  pMsg  -> dispatch
```

---

### Business Rule: BR-23 Thread affinity and event-driven concurrency

**Overview**

All socket state is mutated on the thread that owns `hWndMain`, driven by posted window messages. There are no locks, no worker threads, and no reentrancy protection.

**Detailed description**

`StartListen` and both connect functions pass `hWndMain` (or a caller-supplied `HWND` for `StartListen`) to `WSAAsyncSelect`, so completions are delivered as `WM_USER+n` messages to that window (`Code/CPSock.cpp:163-169`, `:239-245`, `:323-329`). The servers run a single `GetMessage`/`DispatchMessage` loop (`Code/TMSrv/Server.cpp:3650-3654`), and every consumer handles the message inline: `Receive`, `ReadMessage`, dispatch to handlers, and `SendMessageA` all execute in that one call stack. The 1 Hz and 12 s timers are also window messages on the same thread (`:3723-3729`), so timer-driven sends and event-driven sends cannot interleave.

This is the component's entire concurrency story, and it is load-bearing. `pKeyWord` is a mutable global read on every frame; `ConnectPort` is a mutable global advanced by every failed bind; `LastSendTime` is a mutable global overwritten by every enqueue. None of them would survive concurrent access, and none needs to, because no second thread exists. The safety is architectural, not enforced: `CPSock` has no `CRITICAL_SECTION`, no atomic fields, and no assertion that the calling thread matches the owning window.

The constraint also couples the transport to the GUI. Failure paths call `MessageBox` (`:137`, `:149`, `:157`, `:165`, `:200`, `:220`, `:292`), which blocks the UI thread and therefore the entire message loop; `AddMessage`'s error paths call `Log`, which calls `SetWindowText` on the log window (`Code/TMSrv/Server.cpp:6725`). A transport error can therefore re-enter the window procedure through synchronous window messages. There is no service-mode variant, no console mode, and no way to run the transport without a window.

**Rule workflow**

```
thread = owner of hWndMain (single)
WSAAsyncSelect(..., hWndMain, msg, ...) -> kernel posts msg
loop: GetMessage -> DispatchMessage -> MainWndProc
  -> Receive (mutates nRecvPosition)
  -> ReadMessage (mutates nProcPosition, decrypts buffer in place)
  -> handler -> AddMessage (mutates global LastSendTime, pKeyWord read)
             -> SendMessageA (mutates nSentPosition)
no other thread touches CPSock state at any time
```

---

### Business Rule: BR-24 Debug packet validation gating

**Overview**

The only schema-level validation hook is compiled conditionally under `_PACKET_DEBUG`, is invoked from `AddMessage`'s enqueue path, and delegates to a function whose body is entirely commented out.

**Detailed description**

Inside `AddMessage`, after the header has been stamped, the code under `#ifdef _PACKET_DEBUG` casts the caller's buffer to `MSG_STANDARD*` and calls `BASE_CheckPacket(debug)`; if it returns true it logs `"**PACKET_DEBUG** Type:%d Size:%d"` (`Code/CPSock.cpp:544-552`). `_PACKET_DEBUG` is defined only in the Debug configurations (`Code/TMSrv/TMSrv.vcxproj:61`, `Code/DBSrv/DBSrv.vcxproj:61`) and absent from Release (`:79`, `:83`). The Debug build therefore compiles the block; the shipped Release build does not contain it at all.

Even in Debug the block cannot fire usefully. `BASE_CheckPacket` (`Code/Basedef.cpp:6475-6596`) opens a comment at `:6476` that does not close until `:6594`; the entire table of `if (m->Type == X && m->Size != sizeof(MSG_X))` assertions is inside it, and the function ends with `return FALSE;` (`:6595`). Every call site - `Code/CPSock.cpp:547`, `Code/TMSrv/Server.cpp:3898`, `:3983`, `Code/DBSrv/Server.cpp:848`, `:931` - is therefore a no-op whose enclosing `if` never executes.

The business meaning is that no layer enforces a per-message size contract. `ReadMessage` guarantees only that `Size` is between 12 and 8192; handlers cast the buffer to `MSG_*` and read fields at fixed offsets regardless of the declared size. Because the receive buffer is 128 KiB and is only zero-initialised at process start (never per connection), a short frame followed by a struct read returns bytes belonging to earlier traffic or stack-adjacent heap content rather than faulting - the classic "in-bounds garbage" failure mode. The send side has the mirror gap: `AddMessage` accepts any `Size` below the buffer limit, including sizes that violate the peer's own window (as at `Code/TMSrv/_MSG_Action.cpp:272`).

**Rule workflow**

```
AddMessage
  -> stamp header
  -> #ifdef _PACKET_DEBUG
       BASE_CheckPacket((MSG_STANDARD*)pMsg)
         -> body commented out; always FALSE
         -> log line never emitted
  -> transform and queue
Release build: block not compiled
ReadMessage
  -> validates only 12 <= Size <= 8192  (BR-07)
```

---

### Business Rule: BR-25 Address parameter contract

**Overview**

Every connect/bind entry point takes `int ip` interpreted as a 4-byte network-order IPv4 address. `0` means "any interface" for listeners. The contract is implicit and re-derived at each call site.

**Detailed description**

`StartListen` assigns `local_sin.sin_addr.s_addr = ip` directly (`Code/CPSock.cpp:144`), and both connect functions do the same for the local bind address (`:205`, `:278`). No byte-order conversion is applied, so the caller must supply the address exactly as `in_addr` expects it. TMSrv satisfies this by declaring `unsigned char LocalIP[4]` and filling it from a parsed dotted quad (`Code/TMSrv/Server.cpp:367`, `:3526-3529`), then passing `*(int*)LocalIP` (`:3560`, `:3601`). On a little-endian host, the four bytes `a.b.c.d` at increasing addresses reinterpret as the integer whose memory image is `a.b.c.d`, which is precisely the network-order value - the cast is correct by layout, not by arithmetic. DBSrv passes a literal `0` for both listeners (`Code/DBSrv/Server.cpp:586-587`), which binds all interfaces, and `INADDR_ANY` for the outbound `AdminClient` local bind (`:599`).

The remote address is handled differently and correctly: `inet_addr(HostAddr)` performs the textual conversion (`Code/CPSock.cpp:192`, `:305`), and the port is converted with `htons` (`:194`, `:307`). `inet_addr` accepts dotted-quad text and the `255.255.255.255` broadcast form; it returns `INADDR_NONE` for malformed input, which the code does not distinguish from the legitimate `255.255.255.255`. A hostname (rather than an address) is never accepted, because `inet_addr` does not resolve names - yet the parameter is named `HostAddr` and `StartListen` calls `gethostname`, suggesting an earlier design that did resolve names.

The address is also used as identity, outside the transport. DBSrv matches accepted game servers by comparing `pUser[i].IP` against the peer address (`Code/DBSrv/Server.cpp:1086-1093`) and by comparing the peer address against `LocalIP` to reject off-LAN connections (`:1150`); TMSrv's `GetUserFromSocket` matches by handle instead (`Code/TMSrv/Server.cpp:6729-6741`). `GetAdminFromSocket` in DBSrv (`:2180`) does the same for admin sessions. The `ip` argument to the transport therefore participates in an authorisation decision made two layers up, which is why an incorrect endianness assumption would silently bind the wrong interface rather than fail loudly.

**Rule workflow**

```
caller: LocalIP[4] parsed from "a.b.c.d" (TMSrv) or 0 (DBSrv)
  -> (int)LocalIP bytes -> sin_addr.s_addr (no htonl)
  -> bind(ip:port)
remote: inet_addr("a.b.c.d") -> sin_addr.s_addr ; htons(port)
identity: DBSrv compares stored peer IP for slot reuse and LAN gating
```

---

## 4. Component Structure

```
Code/
+-- CPSock.h                                   # public contract: HEADER, limits, message IDs, _AUTH_GAME
|   +-- :25-31   WSA_READ / WSA_READDB / WSA_ACCEPT / WSA_READBILL / WSA_ACCEPTADMIN / WSA_READADMIN / WSA_READADMINCLIENT
|   +-- :33      MAX_PENDING_CONNECTS = 8
|   +-- :35-36   RECV_BUFFER_SIZE / SEND_BUFFER_SIZE = 128*1024
|   +-- :38      MAX_MESSAGE_SIZE = 8192            (comment claims 4K)
|   +-- :40      INITCODE = 0x1F11F311
|   +-- :42-50   struct HEADER  (12 bytes, unpacked)
|   +-- :53-86   class CPSock   (7 public fields, 14 public methods)
|   +-- :88-91   struct _AUTH_GAME { char Unk[196]; }
|   +-- :132     #define g_cGame (sizeof(_AUTH_GAME))
|
+-- CPSock.cpp                                 # implementation shared by TMSrv and DBSrv
    +-- :27      int ConnectPort                  (global, no header declaration)
    +-- :29-46   unsigned char pKeyWord[512]      (global, non-const, "7.xx keys")
    +-- :48-51   extern hWndMain, CurrentTime, LastSendTime; Log prototype
    +-- :62-73   constructor: 2 x malloc(128K), positions zeroed
    +-- :75-82   destructor: free both buffers
    +-- :84-98   CloseSocket
    +-- :109-117 WSAInitialize (WSAStartup 1.1)
    +-- :128-174 StartListen (socket/bind/listen/WSAAsyncSelect FD_ACCEPT)
    +-- :176-255 ConnectServer (bind fallback, connect, INITCODE, FD_READ|FD_CLOSE)
    +-- :257-334 ConnectBillServer (bind fallback +6000, no INITCODE)
    +-- :336-347 Receive (single recv, full-buffer treated as failure)
    +-- :353-467 ReadMessage (init gate, header parse, in-place de-obfuscation, checksum)
    +-- :469-496 ReadBillMessage (196-byte pop, errors never set)
    +-- :498-511 SendBillMessage (196-byte append -> SendMessageA)
    +-- :513-591 AddMessage (capacity guard, header stamp, transform, checksum, copy)
    +-- :593-603 RefreshRecvBuffer (no call sites)
    +-- :605-615 RefreshSendBuffer (called from SendMessageA:632)
    +-- :617-684 SendMessageA (single send attempt, success always returned)
    +-- :686-693 SendOneMessage (AddMessage result discarded)
```

Consumers that constitute the component boundary in practice:

```
Code/TMSrv/Server.cpp
  :281-282   CurrentTime, LastSendTime definitions
  :250,262   BillServerSocket, DBServerSocket globals
  :371       ListenSocket global
  :269       CUser pUser[MAX_USER]  -> 1000 CPSock instances
  :3496      WSAInitialize
  :3562      ConnectServer  -> DBSrv:7514 (WSA_READDB)
  :3603      ConnectBillServer (WSA_READBILL)
  :3634      StartListen(GAME_PORT 8281, WSA_ACCEPT)
  :3731-3810 WSA_READBILL handler
  :3812-3917 WSA_READDB handler
  :3919-4004 WSA_READ handler
  :4006-4045 WSA_ACCEPT handler
  :6729-6741 GetUserFromSocket (handle -> slot)
  :1377-1391 SendBilling (stub) / SendBilling2 (real)
  :4300-4302 ProcessBILLMessage (empty)

Code/TMSrv/CUser.cpp      :51-79 AcceptUser, :81-92 CloseUser
Code/TMSrv/ProcessSecMinTimer.cpp  :201-221 1 Hz send flush sweep
Code/DBSrv/Server.cpp     :520 WSAInitialize, :586-587 listeners, :599 AdminClient connect
                          :756-795 admin-client loop, :797-875 admin loop, :877-949 game loop
                          :951-1066 admin accept, :1068-1191 game accept
Code/DBSrv/CUser.cpp      :39-65 AcceptUser
Code/Basedef.h            :925-930 _MSG macro (duplicate header layout), :943-946 MSG_STANDARD
Code/Basedef.cpp          :6475-6596 BASE_CheckPacket (stub)
```

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
CPSock
 +-- CPSock.h            (HEADER, limits, message IDs, _AUTH_GAME)
 +-- Basedef.h           (only for MSG_STANDARD in the _PACKET_DEBUG block, :545)
 +-- Log()               (extern, resolved to Code/TMSrv/Server.cpp:6701 or Code/DBSrv/Server.cpp:2115)
 +-- hWndMain            (extern, Win32 window handle)
 +-- CurrentTime         (extern, timeGetTime() set at Code/TMSrv/Server.cpp:3625)
 +-- LastSendTime        (extern, written at :542 only)
 +-- BASE_CheckPacket()  (extern, Code/Basedef.cpp:6475; stub)

Consumers
 TMSrv::MainWndProc  -> GetUserFromSocket -> pUser[i].cSock -> CPSock::Receive -> CPSock::ReadMessage
                                                             -> ProcessClientMessage -> Exec_MSG_* -> SendFunc
 SendFunc / handlers -> CPSock::AddMessage -> CPSock::SendMessageA
 TMSrv::SendBilling2 -> CPSock::SendBillMessage -> CPSock::SendMessageA
 TMSrv::ProcessSecMinTimer -> CPSock::SendMessageA (1 Hz flush)
 DBSrv::MainWndProc  -> pUser/pAdmin[i].cSock -> same read path -> ProcessClientMessage / ProcessAdminMessage
 DBSrv::CFileDB / CRanking -> SendOneMessage
```

### 5.2 Symbol-level dependency table

| Symbol | Kind | Provider | Used at | Notes |
|---|---|---|---|---|
| `hWndMain` | extern `HWND` | `Code/TMSrv/Server.cpp`, `Code/DBSrv/Server.cpp` | `Code/CPSock.cpp:48`, `:239`, `:323` | Transport cannot exist without a window |
| `CurrentTime` | extern `unsigned int` | `Code/TMSrv/Server.cpp:281`, `Code/DBSrv/Server.cpp:55` | `:49`, `:541` | Stamped into every header |
| `LastSendTime` | extern `unsigned int` | `Code/TMSrv/Server.cpp:282`, `Code/DBSrv/Server.cpp:56` | `:50`, `:542` | Global, not per-socket |
| `Log` | extern function | `Code/TMSrv/Server.cpp:6701`, `Code/DBSrv/Server.cpp:2115` | `:51`, `:521`, `:530` | Writes a file and `SetWindowText` |
| `BASE_CheckPacket` | extern function | `Code/Basedef.cpp:6475` | `:547` | Stub returning `FALSE` |
| `MSG_STANDARD` | struct | `Code/Basedef.h:943-946` | `:545` | Duplicate of `HEADER` layout |
| `rand` | CRT | `<stdlib.h>` | `:535` | Never seeded |
| `closesocket`, `recv`, `send`, `socket`, `bind`, `listen`, `accept`, `connect`, `WSAAsyncSelect`, `WSAStartup`, `WSAGetLastError`, `gethostname`, `inet_addr`, `htons` | Win32/Winsock | `ws2_32.lib` | throughout | Winsock 1.1 header in TMSrv, 2.x in DBSrv |

### 5.3 External dependencies

| Dependency | Version / form | Purpose | Evidence |
|---|---|---|---|
| Winsock (`ws2_32.lib`) | `WSAStartup(MAKEWORD(1,1))` | All socket I/O | `Code/CPSock.cpp:113`; `AdditionalDependencies` in both vcxproj |
| Win32 user32 / kernel32 | via `<Windows.h>` | `MessageBox`, `gethostname`, window messaging | `Code/CPSock.cpp:137-165` |
| C runtime | static CRT in Debug, `MultiThreadedDebugDLL` in TMSrv Release | `malloc`, `free`, `memcpy`, `rand`, `sprintf` | `Code/CPSock.cpp:66`, `:586`; `Code/TMSrv/TMSrv.vcxproj:80` |
| MSVC compiler / x86 ABI | Win32, default packing | `HEADER` layout and pointer casts | `Code/CPSock.h:42-50` |
| `Basedef.h` protocol constants | compile-time | `MSG_STANDARD` in debug block | `Code/CPSock.cpp:545` |

No third-party networking library, no TLS library, no serialization framework, and no configuration file is used by the component. All limits are compile-time constants in `CPSock.h`.

---

## 6. Afferent and Efferent Coupling

Counting method: afferent = distinct translation units (`.cpp`/`.h`) that reference the symbol or include the header, measured by exhaustive static search over `Code/` (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`); efferent = distinct external/internal entities the symbol directly invokes or reads.

| Component (C++ class / member / free symbol) | Afferent | Efferent | Criticality |
|---|---|---|---|
| `CPSock` (class as a whole) | 40 TUs reference `CPSock` or `cSock.`; 16 include `CPSock.h` | 6 (`Basedef.h`, `Log`, `hWndMain`, `CurrentTime`/`LastSendTime`, Winsock, CRT) | Critical (single transport) |
| `CPSock::AddMessage` | 67 references across 17 files | 5 (guards, `rand`, `pKeyWord`, `Log`, globals) | Critical |
| `CPSock::SendOneMessage` | 129 references across 26 files | 2 (`AddMessage`, `SendMessageA`) | Critical |
| `CPSock::SendMessageA` | 32 references across 10 files | 2 (`RefreshSendBuffer`, `send`) | High |
| `CPSock::ReadMessage` | 5 call sites in 2 files (`TMSrv/Server.cpp:3889,3975`; `DBSrv/Server.cpp:780,840,924`) | 2 (`pKeyWord`, buffer state) | Critical |
| `CPSock::Receive` | 8 call sites in 2 files (`TMSrv/Server.cpp:3745,3763,3850,3938,3946`; `DBSrv/Server.cpp:767,825,904`) | 1 (`recv`) | Critical |
| `CPSock::CloseSocket` | 40 references across 6 files | 1 (`closesocket`) | High |
| `CPSock::ReadBillMessage` | 1 call site (`TMSrv/Server.cpp:3793`) | 0 | Medium |
| `CPSock::SendBillMessage` | 1 call site (`TMSrv/Server.cpp:1388`) | 2 (`SendMessageA`, `g_cGame`) | Medium |
| `CPSock::ConnectServer` | 4 call sites (`TMSrv/Server.cpp:3562,3827,3862`; `DBSrv/Server.cpp:599`) | 1 (`send`) | High |
| `CPSock::ConnectBillServer` | 3 call sites (`TMSrv/Server.cpp:3603`, `TMSrv/ProcessSecMinTimer.cpp:143`, `TMSrv/imple.cpp:1151`) | 0 | Medium |
| `CPSock::StartListen` | 3 call sites (`TMSrv/Server.cpp:3634`; `DBSrv/Server.cpp:586,587`) | 2 (`gethostname`, `MessageBox`) | High |
| `CPSock::WSAInitialize` | 2 call sites | 1 (`WSAStartup`) | High |
| `CPSock::RefreshRecvBuffer` | 0 (dead public API) | 1 (`memcpy`) | None |
| `CPSock::RefreshSendBuffer` | 1 (`SendMessageA:632`) | 1 (`memcpy`) | Low |
| `HEADER` (struct) | 0 external (only `CPSock.cpp`/`.h`); layout duplicated by `_MSG` in `Basedef.h:925-930` | 0 | Critical (wire contract) |
| `pKeyWord` (512-byte table) | 0 external (external linkage, no header declaration) | 0 | Critical (protocol) |
| `INITCODE` (macro) | 0 external | 0 | Critical (handshake) |
| `RECV_BUFFER_SIZE` / `SEND_BUFFER_SIZE` | 5 / 7 references, 3 / 2 files (incl. raw `recv` in `TMSrv/Server.cpp:3780,3963`) | 0 | High |

Interpretation of the numbers. The class has very high afferent coupling (40 TUs) because it is embedded by value in `CUser` and exposed as a public field (`unsigned int Sock`, all four positions), so consumers read and write transport internals directly rather than through methods. It has low efferent coupling (6) because it is a leaf in the dependency graph: it includes only `Windows.h`, `stdio.h`, `stdlib.h`, `CPSock.h` and `Basedef.h`, and calls only `Log`, `BASE_CheckPacket` and the three globals. That combination - many dependents, few dependencies - is the classic profile of stable infrastructure, and it explains why the file has not changed since 2018 apart from cosmetic edits (last functional commit `489c4e0 "Add project files"`, last edits `47c9aa8`/`90336a3` in May 2018 which touched only the licence header, an accented comment and a trailing newline).

Cohesion is moderate. Every method operates on the same four positions and two buffers, so functional cohesion is high. But the class serves three different protocols (game/DB framed, billing unframed, listener bootstrap) and owns two unrelated concerns (socket lifecycle and message codec). The billing variant in particular shares only the position arithmetic with the framed path, and the listener methods share nothing but the class name.

---

## 7. Endpoints

The component exposes no HTTP, REST, GraphQL or gRPC surface. It exposes raw TCP byte streams: two passive listeners in each executable's composition, plus outbound connectors. The transport itself does not own ports; the callers pass them.

| Endpoint | Direction | Owner / call site | Protocol | Frame format |
|---|---|---|---|---|
| `0.0.0.0:8281` (`GAME_PORT`) | Listen (`FD_ACCEPT`, `WSA_ACCEPT`) | TMSrv `Code/TMSrv/Server.cpp:3634` | TCP + `INITCODE` + framed | `HEADER` + obfuscated body |
| `<LocalIP>:8281` when `LocalIP.txt` is present | Listen (bound to interface) | TMSrv `Code/TMSrv/Server.cpp:3560,3634` | TCP + `INITCODE` + framed | same |
| `0.0.0.0:7514` (`DB_PORT`) | Listen (`FD_ACCEPT`, `WSA_ACCEPT`) | DBSrv `Code/DBSrv/Server.cpp:586` | TCP + `INITCODE` + framed | same |
| `0.0.0.0:8895` (`ADMIN_PORT`) | Listen (`FD_ACCEPT`, `WSA_ACCEPTADMIN`) | DBSrv `Code/DBSrv/Server.cpp:587` | TCP + `INITCODE` + framed | same |
| DBSrv `:7514` (remote) | Connect (`WSA_READDB`) | TMSrv `Code/TMSrv/Server.cpp:3562` | TCP + `INITCODE` | framed |
| Billing host:port from `biserver.txt` | Connect (`WSA_READBILL`) | TMSrv `Code/TMSrv/Server.cpp:3603`, `Code/TMSrv/ProcessSecMinTimer.cpp:143` | TCP, no handshake | 196-byte `_AUTH_GAME` records |
| Redirect host:port from `redirect.txt` | Connect (`WSA_READADMINCLIENT`) | DBSrv `Code/DBSrv/Server.cpp:599` | TCP + `INITCODE` | framed |
| Reconnect to DBSrv after failure | Connect (2 attempts, 200 ms apart) | TMSrv `Code/TMSrv/Server.cpp:3823-3833`, `:3858-3868` | TCP + `INITCODE` | framed |

`ADMIN_PORT` is declared "DB server listening port, NPTool connects to it" (`Code/Basedef.h:46`), which identifies the expected external peer of `:8895`. The external peers of `:8281` are the closed-source W2 game clients and, in a multi-server deployment, other TMSrv instances acting as clients. All of them must satisfy BR-06.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling (as implemented) |
|---|---|---|---|---|---|
| W2 game client | External TCP clients | All gameplay traffic | TCP `:8281`, `WSAAsyncSelect` | `HEADER` + transform + checksum; `MSG_*` payloads | `Receive` FALSE -> retry once -> `CloseUser`; `Error 1/2` -> log only, socket stays open; `FD_CLOSE` -> `CloseUser` |
| DBSrv (from TMSrv) | Internal service | Account/character persistence, guild, ranking, capsule | TCP `:7514` | same framed format | `Receive` FALSE -> close, reconnect twice, else `PostQuitMessage`; `Error 1/2` -> log only |
| Billing server (outbound) | External service | Session and charging records | TCP, host from `biserver.txt` | 196-byte `_AUTH_GAME` records, no framing | `Receive` FALSE -> close + `BillCounter = 360`; inbound records discarded by empty handler; send result ignored by caller |
| NPTool / admin | External TCP clients | Admin commands, magic trumpet, notices, account info | TCP `:8895` | framed format | `Receive` failure -> log; unknown IP -> close; `Error 1/2` -> log only |
| Redirect server (DBSrv outbound) | External service | Character transfer / redirection | TCP from `redirect.txt` | framed format | `Receive` failure -> log only; `FD_CLOSE` -> `CloseSocket` |
| Win32 window message queue | OS integration | Event delivery for all sockets | `WM_USER + 2..7, +100` | `wParam` = SOCKET, `lParam` = `MAKEWORD(event,error)` | None; decoding is duplicated in each consumer |
| Host server globals (`Log`, clock) | In-process | Diagnostics and timestamps | direct calls | `char*` strings, `unsigned int` | `Log` writes to file and `SetWindowText`; no failure path |
| `Basedef.h` protocol definitions | Compile-time | `MSG_STANDARD` in debug block | macro/struct | 12-byte header duplicate | None; no static assertion that sizes match `HEADER` |
| MSVC / Winsock | Platform | Sockets, CRT | Win32 API | native | `MessageBox` on listener failures; WSA error codes mostly discarded |

Notable integration asymmetries:

- **TMSrv has a 1 Hz flush sweep; no other socket does.** A partially written DB/billing/admin tail has no timer-driven retry.
- **DBSrv treats `Receive` failure non-fatally** (`Code/DBSrv/Server.cpp:904-916` resets positions and keeps the slot), while TMSrv treats it fatally (`CloseUser`) and treats protocol errors non-fatally. The two sides of the same link therefore disagree about which failures are terminal.
- **Billing inbound is a black hole.** `ProcessBILLMessage` is empty (`Code/TMSrv/Server.cpp:4300-4302`), so the integration is write-only in practice despite a complete read path.
- **The `INITCODE` requirement applies to external peers** (clients, admin tools) but is nowhere documented; it is only discoverable in `CPSock.cpp:366-383`.

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / consequence |
|---|---|---|---|
| Facade / wrapper | `CPSock` hides `socket`/`bind`/`connect`/`recv`/`send` behind 14 methods | `Code/CPSock.h:53-86` | Single transport seam; also the single point of failure |
| Reactor (async-select variant) | `WSAAsyncSelect` + window messages; event decoding in consumers | `Code/CPSock.cpp:163,239,323`; `Code/TMSrv/Server.cpp:3718` | All I/O serialised on the UI thread; no threads, no IOCP |
| Length-prefixed framing / TLV-lite | 12-byte header with `Size`, `Type`, `ID` | `Code/CPSock.h:42-50` | One frame per struct; no schema, no versioning |
| Fixed-size ring buffer (manual) | Four positions over two static-size arrays; compaction helpers | `Code/CPSock.cpp:593-615` | Zero allocation per message; receive compaction unused |
| Symmetric stream cipher (toy) | Per-byte add/sub keyed by table + ring position | `Code/CPSock.cpp:430-453`, `:558-581` | Obfuscation only; key material in both binaries |
| Shared-secret handshake | Single constant dword, no nonce | `Code/CPSock.cpp:249-252`, `:366-383` | Identifies "a peer that speaks this protocol", not a session |
| Strategy-by-branch (implicit) | Billing vs framed handled by separate methods, not polymorphism | `Code/CPSock.cpp:469-511` | Two protocols in one class; no runtime selection |
| Global mutable state | `pKeyWord`, `ConnectPort`, `LastSendTime`, `CurrentTime` | `Code/CPSock.cpp:27,29,49-50` | Simple, non-reentrant, non-testable in isolation |
| Embedded-by-value composition | `CUser` contains `CPSock` | `Code/TMSrv/CUser.h:47`, `Code/DBSrv/CUser.h:34` | Per-session transport state, 256 KiB each, no indirection |
| Two-phase init | Constructor allocates; `WSAInitialize`/`StartListen`/`Connect*` activate | `Code/CPSock.cpp:62-73`, `:109` | Objects exist before Winsock is initialised |
| Defensive index reset (partial) | Position validation in `SendMessageA` resets corrupted queues | `Code/CPSock.cpp:634-656` | Self-healing for send state; no equivalent for receive |

### 9.1 Build and configuration handling

| Aspect | TMSrv Debug | TMSrv Release | DBSrv Debug | DBSrv Release |
|---|---|---|---|---|
| Platform | Win32 | Win32 | Win32 | Win32 |
| `_PACKET_DEBUG` | defined (`TMSrv.vcxproj:61`) | not defined (`:79`) | defined (`DBSrv.vcxproj:61`) | not defined (`:83`) |
| RuntimeLibrary | `MultiThreadedDebug` (`:62`) | `MultiThreadedDebugDLL` (`:80`) | `MultiThreadedDebug` (`:62`) | not overridden |
| Warning level | Level3 | Level3 | Level3 | Level3 |
| `CPSock.cpp` in build | `:92` | same ItemGroup | `:95` | same ItemGroup |
| UNICODE | not defined | not defined | not defined | not defined |

The absence of `UNICODE` is functionally significant. `CPSock::SendBillMessage` calls `SendMessage();` with no arguments (`Code/CPSock.cpp:508`). Because `SendMessage` is a Win32 macro, it expands to `SendMessageA`, and that name matches the member function `CPSock::SendMessageA()` declared in the class - so the call compiles and means "flush this socket's send queue". If `UNICODE` were ever defined, the macro would expand to `SendMessageW`, a four-argument global function, and the translation unit would fail to compile. The behaviour is correct today by a coincidence of macro naming, not by intent.

The component contains no configuration surface: no environment variables, no INI reader, no feature flags. The only runtime-configurable values are the addresses and ports supplied by callers (`LocalIP.txt`, `ServerList.txt`, `biserver.txt`, `redirect.txt` - all read by the host servers, not by `CPSock`). Every protocol parameter is a compile-time constant in `CPSock.h`.

---

## 10. Technical Debt & Risks

| ID | Risk Level | Component Area | Issue | Impact | Evidence |
|---|---|---|---|---|---|
| TD-01 | High | Integrity | Checksum mismatch returns the message; dispatch suppression depends on every caller remembering to test `Error` | A caller that omits the test processes corrupted data; adversaries can forge valid checksums with the embedded table | `Code/CPSock.cpp:457-464`; tests at `TMSrv/Server.cpp:3990`, `DBSrv/Server.cpp:855,938` |
| TD-02 | High | Transmission | `SOCKET_ERROR` and partial send both return `TRUE`; captured `err` unused | Write failures are invisible; callers cannot back off, retry or disconnect | `Code/CPSock.cpp:660-683` |
| TD-03 | High | Queueing | `SendOneMessage` discards the `AddMessage` result | Silent message loss when the 128 KiB queue is full; DB saves and client notifications vanish with only a log line | `Code/CPSock.cpp:686-692`; log at `:520` |
| TD-04 | High | Reception | `tReceiveSize == Rest` classified as failure without advancing | Permanent byte-stream desync and data loss on a full-buffer read; retry overwrites received bytes | `Code/CPSock.cpp:338-342`; caller retry `TMSrv/Server.cpp:3940-3956` |
| TD-05 | High | Eventing | `WSAAsyncSelect(...) > 0` is never true; failure detection disabled at all five arming sites | An unarmed socket is accepted, stored and never serviced; clients hang with no log | `Code/CPSock.cpp:163,239,323`; `TMSrv/CUser.cpp:61`; `DBSrv/CUser.cpp:49` |
| TD-06 | High | Validation | No per-message schema check; `BASE_CheckPacket` body commented out; gated by Debug-only macro | Handlers read struct fields beyond `Size` from stale buffer contents; malformed frames reach business logic | `Code/CPSock.cpp:397`, `:544-552`; `Code/Basedef.cpp:6475-6596`; `TMSrv.vcxproj:61,79` |
| TD-07 | High | Send validation | `AddMessage` accepts `Size < sizeof(HEADER)`, `Size > MAX_MESSAGE_SIZE`, negative `Size` | 4-byte frames are already produced (`_MSG_Action.cpp:272,278`) and cause the peer to discard its buffer; negative size corrupts `nSendPosition` | `Code/CPSock.cpp:518`; `Code/TMSrv/_MSG_Action.cpp:272` |
| TD-08 | High | Protocol error handling | Error 1/2 never closes the connection in any consumer | Sticky-init and repeated-checksum connections consume slots and log indefinitely | `Code/CPSock.cpp:375-379`; `TMSrv/Server.cpp:3990`; `DBSrv/Server.cpp:785,855,938` |
| TD-09 | Medium | Billing framing | Fixed 196-byte records with no length, type or checksum; `ReadBillMessage` never reports errors | Any byte slip permanently misaligns the billing stream undetected | `Code/CPSock.cpp:469-496` |
| TD-10 | Medium | Billing integration | `ProcessBILLMessage` empty; `SendBilling` stub; `SendBilling2` ignores send result | Billing is effectively write-only and failures are unobservable | `Code/TMSrv/Server.cpp:4300-4302`, `:1377-1380`, `:1388` |
| TD-11 | Medium | Reception | `recv() == 0` (graceful close) treated as success with no progress | Relies entirely on `FD_CLOSE` arriving; a raced read wastes an iteration and can loop on `Rest == 0` | `Code/CPSock.cpp:341-346` |
| TD-12 | Medium | Resource | 262,144 bytes per instance, `malloc` unchecked, 1,000 instances in TMSrv | ~250 MiB committed at static init; allocation failure becomes a null-pointer write in `AddMessage`/`Receive` | `Code/CPSock.cpp:66-67`; `Code/TMSrv/Server.cpp:269`; `Code/Basedef.h:56` |
| TD-13 | Medium | Ownership | No copy constructor / assignment operator; class holds owned pointers | Any by-value copy produces double free and shared buffers; safety depends on convention in `DBSrv/Server.cpp:1152-1157` | `Code/CPSock.h:53-86`; `Code/CPSock.cpp:75-82` |
| TD-14 | Medium | Handshake | `INITCODE` is a constant with no nonce, no version and no authentication; a wrong first dword is logged forever rather than rejected | Any peer that knows the constant is "authenticated"; protocol version cannot evolve | `Code/CPSock.cpp:249-252`, `:366-383`; `Code/CPSock.h:40` |
| TD-15 | Medium | Crypto/obfuscation | Static 512-byte table, transmitted keyword index, 8-bit additive checksum, `rand()` unseeded | Traffic is trivially deobfuscated and forged; keyword sequence is deterministic | `Code/CPSock.cpp:29-46`, `:535`; no `srand` anywhere |
| TD-16 | Medium | Error semantics | `ErrorType` overloaded (`Size` vs `InitCode`); `ReadBillMessage` always zero; `ret == -1` branches dead | Logs misdescribe failures; dead code misleads maintainers | `Code/CPSock.cpp:376,403,461,471`; `TMSrv/Server.cpp:3757,3957` |
| TD-17 | Medium | Port cursor | `ConnectPort` global, never reset, shared by game and billing ladders with different increments | Fallback ports drift with history; concurrent client objects share one cursor | `Code/CPSock.cpp:27`, `:210-216`, `:287-303` |
| TD-18 | Medium | UI coupling | `MessageBox` on listener/connect failures; `Log` -> `SetWindowText` in enqueue errors | Blocks the message loop; no headless/service mode possible | `Code/CPSock.cpp:137-165`, `:220`, `:292`, `:521` |
| TD-19 | Low | Listener lifecycle | `StartListen` overwrites `Sock` without closing a previous listener; `local_sin` not zeroed; `gethostname` result unused | Listener leak on rebind; uninitialised `sin_zero`; pointless name lookup on the UI thread | `Code/CPSock.cpp:131-171` |
| TD-20 | Low | Dead API | `RefreshRecvBuffer` has zero call sites; `ConnectBillServer` diagnostics built and discarded | Dead code implies compaction that never happens; misleading diagnostics | `Code/CPSock.cpp:593-603`, `:300-321` |
| TD-21 | Low | Type width | `unsigned int Sock` holds a `SOCKET`; `Sock <= 0`/`Sock < 0` comparisons partly dead; `int tSock` in `AcceptUser` | Not 64-bit safe; dead conditions suggest false error handling | `Code/CPSock.h:56`; `Code/CPSock.cpp:623,634,647`; `TMSrv/CUser.cpp:56` |
| TD-22 | Low | Documentation | Comments contradict constants (`MAX_MESSAGE_SIZE` "4K" vs 8192; buffers "64k" vs 128K); `_AUTH_GAME` marked "NEEDS TO BE FIXED" | Reviewers and protocol implementers are misled about limits | `Code/CPSock.h:35-38,88` |
| TD-23 | Low | Build/portability | Shared TU compiled with Winsock 1.1 header in TMSrv and 2.x in DBSrv; TMSrv Release uses `MultiThreadedDebugDLL`; `SendMessage()` depends on the `SendMessage`->`SendMessageA` macro | Divergent ABIs for the same source; UNICODE enablement breaks compilation; heap mode mismatch between Debug/Release | `Code/CPSock.cpp:20,508`; `TMSrv/Server.cpp:21` vs `DBSrv/Server.cpp:21`; `TMSrv.vcxproj:80` |
| TD-24 | Low | Observability | No counters for accepted/rejected frames, checksum failures, queue-full drops, or send failures | Only ad-hoc `Log` strings; no way to quantify loss in production | `Code/CPSock.cpp:520,529` |
| TD-25 | Low | Testability | Global `hWndMain`, `CurrentTime`, `LastSendTime`, `Log`; file-scope mutable `pKeyWord`/`ConnectPort`; no injection seam | The component cannot be instantiated or driven without a window, a clock and a log sink | `Code/CPSock.cpp:27-51` |

---

## 11. Test Coverage Analysis

### 11.1 Test inventory and evidence

No tests exist for this component, and no test infrastructure exists in the repository.

| Search performed | Scope | Result |
|---|---|---|
| Solution project list (`W2PP Code Project.sln`) | whole repo | 3 entries: `DBSrv`, `TMSrv`, `Solution Items`. No test project |
| File-name search `*test*`, `*Test*`, `*TEST*` | whole repo, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` | 1 match: `W2PP Code Project.sln` (name substring "Project" contains no test token; the search returned the solution only, no test file) |
| Content search `gtest`, `catch2`, `CppUnit`, `BOOST_TEST`, `assert(` | `Code/**/*.cpp`, `Code/**/*.h` | 0 matches |
| Build systems: `CMakeLists.txt`, `Makefile`, `*.yml`, `*.yaml`, CI configs | whole repo | 0 matches |
| Framework project types (`.vcxproj` with test target) | `Code/TMSrv`, `Code/DBSrv` | 0; both are `Application` executables |
| Test fixtures / mocks / harness directories | whole repo | none |
| Executable artefacts | `Code/release/` | `TMSrv.exe` (5,094,440 bytes, 2026-11-06 17:25:50) and `TMSrv.pdb`; no test binary, no DBSrv binary |

No test files could be located anywhere in the project, including outside the component directory. The closest thing to an assertion is the `_PACKET_DEBUG` block in `AddMessage` (`Code/CPSock.cpp:544-552`) and the four debug blocks in the servers (`TMSrv/Server.cpp:3895-3905`, `:3980-3988`; `DBSrv/Server.cpp:845-853`, `:929-936`). All of them call `BASE_CheckPacket`, which is a stub returning `FALSE` (`Code/Basedef.cpp:6475-6596`), and all are compiled only in Debug. They are logging hooks, not tests: none asserts, none fails a build, and none is executed by any automation.

### 11.2 Coverage by function

| Function | Unit Tests | Integration Tests | Estimated Coverage | Test Quality |
|---|---|---|---|---|
| `CPSock::CPSock` / `~CPSock` | 0 | 0 | 0% | No test; allocation failure path untested and unchecked |
| `CPSock::CloseSocket` | 0 | 0 | 0% | No test; idempotence and stale-handle reuse unverified |
| `CPSock::WSAInitialize` | 0 | 0 | 0% | No test; only the success path is exercised in production |
| `CPSock::StartListen` | 0 | 0 | 0% | Bind/listen failure branches only reachable manually (modal dialog) |
| `CPSock::ConnectServer` | 0 | 0 | 0% | Bind-fallback ladder never exercised automatically; 3 levels unverified |
| `CPSock::ConnectBillServer` | 0 | 0 | 0% | Same; plus the unused diagnostic path |
| `CPSock::Receive` | 0 | 0 | 0% | Full-buffer, zero-byte and error paths have no coverage |
| `CPSock::ReadMessage` | 0 | 0 | 0% | 20+ branches (init, size window, partial, checksum) entirely untested |
| `CPSock::ReadBillMessage` | 0 | 0 | 0% | 196-byte boundary behaviour untested |
| `CPSock::SendBillMessage` | 0 | 0 | 0% | Capacity guard untested |
| `CPSock::AddMessage` | 0 | 0 | 0% | Transform/checksum symmetry never verified against a reference implementation |
| `CPSock::SendMessageA` | 0 | 0 | 0% | Partial-send and error paths unverified |
| `CPSock::SendOneMessage` | 0 | 0 | 0% | Silent-drop behaviour undocumented by any test |
| `CPSock::RefreshRecvBuffer` / `RefreshSendBuffer` | 0 | 0 | 0% | Receive variant dead; send variant only exercised indirectly |
| Header codec (implicit) | 0 | 0 | 0% | No golden-frame test; no cross-implementation vector |

### 11.3 Consequence for the rule inventory

Every business rule in Section 3 is verified only by reading the source and, for a few, by observing the shipped binary's existence. The rules with the highest combination of impact and zero verification are:

| Rule | Why untested behaviour matters |
|---|---|
| BR-11 / BR-12 transform + checksum | A one-off error in the shift or the accumulator would corrupt 100% of traffic and be attributed to "network problems"; no reference vector exists |
| BR-16 full-buffer classification | Requires a 128 KiB in-flight burst to reproduce; no harness can generate it |
| BR-07 / BR-08 framing boundaries | Exact 12-byte and 8192-byte frames are untested edge cases |
| BR-06 init gate | The sticky-error behaviour is only observable by connecting with a wrong first dword |
| BR-04 port fallback | Needs bind failure injection; never exercised |
| BR-15 partial send | Needs socket backpressure injection; never exercised |
| BR-18 billing framing | No consumer validates content, so a misaligned stream is invisible |

The only empirical evidence available in the repository is the prebuilt `Code/release/TMSrv.exe` (timestamp 2026-11-06), which demonstrates that the source compiles and links in the environment that produced it. It does not demonstrate that any rule behaves as documented, and it cannot be re-run deterministically because it depends on `LocalIP.txt`, `ServerList.txt`, `biserver.txt`, a DBSrv peer, and a billing peer.

---

## Appendix A - Evidence Index

| File | Lines | Content used |
|---|---|---|
| `Code/CPSock.h` | 25-31 | WSA message identifiers |
| `Code/CPSock.h` | 33, 35-38, 40 | backlog, buffer sizes, message size limit, INITCODE |
| `Code/CPSock.h` | 42-50 | `HEADER` layout |
| `Code/CPSock.h` | 53-86 | class surface (fields and methods) |
| `Code/CPSock.h` | 88-91, 132 | `_AUTH_GAME`, `g_cGame` |
| `Code/CPSock.cpp` | 20-25 | includes (`Windows.h`, `stdio.h`, `stdlib.h`, `CPSock.h`, `Basedef.h`) |
| `Code/CPSock.cpp` | 27, 29-46 | `ConnectPort`, `pKeyWord[512]` |
| `Code/CPSock.cpp` | 48-51 | external globals and `Log` prototype |
| `Code/CPSock.cpp` | 62-82 | constructor / destructor |
| `Code/CPSock.cpp` | 84-98 | `CloseSocket` |
| `Code/CPSock.cpp` | 109-117 | `WSAInitialize` (1.1) |
| `Code/CPSock.cpp` | 128-174 | `StartListen` |
| `Code/CPSock.cpp` | 176-255 | `ConnectServer`, INITCODE send |
| `Code/CPSock.cpp` | 257-334 | `ConnectBillServer` |
| `Code/CPSock.cpp` | 336-347 | `Receive` |
| `Code/CPSock.cpp` | 353-467 | `ReadMessage` (init gate, size window, transform, checksum) |
| `Code/CPSock.cpp` | 469-511 | billing read/write |
| `Code/CPSock.cpp` | 513-591 | `AddMessage` |
| `Code/CPSock.cpp` | 593-615 | compaction helpers |
| `Code/CPSock.cpp` | 617-684 | `SendMessageA` |
| `Code/CPSock.cpp` | 686-693 | `SendOneMessage` |
| `Code/Basedef.h` | 44-46, 56-57, 75-77, 82, 107 | ports, `MAX_USER`, `ADMIN_RESERV`, equip/carry/cargo, `MAX_MOB` |
| `Code/Basedef.h` | 925-930, 943-946 | `_MSG` macro and `MSG_STANDARD` |
| `Code/Basedef.h` | 2428 | `BASE_CheckPacket` declaration |
| `Code/Basedef.cpp` | 6475-6596 | `BASE_CheckPacket` stub |
| `Code/TMSrv/Server.cpp` | 269, 281-282, 367 | `pUser`, clocks, `LocalIP` |
| `Code/TMSrv/Server.cpp` | 3496, 3520-3562, 3603, 3634 | startup wiring and listeners |
| `Code/TMSrv/Server.cpp` | 3731-3810 | billing inbound handler |
| `Code/TMSrv/Server.cpp` | 3812-3917 | DB inbound handler and reconnect |
| `Code/TMSrv/Server.cpp` | 3919-4045 | client read/accept handlers |
| `Code/TMSrv/Server.cpp` | 1377-1391 | `SendBilling` stub, `SendBilling2` |
| `Code/TMSrv/Server.cpp` | 4145-4148 | `WSACleanup` |
| `Code/TMSrv/Server.cpp` | 4300-4302 | empty `ProcessBILLMessage` |
| `Code/TMSrv/Server.cpp` | 6701-6727 | `Log` |
| `Code/TMSrv/Server.cpp` | 6729-6741 | `GetUserFromSocket` |
| `Code/TMSrv/CUser.cpp` | 51-79, 81-92 | accept and close |
| `Code/TMSrv/CUser.h` | 47 | `CPSock cSock` embedded |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | 201-221 | 1 Hz send flush |
| `Code/TMSrv/_MSG_Action.cpp` | 23, 272, 278, 287 | `sizeof(pointer)` send-size defect |
| `Code/TMSrv/ProcessClientMessage.cpp` | 38-66 | dispatcher contract |
| `Code/DBSrv/Server.cpp` | 520, 586-587, 599 | init, listeners, redirect client |
| `Code/DBSrv/Server.cpp` | 756-949 | admin-client, admin, game-server read handlers |
| `Code/DBSrv/Server.cpp` | 951-1191 | admin and game accept paths |
| `Code/DBSrv/Server.cpp` | 2115-2127 | `Log` |
| `Code/DBSrv/Server.cpp` | 2169-2190 | socket-to-slot lookups |
| `Code/DBSrv/CUser.cpp` | 39-65 | accept |
| `Code/TMSrv/TMSrv.vcxproj` | 61, 79-80, 92 | `_PACKET_DEBUG`, CRT, `CPSock.cpp` |
| `Code/DBSrv/DBSrv.vcxproj` | 61, 83, 95 | `_PACKET_DEBUG`, `CPSock.cpp` |
| `W2PP Code Project.sln` | project list | only `DBSrv` and `TMSrv` |

## Appendix B - Static measurements used in the report

| Measurement | Method | Value |
|---|---|---|
| Translation units referencing `CPSock` or `cSock.` | exhaustive content search over `Code/**/*.cpp,*.h` | 40 |
| Translation units including `CPSock.h` | same | 16 |
| `AddMessage` references | same | 67 across 17 files |
| `SendOneMessage` references | same | 129 across 26 files |
| `SendMessageA` references | same | 32 across 10 files |
| `Receive()` call sites | same | 8, in 2 server TUs |
| `ReadMessage` call sites | same | 5, in 2 server TUs |
| `RefreshRecvBuffer` call sites | same | 0 |
| `pKeyWord` references | same | 5, all in `CPSock.cpp` |
| `INITCODE` references | same | 6, all in `CPSock.cpp`/`CPSock.h` |
| `HEADER` references outside CPSock | same | 0 (the `WriteHeader` matches are unrelated file writers) |
| `srand` references | same | 0 |
| `setsockopt` / `ioctlsocket` / `TCP_NODELAY` references | same | 0 |
| Test-framework references | same | 0 |
| Solution projects | `.sln` parse | 3 (2 buildable) |
