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


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <vector>

#include "../Basedef.h"
#include "../ItemEffect.h"
#include "../net/LocalAddress.h"
#include "../platform/FileSystem.h"
#include "CUser.h"
#include "CFileDB.h"
#include "Server.h"
#include "CRanking.h"
#include "CReadFiles.h"

int ServerIndex = -1;
int CurrentTransInfo = 0;
int Sapphire = 8;
int MinCounter = 0;
int SecCounter = 0;
int HourCounter = 0;
int TransperCharacter = 0;
unsigned int CurrentTime = 0;
unsigned int LastSendTime = 0;

int UserConnection[MAX_SERVER] = {0,};

unsigned char LocalIP[4] = {0,};
unsigned int pAdminIP[MAX_ADMIN] = {0,};

int TransGuildConn[MAX_TRANS];
int TransGuildIndex[MAX_TRANS];

extern unsigned short g_pGuildWar[65536];
extern unsigned short g_pGuildAlly[65536];

extern char g_pServerList[MAX_SERVERGROUP][MAX_SERVERNUMBER][64];

char adminclientid[256];
char adminclientpass[256];

FILE *fLogFile;
FILE *fDayLogFile;

int LastLogDay = -1;
int LastDayLogDay = -1;

CUser pUser[MAX_SERVER];
CUser pAdmin[MAX_ADMIN];

CFileDB cFileDB;

STRUCT_MOB g_pBaseSet[MAX_CLASS];
STRUCT_ITEMLOG ItemDayLog[MAX_ITEMLIST];

std::string g_statusText;

std::string BuildStatusText()
{
	char line[512];
	std::string text;

	snprintf(line, sizeof(line), "server group %d local %u.%u.%u.%u",
		ServerIndex, LocalIP[0], LocalIP[1], LocalIP[2], LocalIP[3]);
	text += line;
	text += "\n";

	for (int i = 0; i < MAX_SERVER; i++)
	{
		if (pUser[i].Mode == USER_EMPTY && !pUser[i].cSock.IsOpen())
			continue;

		const unsigned char *ip = (const unsigned char *)&pUser[i].IP;
		snprintf(line, sizeof(line),
			"game %d ip %u.%u.%u.%u %s users %d",
			i, ip[0], ip[1], ip[2], ip[3],
			pUser[i].cSock.IsOpen() ? "open" : "closed", pUser[i].Count);
		text += line;
		text += "\n";
	}

	for (int i = 0; i < MAX_ADMIN; i++)
	{
		if (pAdmin[i].Mode == USER_EMPTY && !pAdmin[i].cSock.IsOpen())
			continue;

		const unsigned char *ip = (const unsigned char *)&pAdmin[i].IP;
		snprintf(line, sizeof(line),
			"admin %d ip %u.%u.%u.%u %s",
			i, ip[0], ip[1], ip[2], ip[3],
			pAdmin[i].cSock.IsOpen() ? "open" : "closed");
		text += line;
		text += "\n";
	}

	return text;
}
int ReadTrandGuildInfo()
{
	memset(TransGuildConn, 0, MAX_TRANS * sizeof(int));
	memset(TransGuildIndex, 0, MAX_TRANS * sizeof(int));

	FILE *fp = fopen("Trans.txt", "rt");

	if(fp == NULL)
		return false;

	int ret = fscanf(fp, "%d", &CurrentTransInfo);

	if(!ret)
		return false;

	int i = 0;

	for(; i < MAX_TRANS; i++)
	{
		int target = 0, conn = 0, index = 0;

		int ret = fscanf(fp, "%d %d %d", &target, &conn, &index);

		if(ret < 0)
			break;

		if(target != i + MAX_TRANS*2)
			break;

		TransGuildConn[i] = conn;
		TransGuildIndex[i] = index;
	}

	fclose(fp);

	CurrentTransInfo = i;

	return true;
}

int WriteTransGuildInfo()
{
	FILE *fp = fopen("Trans.txt", "wt");

	int ret = fprintf(fp, "%d\n", CurrentTransInfo);

	if(!ret)
		return false;

	for(int i = 0; i < CurrentTransInfo; i++)
	{
		int ret = fprintf(fp, "%d %d %d\n", i + MAX_TRANS*2, TransGuildConn[i], TransGuildIndex[i]);

		if(ret < 0)
			break;
	}

	fclose(fp);

	return true;
}

void ConvertGuildNumber(int group, struct STRUCT_ITEM *sour)
{
	unsigned short guildnumber = BASE_GetGuild(sour);

	int i = 0;

	for(; i < CurrentTransInfo && (TransGuildConn[i] != group || TransGuildIndex[i] != guildnumber); i++);

	if(i == CurrentTransInfo)
	{
		TransGuildConn[i]  = group;
		TransGuildIndex[i] = guildnumber;

		CurrentTransInfo++;

		WriteTransGuildInfo();
	}

	unsigned short targetguild = (i + MAX_TRANS*2);

	sour->stEffect[0].cEffect = EF_HWORDGUILD;
	sour->stEffect[0].cValue = HighByte(targetguild);
	sour->stEffect[1].cEffect = EF_LWORDGUILD;
	sour->stEffect[1].cValue = targetguild % 256;
}

