/*
 * Captures a scripted sequence of frames through the legacy CPSock encoder.
 *
 * This runs before the legacy transport is cut over and writes:
 *   legacy_stream.bin  - the exact bytes the legacy encoder produced
 *   legacy_keywords.txt - the keyword index used for each frame
 *
 * The captured stream is replayed by the codec/transport tests.
 */

#include "../../Code/CPSock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The legacy transport references these host globals. The capture tool does
// not run a server, so it provides inert definitions.
HWND         hWndMain = nullptr;
unsigned int CurrentTime = 0;
unsigned int LastSendTime = 0;

void Log(char *, char *, unsigned int) {}

struct MSG_STANDARD;
int BASE_CheckPacket(MSG_STANDARD *) { return 0; }

namespace
{

struct ScriptStep
{
	int   size;
	short type;
	short id;
};

bool WriteFile(const std::string &path, const void *data, std::size_t size)
{
	FILE *fp = std::fopen(path.c_str(), "wb");
	if (fp == nullptr)
		return false;

	const bool ok = std::fwrite(data, 1, size, fp) == size;
	std::fclose(fp);
	return ok;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
		return 2;
	}

	const std::string outDir = argv[1];

	// A scripted sequence covering the size window and several message types.
	const ScriptStep script[] =
	{
		{   12, static_cast<short>(0x0100), 1 },
		{   13, static_cast<short>(0x020D), 2 },
		{   48, static_cast<short>(0x010A), 3 },
		{  256, static_cast<short>(0x0404), 4 },
		{ 1024, static_cast<short>(0x0800), 5 },
		{ 8192, static_cast<short>(0x4001), 6 },
	};

	CPSock sock;
	sock.Sock = 1; // AddMessage only requires a non-zero handle.

	std::srand(0x5EED);

	std::vector<unsigned char> keywords;
	keywords.reserve(sizeof(script) / sizeof(script[0]));

	for (std::size_t step = 0; step < sizeof(script) / sizeof(script[0]); ++step)
	{
		const ScriptStep &item = script[step];
		std::vector<unsigned char> frame(static_cast<std::size_t>(item.size), 0);

		for (std::size_t i = 12; i < frame.size(); ++i)
			frame[i] = static_cast<unsigned char>((i * 37u + step * 11u) & 0xFFu);

		HEADER *header = reinterpret_cast<HEADER *>(frame.data());
		header->Type = item.type;
		header->ID = item.id;
		header->ClientTick = 0;

		CurrentTime += 17;

		if (!sock.AddMessage(reinterpret_cast<char *>(frame.data()), item.size))
		{
			std::fprintf(stderr, "legacy encoder rejected frame %zu\n", step);
			return 1;
		}

		keywords.push_back(frame[2]);
	}

	const std::size_t streamSize = static_cast<std::size_t>(sock.nSendPosition);
	const std::string streamPath = outDir + "/legacy_stream.bin";
	if (!WriteFile(streamPath, sock.pSendBuffer, streamSize))
	{
		std::fprintf(stderr, "cannot write %s\n", streamPath.c_str());
		return 1;
	}

	std::string keywordText;
	for (std::size_t i = 0; i < keywords.size(); ++i)
	{
		char line[16];
		std::snprintf(line, sizeof(line), "%u\n", static_cast<unsigned>(keywords[i]));
		keywordText += line;
	}

	const std::string keywordPath = outDir + "/legacy_keywords.txt";
	if (!WriteFile(keywordPath, keywordText.data(), keywordText.size()))
	{
		std::fprintf(stderr, "cannot write %s\n", keywordPath.c_str());
		return 1;
	}

	std::printf("captured %zu bytes, %zu frames\n", streamSize, keywords.size());
	return 0;
}
