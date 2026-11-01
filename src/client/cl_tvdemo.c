/*
 * Wolfenstein: Enemy Territory GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
 *
 * ET: Legacy
 * Copyright (C) 2012-2024 ET:Legacy team <mail@etlegacy.com>
 *
 * This file is part of ET: Legacy - http://www.etlegacy.com
 *
 * ET: Legacy is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ET: Legacy is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ET: Legacy. If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, Wolfenstein: Enemy Territory GPL Source Code is also
 * subject to certain additional terms. You should have received a copy
 * of these additional terms immediately following the terms and conditions
 * of the GNU General Public License which accompanied the source code.
 * If not, please request a copy in writing from id Software at the address below.
 *
 * id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
 */
/**
 * @file cl_tvdemo.c
 * @brief Client side playback of tv server (ETTV) demos
 *
 * A tv demo holds every entity of the master server together with its entityShared_t
 * part and the playerstates of all clients. Instead of starting a local tv server, every
 * demo message is parsed here and transcoded into a regular server message for the
 * current point of view, so the client demo code (seeking, rewind, freecam) handles it
 * like any other demo.
 */

#include "client.h"

#define TV_MSGLEN               (MAX_MSGLEN * 16)
#define TV_MAX_ENT_CLUSTERS     16
#define TV_MAX_TOTAL_ENT_LEAFS  128
#define TV_MAX_PORTAL_DEPTH     2

/**
 * @struct tvSnapshot_t
 * @brief Snapshot of the master server as received by the tv server
 */
typedef struct
{
	qboolean valid;
	int snapFlags;
	int serverTime;
	int messageNum;
	int deltaNum;
	playerState_t ps;                   ///< playerstate of the tv server client on the master server
	int numEntities;
	int parseEntitiesNum;
} tvSnapshot_t;

/**
 * @enum tvStatsType_t
 * @brief Player stats the tv server requests from the master server
 */
typedef enum
{
	TV_STATS_WS,                        ///< "ws" reply to "weaponstats"
	TV_STATS_WWS,                       ///< "wws" reply to "wstats"
	TV_STATS_GSTATS,                    ///< "gstats" reply to "sgstats"
	TV_STATS_NUM
} tvStatsType_t;

/**
 * @enum tvRankType_t
 * @brief Weapon rankings the tv server requests from the master server
 */
typedef enum
{
	TV_RANK_ASTATS,
	TV_RANK_ASTATSB,
	TV_RANK_BSTATS,
	TV_RANK_BSTATSB,
	TV_RANK_WBSTATS,
	TV_RANK_NUM
} tvRankType_t;

static const char *tvStatsReplies[TV_STATS_NUM] = { "ws", "wws", "gstats" };
static const char *tvRankReplies[TV_RANK_NUM]   = { "astats", "astatsb", "bstats", "bstatsb", "wbstats" };

/**
 * @struct tvDemoState_t
 * @brief Everything that changes while reading the demo, a copy is kept with every rewind backup
 */
typedef struct
{
	tvSnapshot_t snap;                                              ///< latest valid snapshot
	tvSnapshot_t snapshots[PACKET_BACKUP];
	ettvClientSnapshot_t playerstates[PACKET_BACKUP][MAX_CLIENTS];  ///< playerstates of all clients for each snapshot

	entityState_t entityBaselines[MAX_GENTITIES];
	entityShared_t entityBaselinesShared[MAX_GENTITIES];

	int parseEntitiesNum;
	entityState_t parseEntities[MAX_PARSE_ENTITIES];
	entityShared_t parseEntitiesShared[MAX_PARSE_ENTITIES];

	int lastMasterCommand;                                          ///< last server command sequence read from the demo
	int commandSequence;                                            ///< last server command sequence given to the client

	// last transcoded snapshot, the next one is delta compressed from it
	qboolean outValid;
	int outMessageNum;
	playerState_t outPs;
	int outNumEntities;
	entityState_t outEntities[MAX_ENTITIES_IN_SNAPSHOT];

	// replies to requests of the tv server, given to the client when it asks for them
	qboolean statsValid[TV_STATS_NUM][MAX_CLIENTS];
	char stats[TV_STATS_NUM][MAX_CLIENTS][MAX_STRING_CHARS];
	char rankings[TV_RANK_NUM][MAX_STRING_CHARS];
} tvDemoState_t;

/**
 * @struct tvEntityClusters_t
 * @brief PVS clusters of an entity, recomputed only when it moves
 */
typedef struct
{
	qboolean valid;
	qboolean bmodel;
	vec3_t origin, angles, mins, maxs;
	int numClusters;
	int clusternums[TV_MAX_ENT_CLUSTERS];
	int lastCluster;
	int originCluster;
} tvEntityClusters_t;

/**
 * @struct tvDemo_t
 * @brief tv demo playback, kept as it is on rewind
 */
typedef struct
{
	qboolean active;
	tvDemoState_t *state;
	int followClient;                                               ///< -1 to view from the tv server point of view

	tvEntityClusters_t clusters[MAX_GENTITIES];
	byte outData[TV_MSGLEN];
} tvDemo_t;

static tvDemo_t tvd;

/**
 * @brief CL_TV_Active
 * @return qtrue if a tv demo is being played
 */
qboolean CL_TV_Active(void)
{
	return tvd.active;
}

/**
 * @brief Prepare playback of a tv demo
 */
void CL_TV_Start(void)
{
	CL_TV_Stop();

	tvd.state = (tvDemoState_t *)Com_Allocate(sizeof(tvDemoState_t));
	if (!tvd.state)
	{
		Com_Error(ERR_FATAL, "CL_TV_Start: couldn't allocate %.2f MB for tv demo playback\n", sizeof(tvDemoState_t) / 1024.0 / 1024.0);
	}

	CL_TV_ResetState();

	tvd.active       = qtrue;
	tvd.followClient = -1;

	// lets cgame offer following players
	Cvar_Set("cl_tvdemo", "1");
}

/**
 * @brief CL_TV_Stop
 */
void CL_TV_Stop(void)
{
	if (tvd.state)
	{
		Com_Dealloc(tvd.state);
		tvd.state = NULL;
	}

	if (tvd.active)
	{
		Cvar_Set("cl_tvdemo", "0");
	}

	tvd.active = qfalse;
}

/**
 * @brief Clear the parsed demo data so the demo can be read again from its start
 */
