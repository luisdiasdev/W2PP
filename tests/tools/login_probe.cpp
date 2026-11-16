/*
 * Login probe: developer tool for the headless TMSrv.
 *
 * Connects to the game port, completes the init handshake, sends a real
 * _MSG_AccountLogin frame and prints every decoded reply. Useful to check the
 * TMSrv -> DBSrv -> client round trip without launching the game client.
 *
 * usage: login_probe [host] [port] [account] [password]
 */

#include "../../Code/Basedef.h"
#include "../../Code/net/Connection.h"
#include "../../Code/net/Connector.h"
#include "../../Code/net/FrameCodec.h"
#include "../../Code/net/Protocol.h"
#include "../../Code/net/Session.h"

#include <asio.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	const int port = argc > 2 ? std::atoi(argv[2]) : GAME_PORT;
	const char *accountName = argc > 3 ? argv[3] : "ADMIN";
	const char *password = argc > 4 ? argv[4] : "ADMIN";

	asio::io_context context;
	net::Connection connection;
	std::shared_ptr<net::Session> session;
	bool connected = false;

	auto connector = std::make_shared<net::Connector>(context);

	connector->Connect(
		host, static_cast<unsigned short>(port), "0.0.0.0", true,
		[&](asio::ip::tcp::socket socket)
		{
			connected = true;

			session = std::make_shared<net::Session>(
				context, std::move(socket), net::SessionMode::Framed, false);
			connection.Attach(session);

			session->SetFrameHandler(
				[&](std::uint8_t *frame, std::size_t size)
				{
					const net::HeaderFields fields = net::ParseHeader(frame);

					std::printf("reply type=%d id=%d size=%zu kw=%u tick=%u\n",
						fields.type, fields.id, size, fields.keywordIndex,
						fields.clientTick);

					const std::size_t textSize =
						size > sizeof(HEADER) ? size - sizeof(HEADER) : 0;
					const std::size_t shown = textSize < 120 ? textSize : 120;

					if (shown > 0)
					{
						char text[128];
						std::memset(text, 0, sizeof(text));
						std::memcpy(text, frame + sizeof(HEADER), shown);
						std::printf("  payload: %.120s\n", text);
					}

					std::fflush(stdout);
				});

			session->SetErrorHandler(
				[&](const char *reason)
				{
					std::printf("session error: %s\n", reason != nullptr ? reason : "");
					std::fflush(stdout);
				});

			session->SetCloseHandler(
				[]()
				{
					std::printf("session closed\n");
					std::fflush(stdout);
				});

			session->Start();

			MSG_AccountLogin login;
			std::memset(&login, 0, sizeof(login));

			login.Type = _MSG_AccountLogin;
			login.Size = sizeof(login);
			login.ID = 0;
			login.ClientVersion = APP_VERSION;

			std::strncpy(login.AccountName, accountName,
				sizeof(login.AccountName) - 1);
			std::strncpy(login.AccountPassword, password,
				sizeof(login.AccountPassword) - 1);

			std::printf("sending _MSG_AccountLogin account=%s client=%d size=%d\n",
				accountName, login.ClientVersion, login.Size);
			std::fflush(stdout);

			if (!connection.SendOneMessage(reinterpret_cast<char *>(&login),
			                               sizeof(login)))
				std::printf("send failed\n");
		},
		[&](const std::string &error)
		{
			std::printf("connect error: %s\n", error.c_str());
			std::fflush(stdout);
			context.stop();
		});

	asio::steady_timer timer(context);
	timer.expires_after(std::chrono::seconds(8));
	timer.async_wait([&](const asio::error_code &) { context.stop(); });

	context.run();

	std::printf("done connected=%d\n", connected ? 1 : 0);

	if (session)
		session->ForceClose();

	connection.Detach();
	context.restart();
	context.poll();

	return connected ? 0 : 1;
}
