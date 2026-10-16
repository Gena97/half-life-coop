#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "coop_levels.h"

namespace
{

const char *const chapterMaps[][11] =
{
	{ "c0a0", "c0a0a", "c0a0b", "c0a0c", "c0a0d", "c0a0e", NULL },
	{ "c1a0", "c1a0d", "c1a0a", "c1a0b", "c1a0e", NULL },
	{ "c1a0c", "c1a1", "c1a1a", "c1a1f", "c1a1b", "c1a1c", "c1a1d", NULL },
	{ "c1a2", "c1a2a", "c1a2b", "c1a2c", "c1a2d", NULL },
	{ "c1a3", "c1a3d", "c1a3a", "c1a3b", "c1a3c", NULL },
	{ "c1a4", "c1a4k", "c1a4b", "c1a4d", "c1a4e", "c1a4f", "c1a4i", "c1a4g", "c1a4j", NULL },
	{ "c2a1", "c2a1b", "c2a1a", NULL },
	{ "c2a2", "c2a2a", "c2a2b1", "c2a2b2", "c2a2c", "c2a2d", "c2a2e", "c2a2f", "c2a2g", "c2a2h", NULL },
	{ "c2a3", "c2a3a", "c2a3b", "c2a3c", "c2a3d", "c2a3e", NULL },
	{ "c2a4", "c2a4a", "c2a4b", "c2a4c", NULL },
	{ "c2a4d", "c2a4e", "c2a4f", "c2a4g", NULL },
	{ "c2a5", "c2a5w", "c2a5x", "c2a5a", "c2a5b", "c2a5c", "c2a5d", "c2a5e", "c2a5f", "c2a5g", NULL },
	{ "c3a1", "c3a1a", "c3a1b", NULL },
	{ "c3a2e", "c3a2", "c3a2a", "c3a2b", "c3a2c", "c3a2f", "c3a2d", NULL },
	{ "c4a1", NULL },
	{ "c4a2", "c4a2a", "c4a2b", NULL },
	{ "c4a1a", "c4a1b", "c4a1c", "c4a1d", "c4a1e", "c4a1f", NULL },
	{ "c4a3", NULL },
	{ "c5a1", NULL }
};

BOOL FindLevelFloor( CBasePlayer *pPlayer, const Vector &seed, BOOL duck, Vector &result )
{
	static const Vector offsets[] =
	{
		Vector( 0, 0, 0 ),
		Vector( 48, 0, 0 ), Vector( -48, 0, 0 ), Vector( 0, 48, 0 ), Vector( 0, -48, 0 ),
		Vector( 48, 48, 0 ), Vector( 48, -48, 0 ), Vector( -48, 48, 0 ), Vector( -48, -48, 0 )
	};
	// Номер корпуса задаётся явно и не зависит от прежней позы игрока
	int hull = duck ? head_hull : human_hull;
	for ( int i = 0; i < ARRAYSIZE(offsets); ++i )
	{
		Vector candidate = seed + offsets[i];
		TraceResult trace;
		// До соседней точки должен существовать свободный проход для выбранного корпуса
		UTIL_TraceHull( seed, candidate, ignore_monsters, hull, pPlayer->edict(), &trace );
		if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction != 1.0f )
			continue;
		UTIL_TraceHull( candidate, candidate - Vector( 0, 0, 64 ), ignore_monsters, hull, pPlayer->edict(), &trace );
		if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction == 1.0f || trace.vecPlaneNormal.z < 0.7f )
			continue;
		candidate = trace.vecEndPos + Vector( 0, 0, 1 );
		int contents = UTIL_PointContents(candidate);
		if ( contents == CONTENTS_SOLID || contents == CONTENTS_LAVA || contents == CONTENTS_SLIME )
			continue;
		UTIL_TraceHull( candidate, candidate, dont_ignore_monsters, hull, pPlayer->edict(), &trace );
		if ( trace.fStartSolid || trace.fAllSolid )
			continue;
		result = candidate;
		return TRUE;
	}
	return FALSE;
}

}

int CoopLevelChapter( const char *map )
{
	if ( !map || !*map )
		return -1;
	for ( int chapter = 0; chapter < ARRAYSIZE(chapterMaps); ++chapter )
		for ( int i = 0; i < ARRAYSIZE(chapterMaps[chapter]) && chapterMaps[chapter][i]; ++i )
			if ( !stricmp(map, chapterMaps[chapter][i]) )
				return chapter;
	return -1;
}

BOOL CoopLevelKnown( const char *map )
{
	return CoopLevelChapter(map) >= 0;
}

const char *CoopLevelMapByIndex( int index )
{
	if ( index < 0 )
		return NULL;
	if ( index < ARRAYSIZE(chapterMaps) )
		return chapterMaps[index][0];
	index -= ARRAYSIZE(chapterMaps);
	for ( int chapter = 0; chapter < ARRAYSIZE(chapterMaps); ++chapter )
		for ( int i = 0; i < ARRAYSIZE(chapterMaps[chapter]) && chapterMaps[chapter][i]; ++i )
			if ( index-- == 0 )
				return chapterMaps[chapter][i];
	return NULL;
}

BOOL CoopLevelFindStart( CBasePlayer *pPlayer, Vector &origin, Vector &angles, BOOL &duck )
{
	if ( !pPlayer )
		return FALSE;
	const char *map = STRING(gpGlobals->mapname);
	Vector seed;
	BOOL useDuck;
	Vector viewAngles( 0, 0, 0 );
	if ( !stricmp(map, "c1a4e") )
	{
		// В этой карте нет ни одной штатной точки появления
		seed = Vector( 1425, 986, 380 );
		useDuck = FALSE;
		viewAngles.y = 270;
	}
	else if ( !stricmp(map, "c1a1b") )
	{
		// Штатный вход находится в низком проходе и требует приседания
		seed = Vector( -285, -172, -177 );
		useDuck = TRUE;
	}
	else if ( !stricmp(map, "c1a2d") )
	{
		// Штатная точка зажата ящиками, свободный участок находится перед ними
		seed = Vector( 1322, -1544, -518 );
		useDuck = FALSE;
	}
	else
		return FALSE;
	Vector candidate;
	if ( !FindLevelFloor(pPlayer, seed, useDuck, candidate) )
		return FALSE;
	origin = candidate;
	angles = viewAngles;
	duck = useDuck;
	return TRUE;
}
