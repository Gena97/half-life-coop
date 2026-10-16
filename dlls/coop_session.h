#ifndef COOP_SESSION_H
#define COOP_SESSION_H

class CBasePlayer;
class CBaseEntity;

void CoopSessionInit( void );
void CoopSessionReset( void );
void CoopSessionServerActivate( void );
void CoopSessionServerDeactivate( void );
void CoopSessionThink( void );
BOOL CoopSessionSave( const char *name );
BOOL CoopSessionLoad( const char *name );
BOOL CoopSessionSaveCheckpoint( void );
void CoopSessionRequestCheckpoint( CBaseEntity *trigger );
BOOL CoopSessionRestoreCheckpoint( void );
BOOL CoopSessionClientCommand( CBasePlayer *player, const char *command );

#endif
