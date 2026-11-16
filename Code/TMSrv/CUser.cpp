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
#include "CUser.h"
#include "Server.h"


CUser::CUser()
{	
	Mode		     = USER_EMPTY;
	Unk3			 = 0;
	IsBillConnect    = 0;
	LastReceiveTime  = 0;

	memset(Cargo, 0, sizeof(Cargo));

	Admin			 = 0;
	CastleStatus	 = 0;
	MuteChat		 = 0;
	UseItemTime		 = 0;
	Message			 = 0;
	AttackTime		 = 0;
	LastClientTick	 = 0;
	PotionTime		 = 0;

	OnlyTrade		 = 0;
}

CUser::~CUser()
{

}

int CUser::CloseUser()
{
	// Graceful close: the session flushes its queue and closes on its own,
	// while the slot is released immediately so it can be reused.
	cSock.CloseSocket();
	cSock.Detach();

	IsBillConnect = 0;

	Mode = USER_EMPTY;

	AccountName[0] = 0;

	return TRUE;
}
