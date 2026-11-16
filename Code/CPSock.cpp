/*
*   Copyright (C) {2015}  {VK, Charles TheHouse}
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see [http://www.gnu.org/licenses/].
*
*   Contact at:
*/

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CPSock.h"
#include "Basedef.h"
#include "net/FrameCodec.h"

int ConnectPort = 0;

//Server.cpp
extern HWND			hWndMain;
extern unsigned int CurrentTime;
extern unsigned int LastSendTime;
void   Log (char *String1, char *String2 , unsigned int ip);


//////////////////////////////////////////////////////////////////////////
//
//             CPSock,  ~CPSock
//
// Receive buffer and transmit buffer is allocated.
// Send and receive buffer pointer is reset Bubba
//////////////////////////////////////////////////////////////////////////

CPSock::CPSock()
{	
	Sock = 0;
	Init = 0;
	pSendBuffer      = (char *)malloc(SEND_BUFFER_SIZE);
	pRecvBuffer      = (char *)malloc(RECV_BUFFER_SIZE);

	nSendPosition    = 0;
	nSentPosition    = 0;
	nRecvPosition    = 0;
	nProcPosition    = 0;	
}

CPSock::~CPSock()
{   
	if (pSendBuffer	!= NULL)    
		free(pSendBuffer);

    if (pRecvBuffer != NULL)   
		free(pRecvBuffer);
}

BOOL CPSock::CloseSocket()
{	
	nSendPosition    = 0;
	nSentPosition    = 0;
	nRecvPosition    = 0;
	nProcPosition    = 0;
	Init             = 0;

    if (Sock != 0) 
		closesocket(Sock);

	Sock = 0;

	return TRUE;
}

/////////////////////////////////////////////////////////////////////////
//
//                       WSAInitialize   
//
// WSA initialization
// Server, all clients are executed once the first
/////////////////////////////////////////////////////////////////////////


BOOL CPSock::WSAInitialize()
{    
	WSADATA WSAData;

     if (WSAStartup(MAKEWORD(1,1), &WSAData) != 0) 
		 return FALSE;

 	 return TRUE;
}

/////////////////////////////////////////////////////////////////////////
//
//							StartListen
//
// Listening socket (LSock) Listen to begin with.
// Connection should be connected to the WSA_ACCEPT.
// If successful LSock, a failure FALSE (0) is returned.
/////////////////////////////////////////////////////////////////////////

SOCKET CPSock::StartListen(HWND hWnd, int ip, int port, int WSA)
{      
	   
	SOCKADDR_IN	local_sin;
	char		Temp[256];
	SOCKET		tSock = socket(AF_INET, SOCK_STREAM, 0);

	if(tSock == INVALID_SOCKET) 
	{	
		platform::ReportFatal(hWnd, "Initialize socket fail", "ERROR", MB_OK);

		return FALSE;
	}
    
	gethostname(Temp, 256);
	local_sin.sin_family		= AF_INET;
	local_sin.sin_addr.s_addr	= ip;		//INADDR_ANY;
	local_sin.sin_port			= htons((unsigned short int)port);       

	if(bind(tSock, (struct sockaddr FAR *)&local_sin, sizeof(local_sin)) == SOCKET_ERROR)
	{	
		platform::ReportFatal(hWnd, "Binding fail", "ERROR", MB_OK);
		closesocket(tSock);

		return FALSE;
	}

	if(listen(tSock, MAX_PENDING_CONNECTS) < 0)
	{	
		platform::ReportFatal(hWnd, "Listen fail", "ERROR", MB_OK);
		closesocket(tSock);

		return FALSE;
	}

	if(WSAAsyncSelect(tSock, hWnd, WSA, FD_ACCEPT) > 0)
	{	
		platform::ReportFatal(hWnd, "WSAAsyncSelect fail", "ERROR", MB_OK);
		closesocket(tSock);

		return FALSE;
	}

	Sock = tSock;

	return tSock;
}

