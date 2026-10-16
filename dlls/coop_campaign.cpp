#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"
#include "coop_campaign.h"
#include "coop_levels.h"
#ifdef HLCOOP_MAPCHECK
#include "coop_transition_check.h"
#endif

#define COOP_MAX_PLAYERS 32
#define COOP_MAX_WEAPONS 32
#define COOP_MAX_REVERSE_VOLUMES 32
#define COOP_NAME_LENGTH 64
#define COOP_TARGET_LENGTH 128
#define COOP_PLAYER_SAVE_MAGIC 0x32504C43u
#define COOP_PLAYER_SAVE_VERSION 1u

struct CoopWeaponState
{
	char name[COOP_NAME_LENGTH];
	int clip;
};

struct CoopAmmoState
{
	char name[COOP_NAME_LENGTH];
	int count;
};

struct CoopPlayerState
{
	BOOL valid;
	BOOL pending;
	BOOL inventory;
	BOOL personalOrigin;
	BOOL duck;
	int slot;
	int userID;
	char authID[COOP_NAME_LENGTH];
	char name[COOP_NAME_LENGTH];
	Vector origin;
	Vector angles;
	Vector velocity;
	float health;
	float maxHealth;
	float armor;
	int suit;
	int longJump;
	int flashBattery;
	int items[MAX_ITEMS];
	char activeWeapon[COOP_NAME_LENGTH];
	int weaponCount;
	CoopWeaponState weapons[COOP_MAX_WEAPONS];
	int ammoCount;
	CoopAmmoState ammo[MAX_AMMO_SLOTS];
};

struct CoopReverseVolume
{
	Vector mins;
	Vector maxs;
};

struct CoopCampaignState
{
	BOOL awaitingMap;
	BOOL transitionQueued;
	BOOL persistent;
	BOOL checkpoint;
	BOOL checkpointDuck;
	char map[COOP_NAME_LENGTH];
	char nextMap[COOP_NAME_LENGTH];
	char sourceMap[COOP_NAME_LENGTH];
	char landmark[COOP_NAME_LENGTH];
	char changeTarget[COOP_TARGET_LENGTH];
	Vector sourceLandmark;
	Vector leaderOrigin;
	Vector leaderAngles;
	BOOL leaderDuck;
	int leaderSlot;
	Vector checkpointOrigin;
	Vector checkpointAngles;
	float changeDelay;
	float fireTargetTime;
	float queuedTime;
	float arrivalTime;
	float nextCheckpointTime;
	int reverseVolumeCount;
	CoopReverseVolume reverseVolumes[COOP_MAX_REVERSE_VOLUMES];
	BOOL waitForExit[COOP_MAX_PLAYERS + 1];
	float spawnTime[COOP_MAX_PLAYERS + 1];
	CoopPlayerState players[COOP_MAX_PLAYERS];
};

// Между картами хранятся только значения, указатели движка не переживают загрузку
static CoopCampaignState g_CoopCampaign;
static CoopCampaignState g_CoopPreviousCampaign;
static BOOL g_CoopPreviousValid;
static CoopCampaignState g_CoopStagedCampaign;
static BOOL g_CoopPlayersStaged;
static CoopPlayerState g_CoopRespawn[COOP_MAX_PLAYERS];
static char g_CoopLoadedMap[COOP_NAME_LENGTH];

enum CoopStartProfile
{
	COOP_START_NONE,
	COOP_START_CIVILIAN,
	COOP_START_SUIT,
	COOP_START_CROWBAR,
	COOP_START_SECURITY,
	COOP_START_SHOTGUN,
	COOP_START_ASSAULT,
	COOP_START_MAGNUM,
	COOP_START_RECOVERED,
	COOP_START_LAB,
	COOP_START_MILITARY,
	COOP_START_XEN
};

static CoopStartProfile g_CoopStartProfile;
static char g_CoopStartMap[COOP_NAME_LENGTH];
static int g_CoopStartUsers[COOP_MAX_PLAYERS];

static void ClearCoopDirectStart( void )
{
	g_CoopStartProfile = COOP_START_NONE;
	g_CoopStartMap[0] = 0;
	memset( g_CoopStartUsers, 0, sizeof(g_CoopStartUsers) );
}

static void CopyCoopString( char *destination, size_t capacity, const char *source )
{
	if ( !source )
		source = "";
	strncpy( destination, source, capacity - 1 );
	destination[capacity - 1] = 0;
}

static BOOL CoopFinite( float value, float limit )
{
	return value >= -limit && value <= limit;
}

static BOOL CoopFiniteVector( const Vector &value )
{
	return CoopFinite( value.x, 65536 ) && CoopFinite( value.y, 65536 ) && CoopFinite( value.z, 65536 );
}

