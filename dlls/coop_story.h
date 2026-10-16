#ifndef COOP_STORY_H
#define COOP_STORY_H

#include "extdll.h"
#include <stdio.h>

class CBaseEntity;
class CBasePlayer;
class CItem;
class Vector;

void CoopStoryServerActivate( void );
void CoopStoryReset( void );
void CoopStoryThink( void );
void CoopStoryEntitySpawn( CBaseEntity *pEntity );
BOOL CoopStoryHandleClientCommand( CBasePlayer *pPlayer, const char *pCommand );
BOOL CoopStoryHandleUse( CBaseEntity *pEvent, CBaseEntity *pActivator );

CBasePlayer *CoopStoryChoosePlayer( CBaseEntity *pActivator );
void CoopStoryCameraStarted( CBaseEntity *pCamera, CBasePlayer *pPlayer, float flEndTime, BOOL fTakeControl );
void CoopStoryCameraStopped( CBaseEntity *pCamera );
void CoopStoryResetPlayer( CBasePlayer *pPlayer );
BOOL CoopStoryIsBusy( void );
BOOL CoopStoryAwaitingRestore( void );
void CoopStoryEndCampaign( void );
BOOL CoopRestoreCamera( CBaseEntity *pCamera, CBasePlayer *pPlayer, float flRemaining, BOOL fTakeControl );

BOOL CoopStoryFindTrainSpawn( CBasePlayer *pPlayer, Vector &vecOrigin, Vector &vecAngles );
BOOL CoopStoryHasTrainSpawn( void );
void CoopStoryPlayerGotItem( CBasePlayer *pPlayer, CItem *pItem );
BOOL CoopStoryPlayCDTrack( int track );

BOOL CoopStoryWrite( FILE *pFile );
BOOL CoopStoryRead( FILE *pFile );
BOOL CoopStoryValidate( FILE *pFile );
BOOL CoopStoryStage( FILE *pFile );
void CoopStoryCommit( void );
void CoopStoryCancel( void );

#endif