int SendAdminSignal(int svr, unsigned short id, unsigned short signal)
{
	MSG_STANDARD sm;

	sm.Type = signal;
	sm.ID = id;
	sm.Size = sizeof(MSG_STANDARD);

	pAdmin[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	return true;
}

int SendAdminMessage(int svr, unsigned short id, char *message)
{
	MSG_NPNotice sm;

	sm.Type = _MSG_NPNotice;
	sm.ID = id;
	sm.Size = sizeof(MSG_NPNotice);

	strncpy(sm.String, message, MESSAGE_LENGTH);

	pAdmin[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	return true;
}

int SendAdminParm(int svr, unsigned short id, unsigned short signal, int parm)
{
	MSG_STANDARDPARM sm;

	sm.Type = signal;
	sm.ID = id;
	sm.Size = sizeof(MSG_STANDARDPARM);
	sm.Parm = parm;

	pAdmin[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	return true;
}

int SendAdminState(int svr, unsigned short id, unsigned short signal, char *pass)
{
	MSG_STANDARDPARM sm;

	sm.Type = signal;
	sm.ID	= id;
	sm.Size = sizeof(MSG_STANDARDPARM);

	if(pass[0] == '@') sm.Parm = 1;
	if(pass[0] == '_') sm.Parm = 2;
	if(pass[0] == '#') sm.Parm = 3;

	pAdmin[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	return true;
}

void ReadAdmin()
{
	char temp[256];

	sprintf(temp, "Admin.txt");

	FILE *fp = fopen(temp, "r");

	if(fp == NULL) 
		return;

	while(1)
	{
		int a, b, c, d;
		a = b = c = d = 0;

		char * ret = fgets((char*)temp, 127, fp);    

		if(ret == NULL) 
			break;

		int idx = -1;

		for(int i = 0; i < 255; i++) 
			if(temp[i] == '.')
				temp[i] = ' ';

		sscanf(temp, "%d %d %d %d %d", &idx, &a, &b, &c, &d);

		unsigned int ip = (d << 24) + (c << 16) + (b << 8) + a; 

		if (idx < 0 || idx >= MAX_ADMIN) 
			continue;

		pAdminIP[idx] = ip;
	};

	fclose(fp);
}

int ReadConfig()
{
	FILE *fp = fopen("Config.txt", "rt");
	
	if(fp == NULL) 
	{ 
		platform::ReportFatal("cant find Config.txt", "BOOTING ERROR");

		return false; 
	}

	fscanf(fp, "Sapphire %d\n", &Sapphire);
	fscanf(fp, "LastCapsule %hu\n", &LastCapsule);

	fclose(fp);

	return true;
}

int WriteConfig()
{
	FILE * fp= fopen("Config.txt", "wt");

	if(fp == NULL)
		return false;

	fprintf(fp, "Sapphire %d\n", Sapphire);
	fprintf(fp, "LastCapsule %d\n", LastCapsule);

	fclose(fp);

	return true;
}

int ProcessAdminClientMessage(char *msg)
{
	MSG_STANDARD *std = (MSG_STANDARD *)msg;

	if(!(std->Type & FLAG_NP2DB) || (std->ID < 0) || (std->ID >= MAX_DBACCOUNT))
	{	
		char temp[128];
		sprintf(temp, "err,adminclient_packet:%d ID:%d Size:%d KeyWord:%d", std->Type, std->ID, std->Size, std->KeyWord);

		Log(temp,"-system",0);

		return false;
	} 

    switch(std->Type)    
	{
		case _MSG_NPCreateCharacter_Reply:
		{	
			MSG_NPCreateCharacter_Reply *m = (MSG_NPCreateCharacter_Reply*)msg;

			char * account	= m->Account;
			int slot		= m->Slot;
			int result		= m->Result;
			int IdxName		= cFileDB.GetIndex(m->Account);
			int	svr			= IdxName / MAX_USER;
			int	id			= IdxName % MAX_USER;

			MSG_ReqTransper sm; 
			memset(&sm, 0, sizeof(sm));

			sm.Type = _MSG_ReqTransper; 
			sm.Size = sizeof(sm);
			sm.ID	= id;
			sm.Slot = slot;
			sm.Result = m->Result;

			EnableAccount(-1, account);

			if(m->Result == 0)//; //0: Success Delete Parm2
			{	
				STRUCT_ACCOUNTFILE file;
				memcpy(file.Info.AccountName, m->Account, ACCOUNTNAME_LENGTH);

				int ret = cFileDB.DBReadAccount(&file);

				if(ret == false)
				{	
					char tt[256]; 
					sprintf(tt, "etc tranper_FAILED!!!!!(read) %s %s", m->Account, m->Name);

					Log(tt, file.Info.AccountName, 0);

					if(IdxName)	
					{
						sm.Result = 4;
						pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);
					}

					return true;
				}

				char tt[256]; 
				sprintf(tt, "etc tranper_success(deleted) %s %s", m->Account, m->Name);

				Log(tt, file.Info.AccountName, 0);

				memset(&file.Char[slot], 0, sizeof(STRUCT_MOB));

				_strupr(file.Info.AccountName);

				ret = cFileDB.DBWriteAccount(&file);

	            if(ret == false)
				{	
					char tt[256]; 
					sprintf(tt, "etc tranper_FAILED!!!!!(write) %s %s", m->Account, m->Name);

					Log(tt, file.Info.AccountName, 0);

					if(IdxName)	
					{
						sm.Result = 4;	
						pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);
					}

					return true;
				}

				if(IdxName)
				{	
					pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);

					memset(&(cFileDB.pAccountList[IdxName].File.Char[slot]), 0, sizeof(STRUCT_MOB));
				}
			}
			if(result == 1) //1: Threre's Same Name Already
			{	
				if(IdxName)	
					pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);
			}	
			else if(result == 2) //2: No character space. delete first.
			{	
				if(IdxName)	
					pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);
			}	
			else if(result == 3) //3: That account is playing
			{	
				if(IdxName)	
					pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);
			}
		}
		break;	
	}
	
	return true;
}


