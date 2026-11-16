#include <catch2/catch_test_macros.hpp>

#include "../../Code/net/Acceptor.h"
#include "../../Code/net/Connection.h"
#include "../../Code/net/Connector.h"
#include "../../Code/net/FrameCodec.h"
#include "../../Code/net/Protocol.h"
#include "../../Code/net/Session.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace
{

using asio::ip::tcp;

// Message types from Basedef.h (client-to-game and game-to-client flags).
constexpr std::int16_t kAccountLogin = 13 | 0x0200;        // _MSG_AccountLogin
constexpr std::int16_t kCnfAccountLogin = 10 | 0x0100;     // _MSG_CNFAccountLogin
constexpr std::int16_t kCharacterLogin = 19 | 0x0200;      // _MSG_CharacterLogin
constexpr std::int16_t kCnfCharacterLogin = 20 | 0x0100;   // _MSG_CNFCharacterLogin
constexpr std::int16_t kCloseCommand = 0x0A01;
constexpr std::int16_t kAfterCloseCommand = 0x0A02;

bool RunUntil(asio::io_context &context, const std::function<bool()> &predicate,
              int timeoutMs = 4000)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(timeoutMs);

	while (!predicate() && std::chrono::steady_clock::now() < deadline)
		context.run_for(std::chrono::milliseconds(5));

	return predicate();
}

std::vector<char> MakeMessage(std::int16_t type, std::int16_t id, std::size_t size)
{
	std::vector<char> message(size, 0);

	for (std::size_t i = sizeof(HEADER); i < size; ++i)
		message[i] = static_cast<char>((i * 5 + id) & 0xFF);

	HEADER *header = reinterpret_cast<HEADER *>(message.data());
	header->Type = type;
	header->ID = id;

	return message;
}

struct GameSlot
{
	net::Connection connection;
	std::uint64_t generation = 0;
	int frames = 0;
	int closeCallbacks = 0;
	int staleCallbacks = 0;
	bool midDrainClosed = false;
};

void AttachGameSlot(asio::io_context &context, GameSlot &slot, tcp::socket socket)
{
	auto session = std::make_shared<net::Session>(
		context, std::move(socket), net::SessionMode::Framed, true);

	slot.connection.Attach(session);
	slot.generation = session->Generation();

	session->SetFrameHandler(
		[&slot](std::uint8_t *frame, std::size_t)
		{
			if (!slot.connection.IsCurrent(slot.generation))
			{
				++slot.staleCallbacks;
				return;
			}

			++slot.frames;

			const net::HeaderFields fields = net::ParseHeader(frame);

			if (fields.type == kCloseCommand)
			{
				// Mirrors CloseUser after a notice was queued: the graceful close
				// drains the queued frame, so the session stays alive after the
				// slot has already been released.
				std::vector<char> notice = MakeMessage(kCnfCharacterLogin, 99, 48);
				slot.connection.SendOneMessage(
					notice.data(), static_cast<int>(notice.size()));

				slot.connection.CloseSocket();
				slot.connection.Detach();
				slot.midDrainClosed = true;
				return;
			}

			const std::int16_t replyType =
				fields.type == kAccountLogin ? kCnfAccountLogin :
				fields.type == kCharacterLogin ? kCnfCharacterLogin : 0;

			if (replyType == 0)
				return;

			std::vector<char> reply = MakeMessage(replyType, fields.id, 64);

			slot.connection.SendOneMessage(
				reply.data(), static_cast<int>(reply.size()));
		});

	session->SetCloseHandler(
		[&slot]()
		{
			if (!slot.connection.IsCurrent(slot.generation))
				return;

			++slot.closeCallbacks;
			slot.connection.Detach();
		});

	session->Start();
}

struct FixtureClient
{
	net::Connection connection;
	std::shared_ptr<net::Session> session;
	std::vector<std::vector<std::uint8_t>> received;
	int closes = 0;
};

void ConnectClient(asio::io_context &context, std::shared_ptr<net::Acceptor> acceptor,
                   FixtureClient &client)
{
	auto connector = std::make_shared<net::Connector>(context);

	connector->Connect(
		"127.0.0.1", acceptor->LocalPort(), "127.0.0.1", true,
		[&client, &context](tcp::socket socket)
		{
			client.session = std::make_shared<net::Session>(
				context, std::move(socket), net::SessionMode::Framed, false);
			client.connection.Attach(client.session);

			client.session->SetFrameHandler(
				[&client](std::uint8_t *frame, std::size_t size)
				{
					client.received.emplace_back(frame, frame + size);
				});

			client.session->SetCloseHandler([&client]() { ++client.closes; });

			client.session->Start();
		},
		[](const std::string &) {});

	REQUIRE(RunUntil(context, [&]() { return client.session != nullptr; }));
}

} // namespace

