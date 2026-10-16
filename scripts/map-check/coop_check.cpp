#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <time.h>
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "client.h"
#include "gamerules.h"
#include "coop_campaign.h"
#include "coop_levels.h"
#include "coop_story.h"
#include "coop_check.h"
#include "coop_transition_check.h"

#ifdef HLCOOP_MAPCHECK
namespace
{
const int MAP_COUNT = 96;
const char *const BOT_NAME = "hccheck_probe";

enum CheckPhase
{
	CHECK_WAIT_MAP,
	CHECK_WARMUP,
	CHECK_RUNNING
};

FILE *report;
BOOL active;
CheckPhase phase;
unsigned long runId;
int firstMap, requestedCount, completed, passed, failed, warnings;
int bootPassed, bootFailed, scenePassed, sceneFailed;
float dwellSeconds, activationTime, runTime, nextSample;
char expectedMap[32];
edict_t *botIdentity;
int botIndex, botSerial, botUser;
BOOL everReady, everClear, passiveDeath, initialReady;
BOOL entryActorReady, trainDoorOpened, entrySpaceClear;

float MapDwell( void )
{
	return !stricmp(expectedMap, "c0a0e") && dwellSeconds < 45 ? 45 : dwellSeconds;
}

BOOL AllowedServer( void )
{
	if ( !IS_DEDICATED_SERVER() )
		return FALSE;
	char directory[1024];
	GET_GAME_DIR(directory);
	directory[sizeof(directory) - 1] = 0;
	size_t length = strlen(directory);
	while ( length && (directory[length - 1] == '/' || directory[length - 1] == '\\') )
		directory[--length] = 0;
	const char *name = directory + length;
	while ( name > directory && name[-1] != '/' && name[-1] != '\\' )
		--name;
	return !stricmp(name, "hlcoop_check") || !stricmp(name, "hlcoop_check_2") ||
		!stricmp(name, "hlcoop_check_3") || !stricmp(name, "hlcoop_check_4");
}

void JsonString( const char *value )
{
	fputc('"', report);
	for ( const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p )
	{
		if ( *p == '"' || *p == '\\' )
			fprintf(report, "\\%c", *p);
		else if ( *p < 32 )
			fprintf(report, "\\u%04x", *p);
		else
			fputc(*p, report);
	}
	fputc('"', report);
}

BOOL FiniteVector( const Vector &value )
{
	return _finite(value.x) && _finite(value.y) && _finite(value.z);
}

void JsonVector( const Vector &value )
{
	if ( FiniteVector(value) )
		fprintf(report, "[%.4f,%.4f,%.4f]", value.x, value.y, value.z);
	else
		fputs("null", report);
}

void BeginRecord( const char *type )
{
	fprintf(report, "{\"run\":%lu,\"type\":", runId);
	JsonString(type);
}

void EndRecord( void )
{
	fputs("}\n", report);
	fflush(report);
}

void MapFields( void )
{
	fprintf(report, ",\"index\":%d,\"map\":", firstMap + completed);
	JsonString(expectedMap);
	fputs(",\"observed_map\":", report);
	JsonString(STRING(gpGlobals->mapname));
	fprintf(report, ",\"game_time\":%.4f,\"map_dwell_seconds\":%.0f", gpGlobals->time, MapDwell());
}

void ClearBot( void )
{
	botIdentity = NULL;
	botIndex = botSerial = botUser = 0;
}

CBasePlayer *BotPlayer( void )
{
	if ( !botIdentity || botIndex < 1 || botIndex > gpGlobals->maxClients )
		return NULL;
	edict_t *entity = INDEXENT(botIndex);
	if ( !entity || entity != botIdentity || entity->free || entity->serialnumber != botSerial ||
		!entity->pvPrivateData || !(entity->v.flags & FL_FAKECLIENT) || GETPLAYERUSERID(entity) != botUser )
		return NULL;
	CBaseEntity *player = CBaseEntity::Instance(entity);
	return player && player->IsPlayer() ? (CBasePlayer *)player : NULL;
}

BOOL Ready( CBasePlayer *player )
{
	return player && player->IsAlive() && !player->IsDormant() && !player->IsObserver() &&
		!player->m_fCoopSpawnPending && player->pev->solid == SOLID_SLIDEBOX;
}

BOOL ClearHull( CBasePlayer *player )
{
	if ( !player || !FiniteVector(player->pev->origin) )
		return FALSE;
	TraceResult trace;
	UTIL_TraceHull(player->pev->origin, player->pev->origin, dont_ignore_monsters,
		(player->pev->flags & FL_DUCKING) ? head_hull : human_hull, player->edict(), &trace);
	return !trace.fStartSolid && !trace.fAllSolid;
}

void ElevatorFields( void )
{
	if ( stricmp(expectedMap, "c1a2") )
		return;
	fputs(",\"elevator_doors\":[", report);
	CBaseEntity *door = NULL;
	int count = 0;
	while ( (door = UTIL_FindEntityByTargetname(door, "startele1")) != NULL )
	{
		if ( !FClassnameIs(door->pev, "func_door") || door->IsDormant() )
			continue;
		fprintf(report, "%s{\"state\":%d,\"origin\":", count++ ? "," : "", door->GetToggleState());
		JsonVector(door->pev->origin);
		fputc('}', report);
	}
	fputc(']', report);
}

int IntroBarriers( void )
{
	int count = 0;
	CBaseEntity *barrier = NULL;
	while ( (barrier = UTIL_FindEntityByTargetname(barrier, "intro_box")) != NULL )
		if ( !(barrier->pev->flags & FL_KILLME) )
			++count;
	return count;
}

void EntryFields( BOOL firstSnapshot )
{
	BOOL guardMap = !stricmp(expectedMap, "c0a0e");
	BOOL bossMap = !stricmp(expectedMap, "c4a2a") || !stricmp(expectedMap, "c4a2b");
	if ( guardMap || bossMap )
	{
		CBaseEntity *actor = UTIL_FindEntityByTargetname(NULL, guardMap ? "barney1" : "big_momma");
		BOOL exists = actor && !actor->IsDormant() && !(actor->pev->flags & FL_KILLME) &&
			FClassnameIs(actor->pev, guardMap ? "monster_barney" : "monster_bigmomma");
		const char *route = !stricmp(expectedMap, "c4a2a") ? "goose5" : "goose8b";
		if ( firstSnapshot )
			entryActorReady = exists && actor->pev->health > 0 &&
				(guardMap || !strcmp(STRING(actor->pev->netname), route));
		fprintf(report, ",\"entry_actor_exists\":%s,\"initial_actor_ready\":%s",
			exists ? "true" : "false", entryActorReady ? "true" : "false");
		if ( exists )
		{
			fprintf(report, ",\"entry_actor_health\":%.3f,\"entry_actor_route\":", actor->pev->health);
			JsonString(STRING(actor->pev->netname));
			fputs(",\"entry_actor_origin\":", report);
			JsonVector(actor->pev->origin);
		}
	}
	if ( guardMap )
	{
		CBaseEntity *door = UTIL_FindEntityByTargetname(NULL, "traindoor");
		CBaseEntity *end = UTIL_FindEntityByTargetname(NULL, "traindoor3");
		if ( door && end && FClassnameIs(door->pev, "func_train") &&
			(door->pev->origin - (end->pev->origin - (door->pev->mins + door->pev->maxs) * 0.5f)).Length() < 2 &&
			door->pev->velocity.Length() < 1 )
			trainDoorOpened = TRUE;
		fprintf(report, ",\"train_door_exists\":%s,\"train_door_opened\":%s", door ? "true" : "false", trainDoorOpened ? "true" : "false");
		if ( door )
		{
			fprintf(report, ",\"train_door_state\":%d,\"train_door_origin\":", door->GetToggleState());
			JsonVector(door->pev->origin);
			fputs(",\"train_door_target\":", report);
			JsonString(STRING(door->pev->target));
		}
	}
	if ( !stricmp(expectedMap, "c2a3e") )
	{
		int remaining = IntroBarriers();
		CBasePlayer *player = BotPlayer();
		if ( !remaining && Ready(player) && ClearHull(player) )
			entrySpaceClear = TRUE;
		fprintf(report, ",\"intro_box_remaining\":%d,\"entry_space_cleared\":%s", remaining, entrySpaceClear ? "true" : "false");
	}
}

void Sample( const char *samplePhase )
{
	BeginRecord("sample");
	MapFields();
	fputs(",\"phase\":", report);
	JsonString(samplePhase);
	fprintf(report, ",\"elapsed\":%.4f", gpGlobals->time - runTime);
	CBasePlayer *player = BotPlayer();
	fprintf(report, ",\"player_exists\":%s", player ? "true" : "false");
	if ( player )
	{
		fprintf(report, ",\"health\":%.3f,\"armor\":%.3f,\"pending\":%s,\"solid\":%d,\"movetype\":%d,\"flags\":%d,\"frozen\":%s,\"clear_hull\":%s,\"origin\":",
			player->pev->health, player->pev->armorvalue, player->m_fCoopSpawnPending ? "true" : "false",
			player->pev->solid, player->pev->movetype, player->pev->flags,
			(player->pev->flags & FL_FROZEN) ? "true" : "false", ClearHull(player) ? "true" : "false");
		JsonVector(player->pev->origin);
		fputs(",\"velocity\":", report);
		JsonVector(player->pev->velocity);
		fprintf(report, ",\"story_busy\":%s", CoopStoryIsBusy() ? "true" : "false");
		if ( !player->IsAlive() )
		{
			fputs(",\"damage_entity_class\":", report);
			edict_t *source = player->pev->dmg_inflictor;
			JsonString(source && !source->free ? STRING(source->v.classname) : "");
		}
	}
	ElevatorFields();
	// Состояние актёра фиксируется до первого кадра боя и не требует его победы или выживания
	EntryFields(!strcmp(samplePhase, "spawn"));
	EndRecord();
}

void Stop( const char *status )
{
	if ( !active )
		return;
	BOOL hadBot = BotPlayer() != NULL;
	BeginRecord("summary");
	fputs(",\"status\":", report);
	JsonString(status);
	fprintf(report, ",\"total\":%d,\"checked\":%d,\"passed\":%d,\"failed\":%d,\"warnings\":%d,\"boot_checks\":{\"passed\":%d,\"failed\":%d},\"scene_checks\":{\"checked\":%d,\"passed\":%d,\"failed\":%d}",
		requestedCount, completed, passed, failed, warnings, bootPassed, bootFailed,
		scenePassed + sceneFailed, scenePassed, sceneFailed);
	EndRecord();
	fclose(report);
	report = NULL;
	active = FALSE;
	ClearBot();
	if ( hadBot )
		SERVER_COMMAND("kick \"hccheck_probe\"\n");
	ALERT(at_console, "Map check: %s, checked %d/%d, passed %d, failed %d\n", status, completed, requestedCount, passed, failed);
}

void QueueMap( void )
{
	const char *map = CoopLevelMapByIndex(19 + firstMap + completed);
	if ( !map || !IS_MAP_VALID((char *)map) )
	{
		Stop("invalid_catalog_map");
		return;
	}
	strncpy(expectedMap, map, sizeof(expectedMap) - 1);
	expectedMap[sizeof(expectedMap) - 1] = 0;
	BOOL hadBot = BotPlayer() != NULL;
	ClearBot();
	everReady = everClear = passiveDeath = initialReady = FALSE;
	entryActorReady = trainDoorOpened = entrySpaceClear = FALSE;
	phase = CHECK_WAIT_MAP;
	BeginRecord("map_queued");
	MapFields();
	EndRecord();
	char command[64];
	_snprintf(command, sizeof(command), "map %s\n", expectedMap);
	if ( hadBot )
		SERVER_COMMAND("kick \"hccheck_probe\"\n");
	SERVER_COMMAND(command);
}

void FinishMap( const char *overrideReason = NULL )
{
	Sample("end");
	BOOL boot = everReady && everClear;
	const char *scene = "not_checked";
	if ( !stricmp(expectedMap, "c1a2") && !overrideReason && boot )
	{
		int count = 0, opened = 0;
		CBaseEntity *door = NULL;
		while ( (door = UTIL_FindEntityByTargetname(door, "startele1")) != NULL )
			if ( FClassnameIs(door->pev, "func_door") && !door->IsDormant() )
			{
				++count;
				if ( door->GetToggleState() == TS_AT_TOP )
					++opened;
			}
		scene = count >= 2 && opened == count ? "passed" : "failed";
	}
	else if ( !overrideReason && boot && !stricmp(expectedMap, "c0a0e") )
		scene = entryActorReady && trainDoorOpened ? "passed" : "failed";
	else if ( !overrideReason && boot && !stricmp(expectedMap, "c2a3e") )
		scene = !IntroBarriers() && entrySpaceClear ? "passed" : "failed";
	else if ( !overrideReason && boot && (!stricmp(expectedMap, "c4a2a") || !stricmp(expectedMap, "c4a2b")) )
		scene = entryActorReady ? "passed" : "failed";
	if ( !strcmp(scene, "passed") ) ++scenePassed;
	else if ( !strcmp(scene, "failed") ) ++sceneFailed;
	BOOL success = !overrideReason && boot && strcmp(scene, "failed");
	CBasePlayer *player = BotPlayer();
	BOOL frozen = player && (player->pev->flags & FL_FROZEN);
	if ( passiveDeath || frozen || overrideReason || (everReady && !initialReady) )
		++warnings;
	if ( boot ) ++bootPassed;
	else ++bootFailed;
	if ( success ) ++passed;
	else ++failed;
	BeginRecord("map_result");
	MapFields();
	fprintf(report, ",\"status\":\"%s\",\"boot\":\"%s\",\"scene\":\"%s\",\"passive_death\":%s,\"frozen\":%s,\"reason\":",
		success ? "passed" : "failed", boot ? "passed" : "failed", scene,
		passiveDeath ? "true" : "false", frozen ? "true" : "false");
	JsonString(overrideReason ? overrideReason : !everReady ? "spawn_pending_or_missing" : !everClear ? "blocked_hull" :
		!strcmp(scene, "failed") ? "entry_scene_not_completed" : passiveDeath ? "passive_death_after_spawn" : frozen ? "scripted_control_or_frozen" : "startup_observed");
	EndRecord();
	++completed;
	if ( completed >= requestedCount )
		Stop("complete");
	else
		QueueMap();
}

BOOL SpawnProbe( void )
{
	CoopCampaignPrepareDirectStart(expectedMap);
	edict_t *entity = g_engfuncs.pfnCreateFakeClient(BOT_NAME);
	if ( !entity )
		return FALSE;
	botIdentity = entity;
	botIndex = ENTINDEX(entity);
	botSerial = entity->serialnumber;
	botUser = GETPLAYERUSERID(entity);
	if ( botUser <= 0 )
		return FALSE;
	char reject[128] = { 0 };
	if ( !ClientConnect(entity, BOT_NAME, "127.0.0.1", reject) )
		return FALSE;
	ClientPutInServer(entity);
	entity->v.flags |= FL_CLIENT | FL_FAKECLIENT;
	return BotPlayer() != NULL;
}

BOOL ParseNumber( const char *text, int minimum, int maximum, int &result )
{
	char *end;
	long value = strtol(text, &end, 10);
	if ( !*text || *end || value < minimum || value > maximum )
		return FALSE;
	result = (int)value;
	return TRUE;
}

void StartCommand( void )
{
	if ( !AllowedServer() || !g_pGameRules || !g_pGameRules->IsCoOp() || gpGlobals->maxClients < 2 || active || CoopTransitionCheckActive() )
	{
		ALERT(at_console, "Map check: use an idle dedicated hlcoop_check server with at least two slots\n");
		return;
	}
	int start = 0, count = MAP_COUNT, dwell = 20;
	if ( CMD_ARGC() > 4 || (CMD_ARGC() > 1 && !ParseNumber(CMD_ARGV(1), 0, MAP_COUNT - 1, start)) ||
		(CMD_ARGC() > 2 && !ParseNumber(CMD_ARGV(2), 1, MAP_COUNT - start, count)) ||
		(CMD_ARGC() > 3 && !ParseNumber(CMD_ARGV(3), 15, 120, dwell)) )
	{
		ALERT(at_console, "Map check: hccheck_start [firstIndex 0..95] [count] [dwellSeconds 15..120]\n");
		return;
	}
	if ( CMD_ARGC() <= 2 )
		count = MAP_COUNT - start;
	char directory[1024], path[1100];
	GET_GAME_DIR(directory);
	_snprintf(path, sizeof(path), "%s/map-check.jsonl", directory);
	path[sizeof(path) - 1] = 0;
	report = fopen(path, "ab");
	if ( !report )
	{
		ALERT(at_console, "Map check: unable to open map-check.jsonl\n");
		return;
	}
	runId = (unsigned long)time(NULL);
	firstMap = start;
	requestedCount = count;
	dwellSeconds = (float)dwell;
	completed = passed = failed = warnings = bootPassed = bootFailed = scenePassed = sceneFailed = 0;
	active = TRUE;
	BeginRecord("start");
	fprintf(report, ",\"first_index\":%d,\"total\":%d,\"dwell_seconds\":%d,\"fake_clients\":1,\"scene_scope\":\"c0a0e_guard_train_door,c1a2_elevator_doors,c2a3e_intro_box,c4a2a_c4a2b_entry_boss\"", start, count, dwell);
	EndRecord();
	ALERT(at_console, "Map check: started %d maps at index %d, %.0f seconds per map\n", requestedCount, firstMap, dwellSeconds);
	QueueMap();
}

void StopCommand( void )
{
	if ( AllowedServer() )
	{
		if ( active )
			Stop("stopped");
		else
			ALERT(at_console, "Map check: no active run\n");
	}
}
}