void CL_TV_ResetState(void)
{
	if (tvd.state)
	{
		Com_Memset(tvd.state, 0, sizeof(tvDemoState_t));
	}

	Com_Memset(tvd.clusters, 0, sizeof(tvd.clusters));
}

/**
 * @brief Copy the parsed demo data into a rewind backup
 * @param[in] backup previous backup to overwrite or NULL
 * @return backup
 */
void *CL_TV_SaveState(void *backup)
{
	if (!tvd.state)
	{
		return backup;
	}

	if (!backup)
	{
		backup = Com_Allocate(sizeof(tvDemoState_t));
		if (!backup)
		{
			Com_Error(ERR_FATAL, "CL_TV_SaveState: couldn't allocate %.2f MB for tv demo rewind backup\n", sizeof(tvDemoState_t) / 1024.0 / 1024.0);
		}
	}

	Com_Memcpy(backup, tvd.state, sizeof(tvDemoState_t));

	return backup;
}

/**
 * @brief Restore the parsed demo data from a rewind backup
 * @param[in] backup
 */
void CL_TV_RestoreState(const void *backup)
{
	if (!tvd.state || !backup)
	{
		return;
	}

	Com_Memcpy(tvd.state, backup, sizeof(tvDemoState_t));
}

/**
 * @brief CL_TV_FreeState
 * @param[in] backup
 */
void CL_TV_FreeState(void *backup)
{
	if (backup)
	{
		Com_Dealloc(backup);
	}
}

/**
 * @brief Check if a demo was recorded by a tv server, whatever its file extension is
 *
 * @details Baselines in the gamestate of a tv demo are followed by their entityShared_t
 *          part, which starts with a magic byte (see MSG_ETTV_WriteDeltaEntityShared).
 *
 * @param[in] f demo file, its read position is restored
 * @return
 */
qboolean CL_TV_IsTVDemo(fileHandle_t f)
{
	msg_t         msg;
	byte          data[MAX_MSGLEN];
	entityState_t nullstate, state;
	int           pos, seq, len, cmd, newnum;
	qboolean      tvDemo = qfalse;

	pos = FS_FTell(f);

	if (FS_Read(&seq, 4, f) != 4 || FS_Read(&len, 4, f) != 4)
	{
		(void) FS_Seek(f, pos, FS_SEEK_SET);
		return qfalse;
	}

	len = LittleLong(len);

	if (len <= 0 || len > MAX_MSGLEN || FS_Read(data, len, f) != len)
	{
		(void) FS_Seek(f, pos, FS_SEEK_SET);
		return qfalse;
	}

	MSG_Init(&msg, data, sizeof(data));
	msg.cursize = len;
	MSG_Bitstream(&msg);

	// reliable sequence acknowledge number
	MSG_ReadLong(&msg);

	// the gamestate can follow server commands
	do
	{
		cmd = MSG_ReadByte(&msg);

		if (cmd == svc_serverCommand)
		{
			MSG_ReadLong(&msg);
			MSG_ReadString(&msg);
		}
	}
	while (cmd == svc_serverCommand && msg.readcount <= msg.cursize);

	if (cmd == svc_gamestate)
	{
		// server command sequence
		MSG_ReadLong(&msg);

		do
		{
			cmd = MSG_ReadByte(&msg);

			if (cmd == svc_configstring)
			{
				MSG_ReadShort(&msg);
				MSG_ReadBigString(&msg);
			}
		}
		while (cmd == svc_configstring && msg.readcount <= msg.cursize);

		if (cmd == svc_baseline)
		{
			newnum = MSG_ReadBits(&msg, GENTITYNUM_BITS);

			if (newnum >= 0 && newnum < MAX_GENTITIES)
			{
				Com_Memset(&nullstate, 0, sizeof(nullstate));
				MSG_ReadDeltaEntity(&msg, &nullstate, &state, newnum);

				// a regular demo continues with the next gamestate command
				tvDemo = MSG_ReadBits(&msg, 8) == 0x77;
			}
		}
		else
		{
			tvDemo = cmd == svc_ettv_currentstate;
		}
	}

	(void) FS_Seek(f, pos, FS_SEEK_SET);

	return tvDemo;
}

/*
=========================================================================
TV DEMO PARSING
=========================================================================
*/

/**
 * @brief Parses deltas from the given base and adds the resulting entity to the current frame
 * @param[in] msg
 * @param[in,out] frame
 * @param[in] newnum
 * @param[in] old
 * @param[in] oldShared
 * @param[in] unchanged
 */
static void CL_TV_DeltaEntity(msg_t *msg, tvSnapshot_t *frame, int newnum, entityState_t *old, entityShared_t *oldShared, qboolean unchanged)
{
	tvDemoState_t  *tv          = tvd.state;
	entityState_t  *state       = &tv->parseEntities[tv->parseEntitiesNum & (MAX_PARSE_ENTITIES - 1)];
	entityShared_t *stateShared = &tv->parseEntitiesShared[tv->parseEntitiesNum & (MAX_PARSE_ENTITIES - 1)];

	if (unchanged)
	{
		*state       = *old;
		*stateShared = *oldShared;
	}
	else
	{
		MSG_ReadDeltaEntity(msg, old, state, newnum);
		MSG_ETTV_ReadDeltaEntityShared(msg, oldShared, stateShared);
	}

	if (state->number == (MAX_GENTITIES - 1))
	{
		return;     // entity was delta removed
	}

	tv->parseEntitiesNum++;
	frame->numEntities++;
}

/**
 * @brief CL_TV_ParsePacketEntities
 * @param[in] msg
 * @param[in] oldframe
 * @param[out] newframe
 */