SOCKET CPSock::ConnectServer(char *HostAddr, int Port, int ip, int WSA)
{      
	SOCKADDR_IN InAddr;	//Connect
	SOCKADDR_IN local_sin; // Bind local IP

	memset(&InAddr, 0, sizeof(InAddr));
	memset(&local_sin,	0,	sizeof(local_sin));

	nSendPosition = 0;
	nSentPosition = 0;
	nRecvPosition = 0;
	nProcPosition = 0;

	if(Sock != 0) 
		CloseSocket();

	InAddr.sin_addr.s_addr	= inet_addr(HostAddr);
  	InAddr.sin_family		= AF_INET;
	InAddr.sin_port			= htons((unsigned short)Port);

	SOCKET tSock = socket(AF_INET, SOCK_STREAM, 0);

	if(tSock == INVALID_SOCKET) 
    {	
		platform::ReportWarning(NULL, "Initialize socket fail", "ERROR", MB_OK);
		return 0;
	}
	
    local_sin.sin_family		= AF_INET;
    local_sin.sin_addr.s_addr	= ip;
  	local_sin.sin_port			= 0;      

	if(bind(tSock, (struct sockaddr FAR *)&local_sin, sizeof(local_sin)) == SOCKET_ERROR)
    {	
		ConnectPort += 10;
		local_sin.sin_port = htons((unsigned short int)(ConnectPort + 5000));

		if(bind(tSock, (struct sockaddr FAR *)&local_sin, sizeof(local_sin)) == SOCKET_ERROR)
		{	
			ConnectPort += 10;
			local_sin.sin_port = htons((unsigned short int)(ConnectPort + 5000));

			if(bind(tSock, (struct sockaddr FAR *)&local_sin, sizeof(local_sin)) == SOCKET_ERROR)
			{	
				platform::ReportWarning(NULL, "Binding fail", "ERROR", MB_OK);
				closesocket(tSock);

				return 0;
			}
		}
	}

	if(tSock == INVALID_SOCKET)	
		return 0;

	if(connect(tSock, (PSOCKADDR)&InAddr, sizeof(InAddr)) < 0)       
	{	
		closesocket(tSock);
		Sock = 0;

		return 0;
	}

	if(WSAAsyncSelect(tSock, hWndMain, WSA, FD_READ | FD_CLOSE) > 0) 
	{	
		closesocket(tSock);
		Sock = 0;

		return 0;
	}

	Sock = tSock;

	unsigned int InitCode = INITCODE;
	send(tSock, (char*)&InitCode, 4, 0);

	Init = 1;

	return tSock;
}

SOCKET CPSock::ConnectBillServer(char *HostAddr, int Port, int ip, int WSA)
{
	char msg[256];	

	SOCKADDR_IN remote_sin;
	SOCKADDR_IN local_sin;
	SOCKET      tSock;

	nRecvPosition = 0;
	nSendPosition = 0;
	nProcPosition = 0;

    if(Sock != 0) 
		CloseSocket();
	
	tSock = socket(AF_INET, SOCK_STREAM, 0);

	if(tSock == INVALID_SOCKET) 
		return 0;

	local_sin.sin_family		= AF_INET;
	local_sin.sin_addr.s_addr	= ip;
	local_sin.sin_port			= 0;    

	if(bind(tSock, (struct sockaddr FAR *)&local_sin, sizeof(local_sin)) == SOCKET_ERROR)
	{	
		local_sin.sin_port = htons((unsigned short int)(ConnectPort + 6000));

		if(bind(tSock, (struct sockaddr FAR *)&local_sin, sizeof(local_sin)) == SOCKET_ERROR)
		{	
			ConnectPort += 10;
			local_sin.sin_port = htons((unsigned short int)(ConnectPort + 6000));

			if(bind(tSock, (struct sockaddr FAR *)&local_sin, sizeof(local_sin)) == SOCKET_ERROR)
			{	
				platform::ReportWarning(NULL, "Binding fail", "ERROR", MB_OK);	
				closesocket(tSock);

				return 0;
			}
		}
	}

	sprintf(msg, "sock:%d ip:%d.%d.%d.%d port:%d-%d", tSock, local_sin.sin_addr.S_un.S_un_b.s_b1, local_sin.sin_addr.S_un.S_un_b.s_b2, 
		local_sin.sin_addr.S_un.S_un_b.s_b3, local_sin.sin_addr.S_un.S_un_b.s_b4, local_sin.sin_port, ConnectPort); 

	ConnectPort++;

	remote_sin.sin_addr.s_addr	 = inet_addr(HostAddr);
	remote_sin.sin_family		 = AF_INET;
	remote_sin.sin_port			 = htons((unsigned short)Port);  

	if(connect(tSock, (PSOCKADDR)&remote_sin, sizeof(remote_sin)) < 0)       
	{	
		sprintf(msg, "sock:%d ip:%d.%d.%d.%d port:%d-%d", tSock, remote_sin.sin_addr.S_un.S_un_b.s_b1, remote_sin.sin_addr.S_un.S_un_b.s_b2, 
			remote_sin.sin_addr.S_un.S_un_b.s_b3, remote_sin.sin_addr.S_un.S_un_b.s_b4, remote_sin.sin_port, Port); 

		closesocket(tSock);
		Sock = 0;

		return 0;
	}

	sprintf(msg, "sock:%d ip:%d.%d.%d.%d port:%d-%d", tSock, remote_sin.sin_addr.S_un.S_un_b.s_b1, remote_sin.sin_addr.S_un.S_un_b.s_b2, 
		remote_sin.sin_addr.S_un.S_un_b.s_b3, remote_sin.sin_addr.S_un.S_un_b.s_b4, remote_sin.sin_port, Port);

    if(WSAAsyncSelect(tSock, hWndMain, WSA, FD_READ|FD_CLOSE) > 0) 
	{	
		closesocket(tSock);
		Sock = 0;

		return 0;
	}

	Sock = tSock;

	return tSock;
}