static BOOL CoopIdentifier( const char *name, BOOL allowEmpty )
{
	if ( !name || !*name )
		return allowEmpty;
	for ( const unsigned char *p = (const unsigned char *)name; *p; ++p )
	{
		if ( !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9') || *p == '_' || *p == '-') )
			return FALSE;
	}
	return TRUE;
}

static BOOL CoopStrongAuth( const char *authID )
{
	return authID && *authID && stricmp( authID, "BOT" ) &&
		!strstr( authID, "PENDING" ) && !strstr( authID, "LAN" ) &&
		(strncmp( authID, "STEAM_", 6 ) == 0 || strncmp( authID, "VALVE_", 6 ) == 0 || authID[0] == '[');
}

static CBasePlayer *CoopPlayerByIndex( int slot )
{
	CBaseEntity *pEntity = UTIL_PlayerByIndex( slot );
	if ( !pEntity || !pEntity->IsPlayer() || pEntity->IsDormant() ||
		GETPLAYERUSERID( pEntity->edict() ) <= 0 )
		return NULL;
	return (CBasePlayer *)pEntity;
}

static BOOL CoopActivePlayer( CBasePlayer *pPlayer )
{
	return pPlayer && pPlayer->IsAlive() && !pPlayer->IsDormant() &&
		!pPlayer->IsObserver() && !pPlayer->m_fCoopSpawnPending &&
		pPlayer->pev->solid == SOLID_SLIDEBOX;
}

static BOOL CoopInTransition( CBasePlayer *pPlayer, const char *landmark )
{
	CBaseEntity *pVolume = NULL;
	while ( (pVolume = UTIL_FindEntityByTargetname( pVolume, landmark )) != NULL )
	{
		if ( !FClassnameIs( pVolume->pev, "trigger_transition" ) )
			continue;
		if ( pVolume->Intersects( pPlayer ) )
			return TRUE;
	}
	// Без явной области отставших собираем у инициатора перехода
	return FALSE;
}

static void CaptureCoopPlayer( CoopPlayerState &saved, CBasePlayer *pPlayer, BOOL dying = FALSE )
{
	memset( &saved, 0, sizeof(saved) );
	saved.valid = saved.pending = TRUE;
	saved.slot = pPlayer->entindex();
	saved.userID = GETPLAYERUSERID( pPlayer->edict() );
	CopyCoopString( saved.authID, sizeof(saved.authID), GETPLAYERAUTHID( pPlayer->edict() ) );
	CopyCoopString( saved.name, sizeof(saved.name), STRING(pPlayer->pev->netname) );
	saved.inventory = CoopActivePlayer( pPlayer ) || (dying && !pPlayer->IsObserver() && !pPlayer->m_fCoopSpawnPending);
	saved.personalOrigin = saved.inventory;
	saved.origin = pPlayer->pev->origin;
	saved.angles = pPlayer->pev->v_angle;
	saved.velocity = dying ? g_vecZero : pPlayer->pev->velocity;
	saved.duck = FBitSet( pPlayer->pev->flags, FL_DUCKING ) ? TRUE : FALSE;
	saved.health = saved.inventory && !dying ? max( pPlayer->pev->health, 1 ) : 100;
	saved.maxHealth = max( pPlayer->pev->max_health, saved.health );
	saved.armor = saved.inventory ? pPlayer->pev->armorvalue : 0;
	saved.suit = (pPlayer->pev->weapons & (1 << WEAPON_SUIT)) ? 1 : 0;
	saved.longJump = pPlayer->m_fLongJump ? 1 : 0;
	saved.flashBattery = pPlayer->m_iFlashBattery;
	if ( !saved.inventory )
		return;
	for ( int i = 0; i < MAX_ITEMS; ++i )
		saved.items[i] = pPlayer->m_rgItems[i];
	if ( pPlayer->m_pActiveItem )
		CopyCoopString( saved.activeWeapon, sizeof(saved.activeWeapon), STRING(pPlayer->m_pActiveItem->pev->classname) );
	for ( int i = 0; i < MAX_ITEM_TYPES; ++i )
	{
		for ( CBasePlayerItem *pItem = pPlayer->m_rgpPlayerItems[i]; pItem && saved.weaponCount < COOP_MAX_WEAPONS; pItem = pItem->m_pNext )
		{
			if ( !pItem->GetWeaponPtr() )
				continue;
			CoopWeaponState &weapon = saved.weapons[saved.weaponCount++];
			CopyCoopString( weapon.name, sizeof(weapon.name), STRING(pItem->pev->classname) );
			weapon.clip = ((CBasePlayerWeapon *)pItem)->m_iClip;
		}
	}
	for ( int i = 1; i < MAX_AMMO_SLOTS; ++i )
	{
		const char *name = CBasePlayerItem::AmmoInfoArray[i].pszName;
		if ( !name || !*name )
			continue;
		CoopAmmoState &ammo = saved.ammo[saved.ammoCount++];
		CopyCoopString( ammo.name, sizeof(ammo.name), name );
		ammo.count = pPlayer->m_rgAmmo[i];
	}
}

static CoopPlayerState *FindCoopPlayerState( CBasePlayer *pPlayer )
{
	if ( !pPlayer || g_CoopCampaign.awaitingMap || !g_CoopCampaign.map[0] || stricmp( STRING(gpGlobals->mapname), g_CoopCampaign.map ) )
		return NULL;
	const char *authID = GETPLAYERAUTHID( pPlayer->edict() );
	const char *name = STRING(pPlayer->pev->netname);
	CoopPlayerState *pMatch = NULL;
	BOOL ambiguous = FALSE;
	for ( int i = 0; i < COOP_MAX_PLAYERS; ++i )
	{
		CoopPlayerState &saved = g_CoopCampaign.players[i];
		if ( !saved.valid || !saved.pending )
			continue;
		BOOL match;
		if ( g_CoopCampaign.persistent )
		{
			if ( CoopStrongAuth( saved.authID ) )
				match = authID && !strcmp( saved.authID, authID );
			else
			{
				match = *saved.name && name && !strcmp( saved.name, name );
				if ( match && saved.slot == pPlayer->entindex() )
					return &saved;
			}
		}
		else
		{
			match = saved.slot == pPlayer->entindex() && saved.userID == GETPLAYERUSERID( pPlayer->edict() ) &&
				authID && !strcmp( saved.authID, authID );
		}
		if ( match )
		{
			// Не выдаём чужой снимок при совпавших именах без Steam ID
			if ( pMatch )
				ambiguous = TRUE;
			pMatch = &saved;
		}
	}
	return ambiguous ? NULL : pMatch;
}

static void CaptureAvailableCoopPlayer( CoopPlayerState &saved, CBasePlayer *pPlayer )
{
	CaptureCoopPlayer( saved, pPlayer );
	if ( saved.inventory )
		return;
	CoopPlayerState *pPending = FindCoopPlayerState( pPlayer );
	if ( !pPending && pPlayer->entindex() >= 1 && pPlayer->entindex() <= COOP_MAX_PLAYERS )
	{
		CoopPlayerState &respawn = g_CoopRespawn[pPlayer->entindex() - 1];
		const char *authID = GETPLAYERAUTHID( pPlayer->edict() );
		if ( respawn.valid && respawn.pending && respawn.userID == saved.userID &&
			authID && !strcmp( authID, respawn.authID ) )
			pPending = &respawn;
	}
	if ( pPending )
	{
		saved = *pPending;
		saved.personalOrigin = FALSE;
		saved.velocity = g_vecZero;
		if ( g_CoopCampaign.checkpoint )
		{
			saved.origin = g_CoopCampaign.checkpointOrigin;
			saved.angles = g_CoopCampaign.checkpointAngles;
			saved.duck = g_CoopCampaign.checkpointDuck;
		}
	}
}

static BOOL CoopHullClear( CBasePlayer *pPlayer, const Vector &position, BOOL duck, BOOL geometryOnly = FALSE )
{
	TraceResult trace;
	UTIL_TraceHull( position, position, geometryOnly ? ignore_monsters : dont_ignore_monsters,
		duck ? head_hull : human_hull, pPlayer->edict(), &trace );
	return !trace.fStartSolid && !trace.fAllSolid;
}

static BOOL FindCoopFloorAnchor( CBasePlayer *pPlayer, const Vector &base, BOOL duck, Vector &result )
{
	static const float rises[] = { 1, 2, 4, 8, 16, 24, 32, 36 };
	int baseContents = UTIL_PointContents( base );
	if ( baseContents == CONTENTS_SOLID || baseContents == CONTENTS_LAVA || baseContents == CONTENTS_SLIME )
		return FALSE;
	for ( int i = 0; i < ARRAYSIZE(rises); ++i )
	{
		TraceResult trace;
		Vector raised = base + Vector( 0, 0, rises[i] );
		UTIL_TraceHull( raised, base, ignore_monsters, duck ? head_hull : human_hull, pPlayer->edict(), &trace );
		if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction == 1.0f || trace.vecPlaneNormal.z < 0.7f )
			continue;
		// На границе карт ступни могут оказаться чуть ниже пола, поправка не меняет сторону стены
		Vector candidate = trace.vecEndPos + Vector( 0, 0, 1 );
		int contents = UTIL_PointContents( candidate );
		if ( contents == CONTENTS_SOLID || contents == CONTENTS_LAVA || contents == CONTENTS_SLIME ||
			!CoopHullClear( pPlayer, candidate, duck, TRUE ) )
			continue;
		if ( rises[i] > 8 )
		{
			// Большая поправка опирается на тот же пол под центром, не поднимает через потолок или стенку
			float halfHeight = duck ? 18 : 36;
			TraceResult floor;
			UTIL_TraceLine( base, base - Vector( 0, 0, halfHeight ), ignore_monsters, pPlayer->edict(), &floor );
			if ( floor.fStartSolid || floor.fAllSolid || floor.flFraction == 1.0f || floor.vecPlaneNormal.z < 0.7f )
				continue;
			float support = 16 * (fabs(floor.vecPlaneNormal.x) + fabs(floor.vecPlaneNormal.y)) +
				halfHeight * floor.vecPlaneNormal.z;
			if ( fabs( DotProduct(floor.vecPlaneNormal, candidate - floor.vecEndPos) - support ) > 2 )
				continue;
		}
		result = candidate;
		return TRUE;
	}
	return FALSE;
}

static BOOL FindCoopDoorEdgeCorrection( CBasePlayer *pPlayer, const Vector &base, BOOL duck, Vector &result )
{
	if ( !g_CoopCampaign.sourceMap[0] || !g_CoopCampaign.landmark[0] )
		return FALSE;
	static const char *doorClasses[] = { "func_door", "func_door_rotating" };
	float halfHeight = duck ? 18 : 36;
	float bestDistance = 19;
	BOOL found = FALSE;
	for ( int doorClass = 0; doorClass < ARRAYSIZE(doorClasses); ++doorClass )
	{
		CBaseEntity *pDoor = NULL;
		while ( (pDoor = UTIL_FindEntityByClassname( pDoor, doorClasses[doorClass] )) != NULL )
		{
			if ( !pDoor->pev->globalname || pDoor->pev->solid != SOLID_BSP )
				continue;
			const Vector &mins = pDoor->pev->absmin;
			const Vector &maxs = pDoor->pev->absmax;
			if ( base.x + 16 <= mins.x || base.x - 16 >= maxs.x ||
				base.y + 16 <= mins.y || base.y - 16 >= maxs.y ||
				base.z + halfHeight <= mins.z || base.z - halfHeight >= maxs.z )
				continue;
			for ( int axis = 0; axis < 2; ++axis )
			{
				if ( maxs[axis] - mins[axis] > maxs[1 - axis] - mins[1 - axis] )
					continue;
				Vector candidate = base;
				// Выступающий край корпуса отводим от полотна, центр остаётся на прежней стороне
				if ( base[axis] > maxs[axis] )
					candidate[axis] = maxs[axis] + 17;
				else if ( base[axis] < mins[axis] )
					candidate[axis] = mins[axis] - 17;
				else
					continue;
				float distance = fabs( candidate[axis] - base[axis] );
				if ( distance <= 0 || distance > 18 || distance >= bestDistance )
					continue;
				TraceResult trace;
				UTIL_TraceLine( base, candidate, ignore_monsters, pPlayer->edict(), &trace );
				if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction != 1.0f )
					continue;
				int contents = UTIL_PointContents( candidate );
				if ( contents == CONTENTS_SOLID || contents == CONTENTS_LAVA || contents == CONTENTS_SLIME ||
					!CoopHullClear( pPlayer, candidate, duck ) )
					continue;
				result = candidate;
				bestDistance = distance;
				found = TRUE;
			}
		}
	}
	return found;
}

