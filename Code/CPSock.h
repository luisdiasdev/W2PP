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

#ifndef _CPSOCK_ // Last updated august 2012
#define _CPSOCK_ 

#include <winsock2.h>
#include <Windows.h>

#include "net/Protocol.h"

#define WSA_READ            (WM_USER + 100)
#define WSA_READDB          (WM_USER + 2) 
#define WSA_ACCEPT          (WM_USER + 3) 
#define WSA_READBILL		(WM_USER + 4)
#define WSA_ACCEPTADMIN     (WM_USER + 5) 
#define WSA_READADMIN		(WM_USER + 6) 
#define WSA_READADMINCLIENT	(WM_USER + 7) 

class  CPSock
{   
public:
	unsigned int  Sock;
	char   *pSendBuffer;
	char   *pRecvBuffer;
	int		nSendPosition;
	int		nRecvPosition;
	int		nProcPosition;
	int		nSentPosition;
	int     Init;	

public:
	CPSock();
	~CPSock();

	BOOL	CloseSocket			();
	BOOL	WSAInitialize		();
	SOCKET	StartListen			(HWND hWnd, int ip, int Port, int WSA);
	SOCKET	ConnectServer		(char *HostAddr, int Port, int ip, int WSA);
	SOCKET  ConnectBillServer	(char *HostAddr, int Port, int ip, int WSA);

	BOOL	Receive				();
	char   *ReadMessage			(int *ErrorCode, int *ErrorType);
	
	BOOL	AddMessage			(char *pMsg, int Size);
	BOOL	SendMessageA		();
	BOOL	SendOneMessage		(char* Msg, int Size);
	
	void	RefreshRecvBuffer	(void);
	void	RefreshSendBuffer	(void);
	BOOL	SendBillMessage		(char * Msg);
	char   *ReadBillMessage		(int *ErrorCode, int *ErrorType);
};

#endif
