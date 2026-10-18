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
 * @file g_missilepreview.c
 * @brief Server-side missile trajectory preview, shown to unmodified clients
 *
 * Holding +activate with a rifle grenade, a throwable, a panzerfaust/bazooka or a deployed mortar
 * simulates the round the server would fire right now and shows it with what a stock cgame
 * already draws:
 * - the arc and a detonation marker as ET_BEAM entities (CG_Beam)
 * - the missile camera, through an invisible ET_MISSILE owned by the player (CG_Missile only picks
 *   it up while sv_cheats is on, so the camera needs cheats even when g_missilePreview is 2). The
 *   camera view skips packet entities, so what it should show goes out as EV_RAILTRAIL rails meanwhile
 * - a centerprint readout of the detonation height and the damage to a /missiletarget box
 *
 * The simulation replays weapon_*_fire(), fire_missile(), G_RunMissile(), G_BounceMissile() and
 * G_MissileImpact() one server frame at a time against the live world. Keep it in sync with them.
 */

#include "g_local.h"

#define MP_ARC_SEGMENTS         32      ///< beams drawing the arc
#define MP_MARKER_LINES         4       ///< beams drawing the detonation marker
#define MP_BEAMS                (MP_ARC_SEGMENTS + MP_MARKER_LINES)
#define MP_TARGET_EDGES         12
#define MP_MAX_POINTS           2048    ///< one sample per simulated server frame
#define MP_RECOMPUTE_MSEC       50
#define MP_READOUT_MSEC         100     ///< minimum gap between two readout centerprints
#define MP_READOUT_REFRESH_MSEC 2000    ///< resend an unchanged readout before its centerprint fades
#define MP_ENTITY_RESERVE       64      ///< free entity slots the preview always leaves to the game
#define MP_CAMERA_DISTANCE      240.f   ///< how far back along the arc the camera looks from
#define MP_CAM_LOOKBACK         8       ///< camera waypoints looked back over, as in the fork's CG_DrawMissileCamera()
#define MP_CAM_WAYPOINTS        48      ///< the fork samples its trajectory into this many waypoints
#define MP_TARGET_HEALTH        100000
#define MP_READOUT_SIZE         128
#define MP_CAM_RAILS            8       ///< rails drawing the part of the arc the missile camera sees
#define MP_RAIL_MSEC            100     ///< minimum gap between two rail updates of a changed arc
#define MP_RAIL_REFRESH_MSEC    500     ///< resend unchanged rails before the client's cg_railTrailTime (750) drops them
#define MP_RAIL_INDEX_BASE      0x40000000
#define MP_RAIL_BOX_SLOT        (MP_CAM_RAILS + MP_MARKER_LINES)

static const char mpBeamClassname[]   = "missilepreview_beam";
static const char mpCameraClassname[] = "missilepreview_camera";
static const char mpTargetClassname[] = "missiletarget";

static const vec3_t mpArcColor           = { 1.0f, 0.45f, 0.1f };
static const vec3_t mpMarkerColor        = { 1.0f, 0.0f, 0.0f };
static const vec3_t mpTargetColor        = { 0.2f, 0.8f, 1.0f };
static const vec3_t mpTargetUnarmedColor = { 0.3f, 0.35f, 0.4f };

/**
 * @struct mpClient_s
 * @brief Per client preview state
 */
typedef struct mpClient_s
{
	gentity_t *beams[MP_BEAMS];
	gentity_t *camera;
	gentity_t *target;
	gentity_t *targetEdges[MP_TARGET_EDGES];
	qboolean targetArmed;                       ///< the box only becomes solid once its owner stepped out of it

	// the shown arc, kept so it can be redrawn without simulating again
	vec3_t arcVerts[MP_ARC_SEGMENTS + 1];
	int numArcVerts;
	int camVert;                                ///< first arc vertex at or before the missile camera
	qboolean markerShown;
	vec3_t markerEnd;
	vec3_t markerBase[MP_MARKER_LINES];

	// stock clients don't draw packet entities in the missile camera, only local entities such as
	// the rails EV_RAILTRAIL makes, so while the camera is up the part of the arc it sees, the marker
	// and the target box go out as rails instead of beams
	qboolean camRails;
	qboolean railsDirty;
	qboolean boxRailDirty;
	int railGeneration;                         ///< new rail indexes per camera session, see G_MP_RailIndex()
	int arcRailsSent;                           ///< arc rail slots in use, collapsed when no longer needed
	qboolean markerRailsSent;
	qboolean boxRailSent;
	int lastRailTime;
	int lastBoxRailTime;

	int lastComputeTime;
	int lastReadoutTime;
	char readoutSent[MP_READOUT_SIZE];
	char readoutPending[MP_READOUT_SIZE];
	qboolean warnedNoEntities;
} mpClient_t;

static mpClient_t mpClients[MAX_CLIENTS];

/**
 * @enum mpStatus_e
 * @brief Outcome of one simulated server frame
 */
typedef enum mpStatus_e
{
	MP_FLYING,
	MP_EXPLODED,
	MP_VANISHED,    ///< freed without an explosion: SURF_NOIMPACT, out of the map, lost above the sky
	MP_RESTING      ///< came to rest with no fuse of its own (dynamite, landmine, satchel)
} mpStatus_t;

/**
 * @struct mpMissile_s
 * @brief The gentity_t fields G_RunMissile() and friends read and write
 */
typedef struct mpMissile_s
{
	weapon_t weapon;
	trajectory_t pos;
	vec3_t currentOrigin;
	vec3_t mins, maxs;
	int ownerNum;
	int clipmask;
	int contents;
	int eFlags;
	int damage;
	int groundEntityNum;
	int nextthink;
	int lastSurfaceFlags;
	qboolean inSky;                             ///< ent->count
} mpMissile_t;

/**
 * @struct mpResult_s
 * @brief What a simulated shot does
 */
typedef struct mpResult_s
{
	weapon_t weapon;                            ///< weapon of the simulated round (the rifle grenade for a K43/Garand)
	vec3_t end;                                 ///< detonation point, or where the round vanished
	qboolean explodes;
	mpStatus_t status;                          ///< how the simulation ended, MP_FLYING when it ran out of time
	int flightTime;                             ///< msec from the round leaving the muzzle until that happened
	int restTime;                               ///< msec until it came to rest for good, 0 if it never did
	int hitEntityNum;                           ///< entity struck by a detonate-on-contact round, ENTITYNUM_NONE otherwise
} mpResult_t;

static vec3_t   mpPoints[MP_MAX_POINTS];
static qboolean mpKeyPoints[MP_MAX_POINTS];     ///< launch, bounces and the end, kept when thinning the arc
static int      mpNumPoints;
static vec3_t   mpLastPoint;                    ///< the last point recorded, even when merged into the one before

/**
 * @struct mpCamWaypoint_s
 * @brief A waypoint of the fork's camera placement, and which arc point it is
 */
typedef struct mpCamWaypoint_s
{
	vec3_t pos;
	int point;                                  ///< index into mpPoints
} mpCamWaypoint_t;

// the camera is placed the way the fork places it, from waypoints sampled like the fork samples them;
// only the last MP_CAM_LOOKBACK + 1 of them are ever looked at
static mpCamWaypoint_t mpCamWaypoints[MP_CAM_LOOKBACK + 1];
static int             mpNumCamWaypoints;      ///< total recorded, the ring keeps the latest
static mpCamWaypoint_t mpCamSegStart;           ///< start of the final frame, the fork's predStart
static qboolean        mpEndIsWaypoint;         ///< the fork's trajectory ends on a waypoint: think detonation or SURF_NOIMPACT

extern vec3_t playerMins, playerMaxs;

/**
 * @brief G_MissilePreviewInit
 */
void G_MissilePreviewInit(void)
{
	Com_Memset(mpClients, 0, sizeof(mpClients));
}

/**
 * @brief G_MP_Enabled
 * @return qtrue if the preview may be used right now
 */
static qboolean G_MP_Enabled(void)
{
	return (g_missilePreview.integer >= 2 || (g_missilePreview.integer == 1 && g_cheats.integer)) ? qtrue : qfalse;
}

/**
 * @brief G_MP_IsPreviewable
 * @param[in] weapon
 * @return qtrue for weapons whose round the simulation knows
 */
static qboolean G_MP_IsPreviewable(weapon_t weapon)
{
	int type;

	if (!IS_VALID_WEAPON(weapon))
	{
		return qfalse;
	}

	switch (weapon)
	{
	case WP_KAR98:
	case WP_CARBINE:
	case WP_GPG40:
	case WP_M7:
	case WP_GRENADE_LAUNCHER:
	case WP_GRENADE_PINEAPPLE:
	case WP_DYNAMITE:
	case WP_LANDMINE:
	case WP_SATCHEL:
	case WP_SMOKE_MARKER:
	case WP_SMOKE_BOMB:
		return qtrue;
	default:
		break;
	}

	// the undeployed mortar can't fire at all
	type = GetWeaponTableData(weapon)->type;

	return ((type & WEAPON_TYPE_PANZER) || CHECKBITWISE(type, WEAPON_TYPE_MORTAR | WEAPON_TYPE_SET)) ? qtrue : qfalse;
}

/**
 * @brief The rifle still holds an unfired grenade, what eventually leaves it is the launcher's round
 * @param[in] weapon
 * @return
 */
static weapon_t G_MP_RoundWeapon(weapon_t weapon)
{
	if (weapon == WP_KAR98 || weapon == WP_CARBINE)
	{
		return GetWeaponTableData(weapon)->weapAlts;
	}

	return weapon;
}

/**
 * @brief G_MP_DamageColor
 * @param[in] damage
 * @return colour code escalating with how much of a full health player the damage takes
 */
