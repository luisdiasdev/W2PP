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

#include "CReadFiles.h"
#include "CFileDB.h"
#include "CUser.h"
#include "Server.h"

#include "CRanking.h"

// Included last: the asio headers pull in the Windows headers, which define
// DeleteFile as a macro. FileSystem.h undefines it so platform::DeleteFile
// resolves.
#include "../platform/FileSystem.h"

const char* const CReadFiles::UPDATE_CONNECTION_PATH = "C:/wamp/www/serv%2.2d.htm";
const char* const CReadFiles::UPDATE_CONNECTION_DATA_PATH = "../../Common/data%2.2d.csv";

const char* const CReadFiles::IMPORT_ITEM_PATH = "../../Common/ImportItem/*.*";
const char* const CReadFiles::IMPORT_ITEM2_PATH = "../../Common/ImportItem/%s";
const char* const CReadFiles::IMPORT_ITEM_ERROR_PATH = "../../Common/Error/%s";

const char* const CReadFiles::IMPORT_USER_PATH = "../../Common/ImportUser/*.*";
const char* const CReadFiles::IMPORT_USER2_PATH = "../../Common/ImportUser/%s";

const char* const CReadFiles::GUILD_INFO_PATH = "../../Common/GuildInfo";

const char* const CReadFiles::IMPORT_DONATE_PATH = "../../Common/ImportDonate/*.*";
const char* const CReadFiles::IMPORT_DONATE2_PATH = "../../Common/ImportDonate/%s";
const char* const CReadFiles::IMPORT_DONATE_ERROR_PATH = "../../Common/ImportDonateError/%s";

const char* const CReadFiles::UPDATE_USER_PATH = "../../Common/serv%2.2d/update/*.*";
const char* const CReadFiles::UPDATE_USER2_PATH = "../../Common/serv%2.2d/update/%s";

const char* const CReadFiles::RANKING_PATH = "../../Common/Ranking.txt";

void CReadFiles::UpdateConnection()
{
	char temp[256];

	sprintf(temp, UPDATE_CONNECTION_PATH, ServerIndex);

	FILE *fp = fopen(temp, "wt");

	if (fp == NULL)
		return;

	for (int i = 0; i < MAX_SERVER; i++)
	{
		if (pUser[i].Mode == USER_EMPTY)
		{
			fprintf(fp, "-1\n");
			continue;
		}

		if (UserConnection[i] < pUser[i].Count)
			UserConnection[i] = pUser[i].Count;

		fprintf(fp, "%d\n", ((4 * pUser[i].Count / 3) + rand() % 4));
	}

	fclose(fp);
}

void CReadFiles::UpdateConnectionData()
{
	char temp[256];

	sprintf(temp, UPDATE_CONNECTION_DATA_PATH, ServerIndex);

	FILE *fp = fopen(temp, "a+");

	if (fp == NULL)
		return;

	struct tm when;
	time_t now;
	time(&now);
	when = *localtime(&now);

	sprintf(temp, "%4.4d_%2.2d_%2.2d_%2.2d ", when.tm_year + 1900, when.tm_mon + 1, when.tm_mday, when.tm_hour);

	fprintf(fp, temp);

	if (fp == NULL)
		return;

	int total = 0;

	for (int i = 0; i < MAX_SERVER; i++)
	{
		int ret = UserConnection[i];

		total = total + ret;

		fprintf(fp, ",%4d  ", ret);

		UserConnection[i] = 0;
	}

	fprintf(fp, ",%4d  ", total);
	fprintf(fp, "\n");

	fclose(fp);
}

