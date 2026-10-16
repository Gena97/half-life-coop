#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "gamerules.h"
#include "saverestore.h"
#include "coop_campaign.h"
#include "coop_worldstate.h"
#include "coop_story.h"
#include "coop_session.h"
#include "coop_levels.h"
#include "coop_start.h"

static char g_requestedStart[32];
static char g_startDestination[32];
static BOOL g_startChanging;

static void ReadMenuChoice( void )
{
	if ( IS_DEDICATED_SERVER() )
		return;
	CBaseEntity *host = UTIL_PlayerByIndex(1);
	if ( !host || !host->IsPlayer() || host->IsDormant() || GETPLAYERUSERID(host->edict()) <= 0 )
		return;
	char *info = g_engfuncs.pfnGetInfoKeyBuffer(host->edict());
	const char *choice = g_engfuncs.pfnInfoKeyValue(info, "hlcoop_choice");
	if ( !choice || !*choice )
		return;
	int index = 0;
	BOOL valid = strlen(choice) < 4;
	for ( const char *p = choice; valid && *p; ++p )
	{
		if ( *p < '0' || *p > '9' )
			valid = FALSE;
		else
			index = index * 10 + *p - '0';
	}
	const char *map = valid ? CoopLevelMapByIndex(index) : NULL;
	const char *skill = g_engfuncs.pfnInfoKeyValue(info, "hlcoop_skill");
	int selectedSkill = skill && strlen(skill) == 1 && *skill >= '0' && *skill <= '2' ? *skill - '0' + 1 : 2;
	// Выбор меню принадлежит хозяину и потребляется до следующего перехода
	g_engfuncs.pfnSetClientKeyValue(1, info, "hlcoop_choice", "");
	SERVER_COMMAND( "setinfo hlcoop_choice \"\"\n" );
	if ( !map || !IS_MAP_VALID((char *)map) )
		return;
	strncpy( g_requestedStart, map, sizeof(g_requestedStart) - 1 );
	g_requestedStart[sizeof(g_requestedStart) - 1] = 0;
	CVAR_SET_FLOAT( "skill", selectedSkill );
}

static void CoopStartCommand( void )
{
	if ( CMD_ARGC() != 2 || !CoopLevelKnown(CMD_ARGV(1)) || !IS_MAP_VALID((char *)CMD_ARGV(1)) )
	{
		ALERT( at_console, "Half-Life Co-op: use coop_start with a campaign map from the level menu\n" );
		return;
	}
	if ( gpGlobals->maxClients < 2 || CoopWorldHasPendingTransition() || g_startChanging )
	{
		ALERT( at_console, "Half-Life Co-op: wait for the cooperative server to finish loading\n" );
		return;
	}
	strncpy( g_requestedStart, CMD_ARGV(1), sizeof(g_requestedStart) - 1 );
	g_requestedStart[sizeof(g_requestedStart) - 1] = 0;
}

void CoopStartInit( void )
{
	g_requestedStart[0] = g_startDestination[0] = 0;
	g_startChanging = FALSE;
	g_engfuncs.pfnAddServerCommand( (char *)"coop_start", CoopStartCommand );
}

void CoopStartCancelQueued( BOOL clearMenu )
{
	g_requestedStart[0] = 0;
	if ( clearMenu && !IS_DEDICATED_SERVER() )
	{
		CBaseEntity *host = UTIL_PlayerByIndex(1);
		if ( host && host->IsPlayer() )
		{
			char *info = g_engfuncs.pfnGetInfoKeyBuffer(host->edict());
			g_engfuncs.pfnSetClientKeyValue(1, info, "hlcoop_choice", "");
		}
		SERVER_COMMAND( "setinfo hlcoop_choice \"\"\n" );
	}
}

void CoopStartServerActivate( void )
{
	if ( !g_pGameRules || !g_pGameRules->IsCoOp() )
	{
		g_requestedStart[0] = g_startDestination[0] = 0;
		g_startChanging = FALSE;
		return;
	}
	const char *map = STRING(gpGlobals->mapname);
	if ( g_startChanging )
	{
		if ( !strcmp(map, g_startDestination) )
			CoopCampaignPrepareDirectStart( map );
		g_startChanging = FALSE;
		g_startDestination[0] = 0;
	}
}

BOOL CoopStartThink( void )
{
	if ( g_startChanging )
		return TRUE;
	ReadMenuChoice();
	if ( !g_requestedStart[0] || CoopWorldHasPendingTransition() )
		return FALSE;
	BOOL ready = FALSE;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBaseEntity *entity = UTIL_PlayerByIndex( i );
		if ( entity && entity->IsPlayer() && !entity->IsDormant() && entity->IsAlive() &&
			!((CBasePlayer *)entity)->m_fCoopSpawnPending )
		{
			ready = TRUE;
			break;
		}
	}
	if ( !ready )
		return FALSE;
	strcpy( g_startDestination, g_requestedStart );
	g_requestedStart[0] = 0;
	// Выбор уровня начинает новую сессию и не переносит мир предыдущей кампании
	CoopWorldReset();
	CoopCampaignReset();
	CoopStoryReset();
	CoopSessionReset();
	gGlobalState.ClearStates();
	g_startChanging = TRUE;
	ALERT( at_console, "Half-Life Co-op: starting selected level %s\n", g_startDestination );
	CHANGE_LEVEL( g_startDestination, NULL );
	return TRUE;
}