void ProcessClientMessage(int conn, char *msg)
{
	MSG_STANDARD *std = (MSG_STANDARD *)msg;

	if (!(std->Type & FLAG_GAME2DB) || (std->ID < 0) || (std->ID > MAX_USER)) 
	{	
		char temp[256];

		MSG_STANDARD *m = (MSG_STANDARD *)msg;

		sprintf(temp, "err,packet Type:%d ID:%d Size:%d KeyWord:%d", m->Type, m->ID, m->Size, m->KeyWord);

		Log(temp,"-system",0);

		return;
	}

	cFileDB.ProcessMessage(msg, conn);
}

void DisableAccount(int conn, char *account, int Year, int YearDay)
{
	account[ACCOUNTNAME_LENGTH-1] = 0;
	account[ACCOUNTNAME_LENGTH-2] = 0;

	_strupr(account);

	STRUCT_ACCOUNTFILE file;
	memcpy(file.Info.AccountName, account, ACCOUNTNAME_LENGTH);

	int iret = cFileDB.DBReadAccount(&file); 

	if(iret == false) 
	{	
		if(conn >= 0) 
			SendAdminSignal(conn, 0, _MSG_NPNotFound);

		if(conn >= 0) 
			SendAdminMessage(conn, 0, "There's no account with that account name");

		return;
	}
	
	if(file.Info.YearDay != 0 && file.Info.Year != 0) 
	{	
		if(conn >= 0) 
			SendAdminState(conn, 0, _MSG_NPState, "1");

		return;
	}

	file.Info.AccountPass[ACCOUNTPASS_LENGTH-1] = 0;
	file.Info.AccountPass[ACCOUNTPASS_LENGTH-2] = 0;

	file.Info.Year = Year;
	file.Info.YearDay = YearDay;

	cFileDB.DBWriteAccount(&file);

	if(conn >= 0)
	{	
		int idx = pAdmin[conn].DisableID;

		MSG_NPReqAccount sm; 
		memset(&sm, 0, sizeof(MSG_NPReqAccount));

		sm.Type = _MSG_NPReqAccount; 
		sm.Size = sizeof(MSG_NPReqAccount);
		sm.ID = idx;

		strncpy(sm.Account, account, ACCOUNTNAME_LENGTH);

		sm.Char[0] = 0;

		ProcessAdminMessage(conn, (char*)&sm);

		pAdmin[conn].DisableID = 0;
	}
}

void EnableAccount(int conn, char *account)
{
	account[ACCOUNTNAME_LENGTH-1] = 0;
	account[ACCOUNTNAME_LENGTH-2] = 0;

	_strupr(account);

	STRUCT_ACCOUNTFILE file;
  	memcpy(file.Info.AccountName, account, ACCOUNTNAME_LENGTH);

	int iret = cFileDB.DBReadAccount(&file);  

	if(iret == false) 
	{	
		if(conn >= 0) 
			SendAdminSignal(conn, 0, _MSG_NPNotFound);

		if(conn >= 0) 
			SendAdminMessage(conn, 0, "There's no account with that account name");

		return;
	}

	if(file.Info.Year == 0 && file.Info.YearDay == 0) 
	{	
		if(conn >= 0) 
			SendAdminState(conn, 0, _MSG_NPState, file.Info.AccountPass);

		return;
	}

	file.Info.AccountPass[ACCOUNTPASS_LENGTH-1] = 0;
	file.Info.AccountPass[ACCOUNTPASS_LENGTH-2] = 0;
	file.Info.Year = 0;
	file.Info.YearDay = 0;

	cFileDB.DBWriteAccount(&file);

	if(conn >= 0)
		SendAdminState(conn, 0, _MSG_NPState, file.Info.AccountPass);

	return ;
}

