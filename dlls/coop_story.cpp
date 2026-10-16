#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "items.h"
#include "trains.h"
#include "coop_story.h"
#include "coop_campaign.h"
#include "coop_session.h"
#ifdef HLCOOP_MAPCHECK
#include "coop_transition_check.h"
#endif

extern int gmsgItemPickup;

namespace
{

const int STORY_MAX_PLAYERS = 32;
const unsigned int STORY_FILE_MAGIC = 0x31595453;
const unsigned int STORY_FILE_VERSION = 1;

struct StoryCameraPlayer
{
	EHANDLE hPlayer;
	BOOL fWasFrozen;
	BOOL fApplied;
};

struct StoryFileData
{
	unsigned int magic;
	unsigned int version;
	int hasSuit;
	int hasLongJump;
	int completed;
	int cdTrack;
	int hasCamera;
	int cameraTakeControl;
	int cameraOwner;
	float cameraRemaining;
	char cameraTarget[64];
	int cameraApplied[STORY_MAX_PLAYERS];
	int cameraWasFrozen[STORY_MAX_PLAYERS];
};

EHANDLE g_hCamera;
EHANDLE g_hCameraOwner;
StoryCameraPlayer g_cameraPlayers[STORY_MAX_PLAYERS];
float g_flCameraEndTime;
BOOL g_fCameraTakeControl;
BOOL g_fSharedSuit;
BOOL g_fSharedLongJump;
BOOL g_fCompleted;
BOOL g_fGroupUse;
BOOL g_fFailurePending;
float g_flFailureTime;
float g_flNextFailureTime;
float g_flNextEquipmentCheck;
EHANDLE g_hIntroTrain;
BOOL g_fIntroReferenceReady;
unsigned int g_iTrainDiagnostics;
unsigned int g_iTrainFailedPlayers;
unsigned int g_iTrainSuccessfulPlayers;
Vector g_vecIntroSpawnLocal;
Vector g_vecIntroViewAngles;
Vector g_vecIntroTrainAngles;
StoryFileData g_pendingStory;
StoryFileData g_stagedStory;
BOOL g_fStoryStaged;
BOOL g_fPendingStory;
BOOL g_fPendingStoryReady;
int g_iCDTrack = -2;
EHANDLE g_hMusicPlayers[STORY_MAX_PLAYERS];
int g_iMusicTracks[STORY_MAX_PLAYERS];
int g_iMusicUsers[STORY_MAX_PLAYERS];

BOOL IsCoopStory( void )
{
	return g_pGameRules && g_pGameRules->IsCoOp();
}

CBasePlayer *StoryPlayerByIndex( int index, BOOL fAliveOnly )
{
	CBaseEntity *pEntity = UTIL_PlayerByIndex( index );
	if ( !pEntity || !pEntity->IsPlayer() || pEntity->IsDormant() ||
		!(pEntity->pev->flags & FL_CLIENT) || GETPLAYERUSERID( pEntity->edict() ) <= 0 ||
		(pEntity->pev->flags & (FL_KILLME | FL_SPECTATOR)) )
		return NULL;

	CBasePlayer *pPlayer = (CBasePlayer *)pEntity;
	if ( fAliveOnly && (!pPlayer->IsAlive() || pPlayer->m_fCoopSpawnPending || pPlayer->IsObserver()) )
		return NULL;

	return pPlayer;
}

int StoryPlayerCount( void )
{
	return min( (int)gpGlobals->maxClients, STORY_MAX_PLAYERS );
}

void SendMusic( CBasePlayer *pPlayer )
{
	if ( g_iCDTrack < -1 )
		return;
#ifdef HLCOOP_MAPCHECK
	if ( !CoopTransitionCheckMusicCommand(pPlayer, g_iCDTrack) )
#endif
	{
		if ( g_iCDTrack == -1 )
			CLIENT_COMMAND( pPlayer->edict(), "cd stop\n" );
		else
			CLIENT_COMMAND( pPlayer->edict(), "cd play %d\n", g_iCDTrack );
	}
	int index = pPlayer->entindex() - 1;
	if ( index >= 0 && index < STORY_MAX_PLAYERS )
	{
		g_hMusicPlayers[index] = pPlayer;
		g_iMusicTracks[index] = g_iCDTrack;
		g_iMusicUsers[index] = GETPLAYERUSERID( pPlayer->edict() );
	}
}

void UpdateMusic( void )
{
	for ( int i = 1; i <= StoryPlayerCount(); i++ )
	{
		CBasePlayer *pPlayer = StoryPlayerByIndex( i, FALSE );
		if ( pPlayer && ((CBaseEntity *)g_hMusicPlayers[i - 1] != pPlayer || g_iMusicTracks[i - 1] != g_iCDTrack ||
			g_iMusicUsers[i - 1] != GETPLAYERUSERID( pPlayer->edict() )) )
			SendMusic( pPlayer );
	}
}

void ReleaseCameraPlayer( int index )
{
	StoryCameraPlayer &state = g_cameraPlayers[index];
	CBaseEntity *pEntity = state.hPlayer;
	if ( state.fApplied && pEntity && pEntity->IsPlayer() && !pEntity->IsDormant() )
	{
		CBasePlayer *pPlayer = (CBasePlayer *)pEntity;
		SET_VIEW( pPlayer->edict(), pPlayer->edict() );
		if ( g_fCameraTakeControl && !state.fWasFrozen )
			pPlayer->EnableControl( TRUE );
	}
	state.hPlayer = NULL;
	state.fApplied = FALSE;
	state.fWasFrozen = FALSE;
}

void ApplyCameraPlayer( CBasePlayer *pPlayer )
{
	int index = pPlayer->entindex() - 1;
	if ( index < 0 || index >= STORY_MAX_PLAYERS || !g_hCamera )
		return;

	StoryCameraPlayer &state = g_cameraPlayers[index];
	if ( state.fApplied && (CBaseEntity *)state.hPlayer != pPlayer )
		ReleaseCameraPlayer( index );

	if ( !state.fApplied )
	{
		state.hPlayer = pPlayer;
		state.fWasFrozen = (pPlayer->pev->flags & FL_FROZEN) != 0;
		state.fApplied = TRUE;
		SET_VIEW( pPlayer->edict(), g_hCamera->edict() );
	}
	if ( g_fCameraTakeControl )
		pPlayer->EnableControl( FALSE );
}

void ShareEquipment( void )
{
	for ( int i = 1; i <= StoryPlayerCount(); i++ )
	{
		CBasePlayer *pPlayer = StoryPlayerByIndex( i, FALSE );
		if ( !pPlayer )
			continue;
		if ( pPlayer->pev->weapons & (1 << WEAPON_SUIT) )
			g_fSharedSuit = TRUE;
		if ( pPlayer->m_fLongJump )
			g_fSharedLongJump = TRUE;
	}
	if ( g_fSharedLongJump )
		g_fSharedSuit = TRUE;

	for ( int i = 1; i <= StoryPlayerCount(); i++ )
	{
		CBasePlayer *pPlayer = StoryPlayerByIndex( i, TRUE );
		if ( !pPlayer )
			continue;

		// Сюжетный предмет срабатывает один раз, возможности получают все участники
		if ( g_fSharedSuit )
			pPlayer->pev->weapons |= (1 << WEAPON_SUIT);
		if ( g_fSharedLongJump && !pPlayer->m_fLongJump )
		{
			pPlayer->m_fLongJump = TRUE;
			g_engfuncs.pfnSetPhysicsKeyValue( pPlayer->edict(), "slj", "1" );
			if ( gmsgItemPickup )
			{
				MESSAGE_BEGIN( MSG_ONE, gmsgItemPickup, NULL, pPlayer->pev );
					WRITE_STRING( "item_longjump" );
				MESSAGE_END();
			}
		}
	}
}

void AddLandmarkAlias( const char *pMissingName, const char *pExistingName )
{
	if ( UTIL_FindEntityByTargetname( NULL, pMissingName ) )
		return;
	CBaseEntity *pExisting = UTIL_FindEntityByTargetname( NULL, pExistingName );
	if ( !pExisting || !FClassnameIs( pExisting->pev, "info_landmark" ) )
		return;

	CBaseEntity *pAlias = CBaseEntity::Create( "info_landmark", pExisting->pev->origin, g_vecZero );
	if ( pAlias )
	{
		pAlias->pev->targetname = ALLOC_STRING( pMissingName );
		ALERT( at_console, "Co-op: landmark alias %s -> %s\n", pMissingName, pExistingName );
	}
}

void PrepareIntroTrain( void )
{
	g_hIntroTrain = NULL;
	g_fIntroReferenceReady = FALSE;
	CBaseEntity *pTrainEntity = UTIL_FindEntityByString( NULL, "globalname", "intro_train" );
	if ( !pTrainEntity || !FClassnameIs( pTrainEntity->pev, "func_tracktrain" ) )
		return;

	CFuncTrackTrain *pTrain = (CFuncTrackTrain *)pTrainEntity;
	g_hIntroTrain = pTrain;
	CBaseEntity *pTrackEntity = UTIL_FindEntityByTargetname( NULL, STRING(pTrain->pev->target) );
	CBaseEntity *pStart = UTIL_FindEntityByClassname( NULL, "info_player_start" );
	if ( !pStart || !pTrackEntity || !FClassnameIs( pTrackEntity->pev, "path_track" ) )
	{
		if ( !(g_iTrainDiagnostics & 1) )
		{
			g_iTrainDiagnostics |= 1;
			ALERT( at_console, "Co-op train: reference unavailable map=%s train=%d target=%s start=%d track=%d\n",
				STRING(gpGlobals->mapname), pTrain->entindex(), STRING(pTrain->pev->target),
				pStart ? pStart->entindex() : 0, pTrackEntity ? pTrackEntity->entindex() : 0 );
		}
		return;
	}

	CPathTrack *pTrack = (CPathTrack *)pTrackEntity;
	Vector vecInitialOrigin = pTrack->pev->origin + Vector( 0, 0, pTrain->m_height );
	Vector vecLook = pTrack->pev->origin;
	pTrack->LookAhead( &vecLook, pTrain->m_length, 0 );
	vecLook.z += pTrain->m_height;
	if ( (vecLook - vecInitialOrigin).Length() < 1.0f )
	{
		if ( !(g_iTrainDiagnostics & 2) )
		{
			g_iTrainDiagnostics |= 2;
			ALERT( at_console, "Co-op train: reference waiting for path links map=%s target=%s ppath=%p\n",
				STRING(gpGlobals->mapname), STRING(pTrain->pev->target), (void *)pTrain->m_ppath );
		}
		return;
	}

	g_vecIntroTrainAngles = UTIL_VecToAngles( vecLook - vecInitialOrigin );
	g_vecIntroTrainAngles.y += 180;
	if ( pTrain->pev->spawnflags & SF_TRACKTRAIN_NOPITCH )
		g_vecIntroTrainAngles.x = 0;

	Vector vecForward, vecRight, vecUp;
	UTIL_MakeVectorsPrivate( g_vecIntroTrainAngles, vecForward, vecRight, vecUp );
	Vector vecOffset = pStart->pev->origin - vecInitialOrigin;
	if ( vecOffset.Length() > 512.0f )
	{
		if ( !(g_iTrainDiagnostics & 4) )
		{
			g_iTrainDiagnostics |= 4;
			ALERT( at_console, "Co-op train: distant reference spawn=(%.1f %.1f %.1f) track=(%.1f %.1f %.1f)\n",
				pStart->pev->origin.x, pStart->pev->origin.y, pStart->pev->origin.z,
				vecInitialOrigin.x, vecInitialOrigin.y, vecInitialOrigin.z );
		}
		return;
	}
	g_vecIntroSpawnLocal = Vector( DotProduct( vecOffset, vecForward ),
		-DotProduct( vecOffset, vecRight ), DotProduct( vecOffset, vecUp ) );
	g_vecIntroViewAngles = pStart->pev->angles;
	g_fIntroReferenceReady = TRUE;
	if ( !(g_iTrainDiagnostics & 8) )
	{
		g_iTrainDiagnostics |= 8;
		ALERT( at_console, "Co-op train: ready map=%s id=%d model=%s index=%d expected=%d origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f) height=%.1f ppath=%p speed=%.1f\n",
			STRING(gpGlobals->mapname), pTrain->entindex(), STRING(pTrain->pev->model),
			pTrain->pev->modelindex, MODEL_INDEX(STRING(pTrain->pev->model)),
			pTrain->pev->origin.x, pTrain->pev->origin.y, pTrain->pev->origin.z,
			pTrain->pev->angles.x, pTrain->pev->angles.y, pTrain->pev->angles.z,
			pTrain->m_height, (void *)pTrain->m_ppath, pTrain->pev->speed );
		ALERT( at_console, "Co-op train: source spawn=(%.1f %.1f %.1f) initial=(%.1f %.1f %.1f) refangles=(%.1f %.1f %.1f) local=(%.1f %.1f %.1f)\n",
			pStart->pev->origin.x, pStart->pev->origin.y, pStart->pev->origin.z,
			vecInitialOrigin.x, vecInitialOrigin.y, vecInitialOrigin.z,
			g_vecIntroTrainAngles.x, g_vecIntroTrainAngles.y, g_vecIntroTrainAngles.z,
			g_vecIntroSpawnLocal.x, g_vecIntroSpawnLocal.y, g_vecIntroSpawnLocal.z );
	}
}

BOOL FindTrainFloor( CBasePlayer *pPlayer, CFuncTrackTrain *pTrain, const Vector &vecCandidate, Vector &vecOrigin, TraceResult *pFirstTrace = NULL )
{
	TraceResult trace;
	UTIL_TraceHull( vecCandidate, vecCandidate - Vector( 0, 0, 64 ), dont_ignore_monsters, human_hull, pPlayer->edict(), &trace );
	if ( pFirstTrace )
		*pFirstTrace = trace;
	if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction == 1.0f ||
		trace.vecPlaneNormal.z < 0.7f || trace.pHit != pTrain->edict() )
		return FALSE;
	Vector vecFloor = trace.vecEndPos + Vector( 0, 0, 1 );
	UTIL_TraceHull( vecFloor, vecFloor, dont_ignore_monsters, human_hull, pPlayer->edict(), &trace );
	if ( trace.fStartSolid || trace.fAllSolid )
		return FALSE;
	// Запас между пассажирами сохраняет свободные корпуса при повороте вагона
	for ( int i = 1; i <= StoryPlayerCount(); ++i )
	{
		CBasePlayer *pMember = StoryPlayerByIndex( i, TRUE );
		if ( !pMember || pMember == pPlayer ||
			vecFloor.z + VEC_HULL_MAX.z <= pMember->pev->origin.z + pMember->pev->mins.z ||
			vecFloor.z + VEC_HULL_MIN.z >= pMember->pev->origin.z + pMember->pev->maxs.z )
			continue;
		if ( pMember->pev->groundentity != pTrain->edict() )
		{
			UTIL_TraceHull( pMember->pev->origin, pMember->pev->origin - Vector( 0, 0, 8 ), ignore_monsters,
				FBitSet(pMember->pev->flags, FL_DUCKING) ? head_hull : human_hull, pMember->edict(), &trace );
			if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction == 1.0f ||
				trace.vecPlaneNormal.z < 0.7f || trace.pHit != pTrain->edict() )
				continue;
		}
		if ( (vecFloor - pMember->pev->origin).Length2D() + 0.01f < 64 )
			return FALSE;
	}
	vecOrigin = vecFloor;
	return TRUE;
}