static const char *G_MP_DamageColor(int damage)
{
	if (damage <= 0)
	{
		return S_COLOR_WHITE;
	}

	if (damage < 50)
	{
		return S_COLOR_YELLOW;
	}

	return (damage < 100) ? S_COLOR_ORANGE : S_COLOR_RED;
}

/**
 * @brief G_MP_Owns
 * @param[in] e
 * @param[in] classname
 * @param[in] clientNum
 * @return qtrue if @p e is still the preview entity of that kind spawned for @p clientNum
 */
static qboolean G_MP_Owns(gentity_t *e, const char *classname, int clientNum)
{
	return (e && e->inuse && e->classname == classname && e->r.singleClient == clientNum) ? qtrue : qfalse;
}

/**
 * @brief G_MP_Free
 * @param[in,out] e
 * @param[in] classname
 * @param[in] clientNum
 */
static void G_MP_Free(gentity_t **e, const char *classname, int clientNum)
{
	if (G_MP_Owns(*e, classname, clientNum))
	{
		G_FreeEntity(*e);
	}

	*e = NULL;
}

/**
 * @brief G_MissilePreviewIsCamera
 * @param[in] ent
 * @return qtrue for the fake missile the preview points the client's missile camera with
 */
qboolean G_MissilePreviewIsCamera(gentity_t *ent)
{
	return (ent->classname == mpCameraClassname) ? qtrue : qfalse;
}

/**
 * @brief G_MissilePreviewIsTarget
 * @param[in] ent
 * @return qtrue for a /missiletarget box
 */
qboolean G_MissilePreviewIsTarget(gentity_t *ent)
{
	return (ent->classname == mpTargetClassname) ? qtrue : qfalse;
}

/*
======================================================================
SIMULATION
======================================================================
*/

/**
 * @brief Records a point of the arc, merging repeats
 * @param[in] point
 * @param[in] key
 */
static void G_MP_AddPoint(const vec3_t point, qboolean key)
{
	VectorCopy(point, mpLastPoint);

	if (mpNumPoints > 0 && VectorCompare(point, mpPoints[mpNumPoints - 1]))
	{
		if (key)
		{
			mpKeyPoints[mpNumPoints - 1] = qtrue;
		}
		return;
	}

	if (mpNumPoints == MP_MAX_POINTS)
	{
		// out of room, keep the latest point rather than lose the end of the arc
		VectorCopy(point, mpPoints[MP_MAX_POINTS - 1]);
		if (key)
		{
			mpKeyPoints[MP_MAX_POINTS - 1] = qtrue;
		}
		return;
	}

	VectorCopy(point, mpPoints[mpNumPoints]);
	mpKeyPoints[mpNumPoints] = key;
	mpNumPoints++;
}

/**
 * @brief The last recorded point, as a camera waypoint
 * @param[out] wp
 */
static void G_MP_LastPointWaypoint(mpCamWaypoint_t *wp)
{
	VectorCopy(mpLastPoint, wp->pos);
	wp->point = mpNumPoints - 1;
}

/**
 * @brief Records the last recorded point as a camera waypoint
 */
static void G_MP_AddCamWaypoint(void)
{
	G_MP_LastPointWaypoint(&mpCamWaypoints[mpNumCamWaypoints % (MP_CAM_LOOKBACK + 1)]);
	mpNumCamWaypoints++;
}

/**
 * @brief Computes where and how fast the round would leave, mirroring the weapon_*_fire() functions
 * @param[in] ent
 * @param[in] weapon
 * @param[out] start
 * @param[out] velocity
 */
static void G_MP_Launch(gentity_t *ent, weapon_t weapon, vec3_t start, vec3_t velocity)
{
	gclient_t       *client = ent->client;
	weaponTable_t   *wt     = GetWeaponTableData(weapon);
	weapFireTable_t *ft     = GetWeaponFireTableData(weapon);
	vec3_t          forward, right, up, muzzle;
	vec3_t          trBase, angles;
	trace_t         tr;

	// the entity state the fire functions read, as ClientThink_real() leaves it before ClientEvents():
	// r.currentOrigin is the precise origin, s.pos.trBase and s.apos.trBase are snapped
	VectorCopy(client->ps.origin, trBase);
	SnapVector(trBase);
	VectorCopy(client->ps.viewangles, angles);
	SnapVector(angles);

	// CalcMuzzlePoints()
	AngleVectors(client->ps.viewangles, forward, right, up);
	VectorCopy(client->ps.origin, muzzle);
	muzzle[2] += client->ps.viewheight;
	VectorMA(muzzle, wt->muzzlePointOffset[1], right, muzzle);
	VectorMA(muzzle, wt->muzzlePointOffset[2], up, muzzle);
	SnapVector(muzzle);

	if (wt->type & WEAPON_TYPE_RIFLENADE)
	{
		// weapon_gpg40_fire()
		vec3_t viewpos, orig_viewpos;

		VectorCopy(muzzle, start);

		VectorCopy(trBase, viewpos);
		viewpos[2] += client->ps.viewheight;
		VectorCopy(viewpos, orig_viewpos);
		VectorMA(viewpos, 32, forward, viewpos);

		trap_Trace(&tr, orig_viewpos, ft->boundingBox[0], ft->boundingBox[1], viewpos, ent->s.number, MASK_MISSILESHOT);
		if (tr.fraction < 1)
		{
			VectorCopy(tr.endpos, start);
			SnapVectorTowards(start, orig_viewpos);
		}
		else
		{
			trap_Trace(&tr, viewpos, ft->boundingBox[0], ft->boundingBox[1], start, ent->s.number, MASK_MISSILESHOT);
			if (tr.fraction < 1)
			{
				VectorCopy(tr.endpos, start);
				SnapVectorTowards(start, viewpos);
			}
		}

		VectorScale(forward, 2000, velocity);
	}
	else if (wt->type & WEAPON_TYPE_MORTAR)
	{
		// weapon_mortar_fire()
		vec3_t mortarAngles, testPos;

		VectorCopy(client->ps.viewangles, mortarAngles);
		mortarAngles[PITCH] -= 60.f;
		AngleVectors(mortarAngles, forward, NULL, NULL);

		VectorCopy(muzzle, start);
		VectorMA(start, 32, forward, testPos);

		velocity[0] = forward[0] * 3000 * 1.1f;
		velocity[1] = forward[1] * 3000 * 1.1f;
		velocity[2] = forward[2] * 1500 * 1.1f;

		trap_Trace(&tr, testPos, ft->boundingBox[0], ft->boundingBox[1], start, ent->s.number, MASK_MISSILESHOT);
		if (tr.fraction < 1)
		{
			VectorCopy(tr.endpos, start);
			SnapVectorTowards(start, testPos);
		}
	}
	else if (wt->type & WEAPON_TYPE_PANZER)
	{
		// weapon_antitank_fire()
		VectorCopy(forward, velocity);
		VectorNormalize(velocity);
		VectorScale(velocity, 2500, velocity);
		VectorCopy(muzzle, start);
	}
	else
	{
		// weapon_grenadelauncher_fire()
		vec3_t viewpos;
		float  upangle, pitch = angles[PITCH];

		if (pitch >= 0)
		{
			forward[2] += 0.5f;
			pitch       = 1.3f;
		}
		else
		{
			pitch       = -pitch;
			pitch       = MIN(pitch, 30);
			pitch      /= 30.f;
			pitch       = 1 - pitch;
			forward[2] += (pitch * 0.5f);
			pitch      *= 0.3f;
			pitch      += 1.f;
		}

		VectorNormalizeFast(forward);

		upangle  = -angles[PITCH];
		upangle  = MIN(upangle, 50);
		upangle  = MAX(upangle, -50);
		upangle  = upangle / 100.0f;
		upangle += 0.5f;

		if (upangle < .1f)
		{
			upangle = .1f;
		}

		if ((wt->type & WEAPON_TYPE_GRENADE) || weapon == WP_SMOKE_MARKER || weapon == WP_SMOKE_BOMB)
		{
			upangle *= 900;
		}
		else
		{
			upangle *= 400;
		}

		VectorMA(muzzle, 8, forward, start);
		start[2] -= 8;
		upangle  *= pitch;
		SnapVector(start);

		VectorScale(forward, upangle, velocity);

		VectorCopy(trBase, viewpos);
		viewpos[2] += client->ps.viewheight;

		trap_Trace(&tr, viewpos, ft->boundingBox[0], ft->boundingBox[1], start, ent->s.number, MASK_MISSILESHOT);

		if (tr.startsolid)
		{
			VectorCopy(velocity, viewpos);
			VectorNormalizeFast(viewpos);
			VectorMA(client->ps.origin, -24.f, viewpos, viewpos);

			trap_Trace(&tr, viewpos, ft->boundingBox[0], ft->boundingBox[1], start, ent->s.number, MASK_MISSILESHOT);

			VectorCopy(tr.endpos, start);
		}
		else if (tr.fraction < 1)
		{
			VectorCopy(tr.endpos, start);
			SnapVectorTowards(start, viewpos);
		}
	}
}

/**
 * @brief Sets up the round as fire_missile() and G_PreFilledMissileEntity() would
 * @param[in] ent
 * @param[in] weapon
 * @param[in] start
 * @param[in] velocity
 * @param[out] m
 */
