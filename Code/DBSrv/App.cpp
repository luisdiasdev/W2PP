/*
 * Headless DBSrv host.
 *
 * Owns the asio runtime, listeners, redirect connector, slot attachment and
 * shutdown. The protocol handlers remain in Server.cpp / CFileDB.cpp /
 * CReadFiles.cpp.
 */

#include "Server.h"
#include "CReadFiles.h"
#include "CUser.h"

#include "../host/Runtime.h"
#include "../net/Acceptor.h"
#include "../net/Connector.h"
#include "../net/LocalAddress.h"
#include "../net/Session.h"
#include "../platform/Diagnostics.h"
#include "../platform/Path.h"

#include <asio.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

net::Connection AdminClient;

static std::shared_ptr<net::Acceptor> ListenSocket;
static std::shared_ptr<net::Acceptor> AdminSocket;
static std::unique_ptr<host::Runtime> g_runtime;
static std::shared_ptr<net::Connector> g_adminConnector;

namespace
{

void PlatformDiagnosticSink(const char *line)
{
	char buffer[1024];
	strncpy(buffer, line != NULL ? line : "", sizeof(buffer) - 1);
	buffer[sizeof(buffer) - 1] = '\0';

	char systemTag[] = "-system";
	Log(buffer, systemTag, 0);
}

unsigned int PeerIp(const std::string &peer)
{
	asio::error_code ec;
	const asio::ip::address address = asio::ip::make_address(peer, ec);

	if (ec || !address.is_v4())
		return 0;

	const asio::ip::address_v4::bytes_type bytes = address.to_v4().to_bytes();
	return static_cast<unsigned int>(bytes[0]) |
	       (static_cast<unsigned int>(bytes[1]) << 8) |
	       (static_cast<unsigned int>(bytes[2]) << 16) |
	       (static_cast<unsigned int>(bytes[3]) << 24);
}

bool LoadBaseMobSet()
{
	struct BaseFile
	{
		const char *path;
		int index;
		const char *message;
	};

	const BaseFile files[] =
	{
		{ "./BaseMob/TK", 0, "no TransKnight file" },
		{ "./BaseMob/FM", 1, "no Foema file" },
		{ "./BaseMob/BM", 2, "no BeastMaster file" },
		{ "./BaseMob/HT", 3, "no Huntress file" }
	};

	platform::File file;

	for (const BaseFile &item : files)
	{
		if (!file.OpenRead(item.path))
		{
			platform::ReportFatal(hWndMain, item.message, "BOOTING ERROR", 0);
			return false;
		}

		if (!file.Read(&g_pBaseSet[item.index], sizeof(STRUCT_MOB)))
		{
			platform::ReportFatal(hWndMain, item.message, "READ ERROR", 0);
			return false;
		}

		file.Close();
	}

	for (int i = 0; i < MAX_CLASS; ++i)
		g_pBaseSet[i].BaseScore = g_pBaseSet[i].CurrentScore;

	return true;
}

void AcceptGameConnection(asio::ip::tcp::socket socket, const std::string &peer)
{
	const unsigned int ip = PeerIp(peer);
	const unsigned char *cIP = reinterpret_cast<const unsigned char *>(&ip);

	char sIP[64];
	snprintf(sIP, sizeof(sIP), "%u.%u.%u.%u", cIP[0], cIP[1], cIP[2], cIP[3]);

	int user = -1;

	for (int i = 0; i < MAX_SERVER; ++i)
	{
		if (pUser[i].IP == ip && ip != 0)
		{
			user = i;
			break;
		}
	}

	if (user == -1)
	{
		int i = 1;

		for (; i < MAX_SERVERNUMBER; ++i)
		{
			if (!strcmp(g_pServerList[ServerIndex][i], sIP))
				break;
		}

		if (i == MAX_SERVERNUMBER)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err accept request from  %s", sIP);
			Log(temp, "-system", 0);
			return;
		}

		user = i - 1;
	}
	else if (pUser[user].Mode != USER_EMPTY)
	{
		char temp[256];
		snprintf(temp, sizeof(temp), "err accept no previous slot %d", user);
		Log(temp, "-system", 0);

		pUser[user].cSock.CloseSocket();
		pUser[user].Mode = USER_EMPTY;
		return;
	}

