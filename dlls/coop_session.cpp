#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "gamerules.h"
#include "coop_campaign.h"
#include "coop_worldstate.h"
#include "coop_story.h"
#include "coop_session.h"
#include "coop_start.h"
#include <stdio.h>
#include <vector>
#include <ctype.h>
#include <time.h>
#include <memory>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static cvar_t coop_autosave = { "coop_autosave", "1", FCVAR_SERVER };
static cvar_t coop_autosave_interval = { "coop_autosave_interval", "120", FCVAR_SERVER };
static float g_nextAutoSave;
static float g_allDeadSince = -1;
static float g_nextSaveAttempt;
static float g_nextWipeAttempt;
static BOOL g_loadingSession;
static BOOL g_restoreQueued;
static BOOL g_sessionActive;
static char g_requestedLoad[49];
static EHANDLE g_checkpointTrigger;
static float g_nextTriggerSave;
static char g_saveDirectory[1024];
static unsigned int g_sessionId;
static unsigned int g_sessionGeneration;

struct CoopSessionHeader
{
	char magic[8];
	unsigned int version;
	unsigned int size[3];
	unsigned int checksum[3];
	unsigned int skill;
	unsigned int sessionId;
};

static const unsigned int COOP_SAVE_LIMIT = 192 * 1024 * 1024;
typedef BOOL (*CoopSectionFunction)(FILE *);

static BOOL IsCoopSession( void )
{
	return g_pGameRules && g_pGameRules->IsCoOp() && gpGlobals->maxClients > 1;
}

