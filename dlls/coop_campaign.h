#ifndef COOP_CAMPAIGN_H
#define COOP_CAMPAIGN_H

#include "extdll.h"
#include <stdio.h>

class CBasePlayer;

BOOL CoopCampaignBeginTransition( const char *nextMap, const char *landmark, const Vector &sourceLandmark, CBasePlayer *activator, const char *changeTarget, float delay );
void CoopCampaignCancelTransition( void );
void CoopCampaignServerActivate( void );
void CoopCampaignPrepareDirectStart( const char *map );
void CoopCampaignRegisterReverseTransition( const char *nextMap, const char *landmark, const Vector &mins, const Vector &maxs );
void CoopCampaignThink( void );
BOOL CoopCampaignHasArrival( void );
BOOL CoopCampaignHasPersonalArrival( CBasePlayer *pPlayer );
BOOL CoopCampaignFindSpawn( CBasePlayer *pPlayer, Vector &origin, Vector &angles, BOOL &duck );
BOOL CoopCampaignRestorePlayer( CBasePlayer *pPlayer );
BOOL CoopCampaignCanTransition( CBasePlayer *pPlayer );
void CoopCampaignPlayerSpawned( CBasePlayer *pPlayer );
void CoopCampaignPlayerDied( CBasePlayer *pPlayer );
void CoopCampaignPlayerStripped( CBasePlayer *pPlayer );
void CoopCampaignPlayerDisconnected( int slot );
void CoopCampaignRestartCheckpoint( CBasePlayer *affected );
BOOL CoopCampaignSavePlayers( FILE *file );
BOOL CoopCampaignValidatePlayers( FILE *file );
BOOL CoopCampaignLoadPlayers( FILE *file );
BOOL CoopCampaignStagePlayers( FILE *file );
void CoopCampaignCommitPlayers( void );
void CoopCampaignCancelPlayers( void );
const char *CoopCampaignLoadedMap( void );
void CoopCampaignReset( void );

#endif
