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
#include <time.h> 

#include "CFileDB.h"
#include "../net/Connection.h"
#include "../platform/FileSystem.h"
#include "CUser.h"
#include "Server.h"
#include "CRanking.h"
#include "CReadFiles.h"
#include "../Basedef.h"
#include "../storage/AccountRepository.h"
#include "../storage/Database.h"
#include "../storage/RecordCodec.h"
#include "../storage/Schema.h"
#include "../storage/StorageConfig.h"
#include "../security/CredentialBytes.h"

#include <algorithm>
#include <string>
#include <vector>


extern int ServerIndex; //Server.cpp
extern int Sapphire;
extern int TransperCharacter;

extern CUser pUser[MAX_SERVER]; // Server.cpp
extern CUser pAdmin[MAX_ADMIN]; // Server.cpp

extern net::Connection AdminClient;

extern STRUCT_MOB g_pBaseSet[MAX_CLASS]; // Server.cpp

extern int  ChargedGuildList[MAX_SERVER][MAX_GUILDZONE];
extern STRUCT_ITEMLOG ItemDayLog[MAX_ITEMLIST];

unsigned short	g_pGuildWar[65536] = {0,};
unsigned short	g_pGuildAlly[65536] = {0,};
STRUCT_GUILDINFO GuildInfo[65536];


CFileDB::CFileDB()
{
	for(int i = 0; i < MAX_DBACCOUNT; i++)
	{
		pAccountList[i].Login =  0;
        pAccountList[i].Slot  = -1;
		pAccountList[i].SecurePass = -1;

		pAccountList[i].Mac[0] = 0;
		pAccountList[i].Mac[1] = 0;
		pAccountList[i].Mac[2] = 0;
		pAccountList[i].Mac[3] = 0;

		pAccountList[i].AccountId = 0;
		pAccountList[i].State = 0;

		memset(&pAccountList[i].File, 0, sizeof(pAccountList[i].File));
	}
}

CFileDB::~CFileDB()
{
}

int CFileDB::AddAccount(char *id, char *pass, char *name, int ssn1, int ssn2, char *email, char *tel, char *addr, int bonus)
{
	char check[ACCOUNTNAME_LENGTH];

	strncpy(check, id, ACCOUNTNAME_LENGTH - 1);
	check[ACCOUNTNAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;

	if (!repository_)
	{
		Log("err addaccount storage not initialized", check, 0);
		return false;
	}

	storage::AccountRecord existing;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(storage::NameBytes(check, ACCOUNTNAME_LENGTH),
			&existing, &found, &error))
	{
		Log(error, check, 0);
		return false;
	}

	if (found)
		return false;

	STRUCT_ACCOUNTFILE file;

	memset(&file, 0, sizeof(STRUCT_ACCOUNTFILE));

	strncpy(file.Info.AccountName, id, ACCOUNTNAME_LENGTH - 1);
	strncpy(file.Info.AccountPass, pass, ACCOUNTPASS_LENGTH - 1);
	strncpy(file.Info.RealName, name, REALNAME_LENGTH - 1);
	strncpy(file.Info.Email, email, EMAIL_LENGTH - 1);
	strncpy(file.Info.Telephone, tel, TELEPHONE_LENGTH - 1);
	strncpy(file.Info.Address, addr, ADDRESS_LENGTH - 1);

	file.Info.NumericToken[0] = -1;
	file.Donate = 0;
	file.Info.SSN1 = ssn1;
	file.Info.SSN2 = ssn2;

	memset(file.ShortSkill, -1, 8);

	if(bonus != 0)
	{
		for(int i = 0; i < 100; i++)
		{
			if(i < 50)
				file.Cargo[i].sIndex = 401;
			else
				file.Cargo[i].sIndex = 406;
		}
	}

	int ret = DBWriteAccount(&file);

	if(ret == 0)
		return false;

	return true;
}

int CFileDB::UpdateAccount(char *id, char *pass)
{
	char check[ACCOUNTNAME_LENGTH];

	strncpy(check, id, ACCOUNTNAME_LENGTH - 1);
	check[ACCOUNTNAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;

	if (!repository_)
		return false;

	storage::AccountRecord record;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(storage::NameBytes(check, ACCOUNTNAME_LENGTH),
			&record, &found, &error))
	{
		Log(error, check, 0);
		return false;
	}

	if (!found)
		return false;

	if (!repository_->ChangePassword(record.account_id,
			security::NewCredential(pass, ACCOUNTPASS_LENGTH), &error))
	{
		Log(error, check, 0);
		return false;
	}

	int Idx = GetIndex(id);

	if (Idx != 0)
		memset(pAccountList[Idx].File.Info.AccountPass, 0, ACCOUNTPASS_LENGTH);

	return true;
}

int ProcessRecord(int conn, char * str)
{
	char file[256];

	struct tm when;
	time_t now;
	time(&now);
	when = *localtime(&now);

    sprintf(file,"RC%4.4d_%2.2d_%2.2d_%2.2d_%2.2d_%d2.2d_%2d.txt", when.tm_year-100, when.tm_mon+1, when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec, conn);
	
	char filetemp[236];
   
	sprintf(filetemp, "../../Common/record%d/%s", ServerIndex, file);
	
	FILE *fp = fopen(filetemp, "wt");

	if(fp == NULL)
		return false;
	
	fprintf(fp, "%s", str);
	fclose(fp);

	return true;
}

