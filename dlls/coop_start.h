#ifndef COOP_START_H
#define COOP_START_H

void CoopStartInit( void );
void CoopStartServerActivate( void );
void CoopStartCancelQueued( BOOL clearMenu = FALSE );
BOOL CoopStartThink( void );

#endif