static void G_MP_InitMissile(gentity_t *ent, weapon_t weapon, const vec3_t start, const vec3_t velocity, mpMissile_t *m)
{
	weaponTable_t   *wt     = GetWeaponTableData(weapon);
	weapFireTable_t *ft     = GetWeaponFireTableData(weapon);
	gclient_t       *client = ent->client;

	Com_Memset(m, 0, sizeof(*m));

	m->weapon   = weapon;
	m->ownerNum = ent->s.number;
	m->damage   = wt->damage;
	m->clipmask = ft->clipMask;
	m->contents = ft->contents;
	m->eFlags   = ft->eFlags;

	if (m->contents == CONTENTS_CORPSE)
	{
		VectorCopy(ft->boundingBox[0], m->mins);
		VectorCopy(ft->boundingBox[1], m->maxs);
	}

	m->pos.trType = ft->trType;
	m->pos.trTime = level.time + ft->trTime;
	VectorCopy(start, m->pos.trBase);
	VectorCopy(velocity, m->pos.trDelta);
	SnapVector(m->pos.trDelta);
	VectorCopy(start, m->currentOrigin);

	// G_Spawn() hands out a zeroed slot, so this is what a fresh missile carries
	m->groundEntityNum = 0;

	if (wt->grenadeTime)
	{
		int fuse;

		// PM_Weapon() starts the fuse together with the throw delay, and while cooking the delay
		// only begins to run down once attack is released, with the fuse still ticking
		if (client->ps.grenadeTimeLeft > 0)
		{
			fuse = client->ps.grenadeTimeLeft - client->ps.weaponDelay;
		}
		else
		{
			fuse = wt->grenadeTime - wt->fireDelayTime;
		}

		// PM_CheckGrenade() never lets the fuse drop below 100
		m->nextthink = level.time + MAX(fuse, 100);
	}
	else if (ft->nextThink && weapon != WP_DYNAMITE && weapon != WP_LANDMINE && weapon != WP_SMOKE_MARKER)
	{
		// dynamite and landmine think only to sink away unarmed, the smoke marker's think only checks
		// the airstrike, which then comes down where the canister lies: they all end where they rest
		m->nextthink = level.time + ft->nextThink;
	}
}

/**
 * @brief G_RunThink() check, only made where a due think ends the round
 * @param[in] m
 * @param[in] levelTime
 * @return qtrue if the round's think (its detonation) runs this frame
 */
static qboolean G_MP_ThinkDue(const mpMissile_t *m, int levelTime)
{
	if (m->nextthink > 0 && m->nextthink <= levelTime)
	{
		// the fork's fuse detonation adds the burst point as a last waypoint
		mpEndIsWaypoint = qtrue;
		return qtrue;
	}

	return qfalse;
}

/**
 * @brief MissileGroundCheck()
 * @param[in] m
 * @return qtrue if there is ground right below the round
 */
static qboolean G_MP_HasGround(mpMissile_t *m)
{
	vec3_t  end;
	trace_t tr;

	VectorCopy(m->currentOrigin, end);
	end[2] -= 4;

	// the real trace passes the missile itself, there is no such entity here
	trap_Trace(&tr, m->currentOrigin, m->mins, m->maxs, end, ENTITYNUM_NONE, MASK_MISSILESHOT);

	return (tr.fraction == 1.f) ? qfalse : qtrue;
}

/**
 * @brief G_BounceMissile()
 * @param[in,out] m
 * @param[in] tr
 * @param[in] previousTime
 * @param[in] levelTime
 * @return MP_EXPLODED for a rifle grenade that bounced late enough, MP_FLYING otherwise
 */
static mpStatus_t G_MP_Bounce(mpMissile_t *m, trace_t *tr, int previousTime, int levelTime)
{
	qboolean  riflenade = (GetWeaponTableData(m->weapon)->type & WEAPON_TYPE_RIFLENADE) ? qtrue : qfalse;
	vec3_t    velocity;
	float     dot;
	int       hitTime;
	gentity_t *ground;

	// boom after 750 msecs
	if (riflenade && (m->nextthink - levelTime) < 3250)
	{
		return MP_EXPLODED;
	}

	// reflect the velocity on the trace plane
	hitTime = (int)(previousTime + ((levelTime - previousTime) * tr->fraction));
	BG_EvaluateTrajectoryDelta(&m->pos, hitTime, velocity, qfalse, 0);
	dot = DotProduct(velocity, tr->plane.normal);
	VectorMA(velocity, -2 * dot, tr->plane.normal, m->pos.trDelta);

	// bounces without any velocity do not constitute proper bounces
	if (velocity[0] == 0.0 && velocity[1] == 0.0 && velocity[2] == 0.0)
	{
		return MP_FLYING;
	}

	if (tr->plane.normal[2] > 0.2f)
	{
		m->groundEntityNum = tr->entityNum;
	}

	ground = (m->groundEntityNum != ENTITYNUM_NONE) ? &g_entities[m->groundEntityNum] : NULL;

	if (m->groundEntityNum != ENTITYNUM_WORLD && ground)
	{
		VectorMA(m->pos.trDelta, 0.85f, ground->instantVelocity, m->pos.trDelta);
	}

	if (m->eFlags & EF_BOUNCE_HALF)
	{
		if (m->eFlags & EF_BOUNCE)
		{
			VectorScale(m->pos.trDelta, 0.35f, m->pos.trDelta);
		}
		else
		{
			VectorScale(m->pos.trDelta, 0.65f, m->pos.trDelta);
		}

		if (m->groundEntityNum != ENTITYNUM_WORLD)
		{
			VectorScale(m->pos.trDelta, 0.5f, m->pos.trDelta);
		}

		// check for stop
		if ((tr->plane.normal[2] > 0.2f && VectorLengthSquared(m->pos.trDelta) < 1600) || (!tr->fraction && !riflenade))
		{
			if (m->contents == CONTENTS_CORPSE)
			{
				m->ownerNum = ENTITYNUM_WORLD;
			}

			// G_SetOrigin()
			VectorCopy(tr->endpos, m->pos.trBase);
			m->pos.trType     = TR_STATIONARY;
			m->pos.trTime     = 0;
			m->pos.trDuration = 0;
			VectorClear(m->pos.trDelta);
			VectorCopy(tr->endpos, m->currentOrigin);

			if (riflenade)
			{
				// explode one 750msecs after launchtime
				m->nextthink = levelTime + (750 - (levelTime + 4000 - m->nextthink));
			}

			return MP_FLYING;
		}
	}

	SnapVector(m->pos.trDelta);

	VectorAdd(m->currentOrigin, tr->plane.normal, m->currentOrigin);
	VectorCopy(m->currentOrigin, m->pos.trBase);
	SnapVector(m->pos.trBase);
	m->pos.trTime = levelTime;

	return MP_FLYING;
}

/**
 * @brief G_MissileImpact(), without breaking func_explosives: whether a round breaks one depends on
 * damage rules this doesn't replay, so they count as solid
 * @param[in,out] m
 * @param[in] tr
 * @param[in] previousTime
 * @param[in] levelTime
 * @param[out] res
 * @return
 */
static mpStatus_t G_MP_Impact(mpMissile_t *m, trace_t *tr, int previousTime, int levelTime, mpResult_t *res)
{
	gentity_t *other = &g_entities[tr->entityNum];

	if ((!other->takedamage || !m->damage) && (m->eFlags & (EF_BOUNCE | EF_BOUNCE_HALF)))
	{
		return G_MP_Bounce(m, tr, previousTime, levelTime);
	}

	if (other->takedamage || other->dmgparent)
	{
		// if no damage value, then this is a splash damage grenade only
		if (!m->damage)
		{
			return G_MP_Bounce(m, tr, previousTime, levelTime);
		}

		// struck directly: the entity takes the impact damage and is left out of the splash
		res->hitEntityNum = other->s.number;
	}

	return MP_EXPLODED;
}

/**
 * @brief The part of G_RunMissile() that runs while a round travels through the sky
 * @param[in,out] m
 * @param[in] origin
 * @param[in] levelTime
 * @return
 */
static mpStatus_t G_MP_RunSkyFrame(mpMissile_t *m, const vec3_t origin, int levelTime)
{
	trace_t tr;
	vec3_t  tmp;

	if (m->currentOrigin[0] < level.mapcoordsMins[0] ||
	    m->currentOrigin[1] > level.mapcoordsMins[1] ||
	    m->currentOrigin[0] > level.mapcoordsMaxs[0] ||
	    m->currentOrigin[1] < level.mapcoordsMaxs[1])
	{
		return MP_VANISHED;
	}

	VectorCopy(origin, tmp);
	tmp[2] = MAX_MAP_SIZE;
	trap_Trace(&tr, origin, NULL, NULL, tmp, ENTITYNUM_NONE, m->clipmask);

	if (tr.fraction == 1.f)
	{
		// keep flying, the think runs before the origin catches up
		if (G_MP_ThinkDue(m, levelTime))
		{
			return MP_EXPLODED;
		}

		VectorCopy(origin, m->currentOrigin);
		G_MP_AddPoint(origin, qfalse);
		return MP_FLYING;
	}

	// only re-enter through a skybox
	if (!(tr.surfaceFlags & SURF_SKY))
	{
		return MP_VANISHED;
	}

	tmp[2] = -MAX_MAP_SIZE;
	trap_Trace(&tr, origin, NULL, NULL, tmp, ENTITYNUM_NONE, m->clipmask);

	// under the ground limit
	if (tr.fraction == 1.f)
	{
		return MP_VANISHED;
	}

	// above the sky limit, keep flying without moving the origin
	if (tr.surfaceFlags & SURF_SKY)
	{
		if (G_MP_ThinkDue(m, levelTime))
		{
			return MP_EXPLODED;
		}

		G_MP_AddPoint(origin, qfalse);
		return MP_FLYING;
	}

	// back in the world, keep going like normal from the next frame on
	VectorCopy(origin, m->currentOrigin);
	m->inSky = qfalse;
	G_MP_AddPoint(origin, qfalse);

	return MP_FLYING;
}

/**
 * @brief One server frame of G_RunMissile()
 * @param[in,out] m
 * @param[in] previousTime
 * @param[in] levelTime
 * @param[out] res
 * @return
 */