void CReadFiles::ImportItem()
{
	char temp[256];
	char filename[256];
	char movefile[256];
	char *getret;
	FILE *fp;

	platform::DirectoryIterator iterator(IMPORT_ITEM_PATH); platform::DirectoryEntry c_file;

	if (iterator.Result() != platform::FsResult::Ok)

	return;

	int count = 0;

	while (iterator.Next(c_file))
	{
		count++;

		if (count > 10)
			break;


		sprintf(filename, IMPORT_ITEM2_PATH, c_file.name.c_str());
		sprintf(movefile, IMPORT_ITEM_ERROR_PATH, c_file.name.c_str());

		fp = fopen(filename, "rt");

		if (fp == NULL)
		{
			continue;
		}

		char ids[ACCOUNTNAME_LENGTH] = { 0, };
		int Index = 0, Eff1 = 0, Val1 = 0, Eff2 = 0, Val2 = 0, Eff3 = 0, Val3 = 0;

		getret = fgets(temp, 256, fp);

		if (getret == 0)
		{
			fclose(fp);

			platform::RenameFile(filename, movefile);

			Log(filename, "-IMPORT no contents", 0);

			continue;
		}

		sscanf(temp, "%s %d %d %d %d %d %d %d  ", ids, &Index, &Eff1, &Val1, &Eff2, &Val2, &Eff3, &Val3);

		fclose(fp);

		Log(temp, "-IMPORT starting", 0);

		_strupr(ids);

		if (Index < 0 || Index >= MAX_ITEMLIST || Eff1 < 0 || Eff2 < 0 || Eff3 < 0 || Eff1 > 255 || Eff2 > 255 || Eff3 > 255)
		{
			platform::RenameFile(filename, movefile);

			Log(filename, "-IMPORT no contents", 0);

			continue;
		}

		STRUCT_ITEM tempsour;

		tempsour.sIndex = Index;
		tempsour.stEffect[0].cEffect = Eff1;
		tempsour.stEffect[0].cValue = Val1;
		tempsour.stEffect[1].cEffect = Eff2;
		tempsour.stEffect[1].cValue = Val2;
		tempsour.stEffect[2].cEffect = Eff3;
		tempsour.stEffect[2].cValue = Val3;

		int RunTimeSent = 0;

		int IdxName = cFileDB.GetIndex(ids);

		if (IdxName > 0 && IdxName < MAX_DBACCOUNT && cFileDB.pAccountList[IdxName].Slot >= 0 && cFileDB.pAccountList[IdxName].Slot <= 3)
		{
			int svr = IdxName / MAX_USER;
			int id = IdxName % MAX_USER;

			MSG_DBSendItem sm;
			memset(&sm, 0, sizeof(MSG_DBSendItem));

			sm.Type = _MSG_DBSendItem;
			sm.Size = sizeof(MSG_DBSendItem);
			sm.ID = id;

			strncpy(sm.Account, ids, ACCOUNTNAME_LENGTH);

			sm.Item = tempsour;

			pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(MSG_DBSendItem));

			Log(filename, "-IMPORT ITEM RUNTIME - sent", 0);
			RunTimeSent = 1;
		}

		STRUCT_ACCOUNTFILE file;

		memset(&file, 0, sizeof(STRUCT_ACCOUNTFILE));

		_strupr(ids);

		memcpy(file.Info.AccountName, ids, ACCOUNTNAME_LENGTH);

		int hread = cFileDB.DBReadAccount(&file);

		if (!hread)
		{
			if (!RunTimeSent)
				platform::RenameFile(filename, movefile);

			Log(filename, "-IMPORT no account", 0);

			continue;
		}

		int Pos = -1, i = 0;

		for (; i < 126; i++)
		{
			int retsour = BASE_CanCargo(&tempsour, file.Cargo, i % 9, i / 9); // 0041E0AF  |.  E8 D32FFEFF   ||CALL DBsrv.00401087

			if (retsour != 1)
				continue;

			Pos = i;

			break;
		}

		if (Pos == -1)
		{
			for (int i = 127; i >= 0; i--)
			{
				if (!file.Cargo[i].sIndex)
				{
					Pos = i;
					break;
				}
			}
		}

		if (Pos == -1)
		{
			platform::FsResult iret = platform::RenameFile(filename, movefile);

			if (iret != platform::FsResult::Ok)
			{
				Log(temp, "-IMPORT no space, move failed", 0);
			}

			Log(temp, "-IMPORT no space, file moved to err", 0);

			continue;
		}

		*(int*)&file.Cargo[Pos] = *(int*)&tempsour;
		*(int*)((int)&file.Cargo[Pos] + 4) = *(int*)((int)&tempsour + 4);

		int ret = cFileDB.DBWriteAccount(&file);

		if (!ret)
		{
			Log(filename, "-IMPORT ITEM SUCCESS - ACCOUNT SAVE FAIL", 0);

			if (!RunTimeSent)
				platform::RenameFile(filename, movefile);


			continue;
		}

		char log[256];

		sprintf(log, "-import item cargo %s(%d)", ids, i);

		Log(filename, log, 0);

		ret = (platform::DeleteFile(filename) == platform::FsResult::Ok) ? 1 : 0;

		if (!ret)
		{
			ret = (platform::RenameFile(filename, movefile) == platform::FsResult::Ok) ? 1 : 0;

			if (ret)
				Log(filename, "-IMPORT ITEM SUCCESS - FILE MOVED [WORNING]", 0);
			else
				Log(filename, "-IMPORT ITEM SUCCESS - FILE NOT DELETED!!!!!!!! [WORNING]", 0);
		}

		

	}

}