int CFileDB::ProcessMessage(char *Msg, int conn)
{
	MSG_STANDARD *std = (MSG_STANDARD*)Msg;

	switch(std->Type)
	{

	case _MSG_ReqTransper:
		{
			int IdxName = conn;

			int svr = IdxName / MAX_USER;
			int id = IdxName % MAX_USER;

			MSG_ReqTransper *m = (MSG_ReqTransper*)Msg;

			m->OldName[NAME_LENGTH-1] = 0;
			m->OldName[NAME_LENGTH-2] = 0;
			m->NewName[NAME_LENGTH-1] = 0;
			m->NewName[NAME_LENGTH-2] = 0;

			int slot = m->Slot;

			if((m->ID <= 0 || m->ID >= MAX_USER) || (slot < 0 || slot >= MOB_PER_ACCOUNT))
			{
				char tt[256];
				sprintf(tt, "err,transper id:%d slot:%d old:%s new:%s", m->ID, slot, m->OldName, m->NewName);
				Log(tt, "system", 0);

				MSG_ReqTransper sm;

				sm.Type = _MSG_ReqTransper;
				sm.Size = sizeof(MSG_ReqTransper);
				sm.ID = id;

				sm.Result = 4;

				if(IdxName)
					pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			int idx = GetIndex(conn, m->ID);

			if(TransperCharacter == 0 || AdminClient.IsOpen() == false)
			{
				Log("err,transper but not transper_server", pAccountList[idx].File.Info.AccountName, 0);

				MSG_ReqTransper sm;

				sm.Type = _MSG_ReqTransper;
				sm.Size = sizeof(MSG_ReqTransper);
				sm.ID = id;

				sm.Result = 4;

				if(IdxName != 0)
					pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			STRUCT_MOB *mob;

			mob = &pAccountList[idx].File.Char[slot];

			mob->MobName[NAME_LENGTH-1] = 0;
			mob->MobName[NAME_LENGTH-2] = 0;

			char slotname[NAME_LENGTH];

			strncpy(slotname, mob->MobName, NAME_LENGTH);
			strncpy(mob->MobName, m->NewName, NAME_LENGTH);

			mob->MobName[NAME_LENGTH-1] = 0;
			mob->MobName[NAME_LENGTH-2] = 0;

			UpcaseString(slotname);
			UpcaseString(m->OldName);

			if(strcmp(slotname, m->OldName))
			{
				char tt[256];

				sprintf(tt, "err,transper id:%d slot:%d old:%s current:%s", m->ID, slot, m->OldName, slotname);
				Log(tt, pAccountList[idx].File.Info.AccountName, 0);

				MSG_ReqTransper sm;

				sm.Type = _MSG_ReqTransper;
				sm.Size = sizeof(MSG_ReqTransper);
				sm.ID = id;

				sm.Result = 4;

				if(IdxName != 0)
					pUser[svr].cSock.SendOneMessage((char*)&sm, sm.Size);

				return true;
			}

			DisableAccount(-1, pAccountList[idx].File.Info.AccountName, 0, 0);

			MSG_NPCreateCharacter sm;

			sm.Type = _MSG_NPCreateCharacter;
			sm.Size = sizeof(MSG_NPCreateCharacter);
			sm.ID = id;

			sm.Slot = m->Slot;

			strncpy(sm.Account, pAccountList[idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

			sm.Mob = *mob;

			AdminClient.SendOneMessage((char*)&sm, sm.Size);

			return true;
		}

	case _MSG_GuildZoneReport:
		{
			MSG_GuildZoneReport *m = (MSG_GuildZoneReport*)Msg;

			int srv = m->ID;

			if(srv < 0 || srv >= MAX_SERVER)
				break;

			for(int i = 0; i < 5; i++)
				ChargedGuildList[srv][i] = m->Guild[i];

			MSG_GuildReport sm;

			sm.Type = _MSG_GuildReport;
			sm.Size = sizeof(MSG_GuildReport);
			sm.ID = 0;

			memcpy(sm.ChargedGuildList, ChargedGuildList, sizeof(ChargedGuildList));

			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen())
					pUser[i].cSock.SendOneMessage((char*)&sm, sizeof(MSG_GuildReport));
			}

		} break;

	case _MSG_War:
		{
			MSG_STANDARDPARM2 *m = (MSG_STANDARDPARM2*)Msg;

			struct tm when;
			time_t now;
			time(&now);
			when = *localtime(&now);

			int myguild = m->Parm1;

			char temp[128];
			sprintf(temp, "guild war request %d %d", m->Parm1, m->Parm2);
			Log(temp, "system", 0);

			if(myguild <= 0 || myguild >= 65536)
				break;

			g_pGuildWar[myguild] = m->Parm2;
			//g_pGuildWar[myguild] = when.tm_mday;

			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen())
					SendDBSignalParm2(i, 0, _MSG_War, m->Parm1, m->Parm2);
			}
			
		} break;

	case _MSG_GuildAlly:
		{
			MSG_GuildAlly *m = (MSG_GuildAlly*)Msg;

			struct tm when;
			time_t now;
			time(&now);
			when = *localtime(&now);

			char temp[256];

			int myguild = m->Guild;

			sprintf(temp, "guild ally request %d %d",  m->Guild, m->Ally);

			Log(temp, "-system", 0);

			if(myguild <= 0 || myguild >= 65536)
				break;

			g_pGuildAlly[myguild] = m->Ally;
			//g_pGuildAlly[myguild] = when.tm_mday;

			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen())
					SendDBSignalParm2(i, 0, _MSG_GuildAlly, m->Guild, m->Ally);
			}

		} break;

	case _MSG_GuildInfo:
		{
			MSG_GuildInfo *m = (MSG_GuildInfo*)Msg;

			int myguild = m->Guild;

			// char temp[256];


			if(myguild <= 0 || myguild >= 65536)
				break;

			GuildInfo[myguild] = m->GuildInfo;

			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen() == false)
					continue;

				if(pUser[i].cSock.IsOpen())
					SendGuildInfo(i, myguild);
			}

			CReadFiles::WriteGuildInfo();
		} break;

	case _MSG_DBUpdateSapphire:
		{
			MSG_STANDARDPARM *m = (MSG_STANDARDPARM*)Msg;

			if(m->Parm == 1)
				Sapphire *= 2;
			else
				Sapphire /= 2;

			if(Sapphire < 1)
				Sapphire = 1;
			else if(Sapphire > 64)
				Sapphire = 64;

			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen())
					SendDBSignalParm3(i, 0, _MSG_DBSetIndex, -1, Sapphire, i);
			}

			WriteConfig();

		} break;

	case _MSG_DBNewAccount:
		{
			MSG_NewAccount *m = (MSG_NewAccount*)Msg;

			int Idx = GetIndex(conn, m->ID);

			UpcaseString(m->AccountInfo.AccountName);

			char *accname = m->AccountInfo.AccountName;

			if((accname[0] == 'C' && accname[1] == 'O' && accname[2] == 'M' && accname[3] >= '0' && accname[3] <= '9' && accname[4] == 0) || 
				(accname[0] == 'L' && accname[1] == 'P' && accname[2] == 'T' && accname[3] >= '0' && accname[3] <= '9' && accname[4] == 0))
				return false;

			STRUCT_ACCOUNTFILE *file;

			file = &pAccountList[Idx].File;

			file->Info = m->AccountInfo;

			memset(file->Char, 0, sizeof(STRUCT_ACCOUNTFILE)-sizeof(STRUCT_ACCOUNTINFO));
			memset(file->Cargo, 0, sizeof(STRUCT_ITEM)*MAX_CARGO);

			file->Coin = 0;

			int ret = DBWriteAccount(file);

			if(ret == 0)
			{
				SendDBSignal(conn, m->ID, _MSG_DBNewAccountFail);

				return true;				
			}

			UpcaseString(file->Info.AccountName);

			AddAccountList(Idx);

			pAccountList[Idx].AccountId = GetAccountId(file->Info.AccountName);
			pAccountList[Idx].State = 0;

			MSG_DBCNFAccountLogin sm;

			sm.Type = _MSG_DBCNFAccountLogin;
			sm.ID = m->ID;
			sm.Size = sizeof(MSG_DBCNFAccountLogin);

			memcpy(sm.AccountName, file->Info.AccountName, ACCOUNTNAME_LENGTH);
			memset(sm.Cargo, 0, sizeof(STRUCT_ITEM)*MAX_CARGO);

			DBGetSelChar(&sm.sel, file);

			pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_DBCNFAccountLogin));

		} break;

	case  _MSG_MessageDBRecord:
		{
			MSG_MessageDBRecord *m = (MSG_MessageDBRecord*)Msg;

			m->Record[255] = 0;
			m->Record[254] = 0;

			int ret = ProcessRecord(conn, m->Record);

			Log(m->Record, "system", 0);

			if(ret == 0)
				Log("DBRecord Fail", "system", 0);

		} break;

	case _MSG_NPAppeal:
		{
			for(int i = 0; i < MAX_ADMIN; i++)
			{
				if(pAdmin[i].Mode == USER_EMPTY)
					continue;

				if(pAdmin[i].cSock.IsOpen())
					pAdmin[i].cSock.SendOneMessage((char*)std, std->Size);
			}

		} break;

	case _MSG_MessageDBImple:
		{
			MSG_MessageDBImple *m = (MSG_MessageDBImple*)Msg;

			m->ID = 0;

			m->String[MESSAGE_LENGTH-1] = 0;
			m->String[MESSAGE_LENGTH-2] = 0;

			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen())
					pUser[i].cSock.SendOneMessage((char*)m, sizeof(MSG_MessageDBImple));
			}
		} break;

	case _MSG_DBAccountLogin:
		{
			MSG_AccountLogin *m = (MSG_AccountLogin*)Msg;

			UpcaseString(m->AccountName);

			char *ac = m->AccountName;

			if((ac[0] == 'C' && ac[1] == 'O' && ac[2] == 'M' && ac[3] >= '0' && ac[3] <= '9' && ac[4] == 0) || 
				(ac[0] == 'L' && ac[1] == 'P' && ac[2] == 'T' && ac[3] >= '0' && ac[3] <= '9' && ac[4] == 0))
			{
				SendDBSignal(conn, m->ID, _MSG_DBAccountLoginFail_Account);

				return false;
			}

			int Idx = GetIndex(conn, m->ID);
			int IdxName = GetIndex(m->AccountName);

			if (!repository_)
			{
				Log("err,login storage not initialized", m->AccountName, 0);

				SendDBSignal(conn, m->ID, _MSG_DBAccountLoginFail_Account);

				return true;
			}

			storage::AccountRecord record;
			std::string storageError;
			bool tempKeyUsed = false;

			std::string password = security::SubmittedCredential(
				m->AccountPassword, ACCOUNTPASS_LENGTH);
			std::string tempKey(m->Zero, sizeof(m->Zero));

			storage::LoginStatus status = repository_->AdmitLogin(
				storage::NameBytes(m->AccountName, ACCOUNTNAME_LENGTH),
				password, tempKey, &record, &tempKeyUsed, &storageError);

			switch (status)
			{
			case storage::LoginStatus::Success:
				break;

			case storage::LoginStatus::NotFound:
				SendDBSignal(conn, m->ID, _MSG_DBAccountLoginFail_Account);
				return true;

			case storage::LoginStatus::WrongPassword:
			case storage::LoginStatus::Locked:
				if (!storageError.empty())
					Log(storageError, m->AccountName, 0);
				SendDBSignal(conn, m->ID, _MSG_DBAccountLoginFail_Pass);
				return true;

			case storage::LoginStatus::Blocked:
				SendDBSignalParm(conn, m->ID, _MSG_DBAccountLoginFail_Block, 0);
				return true;

			case storage::LoginStatus::TempKeyMismatch:
				// Legacy behavior: the stored server-change token was cleared
				// and the attempt ends without a reply.
				return true;

			case storage::LoginStatus::Error:
			default:
				Log(storageError, m->AccountName, 0);
				SendDBSignal(conn, m->ID, _MSG_DBAccountLoginFail_Account);
				return true;
			}

			STRUCT_ACCOUNTFILE file;

			storage::AccountFileFromRecord(record, &file);

			int ChangeServer = tempKeyUsed ? 1 : 0;

			if(IdxName == Idx)
				return true;

			if(IdxName != 0)
			{
				Log("err, desconectado. conexão anterior finalizada.", m->AccountName, 0);

				if(m->DBNeedSave == 0)
				{
					SendDBSignal(conn, m->ID, _MSG_DBAlreadyPlaying);

					return true;
				}

				SendDBSignal(conn, m->ID, _MSG_DBStillPlaying);
				SendDBSavingQuit(IdxName, 0);

				break;
			}

			UpcaseString(file.Info.AccountName);

			int right = -1;
			int left = -1;

			for(int q = 0; q < 4; q++)
			{
				if(file.Char[q].MobName[0] != 0)
				{
					if(file.Char[q].Equip[13].sIndex == 774)
						left = q;
				}

				if(file.Char[q].MobName[0] != 0)
				{
					if(file.Char[q].Equip[13].sIndex == 775)
						right  = q;
				}
			}

			if(left != -1 && right != -1)
			{
				char temp[NAME_LENGTH];

				memcpy(temp, file.Char[left].MobName, NAME_LENGTH);
				memcpy(file.Char[left].MobName, file.Char[right].MobName, NAME_LENGTH);
				memcpy(file.Char[right].MobName, temp, NAME_LENGTH);

				file.Char[left].Equip[13].sIndex = 0;
				file.Char[right].Equip[13].sIndex = 0;

				char tt[256];

				sprintf(tt, "etc,name swap %s %s", file.Char[left].MobName, file.Char[right].MobName);

				Log(tt, m->AccountName, 0);

				DBWriteAccount(&file);
			}

			STRUCT_ACCOUNTFILE *pFile;

			pFile = &pAccountList[Idx].File;

			memcpy(pFile, &file, sizeof(STRUCT_ACCOUNTFILE));

			pAccountList[Idx].AccountId = record.account_id;
			pAccountList[Idx].State = record.state;

			pAccountList[Idx].Mac[0] = m->AdapterName[0];
			pAccountList[Idx].Mac[1] = m->AdapterName[1];
			pAccountList[Idx].Mac[2] = m->AdapterName[2];
			pAccountList[Idx].Mac[3] = m->AdapterName[3];

			AddAccountList(Idx);

			STRUCT_SELCHAR selchar;

			DBGetSelChar(&selchar, &pAccountList[Idx].File);

			pAccountList[Idx].SecurePass = -1;

			MSG_DBCNFAccountLogin sm;

			memset(&sm, 0, sizeof(MSG_DBCNFAccountLogin));

			sm.Type = _MSG_DBCNFAccountLogin;
			sm.ID = m->ID;

			sm.Unknow_28 = 0xCCCCCCCC;

			strncpy(sm.AccountName, file.Info.AccountName, ACCOUNTNAME_LENGTH);
			memcpy(sm.Cargo, pAccountList[Idx].File.Cargo, sizeof(pAccountList[Idx].File.Cargo));

			sm.Coin = pAccountList[Idx].File.Coin;

			sm.sel = selchar;

			pUser[conn].cSock.SendOneMessage((char *)&sm, sizeof(MSG_DBCNFAccountLogin));
			
			if (GetAccountsByMac(m->AdapterName) <= 1)
			{
				MSG_DBCheckPrimaryAccount sm_pa;
				memset(&sm_pa, 0, sizeof(MSG_DBCheckPrimaryAccount));

				sm_pa.Size = sizeof(MSG_DBCheckPrimaryAccount);
				sm_pa.Type = _MSG_DBCheckPrimaryAccount;

				memcpy(&sm_pa.Mac, m->AdapterName, sizeof(sm_pa.Mac));

				strncpy(sm_pa.AccountName, pAccountList[Idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

				for (int i = 0; i < MAX_SERVER; i++)
				{
					if (pUser[i].cSock.IsOpen() == false || pUser[i].Mode == USER_EMPTY)
						continue;

					pUser[i].cSock.SendOneMessage((char*)&sm_pa, sizeof(MSG_DBCheckPrimaryAccount));
				}
			}
			if (ChangeServer == 1)
			{
				DBWriteAccount(&file);

				pAccountList[Idx].SecurePass = 1;

				int Slot;
				int Server;
				int Enc[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

				sscanf(m->Zero, "*%d %d %d %d %d %d %d %d %d %d", &Server, &Enc[0], &Enc[1], &Enc[2], &Enc[3], &Enc[4], &Enc[5], &Enc[6], &Enc[7], &Slot);

				pAccountList[Idx].Slot = Slot;

				MSG_CNFCharacterLogin sm;

				sm.Type = _MSG_DBCNFCharacterLogin;
				sm.ID = m->ID;
				sm.Size = sizeof(MSG_CNFCharacterLogin);

				sm.Slot = Slot;

				sm.mob = pAccountList[Idx].File.Char[Slot];
				sm.Donate = pAccountList[Idx].File.Donate;

				memcpy(sm.ShortSkill, pAccountList[Idx].File.ShortSkill[Slot], 16);
				memcpy(sm.affect, pAccountList[Idx].File.affect[Slot], sizeof(pAccountList[Idx].File.affect[Slot]));
				memcpy(&sm.mobExtra, &pAccountList[Idx].File.mobExtra[Slot], sizeof(STRUCT_MOBEXTRA));

				char *mob;

				mob = pAccountList[Idx].File.Char[Slot].MobName;

				if (mob[0] == 0)
				{
					Log("err,charlogin mobname empty", pAccountList[Idx].File.Info.AccountName, 0);

					return true;
				}
				pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFCharacterLogin));
				int rankId = rankingSystem.grindRanking.getElementIndex(pAccountList[Idx].File.Char[Slot].MobName);

				if (rankId >= LAST && rankId <= FIRST)
				{
					rankingSystem.grindRanking.setElementConnId(rankId, GrindRankingConnId(conn, sm.ID));
				}

				// Send ranking info to tm->client
				rankingSystem.sendUpdateIndividualRank(rankId, conn, sm.ID, new STRUCT_RANKING(pAccountList[Idx].File.Char[Slot].MobName, pAccountList[Idx].File.Char[Slot].Exp, pAccountList[Idx].File.mobExtra[Slot].ClassMaster, pAccountList[Idx].File.Char[Slot].CurrentScore.Level, pAccountList[Idx].File.Char[Slot].Class));
			}
		} break;
		
	case _MSG_DBCreateCharacter:
		{
			MSG_CreateCharacter *m = (MSG_CreateCharacter*)Msg;

			int Slot = m->Slot;
			int cls = m->MobClass;

			int Idx = GetIndex(conn, m->ID);

			if(Slot < 0 || Slot >= MOB_PER_ACCOUNT)
			{
				Log("err,newchar  slot out of range", pAccountList[Idx].File.Info.AccountName, 0);

				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				return true;
			}
			if(cls < 0 || cls >= 4)
			{
				Log("err,newchar - class out of range", pAccountList[Idx].File.Info.AccountName, 0);

				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				return true;
			}

			int Secure = pAccountList[Idx].SecurePass;

			if (Secure == -1)
			{
				Log("err,newchar secure illegal", pAccountList[Idx].File.Info.AccountName, 0);

				break;
			}

			char check[NAME_LENGTH];

			strncpy(check, m->MobName, NAME_LENGTH);

			UpcaseString(check);

			if(strcmp(check, "KING") == 0 || strcmp(check, "KINGDOM") == 0 || strcmp(check, "GRITAR") == 0 || strcmp(check, "RELO") == 0)
			{
				Log("err,newchar - cmd name", pAccountList[Idx].File.Info.AccountName, 0);

				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				return false;
			}

			if((check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0) || 
				(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0))
			{
				Log("err,newchar - com", pAccountList[Idx].File.Info.AccountName, 0);

				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				return false;
			}

			STRUCT_MOB *mob;

			mob = &pAccountList[Idx].File.Char[Slot];


			mob->MobName[NAME_LENGTH-1] = 0;
			mob->MobName[NAME_LENGTH-2] = 0;

			m->MobName[NAME_LENGTH-1] = 0;
			m->MobName[NAME_LENGTH-2] = 0;

			if(mob->MobName[0] != 0)
			{
				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				Log("err,newchar already charged", pAccountList[Idx].File.Info.AccountName, 0);
				Log(mob->MobName, m->MobName, 0);

				return true;
			}

			m->MobName[NAME_LENGTH-1] = 0;
			m->MobName[NAME_LENGTH-2] = 0;

			int len = strlen(m->MobName);

			for(int i = 0; i < len; i++)
			{
				if(m->MobName[i] == 'í' && m->MobName[i + 1] == 'í')
				{
					SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

					return true;
				}
			}

			int ret = CreateCharacter(pAccountList[Idx].File.Info.AccountName, m->MobName);

			if(ret == 0)
			{
				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				return true;
			}
			STRUCT_MOBEXTRA *extra;

			extra = &pAccountList[Idx].File.mobExtra[Slot];

			BASE_ClearMob(mob);
			BASE_ClearMobExtra(extra);

			memset(&pAccountList[Idx].File.affect[Slot], 0, sizeof(pAccountList[Idx].File.affect[Slot]));

			memset(&pAccountList[Idx].File.ShortSkill[Slot], -1, 16);

			extra->ClassMaster = MORTAL;

			if(cls == 0)
				memcpy(mob, &g_pBaseSet[0], sizeof(STRUCT_MOB));
			
			else if(cls == 1)
				memcpy(mob, &g_pBaseSet[1], sizeof(STRUCT_MOB));
			
			else if(cls == 2)
				memcpy(mob, &g_pBaseSet[2], sizeof(STRUCT_MOB));
			
			else if(cls == 3)
				memcpy(mob, &g_pBaseSet[3], sizeof(STRUCT_MOB));
			
			else
			{
				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				Log("err,newchar fail - undefined class", m->MobName, 0);

				return true;
			}


			extra->MortalFace = mob->Equip[0].sIndex;

			memcpy(mob->MobName, m->MobName, NAME_LENGTH);

			ret = DBWriteAccount(&pAccountList[Idx].File);

			if(ret == 0)
			{
				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				Log("err,newchar fail - create file", m->MobName, 0);

				return true;
			}

			char temp[256];

			sprintf(temp, "create character [%s]", mob->MobName);

			Log(temp, pAccountList[Idx].File.Info.AccountName, 0);

			MSG_CNFNewCharacter sm;

			sm.Type = _MSG_DBCNFNewCharacter;

			DBGetSelChar(&sm.sel, &pAccountList[Idx].File);

			sm.ID = m->ID;

			pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFNewCharacter));

		}	break;

	case _MSG_DBCharacterLogin:
		{
			MSG_CharacterLogin *m = (MSG_CharacterLogin*)Msg;

			int Slot = m->Slot;

			int Idx = GetIndex(conn, m->ID);

			if(Slot < 0 || Slot >= MOB_PER_ACCOUNT)
			{
				Log("err,charlogin slot illegal", pAccountList[Idx].File.Info.AccountName, 0);

				break;
			}

			int Secure = pAccountList[Idx].SecurePass;

			if (Secure == -1)
			{
				Log("err,charlogin secure illegal", pAccountList[Idx].File.Info.AccountName, 0);

				break;
			}

			pAccountList[Idx].Slot = Slot;

			MSG_CNFCharacterLogin sm;

			sm.Type = _MSG_DBCNFCharacterLogin;
			sm.ID = m->ID;
			sm.Size = sizeof(MSG_CNFCharacterLogin);

			sm.Slot = Slot;

			sm.mob = pAccountList[Idx].File.Char[Slot];
			sm.Donate = pAccountList[Idx].File.Donate;

			memcpy(sm.ShortSkill, pAccountList[Idx].File.ShortSkill[Slot], 16);
			memcpy(sm.affect, pAccountList[Idx].File.affect[Slot], sizeof(pAccountList[Idx].File.affect[Slot]));
			memcpy(&sm.mobExtra, &pAccountList[Idx].File.mobExtra[Slot], sizeof(STRUCT_MOBEXTRA));

			char *mob;

			mob = pAccountList[Idx].File.Char[Slot].MobName;

			if(mob[0] == 0)
			{
				Log("err,charlogin mobname empty", pAccountList[Idx].File.Info.AccountName, 0);

				return true;
			}

			pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFCharacterLogin));

			int rankId = rankingSystem.grindRanking.getElementIndex(pAccountList[Idx].File.Char[Slot].MobName);

			if (rankId >= LAST && rankId <= FIRST)
			{
				rankingSystem.grindRanking.setElementConnId(rankId, GrindRankingConnId(conn, sm.ID));
			}

			// Send ranking info to tm->client
			rankingSystem.sendUpdateIndividualRank(rankId, conn, sm.ID, new STRUCT_RANKING(pAccountList[Idx].File.Char[Slot].MobName, pAccountList[Idx].File.Char[Slot].Exp, pAccountList[Idx].File.mobExtra[Slot].ClassMaster, pAccountList[Idx].File.Char[Slot].CurrentScore.Level, pAccountList[Idx].File.Char[Slot].Class));
			return true;
		}

	case _MSG_DBNoNeedSave:
		{
			MSG_STANDARD *m = (MSG_STANDARD*)Msg;

			int Idx = GetIndex(conn, m->ID);

			char acc[ACCOUNTNAME_LENGTH];

			strncpy(acc, pAccountList[Idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

			RemoveAccountList(Idx);

			for(int i = 0; i < MAX_ADMIN; i++)
			{
				if(pAdmin[i].Level <= 2)
					continue;

				if(pAdmin[i].DisableID == 0)
					continue;

				if(pAdmin[i].DisableID != Idx)
					continue;

				DisableAccount(i, acc, pAdmin[i].Year, pAdmin[i].YearDay);
			}

		}	break;

	case _MSG_DBSaveMob:
		{
			MSG_DBSaveMob *m = (MSG_DBSaveMob*)Msg;

			int Idx = GetIndex(conn, m->ID);

			pAccountList[Idx].File.Info.AccountName[ACCOUNTNAME_LENGTH-1] = 0;
			pAccountList[Idx].File.Info.AccountName[ACCOUNTNAME_LENGTH-2] = 0;

			char acc[ACCOUNTNAME_LENGTH];

			strncpy(acc, pAccountList[Idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

			m->AccountName[ACCOUNTNAME_LENGTH-1] = 0;
			m->AccountName[ACCOUNTNAME_LENGTH-2] = 0;

			int Slot = pAccountList[Idx].Slot;

			if(Slot < 0 || Slot >= MOB_PER_ACCOUNT)
			{
				char temp[1024];

				sprintf(temp, "err,savemob1 %d %d %d %s %s",  m->ID, Slot, m->Slot, acc, m->AccountName);

				Log(temp, "-system", 0);

				return true;
			}

			if(Slot != m->Slot)
			{
				char temp[1024];

				sprintf(temp, "err,savemob2 %d %d %d %s %s",  m->ID, Slot, m->Slot, acc, m->AccountName);

				Log(temp, "-system", 0);
			}

			if(pAccountList[Idx].Login == 0)
			{
				char temp[1024];

				sprintf(temp, "err,savemob3 %d %d %d %s %s",  m->ID, Slot, m->Slot, acc, m->AccountName);

				Log(temp, "-system", 0);

				return true;
			}

			pAccountList[Idx].File.Donate = m->Donate;
			pAccountList[Idx].File.Char[Slot] = m->MOB;
			pAccountList[Idx].File.mobExtra[Slot] = m->extra;
			
			memcpy(&pAccountList[Idx].File.affect[Slot], m->affect, sizeof(pAccountList[Idx].File.affect[Slot]));
			memcpy(&pAccountList[Idx].File.Cargo, m->Cargo, sizeof(pAccountList[Idx].File.Cargo));

			memcpy(&pAccountList[Idx].File.ShortSkill[Slot], m->ShortSkill, 16);

			pAccountList[Idx].File.Coin = m->Coin;

			DBWriteAccount(&pAccountList[Idx].File);

			if(m->Export != 0)
				DBExportAccount(&pAccountList[Idx].File);

		} break;

	case _MSG_SavingQuit:
		{
			MSG_SavingQuit *m = (MSG_SavingQuit*)Msg;

			int Idx = GetIndex(conn, m->ID);

			int Slot = pAccountList[Idx].Slot;

			pAccountList[Idx].File.Char[Slot].MobName[NAME_LENGTH-1] = 0;
			pAccountList[Idx].File.Char[Slot].MobName[NAME_LENGTH-2] = 0;

			char acc[ACCOUNTNAME_LENGTH];

			strncpy(acc, pAccountList[Idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

			m->AccountName[ACCOUNTNAME_LENGTH-1] = 0;
			m->AccountName[ACCOUNTNAME_LENGTH-2] = 0;

			if(strncmp(m->AccountName, acc, ACCOUNTNAME_LENGTH))
			{
				char temp[256];

				sprintf(temp, "err,savenquit1 %d %d %d %s %s", m->ID, Slot, m->Slot, acc, m->AccountName);
				Log(temp, "-system", 0);

				RemoveAccountList(Idx);

				return true;
			}

			if((Slot < 0 || Slot >= MOB_PER_ACCOUNT) || (Slot != m->Slot))
			{
				char temp[256];

				sprintf(temp, "err,savenquit2 %d %d %d %s %s", m->ID, Slot, m->Slot, acc, m->AccountName);
				Log(temp, "-system", 0);

				RemoveAccountList(Idx);

				return true;
			}

			if(pAccountList[Idx].Login == 0)
			{
				char temp[256];

				sprintf(temp, "err,savenquit3 %d %d %d %s %s", m->ID, Slot, m->Slot, acc, m->AccountName);
				Log(temp, "-system", 0);

				return true;
			}

			pAccountList[Idx].File.Char[Slot] = m->MOB;

			memcpy(pAccountList[Idx].File.Cargo, m->Cargo, sizeof(STRUCT_ITEM)*MAX_CARGO);
			memcpy(pAccountList[Idx].File.ShortSkill[Slot], m->ShortSkill, 16);

			pAccountList[Idx].File.mobExtra[Slot] = m->extra;

			memcpy(&pAccountList[Idx].File.affect[Slot], m->affect, sizeof(pAccountList[Idx].File.affect[Slot]));

			pAccountList[Idx].File.Coin = m->Coin;
			pAccountList[Idx].File.Donate = m->Donate;

			DBWriteAccount(&pAccountList[Idx].File);
			DBExportAccount(&pAccountList[Idx].File);
			RemoveAccountList(Idx);

			SendDBSignal(conn, m->ID, _MSG_DBCNFAccountLogOut);

			for(int i = 0; i < MAX_ADMIN; i++)
			{
				if(pAdmin[i].Level <= 2 || pAdmin[i].DisableID == 0)
					continue;

				if(pAdmin[i].DisableID == Idx)
				{
					DisableAccount(i, acc, pAdmin[i].Year, pAdmin[i].YearDay);

					break;
				}
			}

		} break;

	case _MSG_DBDeleteCharacter:
		{
			MSG_DeleteCharacter *m = (MSG_DeleteCharacter*)Msg;

			int Idx = GetIndex(conn, m->ID);

			int Slot = m->Slot;

			if (Slot < 0 || Slot >= MOB_PER_ACCOUNT)
			{
				SendDBSignal(conn, m->ID, _MSG_DBDeleteCharacterFail);

				Log("err,deletechar slot", pAccountList[Idx].File.Info.AccountName, 0);

				return true;
			}

			int Secure = pAccountList[Idx].SecurePass;

			if (Secure == -1)
			{
				Log("err,deletechar secure illegal", pAccountList[Idx].File.Info.AccountName, 0);

				break;
			}

			bool passwordMatched = false;
			std::string passwordError;

			if (!repository_
				|| !repository_->VerifyPassword(pAccountList[Idx].AccountId,
					security::SubmittedCredential(m->Password, ACCOUNTPASS_LENGTH),
					&passwordMatched, &passwordError)
				|| !passwordMatched)
			{
				SendDBSignal(conn, m->ID, _MSG_DBDeleteCharacterFail);

				Log("err,deletechar password", pAccountList[Idx].File.Info.AccountName, 0);

				return true;
			}

			STRUCT_MOB *mob;

			mob = &pAccountList[Idx].File.Char[Slot];

			if (pAccountList[Idx].File.mobExtra[Slot].ClassMaster != MORTAL && pAccountList[Idx].File.mobExtra[Slot].ClassMaster != ARCH)
			{
				SendDBSignal(conn, m->ID, _MSG_DBDeleteCharacterFail);
				return true;
			}

			if(mob->BaseScore.Level >= 219)
			{
				SendDBSignal(conn, m->ID, _MSG_DBDeleteCharacterFail);

				Log("err,deletechar level 219", pAccountList[Idx].File.Info.AccountName, 0);

				return true;
			}

	
			memset(pAccountList[Idx].File.ShortSkill[Slot], 0, 16);

			DeleteCharacter(mob->MobName, pAccountList[Idx].File.Info.AccountName);

			char Temp[128];
			sprintf(Temp, "delete char [%s]", mob->MobName);		

			Log(Temp, pAccountList[Idx].File.Info.AccountName, 0);

			BASE_ClearMob(mob);
			BASE_ClearMobExtra(&pAccountList[Idx].File.mobExtra[Slot]);

			DBWriteAccount(&pAccountList[Idx].File);

			MSG_CNFDeleteCharacter sm;

			sm.Type = _MSG_DBCNFDeleteCharacter;

			DBGetSelChar(&sm.sel, &pAccountList[Idx].File);

			sm.ID = m->ID;

			pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFDeleteCharacter));

		} break;

	case _MSG_AccountSecure:
	{
		MSG_AccountSecure *m = (MSG_AccountSecure*)Msg;

		int Idx = GetIndex(conn, m->ID);

		if (!repository_ || Idx < 0 || Idx >= MAX_DBACCOUNT)
			break;

		int Secure = pAccountList[Idx].SecurePass;

		int Change = m->ChangeNumeric;

		if (Change && Secure == -1)
		{
			Log("err,secureverify change error 1", pAccountList[Idx].File.Info.AccountName, 0);

			break;
		}

		std::string token = security::SubmittedCredential(
			m->NumericToken, sizeof(m->NumericToken));
		std::string error;

		if (pAccountList[Idx].File.Info.NumericToken[0] == -1)
		{
			if (!repository_->BindToken(pAccountList[Idx].AccountId, token, false, &error))
			{
				Log(error, pAccountList[Idx].File.Info.AccountName, 0);

				pAccountList[Idx].SecurePass = -1;
				SendDBSignal(conn, m->ID, _MSG_AccountSecureFail);

				break;
			}

			// The plaintext token is never kept in the session record; zero is
			// the "bound" marker that replaces the legacy -1 sentinel.
			memset(pAccountList[Idx].File.Info.NumericToken, 0, sizeof(pAccountList[Idx].File.Info.NumericToken));

			pAccountList[Idx].SecurePass = 1;
			SendDBSignal(conn, m->ID, _MSG_AccountSecure);

			break;
		}

		if (Change == 0 && Secure == -1)
		{
			bool matched = false;

			if (!repository_->VerifyToken(pAccountList[Idx].AccountId, token,
					&matched, &error))
			{
				Log(error, pAccountList[Idx].File.Info.AccountName, 0);
			}

			if (matched)
			{
				pAccountList[Idx].SecurePass = 1;
				SendDBSignal(conn, m->ID, _MSG_AccountSecure);

				break;
			}
		}
		else if (Change == 1 && Secure == 1)
		{
			if (repository_->BindToken(pAccountList[Idx].AccountId, token, true, &error))
			{
				memset(pAccountList[Idx].File.Info.NumericToken, 0, sizeof(pAccountList[Idx].File.Info.NumericToken));

				pAccountList[Idx].SecurePass = 1;
				SendDBSignal(conn, m->ID, _MSG_AccountSecure);

				break;
			}

			Log(error, pAccountList[Idx].File.Info.AccountName, 0);
		}

		pAccountList[Idx].SecurePass = -1;
		SendDBSignal(conn, m->ID, _MSG_AccountSecureFail);
	} break;

	case _MSG_DBCreateArchCharacter:
	{
		MSG_DBCreateArchCharacter *m = (MSG_DBCreateArchCharacter*)Msg;

		int cls = m->MobClass;
		int MortalFace = m->MortalFace;

		int Idx = GetIndex(conn, m->ID);

		int Slot = 0;

		for (Slot = 0; Slot < MOB_PER_ACCOUNT; Slot++)
		if (pAccountList[Idx].File.Char[Slot].MobName[0] == 0)
			break;			   

		if (Slot < 0 || Slot >= MOB_PER_ACCOUNT)
		{
			Log("err,newchar  slot out of range", pAccountList[Idx].File.Info.AccountName, 0);

			SendDBSignal(conn, m->ID, _MSG_DBCNFArchCharacterFail);

			return true;
		}
		if (cls < 0 || cls >= MAX_CLASS)
		{
			Log("err,newchar - class out of range", pAccountList[Idx].File.Info.AccountName, 0);

			SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

			return true;
		}

		char check[NAME_LENGTH];

		strncpy(check, m->MobName, NAME_LENGTH);

		UpcaseString(check);

		if(strcmp(check, "KING") == 0 || strcmp(check, "KINGDOM") == 0 || strcmp(check, "GRITAR") == 0 || strcmp(check, "RELO") == 0)
		{
			Log("err,newchar - cmd name", pAccountList[Idx].File.Info.AccountName, 0);

			SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

			return false;
		}

		if ((check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0) ||
			(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0))
		{
			Log("err,newchar - com", pAccountList[Idx].File.Info.AccountName, 0);

			SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

			return false;
		}

		STRUCT_MOB *mob;

		mob = &pAccountList[Idx].File.Char[Slot];


		mob->MobName[NAME_LENGTH - 1] = 0;
		mob->MobName[NAME_LENGTH - 2] = 0;

		m->MobName[NAME_LENGTH - 1] = 0;
		m->MobName[NAME_LENGTH - 2] = 0;

		if (mob->MobName[0] != 0)
		{
			SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

			Log("err,newchar already charged", pAccountList[Idx].File.Info.AccountName, 0);
			Log(mob->MobName, m->MobName, 0);

			return true;
		}

		m->MobName[NAME_LENGTH - 1] = 0;
		m->MobName[NAME_LENGTH - 2] = 0;

		int len = strlen(m->MobName);

		for (int i = 0; i < len; i++)
		{
			if (m->MobName[i] == 'í' && m->MobName[i + 1] == 'í')
			{
				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				return true;
			}
		}

								  
		STRUCT_MOBEXTRA *extra;

		extra = &pAccountList[Idx].File.mobExtra[Slot];

		BASE_ClearMob(mob);
		BASE_ClearMobExtra(extra);

		memset(&pAccountList[Idx].File.affect[Slot], 0, sizeof(pAccountList[Idx].File.affect[Slot]));
		memset(&pAccountList[Idx].File.ShortSkill[Slot], -1, 16);

		extra->ClassMaster = ARCH;
		extra->QuestInfo.Arch.MortalSlot = m->MortalSlot;

		if (cls == 0)
			memcpy(mob, &g_pBaseSet[0], sizeof(STRUCT_MOB));

		else if (cls == 1)
			memcpy(mob, &g_pBaseSet[1], sizeof(STRUCT_MOB));

		else if (cls == 2)
			memcpy(mob, &g_pBaseSet[2], sizeof(STRUCT_MOB));

		else if (cls == 3)
			memcpy(mob, &g_pBaseSet[3], sizeof(STRUCT_MOB));

		else
		{
			SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

			Log("err,newchar fail - undefined class", m->MobName, 0);

			return true;
		}

		mob->Equip[0].sIndex = MortalFace + 5 + cls;
		mob->BaseScore.Ac = 230;

		memset(&mob->Equip[1], 0, sizeof(STRUCT_ITEM)*(MAX_EQUIP - 1));
		memset(&mob->Carry[0], 0, sizeof(STRUCT_ITEM)*(MAX_CARRY - 4));

		extra->MortalFace = MortalFace;

		memcpy(mob->MobName, m->MobName, NAME_LENGTH);

		int ret = DBWriteAccount(&pAccountList[Idx].File);

		if (ret == 0)
		{
			SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

			Log("err,newchar fail - create file", m->MobName, 0);

			return true;
		}

		char temp[256];

		sprintf(temp, "create character [%s]", mob->MobName);

		Log(temp, pAccountList[Idx].File.Info.AccountName, 0);

		MSG_CNFNewCharacter sm;

		sm.Type = _MSG_DBCNFNewCharacter;

		DBGetSelChar(&sm.sel, &pAccountList[Idx].File);

		sm.ID = m->ID;

		SendDBSignalParm(conn, m->ID, _MSG_DBCNFArchCharacterSucess, Slot);
		pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFNewCharacter));

	}	break;

	case _MSG_MagicTrumpet:
	{
			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen())
					pUser[i].cSock.SendOneMessage(Msg, sizeof(MSG_MagicTrumpet));
			}

			for(int i = 0; i < MAX_ADMIN; i++)
			{
				if(pAdmin[i].Mode == USER_EMPTY)
					continue;

				if(pAdmin[i].cSock.IsOpen())
					pAdmin[i].cSock.SendOneMessage(Msg, sizeof(MSG_MagicTrumpet));
			}
	}break;

	case _MSG_DBNotice:
	{
			for(int i = 0; i < MAX_SERVER; i++)
			{
				if(pUser[i].Mode == USER_EMPTY)
					continue;

				if(pUser[i].cSock.IsOpen())
					pUser[i].cSock.SendOneMessage(Msg, sizeof(MSG_DBNotice));
			}
	}break;

	case _MSG_DBCapsuleInfo:
	{
		MSG_STANDARDPARM *m = (MSG_STANDARDPARM*)Msg;

		int Index = m->Parm;

		if (!repository_)
			return false;

		storage::CharacterRecord capsuleRecord;
		bool capsuleFound = false;
		std::string capsuleError;

		if (!repository_->LoadCapsule(Index, &capsuleRecord, &capsuleFound,
				&capsuleError))
		{
			Log(capsuleError, "capsule", 0);
			return false;
		}

		if (!capsuleFound)
			return false;

		STRUCT_CAPSULE file;
		memset(&file, 0, sizeof(file));

		storage::MobFromCharacterRecord(capsuleRecord, &file.MOB, &file.extra,
			nullptr, nullptr);

		MSG_CNFDBCapsuleInfo sm;

		sm.Size = sizeof(MSG_CNFDBCapsuleInfo);
		sm.Type = _MSG_CNFDBCapsuleInfo;
		sm.ID = m->ID;

		sm.Index = Index;

		sm.Capsule.Read = 1;
		sm.Capsule.MortalClass = file.extra.MortalFace / 10;
		sm.Capsule.ClassCele = file.extra.SaveCelestial[0].Class;
		sm.Capsule.SubClass = file.MOB.Class;
		sm.Capsule.LevelCele = file.extra.SaveCelestial[0].BaseScore.Level;
		sm.Capsule.LevelSub = file.MOB.BaseScore.Level;

		sm.Capsule.For = file.MOB.BaseScore.Str;
		sm.Capsule.Int = file.MOB.BaseScore.Int;
		sm.Capsule.Dex = file.MOB.BaseScore.Dex;
		sm.Capsule.Con = file.MOB.BaseScore.Con;

		sm.Capsule.ScoreBonus = file.MOB.ScoreBonus;
		sm.Capsule.SkillPoint = file.MOB.SkillBonus;

		sm.Capsule.ArchQuest = file.extra.QuestInfo.Arch.Cristal;
		sm.Capsule.CelestialQuest = file.extra.QuestInfo.Celestial.Reset;
		sm.Capsule.ArcanaQuest = file.extra.QuestInfo.Circle;

		pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFDBCapsuleInfo));
	} break;

	case _MSG_DBPutInCapsule:
	{
		MSG_STANDARDPARM *m = (MSG_STANDARDPARM*)Msg;

		int Slot = m->Parm;

		int Idx = GetIndex(conn, m->ID);

		if (Slot < 0 || Slot >= MOB_PER_ACCOUNT)
		{
			Log("err,putincapsule  slot out of range", pAccountList[Idx].File.Info.AccountName, 0);

			return true;
		}

		if (!repository_)
		{
			Log("err,putincapsule storage not initialized", pAccountList[Idx].File.Info.AccountName, 0);

			return true;
		}

		char capturedName[NAME_LENGTH];
		memset(capturedName, 0, sizeof(capturedName));
		memcpy(capturedName, pAccountList[Idx].File.Char[Slot].MobName, NAME_LENGTH - 1);

		int capsuleId = 0;
		std::string capsuleError;

		if (!repository_->PutInCapsule(pAccountList[Idx].AccountId, Slot,
				&capsuleId, &capsuleError))
		{
			Log(capsuleError, pAccountList[Idx].File.Info.AccountName, 0);

			return true;
		}

		memset(pAccountList[Idx].File.ShortSkill[Slot], 0, 16);

		char Temp[128];
		sprintf(Temp, "putchar in capsule [%s]", capturedName);

		Log(Temp, pAccountList[Idx].File.Info.AccountName, 0);

		STRUCT_MOB *mob = &pAccountList[Idx].File.Char[Slot];

		BASE_ClearMob(mob);
		BASE_ClearMobExtra(&pAccountList[Idx].File.mobExtra[Slot]);

		DBWriteAccount(&pAccountList[Idx].File);

		SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleSucess);

		MSG_CNFDeleteCharacter sm;

		sm.Type = _MSG_DBCNFDeleteCharacter;

		DBGetSelChar(&sm.sel, &pAccountList[Idx].File);

		sm.ID = m->ID;

		pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFDeleteCharacter));

		char temp[256];

		sprintf(temp, "../../Common/ImportItem/%d%d%d%d%d", capsuleId, rand()%255, rand()%255, rand()%255, rand()%255);

		FILE *fp = fopen(temp, "a+");

		if (fp != nullptr)
		{
			fprintf(fp, "%s %d %d %d %d %d %d %d", pAccountList[Idx].File.Info.AccountName, 3443, 59, capsuleId/256, 59, (capsuleId - ((capsuleId/256)*256)), 0, 0);
			fclose(fp);
		}
	} break;

	case _MSG_DBOutCapsule:
	{
		MSG_DBOutCapsule *m = (MSG_DBOutCapsule*)Msg;

		int Slot = m->Slot;
		int NovoSlot = 0;
		int Idx = GetIndex(conn, m->ID);

		if (Slot < 0 || Slot >= MOB_PER_ACCOUNT)
		{
			Log("err,outcapsule  slot out of range", pAccountList[Idx].File.Info.AccountName, 0);

			return true;
		}

		if (!repository_)
		{
			Log("err,outcapsule storage not initialized", pAccountList[Idx].File.Info.AccountName, 0);

			return true;
		}

		STRUCT_ITEM *item = GetItemPointer(&pAccountList[Idx].File.Char[Slot],
			pAccountList[Idx].File.Cargo, m->SourType, m->SourPos);

		if (item == nullptr)
		{
			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail);

			return true;
		}

		int index = item->stEffect[0].cValue * 256 + item->stEffect[1].cValue;

		if (index <= 0)
		{
			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail);

			return true;
		}

		for (NovoSlot = 0; NovoSlot < MOB_PER_ACCOUNT; NovoSlot++)
		{
			if (pAccountList[Idx].File.Char[NovoSlot].MobName[0] == 0)
				break;
		}

		if (NovoSlot < 0 || NovoSlot >= MOB_PER_ACCOUNT)
		{
			Log("err,newcharcapsule  slot out of range", pAccountList[Idx].File.Info.AccountName, 0);

			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail);

			return true;
		}

		m->MobName[NAME_LENGTH - 1] = 0;
		m->MobName[NAME_LENGTH - 2] = 0;

		if (m->MobName[0] == 0)
		{
			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail2);

			return true;
		}

		int len = strlen(m->MobName);

		for (int i = 0; i < len - 1; i++)
		{
			if (m->MobName[i] == 'í' && m->MobName[i + 1] == 'í')
			{
				SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

				return true;
			}
		}

		int ret = CreateCharacter(pAccountList[Idx].File.Info.AccountName, m->MobName);

		if(ret == 0)
		{
			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail2);

			return true;
		}

		storage::CharacterRecord capsuleRecord;
		bool capsuleFound = false;
		std::string capsuleError;

		if (!repository_->LoadCapsule(index, &capsuleRecord, &capsuleFound,
				&capsuleError) || !capsuleFound)
		{
			Log(capsuleError.empty() ? "err capsule missing" : capsuleError,
				pAccountList[Idx].File.Info.AccountName, 0);

			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail2);

			return true;
		}

		STRUCT_MOB mob;
		STRUCT_MOBEXTRA extra;

		storage::MobFromCharacterRecord(capsuleRecord, &mob, &extra, nullptr, nullptr);

		// Legacy take-out clears everything except the first equipment slot and
		// the last four carried items, and resets coin/guild state.
		memset(&mob.Equip[1], 0, sizeof(STRUCT_ITEM) * (MAX_EQUIP - 1));
		memset(&mob.Carry[0], 0, sizeof(STRUCT_ITEM) * (MAX_CARRY - 4));

		mob.Coin = 0;
		mob.Guild = 0;
		mob.GuildLevel = 0;

		memcpy(mob.MobName, m->MobName, NAME_LENGTH - 1);
		mob.MobName[NAME_LENGTH - 1] = 0;

		if (!repository_->TakeOutCapsule(pAccountList[Idx].AccountId, index,
				NovoSlot, storage::NameBytes(m->MobName, NAME_LENGTH),
				&capsuleError))
		{
			Log(capsuleError, pAccountList[Idx].File.Info.AccountName, 0);

			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail2);

			return true;
		}

		STRUCT_ACCOUNTFILE accountFile;
		memset(&accountFile, 0, sizeof(accountFile));

		memcpy(accountFile.Info.AccountName,
			pAccountList[Idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

		if (!DBReadAccount(&accountFile))
		{
			SendDBSignal(conn, m->ID, _MSG_DBCNFCapsuleCharacterFail2);

			return true;
		}

		STRUCT_ITEM *freshItem = GetItemPointer(&accountFile.Char[Slot],
			accountFile.Cargo, m->SourType, m->SourPos);

		if (freshItem != nullptr)
			memset(freshItem, 0, sizeof(STRUCT_ITEM));

		accountFile.Char[NovoSlot] = mob;
		accountFile.mobExtra[NovoSlot] = extra;

		memset(&accountFile.affect[NovoSlot], 0,
			sizeof(accountFile.affect[NovoSlot]));
		memset(&accountFile.ShortSkill[NovoSlot], -1, 16);

		ret = DBWriteAccount(&accountFile);

		if (ret == 0)
		{
			SendDBSignal(conn, m->ID, _MSG_DBNewCharacterFail);

			Log("err,newchar fail - save capsule restore",
				pAccountList[Idx].File.Info.AccountName, 0);

			return true;
		}

		pAccountList[Idx].File = accountFile;

		char temp[256];
		sprintf(temp, "create character [%s] - capsule", m->MobName);

		Log(temp, pAccountList[Idx].File.Info.AccountName, 0);

		MSG_CNFNewCharacter sm;

		sm.Type = _MSG_DBCNFNewCharacter;

		DBGetSelChar(&sm.sel, &pAccountList[Idx].File);

		sm.ID = m->ID;

		pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_CNFNewCharacter));
	} break;

	case _MSG_DBServerChange:
	{
		MSG_DBServerChange *m = (MSG_DBServerChange*)Msg;

		int ID = m->ID;
		int NewServer = m->NewServerID;
		int Slot = m->Slot;

		if (ID <= 0 || ID >= MAX_USER)
			break;

		char temp[256];

		if (NewServer <= 0 || NewServer > MAX_SERVER + 1)
		{
			sprintf(temp, "err,remove server srvnum:%d ", NewServer);
			Log(temp, "system", 0);
			break;
		}
		int Idx = GetIndex(conn, m->ID);

		int Enc[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		
		int EncResult = GetEncPassword(Idx, Enc);

		if (EncResult)
		{
			MSG_STANDARDPARM sm;
			memset(&sm, 0, sizeof(MSG_STANDARDPARM));

			sm.Type = _MSG_DBServerSend1;
			sm.Size = sizeof(MSG_STANDARDPARM);
			sm.ID = ID;

			sm.Parm = EncResult;

			pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_STANDARDPARM));
		}

		else
		{
			SetEncPassword(Idx, NewServer, Enc, m->Slot);
			
			MSG_DBCNFServerChange sm;
			memset(&sm, 0, sizeof(MSG_DBCNFServerChange));
			
			sm.Type = _MSG_DBCNFServerChange;
			sm.Size = sizeof(MSG_DBCNFServerChange);
			sm.ID = ID;

			strcpy(sm.AccountName, pAccountList[Idx].File.Info.AccountName);

			sprintf(sm.Enc, "*%d %d %d %d %d %d %d %d %d %d", m->NewServerID,
				Enc[0], Enc[1], Enc[2], Enc[3], Enc[4], Enc[5], Enc[6], Enc[7], m->Slot);

			memcpy(pAccountList[Idx].File.TempKey, sm.Enc, sizeof(pAccountList[Idx].File.TempKey));

			pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_DBCNFServerChange));
		}
	} break;

	case _MSG_UpdateExpRanking:
	{
		/*
			Process the update exp entries from tmsrv and update the ranking
		*/

		MSG_UpdateExpRanking* m = (MSG_UpdateExpRanking*)Msg;

		int rankId = rankingSystem.grindRanking.getElementIndex(m->RankInfo.Name);
		int newRankId = rankId;

		if (rankId == OUT_OF_RANK)
		{
			/* The player is not in the ranking, so we can try to insert him. */

			STRUCT_RANKING olderLastPlayer = STRUCT_RANKING();
			GrindRankingConnId olderLastConnId = GrindRankingConnId();
			GrindRankingConnId rankConnId = GrindRankingConnId(conn, m->ID);

			newRankId = rankingSystem.tryInsertInRanking(rankId, m->RankInfo.Name, m->RankInfo.Value, m->RankInfo.ClassMaster, m->RankInfo.Level, m->RankInfo.Class, &rankConnId, &olderLastPlayer, &olderLastConnId);

			if (newRankId >= LAST && newRankId <= FIRST)
			{
				/* The player have just entered the ranking. Now we have to notify the appropriate online players of these changes */

				rankId = LAST;

				/* Update the older [LAST] player that was kicked at the ranking */
				if (olderLastConnId.TMSrvId >= 0 && olderLastConnId.TMSrvId <= MAX_SERVER && olderLastConnId.PlayerId > 0 && olderLastConnId.PlayerId < MAX_USER)
					rankingSystem.sendUpdateIndividualRank(OUT_OF_RANK, olderLastConnId.TMSrvId, olderLastConnId.PlayerId, &olderLastPlayer);
				// ---

				/* Update all the other players that were affected by this change */
				rankingSystem.sendUpdateRangeRank(rankId, newRankId);
				// ---
			}
		}
		else if (rankId >= LAST && rankId <= FIRST)
		{
			/* The player is already in the ranking. Here we have to check if he can rise in the ranking and notify the appropriate online players */

			STRUCT_RANKING* thisRank = rankingSystem.grindRanking.getElement(rankId);

			if (m->RankInfo.Value > thisRank->Value && m->RankInfo.ClassMaster <= thisRank->ClassMaster || m->RankInfo.ClassMaster > thisRank->ClassMaster)
			{
				newRankId = rankingSystem.increaseRankingElementValue(rankId, (int)m->RankInfo.Value, m->RankInfo.ClassMaster);

				/* Update all the other players that were affected by this change */
				rankingSystem.sendUpdateRangeRank(rankId, newRankId);
				// ---
			}
		}
	} break;

	case _MSG_DBItemDayLog:
	{
		MSG_STANDARDPARM *m = (MSG_STANDARDPARM*)Msg;

		int ItemID = m->Parm;

		if (ItemID <= 0 || ItemID >= MAX_ITEMLIST)
		{
			Log("err,itemid invalid", "-system", 0);

			return true;
		}

		ItemDayLog[ItemID].Num++;
	} break;

	case _MSG_DBActivatePinCode:
	{
		MSG_DBActivatePinCode *m = (MSG_DBActivatePinCode*)Msg;

		// CActivePinCode::ProcessMessage(Msg, conn);

	} break;

	case _MSG_DBPrimaryAccount:
	{
		MSG_DBPrimaryAccount *m = (MSG_DBPrimaryAccount*)Msg;

		int Idx = GetIndex(conn, m->ID);

		MSG_DBCheckPrimaryAccount sm;
		memset(&sm, 0, sizeof(MSG_DBCheckPrimaryAccount));

		sm.Size = sizeof(MSG_DBCheckPrimaryAccount);
		sm.Type = _MSG_DBCheckPrimaryAccount;
		
		memcpy(&sm.Mac, m->Mac, sizeof(sm.Mac));

		strncpy(sm.AccountName, pAccountList[Idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

		for (int i = 0; i < MAX_SERVER; i++)
		{
			if (pUser[i].cSock.IsOpen() == false || pUser[i].Mode == USER_EMPTY)
				continue;

			pUser[i].cSock.SendOneMessage((char*)&sm, sizeof(MSG_DBCheckPrimaryAccount));
		}

		MSG_DBClientMessage sm2;

		sm2.Type = _MSG_DBClientMessage;
		sm2.ID = m->ID;
		sm2.Size = sizeof(MSG_DBClientMessage);

		strcpy(sm2.String, "Sua conta agora é a primária.");

		pUser[conn].cSock.SendOneMessage((char*)&sm2, sizeof(MSG_DBClientMessage));
	} break;

	}

	return true;
}