static mpStatus_t G_MP_RunFrame(mpMissile_t *m, int previousTime, int levelTime, mpResult_t *res)
{
	qboolean   throwable = (GetWeaponTableData(m->weapon)->firingMode & WEAPON_FIRING_MODE_THROWABLE) ? qtrue : qfalse;
	qboolean   bounced   = qfalse;
	vec3_t     origin;
	trace_t    tr;
	mpStatus_t status;

	if (throwable)
	{
		if (!G_MP_HasGround(m))
		{
			m->groundEntityNum = ENTITYNUM_NONE;
		}

		if (m->groundEntityNum == ENTITYNUM_NONE && m->pos.trType != TR_GRAVITY)
		{
			m->pos.trType = TR_GRAVITY;
			m->pos.trTime = levelTime;
		}
	}

	BG_EvaluateTrajectory(&m->pos, levelTime, origin, qfalse, 0);

	if ((m->clipmask & CONTENTS_BODY) && throwable &&
	    m->pos.trDelta[0] == 0.f && m->pos.trDelta[1] == 0.f && m->pos.trDelta[2] == 0.f)
	{
		m->clipmask &= ~CONTENTS_BODY;
	}

	if (m->contents != CONTENTS_CORPSE && m->inSky)
	{
		return G_MP_RunSkyFrame(m, origin, levelTime);
	}

	// trace a line from the previous position to the current position,
	// ignoring interactions with the missile owner
	trap_Trace(&tr, m->currentOrigin, m->mins, m->maxs, origin, m->ownerNum, m->clipmask);

	VectorCopy(tr.endpos, m->currentOrigin);

	if (tr.startsolid)
	{
		tr.fraction = 0;
	}
	else
	{
		m->lastSurfaceFlags = tr.surfaceFlags;
	}

	if (tr.fraction != 1.f)
	{
		if (m->contents != CONTENTS_CORPSE && ((tr.surfaceFlags & SURF_SKY) || (m->lastSurfaceFlags & SURF_SKY)))
		{
			// goes through sky
			m->inSky = qtrue;

			if (G_MP_ThinkDue(m, levelTime))
			{
				return MP_EXPLODED;
			}

			G_MP_AddPoint(origin, qfalse);
			return MP_FLYING;
		}

		// never explode or bounce on sky
		if (tr.surfaceFlags & SURF_NOIMPACT)
		{
			mpEndIsWaypoint = qtrue;
			return MP_VANISHED;
		}

		if (m->contents != CONTENTS_CORPSE || m->pos.trType != TR_STATIONARY)
		{
			status = G_MP_Impact(m, &tr, previousTime, levelTime, res);

			if (status != MP_FLYING)
			{
				return status;
			}

			G_MP_AddPoint(tr.endpos, qtrue);
			bounced = qtrue;
		}
	}
	else if (VectorLengthSquared(m->pos.trDelta) != 0.f)
	{
		// free fall/no intersection
		m->groundEntityNum = ENTITYNUM_NONE;
	}

	// a bounce leaves the round one unit off the contact point that was just recorded
	if (!bounced)
	{
		G_MP_AddPoint(m->currentOrigin, qfalse);
	}

	// check think function after bouncing
	return G_MP_ThinkDue(m, levelTime) ? MP_EXPLODED : MP_FLYING;
}

/**
 * @brief Simulates the round the player would fire right now
 * @param[in] ent
 * @param[out] res
 */
static void G_MP_Simulate(gentity_t *ent, mpResult_t *res)
{
	weapon_t      weapon    = G_MP_RoundWeapon((weapon_t)ent->client->ps.weapon);
	weaponTable_t *wt       = GetWeaponTableData(weapon);
	int           frameTime = level.frameTime > 0 ? level.frameTime : DEFAULT_SV_FRAMETIME;
	mpStatus_t    status    = MP_FLYING;
	mpMissile_t   m;
	vec3_t        start, velocity;
	int           maxTime, previousTime, levelTime;
	int           launchTime, waypointInterval, nextWaypoint;

	res->weapon       = weapon;
	res->explodes     = qfalse;
	res->restTime     = 0;
	res->hitEntityNum = ENTITYNUM_NONE;

	mpNumPoints       = 0;
	mpNumCamWaypoints = 0;
	mpEndIsWaypoint   = qfalse;

	G_MP_Launch(ent, weapon, start, velocity);
	G_MP_InitMissile(ent, weapon, start, velocity, &m);
	G_MP_AddPoint(start, qtrue);
	G_MP_AddCamWaypoint();
	G_MP_LastPointWaypoint(&mpCamSegStart);

	// the fork spreads its waypoints evenly over its own simulation cap, 12s for the mortar, 4.5s otherwise,
	// counting flight time from the round's backdated trTime
	launchTime       = m.pos.trTime;
	waypointInterval = ((wt->type & WEAPON_TYPE_MORTAR) ? 12000 : 4500) / (MP_CAM_WAYPOINTS - 1);
	nextWaypoint     = waypointInterval;

	// a mortar shell takes over 4 seconds to come back down to launch height
	if (wt->type & WEAPON_TYPE_MORTAR)
	{
		maxTime = 12000;
	}
	else if (wt->type & WEAPON_TYPE_PANZER)
	{
		maxTime = 8000;
	}
	else
	{
		maxTime = 4500;
	}

	if (m.nextthink && m.nextthink - level.time + frameTime > maxTime)
	{
		maxTime = m.nextthink - level.time + frameTime;
	}

	// the round is spawned in ClientThink(), between server frames, and first moves on the next one
	levelTime = level.time;

	while (levelTime - level.time < maxTime)
	{
		// the fork's trajectory ends once the round comes to rest, so do its waypoints
		if (m.pos.trType != TR_STATIONARY)
		{
			if (levelTime - launchTime >= nextWaypoint)
			{
				G_MP_AddCamWaypoint();
				nextWaypoint += waypointInterval;
			}

			G_MP_LastPointWaypoint(&mpCamSegStart);
		}

		previousTime = levelTime;
		levelTime   += frameTime;

		status = G_MP_RunFrame(&m, previousTime, levelTime, res);

		// a resting throwable can still drop again when MissileGroundCheck() finds nothing below it
		if (m.pos.trType == TR_STATIONARY)
		{
			if (!res->restTime)
			{
				res->restTime = levelTime - level.time;
			}
		}
		else
		{
			res->restTime = 0;
		}

		if (status != MP_FLYING)
		{
			break;
		}

		// at rest with nothing left to wait for
		if (m.pos.trType == TR_STATIONARY && !m.nextthink && G_MP_HasGround(&m))
		{
			status = MP_RESTING;
			break;
		}
	}

	VectorCopy(m.currentOrigin, res->end);

	// a round still in the air when the simulation runs out has no known outcome
	res->explodes = (status == MP_EXPLODED || status == MP_RESTING) ? qtrue : qfalse;
	res->status   = status;

	// spawned at level.time, it ends with the server frame that ran out
	res->flightTime = levelTime - level.time;

	G_MP_AddPoint(res->end, qtrue);

	if (mpEndIsWaypoint && m.pos.trType != TR_STATIONARY)
	{
		G_MP_AddCamWaypoint();
	}
}

/**
 * @brief Predicts the damage a missiletarget box takes, mirroring G_MissileImpact() and G_RadiusDamage()
 * @param[in] target
 * @param[in] res
 * @param[out] distOut distance used for the falloff
 * @param[out] blockedOut set when the box is out of line of sight
 * @return raw damage, before the client only reductions G_Damage() applies to players
 */
static int G_MP_PredictDamage(gentity_t *target, const mpResult_t *res, float *distOut, qboolean *blockedOut)
{
	weaponTable_t *wt     = GetWeaponTableData(res->weapon);
	float         radius  = wt->splashRadius;
	float         damage  = wt->splashDamage;
	vec3_t        origin, v, midpoint;
	float         dist, points;
	trace_t       tr;

	*blockedOut = qfalse;

	VectorCopy(res->end, origin);

	// G_ExplodeMissile() raises the blast for dynamite
	if (res->weapon == WP_DYNAMITE)
	{
		origin[2] += 4;
	}

	G_AdjustedDamageVec(target, origin, v);
	dist      = VectorLength(v);
	*distOut  = dist;

	if (res->hitEntityNum == target->s.number)
	{
		return wt->damage;
	}

	if (!damage)
	{
		return 0;
	}

	if (radius < 1)
	{
		radius = 1;
	}

	if (dist >= radius)
	{
		return 0;
	}

	points = damage * (1.0f - dist / radius);

	if (CanDamage(target, origin))
	{
		return (int)round(points);
	}

	*blockedOut = qtrue;

	VectorAdd(target->r.absmin, target->r.absmax, midpoint);
	VectorScale(midpoint, 0.5f, midpoint);

	trap_Trace(&tr, origin, vec3_origin, vec3_origin, midpoint, ENTITYNUM_NONE, MASK_SOLID);
	if (tr.fraction < 1.0f && Distance(midpoint, origin) < radius * 0.2f)
	{
		return (int)round(points * 0.1f);
	}

	return 0;
}

/*
======================================================================
DISPLAY
======================================================================
*/

/**
 * @brief G_MP_SpawnBeam
 * @param[in] clientNum
 * @return a hidden beam
 */
static gentity_t *G_MP_SpawnBeam(int clientNum)
{
	gentity_t *beam = G_Spawn();

	beam->classname      = mpBeamClassname;
	beam->s.eType        = ET_BEAM;
	beam->r.svFlags      = SVF_NOCLIENT;
	beam->r.singleClient = clientNum;
	beam->r.contents     = 0;
	beam->s.legsAnim     = 0; // CG_Beam() draws railCore for anything but 1

	return beam;
}

/**
 * @brief G_MP_ShowBeam
 * @param[in,out] beam
 * @param[in] start
 * @param[in] end
 * @param[in] color
 * @param[in] svFlags who gets to see it
 */
