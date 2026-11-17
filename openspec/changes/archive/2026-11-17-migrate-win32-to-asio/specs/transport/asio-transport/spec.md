# Spec Delta

## Purpose

Provides the TCP transport used by every link in the system — game clients, the game-to-DB link, NP admin, billing and redirect — while preserving the existing wire protocol and connection semantics.

## ADDED Requirements

### Requirement: Protocol-compatible stream framing

The transport SHALL exchange the existing binary stream format without modification: an optional 4-byte init handshake, followed by frames whose 12-byte header carries size, keyword index, checksum, type, id and client tick. Frames SHALL be limited to the existing 12–8192 byte window.

#### Scenario: Peer performs the init handshake

- **WHEN** a peer connects and sends the expected 4-byte init code before any frame
- **THEN** the transport accepts the stream and begins decoding frames

#### Scenario: Init code mismatch

- **WHEN** the first four bytes received on a connection do not equal the expected init code
- **THEN** the transport reports a protocol violation and closes that connection

#### Scenario: Frame outside the size window

- **WHEN** a decoded header declares a size below the header size or above the maximum frame size
- **THEN** the transport discards the stream state, reports the violation and closes the connection

#### Scenario: Byte-for-byte frame parity

- **WHEN** a message is encoded by the new transport and decoded by a reference encoder/decoder built from the legacy algorithm
- **THEN** the header fields, transformed payload and checksum are identical in both directions

### Requirement: Integrity verification gates dispatch

A frame whose checksum does not match its transformed payload SHALL be treated as corrupted: it SHALL NOT be delivered to message handlers, and the connection SHALL be closed.

#### Scenario: Checksum mismatch

- **WHEN** a received frame's computed checksum differs from the checksum in its header
- **THEN** no message handler receives the frame and the connection is closed with a logged reason

### Requirement: Session identity independent of OS handles

Each accepted connection SHALL be assigned a stable session slot for its lifetime. Routing, lookup and cleanup SHALL use that slot identity rather than a native socket handle, so a recycled OS handle cannot match a stale session.

#### Scenario: Slot is released and reused

- **WHEN** a connection closes and its slot is later assigned to a new connection
- **THEN** events addressed to the old connection cannot be delivered to the new session

#### Scenario: Unknown session event

- **WHEN** an event references a session that is no longer registered
- **THEN** the transport ignores it and records a diagnostic instead of closing an unrelated connection

### Requirement: Serialized, non-blocking event processing

All transport callbacks SHALL execute on the process event-loop thread, and no callback SHALL block that thread with sleeps, modal dialogs, or synchronous connect/read/write loops. Gameplay, timers and session state remain single-threaded.

#### Scenario: Concurrent client traffic

- **WHEN** many clients send and receive at the same time
- **THEN** their callbacks run one at a time in event-loop order and no partially applied message is observed by another callback

#### Scenario: Peer is slow to read

- **WHEN** a peer stops reading and the outbound queue for that session fills
- **THEN** the event loop continues serving other sessions and the affected session reports the backpressure condition

### Requirement: Ordered, bounded outbound delivery

Messages accepted for a session SHALL be written in submission order. Each session SHALL enforce the existing 128 KiB outbound limit; a message that would exceed it SHALL be rejected and logged, matching current drop behavior. Partial writes SHALL be completed before the next message is written.

#### Scenario: Message larger than remaining queue capacity

- **WHEN** a message would push the session's queued bytes beyond the limit
- **THEN** the message is not sent, a queue-full diagnostic is recorded, and previously queued messages are unaffected

#### Scenario: Partial write

- **WHEN** the operating system accepts only part of a queued buffer
- **THEN** the transport sends the remainder before starting the next message, preserving order and content

#### Scenario: Write failure

- **WHEN** a write fails because the connection is gone
- **THEN** the failure is surfaced to the session owner, which performs the same cleanup path as a peer disconnect

### Requirement: Link-specific connection behavior

The transport SHALL support passive listeners for game, DB and admin ports and outbound connections for the DB, billing and redirect links. Outbound game/DB/admin links SHALL perform the init handshake; billing SHALL use unframed 196-byte records with no handshake.

#### Scenario: Billing record exchange

- **WHEN** the billing link is active
- **THEN** outbound records are emitted as 196-byte units and inbound data is consumed in 196-byte units with no framing header

#### Scenario: Listener bind failure

- **WHEN** a configured listener cannot bind to its address and port
- **THEN** startup reports the failure, releases the listener and exits non-zero instead of waiting on a modal dialog

### Requirement: Non-blocking reconnect ladder

When the DB link is lost, the transport SHALL attempt the existing reconnect policy — up to two attempts spaced 200 ms apart — using timers rather than blocking sleeps. If reconnection fails, the process SHALL begin graceful shutdown. Billing loss SHALL schedule retry without blocking the event loop.

#### Scenario: DB link drops

- **WHEN** the DB connection closes unexpectedly
- **THEN** the server schedules two reconnect attempts 200 ms apart, continues serving existing work between attempts, and shuts down gracefully if both fail

#### Scenario: Billing peer disconnects

- **WHEN** the billing connection closes
- **THEN** the billing link is reset, a retry is scheduled, and client/DB traffic continues uninterrupted