	if (user < 0 || user >= MAX_SERVER)
	{
		Log("err accept no empty", "-system", 0);
		return;
	}

	if (!(cIP[0] == LocalIP[0] && cIP[1] == LocalIP[1] && cIP[2] == LocalIP[2]))
	{
		char temp[256];
		snprintf(temp, sizeof(temp),
			"err,accept outer ethernet IP: %u.%u.%u.%u",
			cIP[0], cIP[1], cIP[2], cIP[3]);
		Log(temp, "-system", 0);
		return;
	}

	pUser[user].IP = ip;
	pUser[user].Mode = USER_ACCEPT;

	std::shared_ptr<net::Session> session = std::make_shared<net::Session>(
		g_runtime->Context(), std::move(socket), net::SessionMode::Framed, true);

	pUser[user].cSock.Attach(session);
	const std::uint64_t generation = session->Generation();

	session->SetFrameHandler(
		[user, generation](std::uint8_t *frame, std::size_t)
		{
			if (pUser[user].cSock.Generation() != generation)
				return;

			CurrentTime = platform::MonotonicMs();
			ProcessClientMessage(user, reinterpret_cast<char *>(frame));
		});

	session->SetCloseHandler(
		[user, generation]()
		{
			if (pUser[user].cSock.Generation() != generation)
				return;

			pUser[user].cSock.Detach();
			pUser[user].Mode = USER_EMPTY;
		});

	session->SetErrorHandler(
		[user](const char *reason)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,game link %d: %s", user,
				reason != nullptr ? reason : "");
			Log(temp, "-system", 0);
		});

	session->Start();

	cFileDB.SendDBSignalParm3(user, 0, _MSG_DBSetIndex, ServerIndex, Sapphire, user);

	for (int i = 0; i < 65536; ++i)
	{
		if (GuildInfo[i].Citizen)
			cFileDB.SendGuildInfo(user, i);

		if (g_pGuildWar[i])
			cFileDB.SendDBSignalParm2(user, 0, _MSG_War, i, g_pGuildWar[i]);

		if (g_pGuildAlly[i])
			cFileDB.SendDBSignalParm2(user, 0, _MSG_GuildAlly, i, g_pGuildAlly[i]);
	}

	if (TransperCharacter)
		cFileDB.SendDBSignalParm2(user, 0, _MSG_TransperCharacter, 0, 0);
}

