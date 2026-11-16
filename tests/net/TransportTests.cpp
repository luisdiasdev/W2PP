#include <catch2/catch_test_macros.hpp>

#include "../../Code/net/Acceptor.h"
#include "../../Code/net/Connection.h"
#include "../../Code/net/Connector.h"
#include "../../Code/net/FrameCodec.h"
#include "../../Code/net/LocalAddress.h"
#include "../../Code/net/Protocol.h"
#include "../../Code/net/Session.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

using asio::ip::tcp;

std::vector<std::uint8_t> MakeEncodedFrame(std::int16_t type, std::int16_t id,
                                           std::size_t size,
                                           std::uint8_t keywordIndex,
                                           std::uint32_t tick)
{
	std::vector<std::uint8_t> frame(size, 0);

	for (std::size_t i = 12; i < size; ++i)
		frame[i] = static_cast<std::uint8_t>((i * 13u + type) & 0xFFu);

	net::StampHeader(frame.data(), static_cast<std::uint16_t>(size), keywordIndex, tick);
	std::memcpy(frame.data() + 4, &type, sizeof(type));
	std::memcpy(frame.data() + 6, &id, sizeof(id));
	frame[3] = net::EncodePayload(frame.data(), frame.size(), keywordIndex);
	return frame;
}

bool RunUntil(asio::io_context &context, const std::function<bool()> &predicate,
              int timeoutMs = 2000)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(timeoutMs);

	while (!predicate() && std::chrono::steady_clock::now() < deadline)
		context.run_for(std::chrono::milliseconds(5));

	return predicate();
}

struct LoopbackPair
{
	asio::io_context context;
	tcp::socket client{ context };
	tcp::socket server{ context };

	LoopbackPair()
	{
		tcp::acceptor acceptor(context);
		asio::error_code ec;

		acceptor.open(tcp::v4(), ec);
		REQUIRE_FALSE(ec);
		acceptor.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
		REQUIRE_FALSE(ec);
		acceptor.listen(1, ec);
		REQUIRE_FALSE(ec);

		client.connect(acceptor.local_endpoint(), ec);
		REQUIRE_FALSE(ec);
		server = acceptor.accept(ec);
		REQUIRE_FALSE(ec);
	}
};

} // namespace

TEST_CASE("transport session round trip preserves frames", "[transport]")
{
	LoopbackPair pair;

	auto client = std::make_shared<net::Session>(
		pair.context, std::move(pair.client), net::SessionMode::Framed, false);
	auto server = std::make_shared<net::Session>(
		pair.context, std::move(pair.server), net::SessionMode::Framed, false);

	std::vector<std::uint8_t> received;
	std::size_t receivedSize = 0;

	server->SetFrameHandler(
		[&](std::uint8_t *frame, std::size_t size)
		{
			received.assign(frame, frame + size);
			receivedSize = size;
		});

	client->Start();
	server->Start();

	const std::vector<std::uint8_t> encoded =
		MakeEncodedFrame(0x020D, 7, 128, 33, 0x11223344u);

	REQUIRE(client->QueueAndFlush(encoded.data(), encoded.size()));
	REQUIRE(RunUntil(pair.context, [&]() { return receivedSize != 0; }));

	REQUIRE(receivedSize == encoded.size());

	const net::HeaderFields fields = net::ParseHeader(received.data());
	REQUIRE(fields.type == 0x020D);
	REQUIRE(fields.id == 7);
	REQUIRE(fields.clientTick == 0x11223344u);

	for (std::size_t i = 12; i < received.size(); ++i)
		REQUIRE(received[i] == static_cast<std::uint8_t>((i * 13u + 0x020D) & 0xFFu));

	client->ForceClose();
	server->ForceClose();
	pair.context.restart();
	pair.context.poll();
}