BOOL ReadStoryFileData( FILE *pFile, StoryFileData &data )
{
	if ( !pFile || fread( &data, sizeof(data), 1, pFile ) != 1 ||
		data.magic != STORY_FILE_MAGIC || data.version != STORY_FILE_VERSION )
		return FALSE;
	if ( data.hasSuit < 0 || data.hasSuit > 1 || data.hasLongJump < 0 || data.hasLongJump > 1 ||
		data.completed < 0 || data.completed > 1 || data.cdTrack < -2 || data.cdTrack > 30 ||
		data.hasCamera < 0 || data.hasCamera > 1 ||
		data.cameraTakeControl < 0 || data.cameraTakeControl > 1 ||
		data.cameraOwner < 0 || data.cameraOwner > STORY_MAX_PLAYERS ||
		data.cameraRemaining != data.cameraRemaining || data.cameraRemaining < 0 || data.cameraRemaining > 86400 ||
		!memchr( data.cameraTarget, '\0', sizeof(data.cameraTarget) ) )
		return FALSE;
	if ( data.hasCamera && !data.cameraTarget[0] )
		return FALSE;
	for ( int i = 0; i < STORY_MAX_PLAYERS; i++ )
	{
		if ( data.cameraApplied[i] < 0 || data.cameraApplied[i] > 1 ||
			data.cameraWasFrozen[i] < 0 || data.cameraWasFrozen[i] > 1 )
			return FALSE;
	}
	return TRUE;
}

}

