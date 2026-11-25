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

#include "CRanking.h"
#include "CUser.h"
#include "Server.h"
#include "CFileDB.h"
#include "CReadFiles.h"
#include "../platform/FileSystem.h"
#include "../storage/AccountRepository.h"

#include <algorithm>
#include <string>
#include <vector>

#include <errno.h>

extern CFileDB cFileDB;

extern CUser pUser[MAX_SERVER]; // Server.cpp
extern CUser pAdmin[MAX_ADMIN]; // Server.cpp

//Initialize ranking system
RankingSystem rankingSystem;

GrindRanking::GrindRanking()
{
	for (int i = 0; i < MAX_RANK_INDEX; i++)
	{
		m_GrindRank[i] = new STRUCT_RANKING();
		m_GrindRankConnId[i] = GrindRankingConnId();
	}
}

void GrindRanking::setElement(int index, STRUCT_RANKING* target, bool deleteOlder)
{
	if (deleteOlder)
		delete m_GrindRank[index];

	m_GrindRank[index] = target;
}

STRUCT_RANKING* const GrindRanking::getElement(int index)
{
	if (index >= LAST && index <= FIRST)
		return m_GrindRank[index];
	else
		return NULL;
}

int GrindRanking::getElementIndex(char* name)
{
	if (name == NULL)
		return OUT_OF_RANK;

	int index = OUT_OF_RANK;

	for (int i = 0; i < MAX_RANK_INDEX; i++)
	{
		if (!strcmp(m_GrindRank[i]->Name, name))
		{
			index = i;
			break;
		}
	}

	return index;
}

void GrindRanking::setElementConnId(int rankId, GrindRankingConnId connId)
{
	if (rankId >= LAST && rankId <= FIRST)
	{
		m_GrindRankConnId[rankId] = connId;
	}
}

GrindRankingConnId GrindRanking::getElementConnId(int rankId)
{
	if (rankId >= LAST && rankId <= FIRST)
		return m_GrindRankConnId[rankId];
	else
		return GrindRankingConnId();
}

RankingSystem::RankingSystem()
{
	// The ranking is loaded from the database after storage initialization;
	// constructing the global must not query anything.
}

void RankingSystem::loadRanking()
{
	unsigned int startClock = platform::MonotonicMs();
	int entryCount = 0;

	if (cFileDB.Repository() != nullptr)
	{
		std::vector<storage::AccountRepository::RankingEntry> entries;
		std::string error;

		if (cFileDB.Repository()->RankingEntries(&entries, &error))
		{
			for (const storage::AccountRepository::RankingEntry &entry : entries)
			{
				char name[NAME_LENGTH];
				memset(name, 0, sizeof(name));

				size_t length = std::min<std::size_t>(entry.name.size(),
					sizeof(name) - 1);

				if (length > 0)
					memcpy(name, entry.name.data(), length);

				tryInsertInRanking(OUT_OF_RANK, name, entry.exp,
					entry.class_master, entry.level, entry.clazz,
					NULL, NULL, NULL);

				entryCount++;
			}
		}
		else
		{
			Log(error, "-system", 0);
		}
	}

	unsigned int resultClock = platform::MonotonicMs() - startClock;

	char tt[1024];

	sprintf(tt, "Read %d ranking entries in %d.%d seconds.\n", entryCount, resultClock / 60, resultClock % 60);
	Log(tt, "-system", 0);

	CReadFiles::WriteRanking();
}

/*
	Try to insert the player (name, value) in the actual ranking
	
	Return the updated rank Id
*/
int RankingSystem::tryInsertInRanking(int rankId, char* name, long long value, int classvalue, int Level, int cls, GrindRankingConnId* connId, STRUCT_RANKING* olderLastRank, GrindRankingConnId* olderLastConnId)
{
	if (classvalue == MORTAL)
		classvalue--;

	else if (classvalue == ARCH)
		classvalue++;

	if (rankId == OUT_OF_RANK)
	{
		if (value > grindRanking.getElement(LAST)->Value && classvalue >= grindRanking.getElement(LAST)->ClassMaster || classvalue > grindRanking.getElement(LAST)->ClassMaster)
		{
			/*
				The player can enter the ranking, so the last player will be kicked out from the ranking.
				Here we save this older last player info and set the last player as being the new player.
			*/
			if (olderLastRank != NULL)
				*olderLastRank = *grindRanking.getElement(LAST);

			if (olderLastConnId != NULL)
				*olderLastConnId = grindRanking.getElementConnId(LAST);

			if (connId != NULL)
				grindRanking.setElementConnId(LAST, *connId);
			else
				grindRanking.setElementConnId(LAST, GrindRankingConnId());

			STRUCT_RANKING* newRank = new STRUCT_RANKING(name, value, classvalue, Level, cls);

			grindRanking.setElement(LAST, newRank);
			// ---

			/* Check if the new player can rise more in the ranking. */
			rankId = LAST;

			for (int i = rankId + 1; i < MAX_RANK_INDEX; i++)
			{
				if (grindRanking.getElement(rankId)->Value > grindRanking.getElement(i)->Value && grindRanking.getElement(rankId)->ClassMaster >= grindRanking.getElement(i)->ClassMaster || grindRanking.getElement(rankId)->ClassMaster > grindRanking.getElement(i)->ClassMaster)
				{
					riseRankingElement(rankId);
					rankId = i;
				}
				else
				{
					break;
				}
			}
			// ---
		}
	}

	return rankId;
}

