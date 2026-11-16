#include "FrameCodec.h"

#include "Protocol.h"

#include <cstring>

namespace net
{
namespace
{

std::uint16_t ReadU16(const std::uint8_t *p)
{
	std::uint16_t value;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

std::int16_t ReadI16(const std::uint8_t *p)
{
	std::int16_t value;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

std::uint32_t ReadU32(const std::uint8_t *p)
{
	std::uint32_t value;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

void WriteU16(std::uint8_t *p, std::uint16_t value)
{
	std::memcpy(p, &value, sizeof(value));
}

void WriteU32(std::uint8_t *p, std::uint32_t value)
{
	std::memcpy(p, &value, sizeof(value));
}

std::uint8_t TransformValue(std::uint8_t pos)
{
	return pKeyWord[static_cast<std::size_t>(pos) * 2 + 1];
}

} // namespace

HeaderFields ParseHeader(const std::uint8_t *frame)
{
	HeaderFields fields;
	fields.size         = ReadU16(frame);
	fields.keywordIndex = frame[2];
	fields.checksum     = frame[3];
	fields.type         = ReadI16(frame + 4);
	fields.id           = ReadI16(frame + 6);
	fields.clientTick   = ReadU32(frame + 8);
	return fields;
}

void StampHeader(std::uint8_t *frame, std::uint16_t size,
                 std::uint8_t keywordIndex, std::uint32_t clientTick)
{
	WriteU16(frame, size);
	frame[2] = static_cast<char>(keywordIndex);
	frame[3] = 0;
	WriteU32(frame + 8, clientTick);
}

std::uint8_t KeywordByte(std::uint8_t keywordIndex)
{
	return pKeyWord[static_cast<std::size_t>(keywordIndex) * 2];
}

std::uint8_t EncodePayload(std::uint8_t *frame, std::size_t size,
                           std::uint8_t keywordIndex)
{
	std::uint8_t sumPlain = 0;
	std::uint8_t sumCoded = 0;
	int pos = KeywordByte(keywordIndex);

	for (std::size_t i = 4; i < size; ++i, ++pos)
	{
		const std::uint8_t plain = frame[i];
		const std::uint8_t trans = TransformValue(static_cast<std::uint8_t>(pos % 256));
		std::uint8_t coded;

		switch (i & 0x3)
		{
		case 0:
			coded = static_cast<std::uint8_t>(plain + (trans << 1));
			break;
		case 1:
			coded = static_cast<std::uint8_t>(plain - (trans >> 3));
			break;
		case 2:
			coded = static_cast<std::uint8_t>(plain + (trans << 2));
			break;
		default:
			coded = static_cast<std::uint8_t>(plain - (trans >> 5));
			break;
		}

		sumPlain = static_cast<std::uint8_t>(sumPlain + plain);
		sumCoded = static_cast<std::uint8_t>(sumCoded + coded);
		frame[i] = coded;
	}

	return static_cast<std::uint8_t>(sumCoded - sumPlain);
}

DecodeStatus DecodePayload(std::uint8_t *frame, std::size_t size)
{
	if (size < sizeof(HEADER) || size > static_cast<std::size_t>(MAX_MESSAGE_SIZE))
		return DecodeStatus::BadSize;

	const HeaderFields fields = ParseHeader(frame);

	std::uint8_t sumCoded = 0;
	std::uint8_t sumDecoded = 0;
	int pos = KeywordByte(fields.keywordIndex);

	for (std::size_t i = 4; i < size; ++i, ++pos)
	{
		const std::uint8_t coded = frame[i];
		const std::uint8_t trans = TransformValue(static_cast<std::uint8_t>(pos % 256));
		std::uint8_t plain;

		switch (i & 0x3)
		{
		case 0:
			plain = static_cast<std::uint8_t>(coded - (trans << 1));
			break;
		case 1:
			plain = static_cast<std::uint8_t>(coded + (trans >> 3));
			break;
		case 2:
			plain = static_cast<std::uint8_t>(coded - (trans << 2));
			break;
		default:
			plain = static_cast<std::uint8_t>(coded + (trans >> 5));
			break;
		}

		sumCoded = static_cast<std::uint8_t>(sumCoded + coded);
		sumDecoded = static_cast<std::uint8_t>(sumDecoded + plain);
		frame[i] = plain;
	}

	const std::uint8_t checksum = static_cast<std::uint8_t>(sumCoded - sumDecoded);
	if (checksum != fields.checksum)
		return DecodeStatus::BadChecksum;

	return DecodeStatus::Frame;
}

DecodeOutcome DecodeNextFrame(std::uint8_t *buffer, std::size_t available,
                              bool initRequired, bool &initSeen)
{
	DecodeOutcome outcome{};
	outcome.status   = DecodeStatus::NeedMoreData;
	outcome.frame    = nullptr;
	outcome.size     = 0;
	outcome.consumed = 0;

	std::size_t offset = 0;

	if (initRequired && !initSeen)
	{
		if (available < sizeof(std::uint32_t))
			return outcome;

		if (ReadU32(buffer) != static_cast<std::uint32_t>(INITCODE))
		{
			outcome.status = DecodeStatus::InitMismatch;
			return outcome;
		}

		initSeen = true;
		offset = sizeof(std::uint32_t);
		outcome.consumed = offset;
	}

	if (available - offset < sizeof(HEADER))
		return outcome;

	const std::uint16_t size = ReadU16(buffer + offset);

	if (size < static_cast<std::uint16_t>(sizeof(HEADER)) ||
	    size > static_cast<std::uint16_t>(MAX_MESSAGE_SIZE))
	{
		outcome.status   = DecodeStatus::BadSize;
		outcome.size     = size;
		outcome.consumed = 0;
		return outcome;
	}

	if (available - offset < size)
		return outcome;

	std::uint8_t *frame = buffer + offset;
	const DecodeStatus status = DecodePayload(frame, size);

	outcome.status   = status;
	outcome.frame    = frame;
	outcome.size     = size;
	outcome.consumed = offset + size;
	return outcome;
}

} // namespace net