void CoopCheckInit( void )
{
	if ( !AllowedServer() )
		return;
	g_engfuncs.pfnAddServerCommand((char *)"hccheck_start", StartCommand);
	g_engfuncs.pfnAddServerCommand((char *)"hccheck_stop", StopCommand);
}

BOOL CoopCheckActive( void ) { return active; }

void CoopCheckServerActivate( void )
{
	ClearBot();
	if ( !active )
		return;
	activationTime = gpGlobals->time;
	phase = CHECK_WARMUP;
	BeginRecord("map_loaded");
	MapFields();
	EndRecord();
}

BOOL CoopCheckThink( void )
{
	if ( !active )
		return FALSE;
	if ( phase == CHECK_WAIT_MAP )
		return TRUE;
	if ( stricmp(expectedMap, STRING(gpGlobals->mapname)) )
	{
		FinishMap("unexpected_map_transition");
		return TRUE;
	}
	if ( phase == CHECK_WARMUP )
	{
		// Прогрев движка вызывает физику шестнадцать раз без продвижения общего времени
		if ( gpGlobals->time <= activationTime || gpGlobals->frametime > 0.25f )
			return TRUE;
		runTime = gpGlobals->time;
		nextSample = runTime + 1;
		phase = CHECK_RUNNING;
		if ( !SpawnProbe() )
		{
			FinishMap("fakeclient_creation_failed");
			return TRUE;
		}
		CBasePlayer *player = BotPlayer();
		initialReady = everReady = Ready(player);
		everClear = everReady && ClearHull(player);
		Sample("spawn");
	}
	CBasePlayer *player = BotPlayer();
	if ( player )
	{
		int milliseconds = (int)(gpGlobals->frametime * 1000.0f + 0.5f);
		if ( milliseconds < 1 ) milliseconds = 1;
		if ( milliseconds > 255 ) milliseconds = 255;
		g_engfuncs.pfnRunPlayerMove(player->edict(), player->pev->v_angle, 0, 0, 0, 0, 0, (byte)milliseconds);
		player = BotPlayer();
		if ( Ready(player) )
		{
			everReady = TRUE;
			if ( ClearHull(player) )
				everClear = TRUE;
		}
		if ( player && everReady && !player->IsAlive() )
			passiveDeath = TRUE;
	}
	if ( !player )
	{
		FinishMap("probe_disappeared");
		return TRUE;
	}
	if ( gpGlobals->time >= runTime + MapDwell() )
		FinishMap();
	else if ( gpGlobals->time >= nextSample )
	{
		nextSample = gpGlobals->time + 1;
		Sample("periodic");
	}
	// Проверочный прогон не создаёт контрольные точки и не откатывает пассивно погибшего бота
	return TRUE;
}
#endif