static void G_MP_ShowBeam(gentity_t *beam, vec3_t start, vec3_t end, const vec3_t color, int svFlags)
{
	G_SetOrigin(beam, start);
	VectorCopy(end, beam->s.origin2);
	VectorCopy(color, beam->s.angles2);
	beam->r.svFlags = svFlags;
	trap_LinkEntity(beam);
}

/**
 * @brief G_MP_HideBeam
 * @param[in,out] beam
 */
static void G_MP_HideBeam(gentity_t *beam)
{
	beam->r.svFlags = SVF_NOCLIENT;
}

/**
 * @brief Makes sure the beams and the camera of a client exist
 * @param[in] clientNum
 * @param[in,out] st
 * @return qfalse if there aren't enough free entities to spare
 */
static qboolean G_MP_EnsureEntities(int clientNum, mpClient_t *st)
{
	int needed = 0;
	int i;

	for (i = 0; i < MP_BEAMS; i++)
	{
		if (!G_MP_Owns(st->beams[i], mpBeamClassname, clientNum))
		{
			needed++;
		}
	}

	if (!G_MP_Owns(st->camera, mpCameraClassname, clientNum))
	{
		needed++;
	}

	if (!needed)
	{
		return qtrue;
	}

	if (G_EntitiesFree() - needed < MP_ENTITY_RESERVE)
	{
		if (!st->warnedNoEntities)
		{
			trap_SendServerCommand(clientNum, "print \"Missile preview: not enough free entities on this map.\n\"");
			st->warnedNoEntities = qtrue;
		}
		return qfalse;
	}

	for (i = 0; i < MP_BEAMS; i++)
	{
		if (!G_MP_Owns(st->beams[i], mpBeamClassname, clientNum))
		{
			st->beams[i] = G_MP_SpawnBeam(clientNum);
		}
	}

	if (!G_MP_Owns(st->camera, mpCameraClassname, clientNum))
	{
		gentity_t *camera = G_Spawn();

		// a missile with no model, trail or sound that the client takes for one of its own,
		// G_RunEntity() leaves it alone
		camera->classname      = mpCameraClassname;
		camera->s.eType        = ET_MISSILE;
		camera->s.weapon       = WP_NONE;
		camera->s.clientNum    = clientNum;
		camera->r.svFlags      = SVF_NOCLIENT;
		camera->r.singleClient = clientNum;
		camera->r.contents     = 0;
		camera->clipmask       = 0;
		camera->s.pos.trType   = TR_LINEAR;

		st->camera = camera;
	}

	return qtrue;
}

/**
 * @brief Picks the arc vertices for the beams: every frame sample if they fit, otherwise
 * evenly thinned out while keeping the launch, the bounces and the end
 * @param[out] idx room for MP_ARC_SEGMENTS + 1 indexes into mpPoints
 * @return number of vertices
 */
static int G_MP_SelectVertices(int *idx)
{
	int n     = mpNumPoints;
	int keys  = 0;
	int count = 0;
	int stride, i;

	if (n <= MP_ARC_SEGMENTS + 1)
	{
		for (i = 0; i < n; i++)
		{
			idx[i] = i;
		}
		return n;
	}

	for (i = 0; i < n; i++)
	{
		if (i == 0 || i == n - 1 || mpKeyPoints[i])
		{
			keys++;
		}
	}

	if (keys < MP_ARC_SEGMENTS + 1)
	{
		for (stride = n / (MP_ARC_SEGMENTS + 1 - keys) + 1; stride < n; stride++)
		{
			count = 0;

			for (i = 0; i < n; i++)
			{
				if (i == 0 || i == n - 1 || mpKeyPoints[i] || !(i % stride))
				{
					count++;
				}
			}

			if (count <= MP_ARC_SEGMENTS + 1)
			{
				break;
			}
		}

		if (count <= MP_ARC_SEGMENTS + 1)
		{
			count = 0;

			for (i = 0; i < n; i++)
			{
				if (i == 0 || i == n - 1 || mpKeyPoints[i] || !(i % stride))
				{
					idx[count++] = i;
				}
			}

			return count;
		}
	}

	// too many bounces to keep them all
	for (i = 0; i <= MP_ARC_SEGMENTS; i++)
	{
		idx[i] = i * (n - 1) / MP_ARC_SEGMENTS;
	}

	return MP_ARC_SEGMENTS + 1;
}

/**
 * @brief Stores the arc vertices and the marker at the detonation point, which fans back the way
 * the round came
 * @param[in,out] st
 * @param[in] res
 * @param[in] camPoint index into mpPoints the missile camera sits at
 */
static void G_MP_BuildArc(mpClient_t *st, const mpResult_t *res, int camPoint)
{
	int      idx[MP_ARC_SEGMENTS + 1];
	int      numVerts = G_MP_SelectVertices(idx);
	qboolean changed  = (numVerts != st->numArcVerts || res->explodes != st->markerShown) ? qtrue : qfalse;
	int      i;

	st->camVert = 0;

	for (i = 0; i < numVerts; i++)
	{
		if (!changed && !VectorCompare(st->arcVerts[i], mpPoints[idx[i]]))
		{
			changed = qtrue;
		}

		VectorCopy(mpPoints[idx[i]], st->arcVerts[i]);

		if (idx[i] <= camPoint)
		{
			st->camVert = i;
		}
	}

	st->numArcVerts = numVerts;
	st->markerShown = (res->explodes && mpNumPoints >= 2) ? qtrue : qfalse;

	if (st->markerShown)
	{
		vec3_t axis, right, up;

		VectorCopy(res->end, st->markerEnd);
		VectorSubtract(mpPoints[mpNumPoints - 1], mpPoints[mpNumPoints - 2], axis);
		if (VectorNormalize(axis) < 1.0f)
		{
			VectorSet(axis, 0, 0, 1);
		}
		VectorNegate(axis, axis);
		MakeNormalVectors(axis, right, up);

		for (i = 0; i < MP_MARKER_LINES; i++)
		{
			float angle = DEG2RAD(i * 90.0f + 45.0f);

			VectorMA(st->markerEnd, 45.f, axis, st->markerBase[i]);
			VectorMA(st->markerBase[i], 30.f * cos(angle), right, st->markerBase[i]);
			VectorMA(st->markerBase[i], 30.f * sin(angle), up, st->markerBase[i]);
		}
	}

	if (changed)
	{
		st->railsDirty = qtrue;
	}
}

/**
 * @brief While its owner's camera rails are up the target box is drawn for the owner by rails,
 * everyone else keeps seeing the beams
 * @param[in,out] st
 * @param[in] clientNum
 */
static void G_MP_SetTargetEdgeFlags(mpClient_t *st, int clientNum)
{
	int i;

	for (i = 0; i < MP_TARGET_EDGES; i++)
	{
		if (G_MP_Owns(st->targetEdges[i], mpBeamClassname, clientNum) && !(st->targetEdges[i]->r.svFlags & SVF_NOCLIENT))
		{
			st->targetEdges[i]->r.svFlags = st->camRails ? (SVF_BROADCAST | SVF_NOTSINGLECLIENT) : SVF_BROADCAST;
		}
	}
}

/**
 * @brief Shows the stored arc as beams, minus what the camera rails draw
 * @param[in] clientNum
 * @param[in,out] st
 */
static void G_MP_ApplyArc(int clientNum, mpClient_t *st)
{
	const int flags     = SVF_SINGLECLIENT | SVF_BROADCAST;
	int       firstRail = st->camRails ? st->camVert : st->numArcVerts - 1;
	int       i;

	G_MP_SetTargetEdgeFlags(st, clientNum);

	for (i = 0; i < MP_BEAMS; i++)
	{
		if (!G_MP_Owns(st->beams[i], mpBeamClassname, clientNum))
		{
			return;
		}
	}

	for (i = 0; i < MP_ARC_SEGMENTS; i++)
	{
		if (i < firstRail && i < st->numArcVerts - 1)
		{
			G_MP_ShowBeam(st->beams[i], st->arcVerts[i], st->arcVerts[i + 1], mpArcColor, flags);
		}
		else
		{
			G_MP_HideBeam(st->beams[i]);
		}
	}

	for (i = 0; i < MP_MARKER_LINES; i++)
	{
		if (st->markerShown && !st->camRails)
		{
			G_MP_ShowBeam(st->beams[MP_ARC_SEGMENTS + i], st->markerEnd, st->markerBase[i], mpMarkerColor, flags);
		}
		else
		{
			G_MP_HideBeam(st->beams[MP_ARC_SEGMENTS + i]);
		}
	}
}

/**
 * @brief Rail indexes are fresh for every camera session: the client keeps the index of a rail
 * it has freed, and CG_FindLocalEntity() could otherwise hand a new rail that dead one
 * @param[in] clientNum
 * @param[in] st
 * @param[in] slot
 * @return
 */
static int G_MP_RailIndex(int clientNum, const mpClient_t *st, int slot)
{
	return MP_RAIL_INDEX_BASE | (clientNum << 16) | ((st->railGeneration & 0xff) << 8) | slot;
}

/**
 * @brief Sends one rail (EV_RAILTRAIL, drawn by CG_RailTrail()) to a client and whoever follows it
 * @param[in] clientNum
 * @param[in] start
 * @param[in] end
 * @param[in] color
 * @param[in] type 0 a line, 1 a box between two opposite corners
 * @param[in] index rails with the same index replace each other instead of piling up
 */
static void G_MP_SendRail(int clientNum, vec3_t start, vec3_t end, const vec3_t color, int type, int index)
{
	gentity_t *tent = G_TempEntity(start, EV_RAILTRAIL);

	// G_TempEntity() snaps the origin, keep the exact point
	G_SetOrigin(tent, start);
	trap_LinkEntity(tent);

	VectorCopy(end, tent->s.origin2);
	tent->s.dmgFlags     = type;
	tent->s.angles[0]    = (int)(color[0] * 255);
	tent->s.angles[1]    = (int)(color[1] * 255);
	tent->s.angles[2]    = (int)(color[2] * 255);
	tent->s.effect1Time  = index;
	tent->r.svFlags     |= SVF_SINGLECLIENT | SVF_BROADCAST;
	tent->r.singleClient = clientNum;
}