BOOL CPSock::Receive()
{
	int Rest = RECV_BUFFER_SIZE - nRecvPosition;
	int tReceiveSize = recv(Sock, (char*)(pRecvBuffer + nRecvPosition), Rest, 0);

	if(tReceiveSize == SOCKET_ERROR || tReceiveSize == Rest)
		return FALSE;

	nRecvPosition = nRecvPosition + tReceiveSize;

	return TRUE;				  
}

/*
	 nRecvPosition = Final do ultimo pacote
	 nProcPosition = Começo do ultimo pacote
*/
char* CPSock::ReadMessage(int *ErrorCode, int *ErrorType)
{
	*ErrorCode = 0;

	if(nProcPosition >= nRecvPosition)
	{	
		nRecvPosition = 0;
		nProcPosition = 0;

		return 0;
	}

	bool initSeen = (Init != 0);

	net::DecodeOutcome outcome = net::DecodeNextFrame(
		reinterpret_cast<unsigned char *>(pRecvBuffer) + nProcPosition,
		static_cast<size_t>(nRecvPosition - nProcPosition),
		true, initSeen);

	Init = initSeen ? 1 : 0;

	if (outcome.status == net::DecodeStatus::NeedMoreData)
	{
		nProcPosition += static_cast<int>(outcome.consumed);

		return 0;
	}

	if (outcome.status == net::DecodeStatus::InitMismatch)
	{
		unsigned int InitCode = 0;

		memcpy(&InitCode, pRecvBuffer + nProcPosition, sizeof(InitCode));

		*ErrorCode = 2;
		*ErrorType = static_cast<int>(InitCode);

		return 0;
	}

	if (outcome.status == net::DecodeStatus::BadSize)
	{
		nRecvPosition = 0;
		nProcPosition = 0;

		*ErrorCode = 2;
		*ErrorType = static_cast<int>(outcome.size);

		return 0;
	}

	// outcome.status is Frame or BadChecksum: the frame was extracted (and,
	// as before, transformed in place) even when its checksum is wrong.
	char *pMsg = reinterpret_cast<char *>(outcome.frame);

	nProcPosition += static_cast<int>(outcome.consumed);

	if(nRecvPosition <= nProcPosition) 
	{	
		nRecvPosition = 0;
		nProcPosition = 0;
	}

	if (outcome.status == net::DecodeStatus::BadChecksum)
	{
		*ErrorCode = 1;
		*ErrorType = static_cast<int>(outcome.size);
	}

	return pMsg;
}

char* CPSock::ReadBillMessage(int *ErrorCode, int *ErrorType)
{     
	 *ErrorCode = 0;
	 *ErrorType = 0; 

	 if (nProcPosition >= nRecvPosition)
	 { 
		 nRecvPosition = 0;
	     nProcPosition = 0;

		 return 0;
	 }
	 
	 if (nRecvPosition - nProcPosition < g_cGame) // 196
		 return 0;

	 char *pMsg = &(pRecvBuffer[nProcPosition]);

	 nProcPosition = nProcPosition + g_cGame;

	 if(nRecvPosition <= nProcPosition) 
	 {  
		 nRecvPosition = 0;
		 nProcPosition = 0;
	 }

	 return pMsg;
}

BOOL CPSock::SendBillMessage(char *Msg)
{ 
	if (nSendPosition + g_cGame >= SEND_BUFFER_SIZE ) 
		return FALSE;

	for (int i = 0; i < g_cGame; i++)
		pSendBuffer[nSendPosition + i] = Msg[i];

	 nSendPosition = nSendPosition + g_cGame;

	 BOOL Err = SendMessage();

     return Err;
}

