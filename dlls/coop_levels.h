#ifndef COOP_LEVELS_H
#define COOP_LEVELS_H

class CBasePlayer;

// Каталог оригинальной кампании исключает сетевые и учебные карты
int CoopLevelChapter( const char *map );
BOOL CoopLevelKnown( const char *map );
const char *CoopLevelMapByIndex( int index );

// Особые входы используются после позиции сохранения и перед обычными точками карты
BOOL CoopLevelFindStart( CBasePlayer *pPlayer, Vector &origin, Vector &angles, BOOL &duck );

#endif
