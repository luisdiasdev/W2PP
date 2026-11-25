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

#ifndef _CFILEDB_
#define _CFILEDB_

#include "../Basedef.h"

#include <memory>
#include <string>

namespace storage
{
class Database;
class AccountRepository;
struct StorageConfig;
}

typedef struct
{
     int Login;
     int Slot;
     STRUCT_ACCOUNTFILE File;
	 int SecurePass;

	 int Mac[4];

	 // In-memory identity/state for repository calls. Not part of any record
	 // or packet.
	 long long AccountId;
	 int State;
} STRUCT_ACCOUNTLIST;

int ProcessRecord(int conn, char * str);

class CFileDB
{
public:
	STRUCT_ACCOUNTLIST pAccountList[MAX_DBACCOUNT];

	CFileDB();
	~CFileDB();

	int  AddAccount				(char *id, char *pass, char *name, int ssn1, int ssn2, char *email, char *tel, char *addr, int bonus);
	int  UpdateAccount			(char *id, char *pass);

	int  ProcessMessage			(char *Msg, int conn);

	int  SendDBSignal			(int svr, unsigned short id, unsigned short signal);
	int  SendDBSignalParm		(int svr, unsigned short id, unsigned short signal, int parm);
	int  SendDBSignalParm2		(int svr, unsigned short id, unsigned short signal, int parm1, int parm2);
	int  SendDBSignalParm3		(int svr, unsigned short id, unsigned short signal, int parm1, int parm2, int parm3);

	int  SendDBMessage			(int svr, unsigned short id, char * Msg);

	void AddAccountList			(int Idx);
	void RemoveAccountList	    (int Idx);

	int  CreateCharacter		(char *ac, char *ch);
	int  DeleteCharacter		(char *ch, char *account);

	int  GetIndex               (int server ,int id);
	int	 GetIndex				(char *account);
	int  GetAccountsByMac		(int *Mac);

	void SendDBSavingQuit		(int Idx, int mode);

	int  DBWriteAccount         (STRUCT_ACCOUNTFILE *account);
	int  DBExportAccount		(STRUCT_ACCOUNTFILE *file);
	int  DBReadAccount          (STRUCT_ACCOUNTFILE *file);

	int  DBCheckImpleName		(char **ilchat, char * Str);

	void DBGetSelChar			(STRUCT_SELCHAR *sell, STRUCT_ACCOUNTFILE *file);	

	void GetAccountByChar		(char *acc, char *cha);
	
	void InitAccountList		(int idx);
	void SendGuildInfo			(int srv, int Guild);
	int  GetEncPassword			(int idx, int *Enc);
	void SetEncPassword			(int idx, int newsrv, int *Enc, int Unk);

	// PostgreSQL storage. InitStorage connects, migrates and prepares the
	// repository; every game/admin persistence call below goes through it.
	bool InitStorage			(const storage::StorageConfig &config, std::string *error);
	storage::AccountRepository *Repository() { return repository_.get(); }
	const std::string &StorageError() const { return storageError_; }

	// Credential/admin helpers backed by the repository.
	int  GetAccountState			(const char *account);
	bool SetAccountState			(const char *account, int state);
	bool VerifyAccountPassword		(const char *account, const char *password, bool *matched);
	bool ChangeAccountPassword		(const char *account, const char *password);
	long long GetAccountId		(const char *account);

private:
	std::unique_ptr<storage::Database> database_;
	std::unique_ptr<storage::AccountRepository> repository_;
	std::string storageError_;
};

extern STRUCT_GUILDINFO GuildInfo[65536];
extern unsigned short	g_pGuildWar[65536];
extern unsigned short	g_pGuildAlly[65536];
#endif