static void CL_TV_ParsePacketEntities(msg_t *msg, tvSnapshot_t *oldframe, tvSnapshot_t *newframe)
{
	tvDemoState_t  *tv             = tvd.state;
	entityState_t  *oldstate       = NULL;
	entityShared_t *oldstateShared = NULL;
	int            oldindex        = 0;
	int            newnum, oldnum;

	newframe->parseEntitiesNum = tv->parseEntitiesNum;
	newframe->numEntities      = 0;

	// delta from the entities present in oldframe
	if (!oldframe || oldframe->numEntities <= 0)
	{
		oldnum = MAX_GENTITIES;
	}
	else
	{
		oldstate       = &tv->parseEntities[oldframe->parseEntitiesNum & (MAX_PARSE_ENTITIES - 1)];
		oldstateShared = &tv->parseEntitiesShared[oldframe->parseEntitiesNum & (MAX_PARSE_ENTITIES - 1)];
		oldnum         = oldstate->number;
	}

	while (1)
	{
		// read the entity index number
		newnum = MSG_ReadBits(msg, GENTITYNUM_BITS);

		if (newnum >= (MAX_GENTITIES - 1))
		{
			break;
		}

		if (msg->readcount > msg->cursize)
		{
			Com_Error(ERR_DROP, "CL_TV_ParsePacketEntities: end of message");
		}

		while (oldnum < newnum)
		{
			// one or more entities from the old packet are unchanged
			CL_TV_DeltaEntity(msg, newframe, oldnum, oldstate, oldstateShared, qtrue);

			oldindex++;

			if (oldindex >= oldframe->numEntities)
			{
				oldnum = MAX_GENTITIES;
			}
			else
			{
				oldstate       = &tv->parseEntities[(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES - 1)];
				oldstateShared = &tv->parseEntitiesShared[(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES - 1)];
				oldnum         = oldstate->number;
			}
		}

		if (oldnum == newnum)
		{
			// delta from previous state
			CL_TV_DeltaEntity(msg, newframe, newnum, oldstate, oldstateShared, qfalse);

			oldindex++;

			if (oldindex >= oldframe->numEntities)
			{
				oldnum = MAX_GENTITIES;
			}
			else
			{
				oldstate       = &tv->parseEntities[(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES - 1)];
				oldstateShared = &tv->parseEntitiesShared[(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES - 1)];
				oldnum         = oldstate->number;
			}
		}
		else if (oldnum > newnum)
		{
			// delta from baseline
			CL_TV_DeltaEntity(msg, newframe, newnum, &tv->entityBaselines[newnum], &tv->entityBaselinesShared[newnum], qfalse);
		}
	}

	// any remaining entities in the old frame are copied over
	while (oldnum != MAX_GENTITIES)
	{
		CL_TV_DeltaEntity(msg, newframe, oldnum, oldstate, oldstateShared, qtrue);

		oldindex++;

		if (oldindex >= oldframe->numEntities)
		{
			oldnum = MAX_GENTITIES;
		}
		else
		{
			oldstate       = &tv->parseEntities[(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES - 1)];
			oldstateShared = &tv->parseEntitiesShared[(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES - 1)];
			oldnum         = oldstate->number;
		}
	}
}

/**
 * @brief CL_TV_ParseSnapshot
 * @param[in] msg
 * @return qtrue if the snapshot is valid and became the latest one
 */
static qboolean CL_TV_ParseSnapshot(msg_t *msg)
{
	tvDemoState_t *tv = tvd.state;
	tvSnapshot_t  *old;
	tvSnapshot_t  newSnap;
	byte          areamask[MAX_MAP_AREA_BYTES];
	int           len, deltaNum, oldMessageNum, i;

	Com_Memset(&newSnap, 0, sizeof(newSnap));

	newSnap.serverTime = MSG_ReadLong(msg);
	newSnap.messageNum = clc.serverMessageSequence;

	deltaNum = MSG_ReadByte(msg);
	if (!deltaNum)
	{
		newSnap.deltaNum = -1;
	}
	else
	{
		newSnap.deltaNum = newSnap.messageNum - deltaNum;
	}
	newSnap.snapFlags = MSG_ReadByte(msg);

	if (newSnap.deltaNum <= 0)
	{
		newSnap.valid = qtrue;      // uncompressed frame
		old           = NULL;
	}
	else
	{
		old = &tv->snapshots[newSnap.deltaNum & PACKET_MASK];
		if (!old->valid)
		{
			Com_DPrintf("CL_TV_ParseSnapshot: Delta from invalid frame.\n");
		}
		else if (old->messageNum != newSnap.deltaNum)
		{
			Com_DPrintf("CL_TV_ParseSnapshot: Delta frame too old.\n");
		}
		else if (tv->parseEntitiesNum - old->parseEntitiesNum > MAX_PARSE_ENTITIES - 128)
		{
			Com_DPrintf("CL_TV_ParseSnapshot: Delta parseEntitiesNum too old.\n");
		}
		else
		{
			newSnap.valid = qtrue;  // valid delta parse
		}
	}

	// areamask of the tv server point of view, transcoded snapshots don't use it
	len = MSG_ReadByte(msg);

	if (len < 0 || len > sizeof(areamask))
	{
		Com_Error(ERR_DROP, "CL_TV_ParseSnapshot: Invalid size %d for areamask.", len);
	}

	MSG_ReadData(msg, areamask, len);

	if (old)
	{
		MSG_ReadDeltaPlayerstate(msg, &old->ps, &newSnap.ps);
	}
	else
	{
		MSG_ReadDeltaPlayerstate(msg, NULL, &newSnap.ps);
	}

	CL_TV_ParsePacketEntities(msg, old, &newSnap);

	// if not valid, dump the entire thing now that it has
	// been properly read
	if (!newSnap.valid)
	{
		return qfalse;
	}

	// clear the valid flags of any snapshots between the last
	// received and this one
	oldMessageNum = tv->snap.messageNum + 1;

	if (newSnap.messageNum - oldMessageNum >= PACKET_BACKUP)
	{
		oldMessageNum = newSnap.messageNum - (PACKET_BACKUP - 1);
	}
	for ( ; oldMessageNum < newSnap.messageNum; oldMessageNum++)
	{
		tv->snapshots[oldMessageNum & PACKET_MASK].valid = qfalse;
	}

	tv->snap                                          = newSnap;
	tv->snapshots[newSnap.messageNum & PACKET_MASK] = newSnap;

	// playerstates of this snapshot follow in svc_ettv_playerstates
	for (i = 0; i < MAX_CLIENTS; i++)
	{
		tv->playerstates[newSnap.messageNum & PACKET_MASK][i].valid = qfalse;
	}

	return qtrue;
}

/**
 * @brief Read the playerstates of all clients for the latest snapshot
 * @param[in] msg
 * @param[in] snapValid qfalse if the snapshot of this message was invalid, the playerstates are only read then
 */