void CoopStoryServerActivate( void )
{
	// Ссылки старой карты нельзя применять к клиентам новой карты
	for ( int i = 0; i < STORY_MAX_PLAYERS; i++ )
	{
		g_cameraPlayers[i].hPlayer = NULL;
		g_cameraPlayers[i].fApplied = FALSE;
		g_cameraPlayers[i].fWasFrozen = FALSE;
		g_hMusicPlayers[i] = NULL;
		g_iMusicTracks[i] = -2;
		g_iMusicUsers[i] = 0;
	}
	g_hCamera = NULL;
	g_hCameraOwner = NULL;
	g_flCameraEndTime = 0;
	g_fCameraTakeControl = FALSE;
	g_fSharedSuit = FALSE;
	g_fSharedLongJump = FALSE;
	g_fCompleted = FALSE;
	g_iCDTrack = -2;
	g_fGroupUse = FALSE;
	g_fFailurePending = FALSE;
	g_flFailureTime = 0;
	g_flNextFailureTime = 0;
	g_flNextEquipmentCheck = 0;
	g_hIntroTrain = NULL;
	g_fIntroReferenceReady = FALSE;
	g_iTrainDiagnostics = g_iTrainFailedPlayers = g_iTrainSuccessfulPlayers = 0;
	if ( !IsCoopStory() )
		return;
	if ( g_fPendingStory )
	{
		g_fSharedSuit = g_pendingStory.hasSuit;
		g_fSharedLongJump = g_pendingStory.hasLongJump;
		g_fCompleted = g_pendingStory.completed;
		g_iCDTrack = g_pendingStory.cdTrack;
		g_fPendingStoryReady = TRUE;
	}

	// В обновлённой кампании переименован landmark после каскадного резонанса
	const char *pMap = STRING(gpGlobals->mapname);
	if ( FStrEq( pMap, "c1a0b" ) || FStrEq( pMap, "c1a0e" ) || FStrEq( pMap, "c1a0c" ) )
	{
		AddLandmarkAlias( "c1a0btoc", "c1a0etoc" );
		AddLandmarkAlias( "c1a0etoc", "c1a0btoc" );
	}
	PrepareIntroTrain();
}

