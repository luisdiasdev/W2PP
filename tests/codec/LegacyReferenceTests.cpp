/*
 * Independent legacy reference for the wire transform and checksum.
 *
 * This file deliberately does not use net::FrameCodec for the algorithm. It
 * reproduces the loops from the original CPSock encoder/decoder so the codec
 * can be compared against them byte-for-byte.
 */

#include <catch2/catch_test_macros.hpp>

#include "../../Code/net/FrameCodec.h"
#include "../../Code/net/Protocol.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

std::uint8_t LegacyEncode(const std::uint8_t *plain, std::uint8_t *encoded,
                          std::size_t size, std::uint8_t keywordIndex)
{
	std::uint8_t sumPlain = 0;
	std::uint8_t sumCoded = 0;
	int pos = pKeyWord[keywordIndex * 2];

	for (std::size_t i = 4; i < size; ++i, ++pos)
	{
		const std::uint8_t trans = pKeyWord[(pos % 256) * 2 + 1];
		std::uint8_t coded = 0;

		switch (i & 0x3)
		{
		case 0: coded = static_cast<std::uint8_t>(plain[i] + (trans << 1)); break;
		case 1: coded = static_cast<std::uint8_t>(plain[i] - (trans >> 3)); break;
		case 2: coded = static_cast<std::uint8_t>(plain[i] + (trans << 2)); break;
		case 3: coded = static_cast<std::uint8_t>(plain[i] - (trans >> 5)); break;
		}

		sumPlain = static_cast<std::uint8_t>(sumPlain + plain[i]);
		sumCoded = static_cast<std::uint8_t>(sumCoded + coded);
		encoded[i] = coded;
	}

	return static_cast<std::uint8_t>(sumCoded - sumPlain);
}

std::uint8_t LegacyDecode(std::uint8_t *frame, std::size_t size)
{
	std::uint8_t sumCoded = 0;
	std::uint8_t sumPlain = 0;
	int pos = pKeyWord[frame[2] * 2];

	for (std::size_t i = 4; i < size; ++i, ++pos)
	{
		const std::uint8_t trans = pKeyWord[(pos % 256) * 2 + 1];
		const std::uint8_t coded = frame[i];

		switch (i & 0x3)
		{
		case 0: frame[i] = static_cast<std::uint8_t>(coded - (trans << 1)); break;
		case 1: frame[i] = static_cast<std::uint8_t>(coded + (trans >> 3)); break;
		case 2: frame[i] = static_cast<std::uint8_t>(coded - (trans << 2)); break;
		case 3: frame[i] = static_cast<std::uint8_t>(coded + (trans >> 5)); break;
		}

		sumCoded = static_cast<std::uint8_t>(sumCoded + coded);
		sumPlain = static_cast<std::uint8_t>(sumPlain + frame[i]);
	}

	return static_cast<std::uint8_t>(sumCoded - sumPlain);
}

std::vector<std::uint8_t> MakePlainFrame(std::size_t size, std::uint8_t keywordIndex)
{
	std::vector<std::uint8_t> frame(size, 0);
	net::StampHeader(frame.data(), static_cast<std::uint16_t>(size), keywordIndex, 0xCAFEBABEu);

	for (std::size_t i = 12; i < size; ++i)
		frame[i] = static_cast<std::uint8_t>((i * 53u + size) & 0xFFu);

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

TEST_CASE("codec matches the legacy reference across the size window", "[codec]")
{
	std::vector<std::size_t> sizes;
	for (std::size_t size = 12; size <= 64; ++size)
		sizes.push_back(size);
	for (std::size_t size = 65; size <= 8192; size += 97)
		sizes.push_back(size);
	sizes.push_back(8192);

	const std::uint8_t keywords[] = { 0, 35, 255 };

	for (std::uint8_t keywordIndex : keywords)
	{
		for (std::size_t size : sizes)
		{
			const std::vector<std::uint8_t> plain = MakePlainFrame(size, keywordIndex);

			std::vector<std::uint8_t> legacy = plain;
			legacy[3] = LegacyEncode(plain.data(), legacy.data(), size, keywordIndex);

			std::vector<std::uint8_t> codec = plain;
			codec[3] = net::EncodePayload(codec.data(), size, keywordIndex);

			REQUIRE(codec.size() == legacy.size());
			REQUIRE(std::memcmp(codec.data(), legacy.data(), size) == 0);

			std::vector<std::uint8_t> legacyDecoded = codec;
			REQUIRE(LegacyDecode(legacyDecoded.data(), size) == codec[3]);

			std::vector<std::uint8_t> codecDecoded = codec;
			REQUIRE(net::DecodePayload(codecDecoded.data(), size) ==
			        net::DecodeStatus::Frame);

			REQUIRE(std::memcmp(codecDecoded.data(), legacyDecoded.data(), size) == 0);
			REQUIRE(std::memcmp(codecDecoded.data(), plain.data(), 3) == 0);
			REQUIRE(std::memcmp(codecDecoded.data() + 4, plain.data() + 4, size - 4) == 0);
		}
	}
}

TEST_CASE("codec captured fixture matches the legacy decoder", "[codec]")
{
	const std::string fixture = std::string(W2PP_SOURCE_DIR) +
		"/tests/fixtures/legacy_stream.bin";
	std::vector<std::uint8_t> stream = ReadFile(fixture);

	std::size_t offset = 0;

	while (offset < stream.size())
	{
		const std::uint16_t size =
			static_cast<std::uint16_t>(stream[offset] | (stream[offset + 1] << 8));
		REQUIRE(size >= 12);
		REQUIRE(offset + size <= stream.size());

		std::vector<std::uint8_t> codec(stream.begin() + offset,
		                                stream.begin() + offset + size);
		std::vector<std::uint8_t> legacy(codec);

		const std::uint8_t legacyChecksum = LegacyDecode(legacy.data(), legacy.size());
		REQUIRE(legacyChecksum == codec[3]);

		REQUIRE(net::DecodePayload(codec.data(), codec.size()) ==
		        net::DecodeStatus::Frame);

		REQUIRE(std::memcmp(codec.data(), legacy.data(), codec.size()) == 0);

		offset += size;
	}

	REQUIRE(offset == stream.size());
}