void CReadFiles::ImportUser()
{
	FILE *fp = NULL;

	char temp[128];

	platform::DirectoryIterator iterator(IMPORT_USER_PATH); platform::DirectoryEntry c_file;
	if (iterator.Result() != platform::FsResult::Ok)

	return;

	char *ret;

	while (iterator.Next(c_file))
	{
		char filename[256];
		char id[256];
		char pass[256];
		char name[256];
		char email[256];
		char tel[256];
		char addr[256];

		int ssn1;
		int ssn2;


		sprintf(filename, IMPORT_USER2_PATH, c_file.name.c_str());
		fp = fopen(filename, "rt");

		if (fp == NULL)
		{
			continue;
		}

		int len = 0;

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}

		sscanf(temp, "%s", id);

		len = strlen(id);

		if (len == 0 || len >= ACCOUNTNAME_LENGTH)
		{
			fclose(fp);

			continue;
		}

		_strupr(id);

		id[ACCOUNTNAME_LENGTH - 1] = 0;
		id[ACCOUNTNAME_LENGTH - 2] = 0;

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}

		sscanf(temp, "%s", pass);

		len = strlen(pass);

		if (len == 0 || len >= ACCOUNTPASS_LENGTH)
		{
			fclose(fp);

			continue;
		}

		pass[ACCOUNTPASS_LENGTH - 1] = 0;
		pass[ACCOUNTPASS_LENGTH - 2] = 0;

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}
		sscanf(temp, "%s", name);

		len = strlen(name);

		if (len == 0 || len >= REALNAME_LENGTH)
		{
			fclose(fp);

			continue;
		}

		name[REALNAME_LENGTH - 1] = 0;
		name[REALNAME_LENGTH - 2] = 0;

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}

		sscanf(temp, "%d", &ssn1);

		if (ssn1 == 0)
		{
			fclose(fp);

			continue;
		}

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}

		sscanf(temp, "%d", &ssn2);

		if (ssn2 == 0)
		{
			fclose(fp);

			continue;
		}

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}

		len = strlen(temp);

		temp[len - 1] = 0;

		email[EMAIL_LENGTH - 1] = 0;
		email[EMAIL_LENGTH - 2] = 0;

		len = strlen(temp);

		if (len == 0 || len >= EMAIL_LENGTH)
		{
			fclose(fp);

			continue;
		}

		strncpy(email, temp, EMAIL_LENGTH);

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}

		len = strlen(temp);

		temp[len - 1] = 0;

		tel[TELEPHONE_LENGTH - 1] = 0;
		tel[TELEPHONE_LENGTH - 2] = 0;

		len = strlen(temp);

		if (len == 0 || len >= TELEPHONE_LENGTH)
		{
			fclose(fp);

			continue;
		}

		strncpy(tel, temp, TELEPHONE_LENGTH);

		ret = fgets(temp, 127, fp);

		if (!ret)
		{
			fclose(fp);

			continue;
		}

		len = strlen(temp);

		temp[len - 1] = 0;

		addr[ADDRESS_LENGTH - 1] = 0;
		addr[ADDRESS_LENGTH - 2] = 0;

		len = strlen(temp);

		if (len == 0 || len > ADDRESS_LENGTH)
		{
			fclose(fp);

			continue;
		}

		strncpy(addr, temp, ADDRESS_LENGTH);

		int bonus = 0;

		strcpy(temp, "0");

		ret = fgets(temp, 127, fp);

		if (ret)
			sscanf(temp, "%d", &bonus);

		fclose(fp);

		int ret = cFileDB.AddAccount(id, pass, name, ssn1, ssn2, email, tel, addr, bonus);

		if (ret != 0)
			ret = (platform::DeleteFile(filename) == platform::FsResult::Ok) ? 1 : 0;

		continue;
	}

}