void CoopStoryEntitySpawn( CBaseEntity *pEntity )
{
	if ( !IsCoopStory() || !pEntity )
		return;
	// Сюжетные сообщения и титры видны всей группе без повторного запуска событий
	if ( FClassnameIs( pEntity->pev, "env_message" ) )
		pEntity->pev->spawnflags |= SF_MESSAGE_ALL;
}

void CoopStoryReset( void )
{
	CoopStoryCancel();
	CoopStoryCameraStopped( NULL );
	g_fPendingStory = FALSE;
	g_fPendingStoryReady = FALSE;
	memset( &g_pendingStory, 0, sizeof(g_pendingStory) );
	g_fSharedSuit = FALSE;
	g_fSharedLongJump = FALSE;
	g_fCompleted = FALSE;
	g_fGroupUse = FALSE;
	g_fFailurePending = FALSE;
	g_flFailureTime = 0;
	g_flNextFailureTime = 0;
	g_flNextEquipmentCheck = 0;
	g_hIntroTrain = NULL;
	g_fIntroReferenceReady = FALSE;
	g_iTrainDiagnostics = g_iTrainFailedPlayers = g_iTrainSuccessfulPlayers = 0;
	g_iCDTrack = -2;
	for ( int i = 0; i < STORY_MAX_PLAYERS; i++ )
	{
		g_hMusicPlayers[i] = NULL;
		g_iMusicTracks[i] = -2;
		g_iMusicUsers[i] = 0;
	}
}