static BOOL FindSafeCoopPosition( CBasePlayer *pPlayer, const Vector &base, BOOL preferredDuck, Vector &result, BOOL &duck )
{
	static const Vector offsets[] =
	{
		Vector( 0, 0, 0 ),
		Vector( 48, 0, 0 ), Vector( -48, 0, 0 ), Vector( 0, 48, 0 ), Vector( 0, -48, 0 ),
		Vector( 48, 48, 0 ), Vector( 48, -48, 0 ), Vector( -48, 48, 0 ), Vector( -48, -48, 0 ),
		Vector( 96, 0, 0 ), Vector( -96, 0, 0 ), Vector( 0, 96, 0 ), Vector( 0, -96, 0 ),
		Vector( 144, 0, 0 ), Vector( -144, 0, 0 ), Vector( 0, 144, 0 ), Vector( 0, -144, 0 )
	};
	if ( !CoopFiniteVector( base ) )
		return FALSE;
	for ( int crouch = 0; crouch < 2; ++crouch )
	{
		BOOL useDuck = crouch ? !preferredDuck : preferredDuck;
		// Центр стоящего корпуса выше центра присевшего при одинаковом положении ног
		Vector center = base + Vector( 0, 0, useDuck == preferredDuck ? 0 : (useDuck ? -18 : 18) );
		if ( !CoopHullClear( pPlayer, center, useDuck, TRUE ) )
		{
			// Общая высота пола нужна и для бокового места, если первую точку уже занял товарищ
			Vector anchor;
			if ( FindCoopFloorAnchor( pPlayer, center, useDuck, anchor ) )
				center = anchor;
		}
		for ( int i = 0; i < ARRAYSIZE(offsets); ++i )
		{
			Vector candidate = center + offsets[i];
			TraceResult trace;
			if ( i )
			{
				UTIL_TraceHull( center, candidate, ignore_monsters, useDuck ? head_hull : human_hull, pPlayer->edict(), &trace );
				if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction != 1.0f )
					continue;
				UTIL_TraceHull( candidate, candidate - Vector( 0, 0, 72 ), ignore_monsters, useDuck ? head_hull : human_hull, pPlayer->edict(), &trace );
				if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction == 1.0f || trace.vecPlaneNormal.z < 0.7f )
					continue;
				candidate = trace.vecEndPos + Vector( 0, 0, 1 );
			}
			if ( CoopHullClear( pPlayer, candidate, useDuck ) )
			{
				int contents = UTIL_PointContents( candidate );
				if ( contents == CONTENTS_LAVA || contents == CONTENTS_SLIME || contents == CONTENTS_SOLID )
					continue;
				result = candidate;
				duck = useDuck;
				return TRUE;
			}
			if ( !i && FindCoopDoorEdgeCorrection( pPlayer, center, useDuck, result ) )
			{
				duck = useDuck;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static BOOL CoopTouchesReverse( CBasePlayer *pPlayer )
{
	for ( int i = 0; i < g_CoopCampaign.reverseVolumeCount; ++i )
	{
		const CoopReverseVolume &volume = g_CoopCampaign.reverseVolumes[i];
		Vector mins = pPlayer->pev->origin + pPlayer->pev->mins;
		Vector maxs = pPlayer->pev->origin + pPlayer->pev->maxs;
		if ( mins.x <= volume.maxs.x && maxs.x >= volume.mins.x &&
			mins.y <= volume.maxs.y && maxs.y >= volume.mins.y &&
			mins.z <= volume.maxs.z && maxs.z >= volume.mins.z )
			return TRUE;
	}
	return FALSE;
}

static BOOL CoopCheckpointSafe( CBasePlayer *pPlayer )
{
	if ( !CoopActivePlayer( pPlayer ) || pPlayer->pev->waterlevel > 1 ||
		FBitSet( pPlayer->m_afPhysicsFlags, PFLAG_ONBARNACLE ) ||
		FBitSet( pPlayer->pev->flags, FL_FROZEN ) )
		return FALSE;
	TraceResult trace;
	UTIL_TraceHull( pPlayer->pev->origin, pPlayer->pev->origin - Vector( 0, 0, 8 ), ignore_monsters,
		FBitSet( pPlayer->pev->flags, FL_DUCKING ) ? head_hull : human_hull, pPlayer->edict(), &trace );
	if ( trace.fAllSolid || trace.flFraction == 1.0f || trace.vecPlaneNormal.z < 0.7f ||
		(trace.pHit && trace.pHit->v.movetype == MOVETYPE_PUSH) )
		return FALSE;
	CBaseEntity *pTrigger = NULL;
	while ( (pTrigger = UTIL_FindEntityByClassname( pTrigger, "trigger_changelevel" )) != NULL )
	{
		if ( pTrigger->Intersects( pPlayer ) )
			return FALSE;
	}
	pTrigger = NULL;
	while ( (pTrigger = UTIL_FindEntityByClassname( pTrigger, "trigger_hurt" )) != NULL )
	{
		if ( pTrigger->pev->solid == SOLID_TRIGGER && pTrigger->pev->dmg > 0 && pTrigger->Intersects( pPlayer ) )
			return FALSE;
	}
	return TRUE;
}

BOOL CoopCampaignCanTransition( CBasePlayer *pPlayer )
{
	if ( !CoopActivePlayer( pPlayer ) || g_CoopCampaign.transitionQueued || g_CoopCampaign.awaitingMap )
		return FALSE;
	int slot = pPlayer->entindex();
	if ( slot < 1 || slot > COOP_MAX_PLAYERS )
		return FALSE;
	// Медленно подключившийся игрок получает защиту от обратного перехода после появления
	if ( gpGlobals->time < max( g_CoopCampaign.arrivalTime, g_CoopCampaign.spawnTime[slot] ) + 1.5f )
	{
		if ( CoopTouchesReverse( pPlayer ) )
			g_CoopCampaign.waitForExit[slot] = TRUE;
		return FALSE;
	}
	if ( g_CoopCampaign.waitForExit[slot] )
	{
		if ( CoopTouchesReverse( pPlayer ) )
			return FALSE;
		g_CoopCampaign.waitForExit[slot] = FALSE;
	}
	return TRUE;
}

BOOL CoopCampaignBeginTransition( const char *nextMap, const char *landmark, const Vector &sourceLandmark, CBasePlayer *activator, const char *changeTarget, float delay )
{
	if ( !CoopCampaignCanTransition( activator ) || !CoopIdentifier( nextMap, FALSE ) ||
		strlen( nextMap ) >= COOP_NAME_LENGTH || !IS_MAP_VALID( (char *)nextMap ) ||
		!landmark || strlen( landmark ) >= COOP_NAME_LENGTH || !CoopFiniteVector( sourceLandmark ) ||
		!CoopFinite( delay, 3600 ) || (changeTarget && strlen( changeTarget ) >= COOP_TARGET_LENGTH) )
		return FALSE;
	g_CoopPreviousCampaign = g_CoopCampaign;
	g_CoopPreviousValid = TRUE;
	g_CoopCampaign.queuedTime = gpGlobals->time;
	CopyCoopString( g_CoopCampaign.sourceMap, sizeof(g_CoopCampaign.sourceMap), STRING(gpGlobals->mapname) );
	CopyCoopString( g_CoopCampaign.nextMap, sizeof(g_CoopCampaign.nextMap), nextMap );
	CopyCoopString( g_CoopCampaign.landmark, sizeof(g_CoopCampaign.landmark), landmark );
	CopyCoopString( g_CoopCampaign.changeTarget, sizeof(g_CoopCampaign.changeTarget), changeTarget );
	g_CoopCampaign.changeDelay = max( delay, 0 );
	g_CoopCampaign.sourceLandmark = sourceLandmark;
	g_CoopCampaign.leaderOrigin = activator->pev->origin;
	g_CoopCampaign.leaderAngles = activator->pev->v_angle;
	g_CoopCampaign.leaderDuck = FBitSet( activator->pev->flags, FL_DUCKING ) ? TRUE : FALSE;
	g_CoopCampaign.leaderSlot = activator->entindex();
	CoopPlayerState captured[COOP_MAX_PLAYERS];
	for ( int i = 0; i < COOP_MAX_PLAYERS; ++i )
	{
		CBasePlayer *pPlayer = CoopPlayerByIndex( i + 1 );
		CoopPlayerState &saved = captured[i];
		if ( !pPlayer )
		{
			memset( &saved, 0, sizeof(saved) );
			continue;
		}
		CaptureAvailableCoopPlayer( saved, pPlayer );
		saved.personalOrigin = CoopActivePlayer( pPlayer ) &&
			(pPlayer == activator || CoopInTransition( pPlayer, landmark ));
		if ( !saved.personalOrigin )
		{
			saved.origin = g_CoopCampaign.leaderOrigin;
			saved.angles = g_CoopCampaign.leaderAngles;
			saved.duck = g_CoopCampaign.leaderDuck;
			saved.velocity = g_vecZero;
		}
	}
	memcpy( g_CoopCampaign.players, captured, sizeof(captured) );
	g_CoopCampaign.transitionQueued = g_CoopCampaign.awaitingMap = TRUE;
	g_CoopCampaign.persistent = FALSE;
	return TRUE;
}

void CoopCampaignCancelTransition( void )
{
	if ( g_CoopPreviousValid && g_CoopCampaign.transitionQueued )
		g_CoopCampaign = g_CoopPreviousCampaign;
	g_CoopPreviousValid = FALSE;
}

void CoopCampaignServerActivate( void )
{
	ClearCoopDirectStart();
	memset( g_CoopRespawn, 0, sizeof(g_CoopRespawn) );
	g_CoopPreviousValid = FALSE;
	const char *map = STRING(gpGlobals->mapname);
	BOOL expected = g_CoopCampaign.awaitingMap && !stricmp( map, g_CoopCampaign.nextMap );
	if ( !expected )
	{
		CoopCampaignReset();
		CopyCoopString( g_CoopCampaign.map, sizeof(g_CoopCampaign.map), map );
		return;
	}
	g_CoopCampaign.transitionQueued = g_CoopCampaign.awaitingMap = FALSE;
	g_CoopCampaign.reverseVolumeCount = 0;
	g_CoopCampaign.arrivalTime = gpGlobals->time;
	g_CoopCampaign.nextCheckpointTime = gpGlobals->time + 3;
	memset( g_CoopCampaign.waitForExit, 0, sizeof(g_CoopCampaign.waitForExit) );
	memset( g_CoopCampaign.spawnTime, 0, sizeof(g_CoopCampaign.spawnTime) );
	CopyCoopString( g_CoopCampaign.map, sizeof(g_CoopCampaign.map), map );
	if ( !g_CoopCampaign.persistent )
	{
		CBaseEntity *pLandmark = NULL;
		CBaseEntity *pEntity = NULL;
		while ( (pEntity = UTIL_FindEntityByClassname( pEntity, "info_landmark" )) != NULL )
		{
			if ( !strcmp( STRING(pEntity->pev->targetname), g_CoopCampaign.landmark ) )
			{
				pLandmark = pEntity;
				break;
			}
		}
		if ( !pLandmark )
		{
			// У некоторых односторонних переходов одинаковые координаты и разные имена landmark
			pEntity = NULL;
			while ( (pEntity = UTIL_FindEntityByClassname( pEntity, "info_landmark" )) != NULL )
			{
				if ( (pEntity->pev->origin - g_CoopCampaign.sourceLandmark).Length() < 1 )
				{
					pLandmark = pEntity;
					break;
				}
			}
		}
		Vector offset = g_vecZero;
		if ( pLandmark )
		{
			offset = pLandmark->pev->origin - g_CoopCampaign.sourceLandmark;
			g_CoopCampaign.checkpointOrigin = g_CoopCampaign.leaderOrigin + offset;
			g_CoopCampaign.checkpointAngles = g_CoopCampaign.leaderAngles;
			g_CoopCampaign.checkpointDuck = g_CoopCampaign.leaderDuck;
			g_CoopCampaign.checkpoint = TRUE;
		}
		else
		{
			g_CoopCampaign.checkpoint = FALSE;
			ALERT( at_console, "Half-Life Co-op: missing arrival landmark %s on %s, using map spawn\n", g_CoopCampaign.landmark, map );
		}
		for ( int i = 0; i < COOP_MAX_PLAYERS; ++i )
		{
			CoopPlayerState &saved = g_CoopCampaign.players[i];
			if ( !saved.valid )
				continue;
			saved.origin = saved.origin + offset;
			if ( !pLandmark )
				saved.personalOrigin = FALSE;
		}
	}
	g_CoopCampaign.fireTargetTime = gpGlobals->time + max( g_CoopCampaign.changeDelay, 0.1f );
}

static void CreateCoopStartActor( const char *classname, const char *targetname, const Vector &origin,
	const Vector &angles, int spawnflags, const char *firstNode )
{
	// Существующего актёра сохраняем даже после смерти или начала скриптовой сцены
	if ( UTIL_FindEntityByClassname( NULL, classname ) || UTIL_FindEntityByTargetname( NULL, targetname ) )
		return;
	edict_t *actor = CREATE_NAMED_ENTITY( ALLOC_STRING(classname) );
	if ( FNullEnt(actor) )
	{
		ALERT( at_error, "Half-Life Co-op: cannot create direct-start actor %s\n", targetname );
		return;
	}
	if ( !actor->pvPrivateData )
	{
		REMOVE_ENTITY( actor );
		ALERT( at_error, "Half-Life Co-op: missing direct-start actor class %s\n", classname );
		return;
	}
	actor->v.targetname = ALLOC_STRING( targetname );
	actor->v.globalname = 0;
	actor->v.spawnflags = spawnflags;
	actor->v.netname = firstNode && *firstNode ? ALLOC_STRING(firstNode) : 0;
	actor->v.angles = angles;
	UTIL_SetOrigin( &actor->v, origin );
	int result = DispatchSpawn( actor );
	if ( result < 0 || actor->free || !actor->pvPrivateData || FBitSet(actor->v.flags, FL_KILLME) )
	{
		if ( !actor->free )
			REMOVE_ENTITY( actor );
		ALERT( at_error, "Half-Life Co-op: cannot spawn direct-start actor %s\n", targetname );
		return;
	}
	CBaseEntity *entity = CBaseEntity::Instance( actor );
	if ( !entity )
	{
		REMOVE_ENTITY( actor );
		return;
	}
	// Штатный цикл Activate уже завершён до подготовки выбранной карты
	entity->Activate();
	ALERT( at_console, "Half-Life Co-op: direct-start actor %s initialized at %.0f %.0f %.0f\n",
		targetname, origin.x, origin.y, origin.z );
}

void CoopCampaignPrepareDirectStart( const char *map )
{
	ClearCoopDirectStart();
	if ( !map || stricmp( map, STRING(gpGlobals->mapname) ) || !g_pGameRules || !g_pGameRules->IsCoOp() )
		return;
	// Порядок профилей совпадает с каталогом глав оригинальной кампании
	static const CoopStartProfile profiles[] =
	{
		COOP_START_CIVILIAN,	// Прибытие в Чёрную Мезу
		COOP_START_CIVILIAN,	// Аномальные материалы
		COOP_START_SUIT,		// Непредвиденные последствия
		COOP_START_SECURITY,	// Офисный комплекс
		COOP_START_SHOTGUN,	// У нас враги
		COOP_START_ASSAULT,	// Ракетная шахта
		COOP_START_MAGNUM,	// Подача энергии
		COOP_START_MAGNUM,	// По рельсам
		COOP_START_MAGNUM,	// Задержание
		COOP_START_SUIT,		// Переработка отходов
		COOP_START_RECOVERED,	// Сомнительная этика
		COOP_START_LAB,		// Поверхностное натяжение
		COOP_START_MILITARY,	// Забудьте о Фримене
		COOP_START_MILITARY,	// Ядро Лямбды
		COOP_START_XEN,		// Зен
		COOP_START_XEN,		// Логово Гонарха
		COOP_START_XEN,		// Нарушитель
		COOP_START_XEN,		// Нихилант
		COOP_START_SUIT		// Конец игры
	};
	int chapter = CoopLevelChapter( map );
	if ( chapter < 0 || chapter >= ARRAYSIZE(profiles) )
		return;
	g_CoopStartProfile = profiles[chapter];
	if ( chapter == 2 && stricmp( map, "c1a0c" ) )
		g_CoopStartProfile = !stricmp( map, "c1a1" ) ? COOP_START_CROWBAR : COOP_START_SECURITY;
	if ( chapter == 9 && stricmp( map, "c2a4" ) )
		g_CoopStartProfile = COOP_START_SECURITY;
	// После пленения и в финале оружие уже изъято сюжетным событием
	if ( !stricmp( map, "c2a3e" ) || !stricmp( map, "c2a4" ) || !stricmp( map, "c5a1" ) )
		g_CoopStartProfile = COOP_START_SUIT;
	CopyCoopString( g_CoopStartMap, sizeof(g_CoopStartMap), map );
	if ( !stricmp( map, "c0a0e" ) )
	{
		// Охранник обычно прибывает с предыдущей карты до запуска штатной сцены у поезда
		CreateCoopStartActor( "monster_barney", "barney1", Vector(-2017, -458, -221),
			g_vecZero, SF_MONSTER_PREDISASTER, NULL );
	}
	else if ( !stricmp( map, "c4a2a" ) || !stricmp( map, "c4a2b" ) )
	{
		const char *firstNode = !stricmp(map, "c4a2a") ? "goose5" : "goose8b";
		CBaseEntity *entry = UTIL_FindEntityByTargetname( NULL, firstNode );
		if ( entry && FClassnameIs(entry->pev, "info_bigmomma") )
		{
			// Босс продолжает штатный путь и сам запускает разрушения и финальную сцену
			CreateCoopStartActor( "monster_bigmomma", "big_momma", entry->pev->origin,
				entry->pev->angles, SF_MONSTER_HITMONSTERCLIP, firstNode );
		}
		else
			ALERT( at_error, "Half-Life Co-op: missing direct-start boss node %s\n", firstNode );
	}
	if ( !stricmp( map, "c1a2" ) )
	{
		// Прямой старт повторяет прибытие лифта, которое обычно запускает предыдущая карта
		CopyCoopString( g_CoopCampaign.changeTarget, sizeof(g_CoopCampaign.changeTarget), "elestartmm" );
		g_CoopCampaign.fireTargetTime = gpGlobals->time + 1.0f;
	}
	else if ( !stricmp( map, "c2a3e" ) )
	{
		// Прямой старт уже находится в компакторе и пропускает перенос из сцены пленения
		CopyCoopString( g_CoopCampaign.changeTarget, sizeof(g_CoopCampaign.changeTarget), "intro_box" );
		g_CoopCampaign.fireTargetTime = gpGlobals->time + 0.1f;
	}
}

void CoopCampaignRegisterReverseTransition( const char *nextMap, const char *landmark, const Vector &mins, const Vector &maxs )
{
	if ( !nextMap || !landmark )
		return;
	BOOL matches = !stricmp( nextMap, g_CoopCampaign.sourceMap ) && !strcmp( landmark, g_CoopCampaign.landmark );
	if ( !matches && !strcmp( g_CoopCampaign.map, "c1a0c" ) &&
		((!strcmp( g_CoopCampaign.sourceMap, "c1a0b" ) && !strcmp( nextMap, "c1a0e" )) ||
		 (!strcmp( g_CoopCampaign.sourceMap, "c1a0e" ) && !strcmp( nextMap, "c1a0b" ))) &&
		((!strcmp( g_CoopCampaign.landmark, "c1a0btoc" ) && !strcmp( landmark, "c1a0etoc" )) ||
		 (!strcmp( g_CoopCampaign.landmark, "c1a0etoc" ) && !strcmp( landmark, "c1a0btoc" ))) )
	{
		// После эксперимента обратный портал ведёт в изменённый вариант той же карты
		CBaseEntity *pEntry = NULL;
		CBaseEntity *pReverse = NULL;
		CBaseEntity *pEntity = NULL;
		while ( (pEntity = UTIL_FindEntityByClassname( pEntity, "info_landmark" )) != NULL )
		{
			if ( !strcmp( STRING(pEntity->pev->targetname), g_CoopCampaign.landmark ) )
				pEntry = pEntity;
			if ( !strcmp( STRING(pEntity->pev->targetname), landmark ) )
				pReverse = pEntity;
		}
		matches = pEntry && pReverse && (pEntry->pev->origin - pReverse->pev->origin).Length() < 1;
	}
	if ( !matches || stricmp( STRING(gpGlobals->mapname), g_CoopCampaign.map ) ||
		g_CoopCampaign.reverseVolumeCount >= COOP_MAX_REVERSE_VOLUMES ||
		!CoopFiniteVector( mins ) || !CoopFiniteVector( maxs ) )
		return;
	CoopReverseVolume &volume = g_CoopCampaign.reverseVolumes[g_CoopCampaign.reverseVolumeCount++];
	volume.mins = mins;
	volume.maxs = maxs;
	for ( int i = 1; i <= COOP_MAX_PLAYERS; ++i )
		g_CoopCampaign.waitForExit[i] = TRUE;
}

BOOL CoopCampaignHasArrival( void )
{
	return g_CoopCampaign.checkpoint && !g_CoopCampaign.awaitingMap &&
		!stricmp( STRING(gpGlobals->mapname), g_CoopCampaign.map );
}

BOOL CoopCampaignHasPersonalArrival( CBasePlayer *pPlayer )
{
	CoopPlayerState *pSaved = FindCoopPlayerState( pPlayer );
	return pSaved && pSaved->pending && pSaved->personalOrigin;
}

BOOL CoopCampaignFindSpawn( CBasePlayer *pPlayer, Vector &origin, Vector &angles, BOOL &duck )
{
	if ( !pPlayer || !CoopCampaignHasArrival() )
		return FALSE;
	CoopPlayerState *pSaved = FindCoopPlayerState( pPlayer );
	if ( pSaved && pSaved->personalOrigin &&
		FindSafeCoopPosition( pPlayer, pSaved->origin, pSaved->duck, origin, duck ) )
	{
		angles = pSaved->angles;
		return TRUE;
	}
	angles = g_CoopCampaign.checkpointAngles;
	return FindSafeCoopPosition( pPlayer, g_CoopCampaign.checkpointOrigin, g_CoopCampaign.checkpointDuck, origin, duck );
}

static CBasePlayerItem *FindCoopWeapon( CBasePlayer *pPlayer, const char *name )
{
	for ( int i = 0; i < MAX_ITEM_TYPES; ++i )
	{
		for ( CBasePlayerItem *pItem = pPlayer->m_rgpPlayerItems[i]; pItem; pItem = pItem->m_pNext )
		{
			if ( !strcmp( STRING(pItem->pev->classname), name ) )
				return pItem;
		}
	}
	return NULL;
}

static BOOL CoopKnownWeapon( const char *name )
{
	static const char *names[] =
	{
		"weapon_crowbar", "weapon_9mmhandgun", "weapon_glock", "weapon_357", "weapon_python",
		"weapon_9mmAR", "weapon_mp5", "weapon_shotgun", "weapon_crossbow", "weapon_rpg",
		"weapon_gauss", "weapon_egon", "weapon_hornetgun", "weapon_handgrenade", "weapon_satchel",
		"weapon_tripmine", "weapon_snark"
	};
	for ( int i = 0; i < ARRAYSIZE(names); ++i )
	{
		if ( !strcmp( names[i], name ) )
			return TRUE;
	}
	return FALSE;
}

static BOOL CoopKnownAmmo( const char *name )
{
	static const char *names[] =
	{
		"9mm", "357", "ARgrenades", "buckshot", "bolts", "rockets", "uranium",
		"Hornets", "Hand Grenade", "Satchel Charge", "Trip Mine", "Snarks"
	};
	for ( int i = 0; i < ARRAYSIZE(names); ++i )
	{
		if ( !strcmp( names[i], name ) )
			return TRUE;
	}
	return FALSE;
}

static BOOL BuildCoopStartKit( CBasePlayer *pPlayer, CoopPlayerState &saved )
{
	int slot = pPlayer->entindex();
	int userID = GETPLAYERUSERID( pPlayer->edict() );
	if ( g_CoopStartProfile == COOP_START_NONE || pPlayer->m_fCoopHasSpawned ||
		slot < 1 || slot > COOP_MAX_PLAYERS || userID <= 0 ||
		g_CoopStartUsers[slot - 1] == userID || stricmp( g_CoopStartMap, STRING(gpGlobals->mapname) ) )
		return FALSE;
	const int security = (1 << WEAPON_CROWBAR) | (1 << WEAPON_GLOCK);
	const int shotgun = security | (1 << WEAPON_SHOTGUN);
	const int assault = shotgun | (1 << WEAPON_MP5) | (1 << WEAPON_HANDGRENADE);
	const int magnum = assault | (1 << WEAPON_PYTHON);
	const int lab = magnum | (1 << WEAPON_CROSSBOW) | (1 << WEAPON_GAUSS);
	const int military = lab | (1 << WEAPON_RPG);
	int weapons = 0;
	switch ( g_CoopStartProfile )
	{
	case COOP_START_CROWBAR: weapons = 1 << WEAPON_CROWBAR; break;
	case COOP_START_SECURITY: weapons = security; break;
	case COOP_START_SHOTGUN: weapons = shotgun; break;
	case COOP_START_ASSAULT: weapons = assault; break;
	case COOP_START_MAGNUM: weapons = magnum; break;
	case COOP_START_RECOVERED: weapons = shotgun | (1 << WEAPON_MP5); break;
	case COOP_START_LAB: weapons = lab; break;
	case COOP_START_MILITARY: weapons = military; break;
	case COOP_START_XEN: weapons = military | (1 << WEAPON_EGON); break;
	}
	static const struct
	{
		int id;
		const char *name;
		int clip;
	} weaponTypes[] =
	{
		{ WEAPON_CROWBAR, "weapon_crowbar", WEAPON_NOCLIP },
		{ WEAPON_GLOCK, "weapon_9mmhandgun", GLOCK_MAX_CLIP },
		{ WEAPON_SHOTGUN, "weapon_shotgun", SHOTGUN_MAX_CLIP },
		{ WEAPON_MP5, "weapon_9mmAR", MP5_MAX_CLIP },
		{ WEAPON_PYTHON, "weapon_357", PYTHON_MAX_CLIP },
		{ WEAPON_CROSSBOW, "weapon_crossbow", CROSSBOW_MAX_CLIP },
		{ WEAPON_GAUSS, "weapon_gauss", GAUSS_MAX_CLIP },
		{ WEAPON_RPG, "weapon_rpg", RPG_MAX_CLIP },
		{ WEAPON_EGON, "weapon_egon", EGON_MAX_CLIP },
		{ WEAPON_HANDGRENADE, "weapon_handgrenade", HANDGRENADE_MAX_CLIP }
	};
	static const struct
	{
		int weapons;
		const char *name;
		int count;
	} ammoTypes[] =
	{
		{ (1 << WEAPON_GLOCK) | (1 << WEAPON_MP5), "9mm", 120 },
		{ 1 << WEAPON_SHOTGUN, "buckshot", 24 },
		{ 1 << WEAPON_MP5, "ARgrenades", 2 },
		{ 1 << WEAPON_PYTHON, "357", 12 },
		{ 1 << WEAPON_CROSSBOW, "bolts", 10 },
		{ (1 << WEAPON_GAUSS) | (1 << WEAPON_EGON), "uranium", 40 },
		{ 1 << WEAPON_RPG, "rockets", 3 },
		{ 1 << WEAPON_HANDGRENADE, "Hand Grenade", 3 }
	};
	// Только явный выбор уровня создаёт начальный набор вместо снимка игрока
	memset( &saved, 0, sizeof(saved) );
	saved.valid = saved.pending = saved.inventory = TRUE;
	saved.health = saved.maxHealth = 100;
	saved.armor = weapons ? 50 : 0;
	saved.suit = g_CoopStartProfile != COOP_START_CIVILIAN;
	saved.longJump = g_CoopStartProfile == COOP_START_XEN;
	saved.flashBattery = 100;
	saved.angles = pPlayer->pev->v_angle;
	for ( int i = 0; i < ARRAYSIZE(weaponTypes); ++i )
	{
		if ( !(weapons & (1 << weaponTypes[i].id)) )
			continue;
		CoopWeaponState &weapon = saved.weapons[saved.weaponCount++];
		CopyCoopString( weapon.name, sizeof(weapon.name), weaponTypes[i].name );
		weapon.clip = weaponTypes[i].clip;
	}
	for ( int i = 0; i < ARRAYSIZE(ammoTypes); ++i )
	{
		if ( !(weapons & ammoTypes[i].weapons) )
			continue;
		CoopAmmoState &ammo = saved.ammo[saved.ammoCount++];
		CopyCoopString( ammo.name, sizeof(ammo.name), ammoTypes[i].name );
		ammo.count = ammoTypes[i].count;
	}
	const char *activeWeapon = weapons & (1 << WEAPON_GAUSS) ? "weapon_gauss" :
		weapons & (1 << WEAPON_MP5) ? "weapon_9mmAR" :
		weapons & (1 << WEAPON_SHOTGUN) ? "weapon_shotgun" :
		weapons & (1 << WEAPON_GLOCK) ? "weapon_9mmhandgun" :
		weapons & (1 << WEAPON_CROWBAR) ? "weapon_crowbar" : "";
	CopyCoopString( saved.activeWeapon, sizeof(saved.activeWeapon), activeWeapon );
	g_CoopStartUsers[slot - 1] = userID;
	return TRUE;
}

BOOL CoopCampaignRestorePlayer( CBasePlayer *pPlayer )
{
	if ( !CoopActivePlayer( pPlayer ) || g_CoopCampaign.awaitingMap )
		return FALSE;
	CoopPlayerState shared;
	CoopPlayerState *pSaved = FindCoopPlayerState( pPlayer );
	if ( !pSaved && pPlayer->entindex() >= 1 && pPlayer->entindex() <= COOP_MAX_PLAYERS )
	{
		CoopPlayerState &respawn = g_CoopRespawn[pPlayer->entindex() - 1];
		const char *authID = GETPLAYERAUTHID( pPlayer->edict() );
		if ( respawn.valid && respawn.pending && respawn.userID == GETPLAYERUSERID( pPlayer->edict() ) &&
			authID && !strcmp( authID, respawn.authID ) )
			pSaved = &respawn;
	}
	if ( !pSaved && !pPlayer->m_fCoopHasSpawned )
	{
		CBasePlayer *pLeader = CoopPlayerByIndex( g_CoopCampaign.leaderSlot );
		if ( pLeader == pPlayer || !CoopActivePlayer( pLeader ) || !pLeader->m_fCoopHasSpawned )
			pLeader = NULL;
		for ( int i = 1; !pLeader && i <= gpGlobals->maxClients && i <= COOP_MAX_PLAYERS; ++i )
		{
			CBasePlayer *pCandidate = CoopPlayerByIndex( i );
			if ( pCandidate != pPlayer && CoopActivePlayer( pCandidate ) && pCandidate->m_fCoopHasSpawned )
				pLeader = pCandidate;
		}
		if ( !pLeader && BuildCoopStartKit( pPlayer, shared ) )
			pSaved = &shared;
		else if ( pLeader )
		{
			CaptureCoopPlayer( shared, pLeader );
			shared.health = 100;
			shared.maxHealth = max( shared.maxHealth, 100 );
			shared.angles = pPlayer->pev->v_angle;
			shared.velocity = g_vecZero;
			pSaved = &shared;
		}
	}
	if ( !pSaved )
		return FALSE;
	pSaved->pending = FALSE;
	if ( !pSaved->inventory )
		return FALSE;
	pPlayer->RemoveAllItems( TRUE );
	int autoSwitch = pPlayer->m_iAutoWepSwitch;
	pPlayer->m_iAutoWepSwitch = 0;
	for ( int i = 0; i < pSaved->weaponCount; ++i )
	{
		const CoopWeaponState &weapon = pSaved->weapons[i];
		if ( !CoopKnownWeapon( weapon.name ) )
			continue;
		pPlayer->GiveNamedItem( STRING(ALLOC_STRING( weapon.name )) );
		CBasePlayerItem *pItem = FindCoopWeapon( pPlayer, weapon.name );
		if ( pItem && pItem->GetWeaponPtr() )
		{
			CBasePlayerWeapon *pWeapon = (CBasePlayerWeapon *)pItem;
			pWeapon->m_iClip = pWeapon->iMaxClip() < 0 ? WEAPON_NOCLIP : min( max( weapon.clip, 0 ), pWeapon->iMaxClip() );
			pWeapon->m_iClientClip = -2;
			pWeapon->m_fInReload = pWeapon->m_fInSpecialReload = 0;
		}
	}
	pPlayer->m_iAutoWepSwitch = autoSwitch;
	// Выдача оружия добавляет патроны, поэтому точный боезапас восстанавливается последним
	for ( int i = 0; i < MAX_AMMO_SLOTS; ++i )
	{
		pPlayer->m_rgAmmo[i] = 0;
		pPlayer->m_rgAmmoLast[i] = -1;
	}
	for ( int i = 0; i < pSaved->ammoCount; ++i )
	{
		int index = CBasePlayer::GetAmmoIndex( pSaved->ammo[i].name );
		if ( index > 0 && index < MAX_AMMO_SLOTS )
			pPlayer->m_rgAmmo[index] = pSaved->ammo[i].count;
	}
	if ( pSaved->suit )
		pPlayer->pev->weapons |= (1 << WEAPON_SUIT);
	else
		pPlayer->pev->weapons &= ~(1 << WEAPON_SUIT);
	pPlayer->m_fLongJump = pSaved->longJump;
	g_engfuncs.pfnSetPhysicsKeyValue( pPlayer->edict(), "slj", pSaved->longJump ? "1" : "0" );
	pPlayer->pev->health = max( pSaved->health, 1 );
	pPlayer->pev->max_health = max( pSaved->maxHealth, pPlayer->pev->health );
	pPlayer->pev->armorvalue = pSaved->armor;
	pPlayer->m_iFlashBattery = pSaved->flashBattery;
	for ( int i = 0; i < MAX_ITEMS; ++i )
		pPlayer->m_rgItems[i] = pSaved->items[i];
	pPlayer->m_afPhysicsFlags &= ~PFLAG_ONBARNACLE;
	pPlayer->pev->movetype = MOVETYPE_WALK;
	pPlayer->pev->v_angle = pSaved->angles;
	pPlayer->pev->angles = Vector( -pSaved->angles.x, pSaved->angles.y, 0 );
	pPlayer->pev->fixangle = TRUE;
	pPlayer->pev->velocity = pSaved->velocity;
	CBasePlayerItem *pActive = FindCoopWeapon( pPlayer, pSaved->activeWeapon );
	if ( !pActive || !pPlayer->SwitchWeapon( pActive ) )
	{
		for ( int i = 0; i < MAX_ITEM_TYPES; ++i )
		{
			if ( pPlayer->m_rgpPlayerItems[i] && pPlayer->SwitchWeapon( pPlayer->m_rgpPlayerItems[i] ) )
				break;
		}
	}
	pPlayer->m_fKnownItem = FALSE;
	pPlayer->m_fInitHUD = TRUE;
	pPlayer->m_iClientHealth = pPlayer->m_iClientBattery = -1;
	pPlayer->m_pClientActiveItem = NULL;
	pPlayer->TabulateAmmo();
#ifdef HLCOOP_MAPCHECK
	CoopTransitionCheckPlayerRestored(pPlayer);
#endif
	return TRUE;
}

void CoopCampaignPlayerSpawned( CBasePlayer *pPlayer )
{
	if ( !CoopActivePlayer( pPlayer ) || g_CoopCampaign.awaitingMap )
		return;
	int slot = pPlayer->entindex();
	if ( slot >= 1 && slot <= COOP_MAX_PLAYERS )
	{
		g_CoopCampaign.spawnTime[slot] = gpGlobals->time;
		g_CoopCampaign.waitForExit[slot] = CoopTouchesReverse( pPlayer );
	}
	if ( !g_CoopCampaign.checkpoint && CoopCheckpointSafe( pPlayer ) )
	{
		g_CoopCampaign.checkpoint = TRUE;
		g_CoopCampaign.checkpointOrigin = pPlayer->pev->origin;
		g_CoopCampaign.checkpointAngles = pPlayer->pev->v_angle;
		g_CoopCampaign.checkpointDuck = FBitSet( pPlayer->pev->flags, FL_DUCKING ) ? TRUE : FALSE;
		g_CoopCampaign.leaderSlot = slot;
	}
}

void CoopCampaignPlayerDisconnected( int slot )
{
	if ( slot < 1 || slot > COOP_MAX_PLAYERS )
		return;
	g_CoopCampaign.waitForExit[slot] = FALSE;
	g_CoopCampaign.spawnTime[slot] = 0;
	memset( &g_CoopRespawn[slot - 1], 0, sizeof(g_CoopRespawn[slot - 1]) );
	// Запись HCS ожидает своего владельца и не привязана к текущему слоту подключения
	if ( g_CoopCampaign.persistent || g_CoopCampaign.awaitingMap )
		return;
	CBaseEntity *pEntity = UTIL_PlayerByIndex( slot );
	CoopPlayerState *pSaved = pEntity && pEntity->IsPlayer() ? FindCoopPlayerState( (CBasePlayer *)pEntity ) : NULL;
	if ( pSaved )
		pSaved->pending = FALSE;
}

void CoopCampaignPlayerDied( CBasePlayer *pPlayer )
{
	if ( !pPlayer || pPlayer->entindex() < 1 || pPlayer->entindex() > COOP_MAX_PLAYERS ||
		g_CoopCampaign.awaitingMap || g_CoopCampaign.transitionQueued )
		return;
	CoopPlayerState &saved = g_CoopRespawn[pPlayer->entindex() - 1];
	CaptureCoopPlayer( saved, pPlayer, TRUE );
	// Место гибели может быть опасным, возрождение использует общую контрольную точку
	saved.personalOrigin = FALSE;
}

static void StripCoopSnapshot( CoopPlayerState &saved )
{
	if ( !saved.valid || !saved.pending )
		return;
	saved.inventory = TRUE;
	saved.weaponCount = saved.ammoCount = 0;
	saved.activeWeapon[0] = 0;
}

void CoopCampaignPlayerStripped( CBasePlayer *pPlayer )
{
	if ( !pPlayer )
	{
		if ( g_CoopStartProfile != COOP_START_NONE && g_CoopStartProfile != COOP_START_CIVILIAN )
			g_CoopStartProfile = COOP_START_SUIT;
		// Раннее событие карты действует и на ещё не подключившихся участников
		for ( int i = 0; i < COOP_MAX_PLAYERS; ++i )
		{
			StripCoopSnapshot( g_CoopRespawn[i] );
			StripCoopSnapshot( g_CoopCampaign.players[i] );
		}
		return;
	}
	if ( pPlayer->entindex() < 1 || pPlayer->entindex() > COOP_MAX_PLAYERS )
		return;
	StripCoopSnapshot( g_CoopRespawn[pPlayer->entindex() - 1] );
	CoopPlayerState *pSaved = FindCoopPlayerState( pPlayer );
	if ( pSaved )
		StripCoopSnapshot( *pSaved );
	if ( g_CoopCampaign.awaitingMap && !g_CoopCampaign.persistent )
	{
		const char *authID = GETPLAYERAUTHID( pPlayer->edict() );
		for ( int i = 0; i < COOP_MAX_PLAYERS; ++i )
		{
			CoopPlayerState &saved = g_CoopCampaign.players[i];
			if ( saved.slot == pPlayer->entindex() && saved.userID == GETPLAYERUSERID( pPlayer->edict() ) &&
				authID && !strcmp( saved.authID, authID ) )
				StripCoopSnapshot( saved );
		}
	}
}

void CoopCampaignThink( void )
{
	if ( g_CoopCampaign.transitionQueued )
	{
		if ( gpGlobals->time > g_CoopCampaign.queuedTime + 10 &&
			!stricmp( STRING(gpGlobals->mapname), g_CoopCampaign.sourceMap ) )
		{
			CoopCampaignCancelTransition();
			ALERT( at_console, "Half-Life Co-op: level transition did not complete\n" );
		}
		return;
	}
	if ( g_CoopCampaign.awaitingMap )
		return;
	CBasePlayer *pLeader = CoopPlayerByIndex( g_CoopCampaign.leaderSlot );
	if ( !CoopActivePlayer( pLeader ) )
		pLeader = NULL;
	for ( int i = 1; i <= gpGlobals->maxClients && i <= COOP_MAX_PLAYERS; ++i )
	{
		CBasePlayer *pPlayer = CoopPlayerByIndex( i );
		if ( !CoopActivePlayer( pPlayer ) )
			continue;
		if ( !pLeader )
			pLeader = pPlayer;
		if ( gpGlobals->time < max( g_CoopCampaign.arrivalTime, g_CoopCampaign.spawnTime[i] ) + 1.5f )
		{
			if ( CoopTouchesReverse( pPlayer ) )
				g_CoopCampaign.waitForExit[i] = TRUE;
		}
		else if ( g_CoopCampaign.waitForExit[i] && !CoopTouchesReverse( pPlayer ) )
			g_CoopCampaign.waitForExit[i] = FALSE;
	}
	if ( *g_CoopCampaign.changeTarget && pLeader && gpGlobals->time >= g_CoopCampaign.fireTargetTime )
	{
		char target[COOP_TARGET_LENGTH];
		CopyCoopString( target, sizeof(target), g_CoopCampaign.changeTarget );
		g_CoopCampaign.changeTarget[0] = 0;
		FireTargets( target, pLeader, pLeader, USE_TOGGLE, 0 );
		return;
	}
	if ( gpGlobals->time < g_CoopCampaign.nextCheckpointTime )
		return;
	g_CoopCampaign.nextCheckpointTime = gpGlobals->time + 1;
	if ( pLeader && CoopCheckpointSafe( pLeader ) )
	{
		g_CoopCampaign.checkpoint = TRUE;
		g_CoopCampaign.checkpointOrigin = pLeader->pev->origin;
		g_CoopCampaign.checkpointAngles = pLeader->pev->v_angle;
		g_CoopCampaign.checkpointDuck = FBitSet( pLeader->pev->flags, FL_DUCKING ) ? TRUE : FALSE;
		g_CoopCampaign.leaderSlot = pLeader->entindex();
	}
}

void CoopCampaignRestartCheckpoint( CBasePlayer *affected )
{
	if ( g_CoopCampaign.transitionQueued || g_CoopCampaign.awaitingMap )
		return;
	for ( int i = 1; i <= gpGlobals->maxClients && i <= COOP_MAX_PLAYERS; ++i )
	{
		CBasePlayer *pPlayer = CoopPlayerByIndex( i );
		if ( !pPlayer || (affected && pPlayer != affected) )
			continue;
		if ( CoopActivePlayer( pPlayer ) )
		{
			CaptureCoopPlayer( g_CoopRespawn[i - 1], pPlayer, TRUE );
			g_CoopRespawn[i - 1].personalOrigin = FALSE;
		}
		pPlayer->RemoveAllItems( TRUE );
		pPlayer->m_afPhysicsFlags &= ~PFLAG_ONBARNACLE;
		pPlayer->pev->iuser1 = pPlayer->pev->iuser2 = 0;
		SET_VIEW( pPlayer->edict(), pPlayer->edict() );
		pPlayer->Spawn();
	}
}

static BOOL CoopWriteUInt( FILE *file, unsigned int value )
{
	unsigned char bytes[4] = { (unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16), (unsigned char)(value >> 24) };
	return fwrite( bytes, 1, sizeof(bytes), file ) == sizeof(bytes);
}

static BOOL CoopReadUInt( FILE *file, unsigned int &value )
{
	unsigned char bytes[4];
	if ( fread( bytes, 1, sizeof(bytes), file ) != sizeof(bytes) )
		return FALSE;
	value = (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8) | ((unsigned int)bytes[2] << 16) | ((unsigned int)bytes[3] << 24);
	return TRUE;
}

static BOOL CoopWriteFloat( FILE *file, float value )
{
	unsigned int bits;
	memcpy( &bits, &value, sizeof(bits) );
	return CoopWriteUInt( file, bits );
}

static BOOL CoopReadFloat( FILE *file, float &value, float minimum, float maximum )
{
	unsigned int bits;
	if ( !CoopReadUInt( file, bits ) )
		return FALSE;
	memcpy( &value, &bits, sizeof(value) );
	return value >= minimum && value <= maximum;
}

static BOOL CoopWriteVector( FILE *file, const Vector &value )
{
	return CoopWriteFloat( file, value.x ) && CoopWriteFloat( file, value.y ) && CoopWriteFloat( file, value.z );
}

static BOOL CoopReadVector( FILE *file, Vector &value )
{
	return CoopReadFloat( file, value.x, -65536, 65536 ) && CoopReadFloat( file, value.y, -65536, 65536 ) &&
		CoopReadFloat( file, value.z, -65536, 65536 );
}

static BOOL CoopWriteString( FILE *file, const char *value )
{
	unsigned int length = (unsigned int)strlen( value );
	return CoopWriteUInt( file, length ) && fwrite( value, 1, length, file ) == length;
}

static BOOL CoopReadString( FILE *file, char *value, unsigned int capacity )
{
	unsigned int length;
	if ( !CoopReadUInt( file, length ) || length >= capacity || fread( value, 1, length, file ) != length ||
		memchr( value, 0, length ) )
		return FALSE;
	value[length] = 0;
	return TRUE;
}

static BOOL CoopReadInt( FILE *file, int &value, unsigned int maximum )
{
	unsigned int number;
	if ( !CoopReadUInt( file, number ) || number > maximum )
		return FALSE;
	value = (int)number;
	return TRUE;
}

static BOOL CoopWritePlayer( FILE *file, const CoopPlayerState &saved )
{
	if ( !CoopWriteUInt( file, saved.slot ) || !CoopWriteUInt( file, saved.userID ) ||
		!CoopWriteString( file, saved.authID ) || !CoopWriteString( file, saved.name ) ||
		!CoopWriteUInt( file, saved.inventory ) || !CoopWriteUInt( file, saved.duck ) ||
		!CoopWriteVector( file, saved.origin ) || !CoopWriteVector( file, saved.angles ) || !CoopWriteVector( file, saved.velocity ) ||
		!CoopWriteFloat( file, saved.health ) || !CoopWriteFloat( file, saved.maxHealth ) || !CoopWriteFloat( file, saved.armor ) ||
		!CoopWriteUInt( file, saved.suit ) || !CoopWriteUInt( file, saved.longJump ) || !CoopWriteUInt( file, saved.flashBattery ) )
		return FALSE;
	for ( int i = 0; i < MAX_ITEMS; ++i )
	{
		if ( !CoopWriteUInt( file, saved.items[i] ) )
			return FALSE;
	}
	if ( !CoopWriteString( file, saved.activeWeapon ) || !CoopWriteUInt( file, saved.weaponCount ) )
		return FALSE;
	for ( int i = 0; i < saved.weaponCount; ++i )
	{
		if ( !CoopWriteString( file, saved.weapons[i].name ) || !CoopWriteUInt( file, saved.weapons[i].clip + 1 ) )
			return FALSE;
	}
	if ( !CoopWriteUInt( file, saved.ammoCount ) )
		return FALSE;
	for ( int i = 0; i < saved.ammoCount; ++i )
	{
		if ( !CoopWriteString( file, saved.ammo[i].name ) || !CoopWriteUInt( file, saved.ammo[i].count ) )
			return FALSE;
	}
	return TRUE;
}

static BOOL CoopReadPlayer( FILE *file, CoopPlayerState &saved )
{
	memset( &saved, 0, sizeof(saved) );
	if ( !CoopReadInt( file, saved.slot, COOP_MAX_PLAYERS ) || saved.slot < 1 ||
		!CoopReadInt( file, saved.userID, 0x7fffffffu ) ||
		!CoopReadString( file, saved.authID, sizeof(saved.authID) ) || !CoopReadString( file, saved.name, sizeof(saved.name) ) ||
		!CoopReadInt( file, saved.inventory, 1 ) || !CoopReadInt( file, saved.duck, 1 ) ||
		!CoopReadVector( file, saved.origin ) || !CoopReadVector( file, saved.angles ) || !CoopReadVector( file, saved.velocity ) ||
		!CoopReadFloat( file, saved.health, 1, 100000 ) || !CoopReadFloat( file, saved.maxHealth, 1, 100000 ) ||
		!CoopReadFloat( file, saved.armor, 0, 100000 ) || !CoopReadInt( file, saved.suit, 1 ) ||
		!CoopReadInt( file, saved.longJump, 1 ) || !CoopReadInt( file, saved.flashBattery, 100 ) )
		return FALSE;
	for ( int i = 0; i < MAX_ITEMS; ++i )
	{
		if ( !CoopReadInt( file, saved.items[i], 65535 ) )
			return FALSE;
	}
	if ( !CoopReadString( file, saved.activeWeapon, sizeof(saved.activeWeapon) ) ||
		(saved.activeWeapon[0] && !CoopKnownWeapon( saved.activeWeapon )) ||
		!CoopReadInt( file, saved.weaponCount, COOP_MAX_WEAPONS ) )
		return FALSE;
	for ( int i = 0; i < saved.weaponCount; ++i )
	{
		if ( !CoopReadString( file, saved.weapons[i].name, sizeof(saved.weapons[i].name) ) ||
			!CoopKnownWeapon( saved.weapons[i].name ) || !CoopReadInt( file, saved.weapons[i].clip, 1025 ) )
			return FALSE;
		--saved.weapons[i].clip;
		for ( int j = 0; j < i; ++j )
		{
			if ( !strcmp( saved.weapons[i].name, saved.weapons[j].name ) )
				return FALSE;
		}
	}
	if ( !CoopReadInt( file, saved.ammoCount, MAX_AMMO_SLOTS ) )
		return FALSE;
	for ( int i = 0; i < saved.ammoCount; ++i )
	{
		if ( !CoopReadString( file, saved.ammo[i].name, sizeof(saved.ammo[i].name) ) ||
			!CoopKnownAmmo( saved.ammo[i].name ) || !CoopReadInt( file, saved.ammo[i].count, 65535 ) )
			return FALSE;
		for ( int j = 0; j < i; ++j )
		{
			if ( !stricmp( saved.ammo[i].name, saved.ammo[j].name ) )
				return FALSE;
		}
	}
	saved.valid = saved.pending = TRUE;
	saved.personalOrigin = saved.inventory;
	return TRUE;
}

BOOL CoopCampaignSavePlayers( FILE *file )
{
	if ( !file || g_CoopCampaign.transitionQueued || g_CoopCampaign.awaitingMap )
		return FALSE;
	CoopPlayerState saved[COOP_MAX_PLAYERS];
	int count = 0;
	for ( int i = 1; i <= gpGlobals->maxClients && i <= COOP_MAX_PLAYERS; ++i )
	{
		CBasePlayer *pPlayer = CoopPlayerByIndex( i );
		if ( pPlayer )
			CaptureAvailableCoopPlayer( saved[count++], pPlayer );
	}
	if ( !CoopWriteUInt( file, COOP_PLAYER_SAVE_MAGIC ) || !CoopWriteUInt( file, COOP_PLAYER_SAVE_VERSION ) ||
		!CoopWriteString( file, STRING(gpGlobals->mapname) ) ||
		!CoopWriteString( file, g_CoopCampaign.sourceMap ) || !CoopWriteString( file, g_CoopCampaign.landmark ) ||
		!CoopWriteString( file, g_CoopCampaign.changeTarget ) ||
		!CoopWriteFloat( file, g_CoopCampaign.changeTarget[0] ? max( g_CoopCampaign.fireTargetTime - gpGlobals->time, 0 ) : 0 ) ||
		!CoopWriteUInt( file, g_CoopCampaign.checkpoint ) ||
		!CoopWriteVector( file, g_CoopCampaign.checkpointOrigin ) || !CoopWriteVector( file, g_CoopCampaign.checkpointAngles ) ||
		!CoopWriteUInt( file, g_CoopCampaign.checkpointDuck ) || !CoopWriteUInt( file, count ) )
		return FALSE;
	for ( int i = 0; i < count; ++i )
	{
		if ( !CoopWritePlayer( file, saved[i] ) )
			return FALSE;
	}
	return !ferror( file );
}

static BOOL ReadCoopPlayers( FILE *file, CoopCampaignState &loaded )
{
	if ( !file )
		return FALSE;
	memset( &loaded, 0, sizeof(loaded) );
	unsigned int magic, version;
	int count;
	if ( !CoopReadUInt( file, magic ) || magic != COOP_PLAYER_SAVE_MAGIC ||
		!CoopReadUInt( file, version ) || version != COOP_PLAYER_SAVE_VERSION ||
		!CoopReadString( file, loaded.map, sizeof(loaded.map) ) || !CoopIdentifier( loaded.map, FALSE ) ||
		!IS_MAP_VALID( loaded.map ) ||
		!CoopReadString( file, loaded.sourceMap, sizeof(loaded.sourceMap) ) || !CoopIdentifier( loaded.sourceMap, TRUE ) ||
		!CoopReadString( file, loaded.landmark, sizeof(loaded.landmark) ) ||
		!CoopReadString( file, loaded.changeTarget, sizeof(loaded.changeTarget) ) ||
		!CoopReadFloat( file, loaded.changeDelay, 0, 3600 ) ||
		!CoopReadInt( file, loaded.checkpoint, 1 ) || !CoopReadVector( file, loaded.checkpointOrigin ) ||
		!CoopReadVector( file, loaded.checkpointAngles ) || !CoopReadInt( file, loaded.checkpointDuck, 1 ) ||
		!CoopReadInt( file, count, COOP_MAX_PLAYERS ) )
		return FALSE;
	for ( int i = 0; i < count; ++i )
	{
		if ( !CoopReadPlayer( file, loaded.players[i] ) )
			return FALSE;
		for ( int j = 0; j < i; ++j )
		{
			if ( loaded.players[i].slot == loaded.players[j].slot )
				return FALSE;
		}
	}
	// Даже без ранее установленной контрольной точки загрузка использует сохранённые позиции
	if ( !loaded.checkpoint && count )
	{
		loaded.checkpoint = TRUE;
		loaded.checkpointOrigin = loaded.players[0].origin;
		loaded.checkpointAngles = loaded.players[0].angles;
		loaded.checkpointDuck = loaded.players[0].duck;
	}
	loaded.awaitingMap = loaded.persistent = TRUE;
	CopyCoopString( loaded.nextMap, sizeof(loaded.nextMap), loaded.map );
	return TRUE;
}

BOOL CoopCampaignValidatePlayers( FILE *file )
{
	CoopCampaignState loaded;
	return ReadCoopPlayers( file, loaded );
}

BOOL CoopCampaignLoadPlayers( FILE *file )
{
	if ( !CoopCampaignStagePlayers( file ) )
		return FALSE;
	CoopCampaignCommitPlayers();
	return TRUE;
}

BOOL CoopCampaignStagePlayers( FILE *file )
{
	CoopCampaignCancelPlayers();
	if ( !ReadCoopPlayers( file, g_CoopStagedCampaign ) )
	{
		CoopCampaignCancelPlayers();
		return FALSE;
	}
	g_CoopPlayersStaged = TRUE;
	return TRUE;
}

void CoopCampaignCommitPlayers( void )
{
	if ( !g_CoopPlayersStaged )
		return;
	ClearCoopDirectStart();
	g_CoopCampaign = g_CoopStagedCampaign;
	g_CoopPreviousValid = FALSE;
	CopyCoopString( g_CoopLoadedMap, sizeof(g_CoopLoadedMap), g_CoopStagedCampaign.map );
	CoopCampaignCancelPlayers();
}

void CoopCampaignCancelPlayers( void )
{
	memset( &g_CoopStagedCampaign, 0, sizeof(g_CoopStagedCampaign) );
	g_CoopPlayersStaged = FALSE;
}

const char *CoopCampaignLoadedMap( void )
{
	return g_CoopLoadedMap;
}

void CoopCampaignReset( void )
{
	ClearCoopDirectStart();
	CoopCampaignCancelPlayers();
	memset( &g_CoopCampaign, 0, sizeof(g_CoopCampaign) );
	memset( &g_CoopPreviousCampaign, 0, sizeof(g_CoopPreviousCampaign) );
	g_CoopPreviousValid = FALSE;
	memset( g_CoopRespawn, 0, sizeof(g_CoopRespawn) );
	g_CoopLoadedMap[0] = 0;
}