void CReadFiles::ReadGuildInfo()
{
	platform::File guildFile;

	if (!guildFile.OpenRead(GUILD_INFO_PATH))
	{
		platform::ReportWarning("no GuildInfo file", "BOOTING ERROR");

		return;
	}

	guildFile.Read(GuildInfo, sizeof(GuildInfo));
	guildFile.Close();
}

void CReadFiles::WriteGuildInfo()
{
	platform::File guildFile;

	if (!guildFile.OpenReadWrite(GUILD_INFO_PATH))
	{
		platform::ReportWarning("no GuildInfo file", "BOOTING ERROR");

		return;
	}

	guildFile.Write(GuildInfo, sizeof(GuildInfo));
	guildFile.Close();
}

void CReadFiles::ImportDonate()
{
	char temp[256];
	char filename[256];
	char movefile[256];
	char *getret;
	FILE *fp;

	platform::DirectoryIterator iterator(IMPORT_DONATE_PATH); platform::DirectoryEntry c_file;

	if (iterator.Result() != platform::FsResult::Ok)

	return;

	int count = 0;

	while (iterator.Next(c_file))
	{
		count++;

		if (count > 10)
			break;


		sprintf(filename, IMPORT_DONATE2_PATH, c_file.name.c_str());
		sprintf(movefile, IMPORT_DONATE_ERROR_PATH, c_file.name.c_str());

		fp = fopen(filename, "rt");

		if (fp == NULL)
		{
			continue;
		}

		char ids[ACCOUNTNAME_LENGTH] = { 0, };
		int Donate = 0;

		getret = fgets(temp, 256, fp);

		if (getret == 0)
		{
			fclose(fp);

			platform::RenameFile(filename, movefile);

			Log(filename, "-IMPORT no contents", 0);

			continue;
		}

		sscanf(temp, "%s %d ", ids, &Donate);

		fclose(fp);

		Log(temp, "-IMPORT DONATE starting", 0);

		_strupr(ids);

		if (Donate < 0)
		{
			platform::RenameFile(filename, movefile);

			Log(filename, "-IMPORT no contents", 0);

			continue;
		}

		int RunTimeSent = 0;

		int IdxName = cFileDB.GetIndex(ids);

		if (IdxName > 0 && IdxName < MAX_DBACCOUNT && cFileDB.pAccountList[IdxName].Slot >= 0 && cFileDB.pAccountList[IdxName].Slot <= 3)
		{
			int svr = IdxName / MAX_USER;
			int id = IdxName % MAX_USER;

			MSG_DBSendDonate sm;

			sm.Type = _MSG_DBSendDonate;
			sm.Size = sizeof(MSG_DBSendDonate);
			sm.ID = id;

			strncpy(sm.Account, ids, ACCOUNTNAME_LENGTH);

			sm.Donate = Donate;

			pUser[svr].cSock.SendOneMessage((char*)&sm, sizeof(MSG_DBSendDonate));

			Log(filename, "-IMPORT ITEM RUNTIME - sent", 0);
			RunTimeSent = 1;
		}

		STRUCT_ACCOUNTFILE file;

		memset(&file, 0, sizeof(STRUCT_ACCOUNTFILE));

		_strupr(ids);

		memcpy(file.Info.AccountName, ids, ACCOUNTNAME_LENGTH);

		int hread = cFileDB.DBReadAccount(&file);

		if (!hread)
		{
			if (!RunTimeSent)
				platform::RenameFile(filename, movefile);

			Log(filename, "-IMPORT no account", 0);

			continue;
		}

		int Pos = -1, i = 0;

		file.Donate += Donate;

		int ret = cFileDB.DBWriteAccount(&file);

		if (!ret)
		{
			Log(filename, "-IMPORT DONATE SUCCESS - ACCOUNT SAVE FAIL", 0);

			if (!RunTimeSent)
				platform::RenameFile(filename, movefile);


			continue;
		}

		char log[256];

		sprintf(log, "-import donate %s(%d)", ids, Donate);

		Log(filename, log, 0);

		ret = (platform::DeleteFile(filename) == platform::FsResult::Ok) ? 1 : 0;

		if (!ret)
		{
			ret = (platform::RenameFile(filename, movefile) == platform::FsResult::Ok) ? 1 : 0;

			if (ret)
				Log(filename, "-IMPORT DONATE SUCCESS - FILE MOVED [WARNING]", 0);
			else
				Log(filename, "-IMPORT DONATE SUCCESS - FILE NOT DELETED!!!!!!!! [WARNING]", 0);
		}

		

	}

}