int ProcessAdminMessage(int conn, char *msg)
{
	MSG_STANDARD *std = (MSG_STANDARD *)msg;

	if(!(std->Type & FLAG_NP2DB) || (std->ID < 0) || (std->ID >= MAX_DBACCOUNT))
	{	
		MSG_STANDARD *m = (MSG_STANDARD *)msg;

		char temp[128];
		sprintf(temp, "err,packet:%d ID:%d Size:%d KeyWord:%d", m->Type, m->ID, m->Size, m->KeyWord);

		Log(temp,"-system",0);

		return false;
	} 

	switch(std->Type)
	{
	case _MSG_NPCreateCharacter:
		{
			MSG_NPCreateCharacter *m = (MSG_NPCreateCharacter*)msg;

			MSG_NPCreateCharacter_Reply	sm; 
			memset(&sm, 0, sizeof(MSG_NPCreateCharacter_Reply));
			sm.Type = _MSG_NPCreateCharacter_Reply;
			sm.Size		= sizeof(MSG_NPCreateCharacter_Reply);
			sm.Slot		= m->Slot;
			sm.ID		= m->ID;
			sm.Result	= 0;

			strncpy(sm.Account, m->Account, ACCOUNTNAME_LENGTH);
			strncpy(sm.Name, m->Mob.MobName, NAME_LENGTH);

			char *account	= m->Account;
			int slot		= m->Slot;
			int IdxName		= cFileDB.GetIndex(m->Account);

			if(IdxName != 0)	
			{
				sm.Result = 3;

				pAdmin[conn].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			STRUCT_ACCOUNTFILE file;

			memcpy(file.Info.AccountName, m->Account, ACCOUNTNAME_LENGTH);

			int ret = cFileDB.DBReadAccount(&file);

			if(ret == false)
			{	
				char tt[256]; 
				sprintf(tt, "etc,tranper_FAILED!!!!!(read) %s %s", m->Account, m->Mob.MobName);
				Log(tt, file.Info.AccountName, 0);

				sm.Result = 4;

				pAdmin[conn].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			int empty;

			for(empty = 0; empty < MOB_PER_ACCOUNT; empty++) 
			{	
				if(file.Char[empty].MobName[0] == 0) 
					break;
			}

			if(empty == MOB_PER_ACCOUNT)	
			{
				sm.Result = 2;
				
				pAdmin[conn].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			ret = cFileDB.CreateCharacter(m->Account, m->Mob.MobName);

			if(ret == false)
			{	
				sm.Result = 1;

				pAdmin[conn].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			for(int i = 0; i < MAX_EQUIP + MAX_CARRY -1 ; i++)
			{
				STRUCT_ITEM *sour;

				if(i < 16)
					sour = &m->Mob.Equip[i];
				else
					sour = &m->Mob.Carry[i];

				int sidx = sour->sIndex;

				if(sidx == 446 || (sidx >= 508 && sidx <= 509) || (sidx >= 526 && sidx <= 537) || sidx == 522)
					ConvertGuildNumber(conn, sour);
			}
	
			char tt[256]; 
			sprintf(tt, "etc,tranper_success(deleted) %s %s", m->Account, m->Mob.MobName);
			Log(tt, file.Info.AccountName, 0);

			memcpy(&file.Char[empty], &(m->Mob), sizeof(STRUCT_MOB));

			_strupr(file.Info.AccountName);

			ret = cFileDB.DBWriteAccount(&file);

			if(ret == false)
			{	
				char tt[256]; 
				sprintf(tt, "etc,tranper_FAILED!!!!!(write) %s %s", m->Account, m->Mob.MobName);
				Log(tt, file.Info.AccountName, 0);

				sm.Result = 4;
				
				pAdmin[conn].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			sm.Result = 0;
			
			pAdmin[conn].cSock.SendOneMessage((char*)&sm, sm.Size);

		} break;

	case _MSG_NPNotice:
		{
			MSG_NPNotice *m = (MSG_NPNotice*)msg;

			m->AccountName[ACCOUNTNAME_LENGTH-1] = 0;
			m->AccountName[ACCOUNTNAME_LENGTH-2] = 0;
			m->String[MAX_NOTIFY_LENGTH-1] = 0;
			m->String[MAX_NOTIFY_LENGTH-2] = 0;

			if(m->AccountName[0] == 0 && m->Parm1 == 1)
			{
				if(pAdmin[conn].Level < 2)
					return true;

				for (int i = 0; i < MAX_SERVER; i++)
				{	
					if(pUser[i].Mode == USER_EMPTY)
						continue;

					if(pUser[i].cSock.IsOpen() == false)	
						continue;

					MSG_NPNotice sm;
					memset(&sm, 0, sizeof(MSG_NPNotice));

					sm.Type = _MSG_NPNotice;
					sm.ID = 0;
					sm.Size = sizeof(MSG_NPNotice);

					sm.Parm1 = 1;
					sm.Parm2 = 1;

					strncpy(sm.String, m->String, MAX_NOTIFY_LENGTH);

					pUser[i].cSock.SendOneMessage((char*)&sm, sizeof(sm));
				}

				return true;
			}

			if(pAdmin[conn].Level <= 0)
				return true;

			int IdxName = cFileDB.GetIndex(m->AccountName);

			if(IdxName == 0)
			{   
				SendAdminMessage(conn, 0, "Specified user not connected. can't send notice.");

				return true;
			}

			int svr = IdxName / MAX_USER;
			int id = IdxName % MAX_USER;

			if(svr < 0|| svr >= MAX_SERVER || id <= 0 || id >= MAX_USER)
			{	
				SendAdminMessage(conn, 0, "Wrong SVR and ID");

				return true;
			}

			MSG_NPNotice sm;
			memset(&sm, 0, sizeof(MSG_NPNotice));

			sm.Type = _MSG_NPNotice;
			sm.ID = id;
			sm.Size = sizeof(MSG_NPNotice);

			sm.Parm1 = 0;
			sm.Parm2 = 0;

			strncpy(sm.String, m->String, MAX_NOTIFY_LENGTH);

			pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

			return true;

		} break;

	case _MSG_NPIDPASS:
		{
			MSG_NPIDPASS *m = (MSG_NPIDPASS *) msg;

			_strupr(m->Account);

			if(pAdmin[conn].Encode1 != m->Encode1 || pAdmin[conn].Encode2 != m->Encode2) 
				return false;

			if(pAdmin[conn].Level != -1) 
				return true;

			STRUCT_ACCOUNTFILE file;

			memcpy(file.Info.AccountName, m->Account, ACCOUNTNAME_LENGTH);

			int iret = cFileDB.DBReadAccount(&file);  

			if(iret == false) 
				return true;

			if(file.Info.AccountPass[0] == '_') 
				return false;
			if(file.Info.AccountPass[0] == '@') 
				return false; 

			char *p = (char*)file.Info.AccountPass;

			iret = strncmp(m->Pass, p, ACCOUNTPASS_LENGTH);

			if(iret) 
				return false;

			int maxlevel = 0;

			for (int i = 0; i < MOB_PER_ACCOUNT; i++)
			{  
				if (file.Char[i].MobName[0] != 0 && file.Char[i].BaseScore.Level > maxlevel) 
					maxlevel = file.Char[i].BaseScore.Level;
			}

			if(maxlevel < 1000) 
				return false;

			int admin = maxlevel -  1000;

			pAdmin[conn].Level = admin;
			pAdmin[conn].DisableID = 0;

			strncpy(pAdmin[conn].Name, m->Account, ACCOUNTNAME_LENGTH);

			char temp[256];
			sprintf(temp, "sys,Admin Login Success - Level: %d", admin);
			Log(temp, pAdmin[conn].Name, pAdmin[conn].IP);  
			SendAdminMessage(conn, 0, temp);

		} break;

	case _MSG_NPReqAccount: 
		{
			MSG_NPReqAccount *m = (MSG_NPReqAccount*)msg;

			if(pAdmin[conn].Level <= 0)
				return true;

			_strupr(m->Account);
			_strupr(m->Char);

			if(m->Char[0] != 0)		
				cFileDB.GetAccountByChar(m->Account, m->Char); 


			if(m->Account[0] == 0)
			{
				SendAdminSignal(conn, 0, _MSG_NPNotFound);
				SendAdminMessage(conn, 0, "There's no account with that character name");

				return true;
			}  

			STRUCT_ACCOUNTFILE file;

			memcpy(file.Info.AccountName, m->Account, ACCOUNTNAME_LENGTH);

			int iret = cFileDB.DBReadAccount(&file);

			if(iret == false)
			{	
				SendAdminSignal(conn, 0, _MSG_NPNotFound);
				SendAdminMessage(conn, 0, "There's no account with that account name");

				return true;
			}

			MSG_NPAccountInfo sm; 
			memset(&sm, 0, sizeof(MSG_NPAccountInfo));

			sm.Type = _MSG_NPAccountInfo; 
			sm.Size = sizeof(MSG_NPAccountInfo);
			sm.ID = 0;
			sm.account = file; 

			int IdxName = cFileDB.GetIndex(m->Account);

			sm.Session = IdxName;

			file.Char[0].MobName[NAME_LENGTH-1] = 0;
			file.Char[0].MobName[NAME_LENGTH-2] = 0;
			file.Char[1].MobName[NAME_LENGTH-1] = 0;
			file.Char[1].MobName[NAME_LENGTH-2] = 0;
			file.Char[2].MobName[NAME_LENGTH-1] = 0;
			file.Char[2].MobName[NAME_LENGTH-2] = 0;
			file.Char[3].MobName[NAME_LENGTH-1] = 0;
			file.Char[3].MobName[NAME_LENGTH-2] = 0;
			file.Info.AccountName[ACCOUNTNAME_LENGTH-1] = 0;
			file.Info.AccountName[ACCOUNTNAME_LENGTH-2] = 0;

			if(pAdmin[conn].Level <= 0)
				return true;

			int maxlevel = 0;

			for(int i = 0; i < MOB_PER_ACCOUNT; i++)
			{
				if (file.Char[i].MobName[0] != 0 && file.Char[i].BaseScore.Level > maxlevel) 
					maxlevel = file.Char[i].BaseScore.Level;

			}

			int admin = maxlevel - 1000;

			sm.State = 0;

			char accountstate[256];
			memset(accountstate, 0, 255);

			if (sm.account.Info.AccountPass[0] == '@' || sm.account.Info.Year != 0) 
			{
				sprintf(accountstate, "Conta bloqueada.");
				sm.State = 1;
			}
			if (sm.account.Info.AccountPass[0] == '_') 
			{
				sprintf(accountstate, "Conta Defeituosa.");
				sm.State = 2;
			}
			if (sm.account.Info.AccountPass[0] == '#') 
			{
				sprintf(accountstate, "Conta Desativada.");
				sm.State = 3;
			}

			char temp[256]; 			
			sprintf(temp, "%s < %s | %s | %s | %s> - %s", file.Info.AccountName, file.Char[0].MobName, file.Char[1].MobName, file.Char[2].MobName, file.Char[3].MobName, accountstate);

			temp[MESSAGE_LENGTH-1] = 0;
			temp[MESSAGE_LENGTH-2] = 0;

			SendAdminMessage(conn, 0, temp);
			/*
			sm.account.Info.AccountPass[0] = rand() % 24 + 'A';
			sm.account.Info.AccountPass[1] = rand() % 24 + 'A';
			sm.account.Info.AccountPass[2] = rand() % 24 + 'A';
			sm.account.Info.AccountPass[3] = rand() % 24 + 'A';
			sm.account.Info.AccountPass[4] = 0;
			*/
			pAdmin[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_NPAccountInfo));

		} break;

	case _MSG_NPReqSaveAccount:
		{
			if(pAdmin[conn].Level <= 2)
			{
				SendAdminMessage(conn, 0, "Not allowed");

				return true;
			}

			MSG_NPAccountInfo *m = (MSG_NPAccountInfo*)msg;

			int IdxName = cFileDB.GetIndex(m->account.Info.AccountName);

			if(IdxName != 0)
			{	
				SendAdminMessage(conn, 0, "For saving, account should be disabled.");

				return true;
			}

			int maxlevel = 0;

			for(int i = 0; i < MOB_PER_ACCOUNT; i++)
			{

				if (m->account.Char[i].MobName[0] != 0 && m->account.Char[i].BaseScore.Level > maxlevel) 
					maxlevel = m->account.Char[i].BaseScore.Level;

			}

			int admin = maxlevel - 1000;

			if(maxlevel >= 2000)
			{
				if(admin > pAdmin[conn].Level)
				{
					SendAdminMessage(conn, 0, "Set admin level error.");

					return true;
				}

				if(pAdmin[conn].Level == admin && strcmp(pAdmin[conn].Name, m->account.Info.AccountName))
				{
					SendAdminMessage(conn, 0, "Set admin level error.");

					return true;
				}

			}

			STRUCT_ACCOUNTFILE tmpact;

			memcpy(tmpact.Info.AccountName, m->account.Info.AccountName, ACCOUNTNAME_LENGTH);

			int iret = cFileDB.DBReadAccount(&tmpact);

			if(!iret)
			{
				SendAdminMessage(conn, 0, "There's no account with that account name");

				return true;
			}

			memcpy(m->account.Info.AccountPass, tmpact.Info.AccountPass, ACCOUNTPASS_LENGTH);

			cFileDB.DBWriteAccount(&m->account);

			char temp[128]; 
			sprintf(temp, "Saving account [%s] success", m->account.Info.AccountName);
			SendAdminMessage(conn, 0, temp);

			sprintf(temp, "sys,%s written", m->account.Info.AccountName);

			Log(temp, pAdmin[conn].Name, pAdmin[conn].IP);

		} break;

	case _MSG_NPDisable:
		{
			if(pAdmin[conn].Level <= 2) 
				return true;

			MSG_NPEnable *m = (MSG_NPEnable*)msg;

			m->AccountName[ACCOUNTNAME_LENGTH-1] = 0;
			m->AccountName[ACCOUNTNAME_LENGTH-2] = 0;

			_strupr(m->AccountName);

			int IdxName = cFileDB.GetIndex(m->AccountName);

			if(IdxName == 0)
			{	
				DisableAccount(conn, m->AccountName, m->Year, m->YearDay);

				return true;
			}	
			else
			{	
				cFileDB.SendDBSavingQuit(IdxName, 1);
				pAdmin[conn].DisableID = IdxName;
				pAdmin[conn].Year = m->Year;
				pAdmin[conn].YearDay = m->YearDay;
			}

		} break;

	case _MSG_NPEnable:
		{
			if(pAdmin[conn].Level <= 2) 
				return true;

			MSG_NPEnable *m = (MSG_NPEnable*)msg;

			m->AccountName[ACCOUNTNAME_LENGTH-1] = 0;
			m->AccountName[ACCOUNTNAME_LENGTH-2] = 0;

			_strupr(m->AccountName);

			int IdxName = cFileDB.GetIndex(m->AccountName);

			if (IdxName != 0)
			{	
				SendAdminMessage(conn, 0, "Check session. already playing.");

				return true;
			}

			EnableAccount(conn, m->AccountName);	

		} break;

	case _MSG_NPDonate:
		{
			MSG_NPDonate *m = (MSG_NPDonate*)msg;

			char filename[256];
			char name[256];


			struct tm when;
			time_t now;
			time(&now);
			when = *localtime(&now);

			sprintf(name, "%02d%02d%04d%02d%02d%02d%02d", when.tm_mday, when.tm_mon, when.tm_year+1900, when.tm_hour, when.tm_min, when.tm_sec, rand()%255);
			sprintf(filename, "../../Common/ImportDonate/%s", name);

			FILE *fp = fopen(filename, "wt");

			if(fp == NULL)
				break;

			fprintf(fp, "%s %d", m->AccountName, m->Donate);

			fclose(fp);
		} break;
	}

	return true;

}

void ProcessSecTimer()
{
	CReadFiles::ImportUser();
	CReadFiles::UpdateUser();

	SecCounter++;

	if(SecCounter % 30 == 0) 
	{
		CReadFiles::ImportItem(); // Once every two minutes
		CReadFiles::ImportDonate();
	}

	g_statusText = BuildStatusText();

	if(SecCounter % 30 == 0)
	{
		CReadFiles::UpdateConnection();

		MinCounter++;

		if(MinCounter % 30 == 0)
		{
			CReadFiles::UpdateConnectionData();
			HourCounter++;
		}
	}

	if (SecCounter % 600 == 0)
	{
#pragma region Gerador de Ranking
		CReadFiles::WriteRanking();
#pragma endregion
	}
	struct tm when;
	time_t now;
	time(&now);
	when = *localtime(&now);

	if (when.tm_mday != LastLogDay)
		StartLog("A");

	if (when.tm_mday != LastDayLogDay)
	{
		DayLog_ItemLog();
		DayLog_ExpLog();
		LastDayLogDay = when.tm_mday;
	}
	if(when.tm_hour == 0 && when.tm_wday == 0 && when.tm_min == 0 && when.tm_sec == 0)
	{
		for(int i = 0; i < 65536;i++)
		{
			if(GuildInfo[i].Fame)
			{
				GuildInfo[i].Fame = 0;

				for(int j = 0; j < MAX_SERVER; j++)
				{
					if(pUser[j].cSock.IsOpen() == false)
						continue;

					if(pUser[j].Mode == 0)
						continue;

					cFileDB.SendGuildInfo(j, i);
				}
			}
		}

		CReadFiles::WriteGuildInfo();
	}
}

void ProcessMinTimer()
{

}

void StartLog(char *cccc)
{
	 char Temp[256];

     if(fLogFile != NULL)
	 {  
		 int ret = fclose(fLogFile);

		 if(ret)
			 Log("Logfile close fail!!", "-system", 0);
	 }

	 struct tm when;
	 time_t now;
	 time(&now); 
	 when = *localtime(&now);

	 sprintf(Temp, ".\\Log\\DB_%02d_%02d_%04d_%02d_%02d_%02d_%s.txt", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min, when.tm_sec, cccc);

   	 fLogFile = fopen(Temp, "a+");

	 LastLogDay = when.tm_mday;
}

void Log(char *str1, char *str2, unsigned int ip)
{
	struct tm when;
	time_t now;
	time(&now);
	when = *localtime(&now);

	char LogTemp[1024];

	unsigned char *cIP = (unsigned char*)&ip;

	//sprintf(LogTemp, "%2.2d%2.2d%2.2d,%2.2d%2.2d%2.2d,%8.8x", when.tm_year - 100, when.tm_mon + 1, when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec, ip);
	//sprintf(LogTemp, "%s,%s,%s \n", LogTemp, str2, str1);

	if (ip != 0)
		sprintf(LogTemp, "[%02d/%02d/%04d][%02d:%02d:%02d] IP: %d.%d.%d.%d", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min, when.tm_sec, cIP[0], cIP[1], cIP[2], cIP[3]);
	else
		sprintf(LogTemp, "[%02d/%02d/%04d][%02d:%02d:%02d] ", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min, when.tm_sec);
	sprintf(LogTemp, "%s %s %s \n", LogTemp, str2, str1);

	if (fLogFile)
	{
		fprintf(fLogFile, LogTemp);

		// Headless operators read the log while the process runs.
		fflush(fLogFile);
	}

	platform::PublishStatus(LogTemp);
}

void DayLog_ExpLog()
{
	FILE *fp = NULL;

	char Temp[1024];
	// char Temp2[1024];

	struct tm when;
	time_t now;
	time(&now);
	when = *localtime(&now);

	sprintf(Temp, ".\\DayLog\\EXP_DB_%02d_%02d_%04d_%02d_%02d_%02d.txt", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min, when.tm_sec);

	fp = fopen(Temp, "a+");

	if (fp == NULL)
	{
		Log("err,daylog item open fail!!", "-system", 0);
		return;
	}

	DayLog_ReadAccountInDir("./account/*", fp);

	fclose(fp);
}



int GetEmptyUser()
{
	for(int i = 0; i < MAX_SERVER; i++)
	{   
		if(pUser[i].Mode == USER_EMPTY) 
			return i;
	}

	return -1;
}

void DayLog_ReadAccountInDir(char *dir, FILE *log)
{
	char tmp[1024];

	platform::DirectoryIterator iterator(dir);
	platform::DirectoryEntry entry;

	while (iterator.Next(entry))
	{
		if (entry.isDirectory)
		{
			sprintf(tmp, "./account/%s/*", entry.name.c_str());
			DayLog_ReadAccountInDir(tmp, log);
		}
		else
		{
			const platform::LocalTime sysTime = platform::LocalNow();

			// Only use fresh active accounts 
			if (entry.lastWriteTime.year != sysTime.year ||
			    entry.lastWriteTime.month != sysTime.month)
				continue;

			sprintf(tmp, "%s", entry.path.c_str());

			errno = 0;
			FILE* fs;
			fopen_s(&fs, tmp, "rb");

			if (fs != NULL)
			{
				STRUCT_ACCOUNTFILE accBuffer;
				memset(&accBuffer, 0, sizeof(STRUCT_ACCOUNTFILE));

				fread(&accBuffer, sizeof(STRUCT_ACCOUNTFILE), 1, fs);

				struct tm when;
				time_t now;
				time(&now);
				when = *localtime(&now);

				for (int i = 0; i < MOB_PER_ACCOUNT; i++)
				{
					if (accBuffer.Char[i].MobName[0] == '\0')
						continue;

					if (accBuffer.mobExtra[i].DayLog.YearDay == when.tm_yday-1)
						fprintf(log, "[%02d/%02d/%04d][%02d:%02d:%02d] %s Char:%s recebeu %llu exp hoje.\n", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min, when.tm_sec, accBuffer.Info.AccountName, accBuffer.Char[i].MobName, accBuffer.mobExtra[i].DayLog.Exp);
					
				}

				fclose(fs);
			}
			else
			{
				// TODO: Cant open file to read
			}
		}
	}
}

void DayLog_ItemLog()
{
	FILE *fp = NULL;

	char Temp[1024];

	struct tm when;
	time_t now;
	time(&now);
	when = *localtime(&now);

	sprintf(Temp, ".\\DayLog\\ITEM_DB_%02d_%02d_%04d_%02d_%02d_%02d.txt", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min, when.tm_sec);

	fp = fopen(Temp, "a+");

	if (fp == NULL)
	{
		Log("err,daylog item open fail!!", "-system", 0);
		return;
	}

	for (int i = 0; i < MAX_ITEMLIST; i++)
	{
		if (ItemDayLog[i].Num == 0)
			continue;

		sprintf(Temp, "[%02d/%02d/%04d][%02d:%02d:%02d] ", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min, when.tm_sec);

		sprintf(Temp, "%s ItemID:%d ItemName:%s ItemNum:%d \n", Temp, i, g_pItemList[i].Name, ItemDayLog[i].Num);

		if (fp)
			fprintf(fp, Temp);

		ItemDayLog[i].Num = 0;
	}

	fclose(fp);
}

bool file_exists(char *s, ...)
{
	char buf[1024];

	va_list pArgList;

	va_start(pArgList, s);
	vsprintf(buf, s, pArgList);
	va_end(pArgList);

	FILE *fp = fopen(buf, "r");

	if (fp)
	{
		fclose(fp);
		return true;
	}

	return false;
}