CBasePlayer *CoopStoryChoosePlayer( CBaseEntity *pActivator )
{
	if ( pActivator && pActivator->IsPlayer() )
	{
		CBasePlayer *pPlayer = StoryPlayerByIndex( pActivator->entindex(), TRUE );
		if ( pPlayer == pActivator )
			return pPlayer;
	}
	CBasePlayer *pNearest = NULL;
	float flNearestDistance = 1.0e20f;
	for ( int i = 1; i <= StoryPlayerCount(); i++ )
	{
		CBasePlayer *pPlayer = StoryPlayerByIndex( i, TRUE );
		if ( !pPlayer )
			continue;
		if ( !pActivator )
			return pPlayer;
		Vector vecOffset = pPlayer->pev->origin - pActivator->pev->origin;
		float flDistance = DotProduct( vecOffset, vecOffset );
		if ( flDistance < flNearestDistance )
		{
			flNearestDistance = flDistance;
			pNearest = pPlayer;
		}
	}
	return pNearest;
}

void CoopStoryCameraStarted( CBaseEntity *pCamera, CBasePlayer *pPlayer, float flEndTime, BOOL fTakeControl )
{
	if ( !IsCoopStory() || !pCamera || !pPlayer )
		return;
	CoopStoryCameraStopped( NULL );
	g_hCamera = pCamera;
	g_hCameraOwner = pPlayer;
	g_flCameraEndTime = flEndTime;
	g_fCameraTakeControl = fTakeControl;
	for ( int i = 1; i <= StoryPlayerCount(); i++ )
	{
		CBasePlayer *pMember = StoryPlayerByIndex( i, TRUE );
		if ( pMember )
			ApplyCameraPlayer( pMember );
	}
}

void CoopStoryCameraStopped( CBaseEntity *pCamera )
{
	if ( pCamera && (CBaseEntity *)g_hCamera != pCamera )
		return;
	for ( int i = 0; i < STORY_MAX_PLAYERS; i++ )
		ReleaseCameraPlayer( i );
	g_hCamera = NULL;
	g_hCameraOwner = NULL;
	g_flCameraEndTime = 0;
	g_fCameraTakeControl = FALSE;
}

void CoopStoryResetPlayer( CBasePlayer *pPlayer )
{
	if ( !pPlayer )
		return;
	int index = pPlayer->entindex() - 1;
	if ( index >= 0 && index < STORY_MAX_PLAYERS && !pPlayer->m_fCoopHasSpawned )
	{
		// Первое появление повторяет музыку, которая могла потеряться во время подключения
		g_hMusicPlayers[index] = NULL;
		g_iMusicTracks[index] = -2;
		g_iMusicUsers[index] = 0;
	}
	if ( index >= 0 && index < STORY_MAX_PLAYERS && (CBaseEntity *)g_cameraPlayers[index].hPlayer == pPlayer )
		ReleaseCameraPlayer( index );
}

BOOL CoopStoryIsBusy( void )
{
	return IsCoopStory() && (g_fFailurePending || g_fPendingStory || (g_hCamera && !g_fCompleted));
}

BOOL CoopStoryAwaitingRestore( void )
{
	return g_fPendingStory && g_pendingStory.hasCamera;
}

void CoopStoryEndCampaign( void )
{
	if ( !IsCoopStory() || g_fCompleted )
		return;
	g_fCompleted = TRUE;
	UTIL_ClientPrintAll( HUD_PRINTTALK, "Half-Life Co-op: campaign completed\n" );
	ALERT( at_console, "Co-op: campaign completed\n" );
}

