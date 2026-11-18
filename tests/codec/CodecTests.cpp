#include <catch2/catch_test_macros.hpp>

#include "../../Code/net/FrameCodec.h"
#include "../../Code/net/Protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

constexpr std::uint16_t kTypeAccountLogin = 0x020D;

std::vector<std::uint8_t> MakePlainFrame(std::size_t size, std::int16_t type,
                                         std::int16_t id, std::uint8_t keywordIndex,
                                         std::uint32_t clientTick)
{
	std::vector<std::uint8_t> frame(size, 0);

	for (std::size_t i = 12; i < size; ++i)
		frame[i] = static_cast<std::uint8_t>((i * 37u + type) & 0xFFu);

	net::StampHeader(frame.data(), static_cast<std::uint16_t>(size), keywordIndex, clientTick);
	std::memcpy(frame.data() + 4, &type, sizeof(type));
	std::memcpy(frame.data() + 6, &id, sizeof(id));
	return frame;
}

std::vector<std::uint8_t> EncodeFrame(std::vector<std::uint8_t> frame,
                                      std::uint8_t keywordIndex)
{
	frame[3] = net::EncodePayload(frame.data(), frame.size(), keywordIndex);
	return frame;
}

std::vector<std::uint8_t> ReadFile(const std::string &path)
{
	std::ifstream stream(path, std::ios::binary);
	REQUIRE(stream.good());
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
	                                 std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("codec round trip restores every byte", "[codec]")
{
	const std::uint8_t keywords[] = { 0, 1, 35, 127, 255 };
	const std::size_t sizes[] = { 12, 13, 48, 256, 1024, 8192 };

	for (std::uint8_t keyword : keywords)
	{
		for (std::size_t size : sizes)
		{
			const std::vector<std::uint8_t> plain =
				MakePlainFrame(size, kTypeAccountLogin, 7, keyword, 0x11223344u);

			std::vector<std::uint8_t> encoded = EncodeFrame(plain, keyword);

			std::vector<std::uint8_t> expected = plain;
			expected[3] = encoded[3];

			REQUIRE(net::DecodePayload(encoded.data(), encoded.size()) ==
			        net::DecodeStatus::Frame);
			REQUIRE(encoded.size() == expected.size());
			REQUIRE(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);
		}
	}
}

TEST_CASE("codec matches the captured legacy golden frame", "[codec]")
{
	// Captured from the legacy CPSock encoder before cutover (keyword 35,
	// size 12, type 0x0100, id 1, client tick 17).
	const std::uint8_t golden[] =
	{
		0x0c, 0x00, 0x23, 0xab, 0xfa, 0xec,
		0xf5, 0xff, 0x0b, 0xea, 0xf4, 0xfb
	};

	const std::uint8_t keywordIndex = 35;
	std::vector<std::uint8_t> frame =
		MakePlainFrame(sizeof(golden), 0x0100, 1, keywordIndex, 17);

	frame = EncodeFrame(frame, keywordIndex);

	REQUIRE(frame.size() == sizeof(golden));
	REQUIRE(std::memcmp(frame.data(), golden, sizeof(golden)) == 0);

	const net::HeaderFields fields = net::ParseHeader(golden);
	REQUIRE(fields.size == 12);
	REQUIRE(fields.keywordIndex == keywordIndex);
	REQUIRE(fields.checksum == 0xAB);

	std::vector<std::uint8_t> decoded(golden, golden + sizeof(golden));
	REQUIRE(net::DecodePayload(decoded.data(), decoded.size()) ==
	        net::DecodeStatus::Frame);

	const net::HeaderFields decodedFields = net::ParseHeader(decoded.data());
	REQUIRE(decodedFields.type == 0x0100);
	REQUIRE(decodedFields.id == 1);
	REQUIRE(decodedFields.clientTick == 17u);
}

TEST_CASE("codec rejects a corrupted checksum", "[codec]")
{
	const std::uint8_t keywordIndex = 9;
	std::vector<std::uint8_t> frame =
		EncodeFrame(MakePlainFrame(64, kTypeAccountLogin, 1, keywordIndex, 5), keywordIndex);

	// Corrupt the checksum byte. (The legacy checksum is a sum of transform
	// offsets, so payload corruption alone is not detectable by design.)
	frame[3] ^= 0x01;

	REQUIRE(net::DecodePayload(frame.data(), frame.size()) ==
	        net::DecodeStatus::BadChecksum);
}

TEST_CASE("codec rejects a mismatched init code", "[codec]")
{
	std::vector<std::uint8_t> buffer(4 + 12, 0);
	const std::uint32_t wrong = 0xDEADBEEFu;
	std::memcpy(buffer.data(), &wrong, sizeof(wrong));

	bool initSeen = false;
	const net::DecodeOutcome outcome =
		net::DecodeNextFrame(buffer.data(), buffer.size(), true, initSeen);

	REQUIRE(outcome.status == net::DecodeStatus::InitMismatch);
	REQUIRE_FALSE(initSeen);
	REQUIRE(outcome.consumed == 0);
	REQUIRE(outcome.frame == nullptr);
}

TEST_CASE("codec enforces the frame size window", "[codec]")
{
	bool initSeen = false;

	std::vector<std::uint8_t> tooSmall(12, 0);
	const std::uint16_t low = 11;
	std::memcpy(tooSmall.data(), &low, sizeof(low));

	net::DecodeOutcome outcome =
		net::DecodeNextFrame(tooSmall.data(), tooSmall.size(), false, initSeen);
	REQUIRE(outcome.status == net::DecodeStatus::BadSize);
	REQUIRE(outcome.size == 11);
	REQUIRE(outcome.consumed == 0);

	std::vector<std::uint8_t> tooLarge(12, 0);
	const std::uint16_t high = 8193;
	std::memcpy(tooLarge.data(), &high, sizeof(high));

	outcome = net::DecodeNextFrame(tooLarge.data(), tooLarge.size(), false, initSeen);
	REQUIRE(outcome.status == net::DecodeStatus::BadSize);
	REQUIRE(outcome.size == 8193);

	std::vector<std::uint8_t> minFrame =
		EncodeFrame(MakePlainFrame(12, kTypeAccountLogin, 1, 4, 1), 4);
	outcome = net::DecodeNextFrame(minFrame.data(), minFrame.size(), false, initSeen);
	REQUIRE(outcome.status == net::DecodeStatus::Frame);
	REQUIRE(outcome.size == 12);
	REQUIRE(outcome.consumed == 12);

	std::vector<std::uint8_t> maxFrame =
		EncodeFrame(MakePlainFrame(8192, kTypeAccountLogin, 2, 5, 2), 5);
	outcome = net::DecodeNextFrame(maxFrame.data(), maxFrame.size(), false, initSeen);
	REQUIRE(outcome.status == net::DecodeStatus::Frame);
	REQUIRE(outcome.size == 8192);
	REQUIRE(outcome.consumed == 8192);
}

TEST_CASE("codec waits for partial init and partial frames", "[codec]")
{
	const std::uint8_t keywordIndex = 21;
	std::vector<std::uint8_t> encoded =
		EncodeFrame(MakePlainFrame(64, kTypeAccountLogin, 3, keywordIndex, 9), keywordIndex);

	std::vector<std::uint8_t> buffer;
	std::uint32_t initCode = INITCODE;
	buffer.insert(buffer.end(),
	              reinterpret_cast<std::uint8_t *>(&initCode),
	              reinterpret_cast<std::uint8_t *>(&initCode) + sizeof(initCode));
	buffer.insert(buffer.end(), encoded.begin(), encoded.begin() + 8);

	bool initSeen = false;
	net::DecodeOutcome outcome =
		net::DecodeNextFrame(buffer.data(), buffer.size(), true, initSeen);
	REQUIRE(outcome.status == net::DecodeStatus::NeedMoreData);
	REQUIRE(initSeen);
	REQUIRE(outcome.consumed == 4);

	buffer.insert(buffer.end(), encoded.begin() + 8, encoded.end());

	outcome = net::DecodeNextFrame(buffer.data() + 4, buffer.size() - 4, true, initSeen);
	REQUIRE(outcome.status == net::DecodeStatus::Frame);
	REQUIRE(outcome.size == encoded.size());
	REQUIRE(outcome.consumed == encoded.size());
	REQUIRE(outcome.frame == buffer.data() + 4);
}

TEST_CASE("codec extracts multiple frames from one buffer", "[codec]")
{
	std::vector<std::uint8_t> first =
		EncodeFrame(MakePlainFrame(32, kTypeAccountLogin, 1, 10, 10), 10);
	std::vector<std::uint8_t> second =
		EncodeFrame(MakePlainFrame(128, 0x010A, 2, 200, 11), 200);

	std::vector<std::uint8_t> buffer = first;
	buffer.insert(buffer.end(), second.begin(), second.end());

	bool initSeen = false;
	net::DecodeOutcome outcome =
		net::DecodeNextFrame(buffer.data(), buffer.size(), false, initSeen);

	REQUIRE(outcome.status == net::DecodeStatus::Frame);
	REQUIRE(outcome.size == first.size());
	REQUIRE(outcome.consumed == first.size());

	outcome = net::DecodeNextFrame(buffer.data() + outcome.consumed,
	                               buffer.size() - first.size(), false, initSeen);

	REQUIRE(outcome.status == net::DecodeStatus::Frame);
	REQUIRE(outcome.size == second.size());
	REQUIRE(outcome.consumed == second.size());
	REQUIRE(net::ParseHeader(outcome.frame).id == 2);
}

TEST_CASE("codec replays the captured legacy stream", "[codec]")
{
	const std::string fixture = std::string(W2PP_SOURCE_DIR) +
		"/tests/fixtures/legacy_stream.bin";
	std::vector<std::uint8_t> stream = ReadFile(fixture);

	const std::size_t expectedSizes[] = { 12, 13, 48, 256, 1024, 8192 };
	const std::uint8_t expectedKeywords[] = { 35, 7, 101, 96, 160, 251 };
	const std::int16_t expectedTypes[] = { 0x0100, 0x020D, 0x010A, 0x0404, 0x0800, 0x4001 };

	bool initSeen = false;
	std::size_t offset = 0;

	for (std::size_t step = 0; step < sizeof(expectedSizes) / sizeof(expectedSizes[0]); ++step)
	{
		const net::DecodeOutcome outcome =
			net::DecodeNextFrame(stream.data() + offset,
			                     stream.size() - offset, false, initSeen);

		REQUIRE(outcome.status == net::DecodeStatus::Frame);
		REQUIRE(outcome.size == expectedSizes[step]);
		REQUIRE(outcome.consumed == expectedSizes[step]);
		REQUIRE(outcome.frame[2] == expectedKeywords[step]);

		const net::HeaderFields fields = net::ParseHeader(outcome.frame);
		REQUIRE(fields.size == expectedSizes[step]);
		REQUIRE(fields.keywordIndex == expectedKeywords[step]);
		REQUIRE(fields.type == expectedTypes[step]);

		// The captured payload must match the plaintext scripted into the
		// legacy encoder byte-for-byte.
		for (std::size_t i = 12; i < outcome.size; ++i)
		{
			const std::uint8_t expected =
				static_cast<std::uint8_t>((i * 37u + step * 11u) & 0xFFu);
			REQUIRE(outcome.frame[i] == expected);
		}

		offset += outcome.consumed;
	}

	REQUIRE(offset == stream.size());
}