static void CL_TV_ParsePlayerstates(msg_t *msg, qboolean snapValid)
{
	tvDemoState_t        *tv       = tvd.state;
	ettvClientSnapshot_t *frame    = NULL;
	ettvClientSnapshot_t *oldframe = NULL;
	playerState_t        discard;
	int                  clientNum;

	if (snapValid)
	{
		frame = tv->playerstates[tv->snap.messageNum & PACKET_MASK];

		if (tv->snap.deltaNum != -1)
		{
			oldframe = tv->playerstates[tv->snap.deltaNum & PACKET_MASK];
		}
	}

	while (1)
	{
		clientNum = MSG_ReadByte(msg);

		if (clientNum == 255)
		{
			break;
		}

		if (msg->readcount > msg->cursize)
		{
			Com_Error(ERR_DROP, "CL_TV_ParsePlayerstates: end of message");
		}

		if (clientNum < 0 || clientNum >= MAX_CLIENTS)
		{
			Com_Error(ERR_DROP, "CL_TV_ParsePlayerstates: bad client number %i", clientNum);
		}

		if (!frame)
		{
			MSG_ReadDeltaPlayerstate(msg, NULL, &discard);
		}
		else if (!oldframe || !oldframe[clientNum].valid)
		{
			MSG_ReadDeltaPlayerstate(msg, NULL, &frame[clientNum].ps);
			frame[clientNum].valid = qtrue;
		}
		else
		{
			MSG_ReadDeltaPlayerstate(msg, &oldframe[clientNum].ps, &frame[clientNum].ps);
			frame[clientNum].valid = qtrue;
		}
	}
}

/**
 * @brief Read the tv demo gamestate and write a regular gamestate
 * @param[in] msg
 * @param[out] out
 */
static void CL_TV_ParseGamestate(msg_t *msg, msg_t *out)
{
	tvDemoState_t  *tv = tvd.state;
	entityState_t  nullstate, currentState;
	entityShared_t nullstateShared, currentStateShared;
	int            i, cmd, newnum, clientNum, checksumFeed;
	char           *s;

	// wipe everything of the previous level
	CL_TV_ResetState();

	Com_Memset(&nullstate, 0, sizeof(nullstate));
	Com_Memset(&nullstateShared, 0, sizeof(nullstateShared));

	// a gamestate always marks a server command sequence
	tv->lastMasterCommand = MSG_ReadLong(msg);
	tv->commandSequence   = tv->lastMasterCommand;

	MSG_WriteByte(out, svc_gamestate);
	MSG_WriteLong(out, tv->commandSequence);

	while (1)
	{
		cmd = MSG_ReadByte(msg);

		if (cmd == svc_EOF)
		{
			break;
		}

		if (cmd == svc_configstring)
		{
			i = MSG_ReadShort(msg);
			if (i < 0 || i >= MAX_CONFIGSTRINGS)
			{
				Com_Error(ERR_DROP, "CL_TV_ParseGamestate: configstring < 0 or configstring >= MAX_CONFIGSTRINGS");
			}
			s = MSG_ReadBigString(msg);

			MSG_WriteByte(out, svc_configstring);
			MSG_WriteShort(out, i);
			MSG_WriteBigString(out, s);
		}
		else if (cmd == svc_baseline)
		{
			newnum = MSG_ReadBits(msg, GENTITYNUM_BITS);
			if (newnum < 0 || newnum >= MAX_GENTITIES)
			{
				Com_Error(ERR_DROP, "CL_TV_ParseGamestate: Baseline number out of range: %i", newnum);
			}
			MSG_ReadDeltaEntity(msg, &nullstate, &tv->entityBaselines[newnum], newnum);
			MSG_ETTV_ReadDeltaEntityShared(msg, &nullstateShared, &tv->entityBaselinesShared[newnum]);
		}
		else if (cmd == svc_ettv_currentstate)
		{
			// entity states when the recording started, the first snapshot is never delta compressed
			newnum = MSG_ReadBits(msg, GENTITYNUM_BITS);
			if (newnum < 0 || newnum >= MAX_GENTITIES)
			{
				Com_Error(ERR_DROP, "CL_TV_ParseGamestate: Currentstate number out of range: %i", newnum);
			}
			MSG_ReadDeltaEntity(msg, &tv->entityBaselines[newnum], &currentState, newnum);
			MSG_ETTV_ReadDeltaEntityShared(msg, &tv->entityBaselinesShared[newnum], &currentStateShared);
		}
		else
		{
			Com_Error(ERR_DROP, "CL_TV_ParseGamestate: bad command byte");
		}
	}

	clientNum    = MSG_ReadLong(msg);
	checksumFeed = MSG_ReadLong(msg);

	// baselines, the transcoded snapshots delta from them, see CL_TV_OutBaseline
	for (i = 0; i < MAX_GENTITIES; i++)
	{
		if (!tv->entityBaselines[i].number)
		{
			continue;
		}

		MSG_WriteByte(out, svc_baseline);
		MSG_WriteDeltaEntity(out, &nullstate, &tv->entityBaselines[i], qtrue);
	}

	MSG_WriteByte(out, svc_EOF);

	MSG_WriteLong(out, clientNum);
	MSG_WriteLong(out, checksumFeed);
}

/**
 * @brief Keep replies to requests of the tv server, the client gets them when it asks for them
 * @param[in] s server command
 * @return qtrue if the command must not be given to the client
 */
static qboolean CL_TV_CacheMasterCommand(const char *s)
{
	tvDemoState_t *tv = tvd.state;
	char          buf[MAX_STRING_CHARS];
	char          *p = buf;
	char          *token;
	int           i, clientNum;

	Q_strncpyz(buf, s, sizeof(buf));
	token = COM_Parse(&p);

	for (i = 0; i < TV_STATS_NUM; i++)
	{
		if (!Q_stricmp(token, tvStatsReplies[i]))
		{
			token = COM_Parse(&p);

			if (Q_ParseInt(token, &clientNum) && clientNum >= 0 && clientNum < MAX_CLIENTS)
			{
				tv->statsValid[i][clientNum] = qtrue;
				Q_strncpyz(tv->stats[i][clientNum], s, sizeof(tv->stats[i][clientNum]));
			}
			return qtrue;
		}
	}

	for (i = 0; i < TV_RANK_NUM; i++)
	{
		if (!Q_stricmp(token, tvRankReplies[i]))
		{
			Q_strncpyz(tv->rankings[i], s, sizeof(tv->rankings[i]));
			return qtrue;
		}
	}

	// own stats of the tv server client and match stats it requested, like the tv server does not pass them
	if (!Q_stricmp(token, "WeaponStats") || !Q_stricmp(token, "sc"))
	{
		return qtrue;
	}

	return qfalse;
}