void CReadFiles::UpdateUser()
{
	char temp[128];
	char filename[128];
	char *ret;
	FILE *fp;

	sprintf(temp, UPDATE_USER_PATH, ServerIndex); platform::DirectoryIterator iterator(temp); platform::DirectoryEntry c_file;

	if (iterator.Result() != platform::FsResult::Ok)

	return;

	while (iterator.Next(c_file))
	{
		char id[128];
		char pass[256];


		sprintf(filename, UPDATE_USER2_PATH, ServerIndex, c_file.name.c_str());

		fp = fopen(filename, "rt");

		if (fp != NULL)
		{
			int len = 0;

			ret = fgets(temp, 127, fp);

			if (ret == 0)
			{
				fclose(fp);

				continue;
			}

			sscanf(temp, "%s", id);
			len = strlen(id);

			if (len == 0 || len >= ACCOUNTNAME_LENGTH)
			{
				fclose(fp);

				continue;
			}

			_strupr(id);
			id[ACCOUNTNAME_LENGTH - 1] = 0;
			id[ACCOUNTNAME_LENGTH - 2] = 0;

			ret = fgets(temp, 127, fp);
			if (ret == 0)
			{
				fclose(fp);

				continue;
			}
			sscanf(temp, "%s", pass);
			len = strlen(pass);

			if (len == 0 || len >= ACCOUNTPASS_LENGTH)
			{
				fclose(fp);

				continue;
			}

			pass[ACCOUNTPASS_LENGTH - 1] = 0;
			pass[ACCOUNTPASS_LENGTH - 2] = 0;

			fclose(fp);

			int ret = 0;

			ret = cFileDB.UpdateAccount(id, pass);

			if (ret)
			{
				ret = (platform::DeleteFile(filename) == platform::FsResult::Ok) ? 1 : 0;

				sprintf(temp, "AccountName:%s AccountPass:%s filename:%s", id, pass, filename);
				Log(temp, "-UPDATE USER SUCCESS - FILE DELETED [WARNING]", 0);
			}
			else
			{
				ret = (platform::DeleteFile(filename) == platform::FsResult::Ok) ? 1 : 0;

				sprintf(temp, "AccountName:%s AccountPass:%s filename:%s", id, pass, filename);
				Log(temp, "-UPDATE USER FAIL - FILE DELETED [WARNING]", 0);
			}
		}

		}

}

void CReadFiles::WriteRanking()
{
	FILE *fp = NULL;

	fp = fopen(RANKING_PATH, "wt");

	if (fp == NULL)
		return;

	char classmaster[6][64] = { "SEM CLASSMASTER", "M", "A", "C", "C", "SC" };
	char classe[4][64] = { "TK", "FM", "BM", "HT" };

	for (int i = RankPos::FIRST, pos = 1; i > RankPos::LAST; i--)
	{
		if (rankingSystem.grindRanking.getElement(i)->Name[0] != 0 && rankingSystem.grindRanking.getElement(i)->Level < 1000)
		{
			fprintf(fp, "%d %s %s %s\n", pos, rankingSystem.grindRanking.getElement(i)->Name,
				classmaster[rankingSystem.grindRanking.getElement(i)->ClassMaster],
				classe[rankingSystem.grindRanking.getElement(i)->Class]);
			pos++;
		}
	}
	fclose(fp);
}