void CoopStoryThink( void )
{
	if ( !IsCoopStory() )
		return;
	if ( g_fPendingStory && g_fPendingStoryReady )
	{
		CBasePlayer *pOwner = NULL;
		if ( g_pendingStory.cameraOwner > 0 && g_pendingStory.cameraOwner <= StoryPlayerCount() )
			pOwner = StoryPlayerByIndex( g_pendingStory.cameraOwner, TRUE );
		if ( !pOwner )
			pOwner = CoopStoryChoosePlayer( NULL );
		if ( !pOwner )
			return;

		// Ссылки камеры восстанавливаем после загрузки карты и появления участников
		if ( g_pendingStory.hasCamera )
		{
			CBaseEntity *pCamera = UTIL_FindEntityByTargetname( NULL, g_pendingStory.cameraTarget );
			if ( pCamera && FClassnameIs( pCamera->pev, "trigger_camera" ) )
			{
				if ( CoopRestoreCamera( pCamera, pOwner, g_pendingStory.cameraRemaining, g_pendingStory.cameraTakeControl ) )
				{
					for ( int i = 0; i < StoryPlayerCount(); i++ )
					{
						if ( g_pendingStory.cameraApplied[i] && g_cameraPlayers[i].fApplied )
							g_cameraPlayers[i].fWasFrozen = g_pendingStory.cameraWasFrozen[i];
					}
				}
			}
			else
			{
				ALERT( at_error, "Co-op: saved camera %s was not restored\n", g_pendingStory.cameraTarget );
			}
		}
		g_fPendingStory = FALSE;
		g_fPendingStoryReady = FALSE;
	}
	if ( g_fFailurePending && gpGlobals->time >= g_flFailureTime )
	{
		g_fFailurePending = FALSE;
		g_flNextFailureTime = gpGlobals->time + 2.0f;
		CoopStoryCameraStopped( NULL );
		if ( !CoopSessionRestoreCheckpoint() )
			CoopCampaignRestartCheckpoint( NULL );
		return;
	}

	if ( g_hCamera )
	{
		CBaseEntity *pOwner = g_hCameraOwner;
		if ( !pOwner || !pOwner->IsAlive() || pOwner->IsDormant() )
		{
			pOwner = CoopStoryChoosePlayer( NULL );
			g_hCameraOwner = pOwner;
		}
		if ( !pOwner || gpGlobals->time >= g_flCameraEndTime )
		{
			CoopStoryCameraStopped( NULL );
		}
		else
		{
			for ( int i = 1; i <= StoryPlayerCount(); i++ )
			{
				CBasePlayer *pPlayer = StoryPlayerByIndex( i, TRUE );
				if ( pPlayer )
					ApplyCameraPlayer( pPlayer );
				else
					ReleaseCameraPlayer( i - 1 );
			}
		}
	}
	else
	{
		// Удалённая камера не должна оставлять неподвижных игроков
		for ( int i = 0; i < STORY_MAX_PLAYERS; i++ )
			ReleaseCameraPlayer( i );
	}

	if ( gpGlobals->time >= g_flNextEquipmentCheck )
	{
		g_flNextEquipmentCheck = gpGlobals->time + 0.5f;
		ShareEquipment();
		UpdateMusic();
	}
}

BOOL CoopStoryHandleUse( CBaseEntity *pEvent, CBaseEntity *pActivator )
{
	if ( !IsCoopStory() || !pEvent || g_fGroupUse )
		return FALSE;

	if ( FClassnameIs( pEvent->pev, "player_loadsaved" ) )
	{
		if ( !g_fFailurePending && gpGlobals->time >= g_flNextFailureTime )
		{
			g_fFailurePending = TRUE;
			g_flFailureTime = gpGlobals->time + 0.1f;
			ALERT( at_console, "Co-op: campaign failure %s, restoring checkpoint\n", STRING(pEvent->pev->targetname) );
		}
		return TRUE;
	}

	if ( FClassnameIs( pEvent->pev, "player_weaponstrip" ) || FClassnameIs( pEvent->pev, "game_player_equip" ) )
	{
		BOOL fStrip = FClassnameIs( pEvent->pev, "player_weaponstrip" );
		if ( fStrip )
			CoopCampaignPlayerStripped( NULL );
		g_fGroupUse = TRUE;
		for ( int i = 1; i <= StoryPlayerCount(); i++ )
		{
			CBasePlayer *pPlayer = StoryPlayerByIndex( i, FALSE );
			if ( pPlayer )
			{
				pEvent->Use( pPlayer, pEvent, USE_TOGGLE, 0 );
				if ( fStrip )
					CoopCampaignPlayerStripped( pPlayer );
			}
		}
		g_fGroupUse = FALSE;
#ifdef HLCOOP_MAPCHECK
		if ( fStrip ) CoopTransitionCheckNativeStrip( pEvent );
#endif
		return TRUE;
	}

	BOOL fEndEntity = FClassnameIs( pEvent->pev, "game_end" ) || FClassnameIs( pEvent->pev, "trigger_endsection" );
	if ( fEndEntity ||
		(FStrEq( STRING(gpGlobals->mapname), "c5a1" ) &&
		 FClassnameIs( pEvent->pev, "env_message" ) && FStrEq( STRING(pEvent->pev->targetname), "end3" )) )
	{
		CoopStoryEndCampaign();
		return fEndEntity;
	}

	return FALSE;
}

