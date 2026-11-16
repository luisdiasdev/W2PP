/*
 * Headless TMSrv host.
 *
 * Owns the asio runtime, the game listener, the DB and billing links, slot
 * attachment and shutdown. The protocol handlers remain in Server.cpp,
 * ProcessDBMessage.cpp, ProcessClientMessage.cpp and the _MSG_* units.
 */

#include "Server.h"
#include "CCastleZakum.h"
#include "CReadFiles.h"
#include "CUser.h"
#include "Language.h"
#include "ProcessClientMessage.h"
#include "SendFunc.h"

#include "../host/Runtime.h"
#include "../net/Acceptor.h"
#include "../net/BillingLink.h"
#include "../net/Connector.h"
#include "../net/Protocol.h"
#include "../net/RetryLadder.h"
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

#ifdef _WIN32
#include <io.h>
#endif

namespace
{

constexpr int kDbAttempts = 2;
constexpr int kDbRetryDelayMs = 200;

std::unique_ptr<host::Runtime> g_runtime;
std::shared_ptr<net::Acceptor> g_gameAcceptor;
std::shared_ptr<net::Connector> g_dbConnector;
std::shared_ptr<net::RetryLadder> g_dbLadder;
std::unique_ptr<net::BillingLink> g_billingLink;

host::Runtime::TickId g_secondTick;
host::Runtime::TickId g_minuteTick;

bool g_servicesStarted = false;
bool g_shutdownDone = false;
bool g_bootFailure = false;

void PlatformDiagnosticSink(const char *line)
{
	char buffer[1024];
	strncpy(buffer, line != NULL ? line : "", sizeof(buffer) - 1);
	buffer[sizeof(buffer) - 1] = '\0';

	char systemTag[] = "-system";
	Log(buffer, systemTag, 0);
}

void PlatformStatusSink(const char *line)
{
	// The former window showed the last log line; headless runs echo it to the
	// console only when one is attached.
	bool attached = false;

#ifdef _WIN32
	attached = _isatty(_fileno(stdout)) != 0;
#endif

	const char *force = std::getenv("W2PP_FORCE_CONSOLE");

	if (force != nullptr && force[0] != '\0' && force[0] != '0')
		attached = true;

	if (!attached)
		return;

	std::fputs(line != NULL ? line : "", stdout);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

std::string LocalAddressString()
{
	char text[32];
	snprintf(text, sizeof(text), "%u.%u.%u.%u",
		LocalIP[0], LocalIP[1], LocalIP[2], LocalIP[3]);
	return text;
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

void AcceptGameConnection(asio::ip::tcp::socket socket, const std::string &peer);

void ConnectBilling()
{
	if (g_billingLink == nullptr)
		return;

	if (BillServerPort <= 0 || BillServerAddress[0] == 0)
		return;

	if (BillServerSocket.IsOpen())
		return;

	BillServerAddress[sizeof(BillServerAddress) - 1] = 0;

	g_billingLink->Connect(BillServerAddress,
		static_cast<unsigned short>(BillServerPort), LocalAddressString());
}

void StartGameServices()
{
	if (g_servicesStarted)
		return;

	g_servicesStarted = true;

	std::string listenError;
	g_gameAcceptor = std::make_shared<net::Acceptor>(g_runtime->Context());

	if (!g_gameAcceptor->Listen(LocalAddressString(), GAME_PORT, listenError))
	{
		platform::ReportFatal(hWndMain, listenError.c_str(), "game listener", 0);
		return;
	}

	g_gameAcceptor->Start(
		AcceptGameConnection,
		[](const std::string &error)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,game accept: %s", error.c_str());
			Log(temp, "-system", 0);
		});

	GuildZoneReport();

	if (KefraLive == 0)
	{
		for (int k = KEFRA_MOB_INITIAL; k < KEFRA_MOB_END; k++)
			GenerateMob(k, 0, 0);

		GenerateMob(KEFRA_BOSS, 0, 0);
	}

	if (BILLING > 0)
		ConnectBilling();

	Log("game server listening", "-system", 0);
}

void OnDBExhausted()
{
	Log("reconnect DB fail.", "-system", 0);

	if (!g_servicesStarted)
	{
		g_bootFailure = true;
		platform::ReportWarning(hWndMain,
			"Não foi possivel se conectar a DBServer", "Erro ao iniciar", 0);
	}

	g_runtime->RequestShutdown();
}

void ConnectDBAttempt(int attempt)
{
	char temp[256];
	snprintf(temp, sizeof(temp), "sys,db connect attempt %d", attempt);
	Log(temp, "-system", 0);

	g_dbConnector = std::make_shared<net::Connector>(g_runtime->Context());

	g_dbConnector->Connect(
		DBServerAddress, static_cast<unsigned short>(DBServerPort),
		LocalAddressString(), true,
		[](asio::ip::tcp::socket socket)
		{
			net::SessionPtr session = std::make_shared<net::Session>(
				g_runtime->Context(), std::move(socket),
				net::SessionMode::Framed, false);

			DBServerSocket.Attach(session);
			const std::uint64_t generation = session->Generation();

			session->SetFrameHandler(
				[generation](std::uint8_t *frame, std::size_t)
				{
					if (!DBServerSocket.IsCurrent(generation))
						return;

#ifdef _PACKET_DEBUG
					MSG_STANDARD *debug = (MSG_STANDARD *)frame;

					if (BASE_CheckPacket(debug))
					{
						char temp[256];
						snprintf(temp, sizeof(temp),
							"**PACKET_DEBUG** Type:%d Size:%d",
							debug->Type, debug->Size);
						Log(temp, "-system", 0);

						return;
					}
#endif

					ProcessDBMessage(reinterpret_cast<char *>(frame));
				});

			session->SetCloseHandler(
				[generation]()
				{
					if (!DBServerSocket.IsCurrent(generation))
						return;

					DBServerSocket.Detach();

					if (g_runtime == nullptr || g_runtime->IsShuttingDown())
						return;

					Log("err,db read fail", "-system", 0);

					if (!g_dbLadder->IsActive())
						g_dbLadder->Start();
				});

			session->SetErrorHandler(
				[](const char *reason)
				{
					char temp[256];
					snprintf(temp, sizeof(temp), "err,db link: %s",
						reason != nullptr ? reason : "");
					Log(temp, "-system", 0);
				});

			session->Start();

			Log("sys,db server connected", "-system", 0);

			g_dbLadder->ReportSuccess();
			StartGameServices();
		},
		[](const std::string &error)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,db connect: %s", error.c_str());
			Log(temp, "-system", 0);

			g_dbLadder->ReportFailure();
		});
}

void AttachDBLadder()
{
	if (g_dbLadder->IsActive())
		return;

	g_dbLadder->Start();
}

void AcceptGameConnection(asio::ip::tcp::socket socket, const std::string &peer)
{
	const unsigned int ip = PeerIp(peer);
	int user = GetEmptyUser();

	if (user == 0)
	{
		Log("err,accept fail - no empty", "-system", 0);
		return;
	}

	pUser[user].IP = ip;
	pUser[user].Mode = USER_ACCEPT;
	pUser[user].Unk3 = 0;
	pUser[user].AccountName[0] = 0;

	net::SessionPtr session = std::make_shared<net::Session>(
		g_runtime->Context(), std::move(socket), net::SessionMode::Framed, true);

	pUser[user].cSock.Attach(session);
	const std::uint64_t generation = session->Generation();

	session->SetFrameHandler(
		[user, generation](std::uint8_t *frame, std::size_t)
		{
			if (!pUser[user].cSock.IsCurrent(generation))
				return;

#ifdef _PACKET_DEBUG
			MSG_STANDARD *debug = (MSG_STANDARD *)frame;

			if (BASE_CheckPacket(debug))
			{
				char temp[256];
				snprintf(temp, sizeof(temp), "**PACKET_DEBUG** Type:%d Size:%d",
					debug->Type, debug->Size);
				Log(temp, "-system", 0);
			}
#endif

			ProcessClientMessage(user, reinterpret_cast<char *>(frame), FALSE);
		});

	session->SetCloseHandler(
		[user, generation]()
		{
			if (!pUser[user].cSock.IsCurrent(generation))
				return;

			char temp[256];
			snprintf(temp, sizeof(temp), "clo,fd %d-%d", pUser[user].Mode,
				pMob[user].Mode);
			Log(temp, pUser[user].AccountName, pUser[user].IP);

			CloseUser(user);
		});

	session->SetErrorHandler(
		[user](const char *reason)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,client link %d: %s", user,
				reason != nullptr ? reason : "");
			Log(temp, pUser[user].AccountName, pUser[user].IP);
		});

	session->Start();

	if (user >= MAX_USER - ADMIN_RESERV)
	{
		SendClientMessage(user, g_pMessageStringTable[_NN_Reconnect]);

		CloseUser(user);
		return;
	}

	if (ServerDown != -1000)
	{
		SendClientMessage(user, g_pMessageStringTable[_NN_ServerReboot_Cant_Connect]);

		CloseUser(user);
		return;
	}
}

