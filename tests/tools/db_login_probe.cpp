/*
 * DBSrv login probe.
 *
 * Speaks the game-server side of the DB protocol directly to DBSrv: connects
 * to DB_PORT with the framed handshake, sends one _MSG_DBAccountLogin and
 * reports which reply arrived. It exercises the PostgreSQL-backed admission
 * path without TMSrv or a game client.
 *
 * usage: db_login_probe [host] [port] [account] [password] [temp-key]
 *
 * Exit codes: 0 account admitted, 2 credential/state refusal, 3 connect
 * failure, 4 no reply before the timeout.
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
	const int port = argc > 2 ? std::atoi(argv[2]) : DB_PORT;
	const char *accountName = argc > 3 ? argv[3] : "ADMIN";
	const char *password = argc > 4 ? argv[4] : "ADMIN";
	const char *tempKey = argc > 5 ? argv[5] : "";

	asio::io_context context;
	net::Connection connection;
	std::shared_ptr<net::Session> session;
	bool connected = false;
	int outcome = 4;

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

					std::printf("reply type=%d id=%d size=%zu\n",
						fields.type, fields.id, size);

					switch (fields.type)
					{
					case _MSG_DBCNFAccountLogin:
						std::printf("result=admitted\n");
						outcome = 0;
						break;
					case _MSG_DBAccountLoginFail_Account:
						std::printf("result=missing-account\n");
						outcome = 2;
						break;
					case _MSG_DBAccountLoginFail_Pass:
						std::printf("result=wrong-credential-or-locked\n");
						outcome = 2;
						break;
					case _MSG_DBAccountLoginFail_Block:
						std::printf("result=blocked-account\n");
						outcome = 2;
						break;
					default:
						// DBSrv sends session setup frames (for example
						// _MSG_DBSetIndex) on accept; keep waiting for the
						// login result instead of treating them as failures.
						std::printf("result=ignored\n");
						break;
					}

					std::fflush(stdout);

					if (outcome != 4)
						context.stop();
				});

			session->SetErrorHandler(
				[&](const char *reason)
				{
					std::printf("session error: %s\n", reason != nullptr ? reason : "");
					std::fflush(stdout);
				});

			session->SetCloseHandler(
				[&]()
				{
					std::fflush(stdout);
				});

			session->Start();

			MSG_AccountLogin login;
			std::memset(&login, 0, sizeof(login));

			login.Type = _MSG_DBAccountLogin;
			login.Size = sizeof(login);
			login.ID = 1;
			login.ClientVersion = APP_VERSION;

			std::strncpy(login.AccountName, accountName,
				sizeof(login.AccountName) - 1);
			std::strncpy(login.AccountPassword, password,
				sizeof(login.AccountPassword) - 1);
			std::strncpy(login.Zero, tempKey, sizeof(login.Zero) - 1);

			std::printf("sending _MSG_DBAccountLogin account=%s\n", accountName);
			std::fflush(stdout);

			if (!connection.SendOneMessage(reinterpret_cast<char *>(&login),
					sizeof(login)))
				std::printf("send failed\n");
		},
		[&](const std::string &error)
		{
			std::printf("connect error: %s\n", error.c_str());
			std::fflush(stdout);
			outcome = 3;
			context.stop();
		});

	asio::steady_timer timer(context);
	timer.expires_after(std::chrono::seconds(8));
	timer.async_wait([&](const asio::error_code &) { context.stop(); });

	context.run();

	if (!connected)
		outcome = 3;

	std::printf("done connected=%d outcome=%d\n", connected ? 1 : 0, outcome);

	if (session)
		session->ForceClose();

	connection.Detach();
	context.restart();
	context.poll();

	return outcome;
}
