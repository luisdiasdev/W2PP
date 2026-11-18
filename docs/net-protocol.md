# W2 wire protocol

This document describes the binary stream exchanged by the game client, TMSrv,
DBSrv, the NP admin tool, the billing link and the redirect link. The asio
transport must preserve this format byte-for-byte.

The implementation lives in `Code/net/Protocol.h` (constants and keyword
table) and `Code/net/FrameCodec.{h,cpp}` (handshake gate, framing, transform,
checksum). The parity tests are in `tests/codec/`.

## Transport

- Stream protocol over TCP. There are no packet boundaries below the frame
  layer.
- Ports: game `8281`, DB `7514`, NP admin `8895`. Billing and redirect ports
  are read from configuration files.
- Receive and send buffers are 128 KiB (`RECV_BUFFER_SIZE`,
  `SEND_BUFFER_SIZE`).
- A frame is between 12 and 8192 bytes (`MAX_MESSAGE_SIZE`).

## Init handshake

An accepted game, DB or admin connection starts with a four-byte handshake:

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | `INITCODE` = `0x1F11F311`, little-endian bytes `11 F3 11 1F` |

The handshake is sent by the connecting side (`ConnectServer`) and checked by
the accepting side before the first frame. A wrong value is a protocol
violation and closes the connection. The billing link uses unframed records
and sends no handshake.

## Frame header

Every frame starts with this 12-byte header:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `Size` | Total frame size including the header, little-endian. |
| 2 | 1 | `KeyWord` | Keyword-table index used by the transform. |
| 3 | 1 | `CheckSum` | Additive checksum, see below. |
| 4 | 2 | `Type` | Message type, transformed. |
| 6 | 2 | `ID` | Message id, transformed. |
| 8 | 4 | `ClientTick` | Sender tick, transformed. |

Only bytes 0-3 are sent in clear. Bytes 4 through `Size - 1` (the header tail
and the payload) are obfuscated by the transform.

The decoder resets buffered stream state and treats the connection as broken
when `Size` is below 12 or above 8192.

## Keyword table

`pKeyWord` is a 512-byte table of 256 pairs. For keyword index `k`:

- `base = pKeyWord[2 * k]`
- for frame byte `i` (starting at 4), `pos = base + (i - 4)` and
  `trans = pKeyWord[2 * (pos mod 256) + 1]`

The sender chooses the keyword index (the legacy transport used `rand() % 256`);
the value is stored in the header so the receiver can reverse the transform.
The codec accepts the keyword as an explicit parameter so tests are
independent of the random generator.

## Transform

Encoding, with all arithmetic modulo 256:

| `i mod 4` | Encoded byte |
|---|---|
| 0 | `plain + (trans << 1)` |
| 1 | `plain - (trans >> 3)` |
| 2 | `plain + (trans << 2)` |
| 3 | `plain - (trans >> 5)` |

Decoding reverses each operation: subtract for the two additions above and add
for the two subtractions.

## Checksum

With `sumPlain` the sum of the plaintext bytes 4..`Size - 1`, `sumCoded` the sum
of the encoded bytes over the same range, and all arithmetic modulo 256:

```
CheckSum = sumCoded - sumPlain
```

The receiving side recomputes the same expression after transforming the frame
and compares it with the header byte. A mismatch means the frame is rejected.

**Invariant:** because each encoded byte is the plaintext byte plus a
per-position transform offset, `sumCoded - sumPlain` equals the sum of those
offsets. It therefore depends on the keyword index and frame size, not on the
payload bytes. This is a property of the legacy protocol and is preserved
deliberately; the checksum does not detect arbitrary payload corruption.

## Billing records

The billing link has no handshake and no frame header. Each record is exactly
196 bytes (`_AUTH_GAME` / `g_cGame`). Outbound records are written as 196-byte
units and inbound data is consumed in 196-byte units.

## Outbound queue

Messages are written in submission order. A session accepts at most 128 KiB of
queued encoded frames; a message that would exceed the limit is rejected and
logged, and previously queued messages are unaffected.

## Example frames

Both examples are produced by `tests/codec/CodecTests.cpp` (the golden frame
test) and replayed from `tests/fixtures/legacy_stream.bin` (the fixture test).

### Header-only frame, keyword 35

| | Bytes |
|---|---|
| Plaintext | `0c 00 23 00 00 01 01 00 11 00 00 00` |
| Encoded | `0c 00 23 ab fa ec f5 ff 0b ea f4 fb` |

`Size = 12`, `Type = 0x0100`, `ID = 1`, `ClientTick = 17`, `CheckSum = 0xAB`.

### 13-byte frame, keyword 7

| | Bytes |
|---|---|
| Plaintext | `0d 00 07 00 0d 02 02 00 22 00 00 00 c7` |
| Encoded | `0d 00 07 b8 09 f3 e6 f9 28 ff ec fd c7` |

`Size = 13`, `Type = 0x020D`, `ID = 2`, `ClientTick = 34`, `CheckSum = 0xB8`,
and one payload byte `0xC7` at offset 12.