void Shutdown()
{
	if (g_shutdownDone)
		return;

	g_shutdownDone = true;

	if (g_gameAcceptor)
		g_gameAcceptor->Close();

	if (g_dbLadder)
		g_dbLadder->Stop();

	if (g_dbConnector)
		g_dbConnector->Cancel();

	if (BILLING != 0)
	{
		_AUTH_GAME sm;
		memset(&sm, 0, sizeof(sm));

		SendBilling2(&sm, 4);

		BILLING = 0;
	}

	if (g_billingLink)
		g_billingLink->Disconnect();

	for (int i = 0; i < MAX_USER; ++i)
	{
		if (pUser[i].cSock.IsOpen() || pUser[i].Mode != USER_EMPTY)
			pUser[i].CloseUser();
	}

	if (DBServerSocket.IsOpen())
		DBServerSocket.CloseSocket();

	DBServerSocket.Detach();
	BillServerSocket.Detach();

	CReadFiles::WriteGuild();

	if (fLogFile != NULL)
	{
		fclose(fLogFile);
		fLogFile = NULL;
	}

	if (fChatLogFile != NULL)
	{
		fclose(fChatLogFile);
		fChatLogFile = NULL;
	}

	if (fItemLogFile != NULL)
	{
		fclose(fItemLogFile);
		fItemLogFile = NULL;
	}
}