TEST_CASE("game link smoke completes the handshake and one game message", "[transport]")
{
	asio::io_context context;

	auto acceptor = std::make_shared<net::Acceptor>(context);
	std::string listenError;
	REQUIRE(acceptor->Listen("127.0.0.1", 0, listenError));

	GameSlot slot;

	acceptor->Start(
		[&](tcp::socket socket, const std::string &)
		{
			AttachGameSlot(context, slot, std::move(socket));
		},
		[&](const std::string &error) { listenError = error; });

	FixtureClient client;
	ConnectClient(context, acceptor, client);

	REQUIRE(RunUntil(context, [&]() { return slot.connection.IsOpen(); }));

	// Account login: the fixture handler answers with the matching confirmation
	// frame, encoded by the same send path the game server uses.
	std::vector<char> login = MakeMessage(kAccountLogin, 3, 48);
	REQUIRE(client.connection.SendOneMessage(
		login.data(), static_cast<int>(login.size())));

	REQUIRE(RunUntil(context, [&]() { return !client.received.empty(); }));

	REQUIRE(slot.frames == 1);
	REQUIRE(client.received[0].size() == 64);

	const net::HeaderFields reply = net::ParseHeader(client.received[0].data());
	REQUIRE(reply.type == kCnfAccountLogin);
	REQUIRE(reply.id == 3);

	for (std::size_t i = sizeof(HEADER); i < client.received[0].size(); ++i)
		REQUIRE(client.received[0][i] == static_cast<std::uint8_t>((i * 5 + 3) & 0xFF));

	// Character select: same handshake/session, a second game message.
	std::vector<char> character = MakeMessage(kCharacterLogin, 7, 128);
	REQUIRE(client.connection.SendOneMessage(
		character.data(), static_cast<int>(character.size())));

	REQUIRE(RunUntil(context, [&]() { return client.received.size() == 2; }));

	REQUIRE(slot.frames == 2);
	REQUIRE(client.received[1].size() == 64);
	REQUIRE(net::ParseHeader(client.received[1].data()).type == kCnfCharacterLogin);
	REQUIRE(net::ParseHeader(client.received[1].data()).id == 7);

	client.session->ForceClose();
	acceptor->Close();
	slot.connection.Detach();
	context.restart();
	context.poll();
}

TEST_CASE("client slot reuse ignores stale generations", "[transport]")
{
	asio::io_context context;

	auto acceptor = std::make_shared<net::Acceptor>(context);
	std::string listenError;
	REQUIRE(acceptor->Listen("127.0.0.1", 0, listenError));

	GameSlot slot;

	acceptor->Start(
		[&](tcp::socket socket, const std::string &)
		{
			AttachGameSlot(context, slot, std::move(socket));
		},
		[&](const std::string &error) { listenError = error; });

	FixtureClient first;
	ConnectClient(context, acceptor, first);

	REQUIRE(RunUntil(context, [&]() { return slot.connection.IsOpen(); }));

	const std::uint64_t firstGeneration = slot.generation;
	REQUIRE(firstGeneration != 0);
	REQUIRE(slot.connection.IsCurrent(firstGeneration));

	first.session->ForceClose();

	REQUIRE(RunUntil(context, [&]() { return slot.closeCallbacks == 1; }));
	REQUIRE_FALSE(slot.connection.IsCurrent(firstGeneration));
	REQUIRE_FALSE(slot.connection.IsOpen());

	// The same slot is reattached to a new session; events carrying the old
	// generation must be rejected.
	FixtureClient second;
	ConnectClient(context, acceptor, second);

	REQUIRE(RunUntil(context, [&]() { return slot.connection.IsOpen(); }));

	const std::uint64_t secondGeneration = slot.generation;
	REQUIRE(secondGeneration != firstGeneration);
	REQUIRE_FALSE(slot.connection.IsCurrent(firstGeneration));
	REQUIRE(slot.connection.IsCurrent(secondGeneration));

	std::vector<char> message = MakeMessage(kAccountLogin, 11, 96);
	REQUIRE(second.connection.SendOneMessage(
		message.data(), static_cast<int>(message.size())));

	REQUIRE(RunUntil(context, [&]() { return second.received.size() == 1; }));
	REQUIRE(slot.frames == 1);
	REQUIRE(slot.staleCallbacks == 0);
	REQUIRE(net::ParseHeader(second.received[0].data()).id == 11);

	second.session->ForceClose();
	acceptor->Close();
	slot.connection.Detach();
	context.restart();
	context.poll();
}

TEST_CASE("client handler closes the session mid-drain", "[transport]")
{
	asio::io_context context;

	auto acceptor = std::make_shared<net::Acceptor>(context);
	std::string listenError;
	REQUIRE(acceptor->Listen("127.0.0.1", 0, listenError));

	GameSlot slot;

	acceptor->Start(
		[&](tcp::socket socket, const std::string &)
		{
			AttachGameSlot(context, slot, std::move(socket));
		},
		[&](const std::string &error) { listenError = error; });

	FixtureClient client;
	ConnectClient(context, acceptor, client);

	REQUIRE(RunUntil(context, [&]() { return slot.connection.IsOpen(); }));

	// Two frames in one submission: the first handler queues a notice, closes
	// the session and releases the slot, so the second frame must not reach a
	// handler and the late close callback must be ignored.
	std::vector<char> closeFrame = MakeMessage(kCloseCommand, 21, 32);
	std::vector<char> afterFrame = MakeMessage(kAfterCloseCommand, 22, 32);

	REQUIRE(client.connection.AddMessage(
		closeFrame.data(), static_cast<int>(closeFrame.size())));
	REQUIRE(client.connection.AddMessage(
		afterFrame.data(), static_cast<int>(afterFrame.size())));
	REQUIRE(client.connection.SendMessageA());

	REQUIRE(RunUntil(context, [&]() { return client.closes == 1; }));

	REQUIRE(slot.midDrainClosed);
	REQUIRE(slot.frames == 1);
	REQUIRE(slot.closeCallbacks == 0);
	REQUIRE(slot.staleCallbacks == 0);
	REQUIRE_FALSE(slot.connection.IsOpen());

	acceptor->Close();
	slot.connection.Detach();
	context.restart();
	context.poll();
}