TEST_CASE("transport session keeps submission order and caps the queue", "[transport]")
{
	LoopbackPair pair;

	auto client = std::make_shared<net::Session>(
		pair.context, std::move(pair.client), net::SessionMode::Framed, false);
	auto server = std::make_shared<net::Session>(
		pair.context, std::move(pair.server), net::SessionMode::Framed, false);

	std::vector<std::int16_t> ids;
	std::size_t receivedSize = 0;

	server->SetFrameHandler(
		[&](std::uint8_t *frame, std::size_t size)
		{
			ids.push_back(net::ParseHeader(frame).id);
			receivedSize += size;
		});

	client->Start();
	server->Start();

	const std::size_t frameSize = 8192;
	std::size_t queued = 0;

	for (std::int16_t id = 0; id < 40; ++id)
	{
		const std::vector<std::uint8_t> frame =
			MakeEncodedFrame(0x010A, id, frameSize, 17, 1);

		if (!client->Queue(frame.data(), frame.size()))
			break;

		++queued;
	}

	REQUIRE(queued < 40);
	REQUIRE(client->QueuedBytes() <= static_cast<std::size_t>(SEND_BUFFER_SIZE));

	REQUIRE(client->Flush());
	REQUIRE(RunUntil(pair.context,
	                 [&]() { return ids.size() == queued; }, 4000));
	REQUIRE(ids.size() == queued);
	REQUIRE(receivedSize == queued * frameSize);

	for (std::size_t i = 0; i < ids.size(); ++i)
		REQUIRE(ids[i] == static_cast<std::int16_t>(i));

	client->ForceClose();
	server->ForceClose();
	pair.context.restart();
	pair.context.poll();
}

TEST_CASE("transport session reports write failure", "[transport]")
{
	LoopbackPair pair;

	auto client = std::make_shared<net::Session>(
		pair.context, std::move(pair.client), net::SessionMode::Framed, false);

	bool failed = false;
	client->SetErrorHandler([&](const char *) { failed = true; });

	asio::error_code ec;
	pair.server.set_option(asio::socket_base::linger(true, 0), ec);
	pair.server.close(ec);

	for (int attempt = 0; attempt < 8 && !failed; ++attempt)
	{
		const std::vector<std::uint8_t> encoded =
			MakeEncodedFrame(0x010A, 1, 8192, 3, 1);

		client->QueueAndFlush(encoded.data(), encoded.size());
		RunUntil(pair.context, [&]() { return failed; }, 500);
	}

	REQUIRE(failed);

	client->ForceClose();
	pair.context.restart();
	pair.context.poll();
}

TEST_CASE("transport session flushes queued frames before close", "[transport]")
{
	LoopbackPair pair;

	auto client = std::make_shared<net::Session>(
		pair.context, std::move(pair.client), net::SessionMode::Framed, false);

	bool closed = false;
	client->SetCloseHandler([&]() { closed = true; });

	const std::vector<std::uint8_t> encoded =
		MakeEncodedFrame(0x010A, 9, 256, 11, 1);

	REQUIRE(client->Queue(encoded.data(), encoded.size()));
	client->Close();

	std::vector<std::uint8_t> received;
	std::size_t receivedSize = 0;

	received.resize(encoded.size());

	asio::async_read(
		pair.server, asio::buffer(received, encoded.size()),
		[&](const asio::error_code &error, std::size_t bytes)
		{
			if (!error)
				receivedSize = bytes;
		});

	REQUIRE(RunUntil(pair.context,
	                 [&]() { return closed && receivedSize == encoded.size(); },
	                 4000));
	REQUIRE(receivedSize == encoded.size());
	REQUIRE(closed);

	pair.context.restart();
	pair.context.poll();
}