/**
 * @brief Server commands are given to the client renumbered, so replies to its requests can be added
 * @param[in] msg
 * @param[out] out
 */
static void CL_TV_ParseCommandString(msg_t *msg, msg_t *out)
{
	tvDemoState_t *tv = tvd.state;
	int           seq;
	char          *s;

	seq = MSG_ReadLong(msg);
	s   = MSG_ReadString(msg);

	// reliable commands are repeated until the tv server acknowledges them
	if (tv->lastMasterCommand >= seq)
	{
		return;
	}
	tv->lastMasterCommand = seq;

	if (CL_TV_CacheMasterCommand(s))
	{
		return;
	}

	MSG_WriteByte(out, svc_serverCommand);
	MSG_WriteLong(out, ++tv->commandSequence);
	MSG_WriteString(out, s);
}

/*
=========================================================================
TRANSCODED SNAPSHOTS
=========================================================================
*/

/**
 * @brief Can the client be followed in the latest snapshot
 * @param[in] clientNum
 * @return
 */
static qboolean CL_TV_CanFollow(int clientNum)
{
	tvDemoState_t        *tv = tvd.state;
	ettvClientSnapshot_t *player;

	if (clientNum < 0 || clientNum >= MAX_CLIENTS || !tv->snap.valid)
	{
		return qfalse;
	}

	player = &tv->playerstates[tv->snap.messageNum & PACKET_MASK][clientNum];

	if (!player->valid || (player->ps.pm_flags & (PMF_LIMBO | PMF_FOLLOW)))
	{
		return qfalse;
	}

	return player->ps.persistant[PERS_TEAM] == TEAM_AXIS || player->ps.persistant[PERS_TEAM] == TEAM_ALLIES;
}

/**
 * @brief Client whose playerstate is shown in the latest snapshot
 * @return client number or -1 for the tv server point of view
 */
static int CL_TV_ViewClient(void)
{
	tvDemoState_t *tv = tvd.state;

	if (tvd.followClient < 0 || tvd.followClient >= MAX_CLIENTS)
	{
		return -1;
	}

	// intermission is viewed from the tv server point of view
	if (tv->snap.ps.pm_type == PM_INTERMISSION)
	{
		return -1;
	}

	if (!tv->playerstates[tv->snap.messageNum & PACKET_MASK][tvd.followClient].valid)
	{
		return -1;
	}

	return tvd.followClient;
}

/**
 * @brief Follow next or previous player
 * @param[in] dir
 */
static void CL_TV_FollowCycle(int dir)
{
	int clientNum = tvd.followClient;
	int i;

	if (clientNum < 0)
	{
		clientNum = (dir > 0) ? -1 : MAX_CLIENTS;
	}

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		clientNum += dir;

		if (clientNum >= MAX_CLIENTS)
		{
			clientNum = 0;
		}
		else if (clientNum < 0)
		{
			clientNum = MAX_CLIENTS - 1;
		}

		if (CL_TV_CanFollow(clientNum))
		{
			tvd.followClient = clientNum;
			return;
		}
	}

	// leave it where it was
}

/**
 * @brief PVS clusters of an entity, see SV_LinkEntity
 * @param[in] es
 * @param[in] sh
 * @return
 */
static const tvEntityClusters_t *CL_TV_EntityClusters(const entityState_t *es, const entityShared_t *sh)
{
	tvEntityClusters_t *ec    = &tvd.clusters[es->number];
	qboolean           bmodel = es->solid == SOLID_BMODEL;
	vec3_t             absmin, absmax;
	int                leafs[TV_MAX_TOTAL_ENT_LEAFS];
	int                num_leafs, lastLeaf, cluster, i;

	if (ec->valid && ec->bmodel == bmodel &&
	    VectorCompare(ec->origin, sh->currentOrigin) && VectorCompare(ec->angles, sh->currentAngles) &&
	    VectorCompare(ec->mins, sh->mins) && VectorCompare(ec->maxs, sh->maxs))
	{
		return ec;
	}

	ec->valid  = qtrue;
	ec->bmodel = bmodel;
	VectorCopy(sh->currentOrigin, ec->origin);
	VectorCopy(sh->currentAngles, ec->angles);
	VectorCopy(sh->mins, ec->mins);
	VectorCopy(sh->maxs, ec->maxs);

	ec->numClusters   = 0;
	ec->lastCluster   = 0;
	ec->originCluster = CM_LeafCluster(CM_PointLeafnum(sh->currentOrigin));

	// set the abs box
	if (bmodel && (sh->currentAngles[0] != 0.f || sh->currentAngles[1] != 0.f || sh->currentAngles[2] != 0.f))
	{
		// expand for rotation
		float max = RadiusFromBounds(sh->mins, sh->maxs);

		for (i = 0; i < 3; i++)
		{
			absmin[i] = sh->currentOrigin[i] - max;
			absmax[i] = sh->currentOrigin[i] + max;
		}
	}
	else
	{
		VectorAdd(sh->currentOrigin, sh->mins, absmin);
		VectorAdd(sh->currentOrigin, sh->maxs, absmax);
	}

	for (i = 0; i < 3; i++)
	{
		absmin[i] -= 1;
		absmax[i] += 1;
	}

	num_leafs = CM_BoxLeafnums(absmin, absmax, leafs, TV_MAX_TOTAL_ENT_LEAFS, &lastLeaf);

	// outside the world
	if (!num_leafs)
	{
		return ec;
	}

	for (i = 0; i < num_leafs; i++)
	{
		cluster = CM_LeafCluster(leafs[i]);
		if (cluster != -1)
		{
			ec->clusternums[ec->numClusters++] = cluster;
			if (ec->numClusters == TV_MAX_ENT_CLUSTERS)
			{
				break;
			}
		}
	}

	// store off a last cluster if we need to
	if (i != num_leafs)
	{
		ec->lastCluster = CM_LeafCluster(lastLeaf);
	}

	return ec;
}

/**
 * @brief CL_TV_ClustersVisible
 * @param[in] ec
 * @param[in] pvs
 * @return
 */