void ConsoleCommand(const std::string &command)
{
	if (command == "status")
	{
		std::printf("%s", BuildStatusText().c_str());
		std::fflush(stdout);
	}
	else if (command == "saveall")
	{
		SaveAll();
		Log("saveall complete", "-system", 0);
	}
	else if (command == "reboot")
	{
		ServerDown = 1;
	}
	else if (command == "readguild")
	{
		CReadFiles::ReadGuild();
		Log("guild info reloaded", "-system", 0);
	}
	else if (command == "readguildname")
	{
		BASE_InitializeGuildName();
		Log("guild names reloaded", "-system", 0);
	}
	else if (command == "readgameconfig")
	{
		ReadConfig();
		RefreshStatus(0);
		Log("game config reloaded", "-system", 0);
	}
	else if (command == "reloadmob")
	{
		mNPCGen.ReadNPCGenerator();
		Log("npc generator reloaded", "-system", 0);
	}
	else if (command == "quit" || command == "exit")
	{
		g_runtime->RequestShutdown();
	}
	else
	{
		std::printf("commands: saveall, reboot, readguild, readguildname, "
			"readgameconfig, reloadmob, status, quit\n");
		std::fflush(stdout);
	}
}

bool LoadLocalAddress()
{
	FILE *fp = fopen("localip.txt", "rt");

	if (fp == NULL)
	{
		platform::ReportFatal(hWndMain,
			"Não foi possivel encontrar o arquivo LocalIP.txt",
			"Erro ao iniciar", 0);
		return false;
	}

	char name[255];
	memset(name, 0, sizeof(name));

	if (fscanf(fp, "%254s", name) != 1)
	{
		fclose(fp);

		platform::ReportFatal(hWndMain,
			"Não foi possivel ler o arquivo LocalIP.txt",
			"Erro ao iniciar", 0);
		return false;
	}

	fclose(fp);

	int r1 = 0, r2 = 0, r3 = 0, r4 = 0;

	sscanf(name, "%d.%d.%d.%d", &r1, &r2, &r3, &r4);

	LocalIP[0] = static_cast<unsigned char>(r1);
	LocalIP[1] = static_cast<unsigned char>(r2);
	LocalIP[2] = static_cast<unsigned char>(r3);
	LocalIP[3] = static_cast<unsigned char>(r4);

	DBServerAddress[0] = 0;
	ServerGroup = -1;
	ServerIndex = -1;

	for (int i = 0; i < MAX_SERVERGROUP; i++)
	{
		for (int j = 1; j < MAX_SERVERNUMBER; j++)
		{
			if (!strcmp(g_pServerList[i][j], name))
			{
				strncpy(DBServerAddress, g_pServerList[i][0],
					sizeof(DBServerAddress) - 1);
				DBServerAddress[sizeof(DBServerAddress) - 1] = 0;

				ServerGroup = i;
				ServerIndex = j - 1;

				break;
			}
		}

		if (DBServerAddress[0] != 0)
			break;
	}

	if (DBServerAddress[0] == 0)
	{
		platform::ReportFatal(hWndMain,
			"Não foi possivel pegar o ServerGroup. LocalIP.txt / ServerList.txt",
			"Erro ao iniciar", 0);
		return false;
	}

	DBServerPort = DB_PORT;

	return true;
}