TEST_CASE("transport acceptor and connector complete the init handshake", "[transport]")
{
	asio::io_context context;

	auto acceptor = std::make_shared<net::Acceptor>(context);
	std::string error;
	REQUIRE(acceptor->Listen("127.0.0.1", 0, error));
	const unsigned short port = acceptor->LocalPort();
	REQUIRE(port != 0);

	std::shared_ptr<net::Session> serverSession;
	std::vector<std::uint8_t> received;
	std::size_t receivedSize = 0;

	acceptor->Start(
		[&](tcp::socket socket, const std::string &peer)
		{
			REQUIRE(peer == "127.0.0.1");
			serverSession = std::make_shared<net::Session>(
				context, std::move(socket), net::SessionMode::Framed, true);
			serverSession->SetFrameHandler(
				[&](std::uint8_t *frame, std::size_t size)
				{
					received.assign(frame, frame + size);
					receivedSize = size;
				});
			serverSession->Start();
		},
		[&](const std::string &message) { error = message; });

	bool connected = false;
	std::shared_ptr<net::Session> clientSession;

	auto connector = std::make_shared<net::Connector>(context);
	connector->Connect(
		"127.0.0.1", port, "127.0.0.1", true,
		[&](tcp::socket socket)
		{
			connected = true;
			clientSession = std::make_shared<net::Session>(
				context, std::move(socket), net::SessionMode::Framed, false);
			clientSession->Start();
		},
		[&](const std::string &message) { error = message; });

	REQUIRE(RunUntil(context, [&]() { return connected; }));

	const std::vector<std::uint8_t> encoded =
		MakeEncodedFrame(0x020D, 5, 96, 21, 3);

	REQUIRE(clientSession->QueueAndFlush(encoded.data(), encoded.size()));
	REQUIRE(RunUntil(context, [&]() { return receivedSize != 0; }));
	REQUIRE(receivedSize == encoded.size());
	REQUIRE(net::ParseHeader(received.data()).id == 5);

	clientSession->ForceClose();
	serverSession->ForceClose();
	acceptor->Close();
	context.restart();
	context.poll();
}

TEST_CASE("transport local address selection matches the server list", "[transport]")
{
	const std::vector<std::string> localAddresses =
	{
		"192.168.0.10",
		"10.1.2.3"
	};

	const std::vector<std::string> groupAddresses =
	{
		"",
		"10.9.9.9",
		"10.1.2.3"
	};

	const net::ServerGroupMatch match =
		net::MatchServerGroup(localAddresses, groupAddresses);

	REQUIRE(match.group == 2);
	REQUIRE(match.address == "10.1.2.3");

	const net::ServerGroupMatch missing =
		net::MatchServerGroup(localAddresses, { "172.16.0.1" });

	REQUIRE(missing.group == -1);

	const std::vector<std::string> discovered = net::LocalIPv4Addresses();

	for (const std::string &address : discovered)
	{
		asio::error_code ec;
		const asio::ip::address parsed = asio::ip::make_address(address, ec);
		REQUIRE_FALSE(ec);
		REQUIRE(parsed.is_v4());
	}
}

TEST_CASE("transport connector reports a refused connection", "[transport]")
{
	asio::io_context context;

	// Reserve a port, then close it so nothing is listening.
	asio::error_code ec;
	tcp::acceptor probe(context);
	probe.open(tcp::v4(), ec);
	REQUIRE_FALSE(ec);
	probe.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
	REQUIRE_FALSE(ec);
	const unsigned short port = probe.local_endpoint().port();
	probe.close();

	bool failed = false;

	auto connector = std::make_shared<net::Connector>(context);
	connector->Connect(
		"127.0.0.1", port, "127.0.0.1", true,
		[&](tcp::socket) {},
		[&](const std::string &) { failed = true; });

	REQUIRE(RunUntil(context, [&]() { return failed; }, 4000));
	REQUIRE(failed);

	context.restart();
	context.poll();
}