/**
 * @brief Sends the target box rail, or a box of no size to make the client's one vanish
 * @param[in] clientNum
 * @param[in,out] st
 * @param[in] collapse
 */
static void G_MP_SendBoxRail(int clientNum, mpClient_t *st, qboolean collapse)
{
	gentity_t *target = st->target;
	vec3_t    b1, b2;

	if (!G_MP_Owns(target, mpTargetClassname, clientNum))
	{
		st->boxRailSent = qfalse;
		return;
	}

	VectorAdd(target->r.currentOrigin, target->r.mins, b1);
	VectorAdd(target->r.currentOrigin, target->r.maxs, b2);

	if (collapse)
	{
		VectorCopy(b1, b2);
	}

	G_MP_SendRail(clientNum, b1, b2, st->targetArmed ? mpTargetColor : mpTargetUnarmedColor, 1,
	              G_MP_RailIndex(clientNum, st, MP_RAIL_BOX_SLOT));

	st->boxRailSent     = collapse ? qfalse : qtrue;
	st->boxRailDirty    = qfalse;
	st->lastBoxRailTime = level.time;
}

/**
 * @brief Sends the part of the arc the camera sees, thinned to MP_CAM_RAILS segments, and the marker
 * @param[in] clientNum
 * @param[in,out] st
 */
static void G_MP_SendCamRails(int clientNum, mpClient_t *st)
{
	vec3_t verts[MP_CAM_RAILS + 1];
	int    numVerts = 0;
	int    last     = st->numArcVerts - 1;
	int    stride, i;

	if (last < 0)
	{
		return;
	}

	stride = (last - st->camVert + MP_CAM_RAILS - 1) / MP_CAM_RAILS;
	if (stride < 1)
	{
		stride = 1;
	}

	// VectorCopy() is a macro that evaluates its arguments more than once, so no ++ inside it
	for (i = st->camVert; i < last; i += stride)
	{
		VectorCopy(st->arcVerts[i], verts[numVerts]);
		numVerts++;
	}
	VectorCopy(st->arcVerts[last], verts[numVerts]);
	numVerts++;

	for (i = 0; i < numVerts - 1; i++)
	{
		G_MP_SendRail(clientNum, verts[i], verts[i + 1], mpArcColor, 0, G_MP_RailIndex(clientNum, st, i));
	}

	// a shorter tail than last time leaves rails behind, shrink them to nothing
	for (; i < st->arcRailsSent; i++)
	{
		G_MP_SendRail(clientNum, verts[numVerts - 1], verts[numVerts - 1], mpArcColor, 0, G_MP_RailIndex(clientNum, st, i));
	}

	st->arcRailsSent = numVerts - 1;

	if (st->markerShown || st->markerRailsSent)
	{
		for (i = 0; i < MP_MARKER_LINES; i++)
		{
			G_MP_SendRail(clientNum, st->markerEnd, st->markerShown ? st->markerBase[i] : st->markerEnd, mpMarkerColor, 0,
			              G_MP_RailIndex(clientNum, st, MP_CAM_RAILS + i));
		}

		st->markerRailsSent = st->markerShown;
	}

	st->railsDirty   = qfalse;
	st->lastRailTime = level.time;
}

/**
 * @brief Shrinks every rail the client still shows to nothing, they would otherwise stay up for
 * cg_railTrailTime on top of the beams
 * @param[in] clientNum
 * @param[in,out] st
 */
static void G_MP_CollapseCamRails(int clientNum, mpClient_t *st)
{
	vec3_t point;
	int    i;

	if (G_EntitiesFree() - (MP_RAIL_BOX_SLOT + 1) >= MP_ENTITY_RESERVE)
	{
		if (st->numArcVerts)
		{
			VectorCopy(st->arcVerts[st->numArcVerts - 1], point);
		}
		else
		{
			VectorClear(point);
		}

		for (i = 0; i < st->arcRailsSent; i++)
		{
			G_MP_SendRail(clientNum, point, point, mpArcColor, 0, G_MP_RailIndex(clientNum, st, i));
		}

		if (st->markerRailsSent)
		{
			for (i = 0; i < MP_MARKER_LINES; i++)
			{
				G_MP_SendRail(clientNum, point, point, mpMarkerColor, 0, G_MP_RailIndex(clientNum, st, MP_CAM_RAILS + i));
			}
		}

		if (st->boxRailSent)
		{
			G_MP_SendBoxRail(clientNum, st, qtrue);
		}
	}

	st->arcRailsSent    = 0;
	st->markerRailsSent = qfalse;
	st->boxRailSent     = qfalse;
}

/**
 * @brief Keeps the camera rails up to date while the camera is shown
 * @param[in] clientNum
 * @param[in,out] st
 */
static void G_MP_UpdateCamRails(int clientNum, mpClient_t *st)
{
	if (G_EntitiesFree() - (MP_RAIL_BOX_SLOT + 1) < MP_ENTITY_RESERVE)
	{
		return;
	}

	if (level.time - st->lastRailTime >= MP_RAIL_REFRESH_MSEC || (st->railsDirty && level.time - st->lastRailTime >= MP_RAIL_MSEC))
	{
		G_MP_SendCamRails(clientNum, st);
	}

	if (G_MP_Owns(st->target, mpTargetClassname, clientNum) &&
	    (st->boxRailDirty || level.time - st->lastBoxRailTime >= MP_RAIL_REFRESH_MSEC))
	{
		G_MP_SendBoxRail(clientNum, st, qfalse);
	}
}

/**
 * @brief Places the camera the way the fork's CG_DrawMissileCamera() does: back from the end over
 * at most MP_CAM_LOOKBACK waypoints, at the first one MP_CAMERA_DISTANCE away from the end, else
 * at the last one looked at, and if that is on top of the end, at the start of the final frame
 * @param[in] ent
 * @param[in,out] st
 * @param[in] res
 * @return index into mpPoints the camera sits at
 */
static int G_MP_PlaceCamera(gentity_t *ent, mpClient_t *st, const mpResult_t *res)
{
	gentity_t       *camera = st->camera;
	int             count   = MIN(mpNumCamWaypoints, MP_CAM_LOOKBACK + 1);
	mpCamWaypoint_t *wp     = NULL;
	vec3_t          camPos, dir;
	int             camPoint = mpNumPoints - 1;
	int             i;

	VectorCopy(res->end, camPos);

	for (i = 0; i < count; i++)
	{
		wp = &mpCamWaypoints[(mpNumCamWaypoints - 1 - i) % (MP_CAM_LOOKBACK + 1)];

		VectorCopy(wp->pos, camPos);
		camPoint = wp->point;

		if (DistanceSquared(wp->pos, res->end) >= Square(MP_CAMERA_DISTANCE))
		{
			break;
		}
	}

	VectorSubtract(res->end, camPos, dir);
	if (VectorLengthSquared(dir) < 1.f)
	{
		VectorCopy(mpCamSegStart.pos, camPos);
		camPoint = mpCamSegStart.point;
		VectorSubtract(res->end, camPos, dir);
	}

	if (VectorNormalize(dir) == 0.f)
	{
		// the fork looks along +x here, the view direction is more use
		AngleVectors(ent->client->ps.viewangles, dir, NULL, NULL);
	}

	// CG_DrawMissileCamera() sits at the missile and looks along its velocity, 1u/s is enough
	camera->s.pos.trType = TR_LINEAR;
	camera->s.pos.trTime = level.time;
	VectorCopy(camPos, camera->s.pos.trBase);
	VectorCopy(dir, camera->s.pos.trDelta);
	VectorCopy(camPos, camera->r.currentOrigin);
	trap_LinkEntity(camera);

	return camPoint;
}

/**
 * @brief G_MP_SetCameraVisible
 * @param[in] clientNum
 * @param[in,out] st
 * @param[in] visible
 */
static void G_MP_SetCameraVisible(int clientNum, mpClient_t *st, qboolean visible)
{
	if (!G_MP_Owns(st->camera, mpCameraClassname, clientNum))
	{
		return;
	}

	st->camera->r.svFlags = visible ? (SVF_SINGLECLIENT | SVF_BROADCAST) : SVF_NOCLIENT;
}

/**
 * @brief The client's missile camera follows the last of its own moving missiles it processes,
 * so a real round in flight must not have to compete with the preview camera
 * @param[in] clientNum
 * @param[in] camera
 * @return
 */
static qboolean G_MP_HasMissileInFlight(int clientNum, gentity_t *camera)
{
	gentity_t *e;
	int       i;

	for (i = MAX_CLIENTS, e = &g_entities[MAX_CLIENTS]; i < level.num_entities; i++, e++)
	{
		if (!e->inuse || e == camera || e->s.eType != ET_MISSILE)
		{
			continue;
		}

		if (e->s.clientNum == clientNum && e->s.pos.trType != TR_STATIONARY && !(e->r.svFlags & SVF_NOCLIENT))
		{
			return qtrue;
		}
	}

	return qfalse;
}

/**
 * @brief Sends a pending readout, at most every MP_READOUT_MSEC
 * @param[in] clientNum
 * @param[in,out] st
 */
static void G_MP_FlushReadout(int clientNum, mpClient_t *st)
{
	if (!st->readoutPending[0] || level.time - st->lastReadoutTime < MP_READOUT_MSEC)
	{
		return;
	}

	trap_SendServerCommand(clientNum, va("cp \"%s\"", st->readoutPending));

	Q_strncpyz(st->readoutSent, st->readoutPending, sizeof(st->readoutSent));
	st->readoutPending[0] = '\0';
	st->lastReadoutTime   = level.time;
}