void AcceptAdminConnection(asio::ip::tcp::socket socket, const std::string &peer)
{
	ReadAdmin();

	const unsigned int ip = PeerIp(peer);
	const unsigned char *cIP = reinterpret_cast<const unsigned char *>(&ip);

	int user = -1;

	for (int i = 0; i < MAX_ADMIN; ++i)
	{
		if (pAdmin[i].IP == ip && ip != 0)
		{
			user = i;
			break;
		}
	}

	if (user == -1)
	{
		for (int i = 0; i < MAX_ADMIN; ++i)
		{
			if (pAdminIP[i] != 0 && pAdminIP[i] == ip)
			{
				user = i;
				break;
			}
		}
	}

	if (user < 0)
	{
		char temp[256];
		snprintf(temp, sizeof(temp),
			"err, acceptadmin request accept from  %u.%u.%u.%u",
			cIP[0], cIP[1], cIP[2], cIP[3]);
		Log(temp, "-system", 0);
		return;
	}

	if (pAdmin[user].Mode != USER_EMPTY)
	{
		char temp[256];
		snprintf(temp, sizeof(temp),
			"err, acceptadmin not empty previous slot %d", user);
		Log(temp, "-system", 0);

		pAdmin[user].cSock.CloseSocket();
		pAdmin[user].Mode = USER_EMPTY;
		return;
	}

	pAdmin[user].Level = -1;
	pAdmin[user].DisableID = 0;
	pAdmin[user].IP = ip;
	pAdmin[user].Mode = USER_ACCEPT;
	pAdmin[user].Name[0] = 0;

	std::shared_ptr<net::Session> session = std::make_shared<net::Session>(
		g_runtime->Context(), std::move(socket), net::SessionMode::Framed, true);

	pAdmin[user].cSock.Attach(session);
	const std::uint64_t generation = session->Generation();

	session->SetFrameHandler(
		[user, generation](std::uint8_t *frame, std::size_t)
		{
			if (pAdmin[user].cSock.Generation() != generation)
				return;

			CurrentTime = platform::MonotonicMs();

			if (ProcessAdminMessage(user, reinterpret_cast<char *>(frame)) == FALSE)
			{
				pAdmin[user].cSock.CloseSocket();
				pAdmin[user].Mode = USER_EMPTY;
				pAdmin[user].Name[0] = 0;
			}
		});

	session->SetCloseHandler(
		[user, generation]()
		{
			if (pAdmin[user].cSock.Generation() != generation)
				return;

			pAdmin[user].cSock.Detach();
			pAdmin[user].Mode = USER_EMPTY;
			pAdmin[user].Name[0] = 0;
		});

	session->SetErrorHandler(
		[user](const char *reason)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,admin link %d: %s", user,
				reason != nullptr ? reason : "");
			Log(temp, "-system", 0);
		});

	session->Start();

	pAdmin[user].Encode1 = rand() % 10000;
	pAdmin[user].Encode2 = rand() % 10000;

	MSG_NPIDPASS message;
	memset(&message, 0, sizeof(message));

	message.Type = _MSG_NPReqIDPASS;
	message.Size = sizeof(MSG_NPIDPASS);
	message.ID = 0;
	message.Encode1 = pAdmin[user].Encode1;
	message.Encode2 = pAdmin[user].Encode2;

	pAdmin[user].cSock.SendOneMessage(
		reinterpret_cast<char *>(&message), sizeof(MSG_NPIDPASS));
}

void ConnectRedirect()
{
	FILE *fp = fopen("redirect.txt", "rt");

	if (fp == NULL)
		return;

	char host[256];
	int port = 0;

	if (fscanf(fp, "%s %d %s %s", host, &port, adminclientid, adminclientpass) != 4)
	{
		fclose(fp);
		return;
	}

	fclose(fp);

	g_adminConnector = std::make_shared<net::Connector>(g_runtime->Context());

	g_adminConnector->Connect(
		host, static_cast<unsigned short>(port), "0.0.0.0", true,
		[](asio::ip::tcp::socket socket)
		{
			std::shared_ptr<net::Session> session = std::make_shared<net::Session>(
				g_runtime->Context(), std::move(socket),
				net::SessionMode::Framed, false);

			AdminClient.Attach(session);

			session->SetFrameHandler(
				[](std::uint8_t *frame, std::size_t)
				{
					ProcessAdminClientMessage(reinterpret_cast<char *>(frame));
				});

			session->SetCloseHandler(
				[]()
				{
					Log("adminclient closed", "-system", 0);
					AdminClient.Detach();
				});

			session->SetErrorHandler(
				[](const char *reason)
				{
					char temp[256];
					snprintf(temp, sizeof(temp), "err,adminclient: %s",
						reason != nullptr ? reason : "");
					Log(temp, "-system", 0);
				});

			session->Start();

			TransperCharacter = 1;
			Log("sys redirection server connected", "-system", 0);
		},
		[](const std::string &error)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,redirection connect: %s",
				error.c_str());
			Log(temp, "-system", 0);
		});
}

void ConsoleCommand(const std::string &command)
{
	if (command == "status")
	{
		std::printf("%s", g_statusText.c_str());
		std::fflush(stdout);
	}
	else if (command == "readmobbase")
	{
		LoadBaseMobSet();
		Log("reload mobbase", "-system", 0);
	}
	else if (command == "quit" || command == "exit")
	{
		g_runtime->RequestShutdown();
	}
	else
	{
		std::printf("commands: status, readmobbase, quit\n");
		std::fflush(stdout);
	}
}

