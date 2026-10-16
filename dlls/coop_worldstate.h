#ifndef COOP_WORLDSTATE_H
#define COOP_WORLDSTATE_H

#include <stdio.h>

// Снимки мира используют сериализацию SDK и принадлежат текущей кооперативной кампании
void CoopWorldServerActivate( edict_t *pEdictList, int edictCount, int clientMax );
BOOL CoopWorldEntityRestored( const edict_t *pEntity );
BOOL CoopWorldCapture( const char *nextMap, const char *landmark, const Vector &sourceLandmark );
void CoopWorldThink( void );
void CoopWorldReset( void );

// Глобальные имена восстанавливаются до появления остальных сущностей новой карты
BOOL CoopWorldHasPendingTransition( void );
BOOL CoopWorldApplyPendingGlobals( void );

// Проверка файла не меняет текущую кампанию и состояние движка
BOOL CoopWorldWrite( FILE *file );
BOOL CoopWorldValidate( FILE *file );
BOOL CoopWorldRead( FILE *file );
BOOL CoopWorldStage( FILE *file );
void CoopWorldCommit( void );
void CoopWorldCancel( void );

#endif