BOOL CoopStoryFindTrainSpawn( CBasePlayer *pPlayer, Vector &vecOrigin, Vector &vecAngles )
{
	if ( !IsCoopStory() || !pPlayer )
		return FALSE;
	if ( !g_hIntroTrain || !g_fIntroReferenceReady )
		PrepareIntroTrain();
	if ( !g_hIntroTrain )
		return FALSE;

	CFuncTrackTrain *pTrain = (CFuncTrackTrain *)(CBaseEntity *)g_hIntroTrain;
	unsigned int playerBit = 1u << (pPlayer->entindex() - 1);
	Vector vecArrival, vecArrivalAngles;
	BOOL fArrivalDuck = FALSE;
	if ( CoopCampaignHasArrival() && CoopCampaignFindSpawn( pPlayer, vecArrival, vecArrivalAngles, fArrivalDuck ) &&
		!fArrivalDuck && FindTrainFloor( pPlayer, pTrain, vecArrival + Vector( 0, 0, 2 ), vecOrigin ) )
	{
		vecAngles = vecArrivalAngles;
		if ( !(g_iTrainSuccessfulPlayers & playerBit) )
		{
			g_iTrainSuccessfulPlayers |= playerBit;
			ALERT( at_console, "Co-op train: player %d arrival floor accepted at (%.1f %.1f %.1f) train=(%.1f %.1f %.1f)\n",
				pPlayer->entindex(), vecOrigin.x, vecOrigin.y, vecOrigin.z,
				pTrain->pev->origin.x, pTrain->pev->origin.y, pTrain->pev->origin.z );
		}
		return TRUE;
	}
	if ( !pTrain->m_ppath || !g_fIntroReferenceReady )
	{
		if ( !(g_iTrainDiagnostics & 16) )
		{
			g_iTrainDiagnostics |= 16;
			ALERT( at_console, "Co-op train: spawn waiting map=%s ppath=%p reference=%d origin=(%.1f %.1f %.1f) ltime=%.2f next=%.2f\n",
				STRING(gpGlobals->mapname), (void *)pTrain->m_ppath, g_fIntroReferenceReady,
				pTrain->pev->origin.x, pTrain->pev->origin.y, pTrain->pev->origin.z,
				pTrain->pev->ltime, pTrain->pev->nextthink );
		}
		return FALSE;
	}

	static const Vector offsets[] =
	{
		Vector( 0, 0, 0 ), Vector( 36, 0, 0 ), Vector( -36, 0, 0 ),
		Vector( 0, 36, 0 ), Vector( 0, -36, 0 ),
		Vector( 64, 0, 0 ), Vector( -64, 0, 0 ),
		Vector( 0, 64, 0 ), Vector( 0, -64, 0 ),
		Vector( 72, 0, 0 ), Vector( -72, 0, 0 ),
		Vector( 64, 64, 0 ), Vector( 64, -64, 0 ),
		Vector( -64, 64, 0 ), Vector( -64, -64, 0 ),
		Vector( 64, 32, 0 ), Vector( 64, -32, 0 ),
		Vector( -64, 32, 0 ), Vector( -64, -32, 0 )
	};
	Vector vecForward, vecRight, vecUp;
	UTIL_MakeVectorsPrivate( pTrain->pev->angles, vecForward, vecRight, vecUp );
	Vector vecBase = pTrain->pev->origin + vecForward * g_vecIntroSpawnLocal.x -
		vecRight * g_vecIntroSpawnLocal.y + vecUp * g_vecIntroSpawnLocal.z + Vector( 0, 0, 2 );
	const char *pSource = "reference";
	for ( int i = 1; i <= StoryPlayerCount(); i++ )
	{
		CBasePlayer *pMember = StoryPlayerByIndex( i, TRUE );
		if ( pMember && pMember != pPlayer && pMember->pev->groundentity == pTrain->edict() )
		{
			vecBase = pMember->pev->origin + Vector( 0, 0, 2 );
			pSource = "teammate";
			break;
		}
	}
	TraceResult firstTrace;
	memset( &firstTrace, 0, sizeof(firstTrace) );
	for ( int i = 0; i < ARRAYSIZE(offsets); i++ )
	{
		Vector vecCandidate = vecBase + vecForward * offsets[i].x - vecRight * offsets[i].y + vecUp * offsets[i].z;
		if ( !FindTrainFloor( pPlayer, pTrain, vecCandidate, vecOrigin, i == 0 ? &firstTrace : NULL ) )
			continue;

		vecAngles = g_vecIntroViewAngles;
		vecAngles.y += pTrain->pev->angles.y - g_vecIntroTrainAngles.y;
		if ( !(g_iTrainSuccessfulPlayers & playerBit) )
		{
			g_iTrainSuccessfulPlayers |= playerBit;
			ALERT( at_console, "Co-op train: player %d %s floor accepted offset=%d origin=(%.1f %.1f %.1f)\n",
				pPlayer->entindex(), pSource, i, vecOrigin.x, vecOrigin.y, vecOrigin.z );
		}
		return TRUE;
	}
	if ( !(g_iTrainFailedPlayers & playerBit) )
	{
		g_iTrainFailedPlayers |= playerBit;
		ALERT( at_console, "Co-op train: player %d %s floor rejected base=(%.1f %.1f %.1f) train=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f) solid=%d model=%s index=%d expected=%d ppath=%p\n",
			pPlayer->entindex(), pSource, vecBase.x, vecBase.y, vecBase.z,
			pTrain->pev->origin.x, pTrain->pev->origin.y, pTrain->pev->origin.z,
			pTrain->pev->angles.x, pTrain->pev->angles.y, pTrain->pev->angles.z,
			pTrain->pev->solid, STRING(pTrain->pev->model), pTrain->pev->modelindex,
			MODEL_INDEX(STRING(pTrain->pev->model)), (void *)pTrain->m_ppath );
		ALERT( at_console, "Co-op train: trace0 startsolid=%d allsolid=%d fraction=%.3f normalZ=%.3f hit=%d class=%s end=(%.1f %.1f %.1f)\n",
			firstTrace.fStartSolid, firstTrace.fAllSolid, firstTrace.flFraction, firstTrace.vecPlaneNormal.z,
			firstTrace.pHit ? ENTINDEX(firstTrace.pHit) : -1,
			firstTrace.pHit ? STRING(firstTrace.pHit->v.classname) : "none",
			firstTrace.vecEndPos.x, firstTrace.vecEndPos.y, firstTrace.vecEndPos.z );
	}
	return FALSE;
}