/**
 * @brief G_MP_SetReadout
 * @param[in] clientNum
 * @param[in,out] st
 * @param[in] text
 */
static void G_MP_SetReadout(int clientNum, mpClient_t *st, const char *text)
{
	if (!strcmp(text, st->readoutSent) && level.time - st->lastReadoutTime < MP_READOUT_REFRESH_MSEC)
	{
		st->readoutPending[0] = '\0';
		return;
	}

	Q_strncpyz(st->readoutPending, text, sizeof(st->readoutPending));
	G_MP_FlushReadout(clientNum, st);
}

/**
 * @brief Readout of the detonation point, as the fork's missile camera showed it, led by the flight time,
 * or for thrown rounds the time until they lie still:
 * "<time>s [to rest] | <height above ground> (z:<z>) | <distance to the target>u <damage> dmg [DIRECT] [(no LOS)]"
 * @param[in] ent
 * @param[in,out] st
 * @param[in] res
 */
static void G_MP_UpdateReadout(gentity_t *ent, mpClient_t *st, const mpResult_t *res)
{
	int     clientNum = ent->s.number;
	float   seconds   = res->flightTime / 1000.f;
	char    text[MP_READOUT_SIZE];
	char    timeInfo[32];
	char    targetInfo[64];
	vec3_t  down;
	trace_t tr;
	float   groundDist;

	if (res->status == MP_VANISHED)
	{
		G_MP_SetReadout(clientNum, st, va("^1no detonation ^7- the round is gone after %.2fs", seconds));
		return;
	}

	if (!res->explodes)
	{
		G_MP_SetReadout(clientNum, st, va("^1no detonation ^7- still flying after %.2fs", seconds));
		return;
	}

	// a thrown round usually lies still well before its fuse runs out, and the fuse is known anyway,
	// so when it stops moving tells more; dynamite, landmine, satchel and smoke marker only ever rest
	if (res->restTime && (res->status == MP_RESTING || (GetWeaponTableData(res->weapon)->firingMode & WEAPON_FIRING_MODE_THROWABLE)))
	{
		Com_sprintf(timeInfo, sizeof(timeInfo), "%.2fs to rest | ", res->restTime / 1000.f);
	}
	else
	{
		Com_sprintf(timeInfo, sizeof(timeInfo), "%.2fs | ", seconds);
	}

	VectorCopy(res->end, down);
	down[2] -= 8192.f;
	trap_Trace(&tr, res->end, NULL, NULL, down, clientNum, MASK_MISSILESHOT);
	groundDist = tr.fraction * 8192.f;

	targetInfo[0] = '\0';

	if (G_MP_Owns(st->target, mpTargetClassname, clientNum))
	{
		float    dist;
		qboolean blocked;
		int      dmg = G_MP_PredictDamage(st->target, res, &dist, &blocked);

		// the colour code runs to the end of the line, so the damage goes last
		Com_sprintf(targetInfo, sizeof(targetInfo), " | %.0fu %s%i dmg%s%s", dist, G_MP_DamageColor(dmg), dmg,
		            res->hitEntityNum == st->target->s.number ? " DIRECT" : "", blocked ? " (no LOS)" : "");
	}

	if (groundDist > 1.0f)
	{
		Com_sprintf(text, sizeof(text), "%s%.0f (z:%.0f)%s", timeInfo, groundDist, res->end[2], targetInfo);
	}
	else
	{
		Com_sprintf(text, sizeof(text), "%sz:%.0f%s", timeInfo, res->end[2], targetInfo);
	}

	G_MP_SetReadout(clientNum, st, text);
}

/**
 * @brief Removes the arc, the marker and the camera
 * @param[in] clientNum
 * @param[in,out] st
 */
static void G_MP_ClearArc(int clientNum, mpClient_t *st)
{
	int i;

	for (i = 0; i < MP_BEAMS; i++)
	{
		G_MP_Free(&st->beams[i], mpBeamClassname, clientNum);
	}

	G_MP_Free(&st->camera, mpCameraClassname, clientNum);

	if (st->arcRailsSent || st->markerRailsSent || st->boxRailSent)
	{
		G_MP_CollapseCamRails(clientNum, st);
	}

	if (st->camRails)
	{
		st->camRails = qfalse;
		G_MP_SetTargetEdgeFlags(st, clientNum);
	}

	st->numArcVerts       = 0;
	st->markerShown       = qfalse;
	st->readoutPending[0] = '\0';
}

/*
======================================================================
MISSILETARGET
======================================================================
*/

/**
 * @brief G_MP_ClearTarget
 * @param[in] clientNum
 * @param[in,out] st
 */
static void G_MP_ClearTarget(int clientNum, mpClient_t *st)
{
	int i;

	if (st->boxRailSent && G_EntitiesFree() - 1 >= MP_ENTITY_RESERVE)
	{
		G_MP_SendBoxRail(clientNum, st, qtrue);
	}
	st->boxRailSent = qfalse;

	G_MP_Free(&st->target, mpTargetClassname, clientNum);

	for (i = 0; i < MP_TARGET_EDGES; i++)
	{
		G_MP_Free(&st->targetEdges[i], mpBeamClassname, clientNum);
	}

	st->targetArmed = qfalse;
}

/**
 * @brief Draws the box for everyone, since it stops everyone's rounds
 * @param[in,out] st
 * @param[in] color
 */
static void G_MP_DrawTargetBox(mpClient_t *st, const vec3_t color)
{
	gentity_t *target = st->target;
	int       owner   = target->r.singleClient;
	vec3_t    corners[8];
	int       i, bit, edge = 0;

	for (i = 0; i < 8; i++)
	{
		corners[i][0] = target->r.currentOrigin[0] + ((i & 1) ? target->r.maxs[0] : target->r.mins[0]);
		corners[i][1] = target->r.currentOrigin[1] + ((i & 2) ? target->r.maxs[1] : target->r.mins[1]);
		corners[i][2] = target->r.currentOrigin[2] + ((i & 4) ? target->r.maxs[2] : target->r.mins[2]);
	}

	// the 12 edges join corners that differ on one axis
	for (i = 0; i < 8; i++)
	{
		for (bit = 1; bit < 8; bit <<= 1)
		{
			if (!(i & bit))
			{
				if (G_MP_Owns(st->targetEdges[edge], mpBeamClassname, owner))
				{
					G_MP_ShowBeam(st->targetEdges[edge], corners[i], corners[i | bit], color,
					              st->camRails ? (SVF_BROADCAST | SVF_NOTSINGLECLIENT) : SVF_BROADCAST);
				}
				edge++;
			}
		}
	}

	// the owner's camera rails show the box too
	st->boxRailDirty = qtrue;
}

/**
 * @brief G_MP_OwnerInsideTarget
 * @param[in] ent
 * @param[in] target
 * @return qtrue while the owner's own box overlaps the target
 */
static qboolean G_MP_OwnerInsideTarget(gentity_t *ent, gentity_t *target)
{
	int i;

	if (ent->client->sess.sessionTeam == TEAM_SPECTATOR || (ent->client->ps.pm_flags & PMF_LIMBO))
	{
		return qfalse;
	}

	for (i = 0; i < 3; i++)
	{
		if (ent->client->ps.origin[i] + playerMins[i] >= target->r.currentOrigin[i] + target->r.maxs[i] ||
		    ent->client->ps.origin[i] + playerMaxs[i] <= target->r.currentOrigin[i] + target->r.mins[i])
		{
			return qfalse;
		}
	}

	return qtrue;
}

/**
 * @brief Makes the box solid to missiles once its owner stepped out of it. Rounds fired from
 * inside it would otherwise hit it right at the muzzle, so until then they pass through.
 * Splash damage reaches it either way.
 * @param[in] ent
 * @param[in,out] st
 */
static void G_MP_UpdateTargetArming(gentity_t *ent, mpClient_t *st)
{
	if (st->targetArmed || G_MP_OwnerInsideTarget(ent, st->target))
	{
		return;
	}

	// CONTENTS_MISSILECLIP is in MASK_MISSILESHOT only: rounds stop on it like on a player,
	// while players walk through it and bullets pass it
	st->target->r.contents = CONTENTS_MISSILECLIP;
	trap_LinkEntity(st->target);
	st->targetArmed = qtrue;

	G_MP_DrawTargetBox(st, mpTargetColor);
}

/**
 * @brief Reports what a real hit did to the box
 * @param[in,out] self
 * @param[in] attacker
 * @param[in] damage
 * @param point - unused
 */
static void G_MP_TargetPain(gentity_t *self, gentity_t *attacker, int damage, vec3_t point)
{
	const char *msg;
	int        owner = self->r.singleClient;

	self->health = MP_TARGET_HEALTH;

	if (owner < 0 || owner >= MAX_CLIENTS || mpClients[owner].target != self)
	{
		return;
	}

	msg = va("cpm \"^5missiletarget^7: took %s%i ^7dmg\"", G_MP_DamageColor(damage), damage);

	trap_SendServerCommand(owner, msg);

	if (attacker && attacker->client && attacker->s.number != owner)
	{
		trap_SendServerCommand(attacker->s.number, msg);
	}
}

/**
 * @brief Can't happen with MP_TARGET_HEALTH, but never let the box go away on its own
 * @param[in,out] self
 * @param inflictor - unused
 * @param attacker - unused
 * @param damage - unused
 * @param mod - unused
 */
static void G_MP_TargetDie(gentity_t *self, gentity_t *inflictor, gentity_t *attacker, int damage, meansOfDeath_t mod)
{
	self->health     = MP_TARGET_HEALTH;
	self->takedamage = qtrue;
}

/**
 * @brief G_MP_PlaceTarget
 * @param[in] ent
 * @param[in,out] st
 * @param[in] origin
 * @param[in] mins
 * @param[in] maxs
 * @return qfalse if there aren't enough free entities to spare
 */