int CFileDB::SendDBSignal(int svr, unsigned short id, unsigned short signal)
{
	MSG_STANDARD sm;

	sm.Type = signal;
    sm.ID = id;
	sm.Size = sizeof(sm);

	if(pUser[svr].cSock.IsOpen())
		pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	return true;
}

int CFileDB::SendDBSignalParm(int svr, unsigned short id, unsigned short signal, int parm)
{
	 MSG_STANDARDPARM sm;

	 sm.Type = signal;
     sm.ID = id;
	 sm.Size = sizeof(sm);
	 sm.Parm = parm;

	 if(pUser[svr].cSock.IsOpen())
		 pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	 return true;
}

int CFileDB::SendDBSignalParm2(int svr, unsigned short id, unsigned short signal, int parm1, int parm2)
{
	MSG_STANDARDPARM2 sm;

	sm.Type = signal;
    sm.ID = id;
	sm.Size = sizeof(sm);
	sm.Parm1 = parm1;
	sm.Parm2 = parm2;

	if(pUser[svr].cSock.IsOpen())
		pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	return true;
}

int CFileDB::SendDBSignalParm3(int svr, unsigned short id, unsigned short signal, int parm1, int parm2, int parm3)
{
	MSG_STANDARDPARM3 sm;
	
	sm.Type = signal;
    sm.ID = id;
	sm.Size = sizeof(sm);
	sm.Parm1 = parm1;
	sm.Parm2 = parm2;
	sm.Parm3 = parm3;

	if(pUser[svr].cSock.IsOpen())
		pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(sm));

	return true;
}