BOOL CoopStoryHasTrainSpawn( void )
{
	if ( !IsCoopStory() )
		return FALSE;
	if ( !g_hIntroTrain )
		PrepareIntroTrain();
	if ( g_hIntroTrain && FStrEq(STRING(gpGlobals->mapname), "c0a0e") &&
		g_hIntroTrain->pev->speed == 0 && g_hIntroTrain->pev->velocity.Length() < 1 )
		return FALSE;
	return g_hIntroTrain ? TRUE : FALSE;
}

void CoopStoryPlayerGotItem( CBasePlayer *pPlayer, CItem *pItem )
{
	if ( !IsCoopStory() || !pPlayer || !pItem )
		return;
	if ( FClassnameIs( pItem->pev, "item_suit" ) )
		g_fSharedSuit = TRUE;
	else if ( FClassnameIs( pItem->pev, "item_longjump" ) )
	{
		g_fSharedSuit = TRUE;
		g_fSharedLongJump = TRUE;
	}
	else
		return;
	ShareEquipment();
}

BOOL CoopStoryPlayCDTrack( int track )
{
	if ( !IsCoopStory() )
		return FALSE;
	if ( track < -1 || track > 30 )
	{
		ALERT( at_error, "Co-op: CD track %d is out of range\n", track );
		return TRUE;
	}
	g_iCDTrack = track;
	for ( int i = 1; i <= StoryPlayerCount(); i++ )
	{
		CBasePlayer *pPlayer = StoryPlayerByIndex( i, FALSE );
		if ( pPlayer )
			SendMusic( pPlayer );
	}
	return TRUE;
}

BOOL CoopStoryHandleClientCommand( CBasePlayer *pPlayer, const char *pCommand )
{
	if ( !IsCoopStory() || !pPlayer || !pCommand ||
		(!FStrEq( pCommand, "coop_story" ) && !FStrEq( pCommand, "coop_story_status" )) )
		return FALSE;

	char message[192];
	_snprintf( message, sizeof(message), "Campaign: %s | suit %d | long jump %d | scene %d | completed %d\n",
		STRING(gpGlobals->mapname), g_fSharedSuit, g_fSharedLongJump, g_hCamera ? 1 : 0, g_fCompleted );
	message[sizeof(message) - 1] = '\0';
	ClientPrint( pPlayer->pev, HUD_PRINTCONSOLE, message );
	return TRUE;
}

BOOL CoopStoryWrite( FILE *pFile )
{
	if ( !pFile || g_fFailurePending || g_fPendingStory )
		return FALSE;
	StoryFileData data;
	memset( &data, 0, sizeof(data) );
	data.magic = STORY_FILE_MAGIC;
	data.version = STORY_FILE_VERSION;
	data.hasSuit = g_fSharedSuit ? 1 : 0;
	data.hasLongJump = g_fSharedLongJump ? 1 : 0;
	data.completed = g_fCompleted ? 1 : 0;
	data.cdTrack = g_iCDTrack;
	CBaseEntity *pCamera = g_hCamera;
	CBaseEntity *pOwner = g_hCameraOwner;
	if ( pCamera )
	{
		if ( FStringNull( pCamera->pev->targetname ) || strlen( STRING(pCamera->pev->targetname) ) >= sizeof(data.cameraTarget) )
			return FALSE;
		data.hasCamera = 1;
		data.cameraTakeControl = g_fCameraTakeControl ? 1 : 0;
		data.cameraOwner = pOwner ? pOwner->entindex() : 0;
		data.cameraRemaining = max( 0.0f, g_flCameraEndTime - gpGlobals->time );
		strncpy( data.cameraTarget, STRING(pCamera->pev->targetname), sizeof(data.cameraTarget) - 1 );
		for ( int i = 0; i < STORY_MAX_PLAYERS; i++ )
		{
			data.cameraApplied[i] = g_cameraPlayers[i].fApplied ? 1 : 0;
			data.cameraWasFrozen[i] = g_cameraPlayers[i].fWasFrozen ? 1 : 0;
		}
	}
	return fwrite( &data, sizeof(data), 1, pFile ) == 1;
}

BOOL CoopStoryRead( FILE *pFile )
{
	if ( !CoopStoryStage( pFile ) )
		return FALSE;
	CoopStoryCommit();
	return TRUE;
}

BOOL CoopStoryStage( FILE *pFile )
{
	CoopStoryCancel();
	if ( !ReadStoryFileData( pFile, g_stagedStory ) )
	{
		CoopStoryCancel();
		return FALSE;
	}
	g_fStoryStaged = TRUE;
	return TRUE;
}

void CoopStoryCommit( void )
{
	if ( !g_fStoryStaged )
		return;
	// Чтение файла ещё не меняет игроков предыдущей карты
	g_pendingStory = g_stagedStory;
	g_fPendingStory = TRUE;
	g_fPendingStoryReady = FALSE;
	CoopStoryCancel();
}

void CoopStoryCancel( void )
{
	memset( &g_stagedStory, 0, sizeof(g_stagedStory) );
	g_fStoryStaged = FALSE;
}

BOOL CoopStoryValidate( FILE *pFile )
{
	StoryFileData data;
	return ReadStoryFileData( pFile, data );
}