static qboolean CL_TV_ClustersVisible(const tvEntityClusters_t *ec, const byte *pvs)
{
	int i, l = 0;

	for (i = 0; i < ec->numClusters; i++)
	{
		l = ec->clusternums[i];
		if (pvs[l >> 3] & (1 << (l & 7)))
		{
			return qtrue;
		}
	}

	// check overflow clusters that couldn't be stored
	if (ec->lastCluster)
	{
		for ( ; l <= ec->lastCluster; l++)
		{
			if (pvs[l >> 3] & (1 << (l & 7)))
			{
				return qtrue;
			}
		}
	}

	return qfalse;
}

/**
 * @brief Pick entities like the tv server does for its clients, see SV_AddEntitiesVisibleFromPoint
 *
 * @details Area portal states of the master server aren't known, so only PVS is checked.
 *
 * @param[in] origin
 * @param[in] viewClient client number of the transcoded playerstate
 * @param[in,out] added
 * @param[in] entIndex snapshot index of entity numbers, -1 if not in the snapshot
 * @param[in] depth portal recursion depth
 */
static void CL_TV_AddEntitiesVisibleFromPoint(const vec3_t origin, int viewClient, qboolean *added, const int *entIndex, int depth)
{
	tvDemoState_t            *tv  = tvd.state;
	byte                     *pvs = NULL;
	entityState_t            *es, *ment;
	entityShared_t           *sh, *msh;
	const tvEntityClusters_t *ec;
	int                      i, j, cluster;

	// everything is visible from outside of the world or while the map isn't loaded yet
	if (CM_NumClusters() > 0)
	{
		cluster = CM_LeafCluster(CM_PointLeafnum(origin));
		if (cluster >= 0)
		{
			pvs = CM_ClusterPVS(cluster);
		}
	}

	for (i = 0; i < tv->snap.numEntities; i++)
	{
		es = &tv->parseEntities[(tv->snap.parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];
		sh = &tv->parseEntitiesShared[(tv->snap.parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];

		// never send client's own entity, because it can be regenerated from the playerstate
		if (added[es->number] || es->number == viewClient)
		{
			continue;
		}

		// entities can be flagged to explicitly not be sent to the client
		if (sh->svFlags & SVF_NOCLIENT)
		{
			continue;
		}

		// entities can be flagged to be sent to only one client
		if ((sh->svFlags & SVF_SINGLECLIENT) && sh->singleClient != viewClient)
		{
			continue;
		}

		// entities can be flagged to be sent to everyone but one client
		if ((sh->svFlags & SVF_NOTSINGLECLIENT) && sh->singleClient == viewClient)
		{
			continue;
		}

		// broadcast entities and players are always sent
		if ((sh->svFlags & SVF_BROADCAST) || es->number < MAX_CLIENTS || !pvs)
		{
			added[es->number] = qtrue;
			continue;
		}

		ec = CL_TV_EntityClusters(es, sh);

		// just check origin for being in pvs, ignore bmodel extents
		if (sh->svFlags & SVF_IGNOREBMODELEXTENTS)
		{
			if (ec->originCluster >= 0 && (pvs[ec->originCluster >> 3] & (1 << (ec->originCluster & 7))))
			{
				added[es->number] = qtrue;
			}
			continue;
		}

		if (!CL_TV_ClustersVisible(ec, pvs))
		{
			continue;
		}

		// visibility dummies add their master instead of themselves
		if (sh->svFlags & SVF_VISDUMMY)
		{
			if (es->otherEntityNum >= 0 && es->otherEntityNum < MAX_GENTITIES && entIndex[es->otherEntityNum] != -1)
			{
				added[es->otherEntityNum] = qtrue;
			}
			continue;
		}

		if (sh->svFlags & SVF_VISDUMMY_MULTIPLE)
		{
			for (j = 0; j < tv->snap.numEntities; j++)
			{
				ment = &tv->parseEntities[(tv->snap.parseEntitiesNum + j) & (MAX_PARSE_ENTITIES - 1)];
				msh  = &tv->parseEntitiesShared[(tv->snap.parseEntitiesNum + j) & (MAX_PARSE_ENTITIES - 1)];

				if (ment->number != es->number && !(msh->svFlags & SVF_NOCLIENT) && ment->otherEntityNum == es->number)
				{
					added[ment->number] = qtrue;
				}
			}
			continue;
		}

		added[es->number] = qtrue;

		// if its a portal entity, add everything visible from its camera position
		if ((sh->svFlags & SVF_PORTAL) && depth < TV_MAX_PORTAL_DEPTH)
		{
			CL_TV_AddEntitiesVisibleFromPoint(es->origin2, viewClient, added, entIndex, depth + 1);
		}
	}
}

/**
 * @brief Playerstate and entities of the latest snapshot for the current point of view
 * @param[out] ps
 * @param[out] entities
 * @param[out] numEntities
 * @param[in] withEntities
 */
static void CL_TV_BuildSnapshot(playerState_t *ps, entityState_t *entities, int *numEntities, qboolean withEntities)
{
	tvDemoState_t  *tv = tvd.state;
	entityState_t  *es;
	vec3_t         org;
	int            viewClient, i;
	qboolean       added[MAX_GENTITIES];
	int            entIndex[MAX_GENTITIES];

	*numEntities = 0;

	viewClient = CL_TV_ViewClient();

	if (viewClient == -1)
	{
		*ps = tv->snap.ps;
	}
	else
	{
		playerState_t *followed = &tv->playerstates[tv->snap.messageNum & PACKET_MASK][viewClient].ps;

		// like the tv server, keep some values of its own client
		*ps                        = *followed;
		ps->pm_flags              |= PMF_FOLLOW;
		ps->persistant[PERS_SCORE] = tv->snap.ps.persistant[PERS_SCORE];
		ps->eFlags                 = (followed->eFlags & ~(EF_VOTED | EF_READY)) | (tv->snap.ps.eFlags & (EF_VOTED | EF_READY));
		ps->ping                   = tv->snap.ps.ping;
	}

	if (!withEntities)
	{
		return;
	}

	if (viewClient == -1 && !VectorCompare(cl.cgameClientLerpOrigin, vec3_origin))
	{
		// cgame view origin, so the freecam sees entities around it
		VectorCopy(cl.cgameClientLerpOrigin, org);
	}
	else
	{
		VectorCopy(ps->origin, org);
		org[2] += ps->viewheight;

		// account for lean like the server does
		if (ps->leanf != 0.f)
		{
			vec3_t right, v3ViewAngles;

			VectorCopy(ps->viewangles, v3ViewAngles);
			v3ViewAngles[2] += ps->leanf / 2.0f;
			angles_vectors(v3ViewAngles, NULL, right, NULL);
			VectorMA(org, ps->leanf, right, org);
		}
	}

	Com_Memset(added, 0, sizeof(added));

	for (i = 0; i < MAX_GENTITIES; i++)
	{
		entIndex[i] = -1;
	}

	for (i = 0; i < tv->snap.numEntities; i++)
	{
		entIndex[tv->parseEntities[(tv->snap.parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)].number] = i;
	}

	CL_TV_AddEntitiesVisibleFromPoint(org, ps->clientNum, added, entIndex, 0);

	// parsed entities are sorted by number, as the delta compression needs them
	for (i = 0; i < tv->snap.numEntities && *numEntities < MAX_ENTITIES_IN_SNAPSHOT; i++)
	{
		es = &tv->parseEntities[(tv->snap.parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];

		if (added[es->number] && es->number != ps->clientNum)
		{
			entities[(*numEntities)++] = *es;
		}
	}
}

/**
 * @brief Baseline the client got in the transcoded gamestate, like CL_Record it skips unset baselines
 * @param[in] number
 * @return
 */
static entityState_t *CL_TV_OutBaseline(int number)
{
	static entityState_t nullstate;

	if (!tvd.state->entityBaselines[number].number)
	{
		return &nullstate;
	}

	return &tvd.state->entityBaselines[number];
}

/**
 * @brief Delta compress entities of the transcoded snapshot, see SV_EmitPacketEntities
 * @param[out] msg
 * @param[in] from
 * @param[in] fromNum
 * @param[in] to
 * @param[in] toNum
 */
static void CL_TV_EmitPacketEntities(msg_t *msg, entityState_t *from, int fromNum, entityState_t *to, int toNum)
{
	entityState_t *oldent  = NULL, *newent = NULL;
	int           oldindex = 0, newindex = 0;
	int           oldnum, newnum;

	while (newindex < toNum || oldindex < fromNum)
	{
		if (newindex >= toNum)
		{
			newnum = 9999;
		}
		else
		{
			newent = &to[newindex];
			newnum = newent->number;
		}

		if (oldindex >= fromNum)
		{
			oldnum = 9999;
		}
		else
		{
			oldent = &from[oldindex];
			oldnum = oldent->number;
		}

		if (newnum == oldnum)
		{
			// delta update from old position
			MSG_WriteDeltaEntity(msg, oldent, newent, qfalse);
			oldindex++;
			newindex++;
			continue;
		}

		if (newnum < oldnum)
		{
			// this is a new entity, send it from the baseline
			MSG_WriteDeltaEntity(msg, CL_TV_OutBaseline(newnum), newent, qtrue);
			newindex++;
			continue;
		}

		// the old entity isn't present in the new message
		MSG_WriteDeltaEntity(msg, oldent, NULL, qtrue);
		oldindex++;
	}

	MSG_WriteBits(msg, (MAX_GENTITIES - 1), GENTITYNUM_BITS);   // end of packetentities
}

/**
 * @brief Write the latest snapshot as seen from the current point of view
 * @param[out] out
 * @param[in] withEntities qfalse while only the snapshot times are needed
 */
static void CL_TV_WriteSnapshot(msg_t *out, qboolean withEntities)
{
	static entityState_t entities[MAX_ENTITIES_IN_SNAPSHOT];
	tvDemoState_t        *tv = tvd.state;
	playerState_t        ps;
	int                  numEntities, deltaNum = 0;

	CL_TV_BuildSnapshot(&ps, entities, &numEntities, withEntities);

	// delta from the previous transcoded snapshot if it's the latest one the client has
	if (tv->outValid && cl.snap.valid && cl.snap.messageNum == tv->outMessageNum &&
	    tv->snap.messageNum - tv->outMessageNum > 0 && tv->snap.messageNum - tv->outMessageNum < PACKET_BACKUP)
	{
		deltaNum = tv->snap.messageNum - tv->outMessageNum;
	}

	MSG_WriteByte(out, svc_snapshot);
	MSG_WriteLong(out, tv->snap.serverTime);
	MSG_WriteByte(out, deltaNum);
	MSG_WriteByte(out, tv->snap.snapFlags);

	// area portal states of the master server aren't known, empty mask shows all areas
	MSG_WriteByte(out, 0);

	if (deltaNum)
	{
		MSG_WriteDeltaPlayerstate(out, &tv->outPs, &ps);
		CL_TV_EmitPacketEntities(out, tv->outEntities, tv->outNumEntities, entities, numEntities);
	}
	else
	{
		MSG_WriteDeltaPlayerstate(out, NULL, &ps);
		CL_TV_EmitPacketEntities(out, NULL, 0, entities, numEntities);
	}

	tv->outValid       = qtrue;
	tv->outMessageNum  = tv->snap.messageNum;
	tv->outPs          = ps;
	tv->outNumEntities = numEntities;
	Com_Memcpy(tv->outEntities, entities, numEntities * sizeof(entities[0]));
}

/**
 * @brief Transcode a tv demo message into a regular server message
 * @param[in] msg tv demo message, clc.serverMessageSequence must be set already
 * @param[out] out regular server message ready for reading
 * @param[in] withEntities qfalse while only the snapshot times are needed
 */
void CL_TV_TranscodeMessage(msg_t *msg, msg_t *out, qboolean withEntities)
{
	int      cmd, size;
	qboolean snapshot = qfalse, snapValid = qfalse;

	if (!tvd.state)
	{
		Com_Error(ERR_DROP, "CL_TV_TranscodeMessage: tv demo playback isn't initialized");
	}

	MSG_Init(out, tvd.outData, sizeof(tvd.outData));
	MSG_Bitstream(out);

	MSG_Bitstream(msg);

	// reliable sequence acknowledge number
	MSG_WriteLong(out, MSG_ReadLong(msg));

	while (1)
	{
		if (msg->readcount > msg->cursize)
		{
			Com_Error(ERR_DROP, "CL_TV_TranscodeMessage: read past end of server message");
		}

		cmd = MSG_ReadByte(msg);

		if (cmd == svc_EOF)
		{
			break;
		}

		switch (cmd)
		{
		default:
			Com_Error(ERR_DROP, "CL_TV_TranscodeMessage: Illegible tv demo message %d", cmd);
		case svc_nop:
			break;
		case svc_serverCommand:
			CL_TV_ParseCommandString(msg, out);
			break;
		case svc_gamestate:
			CL_TV_ParseGamestate(msg, out);
			break;
		case svc_snapshot:
			snapshot  = qtrue;
			snapValid = CL_TV_ParseSnapshot(msg);
			break;
		case svc_ettv_playerstates:
			CL_TV_ParsePlayerstates(msg, snapValid);
			break;
		}
	}

	// written after the playerstates of all clients were read
	if (snapshot && snapValid)
	{
		CL_TV_WriteSnapshot(out, withEntities);
	}

	MSG_WriteByte(out, svc_EOF);

	// binary message for the cgame
	MSG_BeginReadingUncompressed(msg);
	size = msg->cursize - msg->readcount;

	if (size > 0 && size <= MAX_BINARY_MESSAGE)
	{
		MSG_Uncompressed(out);
		MSG_WriteData(out, &msg->data[msg->readcount], size);
	}

	if (out->overflowed)
	{
		Com_Error(ERR_DROP, "CL_TV_TranscodeMessage: transcoded message overflowed");
	}

	MSG_BeginReading(out);
}

/*
=========================================================================
CLIENT COMMANDS
=========================================================================
*/

/**
 * @brief Add a server command for the client, cgame executes it with the next snapshot
 * @param[in] cmd
 */
static void CL_TV_AddServerCommand(const char *cmd)
{
	tvDemoState_t *tv = tvd.state;

	if (!cmd[0])
	{
		return;
	}

	clc.serverCommandSequence = ++tv->commandSequence;
	Q_strncpyz(clc.serverCommands[clc.serverCommandSequence & (MAX_RELIABLE_COMMANDS - 1)], cmd, sizeof(clc.serverCommands[0]));
}

/**
 * @brief Give cached stats of a player to the client, see TVG_statsPrint
 * @param[in] type
 * @param[in] arg client number or empty for the followed client
 */
static void CL_TV_SendStats(tvStatsType_t type, const char *arg)
{
	tvDemoState_t *tv = tvd.state;
	int           clientNum;

	if (!arg[0])
	{
		clientNum = CL_TV_ViewClient();
	}
	else if (!Q_ParseInt(arg, &clientNum))
	{
		clientNum = -1;
	}

	if (clientNum < 0 || clientNum >= MAX_CLIENTS || !tv->statsValid[type][clientNum])
	{
		return;
	}

	CL_TV_AddServerCommand(tv->stats[type][clientNum]);
}

/**
 * @brief A paused demo doesn't read new snapshots, read the current ones again for the new point of view
 *
 * @details Seeking a frame back makes cgame read its snapshots again, a frame later
 *          the demo is seeked back to the paused time.
 */
static void CL_TV_RefreshPausedView(void)
{
	int frameMsec;

	if (!clc.demo.playing || !cl_freezeDemo->integer)
	{
		return;
	}

	frameMsec = 1000 / (cl.sv_fps > 0 ? cl.sv_fps : DEFAULT_SV_FPS);

	Cbuf_AddText(va("seekservertime %i; wait; seekservertime %i\n", cl.serverTime - frameMsec, cl.serverTime));
}

/**
 * @brief Handle client commands a tv server would answer
 * @param[in] text command with arguments
 * @return qtrue if the command was handled
 */
qboolean CL_TV_ClientCommand(const char *text)
{
	tvDemoState_t *tv = tvd.state;
	char          buf[MAX_STRING_CHARS];
	char          cmd[MAX_TOKEN_CHARS];
	char          arg[MAX_TOKEN_CHARS];
	char          *p = buf;
	int           clientNum;
	int           oldFollowClient = tvd.followClient;

	if (!tvd.active || !tv)
	{
		return qfalse;
	}

	Q_strncpyz(buf, text, sizeof(buf));
	Q_strncpyz(cmd, COM_Parse(&p), sizeof(cmd));
	Q_strncpyz(arg, COM_Parse(&p), sizeof(arg));

	if (!Q_stricmp(cmd, "follow"))
	{
		// no argument goes back to the tv server point of view
		if (!arg[0])
		{
			tvd.followClient = -1;
		}
		else if (!Q_ParseInt(arg, &clientNum) || clientNum < 0 || clientNum >= MAX_CLIENTS)
		{
			Com_Printf("usage: follow [clientNum]\n");
		}
		else if (!CL_TV_CanFollow(clientNum))
		{
			Com_Printf("Client %i can't be followed.\n", clientNum);
		}
		else
		{
			tvd.followClient = clientNum;
		}
	}
	else if (!Q_stricmp(cmd, "follownext"))
	{
		CL_TV_FollowCycle(1);
	}
	else if (!Q_stricmp(cmd, "followprev"))
	{
		CL_TV_FollowCycle(-1);
	}

	if (!Q_stricmp(cmd, "follow") || !Q_stricmp(cmd, "follownext") || !Q_stricmp(cmd, "followprev"))
	{
		if (tvd.followClient != oldFollowClient)
		{
			CL_TV_RefreshPausedView();
		}
		return qtrue;
	}

	if (!Q_stricmp(cmd, "weaponstats"))
	{
		CL_TV_SendStats(TV_STATS_WS, arg);
		return qtrue;
	}

	if (!Q_stricmp(cmd, "wstats"))
	{
		CL_TV_SendStats(TV_STATS_WWS, arg);
		return qtrue;
	}

	if (!Q_stricmp(cmd, "sgstats"))
	{
		CL_TV_SendStats(TV_STATS_GSTATS, arg);
		return qtrue;
	}

	// weapon rankings, see TVG_weaponRankings_cmd
	if (!Q_stricmp(cmd, "topshots"))
	{
		CL_TV_AddServerCommand(tv->rankings[arg[0] ? TV_RANK_ASTATS : TV_RANK_BSTATS]);
		return qtrue;
	}

	if (!Q_stricmp(cmd, "bottomshots"))
	{
		CL_TV_AddServerCommand(tv->rankings[arg[0] ? TV_RANK_ASTATSB : TV_RANK_BSTATSB]);
		return qtrue;
	}

	if (!Q_stricmp(cmd, "stshots"))
	{
		CL_TV_AddServerCommand(tv->rankings[TV_RANK_WBSTATS]);
		return qtrue;
	}

	return qfalse;
}