void Shutdown()
{
	if (ListenSocket)
		ListenSocket->Close();

	if (AdminSocket)
		AdminSocket->Close();

	for (int i = 0; i < MAX_SERVER; ++i)
	{
		if (pUser[i].cSock.IsOpen())
			pUser[i].cSock.CloseSocket();
	}

	for (int i = 0; i < MAX_ADMIN; ++i)
	{
		if (pAdmin[i].cSock.IsOpen())
			pAdmin[i].cSock.CloseSocket();
	}

	if (AdminClient.IsOpen())
		AdminClient.CloseSocket();

	WriteConfig();
	CReadFiles::WriteGuildInfo();
	WriteTransGuildInfo();

	DayLog_ExpLog();
	DayLog_ItemLog();

	if (fLogFile != NULL)
	{
		fclose(fLogFile);
		fLogFile = NULL;
	}
}

} // namespace

int main()
{
	platform::SetWorkingDirectoryToExecutable();

	BASE_InitializeBaseDef();

	if (!LoadBaseMobSet())
		return EXIT_FAILURE;

	StartLog("A");
	platform::SetDiagnosticSink(PlatformDiagnosticSink);
	Log("start log", "-system", 0);

	memset(g_pGuildWar, 0, sizeof(g_pGuildWar));
	memset(g_pGuildAlly, 0, sizeof(g_pGuildAlly));

	if (!ReadConfig())
		return EXIT_FAILURE;

	BASE_InitializeServerList();

	const std::vector<std::string> localAddresses = net::LocalIPv4Addresses();
	std::vector<std::string> groupAddresses;

	for (int i = 0; i < MAX_SERVERGROUP; ++i)
		groupAddresses.push_back(g_pServerList[i][0]);

	const net::ServerGroupMatch match =
		net::MatchServerGroup(localAddresses, groupAddresses);

	if (match.group < 0)
	{
		platform::ReportFatal(hWndMain,
			"Can't get Server Group Index. LocalIP / ServerList.txt",
			"BOOTING ERROR", 0);
		return EXIT_FAILURE;
	}

	ServerIndex = match.group;
	sscanf(match.address.c_str(), "%hhu.%hhu.%hhu.%hhu",
		&LocalIP[0], &LocalIP[1], &LocalIP[2], &LocalIP[3]);

	CReadFiles::ImportItem();
	ReadTrandGuildInfo();

	g_runtime = std::make_unique<host::Runtime>();

	std::string listenError;

	ListenSocket = std::make_shared<net::Acceptor>(g_runtime->Context());

	if (!ListenSocket->Listen("0.0.0.0", DB_PORT, listenError))
	{
		platform::ReportFatal(hWndMain, listenError.c_str(),
			"DB listener", 0);
		return EXIT_FAILURE;
	}

	ListenSocket->Start(
		AcceptGameConnection,
		[](const std::string &error)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,db accept: %s", error.c_str());
			Log(temp, "-system", 0);
		});

	AdminSocket = std::make_shared<net::Acceptor>(g_runtime->Context());

	if (!AdminSocket->Listen("0.0.0.0", ADMIN_PORT, listenError))
	{
		platform::ReportFatal(hWndMain, listenError.c_str(),
			"admin listener", 0);
		return EXIT_FAILURE;
	}

	AdminSocket->Start(
		AcceptAdminConnection,
		[](const std::string &error)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,admin accept: %s", error.c_str());
			Log(temp, "-system", 0);
		});

	ConnectRedirect();
	CReadFiles::ReadGuildInfo();

	g_runtime->ScheduleTick(
		std::chrono::milliseconds(1000),
		[]()
		{
			CurrentTime = platform::MonotonicMs();
			ProcessSecTimer();
		});

	g_runtime->SetShutdownHandler(Shutdown);
	g_runtime->StartSignals();
	g_runtime->StartConsole(ConsoleCommand);

	g_statusText = BuildStatusText();
	Log("db server listening", "-system", 0);

	g_runtime->Run();

	Shutdown();

	// Drop every reference that points into the io_context before destroying it.
	AdminClient.Detach();

	for (int i = 0; i < MAX_SERVER; ++i)
		pUser[i].cSock.Detach();

	for (int i = 0; i < MAX_ADMIN; ++i)
		pAdmin[i].cSock.Detach();

	ListenSocket.reset();
	AdminSocket.reset();
	g_adminConnector.reset();
	g_runtime.reset();

	return EXIT_SUCCESS;
}
