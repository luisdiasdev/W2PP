#include "Connection.h"

#include "FrameCodec.h"
#include "Protocol.h"

#include "../platform/Clock.h"
#include "../platform/Diagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace net
{

Connection::Connection(SessionPtr session)
	: session_(std::move(session))
{
}

void Connection::Attach(SessionPtr session)
{
	session_ = std::move(session);
}

void Connection::Detach()
{
	session_.reset();
}

bool Connection::SendOneMessage(char *msg, int size)
{
	if (!AddMessage(msg, size))
		return false;

	return SendMessageA();
}

bool Connection::AddMessage(char *msg, int size)
{
	char temp[256];

	if (msg == nullptr || size < static_cast<int>(sizeof(HEADER)) ||
	    size > MAX_MESSAGE_SIZE)
	{
		std::snprintf(temp, sizeof(temp), "err,add message size %d", size);
		platform::ReportWarning(temp, nullptr);
		return false;
	}

	if (!IsOpen())
	{
		std::snprintf(temp, sizeof(temp), "err,add buffer invalid %d", size);
		platform::ReportWarning(temp, nullptr);
		return false;
	}

	if (QueuedBytes() + static_cast<std::size_t>(size) >
	    static_cast<std::size_t>(SEND_BUFFER_SIZE))
	{
		HEADER *header = reinterpret_cast<HEADER *>(msg);
		std::snprintf(temp, sizeof(temp),
		              "err,add buffer full %d %d %d",
		              static_cast<int>(QueuedBytes()), size, header->Type);
		platform::ReportWarning(temp, nullptr);
		return false;
	}

	const unsigned char keywordIndex = static_cast<unsigned char>(rand() % 256);
	HEADER *header = reinterpret_cast<HEADER *>(msg);

	StampHeader(reinterpret_cast<unsigned char *>(msg),
	            static_cast<std::uint16_t>(size), keywordIndex,
	            platform::MonotonicMs());

	std::vector<unsigned char> frame(
		reinterpret_cast<unsigned char *>(msg),
		reinterpret_cast<unsigned char *>(msg) + size);

	frame[3] = EncodePayload(frame.data(), frame.size(), keywordIndex);
	header->CheckSum = static_cast<char>(frame[3]);

	if (!session_->Queue(frame.data(), frame.size()))
	{
		std::snprintf(temp, sizeof(temp), "err,add queue rejected %d", size);
		platform::ReportWarning(temp, nullptr);
		return false;
	}

	return true;
}

bool Connection::SendMessageA()
{
	if (!IsOpen())
		return false;

	session_->Flush();
	return true;
}

bool Connection::SendBillMessage(char *msg)
{
	if (msg == nullptr || !IsOpen())
		return false;

	if (!session_->Queue(msg, BILLING_RECORD_SIZE))
		return false;

	return SendMessageA();
}

bool Connection::CloseSocket()
{
	if (session_)
		session_->Close();

	return true;
}

bool Connection::IsOpen() const
{
	return session_ && session_->IsOpen();
}

std::uint64_t Connection::Generation() const
{
	return session_ ? session_->Generation() : 0;
}

std::size_t Connection::QueuedBytes() const
{
	return session_ ? session_->QueuedBytes() : 0;
}

} // namespace net