BOOL CPSock::AddMessage(char *pMsg, int Size)
{
	char temp[256];
	HEADER *pSMsg = (HEADER *)pMsg;

	if(nSendPosition + Size >= SEND_BUFFER_SIZE)	
	{	
		sprintf(temp, "err,add buffer full %d %d %d %d", nSendPosition, Size, pSMsg->Type, Sock);
		Log(temp, "-system", 0);

		return FALSE;
	}

	// check socket valid
	if	(Sock <= 0)
	{
		sprintf(temp, "err,add buffer invalid %d %d %d %d", nSendPosition, Size, pSMsg->Type, Sock);
		Log(temp, "-system", 0);

		return FALSE;
	}
	
	unsigned char iKeyWord  =	rand()%256;
	pSMsg->Size				=	Size;
	pSMsg->KeyWord			=	iKeyWord;
	pSMsg->CheckSum			=	0;
	pSMsg->ClientTick		=	CurrentTime;
	LastSendTime			=	CurrentTime;

#ifdef _PACKET_DEBUG
	MSG_STANDARD *debug = (MSG_STANDARD*)pMsg;

	if(BASE_CheckPacket(debug))
	{
		sprintf(temp, "**PACKET_DEBUG** Type:%d Size:%d", debug->Type, debug->Size);
        Log(temp, "-system", 0);
	}
#endif 

	memcpy(pSendBuffer + nSendPosition, pMsg, Size);

	unsigned char CheckSum = net::EncodePayload(
		reinterpret_cast<unsigned char *>(pSendBuffer) + nSendPosition,
		static_cast<size_t>(Size), iKeyWord);

	pSMsg->CheckSum = CheckSum;
	pSendBuffer[nSendPosition + 3] = CheckSum;

	nSendPosition = nSendPosition + Size;
	
	return TRUE;
}

void CPSock::RefreshRecvBuffer()
{	
	int left = nRecvPosition - nProcPosition;
	if(left <= 0) 
		return;

	memcpy(pRecvBuffer, pRecvBuffer + nProcPosition, left);

	nProcPosition = 0;
	nRecvPosition -= left;
}

void CPSock::RefreshSendBuffer()
{	
	int left = nSendPosition - nSentPosition;
	if	(left <= 0) 
		return;

	memcpy(pSendBuffer, pSendBuffer + nSentPosition, left);

	nSentPosition = 0;
	nSendPosition -= left;
}

BOOL CPSock::SendMessageA()
{
	#ifdef _PACKET_DEBUG
	char temp[256] = { 0, };
	#endif 

	if (Sock <= 0)
	{
		nSendPosition = 0;
		nSentPosition = 0;

		return FALSE;
	}

	if	(nSentPosition > 0) 
		RefreshSendBuffer();

    if(nSendPosition > SEND_BUFFER_SIZE || nSendPosition < 0 || Sock < 0)
    {	
		#ifdef  _PACKET_DEBUG
		sprintf(temp, "err send1 %d %d %d", nSendPosition, nSentPosition, Sock);
		Log(temp, "-system", 0);
		#endif

		nSendPosition = 0;
		nSentPosition = 0;

		return FALSE;
	}

	if(nSentPosition > nSendPosition || nSentPosition >= SEND_BUFFER_SIZE || nSentPosition < 0)
	{	
		#ifdef  _PACKET_DEBUG
		sprintf(temp, "err send2 %d %d %d", nSendPosition, nSentPosition, Sock);
		Log(temp, "-system", 0);
		#endif

		nSendPosition = 0;
		nSentPosition = 0;
	}

	for(int i = 0; i < 1; i++)
	{
		int err = 0;
		int	Left = nSendPosition - nSentPosition;
		int tSend = send(Sock, pSendBuffer + nSentPosition, Left, 0);

		if	(tSend != SOCKET_ERROR)	
			nSentPosition = nSentPosition + tSend;	// Bytes transmitted
		else
			err = WSAGetLastError();	

		if(nSentPosition < nSendPosition || tSend == SOCKET_ERROR)
			continue;
		else
		{	
			nSendPosition = 0;
			nSentPosition = 0;

			return TRUE;
		}
	}

	if(nSendPosition > SEND_BUFFER_SIZE)
		return FALSE;

	return TRUE; 
}

BOOL CPSock::SendOneMessage(char* Msg, int Size)
{	
	AddMessage(Msg, Size);

	BOOL Err = SendMessageA();

	return Err;
}