static qboolean G_MP_PlaceTarget(gentity_t *ent, mpClient_t *st, vec3_t origin, const vec3_t mins, const vec3_t maxs)
{
	int       clientNum = ent->s.number;
	int       needed    = 0;
	int       i;
	gentity_t *target;

	if (!G_MP_Owns(st->target, mpTargetClassname, clientNum))
	{
		needed++;
	}

	for (i = 0; i < MP_TARGET_EDGES; i++)
	{
		if (!G_MP_Owns(st->targetEdges[i], mpBeamClassname, clientNum))
		{
			needed++;
		}
	}

	if (needed && G_EntitiesFree() - needed < MP_ENTITY_RESERVE)
	{
		return qfalse;
	}

	if (!G_MP_Owns(st->target, mpTargetClassname, clientNum))
	{
		target = G_Spawn();

		// a player sized stand-in: G_AdjustedDamageVec() measures it centre to centre like a player
		// and CanDamage() probes its own box corners (see G_MissilePreviewIsTarget())
		target->classname      = mpTargetClassname;
		target->s.eType        = ET_GENERAL;
		target->r.svFlags      = SVF_NOCLIENT;
		target->r.singleClient = clientNum;
		target->takedamage     = qtrue;
		target->health         = MP_TARGET_HEALTH;
		target->pain           = G_MP_TargetPain;
		target->die            = G_MP_TargetDie;

		st->target = target;
	}

	for (i = 0; i < MP_TARGET_EDGES; i++)
	{
		if (!G_MP_Owns(st->targetEdges[i], mpBeamClassname, clientNum))
		{
			st->targetEdges[i] = G_MP_SpawnBeam(clientNum);
		}
	}

	target = st->target;

	VectorCopy(mins, target->r.mins);
	VectorCopy(maxs, target->r.maxs);
	G_SetOrigin(target, origin);
	target->r.contents = 0;
	trap_LinkEntity(target);

	st->targetArmed = qfalse;

	G_MP_DrawTargetBox(st, mpTargetUnarmedColor);
	G_MP_UpdateTargetArming(ent, st);

	return qtrue;
}

/**
 * @brief Places a player sized box to aim at, or removes it
 *   missiletarget                                 - place where you stand, or remove the one that is up
 *   missiletarget <x> <y> <z>                     - place a player box there
 *   missiletarget <x> <y> <z> <mins...> <maxs...> - place a box of that size there
 * @param[in] ent
 * @param dwCommand - unused
 * @param value - unused
 */
void Cmd_MissileTarget_f(gentity_t *ent, unsigned int dwCommand, int value)
{
	int        clientNum = ent - g_entities;
	mpClient_t *st       = &mpClients[clientNum];
	int        argc      = trap_Argc();
	vec3_t     origin, mins, maxs;
	char       arg[MAX_TOKEN_CHARS];
	int        i;

	if (!ent->client)
	{
		return;
	}

	if (!G_MP_Enabled())
	{
		trap_SendServerCommand(clientNum, "print \"missiletarget: needs sv_cheats 1, or g_missilePreview 2 on the server.\n\"");
		return;
	}

	if (argc != 1 && argc != 4 && argc != 10)
	{
		trap_SendServerCommand(clientNum, "print \"usage: missiletarget [x y z [minx miny minz maxx maxy maxz]]\n\"");
		return;
	}

	// only the bare form toggles, so a saved command can be pasted without checking what is up
	if (argc == 1 && G_MP_Owns(st->target, mpTargetClassname, clientNum))
	{
		G_MP_ClearTarget(clientNum, st);
		trap_SendServerCommand(clientNum, "print \"missiletarget: cleared\n\"");
		return;
	}

	if (argc == 1)
	{
		VectorCopy(ent->client->ps.origin, origin);
	}
	else
	{
		for (i = 0; i < 3; i++)
		{
			trap_Argv(i + 1, arg, sizeof(arg));
			origin[i] = Q_atof(arg);
		}
	}

	if (argc == 10)
	{
		for (i = 0; i < 3; i++)
		{
			trap_Argv(i + 4, arg, sizeof(arg));
			mins[i] = Q_atof(arg);
			trap_Argv(i + 7, arg, sizeof(arg));
			maxs[i] = Q_atof(arg);
		}

		if (mins[0] >= maxs[0] || mins[1] >= maxs[1] || mins[2] >= maxs[2])
		{
			trap_SendServerCommand(clientNum, "print \"missiletarget: mins must be less than maxs on every axis\n\"");
			return;
		}
	}
	else
	{
		// the standing box: radius damage always treats a player as standing, whatever the pose
		VectorCopy(playerMins, mins);
		VectorCopy(playerMaxs, maxs);
	}

	if (!G_MP_PlaceTarget(ent, st, origin, mins, maxs))
	{
		trap_SendServerCommand(clientNum, "print \"missiletarget: not enough free entities on this map\n\"");
		return;
	}

	trap_SendServerCommand(clientNum, va("print \"missiletarget: placed at (%.0f %.0f %.0f), box %.0fx%.0fx%.0f - run bare to clear\n\"",
	                                     origin[0], origin[1], origin[2],
	                                     maxs[0] - mins[0], maxs[1] - mins[1], maxs[2] - mins[2]));
	trap_SendServerCommand(clientNum, va("print \"to restore it later: missiletarget %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n\"",
	                                     origin[0], origin[1], origin[2], mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2]));

	if (!st->targetArmed)
	{
		trap_SendServerCommand(clientNum, "print \"missiletarget: step out of the box, rounds pass through it until you do\n\"");
	}
}

/*
======================================================================
FRAME
======================================================================
*/

/**
 * @brief G_MissilePreviewClientDisconnect
 * @param[in] clientNum
 */
void G_MissilePreviewClientDisconnect(int clientNum)
{
	if (clientNum < 0 || clientNum >= MAX_CLIENTS)
	{
		return;
	}

	G_MP_ClearArc(clientNum, &mpClients[clientNum]);
	G_MP_ClearTarget(clientNum, &mpClients[clientNum]);
	Com_Memset(&mpClients[clientNum], 0, sizeof(mpClients[clientNum]));
}

/**
 * @brief Switches the part of the arc the missile camera sees between beams and rails
 * @param[in] clientNum
 * @param[in,out] st
 * @param[in] on
 */
static void G_MP_SetCamRails(int clientNum, mpClient_t *st, qboolean on)
{
	if (on == st->camRails)
	{
		return;
	}

	if (on)
	{
		st->railGeneration++;
		st->railsDirty   = qtrue;
		st->boxRailDirty = qtrue;
		st->lastRailTime = level.time - MP_RAIL_REFRESH_MSEC;
	}
	else
	{
		G_MP_CollapseCamRails(clientNum, st);
	}

	st->camRails = on;

	G_MP_ApplyArc(clientNum, st);
}

/**
 * @brief G_MP_ClientFrame
 * @param[in] ent
 * @param[in,out] st
 */
static void G_MP_ClientFrame(gentity_t *ent, mpClient_t *st)
{
	int       clientNum = ent - g_entities;
	gclient_t *client   = ent->client;
	qboolean  held, camVisible;

	if (!ent->inuse || !client || client->pers.connected != CON_CONNECTED || (ent->r.svFlags & SVF_BOT) || !G_MP_Enabled())
	{
		G_MP_ClearArc(clientNum, st);
		G_MP_ClearTarget(clientNum, st);
		return;
	}

	if (st->target)
	{
		if (G_MP_Owns(st->target, mpTargetClassname, clientNum))
		{
			G_MP_UpdateTargetArming(ent, st);
		}
		else
		{
			G_MP_ClearTarget(clientNum, st);
		}
	}

	if (level.intermissiontime || client->sess.sessionTeam == TEAM_SPECTATOR)
	{
		G_MP_ClearArc(clientNum, st);
		return;
	}

	// the last arc stays up after +activate is released, through weapon switches, death and respawn,
	// until +activate with a previewable weapon replaces it; nothing flies while paused, so it stays then too
	if ((client->ps.pm_flags & PMF_LIMBO) || client->ps.pm_type == PM_DEAD || ent->health <= 0 || level.match_pause != PAUSE_NONE)
	{
		G_MP_SetCameraVisible(clientNum, st, qfalse);
		G_MP_SetCamRails(clientNum, st, qfalse);
		return;
	}

	held = ((client->buttons & BUTTON_ACTIVATE) && G_MP_IsPreviewable((weapon_t)client->ps.weapon)) ? qtrue : qfalse;

	if (held && level.time - st->lastComputeTime >= MP_RECOMPUTE_MSEC && G_MP_EnsureEntities(clientNum, st))
	{
		mpResult_t res;
		int        camPoint;

		st->lastComputeTime = level.time;

		G_MP_Simulate(ent, &res);
		camPoint = G_MP_PlaceCamera(ent, st, &res);
		G_MP_BuildArc(st, &res, camPoint);
		G_MP_ApplyArc(clientNum, st);
		G_MP_UpdateReadout(ent, st, &res);
	}

	// stock clients ignore the camera without sv_cheats, no need to send it then
	camVisible = (held && st->numArcVerts && g_cheats.integer && !G_MP_HasMissileInFlight(clientNum, st->camera)) ? qtrue : qfalse;

	G_MP_SetCameraVisible(clientNum, st, camVisible);
	G_MP_SetCamRails(clientNum, st, camVisible);

	if (st->camRails)
	{
		G_MP_UpdateCamRails(clientNum, st);
	}

	G_MP_FlushReadout(clientNum, st);
}

/**
 * @brief Runs after all entities and ClientEndFrame(), so the world and the player states are final
 */
void G_MissilePreviewRunFrame(void)
{
	int i;

	for (i = 0; i < level.maxclients; i++)
	{
		G_MP_ClientFrame(&g_entities[i], &mpClients[i]);
	}
}
