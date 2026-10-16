#ifndef COOP_TRANSITION_CHECK_H
#define COOP_TRANSITION_CHECK_H

class CBaseEntity;
class CBasePlayer;

void CoopTransitionCheckInit( void );
void CoopTransitionCheckServerActivate( void );
BOOL CoopTransitionCheckThink( void );
BOOL CoopTransitionCheckActive( void );
void CoopTransitionCheckTransitionRequested( CBaseEntity *trigger, CBasePlayer *player, const char *map, const char *landmark );
void CoopTransitionCheckPlayerRestored( CBasePlayer *player );
void CoopTransitionCheckReverseTouch( CBaseEntity *trigger, CBaseEntity *player );
void CoopTransitionCheckTrainBlocked( CBaseEntity *train, CBaseEntity *blocker );
void CoopTransitionCheckXenTouch( CBaseEntity *trigger, CBaseEntity *player );
void CoopTransitionCheckStripProducer( CBaseEntity *producer, float delay, int useType );
void CoopTransitionCheckStripUse( CBaseEntity *event, CBaseEntity *caller, int useType, float value );
void CoopTransitionCheckNativeStrip( CBaseEntity *event );
BOOL CoopTransitionCheckMusicCommand( CBasePlayer *player, int track );

#endif