TEST_CASE("transport connector succeeds on a later attempt", "[transport]")
{
	asio::io_context context;

	asio::error_code ec;
	tcp::acceptor probe(context);
	probe.open(tcp::v4(), ec);
	REQUIRE_FALSE(ec);
	probe.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
	REQUIRE_FALSE(ec);
	const unsigned short port = probe.local_endpoint().port();
	probe.close();

	bool firstFailed = false;

	auto first = std::make_shared<net::Connector>(context);
	first->Connect(
		"127.0.0.1", port, "127.0.0.1", true,
		[&](tcp::socket) {},
		[&](const std::string &) { firstFailed = true; });

	REQUIRE(RunUntil(context, [&]() { return firstFailed; }, 4000));

	auto acceptor = std::make_shared<net::Acceptor>(context);
	std::string error;
	REQUIRE(acceptor->Listen("127.0.0.1", port, error));

	acceptor->Start(
		[&](tcp::socket socket, const std::string &)
		{
			REQUIRE(socket.is_open());
		},
		[&](const std::string &message) { error = message; });

	bool connected = false;

	auto second = std::make_shared<net::Connector>(context);
	second->Connect(
		"127.0.0.1", port, "127.0.0.1", true,
		[&](tcp::socket) { connected = true; },
		[&](const std::string &message) { error = message; });

	REQUIRE(RunUntil(context, [&]() { return connected; }, 4000));
	REQUIRE(connected);

	acceptor->Close();
	context.restart();
	context.poll();
}

TEST_CASE("transport session billing mode exchanges 196-byte records", "[transport]")
{
	LoopbackPair pair;

	auto client = std::make_shared<net::Session>(
		pair.context, std::move(pair.client), net::SessionMode::Billing, false);
	auto server = std::make_shared<net::Session>(
		pair.context, std::move(pair.server), net::SessionMode::Billing, false);

	std::vector<std::uint8_t> received;

	server->SetFrameHandler(
		[&](std::uint8_t *frame, std::size_t size)
		{
			received.assign(frame, frame + size);
		});

	client->Start();
	server->Start();

	std::vector<std::uint8_t> record(BILLING_RECORD_SIZE);
	for (std::size_t i = 0; i < record.size(); ++i)
		record[i] = static_cast<std::uint8_t>((i * 19u + 5u) & 0xFFu);

	REQUIRE(client->QueueAndFlush(record.data(), record.size()));
	REQUIRE(RunUntil(pair.context, [&]() { return !received.empty(); }));
	REQUIRE(received.size() == BILLING_RECORD_SIZE);
	REQUIRE(std::memcmp(received.data(), record.data(), record.size()) == 0);

	client->ForceClose();
	server->ForceClose();
	pair.context.restart();
	pair.context.poll();
}

TEST_CASE("transport connection facade sends and drops on a full queue", "[transport]")
{
	LoopbackPair pair;

	auto clientSession = std::make_shared<net::Session>(
		pair.context, std::move(pair.client), net::SessionMode::Framed, false);
	auto serverSession = std::make_shared<net::Session>(
		pair.context, std::move(pair.server), net::SessionMode::Framed, false);

	net::Connection connection(clientSession);

	std::vector<std::int16_t> ids;
	serverSession->SetFrameHandler(
		[&](std::uint8_t *frame, std::size_t)
		{
			ids.push_back(net::ParseHeader(frame).id);
		});

	clientSession->Start();
	serverSession->Start();

	char message[64];
	std::memset(message, 0, sizeof(message));
	HEADER *header = reinterpret_cast<HEADER *>(message);
	header->Type = 0x010A;
	header->ID = 42;

	REQUIRE(connection.SendOneMessage(message, sizeof(message)));
	REQUIRE(RunUntil(pair.context, [&]() { return ids.size() == 1; }));
	REQUIRE(ids[0] == 42);

	// Fill the queue without running the io_context so writes cannot drain.
	bool rejected = false;

	for (int i = 0; i < 100; ++i)
	{
		std::vector<char> large(8192, 0);
		HEADER *largeHeader = reinterpret_cast<HEADER *>(large.data());
		largeHeader->Type = 0x010A;
		largeHeader->ID = static_cast<short>(100 + i);

		if (!connection.AddMessage(large.data(), static_cast<int>(large.size())))
		{
			rejected = true;
			break;
		}
	}

	REQUIRE(rejected);

	REQUIRE(connection.CloseSocket());
	REQUIRE(RunUntil(pair.context, [&]() { return !connection.IsOpen(); }, 4000));

	serverSession->ForceClose();
	pair.context.restart();
	pair.context.poll();
}