void ReadBillingAddress()
{
	FILE *fp = fopen("biserver.txt", "rt");

	if (fp == NULL)
		return;

	char host[64];
	int port = 0;

	if (fscanf(fp, "%63s %d", host, &port) == 2)
	{
		strncpy(BillServerAddress, host, sizeof(BillServerAddress) - 1);
		BillServerAddress[sizeof(BillServerAddress) - 1] = 0;
		BillServerPort = port;
	}

	fclose(fp);
}

} // namespace

bool ConnectBillingFromConfig()
{
	FILE *fp = fopen("biserver.txt", "rt");

	if (fp == NULL)
		return false;

	char host[64];
	int port = 0;

	if (fscanf(fp, "%63s %d", host, &port) != 2)
	{
		fclose(fp);
		return false;
	}

	fclose(fp);

	strncpy(BillServerAddress, host, sizeof(BillServerAddress) - 1);
	BillServerAddress[sizeof(BillServerAddress) - 1] = 0;
	BillServerPort = port;

	if (g_billingLink == nullptr)
		return false;

	return g_billingLink->Connect(BillServerAddress,
		static_cast<unsigned short>(BillServerPort), LocalAddressString());
}

void ProcessBillingRetry()
{
	if (g_billingLink == nullptr)
		return;

	if (!g_billingLink->Tick())
		return;

	char temp[256];
	snprintf(temp, sizeof(temp), "err,start reconnect BILL %u",
		platform::MonotonicMs());
	Log(temp, "-system", 0);
}

void BillingResetRetry()
{
	if (g_billingLink != nullptr)
		g_billingLink->ResetRetry();
}

void SetSecondTickInterval(int milliseconds)
{
	if (g_runtime == nullptr || !g_secondTick)
		return;

	g_runtime->SetTickInterval(g_secondTick,
		std::chrono::milliseconds(milliseconds));
}

void SetMinuteTickInterval(int milliseconds)
{
	if (g_runtime == nullptr || !g_minuteTick)
		return;

	g_runtime->SetTickInterval(g_minuteTick,
		std::chrono::milliseconds(milliseconds));
}

void RequestServerShutdown()
{
	if (g_runtime != nullptr)
		g_runtime->RequestShutdown();
}

