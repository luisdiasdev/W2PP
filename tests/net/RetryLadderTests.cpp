#include <catch2/catch_test_macros.hpp>

#include "../../Code/host/Runtime.h"
#include "../../Code/net/Acceptor.h"
#include "../../Code/net/Connector.h"
#include "../../Code/net/RetryLadder.h"

#include <asio.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace
{

using asio::ip::tcp;

int ReserveClosedPort(asio::io_context &context)
{
	asio::error_code ec;
	tcp::acceptor probe(context);

	probe.open(tcp::v4(), ec);
	REQUIRE_FALSE(ec);
	probe.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
	REQUIRE_FALSE(ec);

	const unsigned short port = probe.local_endpoint().port();
	probe.close();

	return port;
}

bool RunUntil(asio::io_context &context, const std::function<bool()> &predicate,
              int timeoutMs = 5000)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(timeoutMs);

	while (!predicate() && std::chrono::steady_clock::now() < deadline)
		context.run_for(std::chrono::milliseconds(5));

	return predicate();
}

} // namespace

TEST_CASE("db reconnect ladder retries and succeeds on the second attempt", "[transport]")
{
	asio::io_context context;
	const unsigned short port = ReserveClosedPort(context);

	auto ladder = std::make_shared<net::RetryLadder>(
		context, 2, std::chrono::milliseconds(200));

	std::shared_ptr<net::Acceptor> listener;
	std::shared_ptr<net::Connector> connector;

	int attempts = 0;
	int failures = 0;
	bool connected = false;
	bool exhausted = false;
	bool bindFailed = false;

	const auto started = std::chrono::steady_clock::now();

	const std::function<void()> attempt =
		[&]()
		{
			connector = std::make_shared<net::Connector>(context);

			connector->Connect(
				"127.0.0.1", port, "127.0.0.1", true,
				[&](tcp::socket socket)
				{
					connected = true;
					ladder->ReportSuccess();
					socket.close();
				},
				[&](const std::string &)
				{
					++failures;

					// Bring the listener up only after the first attempt fails, so
					// the ladder has to use its delayed second attempt.
					if (listener == nullptr)
					{
						listener = std::make_shared<net::Acceptor>(context);

						std::string error;

						if (!listener->Listen("127.0.0.1", port, error))
							bindFailed = true;
						else
							listener->Start(
								[](tcp::socket socket, const std::string &)
								{
									socket.close();
								},
								[](const std::string &) {});
					}

					ladder->ReportFailure();
				});
		};

	ladder->SetHandlers(
		[&](int attemptNumber)
		{
			++attempts;
			REQUIRE(attemptNumber == attempts);
			attempt();
		},
		[&]() { exhausted = true; });

	ladder->Start();

	REQUIRE(RunUntil(context, [&]() { return connected || exhausted; }));

	const auto elapsed = std::chrono::steady_clock::now() - started;

	REQUIRE_FALSE(bindFailed);
	REQUIRE(connected);
	REQUIRE_FALSE(exhausted);
	REQUIRE(attempts == 2);
	REQUIRE(failures == 1);
	REQUIRE_FALSE(ladder->IsActive());
	REQUIRE(elapsed >= std::chrono::milliseconds(180));

	if (listener)
		listener->Close();

	context.restart();
	context.poll();
}

TEST_CASE("db reconnect ladder shuts the host down when both attempts fail", "[transport]")
{
	host::Runtime runtime;
	const unsigned short port = ReserveClosedPort(runtime.Context());

	auto ladder = std::make_shared<net::RetryLadder>(
		runtime.Context(), 2, std::chrono::milliseconds(200));

	std::shared_ptr<net::Connector> connector;

	int attempts = 0;
	bool exhausted = false;

	const std::function<void()> attempt =
		[&]()
		{
			connector = std::make_shared<net::Connector>(runtime.Context());

			connector->Connect(
				"127.0.0.1", port, "127.0.0.1", true,
				[&](tcp::socket) { ladder->ReportSuccess(); },
				[&](const std::string &) { ladder->ReportFailure(); });
		};

	ladder->SetHandlers(
		[&](int)
		{
			++attempts;
			attempt();
		},
		[&]()
		{
			exhausted = true;
			runtime.RequestShutdown();
		});

	ladder->Start();
	runtime.Run();

	REQUIRE(exhausted);
	REQUIRE(attempts == 2);
	REQUIRE(runtime.IsShuttingDown());
	REQUIRE_FALSE(ladder->IsActive());

	connector.reset();
	ladder.reset();
}