static BOOL ValidSaveName( const char *name )
{
	if ( !name || !*name || strlen(name) > 48 )
		return FALSE;
	for ( const unsigned char *p = (const unsigned char *)name; *p; ++p )
		if ( !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9') || *p == '_' || *p == '-') )
			return FALSE;
	return TRUE;
}

static BOOL PrepareSaveDirectory( void )
{
	char gamePath[768] = { 0 };
	GET_GAME_DIR( gamePath );
	if ( !gamePath[0] || strlen(gamePath) + 6 >= sizeof(g_saveDirectory) )
		return FALSE;
	snprintf( g_saveDirectory, sizeof(g_saveDirectory), "%s/save", gamePath );
#ifdef _WIN32
	_mkdir( g_saveDirectory );
#else
	mkdir( g_saveDirectory, 0700 );
#endif
	return TRUE;
}

static unsigned int SectionChecksum( const std::vector<unsigned char> &bytes )
{
	unsigned int crc = 0xffffffffu;
	for ( size_t i = 0; i < bytes.size(); ++i )
	{
		crc ^= bytes[i];
		for ( int bit = 0; bit < 8; ++bit )
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static FILE *OpenSectionFile( char *path, size_t capacity )
{
	snprintf( path, capacity, "%s/.coop_section.tmp", g_saveDirectory );
	return fopen( path, "w+b" );
}

class CoopSectionStream
{
public:
	CoopSectionStream() { stream = OpenSectionFile(path, sizeof(path)); }
	~CoopSectionStream()
	{
		if ( stream )
			fclose( stream );
		remove( path );
	}
	FILE *stream;
private:
	char path[1100];
	CoopSectionStream( const CoopSectionStream & );
	CoopSectionStream &operator=( const CoopSectionStream & );
};

class CoopSessionLoadStaging
{
public:
	~CoopSessionLoadStaging()
	{
		CoopCampaignCancelPlayers();
		CoopWorldCancel();
		CoopStoryCancel();
	}
};

static BOOL WriteSection( CoopSectionFunction writer, std::vector<unsigned char> &bytes )
{
	CoopSectionStream section;
	FILE *stream = section.stream;
	if ( !stream )
		return FALSE;
	BOOL result = writer( stream );
	long size = ftell( stream );
	if ( result && !ferror(stream) && size > 0 && (unsigned long)size <= COOP_SAVE_LIMIT )
	{
		bytes.resize( size );
		result = fseek( stream, 0, SEEK_SET ) == 0 && fread( &bytes[0], 1, bytes.size(), stream ) == bytes.size();
	}
	else
		result = FALSE;
	return result;
}

static BOOL ReadSection( CoopSectionFunction reader, const std::vector<unsigned char> &bytes )
{
	CoopSectionStream section;
	FILE *stream = section.stream;
	if ( !stream )
		return FALSE;
	BOOL result = !bytes.empty() && fwrite( &bytes[0], 1, bytes.size(), stream ) == bytes.size() &&
		fseek( stream, 0, SEEK_SET ) == 0;
	if ( result )
		result = reader(stream) && !ferror(stream) && ftell(stream) == (long)bytes.size();
	return result;
}

BOOL CoopSessionSave( const char *name )
{
	if ( !IsCoopSession() || !g_sessionActive || !ValidSaveName(name) || g_loadingSession || g_restoreQueued ||
		CoopWorldHasPendingTransition() || !PrepareSaveDirectory() )
		return FALSE;

	try
	{
		if ( !CoopWorldCapture( NULL, NULL, g_vecZero ) )
			return FALSE;

		std::vector<unsigned char> sections[3];
		if ( !WriteSection(CoopCampaignSavePlayers, sections[0]) ||
			!WriteSection(CoopWorldWrite, sections[1]) || !WriteSection(CoopStoryWrite, sections[2]) )
			return FALSE;

		CoopSessionHeader header = { 0 };
		memcpy( header.magic, "HLCOOP3", 7 );
		header.version = 1;
		header.sessionId = g_sessionId;
		header.skill = (unsigned int)CVAR_GET_FLOAT("skill");
		if ( header.skill < 1 || header.skill > 3 )
			header.skill = 2;
		unsigned int total = sizeof(header);
		for ( int i = 0; i < 3; ++i )
		{
			if ( sections[i].size() > COOP_SAVE_LIMIT - total )
				return FALSE;
			header.size[i] = (unsigned int)sections[i].size();
			header.checksum[i] = SectionChecksum( sections[i] );
			total += header.size[i];
		}

		char path[1152], temporary[1152];
		snprintf( path, sizeof(path), "%s/coop_%s.hcs", g_saveDirectory, name );
		snprintf( temporary, sizeof(temporary), "%s/coop_%s.hcs.tmp", g_saveDirectory, name );
		FILE *stream = fopen( temporary, "wb" );
		if ( !stream )
			return FALSE;
		BOOL result = fwrite( &header, 1, sizeof(header), stream ) == sizeof(header);
		for ( int i = 0; result && i < 3; ++i )
			result = fwrite( &sections[i][0], 1, sections[i].size(), stream ) == sections[i].size();
		if ( fflush(stream) != 0 )
			result = FALSE;
		if ( fclose(stream) != 0 )
			result = FALSE;
		if ( result )
		{
			// Готовый файл заменяет прежнее сохранение только после полной записи
#ifdef _WIN32
			result = MoveFileExA( temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != 0;
#else
			result = rename( temporary, path ) == 0;
#endif
		}
		if ( !result )
			remove( temporary );
		else
			ALERT( at_console, "Half-Life Co-op: session saved as %s on %s\n", name, STRING(gpGlobals->mapname) );
		return result;
	}
	catch ( ... )
	{
		ALERT( at_error, "Half-Life Co-op: unable to allocate session save buffer\n" );
		return FALSE;
	}
}

static BOOL LoadSession( const char *name, BOOL requireCurrentSession )
{
	if ( !g_sessionActive )
	{
		if ( requireCurrentSession || !ValidSaveName(name) )
			return FALSE;
		// Очередь команд движка может исполняться ещё во время загрузки BSP
		strncpy( g_requestedLoad, name, sizeof(g_requestedLoad) - 1 );
		g_requestedLoad[sizeof(g_requestedLoad) - 1] = 0;
		return TRUE;
	}
	if ( !ValidSaveName(name) || g_loadingSession || g_restoreQueued ||
		CoopWorldHasPendingTransition() || !PrepareSaveDirectory() )
		return FALSE;
	if ( gpGlobals->maxClients < 2 )
		return FALSE;

	try
	{
		char path[1152];
		snprintf( path, sizeof(path), "%s/coop_%s.hcs", g_saveDirectory, name );
		std::unique_ptr<FILE, int (*)(FILE *)> input( fopen(path, "rb"), fclose );
		FILE *stream = input.get();
		if ( !stream )
			return FALSE;
		CoopSessionHeader header = { 0 };
		BOOL valid = fread( &header, 1, sizeof(header), stream ) == sizeof(header) &&
			memcmp(header.magic, "HLCOOP3\0", 8) == 0 && header.version == 1 && header.skill >= 1 && header.skill <= 3;
		if ( requireCurrentSession && header.sessionId != g_sessionId )
			valid = FALSE;
		unsigned int total = sizeof(header);
		for ( int i = 0; valid && i < 3; ++i )
		{
			valid = header.size[i] > 0 && header.size[i] <= COOP_SAVE_LIMIT - total;
			if ( valid )
				total += header.size[i];
		}
		if ( valid )
		{
			valid = fseek(stream, 0, SEEK_END) == 0 && ftell(stream) == (long)total &&
				fseek(stream, sizeof(header), SEEK_SET) == 0;
		}
		std::vector<unsigned char> sections[3];
		for ( int i = 0; valid && i < 3; ++i )
		{
			sections[i].resize( header.size[i] );
			valid = fread(&sections[i][0], 1, sections[i].size(), stream) == sections[i].size() &&
				SectionChecksum(sections[i]) == header.checksum[i];
		}
		input.reset();
		if ( !valid )
		{
			ALERT( at_console, "Half-Life Co-op: session header or checksum is invalid\n" );
			return FALSE;
		}
		CoopSectionFunction validators[3] = { CoopCampaignValidatePlayers, CoopWorldValidate, CoopStoryValidate };
		const char *sectionNames[3] = { "players", "world", "story" };
		for ( int i = 0; i < 3; ++i )
		{
			if ( !ReadSection(validators[i], sections[i]) )
			{
				ALERT( at_console, "Half-Life Co-op: invalid %s section in session save\n", sectionNames[i] );
				return FALSE;
			}
		}

		// Кандидаты всех разделов готовы до изменения текущей сессии
		CoopSessionLoadStaging staging;
		if ( !ReadSection(CoopWorldStage, sections[1]) || !ReadSection(CoopCampaignStagePlayers, sections[0]) ||
			!ReadSection(CoopStoryStage, sections[2]) )
			return FALSE;
		CoopWorldCommit();
		CoopStartCancelQueued( TRUE );
		CoopCampaignCommitPlayers();
		CoopStoryCommit();
		const char *map = CoopCampaignLoadedMap();
		if ( !map || !*map )
			return FALSE;
		g_loadingSession = TRUE;
		g_restoreQueued = TRUE;
		g_sessionId = header.sessionId;
		CVAR_SET_FLOAT( "skill", (float)header.skill );
		ALERT( at_console, "Half-Life Co-op: loading session %s on %s\n", name, map );
		CHANGE_LEVEL( (char *)map, NULL );
		return TRUE;
	}
	catch ( ... )
	{
		ALERT( at_error, "Half-Life Co-op: invalid or oversized session save\n" );
		return FALSE;
	}
}

BOOL CoopSessionSaveCheckpoint( void )
{
	return CoopSessionSave( "checkpoint" );
}

void CoopSessionRequestCheckpoint( CBaseEntity *trigger )
{
	if ( trigger && !g_checkpointTrigger )
		g_checkpointTrigger = trigger;
}

BOOL CoopSessionRestoreCheckpoint( void )
{
	return LoadSession( "checkpoint", TRUE );
}

BOOL CoopSessionLoad( const char *name )
{
	return LoadSession( name, FALSE );
}

void CoopSessionReset( void )
{
	g_nextAutoSave = 0;
	g_nextSaveAttempt = 0;
	g_nextWipeAttempt = 0;
	g_allDeadSince = -1;
	g_loadingSession = FALSE;
	g_restoreQueued = FALSE;
	g_sessionActive = FALSE;
	g_requestedLoad[0] = 0;
	g_checkpointTrigger = NULL;
	g_nextTriggerSave = 0;
	// Контрольная точка другой новой игры не должна подменять текущую кампанию
	g_sessionId = (unsigned int)time(NULL) ^ (++g_sessionGeneration * 2654435761u);
}

void CoopSessionServerActivate( void )
{
	g_checkpointTrigger = NULL;
	g_nextTriggerSave = 0;
	g_sessionActive = TRUE;
	g_loadingSession = FALSE;
	g_restoreQueued = FALSE;
	g_nextAutoSave = gpGlobals->time + 8;
	g_nextSaveAttempt = gpGlobals->time + 8;
	g_nextWipeAttempt = gpGlobals->time;
	g_allDeadSince = -1;
}

void CoopSessionServerDeactivate( void )
{
	g_sessionActive = FALSE;
}

void CoopSessionThink( void )
{
	if ( !IsCoopSession() || g_loadingSession || g_restoreQueued )
		return;
	int connected = 0, alive = 0, dead = 0;
	BOOL safe = !CoopStoryIsBusy();
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBaseEntity *entity = UTIL_PlayerByIndex( i );
		if ( !entity || !entity->IsPlayer() || entity->IsDormant() || !STRING(entity->pev->netname)[0] )
			continue;
		++connected;
		CBasePlayer *player = (CBasePlayer *)entity;
		if ( player->IsAlive() && !player->m_fCoopSpawnPending )
		{
			++alive;
			if ( !FBitSet(player->pev->flags, FL_ONGROUND) || FBitSet(player->pev->flags, FL_FROZEN) || player->pev->waterlevel >= 3 ||
				player->pev->velocity.Length() > 100 )
				safe = FALSE;
		}
		else
		{
			safe = FALSE;
			if ( !player->m_fCoopSpawnPending && !player->IsAlive() )
				++dead;
		}
	}
	if ( connected && dead == connected )
	{
		if ( g_allDeadSince < 0 )
			g_allDeadSince = gpGlobals->time;
		if ( gpGlobals->time - g_allDeadSince > 4 && gpGlobals->time >= g_nextWipeAttempt )
		{
			g_nextWipeAttempt = gpGlobals->time + 8;
			if ( !CoopSessionRestoreCheckpoint() )
				CoopCampaignRestartCheckpoint( NULL );
		}
	}
	else
		g_allDeadSince = -1;
	if ( alive && g_requestedLoad[0] )
	{
		char name[sizeof(g_requestedLoad)];
		strcpy( name, g_requestedLoad );
		g_requestedLoad[0] = 0;
		if ( !CoopSessionLoad(name) )
			ALERT( at_console, "Half-Life Co-op: requested session could not be loaded\n" );
		return;
	}
	if ( alive && g_checkpointTrigger && gpGlobals->time >= g_nextTriggerSave )
	{
		g_nextTriggerSave = gpGlobals->time + 2;
		CBaseEntity *trigger = g_checkpointTrigger;
		// Триггер исчезает из сохранённой карты только после успешной записи
		trigger->pev->flags |= FL_KILLME;
		BOOL saved = CoopSessionSaveCheckpoint();
		trigger->pev->flags &= ~FL_KILLME;
		if ( saved )
		{
			UTIL_Remove( trigger );
			g_checkpointTrigger = NULL;
			CoopSessionSave( "autosave" );
			g_nextAutoSave = gpGlobals->time + max( coop_autosave_interval.value, 30 );
		}
		return;
	}

	if ( alive && safe && coop_autosave.value != 0 && gpGlobals->time >= g_nextAutoSave && gpGlobals->time >= g_nextSaveAttempt )
	{
		g_nextSaveAttempt = gpGlobals->time + 8;
		if ( CoopSessionSaveCheckpoint() )
		{
			CoopSessionSave( "autosave" );
			float interval = coop_autosave_interval.value;
			if ( interval < 30 ) interval = 30;
			g_nextAutoSave = gpGlobals->time + interval;
		}
	}
}

static void CoopSaveCommand( void )
{
	const char *name = CMD_ARGC() > 1 ? CMD_ARGV(1) : "quick";
	if ( !CoopSessionSave(name) )
		ALERT( at_console, "Half-Life Co-op: save failed (use coop_save name in a cooperative session)\n" );
}

static void CoopLoadCommand( void )
{
	const char *name = CMD_ARGC() > 1 ? CMD_ARGV(1) : "quick";
	if ( !CoopSessionLoad(name) )
		ALERT( at_console, "Half-Life Co-op: save missing, damaged or incompatible\n" );
}

static void CoopStatusCommand( void )
{
	ALERT( at_console, "Half-Life Co-op: map=%s cooperative=%d slots=%d arrival=%d loading=%d time=%.2f\n",
		STRING(gpGlobals->mapname), IsCoopSession(), gpGlobals->maxClients, CoopCampaignHasArrival(), g_loadingSession, gpGlobals->time );
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBaseEntity *entity = UTIL_PlayerByIndex( i );
		if ( !entity || !entity->IsPlayer() || entity->IsDormant() || !STRING(entity->pev->netname)[0] )
			continue;
		CBasePlayer *player = (CBasePlayer *)entity;
		ALERT( at_console, "Co-op player %d: health=%.0f armor=%.0f pending=%d origin=(%.1f %.1f %.1f) weapons=%u\n",
			i, player->pev->health, player->pev->armorvalue, player->m_fCoopSpawnPending,
			player->pev->origin.x, player->pev->origin.y, player->pev->origin.z, (unsigned int)player->pev->weapons );
	}
	CBaseEntity *train = UTIL_FindEntityByString( NULL, "globalname", "intro_train" );
	if ( train )
		ALERT( at_console, "Co-op train: origin=(%.1f %.1f %.1f) velocity=(%.1f %.1f %.1f) speed=%.1f ltime=%.2f nextthink=%.2f\n",
			train->pev->origin.x, train->pev->origin.y, train->pev->origin.z,
			train->pev->velocity.x, train->pev->velocity.y, train->pev->velocity.z,
			train->pev->speed, train->pev->ltime, train->pev->nextthink );
}

void CoopSessionInit( void )
{
	CoopSessionReset();
	CVAR_REGISTER( &coop_autosave );
	CVAR_REGISTER( &coop_autosave_interval );
	g_engfuncs.pfnAddServerCommand( (char *)"coop_save", CoopSaveCommand );
	g_engfuncs.pfnAddServerCommand( (char *)"coop_load", CoopLoadCommand );
	g_engfuncs.pfnAddServerCommand( (char *)"coop_status", CoopStatusCommand );
}

BOOL CoopSessionClientCommand( CBasePlayer *player, const char *command )
{
	if ( strcmp(command, "coop_save") && strcmp(command, "coop_load") && strcmp(command, "coop_status") )
		return FALSE;
	if ( !IsCoopSession() )
		return FALSE;
	if ( !strcmp(command, "coop_status") )
	{
		ClientPrint( player->pev, HUD_PRINTCONSOLE, UTIL_VarArgs("Half-Life Co-op: %s, %d slots\n", STRING(gpGlobals->mapname), gpGlobals->maxClients) );
		return TRUE;
	}
	if ( player->entindex() != 1 )
	{
		ClientPrint( player->pev, HUD_PRINTCONSOLE, "Only the host can save or load the whole session\n" );
		return TRUE;
	}
	const char *name = CMD_ARGC() > 1 ? CMD_ARGV(1) : "quick";
	BOOL result = !strcmp(command, "coop_save") ? CoopSessionSave(name) : CoopSessionLoad(name);
	ClientPrint( player->pev, HUD_PRINTCONSOLE, result ? "Cooperative session command accepted\n" : "Unable to save or load the cooperative session\n" );
	return TRUE;
}
