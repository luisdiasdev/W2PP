#include <catch2/catch_test_macros.hpp>

#include "../../Code/net/Acceptor.h"
#include "../../Code/net/BillingLink.h"
#include "../../Code/net/Connection.h"
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

bool RunUntil(asio::io_context &context, const std::function<bool()> &predicate,
              int timeoutMs = 5000)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(timeoutMs);

	while (!predicate() && std::chrono::steady_clock::now() < deadline)
		context.run_for(std::chrono::milliseconds(5));

	return predicate();
}

std::vector<std::uint8_t> MakeRecord(std::uint8_t seed)
{
	std::vector<std::uint8_t> record(BILLING_RECORD_SIZE);

	for (std::size_t i = 0; i < record.size(); ++i)
		record[i] = static_cast<std::uint8_t>((i * 19u + seed) & 0xFFu);

	return record;
}

struct RawPair
{
	asio::io_context context;
	tcp::socket client{ context };
	tcp::socket server{ context };

	RawPair()
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

TEST_CASE("billing link sends 196-byte records and retries after a disconnect", "[transport]")
{
	asio::io_context context;

	auto acceptor = std::make_shared<net::Acceptor>(context);
	std::string listenError;
	REQUIRE(acceptor->Listen("127.0.0.1", 0, listenError));
	const unsigned short port = acceptor->LocalPort();
	REQUIRE(port != 0);

	std::shared_ptr<net::Session> serverSession;
	std::vector<std::vector<std::uint8_t>> records;

	acceptor->Start(
		[&](tcp::socket socket, const std::string &)
		{
			serverSession = std::make_shared<net::Session>(
				context, std::move(socket), net::SessionMode::Billing, false);
			serverSession->SetFrameHandler(
				[&](std::uint8_t *frame, std::size_t size)
				{
					records.emplace_back(frame, frame + size);
				});
			serverSession->Start();
		},
		[&](const std::string &error) { listenError = error; });

	net::Connection connection;
	net::BillingLink link(context, connection);

	int connectEvents = 0;
	int lostEvents = 0;

	link.SetHandlers(
		[&](char *record)
		{
			// The real host forwards records to ProcessBILLMessage.
			REQUIRE(record != nullptr);
		},
		[&]() { ++connectEvents; },
		[&](const char *) { ++lostEvents; });

	REQUIRE(link.Connect("127.0.0.1", port, "127.0.0.1"));
	REQUIRE(RunUntil(context, [&]() { return link.IsConnected() && serverSession; }));
	REQUIRE(connectEvents == 1);
	REQUIRE(link.RetryCounter() == 0);

	const std::vector<std::uint8_t> record = MakeRecord(7);
	_AUTH_GAME auth;
	std::memcpy(&auth, record.data(), BILLING_RECORD_SIZE);

	REQUIRE(connection.SendBillMessage(reinterpret_cast<char *>(&auth)));
	REQUIRE(RunUntil(context, [&]() { return records.size() == 1; }));
	REQUIRE(records[0].size() == BILLING_RECORD_SIZE);
	REQUIRE(std::memcmp(records[0].data(), record.data(), BILLING_RECORD_SIZE) == 0);

	serverSession->ForceClose();

	REQUIRE(RunUntil(context,
		[&]()
		{
			return !link.IsConnected() &&
				link.RetryCounter() == net::BillingLink::kRetryTicks;
		}));
	REQUIRE(lostEvents >= 1);
	REQUIRE(connection.IsOpen() == false);

	for (int tick = 1; tick < net::BillingLink::kRetryTicks; ++tick)
		link.Tick();

	REQUIRE(link.RetryCounter() == 1);
	REQUIRE_FALSE(link.IsConnected());
	REQUIRE_FALSE(link.IsDisabled());

	link.Tick();

	REQUIRE(RunUntil(context, [&]() { return link.IsConnected(); }));
	REQUIRE(connectEvents == 2);
	REQUIRE(link.RetryCounter() == 0);

	if (serverSession)
		serverSession->ForceClose();

	connection.Detach();
	acceptor->Close();
	context.restart();
	context.poll();
}

TEST_CASE("billing parser handles split and coalesced 196-byte reads", "[transport]")
{
	RawPair pair;

	auto server = std::make_shared<net::Session>(
		pair.context, std::move(pair.server), net::SessionMode::Billing, false);

	std::vector<std::vector<std::uint8_t>> records;
	server->SetFrameHandler(
		[&](std::uint8_t *frame, std::size_t size)
		{
			records.emplace_back(frame, frame + size);
		});

	server->Start();

	asio::error_code ec;
	pair.client.set_option(tcp::no_delay(true), ec);

	const std::vector<std::uint8_t> split = MakeRecord(3);

	// First 100 bytes, then the remaining 96: the parser must hold the
	// partial record until it is complete.
	asio::write(pair.client, asio::buffer(split.data(), 100), ec);
	REQUIRE_FALSE(ec);
	pair.context.run_for(std::chrono::milliseconds(30));
	REQUIRE(records.empty());

	asio::write(pair.client, asio::buffer(split.data() + 100, BILLING_RECORD_SIZE - 100), ec);
	REQUIRE_FALSE(ec);
	REQUIRE(RunUntil(pair.context, [&]() { return records.size() == 1; }));
	REQUIRE(records[0].size() == BILLING_RECORD_SIZE);
	REQUIRE(std::memcmp(records[0].data(), split.data(), BILLING_RECORD_SIZE) == 0);

	// Three records in a single write: coalesced reads must yield three
	// separate records in submission order.
	std::vector<std::uint8_t> coalesced;
	coalesced.reserve(3 * BILLING_RECORD_SIZE);

	for (std::uint8_t seed : { 10, 11, 12 })
	{
		const std::vector<std::uint8_t> record = MakeRecord(seed);
		coalesced.insert(coalesced.end(), record.begin(), record.end());
	}

	asio::write(pair.client, asio::buffer(coalesced), ec);
	REQUIRE_FALSE(ec);
	REQUIRE(RunUntil(pair.context, [&]() { return records.size() == 4; }));

	for (std::size_t i = 1; i < records.size(); ++i)
		REQUIRE(records[i].size() == BILLING_RECORD_SIZE);

	REQUIRE(records[1][0] == MakeRecord(10)[0]);
	REQUIRE(records[2][0] == MakeRecord(11)[0]);
	REQUIRE(records[3][0] == MakeRecord(12)[0]);

	server->ForceClose();
	pair.context.restart();
	pair.context.poll();
}

TEST_CASE("billing link disables itself when a reconnect attempt fails", "[transport]")
{
	asio::io_context context;

	// Nothing is listening on this port.
	tcp::acceptor probe(context);
	asio::error_code ec;

	probe.open(tcp::v4(), ec);
	REQUIRE_FALSE(ec);
	probe.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
	REQUIRE_FALSE(ec);
	const unsigned short port = probe.local_endpoint().port();
	probe.close();

	net::Connection connection;
	net::BillingLink link(context, connection);

	bool lost = false;

	link.SetHandlers(
		nullptr,
		nullptr,
		[&](const char *) { lost = true; });

	REQUIRE(link.Connect("127.0.0.1", port, "127.0.0.1"));
	REQUIRE(RunUntil(context, [&]() { return lost; }, 4000));

	REQUIRE(lost);
	REQUIRE(link.IsDisabled());
	REQUIRE_FALSE(link.IsConnected());
	REQUIRE_FALSE(connection.IsOpen());

	connection.Detach();
	context.restart();
	context.poll();
}
