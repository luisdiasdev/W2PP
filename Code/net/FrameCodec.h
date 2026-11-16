/*
 * Pure frame codec for the legacy W2 wire format.
 *
 * The codec owns no sockets, no clocks and no mutable globals: callers pass
 * the frame buffer, the frame size and (when encoding) the keyword index.
 * This keeps it fully unit-testable and independent of the host.
 */

#ifndef _NET_FRAMECODEC_H_
#define _NET_FRAMECODEC_H_

#include <cstddef>
#include <cstdint>

namespace net
{

enum class DecodeStatus
{
	Frame,          // complete frame decoded and checksum verified
	NeedMoreData,   // incomplete init handshake or incomplete frame
	InitMismatch,   // first four bytes are not the expected init code
	BadSize,        // declared frame size is outside the supported window
	BadChecksum     // payload checksum does not match
};

struct HeaderFields
{
	std::uint16_t size;
	std::uint8_t  keywordIndex;
	std::uint8_t  checksum;
	std::int16_t  type;
	std::int16_t  id;
	std::uint32_t clientTick;
};

struct DecodeOutcome
{
	DecodeStatus  status;
	std::uint8_t *frame;     // frame start, valid for Frame and BadChecksum
	std::size_t   size;      // frame size in bytes
	std::size_t   consumed;  // bytes consumed from the input, init code included
};

// Reads the 12-byte header fields. Requires at least sizeof(HEADER) bytes.
HeaderFields ParseHeader(const std::uint8_t *frame);

// Writes Size, KeyWord, CheckSum (0) and ClientTick; Type and ID are left as
// the caller set them. The legacy transport did exactly this before encoding.
void StampHeader(std::uint8_t *frame, std::uint16_t size,
                 std::uint8_t keywordIndex, std::uint32_t clientTick);

// Applies the outgoing byte transform to frame[4 .. size-1] in place and
// returns the checksum byte that belongs in frame[3].
std::uint8_t EncodePayload(std::uint8_t *frame, std::size_t size,
                           std::uint8_t keywordIndex);

// Applies the incoming transform to a complete frame in place and verifies
// the checksum stored in the header. The transform is applied even when the
// checksum turns out to be wrong, matching the legacy decoder.
DecodeStatus DecodePayload(std::uint8_t *frame, std::size_t size);

// Locates and decodes the next frame in |buffer|. When |initRequired| is
// true, the four-byte init handshake must precede the first frame;
// |initSeen| carries that state across calls.
DecodeOutcome DecodeNextFrame(std::uint8_t *buffer, std::size_t available,
                              bool initRequired, bool &initSeen);

// Returns the transform base byte for a keyword index (the even table entry).
std::uint8_t KeywordByte(std::uint8_t keywordIndex);

} // namespace net

#endif