int CFileDB::SendDBMessage(int svr, unsigned short id, char *msg)
{
	MSG_MessagePanel sm;

	sm.Type = _MSG_DBMessagePanel;  
    sm.ID = id;
	sm.Size = sizeof(MSG_MessagePanel);

    strncpy(sm.String, msg, MESSAGE_LENGTH);

    pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(MSG_MessagePanel));
	
	return true;
}

void CFileDB::SendGuildInfo(int srv, int Guild)
{
	MSG_GuildInfo sm;

	sm.Type = _MSG_GuildInfo;  
    sm.ID = 0;
	sm.Size = sizeof(MSG_GuildInfo);

	sm.Guild = Guild;
//	sm.GuildInfo = GuildInfo[Guild];

	memcpy(&sm.GuildInfo, &GuildInfo[Guild], sizeof(STRUCT_GUILDINFO));

	if(pUser[srv].cSock.IsOpen())
		pUser[srv].cSock.SendOneMessage((char*)&sm, sizeof(MSG_GuildInfo));
}

void CFileDB::AddAccountList(int Idx)
{ 
	if(pAccountList[Idx].Login == 1)
	{
		Log("err,addAccountlist - already added", pAccountList[Idx].File.Info.AccountName, 0);

		return;
	}

    int conn = Idx / MAX_USER;

	pUser[conn].Count++;

	pAccountList[Idx].Login = true;
	pAccountList[Idx].Slot  = -1;

	return;
}