/*
	Build and send the MSG_SendExpRanking packet to the player
*/
void RankingSystem::sendUpdateIndividualRank(int rankId, int tmsrvId, int playerId, STRUCT_RANKING* playerRank)
{
	if (rankId == OUT_OF_RANK || (rankId >= LAST && rankId <= FIRST))
	{
		if (tmsrvId >= 0 && tmsrvId <= MAX_SERVER && playerId >= 1 && playerId <= MAX_USER)
		{
			MSG_SendExpRanking rankPacket = MSG_SendExpRanking(playerId);

			rankPacket.RankPosition = rankId;

			if (rankId == OUT_OF_RANK) // Player is not in the ranking
			{
				rankPacket.PlayerAbove = *grindRanking.getElement(LAST);
			}
			else if (rankId == FIRST) // Player is the top 1 player
			{
				rankPacket.PlayerUnder = *grindRanking.getElement(rankId - 1);
			}
			else if (rankId == LAST) // Player is the last player in the ranking
			{
				rankPacket.PlayerAbove = *grindRanking.getElement(rankId + 1);
			}
			else if (rankId > LAST && rankId < FIRST) // Player is in the middle of the ranking
			{
				rankPacket.PlayerUnder = *grindRanking.getElement(rankId - 1);
				rankPacket.PlayerAbove = *grindRanking.getElement(rankId + 1);
			}

			if (playerRank != NULL)
			{
				rankPacket.PlayerRank = *playerRank;
			}

			if (pUser[tmsrvId].cSock.IsOpen())
				pUser[tmsrvId].cSock.SendOneMessage((char*)&rankPacket, sizeof(rankPacket));
		}
	}
}

/*
	Proper method to call sendUpdateIndividualRank to a direct ranking Ids range
*/
void RankingSystem::sendUpdateRangeRank(int initId, int endId)
{
	if (initId >= LAST && endId <= FIRST)
	{
		for (int i = initId - 1; i <= endId + 1; i++)
		{
			/* Check the possible "-1" and "MAX_RANK_INDEX" mathematical elements when "initId" is 0 (LAST) or/and "endId" is FIRST */
			if (i >= LAST && i <= FIRST)
			{
				GrindRankingConnId thisRankConnId = grindRanking.getElementConnId(i);

				if (thisRankConnId.TMSrvId >= 0 && thisRankConnId.TMSrvId <= MAX_SERVER && thisRankConnId.PlayerId >= 1 && thisRankConnId.PlayerId <= MAX_USER)
					sendUpdateIndividualRank(i, thisRankConnId.TMSrvId, thisRankConnId.PlayerId, grindRanking.getElement(i));
			}
		}
	}
}

/*
	Swap the rankId and (rankId+1) ranking elements
*/
void RankingSystem::riseRankingElement(int rankId)
{
	if (rankId >= LAST && rankId < FIRST)
	{
		/* Swap STRUCT_RANKING elements */
		STRUCT_RANKING* auxRank = grindRanking.getElement(rankId);
		grindRanking.setElement(rankId, grindRanking.getElement(rankId + 1), false);
		grindRanking.setElement(rankId + 1, auxRank, false);

		/* Swap the GrindRankingConnId elements */
		GrindRankingConnId auxConnId = grindRanking.getElementConnId(rankId);
		grindRanking.setElementConnId(rankId, grindRanking.getElementConnId(rankId + 1));
		grindRanking.setElementConnId(rankId + 1, auxConnId);
	}
}

/*
	Increase the rankId's value and check if he can rise up more.

	Returns the updated rank Id.
*/
int RankingSystem::increaseRankingElementValue(int rankId, int newValue, int classvalue)
{
	int newRankId = rankId;

	if (classvalue == MORTAL)
		classvalue--;

	else if (classvalue == ARCH)
		classvalue++;

	if (rankId >= LAST && rankId <= FIRST)
	{
		STRUCT_RANKING* thisRank = grindRanking.getElement(rankId);

		thisRank->Value = newValue;

		for (int i = newRankId + 1; i < MAX_RANK_INDEX; i++)
		{
			if (thisRank->Value > grindRanking.getElement(i)->Value && thisRank->ClassMaster <= classvalue || thisRank->ClassMaster > classvalue)
			{
				riseRankingElement(newRankId);
				newRankId = i;
			}
			else
				break;
		}
	}

	return newRankId;
}