int main()
{
	platform::SetWorkingDirectoryToExecutable();

	BASE_InitializeHitRate();

	StartLog();
	StartChatLog();

	platform::SetDiagnosticSink(PlatformDiagnosticSink);
	platform::SetStatusSink(PlatformStatusSink);

	memset(&g_pTreasure, 0, sizeof(g_pTreasure));

	ReadConfig();
	ReadLevelItemConfig();

	ConfigReady = 1;

	RefreshStatus(0);

	memset(g_pGuildWar, 0, sizeof(g_pGuildWar));
	memset(g_pGuildAlly, 0, sizeof(g_pGuildAlly));
	memset(GuildInfo, 0, sizeof(GuildInfo));
	memset(pMac, 0, sizeof(pMac));
	memset(pMobMerc, 0, sizeof(pMobMerc));

	Reboot();

	SetCastleDoor(1);

	if (!LoadLocalAddress())
		return EXIT_FAILURE;

	for (int i = 1; i < MAX_SERVERNUMBER; i++)
	{
		if (g_pServerList[ServerGroup][i][0] != 0)
			NumServerInGroup++;
	}

	if (NumServerInGroup <= 0)
		NumServerInGroup = 1;

	if (NumServerInGroup > 10)
		NumServerInGroup = 10;

	if (!strcmp(DBServerAddress, "192.168.0.153"))
		TESTSERVER = 1;

	if (!strcmp(DBServerAddress, "192.168.0.50"))
		LOCALSERVER = 1;

	ReadBillingAddress();

	SetColoseumDoor(1);
	SetColoseumDoor2(3);

	memset(FailAccount, 0, sizeof(FailAccount));

	CurrentTime = platform::MonotonicMs();

	CReadFiles::CReadFiles();
	CCastleZakum::ReadCastleQuest();

	g_runtime = std::make_unique<host::Runtime>();

	g_secondTick = g_runtime->ScheduleTickWithId(
		std::chrono::milliseconds(500),
		[]()
		{
			ProcessSecTimer();
		});

	g_minuteTick = g_runtime->ScheduleTickWithId(
		std::chrono::milliseconds(12000),
		[]()
		{
			ProcessMinTimer();
		});

	g_billingLink = std::make_unique<net::BillingLink>(
		g_runtime->Context(), BillServerSocket);

	g_billingLink->SetHandlers(
		[](char *record)
		{
			ProcessBILLMessage(record);
		},
		[]()
		{
			Log("sys,billing server connected", "-system", 0);

			_AUTH_GAME sm;
			memset(&sm, 0, sizeof(sm));

			SendBilling2(&sm, 4);
		},
		[](const char *reason)
		{
			char temp[256];
			snprintf(temp, sizeof(temp), "err,billing %s",
				reason != nullptr ? reason : "");
			Log(temp, "-system", 0);

			if (g_billingLink != nullptr && g_billingLink->IsDisabled())
			{
				Log("err,Reconnect BILL Server(x2) fail.", "-system", 0);

				BILLING = 0;
			}
		});

	DBServerPort = DB_PORT;

	g_dbLadder = std::make_shared<net::RetryLadder>(
		g_runtime->Context(), kDbAttempts,
		std::chrono::milliseconds(kDbRetryDelayMs));

	g_dbLadder->SetHandlers(
		ConnectDBAttempt,
		OnDBExhausted);

	g_runtime->SetShutdownHandler(Shutdown);
	g_runtime->StartSignals();
	g_runtime->StartConsole(ConsoleCommand);

	AttachDBLadder();

	Log("game server starting", "-system", 0);

	g_runtime->Run();

	Shutdown();

	// Drop every reference that points into the io_context before destroying it.
	for (int i = 0; i < MAX_USER; ++i)
		pUser[i].cSock.Detach();

	DBServerSocket.Detach();
	BillServerSocket.Detach();

	g_billingLink.reset();
	g_dbConnector.reset();
	g_dbLadder.reset();
	g_gameAcceptor.reset();
	g_runtime.reset();

	return g_bootFailure ? EXIT_FAILURE : EXIT_SUCCESS;
}