void CFileDB::RemoveAccountList(int Idx)
{
    if(pAccountList[Idx].Login == false)
		return;
	
	int conn = Idx / MAX_USER;

	pUser[conn].Count--;

	pAccountList[Idx].Login = false;
	pAccountList[Idx].Slot  = -1;

	memset(&pAccountList[Idx].File, 0, sizeof(pAccountList[Idx].File));
}

int CFileDB::CreateCharacter(char *ac, char *ch)
{
	char check[ACCOUNTNAME_LENGTH];

	strncpy(check, ac, ACCOUNTNAME_LENGTH - 1);
	check[ACCOUNTNAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;

	char nameCheck[NAME_LENGTH];

	strncpy(nameCheck, ch, NAME_LENGTH - 1);
	nameCheck[NAME_LENGTH - 1] = 0;

	UpcaseString(nameCheck);

	if(nameCheck[0] == 'C' && nameCheck[1] == 'O' && nameCheck[2] == 'M' && nameCheck[3] >= '0' && nameCheck[3] <= '9' && nameCheck[4] == 0)
		return false;
	if(nameCheck[0] == 'L' && nameCheck[1] == 'P' && nameCheck[2] == 'T' && nameCheck[3] >= '0' && nameCheck[3] <= '9' && nameCheck[4] == 0)
		return false;

	if (!repository_)
		return false;

	bool available = false;
	std::string error;

	if (!repository_->CharacterNameAvailable(
			storage::NameBytes(ch, NAME_LENGTH), &available, &error))
	{
		Log(error, ch, 0);
		return false;
	}

	if (!available)
	{
		Log("err createchar EEXIST", ch, 0);
		return false;
	}

	return true;
}

int CFileDB::DeleteCharacter(char *ch, char *account)
{
	char check[ACCOUNTNAME_LENGTH];

	strncpy(check, ch, ACCOUNTNAME_LENGTH - 1);
	check[ACCOUNTNAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;

	if (!repository_)
		return false;

	storage::AccountRecord ownerRecord;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(
			storage::NameBytes(account, ACCOUNTNAME_LENGTH),
			&ownerRecord, &found, &error))
	{
		Log(error, account, 0);
		return false;
	}

	if (!found)
		return false;

	return repository_->DeleteCharacterByName(ownerRecord.account_id,
		storage::NameBytes(ch, NAME_LENGTH), &error) ? 1 : 0;
}

int CFileDB::GetIndex(int server, int id)
{
    int ret = server * MAX_USER + id;

	return ret;
}

int CFileDB::GetIndex(char *account)
{
	int i;

	for(i = 0; i < MAX_DBACCOUNT; i++)
	{
		if(pAccountList[i].Login == 0) 
			continue;
		
		if(strcmp(pAccountList[i].File.Info.AccountName, account) == 0)
			return i;
	}

	return 0;
}

int CFileDB::GetAccountsByMac(int *Mac)
{
	int i;

	int num = 0;

	for (i = 0; i < MAX_DBACCOUNT; i++)
	{
		if (pAccountList[i].Login == 0)
			continue;

		if (memcmp(pAccountList[i].Mac, Mac, sizeof(pAccountList[i].Mac)) == 0)
			num++;
	}

	return num;
}

void CFileDB::SendDBSavingQuit(int Idx, int mode)
{
	int conn = Idx / MAX_USER;
	int id   = Idx % MAX_USER;

    MSG_DBSavingQuit sm;
	memset(&sm, 0, sizeof(MSG_DBSavingQuit));

	sm.Type = _MSG_DBSavingQuit;
	sm.ID = id;
	sm.Size = sizeof(MSG_DBSavingQuit);
	sm.Mode = mode;

    memcpy(sm.AccountName, pAccountList[Idx].File.Info.AccountName, ACCOUNTNAME_LENGTH);

	if(pUser[conn].cSock.IsOpen() && pUser[conn].Mode != USER_EMPTY)
		pUser[conn].cSock.SendOneMessage((char*)&sm, sizeof(MSG_DBSavingQuit));

    return;
}

int CFileDB::DBWriteAccount(STRUCT_ACCOUNTFILE *account)
{
	char *accname = account->Info.AccountName;

	char check[ACCOUNTNAME_LENGTH];

	strncpy(check, accname, ACCOUNTNAME_LENGTH - 1);
	check[ACCOUNTNAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;

	if (!repository_)
	{
		Log("err writeaccount storage not initialized", accname, 0);
		return false;
	}

	bool inserted = false;
	std::string error;

	if (!repository_->SaveAccountFile(*account, true, &inserted, &error))
	{
		Log(error, accname, 0);
		return false;
	}

	// Never retain credential material in the session record. The token keeps
	// its set/unset marker so the secure-token flow can still distinguish the
	// first binding.
	memset(account->Info.AccountPass, 0, ACCOUNTPASS_LENGTH);

	if (account->Info.NumericToken[0] != -1)
		memset(account->Info.NumericToken, 0, sizeof(account->Info.NumericToken));

	return true;
}

int CFileDB::DBExportAccount(STRUCT_ACCOUNTFILE *file)
{
	char check[ACCOUNTNAME_LENGTH];

	char *accname = file->Info.AccountName;

	strncpy(check, accname, ACCOUNTNAME_LENGTH - 1);
	check[ACCOUNTNAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;

	if (!repository_)
		return false;

	STRUCT_ACCOUNTFILE exported;
	bool found = false;
	std::string error;

	if (!repository_->ExportAccount(
			storage::NameBytes(accname, ACCOUNTNAME_LENGTH),
			&exported, &found, &error))
	{
		Log(error, accname, 0);
		return false;
	}

	if (!found)
		return false;

	char temp[128];

	sprintf(temp, "S:/export/account%d/%s", ServerIndex, check);

	platform::File exportFile;

	if(!exportFile.OpenReadWrite(temp))
		return false;

	if(!exportFile.Seek(0, SEEK_SET))
	{
		exportFile.Close();

		return false;
	}

	if(!exportFile.Write(&exported, sizeof(STRUCT_ACCOUNTFILE)))
	{
		exportFile.Close();

		return false;
	}

	exportFile.Close();

	return true;
}

int CFileDB::DBReadAccount(STRUCT_ACCOUNTFILE *file)
{
	if (file->Info.AccountName[0] == '\0')
		return false;

	char check[ACCOUNTNAME_LENGTH];

	strncpy(check, file->Info.AccountName, ACCOUNTNAME_LENGTH - 1);
	check[ACCOUNTNAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return false;

	if (!repository_)
	{
		Log("err readaccount storage not initialized", file->Info.AccountName, 0);
		return false;
	}

	STRUCT_ACCOUNTFILE loaded;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccountFile(*file, &loaded, &found, &error))
	{
		Log(error, file->Info.AccountName, 0);
		return false;
	}

	if (!found)
		return false;

	*file = loaded;
	return true;
}

int CFileDB::DBCheckImpleName(char **ilchat, char *Str)
{
     for (int j = 0; j < 7; j++)
	 {
		 for (int i = 0; i < ACCOUNTNAME_LENGTH; i++)
	     {
			 if (Str[i] == 0 || Str[i+1] == 0|| Str[i+2] == 0|| Str[i+3]== 0)
				 break;

             if (Str[i] == ' ' || Str[i+1] == ' ' || Str[i+2] == ' ' || Str[i+3] == ' ')
				 continue;

	         if (Str[i] == ilchat[j][0] && Str[i+1] == ilchat[j][1] && Str[i+2] == ilchat[j][2] && Str[i+3] == ilchat[j][3] && ilchat[j][4] == 0)
				 return true;

			 if (ilchat[j][i+4] == 0)
				 continue;

	         if (Str[i] == ilchat[j][0] && Str[i+1] == ilchat[j][1] && Str[i+2] == ilchat[j][2] && Str[i+3] == ilchat[j][3] && Str[i+4] == ilchat[j][4] && Str[i+5] == ilchat[j][5] && ilchat[j][6] == 0)
				 return true;

			 if (ilchat[j][i+6] == 0)
				 continue;

	         if (Str[i] == ilchat[j][0] && Str[i+1] == ilchat[j][1] && Str[i+2] == ilchat[j][2] && Str[i+3] == ilchat[j][3] && Str[i+4] == ilchat[j][4] && Str[i+5] == ilchat[j][5] && Str[i+6] == ilchat[j][6] && Str[i+7] == ilchat[j][7] && ilchat[j][8] == 0)
				 return true;
		 }
	 }

	 return false;
}

void CFileDB::DBGetSelChar(STRUCT_SELCHAR *sel, STRUCT_ACCOUNTFILE *file)
{
	for(int i = 0; i < MOB_PER_ACCOUNT; i++)
	{
		memcpy(sel->Name[i], file->Char[i].MobName, NAME_LENGTH);
		memcpy(sel->Equip[i], file->Char[i].Equip, sizeof(file->Char[i].Equip));

		if (sel->Equip[i][0].sIndex == 22 || sel->Equip[i][0].sIndex == 23 || sel->Equip[i][0].sIndex == 24 || sel->Equip[i][0].sIndex == 25 || sel->Equip[i][0].sIndex == 32)
			sel->Equip[i][0].sIndex = file->mobExtra[i].ClassMaster == MORTAL ? 21 : file->mobExtra[i].MortalFace + 7;

		sel->Guild[i] = file->Char[i].Guild;

		sel->SPX[i] = file->Char[i].SPX;
		sel->SPX[i] = file->Char[i].SPY;

		sel->Score[i] = file->Char[i].CurrentScore;		

		sel->Coin[i] = file->Char[i].Coin;
		sel->Exp[i]  = file->Char[i].Exp;
	}
}

void CFileDB::GetAccountByChar(char *acc, char *cha)
{
	char check[NAME_LENGTH];

	strncpy(check, cha, NAME_LENGTH - 1);
	check[NAME_LENGTH - 1] = 0;

	UpcaseString(check);

	if(check[0] == 'C' && check[1] == 'O' && check[2] == 'M' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return;
	if(check[0] == 'L' && check[1] == 'P' && check[2] == 'T' && check[3] >= '0' && check[3] <= '9' && check[4] == 0)
		return;

	if (!repository_)
		return;

	std::vector<unsigned char> owner;
	bool found = false;
	std::string error;

	if (!repository_->FindAccountByCharacterName(
			storage::NameBytes(check, NAME_LENGTH), &owner, &found, &error))
	{
		Log(error, check, 0);
		return;
	}

	if (!found)
		return;

	memset(acc, 0, ACCOUNTNAME_LENGTH);

	size_t length = std::min<std::size_t>(owner.size(), ACCOUNTNAME_LENGTH - 1);

	if (length > 0)
		memcpy(acc, owner.data(), length);
}

bool CFileDB::InitStorage(const storage::StorageConfig &config, std::string *error)
{
	database_.reset(new storage::Database());

	if (!database_->Connect(config, error))
	{
		storageError_ = error != nullptr ? *error : "database connect failed";
		return false;
	}

	int version = 0;

	if (!storage::Migrate(*database_, error, &version))
	{
		storageError_ = error != nullptr ? *error : "schema migration failed";
		return false;
	}

	std::string importState;

	if (!storage::GetMetadata(*database_, "import_state", &importState, error))
	{
		storageError_ = error != nullptr ? *error : "cannot read import_state";
		return false;
	}

	if (importState != "complete")
	{
		storageError_ = "import_state is '" + importState
			+ "'; run dbimport until it reports complete before starting DBSrv";
		if (error != nullptr)
			*error = storageError_;
		return false;
	}

	repository_.reset(new storage::AccountRepository(*database_, config));

	return true;
}

int CFileDB::GetAccountState(const char *account)
{
	if (!repository_)
		return 0;

	storage::AccountRecord record;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(
			storage::NameBytes(account, ACCOUNTNAME_LENGTH),
			&record, &found, &error))
	{
		Log(error, account, 0);
		return 0;
	}

	return found ? record.state : 0;
}

bool CFileDB::SetAccountState(const char *account, int state)
{
	if (!repository_)
		return false;

	storage::AccountRecord record;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(
			storage::NameBytes(account, ACCOUNTNAME_LENGTH),
			&record, &found, &error))
	{
		Log(error, account, 0);
		return false;
	}

	if (!found)
		return false;

	return repository_->SetState(record.account_id, state, &error);
}

bool CFileDB::VerifyAccountPassword(const char *account, const char *password,
	bool *matched)
{
	if (matched != nullptr)
		*matched = false;

	if (!repository_)
		return false;

	storage::AccountRecord record;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(
			storage::NameBytes(account, ACCOUNTNAME_LENGTH),
			&record, &found, &error))
	{
		Log(error, account, 0);
		return false;
	}

	if (!found)
		return true;

	return repository_->VerifyPassword(record.account_id,
		security::SubmittedCredential(password, ACCOUNTPASS_LENGTH),
		matched, &error);
}

bool CFileDB::ChangeAccountPassword(const char *account, const char *password)
{
	if (!repository_)
		return false;

	storage::AccountRecord record;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(
			storage::NameBytes(account, ACCOUNTNAME_LENGTH),
			&record, &found, &error))
	{
		Log(error, account, 0);
		return false;
	}

	if (!found)
		return false;

	return repository_->ChangePassword(record.account_id,
		security::NewCredential(password, ACCOUNTPASS_LENGTH), &error);
}

long long CFileDB::GetAccountId(const char *account)
{
	if (!repository_)
		return 0;

	storage::AccountRecord record;
	bool found = false;
	std::string error;

	if (!repository_->LoadAccount(
			storage::NameBytes(account, ACCOUNTNAME_LENGTH),
			&record, &found, &error))
	{
		Log(error, account, 0);
		return 0;
	}

	return found ? record.account_id : 0;
}

void CFileDB::InitAccountList(int idx)
{	 
	 memset(&pAccountList[idx].File, 0, sizeof(pAccountList[idx].File));  
}

int  CFileDB::GetEncPassword(int idx, int *Enc)
{
	Enc[0] = rand() % 900 + 100;
	Enc[1] = rand() % 900 + 100;
	Enc[2] = rand() % 900 + 100;
	Enc[3] = rand() % 900 + 100;
	Enc[4] = rand() % 900 + 100;
	Enc[5] = rand() % 900 + 100;
	Enc[6] = rand() % 900 + 100;
	Enc[7] = rand() % 900 + 100;

	return false;
}

void CFileDB::SetEncPassword(int idx, int newsrv, int *Enc, int Unk)
{

}
