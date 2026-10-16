#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <time.h>
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "weapons.h"
#include "player.h"
#include "client.h"
#include "gamerules.h"
#include "trains.h"
#include "saverestore.h"
#include "coop_campaign.h"
#include "coop_levels.h"
#include "coop_worldstate.h"
#include "coop_story.h"
#include "coop_session.h"
#include "coop_start.h"
#include "coop_check.h"
#include "coop_transition_check.h"

#ifdef HLCOOP_MAPCHECK
namespace
{
const int MAX_EDGES = 512;
const int COLUMNS = 37;
const char *const NAMES[2] = { "hctcheck_alpha", "hctcheck_beta" };

enum Phase { WAIT_SOURCE, SOURCE_WARMUP, SOURCE_READY, WAIT_DESTINATION, DESTINATION_WARMUP, DESTINATION_READY, STABILITY };
enum ReversePhase { REVERSE_OFF, REVERSE_ARMED, REVERSE_ENTER, REVERSE_HOLD, REVERSE_LEAVE, REVERSE_OUTSIDE, REVERSE_REENTER, REVERSE_RETURN, REVERSE_STABLE };

struct Edge
{
	int id, bspIndex, spawnflags;
	char source[32], destination[32], model[32], target[128], landmark[64], changeTarget[128];
	float changeDelay;
	Vector sourceLandmark, destinationLandmark, mins, maxs, origin[2], angles[2];
	BOOL duck[2], use;
};

struct WeaponState { char name[64]; int clip; };
struct AmmoState { char name[32]; int count; };
struct State
{
	float health, armor;
	BOOL suit, longJump;
	int flash, weaponCount, ammoCount;
	WeaponState weapons[32];
	AmmoState ammo[MAX_AMMO_SLOTS];
};

struct Probe
{
	int slot, user, suppressedViews, suppressedCrosshair, musicPlay, musicStop;
	char auth[64], sourceRawAuth[64];
	BOOL manualSignon, observed, personal, equal, transformedEqual, positionOK, passiveDeath, storyFrozen, floorSupported, farFromLeader, nameRecovered, flagsRecovered, sourceFloor, sourceParked, spawnObserved, mapSpawnKnown, contentsClear, firstTrainFloor, detached;
	Vector spawnOrigin, sourceOrigin, expectedOrigin, firstOrigin, mapSpawnReference;
	char mapSpawnClass[32];
	int mapSpawnEdict;
	float firstReadyAt;
	State sourceState, firstState;
};

Edge edges[MAX_EDGES];
Probe probes[2];
int edgeCount, firstEdge, requestedCount, completed, passed, failed, setupFailed, limitations, warnings;
BOOL active, farMode, requested, unexpectedActivation;
FILE *report;
unsigned long runId;
Phase phase;
float activatedAt, phaseTime, nextSample, dwell;
int triggerIndex, sourceActivator;
BOOL trainSourceVerified, trainMovementVerified;
BOOL trainDwellExtended;
float trainPathRecoveredAt;
BOOL ambushSourceVerified;
BOOL xenGateTouched, xenPortalTouched, xenSourceVerified;
int xenStage, xenGateEdict, xenPortalEdict;
float xenGateTouchAt, xenPortalTouchAt, xenStageAt;
Vector ambushBetaStart;
BOOL destinationStripProducerObserved, destinationStripUseObserved, destinationStripObserved;
float destinationStripProducerTime, destinationStripUseTime;
int destinationStripProducerEdict, destinationStripCallerEdict, destinationStripEventEdict;
Vector trainSetupOrigin;
char observedActivation[32];
typedef const char *(*AuthIdFunction)(edict_t *);
AuthIdFunction originalAuthId;
typedef void (*SetViewFunction)(const edict_t *, const edict_t *);
SetViewFunction originalSetView;
typedef void (*CrosshairAngleFunction)(const edict_t *, float, float);
CrosshairAngleFunction originalCrosshairAngle;
int suppressedViews, suppressedCrosshair, musicPlay, musicStop;
BOOL pendingReleaseActive, pendingReleaseAttempted, pendingReleaseDuck;
float pendingReleaseUntil;
Vector pendingReleaseGoal;
BOOL reverseEnabled, reverseReturning, reverseOutwardVerified, reverseEarlyTouch, reverseHoldVerified, reverseExitVerified, reverseReturnRequested, reverseMovementUnsafe;
ReversePhase reversePhase;
int reverseTriggerIndex;
float reverseSpawnAt, reversePhaseAt, reverseStartedAt, reverseEarlyTouchAt;
Vector reverseInside, reverseOutside;

void FinishReverse( const char *status, const char *reason );

Edge &Current( void ) { return edges[firstEdge + completed]; }

BOOL AllowedServer( void )
{
	if ( !IS_DEDICATED_SERVER() ) return FALSE;
	char path[1024];
	GET_GAME_DIR(path);
	path[sizeof(path) - 1] = 0;
	size_t length = strlen(path);
	while ( length && (path[length - 1] == '/' || path[length - 1] == '\\') ) path[--length] = 0;
	const char *name = path + length;
	while ( name > path && name[-1] != '/' && name[-1] != '\\' ) --name;
	return !stricmp(name, "hlcoop_tc") || !stricmp(name, "hlcoop_tc_2") ||
		!stricmp(name, "hlcoop_tc_3") || !stricmp(name, "hlcoop_tc_4");
}

const char *RawAuth( edict_t *entity )
{
	return originalAuthId ? originalAuthId(entity) : GETPLAYERAUTHID(entity);
}

const char *LANAuthAdapter( edict_t *entity )
{
	const char *raw = originalAuthId ? originalAuthId(entity) : "";
	if ( active && entity && raw && !strcmp(raw, "BOT") && AllowedServer() && CVAR_GET_FLOAT("sv_lan") == 1 )
		for ( int i = 0; i < 2; ++i )
			if ( probes[i].slot > 0 && probes[i].slot <= gpGlobals->maxClients && entity == INDEXENT(probes[i].slot) &&
				GETPLAYERUSERID(entity) == probes[i].user && !strcmp(STRING(entity->v.netname), NAMES[i]) )
				return "STEAM_ID_LAN";
	return raw ? raw : "";
}

void Copy( char *out, size_t size, const char *value )
{
	strncpy(out, value ? value : "", size - 1);
	out[size - 1] = 0;
}

BOOL Identifier( const char *value )
{
	if ( !value || !*value || strlen(value) >= 32 ) return FALSE;
	for ( const char *p = value; *p; ++p )
		if ( !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') ) return FALSE;
	return TRUE;
}

BOOL Integer( const char *value, int minimum, int maximum, int &out )
{
	char *end;
	long number = strtol(value, &end, 10);
	if ( !*value || *end || number < minimum || number > maximum ) return FALSE;
	out = (int)number;
	return TRUE;
}

BOOL Number( const char *value, float &out, BOOL allowMissing = FALSE )
{
	char *end;
	double number = strtod(value, &end);
	if ( !*value || *end || (!_finite(number) && !allowMissing) || (_finite(number) && (number < -131072 || number > 131072)) ) return FALSE;
	out = (float)number;
	return TRUE;
}

BOOL ReadVector( char **fields, int start, Vector &out, BOOL allowMissing = FALSE )
{
	return Number(fields[start], out.x, allowMissing) && Number(fields[start + 1], out.y, allowMissing) && Number(fields[start + 2], out.z, allowMissing);
}

BOOL Finite( const Vector &value ) { return _finite(value.x) && _finite(value.y) && _finite(value.z); }
BOOL MapSpawnFallback( void ) { return !Finite(Current().destinationLandmark); }

BOOL NativeDestinationStripCase( void )
{
	return Current().id == 219 && !strcmp(Current().source, "c4a3") && !strcmp(Current().destination, "c5a1") &&
		!strcmp(Current().target, "change_to_c5a1") && !strcmp(Current().landmark, "c4a3toc5a1");
}

BOOL NativeDestinationStripWindow( void )
{
	return active && requested && NativeDestinationStripCase() && !strcmp(STRING(gpGlobals->mapname), "c5a1") &&
		(phase == DESTINATION_WARMUP || phase == DESTINATION_READY) && !probes[0].observed && !probes[1].observed;
}

BOOL NativeTrainRide( void )
{
	if ( farMode ) return FALSE;
	const Edge &edge = Current();
	return (!strcmp(edge.source, "c0a0") && !strcmp(edge.destination, "c0a0a")) ||
		(!strcmp(edge.source, "c0a0a") && !strcmp(edge.destination, "c0a0b")) ||
		(!strcmp(edge.source, "c0a0b") && !strcmp(edge.destination, "c0a0c")) ||
		(!strcmp(edge.source, "c0a0c") && !strcmp(edge.destination, "c0a0d")) ||
		(!strcmp(edge.source, "c0a0d") && !strcmp(edge.destination, "c0a0e"));
}

BOOL NativeAmbush( void )
{
	return !strcmp(Current().source, "c2a3d") && !strcmp(Current().destination, "c2a3e") &&
		!strcmp(Current().target, "gotodrag") && !strcmp(Current().landmark, "c2a3de");
}

BOOL NativeXenPortal( void )
{
	return !farMode && Current().id == 208 && !strcmp(Current().source, "c4a1a") &&
		!strcmp(Current().destination, "c4a1b") && !strcmp(Current().target, "change") && !strcmp(Current().landmark, "c4a1b");
}

BOOL AmbushBoxIntersects( CBasePlayer *player, const Vector &mins, const Vector &maxs )
{
	return player && player->pev->absmin.x <= maxs.x && player->pev->absmax.x >= mins.x &&
		player->pev->absmin.y <= maxs.y && player->pev->absmax.y >= mins.y &&
		player->pev->absmin.z <= maxs.z && player->pev->absmax.z >= mins.z;
}

BOOL LoadCatalog( void )
{
	char directory[1024], path[1100], line[4096];
	GET_GAME_DIR(directory);
	_snprintf(path, sizeof(path), "%s/transition-edges.tsv", directory);
	path[sizeof(path) - 1] = 0;
	FILE *file = fopen(path, "rb");
	if ( !file ) return FALSE;
	edgeCount = 0;
	BOOL valid = TRUE;
	while ( valid && fgets(line, sizeof(line), file) )
	{
		if ( *line == '#' || *line == '\r' || *line == '\n' ) continue;
		if ( !strchr(line, '\n') && !feof(file) ) { valid = FALSE; break; }
		line[strcspn(line, "\r\n")] = 0;
		char *fields[COLUMNS];
		int count = 1;
		fields[0] = line;
		for ( char *p = line; *p; ++p )
			if ( *p == '\t' )
			{
				*p = 0;
				if ( count == COLUMNS ) { valid = FALSE; break; }
				fields[count++] = p + 1;
			}
		if ( !valid || count != COLUMNS || edgeCount == MAX_EDGES ) { valid = FALSE; break; }
		Edge &edge = edges[edgeCount];
		int duck0, duck1;
		valid = Integer(fields[0], edgeCount, edgeCount, edge.id) && Identifier(fields[1]) && Identifier(fields[2]) &&
			Integer(fields[3], 0, 65535, edge.bspIndex) && Integer(fields[7], 0, 0x7fffffff, edge.spawnflags) &&
			Number(fields[9], edge.changeDelay) && ReadVector(fields, 10, edge.sourceLandmark, TRUE) &&
			ReadVector(fields, 13, edge.destinationLandmark, TRUE) && ReadVector(fields, 16, edge.mins) &&
			ReadVector(fields, 19, edge.maxs) && ReadVector(fields, 22, edge.origin[0], TRUE) &&
			Integer(fields[25], 0, 1, duck0) && ReadVector(fields, 26, edge.angles[0]) &&
			ReadVector(fields, 29, edge.origin[1], TRUE) && Integer(fields[32], 0, 1, duck1) && ReadVector(fields, 33, edge.angles[1]) &&
			(!strcmp(fields[36], "touch") || !strcmp(fields[36], "use")) &&
			strlen(fields[4]) < sizeof(edge.model) && strlen(fields[5]) < sizeof(edge.target) &&
			strlen(fields[6]) < sizeof(edge.landmark) && strlen(fields[8]) < sizeof(edge.changeTarget);
		if ( !valid ) break;
		Copy(edge.source, sizeof(edge.source), fields[1]); Copy(edge.destination, sizeof(edge.destination), fields[2]);
		Copy(edge.model, sizeof(edge.model), fields[4]); Copy(edge.target, sizeof(edge.target), !strcmp(fields[5], "-") ? "" : fields[5]);
		Copy(edge.landmark, sizeof(edge.landmark), fields[6]); Copy(edge.changeTarget, sizeof(edge.changeTarget), !strcmp(fields[8], "-") ? "" : fields[8]);
		edge.duck[0] = duck0; edge.duck[1] = duck1; edge.use = !strcmp(fields[36], "use");
		++edgeCount;
	}
	valid = valid && !ferror(file) && edgeCount > 0;
	fclose(file);
	return valid;
}

void String( const char *value )
{
	fputc('"', report);
	for ( const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p )
		if ( *p == '"' || *p == '\\' ) fprintf(report, "\\%c", *p);
		else if ( *p < 32 ) fprintf(report, "\\u%04x", *p);
		else fputc(*p, report);
	fputc('"', report);
}

void Vec( const Vector &value )
{
	if ( Finite(value) ) fprintf(report, "[%.4f,%.4f,%.4f]", value.x, value.y, value.z);
	else fputs("null", report);
}

void Begin( const char *type ) { fprintf(report, "{\"run\":%lu,\"type\":", runId); String(type); }
void End( void ) { fputs("}\n", report); fflush(report); }
void EdgeFields( void )
{
	Edge &edge = Current();
	fprintf(report, ",\"edge_id\":%d,\"source_map\":", edge.id); String(edge.source);
	fputs(",\"destination_map\":", report); String(edge.destination);
	fputs(",\"observed_map\":", report); String(STRING(gpGlobals->mapname));
	fputs(",\"landmark\":", report); String(edge.landmark);
	fputs(",\"position_verification_mode\":", report); String(MapSpawnFallback() ? "map_spawn_fallback" : "landmark_relative");
	fprintf(report, ",\"game_time\":%.4f", gpGlobals->time);
	if ( reverseEnabled )
	{
		fputs(",\"aux_reverse_guard\":true,\"aux_leg\":", report); String(reverseReturning ? "return" : "outward");
		fputs(",\"aux_expected_map\":", report); String(reverseReturning ? edge.source : edge.destination);
	}
}

edict_t *Identity( int index )
{
	Probe &probe = probes[index];
	if ( probe.slot < 1 || probe.slot > gpGlobals->maxClients ) return NULL;
	edict_t *entity = INDEXENT(probe.slot);
	if ( !entity || entity->free || GETPLAYERUSERID(entity) != probe.user ||
		strcmp(GETPLAYERAUTHID(entity), probe.auth) || strcmp(STRING(entity->v.netname), NAMES[index]) ) return NULL;
	return entity;
}

CBasePlayer *Player( int index )
{
	edict_t *entity = Identity(index);
	if ( !entity || !entity->pvPrivateData || !(entity->v.flags & FL_FAKECLIENT) ) return NULL;
	CBaseEntity *player = CBaseEntity::Instance(entity);
	return player && player->IsPlayer() ? (CBasePlayer *)player : NULL;
}

BOOL Ready( CBasePlayer *player )
{
	return player && player->IsAlive() && !player->IsDormant() && !player->IsObserver() &&
		!player->m_fCoopSpawnPending && player->pev->solid == SOLID_SLIDEBOX && !(player->pev->flags & FL_FROZEN);
}

BOOL Hull( CBasePlayer *player )
{
	if ( !player || !Finite(player->pev->origin) ) return FALSE;
	TraceResult trace;
	UTIL_TraceHull(player->pev->origin, player->pev->origin, dont_ignore_monsters,
		(player->pev->flags & FL_DUCKING) ? head_hull : human_hull, player->edict(), &trace);
	return !trace.fStartSolid && !trace.fAllSolid;
}

BOOL Floor( CBasePlayer *player )
{
	TraceResult trace;
	UTIL_TraceHull(player->pev->origin, player->pev->origin - Vector(0, 0, 72), ignore_monsters,
		(player->pev->flags & FL_DUCKING) ? head_hull : human_hull, player->edict(), &trace);
	return !trace.fStartSolid && !trace.fAllSolid && trace.flFraction < 1 && trace.vecPlaneNormal.z >= 0.7f;
}

CFuncTrackTrain *IntroTrain( void )
{
	CBaseEntity *entity = UTIL_FindEntityByString(NULL, "globalname", "intro_train");
	return entity ? CFuncTrackTrain::Instance(entity->edict()) : NULL;
}

BOOL TrainFloor( CBasePlayer *player, CFuncTrackTrain *train )
{
	if ( !player || !train ) return FALSE;
	TraceResult trace;
	UTIL_TraceHull(player->pev->origin, player->pev->origin - Vector(0, 0, 8), ignore_monsters,
		(player->pev->flags & FL_DUCKING) ? head_hull : human_hull, player->edict(), &trace);
	return !trace.fStartSolid && !trace.fAllSolid && trace.flFraction < 1 &&
		trace.vecPlaneNormal.z >= 0.7f && trace.pHit == train->edict();
}

BOOL TrainPathLive( void )
{
	CFuncTrackTrain *train = IntroTrain();
	if ( !train || !train->m_ppath ) return FALSE;
	CBaseEntity *track = NULL;
	while ( (track = UTIL_FindEntityByClassname(track, "path_track")) != NULL )
		if ( track == train->m_ppath ) return !track->IsDormant() && !(track->pev->flags & FL_KILLME);
	return FALSE;
}

void TrainFields( void )
{
	CFuncTrackTrain *train = IntroTrain();
	fputs(",\"train\":", report);
	if ( !train ) { fputs("null", report); return; }
	fprintf(report, "{\"edict\":%d,\"origin\":", train->entindex()); Vec(train->pev->origin);
	fputs(",\"angles\":", report); Vec(train->pev->angles);
	fputs(",\"velocity\":", report); Vec(train->pev->velocity);
	fputs(",\"model\":", report); String(STRING(train->pev->model));
	fprintf(report, ",\"model_index\":%d,\"expected_model_index\":%d,\"speed\":%.4f,\"ppath_present\":%s",
		train->pev->modelindex, MODEL_INDEX(STRING(train->pev->model)), train->pev->speed, train->m_ppath ? "true" : "false");
	// Во время восстановления путь может ещё ожидать штатного NearestPath
	CBaseEntity *track = NULL;
	while ( (track = UTIL_FindEntityByClassname(track, "path_track")) != NULL )
		if ( track == train->m_ppath ) break;
	fprintf(report, ",\"ppath_live\":%s,\"ppath_edict\":%d,\"ppath_targetname\":", track ? "true" : "false", track ? track->entindex() : -1);
	String(track ? STRING(track->pev->targetname) : "");
	fputs(",\"passenger_floor_hits\":[", report);
	for ( int i = 0; i < 2; ++i ) fprintf(report, "%s%s", i ? "," : "", TrainFloor(Player(i), train) ? "true" : "false");
	fputs("]}", report);
}

void Capture( CBasePlayer *player, State &state )
{
	memset(&state, 0, sizeof(state));
	state.health = player->pev->health; state.armor = player->pev->armorvalue;
	state.suit = (player->pev->weapons & (1 << WEAPON_SUIT)) != 0;
	state.longJump = player->m_fLongJump; state.flash = player->m_iFlashBattery;
	for ( int slot = 0; slot < MAX_ITEM_TYPES; ++slot )
		for ( CBasePlayerItem *item = player->m_rgpPlayerItems[slot]; item && state.weaponCount < 32; item = item->m_pNext )
		{
			WeaponState &weapon = state.weapons[state.weaponCount++];
			Copy(weapon.name, sizeof(weapon.name), STRING(item->pev->classname));
			weapon.clip = item->GetWeaponPtr() ? ((CBasePlayerWeapon *)item)->m_iClip : WEAPON_NOCLIP;
		}
	for ( int i = 0; i < MAX_AMMO_SLOTS; ++i )
		if ( player->m_rgAmmo[i] && CBasePlayerItem::AmmoInfoArray[i].pszName )
		{
			AmmoState &ammo = state.ammo[state.ammoCount++];
			Copy(ammo.name, sizeof(ammo.name), CBasePlayerItem::AmmoInfoArray[i].pszName); ammo.count = player->m_rgAmmo[i];
		}
}

BOOL PersonalStateEqual( const State &a, const State &b )
{
	return fabs(a.health - b.health) <= 0.01f && fabs(a.armor - b.armor) <= 0.01f && a.suit == b.suit &&
		a.longJump == b.longJump && a.flash == b.flash;
}

BOOL Equal( const State &a, const State &b )
{
	if ( !PersonalStateEqual(a, b) || a.weaponCount != b.weaponCount || a.ammoCount != b.ammoCount ) return FALSE;
	for ( int i = 0; i < a.weaponCount; ++i )
	{
		BOOL found = FALSE;
		for ( int j = 0; j < b.weaponCount; ++j )
			if ( !strcmp(a.weapons[i].name, b.weapons[j].name) && a.weapons[i].clip == b.weapons[j].clip ) found = TRUE;
		if ( !found ) return FALSE;
	}
	for ( int i = 0; i < a.ammoCount; ++i )
	{
		BOOL found = FALSE;
		for ( int j = 0; j < b.ammoCount; ++j )
			if ( !strcmp(a.ammo[i].name, b.ammo[j].name) && a.ammo[i].count == b.ammo[j].count ) found = TRUE;
		if ( !found ) return FALSE;
	}
	return TRUE;
}

BOOL NativeStripStateEqual( const State &source, const State &restored )
{
	return NativeDestinationStripCase() && destinationStripObserved && source.weaponCount > 0 && source.ammoCount > 0 &&
		restored.weaponCount == 0 && restored.ammoCount == 0 && PersonalStateEqual(source, restored);
}

void StateFields( const State &state )
{
	fprintf(report, "{\"health\":%.3f,\"armor\":%.3f,\"suit\":%s,\"long_jump\":%s,\"flash\":%d,\"weapons\":[",
		state.health, state.armor, state.suit ? "true" : "false", state.longJump ? "true" : "false", state.flash);
	for ( int i = 0; i < state.weaponCount; ++i )
	{
		if ( i ) fputc(',', report);
		fputs("{\"name\":", report); String(state.weapons[i].name); fprintf(report, ",\"clip\":%d}", state.weapons[i].clip);
	}
	fputs("],\"ammo\":[", report);
	for ( int i = 0; i < state.ammoCount; ++i )
	{
		if ( i ) fputc(',', report);
		fputs("{\"name\":", report); String(state.ammo[i].name); fprintf(report, ",\"count\":%d}", state.ammo[i].count);
	}
	fputs("]}", report);
}

void BlockerFields( CBasePlayer *player, const Vector &origin )
{
	fprintf(report, "{\"point_contents\":%d,\"hulls\":[", UTIL_PointContents(origin));
	for ( int crouch = 0; crouch < 2; ++crouch )
	{
		TraceResult trace;
		UTIL_TraceHull(origin, origin, dont_ignore_monsters, crouch ? head_hull : human_hull, player->edict(), &trace);
		fprintf(report, "%s{\"duck\":%s,\"start_solid\":%s,\"all_solid\":%s,\"fraction\":%.4f,\"hit_edict\":%d,\"hit_class\":",
			crouch ? "," : "", crouch ? "true" : "false", trace.fStartSolid ? "true" : "false",
			trace.fAllSolid ? "true" : "false", trace.flFraction, trace.pHit ? ENTINDEX(trace.pHit) : -1);
		String(trace.pHit ? STRING(trace.pHit->v.classname) : "");
		fputs(",\"hit_model\":", report); String(trace.pHit ? STRING(trace.pHit->v.model) : "");
		fputs(",\"hit_globalname\":", report); String(trace.pHit ? STRING(trace.pHit->v.globalname) : "");
		UTIL_TraceHull(origin, origin - Vector(0, 0, 72), ignore_monsters, crouch ? head_hull : human_hull, player->edict(), &trace);
		fprintf(report, ",\"floor_start_solid\":%s,\"floor_fraction\":%.4f,\"floor_normal_z\":%.4f}",
			trace.fStartSolid ? "true" : "false", trace.flFraction, trace.vecPlaneNormal.z);
	}
	fputs("]}", report);
}

void Sample( const char *label )
{
	Begin("sample"); EdgeFields(); fputs(",\"phase\":", report); String(label);
	fprintf(report, ",\"elapsed\":%.4f,\"players\":[", gpGlobals->time - phaseTime);
	for ( int i = 0; i < 2; ++i )
	{
		Probe &probe = probes[i]; CBasePlayer *player = Player(i);
		fprintf(report, "%s{\"probe\":%d,\"slot\":%d,\"userid\":%d,\"name\":", i ? "," : "", i, probe.slot, probe.user); String(NAMES[i]);
		fputs(",\"auth\":", report); String(probe.auth);
		edict_t *raw = probe.slot >= 1 && probe.slot <= gpGlobals->maxClients ? INDEXENT(probe.slot) : NULL;
		fprintf(report, ",\"raw_edict_exists\":%s", raw ? "true" : "false");
		if ( raw )
		{
			fprintf(report, ",\"raw_free\":%s,\"raw_userid\":%d,\"raw_flags\":%d,\"raw_private_data\":%s,\"raw_serial\":%d,\"raw_auth\":",
				raw->free ? "true" : "false", GETPLAYERUSERID(raw), raw->v.flags, raw->pvPrivateData ? "true" : "false", raw->serialnumber);
			const char *rawAuth = RawAuth(raw);
			const char *normalizedAuth = GETPLAYERAUTHID(raw);
			String(rawAuth);
			fputs(",\"normalized_auth\":", report); String(normalizedAuth);
			fputs(",\"source_raw_auth\":", report); String(probe.sourceRawAuth);
			fprintf(report, ",\"auth_normalized\":%s,\"raw_auth_changed\":%s,\"slot_userid_matches\":%s",
				rawAuth && normalizedAuth && strcmp(rawAuth, normalizedAuth) ? "true" : "false",
				rawAuth && strcmp(rawAuth, probe.sourceRawAuth) ? "true" : "false", GETPLAYERUSERID(raw) == probe.user ? "true" : "false");
			fputs(",\"raw_netname\":", report); String(STRING(raw->v.netname));
		}
		fprintf(report, ",\"name_recovered\":%s,\"flags_recovered\":%s", probe.nameRecovered ? "true" : "false", probe.flagsRecovered ? "true" : "false");
		fprintf(report, ",\"identity_matches\":%s,\"exists\":%s,\"manual_fake_signon\":%s,\"first_ready_observed\":%s,\"state_equal\":%s,\"position_ok\":%s,\"personal_arrival\":%s,\"passive_death_warning\":%s,\"deliberate_story_frozen\":%s",
			Identity(i) ? "true" : "false", player ? "true" : "false", probe.manualSignon ? "true" : "false",
			probe.observed ? "true" : "false", probe.equal ? "true" : "false", probe.positionOK ? "true" : "false", probe.personal ? "true" : "false",
			probe.passiveDeath ? "true" : "false", probe.storyFrozen ? "true" : "false");
		fprintf(report, ",\"first_spawn_supported\":%s,\"arrival_far_from_leader\":%s", probe.floorSupported ? "true" : "false", probe.farFromLeader ? "true" : "false");
		fprintf(report, ",\"source_floor_supported\":%s", probe.sourceFloor ? "true" : "false");
		fprintf(report, ",\"crosshair_suppressed_calls\":%d", probe.suppressedCrosshair);
		fputs(",\"state_validation_mode\":", report); String(destinationStripObserved ? "native_destination_weaponstrip" : "exact_transfer");
		fprintf(report, ",\"transformed_state_equal\":%s,\"native_strip_observed\":%s", probe.transformedEqual ? "true" : "false", destinationStripObserved ? "true" : "false");
		if ( NativeTrainRide() ) fprintf(report, ",\"first_spawn_on_intro_train\":%s", probe.firstTrainFloor ? "true" : "false");
		if ( requested ) fprintf(report, ",\"source_distance_from_leader\":%.4f", (probe.sourceOrigin - probes[sourceActivator].sourceOrigin).Length());
		fputs(",\"expected_origin\":", report);
		if ( MapSpawnFallback() ) fputs("null", report); else Vec(probe.expectedOrigin);
		fputs(",\"map_spawn_reference\":", report);
		if ( probe.mapSpawnKnown ) Vec(probe.mapSpawnReference); else fputs("null", report);
		if ( probe.mapSpawnKnown )
		{
			fputs(",\"map_spawn_reference_class\":", report); String(probe.mapSpawnClass);
			fprintf(report, ",\"map_spawn_reference_edict\":%d", probe.mapSpawnEdict);
		}
		fprintf(report, ",\"first_point_contents_clear\":%s", probe.contentsClear ? "true" : "false");
		if ( player )
		{
			fprintf(report, ",\"alive\":%s,\"pending\":%s,\"solid\":%d,\"frozen\":%s,\"clear_hull\":%s,\"flags\":%d,\"origin\":",
				player->IsAlive() ? "true" : "false", player->m_fCoopSpawnPending ? "true" : "false", player->pev->solid,
				(player->pev->flags & FL_FROZEN) ? "true" : "false", Hull(player) ? "true" : "false", player->pev->flags); Vec(player->pev->origin);
			State now; Capture(player, now); fputs(",\"state\":", report); StateFields(now);
			if ( requested && player->m_fCoopSpawnPending && !MapSpawnFallback() && Finite(probe.expectedOrigin) )
			{
				fputs(",\"expected_origin_blockers\":", report); BlockerFields(player, probe.expectedOrigin);
			}
			if ( !player->IsAlive() )
			{
				edict_t *source = player->pev->dmg_inflictor;
				fputs(",\"damage_entity_class\":", report); String(source && !source->free ? STRING(source->v.classname) : "");
			}
		}
		fputc('}', report);
	}
	fputs("]", report);
	if ( NativeTrainRide() ) TrainFields();
	End();
}

BOOL FindMapSpawnReference( CBasePlayer *player, Probe &probe )
{
	Vector angles; BOOL duck;
	if ( CoopLevelFindStart(player, probe.mapSpawnReference, angles, duck) )
	{
		Copy(probe.mapSpawnClass, sizeof(probe.mapSpawnClass), "coop_level_custom"); probe.mapSpawnEdict = -1;
		return TRUE;
	}
	static const char *classes[] = { "info_player_coop", "info_player_start", "info_player_deathmatch" };
	float bestDistance = 1e30f; BOOL found = FALSE;
	for ( int i = 0; i < ARRAYSIZE(classes); ++i )
	{
		CBaseEntity *spot = NULL;
		while ( (spot = UTIL_FindEntityByClassname(spot, classes[i])) != NULL )
		{
			if ( !spot->IsTriggered(player) || spot->IsDormant() || !Finite(spot->pev->origin) ) continue;
			Vector reference = spot->pev->origin + Vector(0, 0, 1);
			float distance = (player->pev->origin - reference).Length();
			if ( distance >= bestDistance ) continue;
			found = TRUE; bestDistance = distance; probe.mapSpawnReference = reference;
			Copy(probe.mapSpawnClass, sizeof(probe.mapSpawnClass), classes[i]); probe.mapSpawnEdict = spot->entindex();
		}
	}
	return found;
}

void Observe( int i, const char *label )
{
	Probe &probe = probes[i]; CBasePlayer *player = Player(i);
	if ( probe.observed || !player || !player->IsAlive() || player->IsDormant() || player->IsObserver() ||
		player->m_fCoopSpawnPending || player->pev->solid != SOLID_SLIDEBOX || !Hull(player) ) return;
	if ( (player->pev->flags & FL_FROZEN) && !CoopStoryIsBusy() ) return;
	probe.storyFrozen = (player->pev->flags & FL_FROZEN) != 0;
	probe.observed = TRUE; probe.firstReadyAt = gpGlobals->time; probe.firstOrigin = player->pev->origin; Capture(player, probe.firstState);
	probe.equal = Equal(probe.sourceState, probe.firstState);
	probe.transformedEqual = NativeStripStateEqual(probe.sourceState, probe.firstState);
	probe.floorSupported = Floor(player);
	if ( NativeTrainRide() ) probe.firstTrainFloor = TrainFloor(player, IntroTrain());
	int contents = UTIL_PointContents(player->pev->origin);
	probe.contentsClear = contents != CONTENTS_SOLID && contents != CONTENTS_LAVA && contents != CONTENTS_SLIME;
	if ( MapSpawnFallback() ) probe.mapSpawnKnown = FindMapSpawnReference(player, probe);
	Vector delta = player->pev->origin - (MapSpawnFallback() ? probe.mapSpawnReference : probe.expectedOrigin);
	probe.positionOK = fabs(delta.x) <= 192 && fabs(delta.y) <= 192 && fabs(delta.z) <= 96 &&
		(!MapSpawnFallback() || (probe.mapSpawnKnown && probe.contentsClear));
	Sample(label);
}

int OwnedProbe( const edict_t *client )
{
	if ( active && client && (client->v.flags & FL_FAKECLIENT) && AllowedServer() && CVAR_GET_FLOAT("sv_lan") == 1 )
		for ( int i = 0; i < 2; ++i )
			if ( probes[i].slot > 0 && probes[i].slot <= gpGlobals->maxClients && client == INDEXENT(probes[i].slot) &&
				GETPLAYERUSERID((edict_t *)client) == probes[i].user && !strcmp(STRING(client->v.netname), NAMES[i]) &&
				!strcmp(probes[i].sourceRawAuth, "BOT") )
				return i;
	return -1;
}

void SetViewAdapter( const edict_t *client, const edict_t *view )
{
	int i = OwnedProbe(client);
	if ( i >= 0 )
	{
		// У headless fake-слота после смены карты отсутствует сетевой буфер камеры
		++suppressedViews; ++probes[i].suppressedViews;
		if ( report )
		{
			Begin("set_view_suppressed"); EdgeFields();
			fprintf(report, ",\"probe\":%d,\"slot\":%d,\"userid\":%d,\"view_entity\":%d,\"call_count\":%d,\"camera_protocol_checked\":false",
				i, probes[i].slot, probes[i].user, view ? ENTINDEX((edict_t *)view) : -1, suppressedViews); End();
		}
		return;
	}
	if ( originalSetView ) originalSetView(client, view);
}

void CrosshairAngleAdapter( const edict_t *client, float pitch, float yaw )
{
	int i = OwnedProbe(client);
	if ( i >= 0 )
	{
		// Headless fake-клиент не имеет сетевого буфера для смещения прицела
		++suppressedCrosshair; ++probes[i].suppressedCrosshair;
		if ( report )
		{
			Begin("crosshair_angle_suppressed"); EdgeFields();
			fprintf(report, ",\"probe\":%d,\"slot\":%d,\"userid\":%d,\"call_count\":%d,\"probe_call_count\":%d,\"crosshair_protocol_checked\":false",
				i, probes[i].slot, probes[i].user, suppressedCrosshair, probes[i].suppressedCrosshair); End();
		}
		return;
	}
	if ( originalCrosshairAngle ) originalCrosshairAngle(client, pitch, yaw);
}

void Kick( void )
{
	BOOL owned = FALSE;
	for ( int i = 0; i < 2; ++i )
		if ( !probes[i].detached && Identity(i) && OwnedProbe(INDEXENT(probes[i].slot)) == i ) owned = TRUE;
	// Завершённая проверка освобождает камеру до отключения сетевых адаптеров
	if ( owned ) CoopStoryCameraStopped(NULL);
	BOOL queued = FALSE;
	for ( int i = 0; i < 2; ++i )
		if ( !probes[i].detached && Identity(i) && OwnedProbe(INDEXENT(probes[i].slot)) == i )
		{
			char command[64]; _snprintf(command, sizeof(command), "kick \"%s\"\n", NAMES[i]); SERVER_COMMAND(command);
			queued = TRUE;
		}
	// ClientDisconnect ещё может сбросить камеру и требует действующего адаптера
	if ( queued ) SERVER_EXECUTE();
	for ( int i = 0; i < 2; ++i )
	{
		if ( probes[i].detached ) continue;
		edict_t *entity = Identity(i);
		if ( !entity || OwnedProbe(entity) != i || !Player(i) ) continue;
		// У headless fake-клиента движок может отложить kick до следующего кадра
		probes[i].detached = TRUE;
		ClientDisconnect(entity);
		BOOL dormant = FALSE;
		if ( Identity(i) == entity && OwnedProbe(entity) == i )
		{
			CBasePlayer *player = Player(i);
			if ( player ) { player->MakeDormant(); dormant = TRUE; }
		}
		Begin("fake_probe_detached"); EdgeFields();
		fprintf(report, ",\"probe\":%d,\"slot\":%d,\"userid\":%d,\"reason\":\"owned_fake_probe_retained_after_synchronous_kick\",\"client_disconnect_called\":true,\"made_dormant\":%s,\"cleanup_after_measurement\":true",
			i, probes[i].slot, probes[i].user, dormant ? "true" : "false"); End();
	}
}

void Stop( const char *status )
{
	Kick();
	Begin("summary"); fputs(",\"status\":", report); String(status);
	fprintf(report, ",\"total\":%d,\"checked\":%d,\"passed\":%d,\"failed\":%d,\"setup_not_reached\":%d,\"protocol_limitations\":%d,\"warnings\":%d", requestedCount, completed, passed, failed, setupFailed, limitations, warnings);
	fprintf(report, ",\"set_view_suppressed_calls\":%d", suppressedViews);
	fprintf(report, ",\"crosshair_suppressed_calls\":%d,\"crosshair_protocol_checked\":false", suppressedCrosshair);
	fprintf(report, ",\"music_play_suppressed_calls\":%d,\"music_stop_suppressed_calls\":%d", musicPlay, musicStop);
	End(); fclose(report); report = NULL; active = FALSE;
	if ( originalAuthId ) g_engfuncs.pfnGetPlayerAuthId = originalAuthId;
	originalAuthId = NULL;
	if ( originalSetView ) g_engfuncs.pfnSetView = originalSetView;
	originalSetView = NULL;
	if ( originalCrosshairAngle ) g_engfuncs.pfnCrosshairAngle = originalCrosshairAngle;
	originalCrosshairAngle = NULL;
	ALERT(at_console, "Transition check: %s, %d/%d checked, %d passed, %d failed, %d setup issues, %d protocol limitations\n", status, completed, requestedCount, passed, failed, setupFailed, limitations);
}

void QueueEdge( void )
{
	Kick(); memset(probes, 0, sizeof(probes));
	requested = unexpectedActivation = FALSE; triggerIndex = 0; sourceActivator = 0;
	trainSourceVerified = trainMovementVerified = trainDwellExtended = FALSE; trainPathRecoveredAt = 0; trainSetupOrigin = g_vecZero;
	ambushSourceVerified = FALSE; ambushBetaStart = g_vecZero;
	xenGateTouched = xenPortalTouched = xenSourceVerified = FALSE;
	xenStage = xenGateEdict = xenPortalEdict = 0;
	xenGateTouchAt = xenPortalTouchAt = xenStageAt = 0;
	destinationStripProducerObserved = destinationStripUseObserved = destinationStripObserved = FALSE;
	destinationStripProducerTime = destinationStripUseTime = 0;
	destinationStripProducerEdict = destinationStripCallerEdict = destinationStripEventEdict = 0;
	reverseEnabled = reverseReturning = reverseOutwardVerified = reverseEarlyTouch = reverseHoldVerified = reverseExitVerified = reverseReturnRequested = reverseMovementUnsafe = FALSE;
	reversePhase = REVERSE_OFF; reverseTriggerIndex = 0;
	reverseSpawnAt = reversePhaseAt = reverseStartedAt = reverseEarlyTouchAt = 0;
	reverseInside = reverseOutside = g_vecZero;
	pendingReleaseActive = pendingReleaseAttempted = pendingReleaseDuck = FALSE;
	pendingReleaseUntil = 0; pendingReleaseGoal = g_vecZero;
	// Каждый переход начинается с нового мира, вступительный поезд проходит штатный путь
	CoopStartCancelQueued(FALSE); CoopWorldReset(); CoopCampaignReset(); CoopStoryReset(); CoopSessionReset(); gGlobalState.ClearStates();
	phase = WAIT_SOURCE;
	Begin("edge_queued"); EdgeFields(); End();
	char command[64]; _snprintf(command, sizeof(command), "map %s\n", Current().source); SERVER_COMMAND(command);
}

void Finish( const char *status, const char *reason )
{
	if ( reverseEnabled ) { FinishReverse(!strcmp(status, "passed") ? "failed" : status, reason); return; }
	Sample("end"); Begin("edge_result"); EdgeFields();
	fputs(",\"status\":", report); String(status); fputs(",\"reason\":", report); String(reason);
	fprintf(report, ",\"landmark_verified\":%s", !MapSpawnFallback() && probes[0].observed && probes[1].observed && probes[0].positionOK && probes[1].positionOK ? "true" : "false");
	fprintf(report, ",\"transition_requested\":%s,\"stability_seconds\":%.4f,\"manual_fake_signon\":%s",
		requested ? "true" : "false", phase == STABILITY ? gpGlobals->time - phaseTime : 0,
		(probes[0].manualSignon || probes[1].manualSignon) ? "true" : "false");
	BOOL passiveDeath = probes[0].passiveDeath || probes[1].passiveDeath;
	BOOL storyFrozen = probes[0].storyFrozen || probes[1].storyFrozen;
	BOOL unsupported = (probes[0].observed && !probes[0].floorSupported) || (probes[1].observed && !probes[1].floorSupported);
	BOOL farArrival = probes[1].farFromLeader;
	fprintf(report, ",\"passive_death_warning\":%s,\"deliberate_story_frozen\":%s,\"arrival_floor_warning\":%s,\"arrival_far_from_leader\":%s",
		passiveDeath ? "true" : "false", storyFrozen ? "true" : "false", unsupported ? "true" : "false", farArrival ? "true" : "false");
	fprintf(report, ",\"set_view_suppressed_calls\":%d,\"music_play_suppressed_calls\":%d,\"music_stop_suppressed_calls\":%d",
		probes[0].suppressedViews + probes[1].suppressedViews, probes[0].musicPlay + probes[1].musicPlay, probes[0].musicStop + probes[1].musicStop);
	fprintf(report, ",\"crosshair_suppressed_calls\":%d,\"crosshair_protocol_checked\":false", probes[0].suppressedCrosshair + probes[1].suppressedCrosshair);
	fputs(",\"state_validation_mode\":", report); String(destinationStripObserved ? "native_destination_weaponstrip" : "exact_transfer");
	fprintf(report, ",\"transformed_state_equal\":%s,\"native_strip_observed\":%s", probes[0].transformedEqual && probes[1].transformedEqual ? "true" : "false", destinationStripObserved ? "true" : "false"); End();
	if ( passiveDeath || storyFrozen || unsupported || farArrival ) ++warnings;
	if ( !strcmp(status, "passed") ) ++passed;
	else if ( !strcmp(status, "setup_not_reached") ) ++setupFailed;
	else if ( !strcmp(status, "protocol_limitation") ) ++limitations;
	else ++failed;
	Kick();
	++completed;
	if ( completed == requestedCount ) Stop("complete"); else QueueEdge();
}

BOOL Spawn( int i )
{
	edict_t *entity = g_engfuncs.pfnCreateFakeClient(NAMES[i]);
	if ( !entity ) return FALSE;
	Probe &probe = probes[i]; probe.slot = ENTINDEX(entity); probe.user = GETPLAYERUSERID(entity);
	Copy(probe.sourceRawAuth, sizeof(probe.sourceRawAuth), RawAuth(entity));
	Copy(probe.auth, sizeof(probe.auth), GETPLAYERAUTHID(entity));
	if ( probe.user <= 0 ) return FALSE;
	char reject[128] = { 0 };
	if ( !ClientConnect(entity, NAMES[i], "127.0.0.1", reject) ) return FALSE;
	entity->v.flags |= FL_CLIENT | FL_FAKECLIENT;
	ClientPutInServer(entity);
	return Player(i) != NULL;
}

BOOL InVolume( CBasePlayer *player, BOOL allowMissing )
{
	BOOL found = FALSE; CBaseEntity *entity = NULL;
	while ( (entity = UTIL_FindEntityByTargetname(entity, Current().landmark)) != NULL )
		if ( FClassnameIs(entity->pev, "trigger_transition") )
		{
			found = TRUE;
			if ( entity->Intersects(player) ) return TRUE;
		}
	return allowMissing && !found;
}

CBaseEntity *FindTrigger( void )
{
	CBaseEntity *entity = NULL;
	while ( (entity = UTIL_FindEntityByClassname(entity, "trigger_changelevel")) != NULL )
		if ( !strcmp(STRING(entity->pev->model), Current().model) && !strcmp(STRING(entity->pev->targetname), Current().target) &&
			(entity->pev->absmin - Current().mins).Length() < 4 && (entity->pev->absmax - Current().maxs).Length() < 4 ) return entity;
	return NULL;
}

BOOL TouchesExit( const Vector &origin, BOOL duck )
{
	Vector mins = origin + (duck ? VEC_DUCK_HULL_MIN : VEC_HULL_MIN);
	Vector maxs = origin + (duck ? VEC_DUCK_HULL_MAX : VEC_HULL_MAX);
	CBaseEntity *entity = NULL;
	while ( (entity = UTIL_FindEntityByClassname(entity, "trigger_changelevel")) != NULL )
		if ( entity->m_pfnTouch && !entity->IsDormant() && entity->pev->solid == SOLID_TRIGGER &&
			mins.x <= entity->pev->absmax.x && maxs.x >= entity->pev->absmin.x &&
			mins.y <= entity->pev->absmax.y && maxs.y >= entity->pev->absmin.y &&
			mins.z <= entity->pev->absmax.z && maxs.z >= entity->pev->absmin.z ) return TRUE;
	return FALSE;
}

BOOL Place( CBasePlayer *player, const Vector &seed, const Vector &angles, BOOL duck, BOOL &floorSupported, BOOL parking = FALSE )
{
	if ( !Finite(seed) )
	{
		if ( !parking )
		{
			Begin("source_place_rejected"); EdgeFields();
			fprintf(report, ",\"probe\":%d,\"duck\":%s,\"reason\":\"nonfinite_seed\",\"seed\":", player == Player(1) ? 1 : 0, duck ? "true" : "false");
			Vec(seed); End();
		}
		return FALSE;
	}
	TraceResult trace;
	UTIL_TraceHull(seed, seed - Vector(0, 0, 72), ignore_monsters, duck ? head_hull : human_hull, player->edict(), &trace);
	floorSupported = !trace.fStartSolid && !trace.fAllSolid && trace.flFraction < 1 && trace.vecPlaneNormal.z >= 0.7f;
	if ( parking && !floorSupported ) return FALSE;
	// Точный endpoint сохраняет воздушные и составные позиции, парковка требует пола
	Vector origin = parking ? trace.vecEndPos + Vector(0, 0, 1) : seed;
	UTIL_TraceHull(origin, origin, parking ? dont_ignore_monsters : ignore_monsters, duck ? head_hull : human_hull, player->edict(), &trace);
	if ( trace.fStartSolid || trace.fAllSolid )
	{
		if ( !parking )
		{
			Begin("source_place_rejected"); EdgeFields();
			fprintf(report, ",\"probe\":%d,\"duck\":%s,\"reason\":\"seed_hull_blocked\",\"point_contents\":%d,\"start_solid\":%s,\"all_solid\":%s,\"fraction\":%.4f,\"hit_edict\":%d,\"hit_class\":",
				player == Player(1) ? 1 : 0, duck ? "true" : "false", UTIL_PointContents(origin), trace.fStartSolid ? "true" : "false",
				trace.fAllSolid ? "true" : "false", trace.flFraction, trace.pHit ? ENTINDEX(trace.pHit) : -1);
			String(trace.pHit ? STRING(trace.pHit->v.classname) : "");
			fputs(",\"hit_model\":", report); String(trace.pHit ? STRING(trace.pHit->v.model) : "");
			fputs(",\"seed\":", report); Vec(seed); End();
		}
		return FALSE;
	}
	if ( parking && TouchesExit(origin, duck) ) return FALSE;
	int contents = UTIL_PointContents(origin);
	if ( contents == CONTENTS_SOLID || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA )
	{
		if ( !parking )
		{
			Begin("source_place_rejected"); EdgeFields();
			fprintf(report, ",\"probe\":%d,\"duck\":%s,\"reason\":\"unsafe_seed_point_contents\",\"point_contents\":%d,\"seed\":",
				player == Player(1) ? 1 : 0, duck ? "true" : "false", contents); Vec(seed); End();
		}
		return FALSE;
	}
	if ( duck ) player->pev->flags |= FL_DUCKING; else player->pev->flags &= ~FL_DUCKING;
	UTIL_SetSize(player->pev, duck ? VEC_DUCK_HULL_MIN : VEC_HULL_MIN, duck ? VEC_DUCK_HULL_MAX : VEC_HULL_MAX);
	player->pev->view_ofs = duck ? VEC_DUCK_VIEW : VEC_VIEW;
	player->pev->velocity = g_vecZero; player->pev->basevelocity = g_vecZero;
	player->pev->v_angle = angles; player->pev->angles = Vector(-angles.x, angles.y, 0); player->pev->fixangle = TRUE;
	UTIL_SetOrigin(player->pev, origin);
	return TRUE;
}

void StageSource( void )
{
	for ( int i = 0; i < 2; ++i )
	{
		CBasePlayer *player = Player(i);
		if ( !Ready(player) || !Hull(player) ) continue;
		if ( !probes[i].spawnObserved ) { probes[i].spawnObserved = TRUE; probes[i].spawnOrigin = player->pev->origin; }
		CBasePlayer *other = Player(1 - i);
		if ( probes[i].sourceParked || !other || !other->m_fCoopSpawnPending ) continue;
		static const Vector offsets[] = { Vector(48,0,0), Vector(-48,0,0), Vector(0,48,0), Vector(0,-48,0), Vector(96,0,0), Vector(-96,0,0) };
		for ( int candidate = 0; candidate <= ARRAYSIZE(offsets); ++candidate )
		{
			Vector seed = candidate ? probes[i].spawnOrigin + offsets[candidate - 1] : Current().origin[1];
			BOOL duck = candidate ? ((player->pev->flags & FL_DUCKING) != 0) : Current().duck[1];
			BOOL floor;
			if ( !Place(player, seed, player->pev->v_angle, duck, floor, TRUE) ) continue;
			// Освобождаем обычную точку появления, не снимая pending у второго игрока
			probes[i].sourceParked = TRUE;
			Begin("source_parked"); EdgeFields(); fprintf(report, ",\"probe\":%d,\"point_teleport\":true,\"source_floor_supported\":true,\"origin\":", i); Vec(player->pev->origin); End();
			Sample("source_parked"); break;
		}
	}
}

void Kit( CBasePlayer *player, int i )
{
	player->RemoveAllItems(TRUE);
	player->GiveNamedItem("item_suit"); player->GiveNamedItem("weapon_crowbar"); player->GiveNamedItem("weapon_9mmhandgun");
	player->GiveNamedItem(i ? "weapon_shotgun" : "weapon_357");
	for ( int slot = 0; slot < MAX_ITEM_TYPES; ++slot )
		for ( CBasePlayerItem *item = player->m_rgpPlayerItems[slot]; item; item = item->m_pNext )
			if ( item->GetWeaponPtr() && item->iMaxClip() > 0 ) ((CBasePlayerWeapon *)item)->m_iClip = min(item->iMaxClip(), i ? 3 : 2);
	for ( int ammo = 0; ammo < MAX_AMMO_SLOTS; ++ammo ) player->m_rgAmmo[ammo] = 0;
	player->GiveAmmo(i ? 67 : 31, "9mm", 250); player->GiveAmmo(i ? 7 : 5, i ? "buckshot" : "357", i ? 125 : 36);
	player->pev->health = i ? 91 : 73; player->pev->armorvalue = i ? 47 : 23; player->m_iFlashBattery = i ? 57 : 83;
}

BOOL SetupTrainRide( void )
{
	CBaseEntity *trigger = FindTrigger();
	CFuncTrackTrain *train = IntroTrain();
	if ( !trigger || trigger->IsDormant() || (trigger->pev->flags & FL_KILLME) || !train ||
		(Current().use ? !trigger->m_pfnUse : (!trigger->m_pfnTouch || trigger->pev->solid != SOLID_TRIGGER)) ) return FALSE;
	for ( int i = 0; i < 2; ++i )
	{
		CBasePlayer *player = Player(i);
		if ( !Ready(player) || !Hull(player) || !TrainFloor(player, train) ) return FALSE;
	}
	for ( int i = 0; i < 2; ++i )
	{
		probes[i].sourceFloor = TRUE;
		probes[i].spawnObserved = TRUE; probes[i].spawnOrigin = Player(i)->pev->origin;
		Kit(Player(i), i);
	}
	triggerIndex = trigger->entindex(); trainSetupOrigin = train->pev->origin;
	Begin("setup"); EdgeFields(); fprintf(report, ",\"source_bsp_entity\":%d,\"trigger_edict\":%d,\"model\":", Current().bspIndex, triggerIndex); String(Current().model);
	fputs(",\"activation\":\"native_train_ride\",\"point_teleport\":false,\"train_reposition\":false,\"godmode\":false,\"noclip\":false,\"source_floor_supported\":[true,true],\"airborne_setup\":false,\"timeout_seconds\":240", report);
	TrainFields(); End();
	// Поезд сам доставляет пассажиров и запускает исходный Touch или сюжетный Use
	phase = WAIT_DESTINATION; phaseTime = gpGlobals->time;
	Sample("before_native_train_ride");
	return TRUE;
}

BOOL SetupAmbush( void )
{
	CBaseEntity *exit = FindTrigger(), *entry = NULL;
	CBaseEntity *teleport = UTIL_FindEntityByTargetname(NULL, "ambush_teleport");
	if ( !exit || exit->IsDormant() || !exit->m_pfnUse ) return FALSE;
	while ( (entry = UTIL_FindEntityByClassname(entry, "trigger_once")) != NULL )
		if ( !strcmp(STRING(entry->pev->model), "*100") && !strcmp(STRING(entry->pev->target), "ambush_mm") &&
			(entry->pev->absmin - Vector(-2160, -848, 788)).Length() < 4 &&
			(entry->pev->absmax - Vector(-1840, -352, 916)).Length() < 4 ) break;
	if ( !entry || entry->IsDormant() || entry->pev->solid != SOLID_TRIGGER || !entry->m_pfnTouch ||
		!UTIL_FindEntityByTargetname(NULL, "ambush_mm") || !teleport || !FClassnameIs(teleport->pev, "trigger_teleport") ||
		strcmp(STRING(teleport->pev->model), "*122") ||
		(teleport->pev->absmin - Vector(-2160, -848, 788)).Length() >= 4 ||
		(teleport->pev->absmax - Vector(-1776, -352, 916)).Length() >= 4 ) return FALSE;
	const Vector seeds[] = { Vector(-2000, -600, 825), Vector(-2036, -600, 825) };
	for ( int i = 0; i < (farMode ? 1 : 2); ++i )
		if ( !Place(Player(i), seeds[i], Vector(0, 180, 0), FALSE, probes[i].sourceFloor) || !probes[i].sourceFloor ) return FALSE;
	if ( !Ready(Player(0)) || !Ready(Player(1)) || !Hull(Player(0)) || !Hull(Player(1)) || !entry->Intersects(Player(0)) ) return FALSE;
	if ( farMode )
	{
		// Отставший товарищ остаётся в своей реальной точке появления, вне сцены засады
		probes[1].sourceFloor = Floor(Player(1));
		if ( !probes[1].sourceFloor || entry->Intersects(Player(1)) || teleport->Intersects(Player(1)) ||
			(Player(1)->pev->origin - Player(0)->pev->origin).Length() <= 256 ) return FALSE;
	}
	else if ( !entry->Intersects(Player(1)) ) return FALSE;
	ambushBetaStart = Player(1)->pev->origin;
	// Снаряжение выдаётся до сцены, затем родной weaponstrip действует без вмешательства проверки
	for ( int i = 0; i < 2; ++i ) Kit(Player(i), i);
	triggerIndex = exit->entindex();
	Begin("setup"); EdgeFields();
	fprintf(report, ",\"source_bsp_entity\":%d,\"trigger_edict\":%d,\"scene_entry_edict\":%d,\"model\":", Current().bspIndex, triggerIndex, entry->entindex());
	String(Current().model);
	fputs(",\"activation\":\"native_ambush\",\"source_scene\":\"trigger_once_ambush_mm\",\"scene_entry_model\":\"*100\",\"point_teleport\":true,\"point_staging\":true,\"gotodrag_direct_use\":false,\"godmode\":false,\"noclip\":false,\"native_delay_seconds\":12,\"timeout_seconds\":25,\"source_floor_supported\":[true,true],\"airborne_setup\":false", report);
	fprintf(report, ",\"far_teammate\":%s,\"scene_participants\":%d,\"beta_point_staging\":%s,\"beta_outside_entry\":%s,\"beta_outside_teleport\":%s,\"source_separation\":%.4f,\"beta_start_origin\":",
		farMode ? "true" : "false", farMode ? 1 : 2, farMode ? "false" : "true", entry->Intersects(Player(1)) ? "false" : "true",
		teleport->Intersects(Player(1)) ? "false" : "true", (Player(1)->pev->origin - Player(0)->pev->origin).Length());
	Vec(ambushBetaStart);
	End();
	phase = WAIT_DESTINATION; phaseTime = gpGlobals->time;
	Sample("before_native_ambush");
	// Запускаем только настоящий вход в засаду, gotodrag вызовет исходный multi_manager
	entry->Touch(Player(0));
	return TRUE;
}

CBaseEntity *XenBrush( const char *classname, const char *model, const char *targetname = NULL )
{
	CBaseEntity *entity = NULL;
	while ( (entity = UTIL_FindEntityByClassname(entity, classname)) != NULL )
		if ( !strcmp(STRING(entity->pev->model), model) && (!targetname || !strcmp(STRING(entity->pev->targetname), targetname)) &&
			!entity->IsDormant() && !(entity->pev->flags & FL_KILLME) ) return entity;
	return NULL;
}

CBaseEntity *XenTransitionVolume( void )
{
	CBaseEntity *entity = NULL;
	while ( (entity = UTIL_FindEntityByClassname(entity, "trigger_transition")) != NULL )
		// Spawn очищает модель объёма, его исходные границы остаются в работающем мире
		if ( !strcmp(STRING(entity->pev->targetname), "c4a1b") && entity->pev->solid == SOLID_NOT &&
			!entity->pev->modelindex && !*STRING(entity->pev->model) && !entity->IsDormant() && !(entity->pev->flags & FL_KILLME) &&
			(entity->pev->absmin - Vector(-1040, -1328, 465)).Length() < 4 &&
			(entity->pev->absmax - Vector(-912, -1200, 529)).Length() < 4 ) return entity;
	return NULL;
}

BOOL XenBrushContact( CBaseEntity *entity, const Vector &origin )
{
	if ( !entity || !Finite(origin) ) return FALSE;
	if ( FClassnameIs(entity->pev, "trigger_transition") )
	{
		// Штатная проверка перехода использует пересечение корпусов с этим объёмом
		for ( int axis = 0; axis < 3; ++axis )
			if ( origin[axis] + VEC_HULL_MAX[axis] < entity->pev->absmin[axis] ||
				origin[axis] + VEC_HULL_MIN[axis] > entity->pev->absmax[axis] ) return FALSE;
		return TRUE;
	}
	TraceResult trace;
	UTIL_TraceModel(origin, origin, human_hull, entity->edict(), &trace);
	return trace.fStartSolid || trace.fAllSolid || trace.flFraction < 1.0f;
}

BOOL XenPointSafe( CBasePlayer *player, const Vector &origin )
{
	if ( !player || !Finite(origin) ) return FALSE;
	int contents = UTIL_PointContents(origin);
	if ( contents == CONTENTS_SOLID || contents == CONTENTS_LAVA || contents == CONTENTS_SLIME ) return FALSE;
	TraceResult trace;
	UTIL_TraceHull(origin, origin, dont_ignore_monsters, human_hull, player->edict(), &trace);
	if ( trace.fStartSolid || trace.fAllSolid ) return FALSE;
	CBaseEntity *hurt = NULL;
	while ( (hurt = UTIL_FindEntityByClassname(hurt, "trigger_hurt")) != NULL )
		if ( hurt->pev->solid == SOLID_TRIGGER && hurt->pev->dmg > 0 && XenBrushContact(hurt, origin) ) return FALSE;
	return TRUE;
}

BOOL XenSetupRejected( const char *reason, CBasePlayer *player = NULL, const Vector &origin = g_vecZero )
{
	Begin("native_xen_setup_rejected"); EdgeFields();
	fputs(",\"reason\":", report); String(reason);
	if ( player )
	{
		TraceResult trace;
		UTIL_TraceHull(origin, origin, dont_ignore_monsters, human_hull, player->edict(), &trace);
		fprintf(report, ",\"probe\":%d,\"start_solid\":%s,\"all_solid\":%s,\"hit_edict\":%d,\"hit_class\":",
			player == Player(1) ? 1 : 0, trace.fStartSolid ? "true" : "false", trace.fAllSolid ? "true" : "false", trace.pHit ? ENTINDEX(trace.pHit) : -1);
		String(trace.pHit ? STRING(trace.pHit->v.classname) : "");
		fputs(",\"hit_model\":", report); String(trace.pHit ? STRING(trace.pHit->v.model) : "");
		fputs(",\"seed\":", report); Vec(origin);
	}
	End(); return FALSE;
}

void XenSceneFields( void )
{
	CBaseEntity *hurt = XenBrush("trigger_hurt", "*99", "solved");
	CBaseEntity *blocker = XenBrush("func_wall_toggle", "*163", "blocker");
	fprintf(report, ",\"gate_native_touch\":%s,\"portal_native_touch\":%s,\"gate_touch_time\":%.4f,\"portal_touch_time\":%.4f,\"gate_touch_edict\":%d,\"portal_touch_edict\":%d,\"hurt_edict\":%d,\"hurt_solid\":%d,\"hurt_damage\":%.4f,\"hurt_off\":%s,\"blocker_edict\":%d,\"blocker_solid\":%d,\"blocker_on\":%s",
		xenGateTouched ? "true" : "false", xenPortalTouched ? "true" : "false", xenGateTouchAt, xenPortalTouchAt, xenGateEdict, xenPortalEdict,
		hurt ? hurt->entindex() : -1, hurt ? hurt->pev->solid : -1, hurt ? hurt->pev->dmg : -1,
		hurt && hurt->pev->solid == SOLID_NOT ? "true" : "false", blocker ? blocker->entindex() : -1, blocker ? blocker->pev->solid : -1,
		blocker && blocker->pev->solid == SOLID_BSP ? "true" : "false");
}

BOOL SetupXenPortal( void )
{
	CBaseEntity *exit = FindTrigger(), *gate = XenBrush("trigger_once", "*98"), *portal = XenBrush("trigger_once", "*101");
	CBaseEntity *hurt = XenBrush("trigger_hurt", "*99", "solved"), *blocker = XenBrush("func_wall_toggle", "*163", "blocker");
	if ( !exit || !exit->m_pfnUse ) return XenSetupRejected("exit_use_missing");
	if ( !gate || !portal || !gate->m_pfnTouch || !portal->m_pfnTouch ) return XenSetupRejected("gate_or_portal_callback_missing");
	if ( strcmp(STRING(gate->pev->target), "solved") || strcmp(STRING(portal->pev->target), "go") ) return XenSetupRejected("native_target_mismatch");
	if ( !hurt || hurt->pev->solid != SOLID_TRIGGER || hurt->pev->dmg != 10000 ) return XenSetupRejected("native_hurt_initial_state_mismatch");
	if ( !blocker || blocker->pev->solid != SOLID_NOT ) return XenSetupRejected("native_blocker_initial_state_mismatch");
	if ( !UTIL_FindEntityByTargetname(NULL, "go") || !XenTransitionVolume() ) return XenSetupRejected("native_manager_or_transition_volume_missing");
	const Vector seeds[] = { Vector(-976, -936, 640), Vector(-940, -936, 640) };
	for ( int i = 0; i < 2; ++i )
	{
		if ( !XenPointSafe(Player(i), seeds[i]) ) return XenSetupRejected("gate_seed_unsafe", Player(i), seeds[i]);
		if ( !XenBrushContact(gate, seeds[i]) ) return XenSetupRejected("gate_seed_does_not_contact_native_brush", Player(i), seeds[i]);
		if ( !Place(Player(i), seeds[i], Vector(0, 270, 0), FALSE, probes[i].sourceFloor) ) return XenSetupRejected("gate_seed_place_rejected", Player(i), seeds[i]);
	}
	if ( !Ready(Player(0)) || !Ready(Player(1)) || !Hull(Player(0)) || !Hull(Player(1)) ) return FALSE;
	for ( int i = 0; i < 2; ++i ) Kit(Player(i), i);
	triggerIndex = exit->entindex(); xenGateEdict = gate->entindex(); xenPortalEdict = portal->entindex();
	xenStage = 1; xenStageAt = gpGlobals->time;
	Begin("setup"); EdgeFields();
	fprintf(report, ",\"source_bsp_entity\":%d,\"trigger_edict\":%d,\"model\":", Current().bspIndex, triggerIndex); String(Current().model);
	fputs(",\"activation\":\"point_staged_native_xen_gate_portal\",\"stage\":\"gate\",\"point_teleport\":true,\"source_point_staging\":true,\"continuous_traversal_checked\":false,\"godmode\":false,\"noclip\":false,\"direct_exit_use\":false,\"manual_gate_touch\":false,\"manual_portal_touch\":false,\"manual_world_toggle\":false,\"health_repair_after_staging\":false,\"native_delay_seconds\":4,\"timeout_seconds\":15,\"airborne_setup\":true", report);
	fprintf(report, ",\"gate_bsp_entity\":336,\"gate_model\":\"*98\",\"portal_bsp_entity\":354,\"portal_model\":\"*101\",\"hurt_bsp_entity\":350,\"hurt_model\":\"*99\",\"source_floor_supported\":[%s,%s]", probes[0].sourceFloor ? "true" : "false", probes[1].sourceFloor ? "true" : "false");
	XenSceneFields(); End();
	phase = WAIT_DESTINATION; phaseTime = gpGlobals->time;
	Sample("xen_gate_source_point_staging");
	return TRUE;
}

BOOL StageXenPortalPair( void )
{
	CBaseEntity *portal = XenBrush("trigger_once", "*101"), *volume = XenTransitionVolume();
	if ( !portal || !portal->m_pfnTouch || !volume ) return FALSE;
	static const float heights[] = { 497, 489, 481, 473 };
	static const Vector positions[] = { Vector(-976,-1264,0), Vector(-984,-1264,0), Vector(-968,-1264,0), Vector(-976,-1256,0), Vector(-976,-1272,0) };
	static const Vector offsets[] = { Vector(0,36,0), Vector(0,-36,0), Vector(36,0,0), Vector(-36,0,0), Vector(0,48,0), Vector(0,-48,0), Vector(48,0,0), Vector(-48,0,0) };
	Vector selected[2]; BOOL found = FALSE;
	for ( int height = 0; !found && height < ARRAYSIZE(heights); ++height )
		for ( int position = 0; !found && position < ARRAYSIZE(positions); ++position )
		{
			Vector alpha = positions[position] + Vector(0, 0, heights[height]);
			if ( !XenPointSafe(Player(0), alpha) || !XenBrushContact(portal, alpha) || !XenBrushContact(volume, alpha) ) continue;
			for ( int offset = 0; offset < ARRAYSIZE(offsets); ++offset )
			{
				Vector beta = alpha + offsets[offset];
				if ( !XenPointSafe(Player(1), beta) || !XenBrushContact(volume, beta) ) continue;
				selected[0] = alpha; selected[1] = beta; found = TRUE; break;
			}
		}
	if ( !found ) return FALSE;
	for ( int i = 0; i < 2; ++i )
		if ( !Place(Player(i), selected[i], Vector(0, 270, 0), FALSE, probes[i].sourceFloor) ) return FALSE;
	if ( !Hull(Player(0)) || !Hull(Player(1)) || !InVolume(Player(0), FALSE) || !InVolume(Player(1), FALSE) ) return FALSE;
	xenStage = 2; xenStageAt = gpGlobals->time;
	Begin("native_xen_portal_stage"); EdgeFields();
	fputs(",\"activation\":\"point_staged_native_xen_gate_portal\",\"stage\":\"portal\",\"point_teleport\":true,\"source_point_staging\":true,\"continuous_traversal_checked\":false,\"manual_portal_touch\":false,\"direct_exit_use\":false,\"manual_world_toggle\":false,\"health_repair_after_staging\":false,\"clear_hulls\":true,\"both_in_transition_volume\":true,\"source_origins\":[", report);
	Vec(Player(0)->pev->origin); fputc(',', report); Vec(Player(1)->pev->origin); fputc(']', report);
	XenSceneFields(); End();
	Sample("xen_portal_source_point_staging");
	return TRUE;
}

void XenPortalThink( void )
{
	CBaseEntity *hurt = XenBrush("trigger_hurt", "*99", "solved");
	if ( !Ready(Player(0)) || !Ready(Player(1)) || !Hull(Player(0)) || !Hull(Player(1)) )
	{ Finish("setup_not_reached", "xen_staged_players_became_unsafe_or_died"); return; }
	if ( xenStage == 1 && xenGateTouched && hurt && hurt->pev->solid == SOLID_NOT )
	{
		Begin("native_xen_gate_disabled"); EdgeFields();
		fputs(",\"activation\":\"point_staged_native_xen_gate_portal\",\"manual_gate_touch\":false,\"manual_world_toggle\":false", report);
		XenSceneFields(); End();
		if ( !StageXenPortalPair() ) Finish("setup_not_reached", "xen_portal_pair_has_no_clear_runtime_positions_after_native_gate");
	}
	else if ( xenStage == 1 && gpGlobals->time > xenStageAt + 1 )
		Finish("setup_not_reached", "xen_native_gate_touch_did_not_disable_hurt");
	else if ( xenStage == 2 && (!hurt || hurt->pev->solid != SOLID_NOT) )
		Finish("setup_not_reached", "xen_hurt_became_active_after_gate");
	else if ( xenStage == 2 && !xenPortalTouched && gpGlobals->time > xenStageAt + 1 )
		Finish("setup_not_reached", "xen_native_portal_touch_not_observed");
}

BOOL Setup( void )
{
	CBaseEntity *trigger = FindTrigger();
	if ( !trigger || trigger->IsDormant() || (trigger->pev->flags & FL_KILLME) ||
		(Current().use ? !trigger->m_pfnUse : (!trigger->m_pfnTouch || trigger->pev->solid != SOLID_TRIGGER)) ) return FALSE;
	if ( !Finite(Current().sourceLandmark) ) return FALSE;
	for ( int i = 0; i < 2; ++i )
		if ( !Place(Player(i), farMode && i == 1 ? probes[i].spawnOrigin : Current().origin[i], Current().angles[i], Current().duck[i], probes[i].sourceFloor) ) return FALSE;
	if ( !Hull(Player(0)) || !Hull(Player(1)) || !InVolume(Player(0), TRUE) ||
		(!Current().use && !trigger->Intersects(Player(0))) ||
		(farMode && (Player(1)->pev->origin - Player(0)->pev->origin).Length() <= 256) ) return FALSE;
	for ( int i = 0; i < 2; ++i ) Kit(Player(i), i);
	triggerIndex = trigger->entindex();
	Begin("setup"); EdgeFields(); fprintf(report, ",\"source_bsp_entity\":%d,\"trigger_edict\":%d,\"model\":", Current().bspIndex, triggerIndex); String(Current().model);
	fputs(",\"activation\":", report); String(Current().use ? "use" : "touch"); fputs(",\"point_teleport\":true,\"godmode\":false", report);
	fprintf(report, ",\"source_floor_supported\":[%s,%s],\"airborne_setup\":%s", probes[0].sourceFloor ? "true" : "false", probes[1].sourceFloor ? "true" : "false",
		(!probes[0].sourceFloor || !probes[1].sourceFloor) ? "true" : "false"); End();
	phase = WAIT_DESTINATION; phaseTime = gpGlobals->time;
	Sample("before_trigger");
	if ( Current().use ) trigger->Use(Player(0), Player(0), USE_TOGGLE, 0); else trigger->Touch(Player(0));
	return TRUE;
}

BOOL ProbeTouchesAt( CBaseEntity *entity, const Vector &origin, BOOL duck )
{
	Vector mins = origin + (duck ? VEC_DUCK_HULL_MIN : VEC_HULL_MIN);
	Vector maxs = origin + (duck ? VEC_DUCK_HULL_MAX : VEC_HULL_MAX);
	return mins.x <= entity->pev->absmax.x && maxs.x >= entity->pev->absmin.x &&
		mins.y <= entity->pev->absmax.y && maxs.y >= entity->pev->absmin.y &&
		mins.z <= entity->pev->absmax.z && maxs.z >= entity->pev->absmin.z;
}

BOOL ReleasePathRejected( const char *reason, CBasePlayer *player, const Vector &goal,
	const Vector &point, BOOL duck, int step, const TraceResult &trace, CBaseEntity *entity, BOOL diagnostic )
{
	if ( diagnostic )
	{
		Begin("pending_release_path_rejected"); EdgeFields(); fputs(",\"reason\":", report); String(reason);
		fprintf(report, ",\"duck\":%s,\"step\":%d,\"start_solid\":%s,\"all_solid\":%s,\"fraction\":%.4f,\"normal_z\":%.4f,\"hit_edict\":%d,\"hit_class\":",
			duck ? "true" : "false", step, trace.fStartSolid ? "true" : "false", trace.fAllSolid ? "true" : "false",
			trace.flFraction, trace.vecPlaneNormal.z, trace.pHit ? ENTINDEX(trace.pHit) : -1);
		String(trace.pHit ? STRING(trace.pHit->v.classname) : "");
		fputs(",\"hit_model\":", report); String(trace.pHit ? STRING(trace.pHit->v.model) : "");
		fputs(",\"trigger_class\":", report); String(entity ? STRING(entity->pev->classname) : "");
		fputs(",\"origin\":", report); Vec(player->pev->origin);
		fputs(",\"goal\":", report); Vec(goal); fputs(",\"point\":", report); Vec(point);
		fputs(",\"trace_end\":", report); Vec(trace.vecEndPos); End();
	}
	return FALSE;
}

BOOL ReleasePathSafe( CBasePlayer *player, const Vector &destination, BOOL duck, BOOL diagnostic = FALSE, Vector *floorGoal = NULL )
{
	BOOL wasDuck = (player->pev->flags & FL_DUCKING) != 0;
	Vector start = player->pev->origin + Vector(0, 0, duck == wasDuck ? 0 : (duck ? -18 : 18));
	Vector delta = destination - start, previous = start;
	int hull = duck ? head_hull : human_hull;
	TraceResult trace;
	UTIL_TraceHull(start, start, dont_ignore_monsters, hull, player->edict(), &trace);
	if ( trace.fStartSolid || trace.fAllSolid ) return ReleasePathRejected("initial_hull_blocked", player, destination, start, duck, 0, trace, NULL, diagnostic);
	int steps = (int)(delta.Length2D() / 8) + 1;
	for ( int step = 1; step <= steps; ++step )
	{
		Vector point = start + delta * ((float)step / steps);
		point.z = previous.z;
		UTIL_TraceHull(previous, point, dont_ignore_monsters, hull, player->edict(), &trace);
		if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction != 1.0f )
		{
			// Штатная ходьба может подняться на ступень или пологий участок пола
			Vector raised = previous + Vector(0, 0, 18);
			UTIL_TraceHull(previous, raised, dont_ignore_monsters, hull, player->edict(), &trace);
			if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction != 1.0f )
				return ReleasePathRejected("step_up_blocked", player, destination, raised, duck, step, trace, NULL, diagnostic);
			point.z += 18;
			UTIL_TraceHull(raised, point, dont_ignore_monsters, hull, player->edict(), &trace);
			if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction != 1.0f )
				return ReleasePathRejected("horizontal_step_hull_blocked", player, destination, point, duck, step, trace, NULL, diagnostic);
		}
		UTIL_TraceHull(point, Vector(point.x, point.y, previous.z - 18), ignore_monsters, hull, player->edict(), &trace);
		if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction == 1.0f || trace.vecPlaneNormal.z < 0.7f )
			return ReleasePathRejected("no_walkable_floor_within_step", player, destination, point, duck, step, trace, NULL, diagnostic);
		point = trace.vecEndPos + Vector(0, 0, 0.03125f);
		int contents = UTIL_PointContents(point);
		if ( contents == CONTENTS_SOLID || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA )
			return ReleasePathRejected("unsafe_floor_point_contents", player, destination, point, duck, step, trace, NULL, diagnostic);
		UTIL_TraceHull(point, point, dont_ignore_monsters, hull, player->edict(), &trace);
		if ( trace.fStartSolid || trace.fAllSolid )
			return ReleasePathRejected("floor_hull_blocked", player, destination, point, duck, step, trace, NULL, diagnostic);
		static const char *classes[] = { "trigger_hurt", "trigger_changelevel" };
		for ( int kind = 0; kind < ARRAYSIZE(classes); ++kind )
		{
			CBaseEntity *entity = NULL;
			while ( (entity = UTIL_FindEntityByClassname(entity, classes[kind])) != NULL )
			{
				if ( entity->pev->solid != SOLID_TRIGGER || !entity->m_pfnTouch || !ProbeTouchesAt(entity, point, duck) ) continue;
				// Из исходного обратного выхода разрешён только обычный шаг наружу
				if ( kind == 1 && ProbeTouchesAt(entity, start, duck) && !ProbeTouchesAt(entity, destination, duck) ) continue;
				return ReleasePathRejected("active_exit_or_hurt_on_path", player, destination, point, duck, step, trace, entity, diagnostic);
			}
		}
		previous = point;
	}
	if ( floorGoal ) *floorGoal = previous;
	return TRUE;
}

void PendingRelease( void )
{
	CBasePlayer *leader = Player(0), *waiting = Player(1);
	if ( pendingReleaseActive && (!waiting || !waiting->m_fCoopSpawnPending || !Ready(leader) ||
		gpGlobals->time >= pendingReleaseUntil || (pendingReleaseGoal - leader->pev->origin).Length2D() <= 12) )
	{
		pendingReleaseActive = FALSE;
		Begin("pending_release_movement"); EdgeFields();
		fputs(",\"action\":\"stop\",\"reason\":", report);
		String(waiting && !waiting->m_fCoopSpawnPending ? "second_player_spawned" : "short_walk_finished_or_interrupted");
		if ( leader ) { fputs(",\"origin\":", report); Vec(leader->pev->origin); }
		End();
	}
	if ( pendingReleaseAttempted || !probes[0].observed || probes[1].observed || !Ready(leader) ||
		!waiting || !waiting->m_fCoopSpawnPending || CoopStoryIsBusy() ) return;
	pendingReleaseAttempted = TRUE;
	static const Vector offsets[] =
	{
		Vector(48,0,0), Vector(-48,0,0), Vector(0,48,0), Vector(0,-48,0),
		Vector(48,48,0), Vector(48,-48,0), Vector(-48,48,0), Vector(-48,-48,0),
		Vector(96,0,0), Vector(-96,0,0), Vector(0,96,0), Vector(0,-96,0)
	};
	for ( int crouch = 0; crouch < 2; ++crouch )
	for ( int i = 0; i < ARRAYSIZE(offsets); ++i )
	{
		Vector target = leader->pev->origin + offsets[i];
		BOOL duck = crouch || (leader->pev->flags & FL_DUCKING);
		Vector floorGoal;
		if ( !ReleasePathSafe(leader, target, duck, TRUE, &floorGoal) ) continue;
		pendingReleaseGoal = floorGoal; pendingReleaseDuck = duck;
		pendingReleaseActive = TRUE; pendingReleaseUntil = gpGlobals->time + 3;
		Begin("pending_release_movement"); EdgeFields();
		fputs(",\"action\":\"start\",\"reason\":\"first_player_releases_narrow_arrival_for_pending_teammate\",\"input\":\"pfnRunPlayerMove\",\"point_teleport\":false,\"godmode\":false,\"noclip\":false,\"first_arrival_already_recorded\":true,\"clear_hull_path\":true,\"supported_path\":true,\"existing_reverse_trigger_exit_only\":true,\"origin\":", report);
		Vec(leader->pev->origin); fputs(",\"goal\":", report); Vec(floorGoal);
		fprintf(report, ",\"duck_input\":%s,\"max_step_height\":18", duck ? "true" : "false"); End();
		return;
	}
	Begin("pending_release_movement"); EdgeFields();
	fputs(",\"action\":\"unavailable\",\"reason\":\"no_short_clear_supported_path_without_new_exit_or_hurt_contact\"", report); End();
}

CBaseEntity *ReverseTrigger( void )
{
	if ( edgeCount <= 17 || strcmp(STRING(gpGlobals->mapname), Current().destination) ) return NULL;
	const Edge &edge = edges[17];
	CBaseEntity *entity = NULL;
	while ( (entity = UTIL_FindEntityByClassname(entity, "trigger_changelevel")) != NULL )
		if ( !strcmp(STRING(entity->pev->model), edge.model) && !strcmp(STRING(entity->pev->targetname), edge.target) &&
			(entity->pev->absmin - edge.mins).Length() < 4 && (entity->pev->absmax - edge.maxs).Length() < 4 &&
			!entity->IsDormant() && !(entity->pev->flags & FL_KILLME) && entity->pev->solid == SOLID_TRIGGER && entity->m_pfnTouch ) return entity;
	return NULL;
}

BOOL ReversePathSafe( CBasePlayer *player, const Vector &start, const Vector &destination )
{
	if ( !player || !Finite(start) || !Finite(destination) ) return FALSE;
	TraceResult trace;
	int hull = (player->pev->flags & FL_DUCKING) ? head_hull : human_hull;
	UTIL_TraceHull(start, destination, dont_ignore_monsters, hull, player->edict(), &trace);
	if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction != 1.0f ) return FALSE;
	Vector delta = destination - start;
	int steps = (int)(delta.Length() / 8) + 1;
	for ( int step = 0; step <= steps; ++step )
	{
		Vector point = start + delta * ((float)step / steps);
		int contents = UTIL_PointContents(point);
		if ( contents == CONTENTS_SOLID || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA ) return FALSE;
		UTIL_TraceHull(point, point - Vector(0, 0, 18), ignore_monsters, hull, player->edict(), &trace);
		if ( trace.fStartSolid || trace.fAllSolid || trace.flFraction == 1.0f || trace.vecPlaneNormal.z < 0.7f ) return FALSE;
		static const char *classes[] = { "trigger_hurt", "trigger_changelevel" };
		for ( int kind = 0; kind < ARRAYSIZE(classes); ++kind )
		{
			CBaseEntity *entity = NULL;
			while ( (entity = UTIL_FindEntityByClassname(entity, classes[kind])) != NULL )
			{
				if ( entity->IsDormant() || entity->pev->solid != SOLID_TRIGGER || !entity->m_pfnTouch ||
					!ProbeTouchesAt(entity, point, (player->pev->flags & FL_DUCKING) != 0) ) continue;
				if ( kind == 0 && entity->pev->dmg <= 0 ) continue;
				// Разрешён только проверяемый штатный обратный выход
				if ( kind == 1 && entity->entindex() == reverseTriggerIndex ) continue;
				return FALSE;
			}
		}
	}
	return TRUE;
}

BOOL ReverseModelTouchAt( CBasePlayer *player, CBaseEntity *trigger, const Vector &origin )
{
	if ( !player || !trigger || !Finite(origin) ) return FALSE;
	TraceResult trace;
	UTIL_TraceModel(origin, origin, (player->pev->flags & FL_DUCKING) ? head_hull : human_hull, trigger->edict(), &trace);
	return trace.fStartSolid || trace.fAllSolid || trace.flFraction < 1.0f;
}

void ReverseEvent( const char *action, BOOL canTransition )
{
	Begin("aux_reverse_guard_phase"); EdgeFields(); fputs(",\"action\":", report); String(action);
	fprintf(report, ",\"phase\":%d,\"probe\":0,\"reverse_edge_id\":17,\"reverse_trigger_edict\":%d,\"spawn_reference_time\":%.4f,\"since_spawn\":%.4f,\"can_transition\":%s,\"native_early_touch\":%s,\"point_teleport\":false,\"manual_touch\":false,\"godmode\":false,\"noclip\":false,\"input\":\"pfnRunPlayerMove\",\"origin\":",
		reversePhase, reverseTriggerIndex, reverseSpawnAt, gpGlobals->time - reverseSpawnAt, canTransition ? "true" : "false", reverseEarlyTouch ? "true" : "false");
	CBasePlayer *player = Player(0);
	if ( player ) Vec(player->pev->origin); else fputs("null", report);
	fputs(",\"inside_goal\":", report); Vec(reverseInside);
	fputs(",\"outside_goal\":", report); Vec(reverseOutside); End();
}

void FinishReverse( const char *status, const char *reason )
{
	Sample("aux_reverse_guard_end");
	Begin("aux_reverse_guard_result"); EdgeFields(); fputs(",\"status\":", report); String(status);
	fputs(",\"reason\":", report); String(reason);
	fprintf(report, ",\"outward_edge_id\":16,\"return_edge_id\":17,\"outward_pair_verified\":%s,\"native_early_touch\":%s,\"post_grace_latch_verified\":%s,\"exit_release_verified\":%s,\"native_return_requested\":%s,\"return_pair_verified\":%s,\"one_way_matrix_result_emitted\":false,\"destination_teleport\":false,\"manual_touch\":false,\"manual_latch_clear\":false,\"network_signon_checked\":false,\"steam_auth_checked\":false",
		reverseOutwardVerified ? "true" : "false", reverseEarlyTouch ? "true" : "false", reverseHoldVerified ? "true" : "false",
		reverseExitVerified ? "true" : "false", reverseReturnRequested ? "true" : "false",
		reverseReturning && probes[0].observed && probes[1].observed && probes[0].equal && probes[1].equal && probes[0].positionOK && probes[1].positionOK ? "true" : "false");
	fprintf(report, ",\"early_touch_since_spawn\":%.4f", reverseEarlyTouch ? reverseEarlyTouchAt - reverseSpawnAt : -1.0f);
	End();
	Stop(!strcmp(status, "passed") ? "auxiliary_complete" : "auxiliary_failed");
}

BOOL StartReverseWalk( void )
{
	CBaseEntity *trigger = ReverseTrigger();
	CBasePlayer *player = Player(0);
	if ( !trigger || !Ready(player) || !Ready(Player(1)) || !Hull(player) || !Hull(Player(1)) ||
		!probes[0].floorSupported || !probes[1].floorSupported || !probes[0].firstReadyAt ) return FALSE;
	reverseTriggerIndex = trigger->entindex(); reverseSpawnAt = probes[0].firstReadyAt;
	reverseOutside = player->pev->origin;
	// Край AABB составного триггера не гарантирует контакт с его настоящим brush
	reverseInside = edges[17].origin[0];
	if ( trigger->Intersects(player) || gpGlobals->time >= reverseSpawnAt + 0.9f ||
		!ReverseModelTouchAt(player, trigger, reverseInside) || ReverseModelTouchAt(player, trigger, reverseOutside) ||
		!ReversePathSafe(player, player->pev->origin, reverseInside) || !ReversePathSafe(player, reverseInside, reverseOutside) ) return FALSE;
	reverseOutwardVerified = TRUE; reversePhase = REVERSE_ENTER;
	reversePhaseAt = reverseStartedAt = gpGlobals->time;
	Begin("aux_reverse_guard_path"); EdgeFields();
	fprintf(report, ",\"reverse_edge_id\":17,\"reverse_bsp_entity\":%d,\"reverse_trigger_edict\":%d,\"model\":", edges[17].bspIndex, reverseTriggerIndex); String(STRING(trigger->pev->model));
	fputs(",\"mins\":", report); Vec(trigger->pev->absmin);
	fputs(",\"maxs\":", report); Vec(trigger->pev->absmax);
	fputs(",\"clear_hull_path\":true,\"supported_path\":true,\"floor_trace_depth\":18,\"no_positive_hurt_contact\":true,\"other_exits_excluded\":true,\"actual_brush_contact_at_inside_goal\":true,\"actual_brush_contact_at_outside_goal\":false,\"point_teleport\":false,\"spawn_reference\":\"first_restore_observer_same_spawn_frame\"", report); End();
	ReverseEvent("enter_during_spawn_grace", FALSE);
	return TRUE;
}

void ReverseWalkThink( void )
{
	CBasePlayer *player = Player(0); CBaseEntity *trigger = ReverseTrigger();
	if ( reverseMovementUnsafe || !Ready(player) || !Ready(Player(1)) || !Hull(player) || !Hull(Player(1)) || !Floor(player) || !Floor(Player(1)) || !trigger )
	{ FinishReverse("failed", "reverse_walk_runtime_path_or_players_unsafe"); return; }
	if ( gpGlobals->time > reverseStartedAt + 12 ) { FinishReverse("failed", "reverse_walk_timeout"); return; }
	if ( reversePhase == REVERSE_ENTER )
	{
		if ( gpGlobals->time >= reverseSpawnAt + 1.5f && !reverseEarlyTouch )
		{ FinishReverse("failed", "native_reverse_touch_not_seen_during_spawn_grace"); return; }
		if ( reverseEarlyTouch && (player->pev->origin - reverseInside).Length2D() <= 4 && ReverseModelTouchAt(player, trigger, player->pev->origin) )
		{
			reversePhase = REVERSE_HOLD; reversePhaseAt = gpGlobals->time;
			ReverseEvent("hold_inside_reverse_trigger", FALSE);
		}
	}
	else if ( reversePhase == REVERSE_HOLD && gpGlobals->time >= reverseSpawnAt + 2.0f )
	{
		if ( !ReverseModelTouchAt(player, trigger, player->pev->origin) || CoopCampaignCanTransition(player) )
		{ FinishReverse("failed", "reverse_latch_not_retained_after_spawn_grace"); return; }
		reverseHoldVerified = TRUE; ReverseEvent("post_grace_transition_still_blocked", FALSE);
		reversePhase = REVERSE_LEAVE; reversePhaseAt = gpGlobals->time;
		ReverseEvent("walk_outside_reverse_trigger", FALSE);
	}
	else if ( reversePhase == REVERSE_LEAVE && (player->pev->origin - reverseOutside).Length2D() <= 4 && !trigger->Intersects(player) )
	{
		reversePhase = REVERSE_OUTSIDE; reversePhaseAt = gpGlobals->time;
		ReverseEvent("settle_fully_outside_reverse_trigger", FALSE);
	}
	else if ( reversePhase == REVERSE_OUTSIDE && gpGlobals->time >= reversePhaseAt + 0.25f )
	{
		if ( trigger->Intersects(player) || !CoopCampaignCanTransition(player) )
		{ FinishReverse("failed", "reverse_transition_not_released_after_exit"); return; }
		reverseExitVerified = TRUE; ReverseEvent("exit_releases_transition", TRUE);
		reversePhase = REVERSE_REENTER; reversePhaseAt = gpGlobals->time;
		ReverseEvent("walk_back_into_reverse_trigger", TRUE);
	}
}

void VerifyReverseCommand( void )
{
	if ( !AllowedServer() || !active || CMD_ARGC() != 1 || requestedCount != 1 || completed || farMode || Current().id != 16 ||
		edgeCount <= 17 || strcmp(Current().source, "c1a0a") || strcmp(Current().destination, "c1a0b") ||
		strcmp(edges[17].source, "c1a0b") || strcmp(edges[17].destination, "c1a0a") ||
		strcmp(Current().landmark, edges[17].landmark) || probes[0].observed || probes[1].observed || reverseEnabled )
	{ ALERT(at_console, "Transition check: issue hctcheck_verify_reverse immediately after hctcheck_start 16 1 with far=0, before destination spawn\n"); return; }
	reverseEnabled = TRUE; reversePhase = REVERSE_ARMED;
	Begin("aux_reverse_guard_armed"); EdgeFields();
	fputs(",\"outward_edge_id\":16,\"return_edge_id\":17,\"matrix_result\":false,\"destination_teleport\":false,\"manual_touch\":false,\"manual_latch_clear\":false", report); End();
}

void MoveProbes( void )
{
	int milliseconds = (int)(gpGlobals->frametime * 1000 + 0.5f);
	if ( milliseconds < 1 ) milliseconds = 1; if ( milliseconds > 255 ) milliseconds = 255;
	for ( int i = 0; i < 2; ++i )
	{
		CBasePlayer *player = Player(i);
		if ( !player ) continue;
		Vector angles = player->pev->v_angle;
		float forward = 0;
		unsigned short buttons = 0;
		if ( i == 0 && reverseEnabled && !reverseReturning && Ready(player) &&
			(reversePhase == REVERSE_ENTER || reversePhase == REVERSE_LEAVE || reversePhase == REVERSE_REENTER) )
		{
			Vector goal = reversePhase == REVERSE_LEAVE ? reverseOutside : reverseInside;
			Vector delta = goal - player->pev->origin; delta.z = 0;
			if ( !ReversePathSafe(player, player->pev->origin, goal) ) reverseMovementUnsafe = TRUE;
			else if ( delta.Length2D() > 4 ) { angles = Vector(0, UTIL_VecToAngles(delta).y, 0); forward = 96; }
		}
		else if ( i == 0 && pendingReleaseActive && Ready(player) )
		{
			Vector delta = pendingReleaseGoal - player->pev->origin;
			delta.z = 0;
			if ( !ReleasePathSafe(player, pendingReleaseGoal, pendingReleaseDuck) )
			{
				pendingReleaseActive = FALSE;
				Begin("pending_release_movement"); EdgeFields();
				fputs(",\"action\":\"stop\",\"reason\":\"path_became_blocked_or_unsafe\",\"origin\":", report);
				Vec(player->pev->origin); End();
			}
			else
			{
				// PM_Duck делит команду на три, полный шаг нужен для преодоления обычного трения
				angles = Vector(0, UTIL_VecToAngles(delta).y, 0); forward = pendingReleaseDuck ? 288 : 96;
				if ( pendingReleaseDuck ) buttons |= IN_DUCK;
			}
		}
		g_engfuncs.pfnRunPlayerMove(player->edict(), angles, forward, 0, 0, buttons, 0, (byte)milliseconds);
	}
}

void StartCommand( void )
{
	if ( !AllowedServer() || active || CoopCheckActive() || !g_pGameRules || !g_pGameRules->IsCoOp() || gpGlobals->maxClients < 2 ||
		CVAR_GET_FLOAT("sv_lan") != 1 || !g_engfuncs.pfnGetPlayerAuthId || !g_engfuncs.pfnSetView || !g_engfuncs.pfnCrosshairAngle )
	{ ALERT(at_console, "Transition check: use an idle dedicated LAN hlcoop_tc server with two slots\n"); return; }
	for ( int slot = 1; slot <= gpGlobals->maxClients; ++slot )
		if ( GETPLAYERUSERID(INDEXENT(slot)) > 0 )
		{ ALERT(at_console, "Transition check: disconnect existing clients first\n"); return; }
	int first = 0, count = 1, seconds = 5, farValue = 0;
	if ( !LoadCatalog() || CMD_ARGC() > 5 ||
		(CMD_ARGC() > 1 && !Integer(CMD_ARGV(1), 0, edgeCount - 1, first)) ||
		(CMD_ARGC() > 2 && !Integer(CMD_ARGV(2), 1, edgeCount - first, count)) ||
		(CMD_ARGC() > 3 && !Integer(CMD_ARGV(3), 2, 120, seconds)) ||
		(CMD_ARGC() > 4 && !Integer(CMD_ARGV(4), 0, 1, farValue)) )
	{ ALERT(at_console, "Transition check: valid transition-edges.tsv required; hctcheck_start [first] [count] [dwell 2..120] [far 0|1]\n"); return; }
	char directory[1024], path[1100]; GET_GAME_DIR(directory); _snprintf(path, sizeof(path), "%s/transition-check.jsonl", directory); path[sizeof(path) - 1] = 0;
	report = fopen(path, "ab"); if ( !report ) { ALERT(at_console, "Transition check: cannot open JSONL\n"); return; }
	runId = (unsigned long)time(NULL); firstEdge = first; requestedCount = count; dwell = (float)seconds; farMode = farValue;
	completed = passed = failed = setupFailed = limitations = warnings = 0; active = TRUE;
	suppressedViews = suppressedCrosshair = 0;
	musicPlay = musicStop = 0;
	// Подмена действует только в локальной копии callbacks этой специальной проверочной DLL
	originalAuthId = g_engfuncs.pfnGetPlayerAuthId;
	g_engfuncs.pfnGetPlayerAuthId = LANAuthAdapter;
	originalSetView = g_engfuncs.pfnSetView;
	g_engfuncs.pfnSetView = SetViewAdapter;
	originalCrosshairAngle = g_engfuncs.pfnCrosshairAngle;
	g_engfuncs.pfnCrosshairAngle = CrosshairAngleAdapter;
	Begin("start"); fprintf(report, ",\"first_index\":%d,\"total\":%d,\"dwell_seconds\":%d,\"fake_clients\":2,\"far_teammate\":%s,\"scope\":\"fresh_source_original_trigger_two_fake_clients\",\"source_setup_modes\":[\"point_setup\",\"native_train_ride\",\"native_ambush\",\"point_staged_native_xen_gate_portal\"],\"identity_mode\":\"synthetic_lan_auth_adapter\",\"view_message_mode\":\"suppressed_for_owned_fake_probes\",\"music_message_mode\":\"suppressed_cd_play_stop_for_owned_fake_probes\",\"crosshair_message_mode\":\"suppressed_for_owned_fake_probes\",\"position_verification_modes\":[\"landmark_relative\",\"map_spawn_fallback\"],\"crosshair_protocol_checked\":false,\"music_protocol_checked\":false,\"camera_protocol_checked\":false,\"network_signon_checked\":false,\"steam_auth_checked\":false", first, count, seconds, farValue ? "true" : "false");
	fputs(",\"state_validation_modes\":[\"exact_transfer\",\"native_destination_weaponstrip\"]", report); End();
	ALERT(at_console, "Transition check: started %d edges at %d with two fake clients\n", count, first); QueueEdge();
}

void StopCommand( void )
{
	if ( !AllowedServer() ) return;
	if ( active ) Stop("stopped"); else ALERT(at_console, "Transition check: no active run\n");
}
}

BOOL CoopTransitionCheckActive( void ) { return active; }

void CoopTransitionCheckInit( void )
{
	if ( !AllowedServer() ) return;
	g_engfuncs.pfnAddServerCommand((char *)"hctcheck_start", StartCommand);
	g_engfuncs.pfnAddServerCommand((char *)"hctcheck_stop", StopCommand);
	g_engfuncs.pfnAddServerCommand((char *)"hctcheck_verify_reverse", VerifyReverseCommand);
}

void CoopTransitionCheckServerActivate( void )
{
	if ( !active ) return;
	Copy(observedActivation, sizeof(observedActivation), STRING(gpGlobals->mapname));
	activatedAt = gpGlobals->time;
	phaseTime = activatedAt;
	if ( phase == WAIT_SOURCE && !stricmp(Current().source, observedActivation) ) phase = SOURCE_WARMUP;
	else if ( reverseEnabled && reverseReturnRequested && phase == WAIT_DESTINATION && !stricmp(Current().source, observedActivation) ) phase = DESTINATION_WARMUP;
	else if ( phase == WAIT_DESTINATION && requested && !stricmp(Current().destination, observedActivation) ) phase = DESTINATION_WARMUP;
	else unexpectedActivation = TRUE;
	Begin("server_activate"); EdgeFields(); fprintf(report, ",\"expected\":%s,\"transition_requested\":%s", unexpectedActivation ? "false" : "true", requested ? "true" : "false"); End();
}

void CoopTransitionCheckTransitionRequested( CBaseEntity *trigger, CBasePlayer *player, const char *map, const char *landmark )
{
	if ( active && reverseEnabled && !reverseReturning && reversePhase == REVERSE_REENTER && phase == STABILITY &&
		trigger && trigger->entindex() == reverseTriggerIndex && player == Player(0) &&
		!strcmp(map, Current().source) && !strcmp(landmark, edges[17].landmark) )
	{
		reverseReturning = reverseReturnRequested = TRUE; reversePhase = REVERSE_RETURN;
		Vector offset = edges[17].destinationLandmark - edges[17].sourceLandmark;
		for ( int i = 0; i < 2; ++i )
		{
			CBasePlayer *member = Player(i);
			if ( !member ) { reverseReturnRequested = FALSE; break; }
			Probe &probe = probes[i];
			probe.sourceOrigin = member->pev->origin; probe.personal = member == player || InVolume(member, FALSE);
			probe.expectedOrigin = (probe.personal ? member->pev->origin : player->pev->origin) + offset;
			Capture(member, probe.sourceState);
			probe.manualSignon = probe.observed = probe.equal = probe.transformedEqual = probe.positionOK = probe.passiveDeath = probe.storyFrozen =
				probe.floorSupported = probe.farFromLeader = probe.nameRecovered = probe.flagsRecovered = probe.contentsClear = FALSE;
			probe.firstReadyAt = 0;
		}
		phase = WAIT_DESTINATION; phaseTime = gpGlobals->time;
		Begin("aux_reverse_transition_requested"); EdgeFields();
		fprintf(report, ",\"return_edge_id\":17,\"trigger_edict\":%d,\"source_activator_probe\":0,\"point_teleport\":false,\"manual_touch\":false,\"source_pair_captured\":%s", reverseTriggerIndex, reverseReturnRequested ? "true" : "false"); End();
		Sample("aux_reverse_before_return"); return;
	}
	if ( !active || phase != WAIT_DESTINATION || !trigger || trigger->entindex() != triggerIndex ||
		(player != Player(0) && (!NativeTrainRide() || player != Player(1))) || strcmp(map, Current().destination) || strcmp(landmark, Current().landmark) ) return;
	requested = TRUE;
	sourceActivator = player == Player(1) ? 1 : 0;
	Vector offset = MapSpawnFallback() ? g_vecZero : Current().destinationLandmark - Current().sourceLandmark;
	for ( int i = 0; i < 2; ++i )
	{
		CBasePlayer *probe = Player(i); if ( !probe ) { requested = FALSE; break; }
		probes[i].sourceOrigin = probe->pev->origin; probes[i].personal = probe == player || InVolume(probe, FALSE);
		probes[i].expectedOrigin = (probes[i].personal ? probe->pev->origin : player->pev->origin) + offset;
		Capture(probe, probes[i].sourceState);
	}
	if ( NativeTrainRide() )
	{
		CFuncTrackTrain *train = IntroTrain();
		trainSourceVerified = train && TrainFloor(Player(0), train) && TrainFloor(Player(1), train);
		trainMovementVerified = train && (train->pev->origin - trainSetupOrigin).Length() >= 64;
		Begin("native_train_exit"); EdgeFields();
		fprintf(report, ",\"activation\":\"native_train_ride\",\"source_activator_probe\":%d,\"point_teleport\":false,\"passengers_on_train\":%s,\"train_moved\":%s,\"setup_train_origin\":",
			sourceActivator, trainSourceVerified ? "true" : "false", trainMovementVerified ? "true" : "false");
		Vec(trainSetupOrigin); TrainFields(); End();
	}
	if ( NativeAmbush() )
	{
		float elapsed = gpGlobals->time - phaseTime;
		BOOL delayObserved = elapsed >= 11.5f;
		BOOL betaOutsideEntry = !AmbushBoxIntersects(Player(1), Vector(-2160, -848, 788), Vector(-1840, -352, 916));
		BOOL betaOutsideTeleport = !AmbushBoxIntersects(Player(1), Vector(-2160, -848, 788), Vector(-1776, -352, 916));
		float separation = (probes[1].sourceOrigin - probes[0].sourceOrigin).Length();
		ambushSourceVerified = delayObserved && (!farMode || (betaOutsideEntry && betaOutsideTeleport && separation > 256));
		Begin("native_ambush_exit"); EdgeFields();
		fprintf(report, ",\"scene_elapsed_seconds\":%.4f,\"native_delay_observed\":%s,\"gotodrag_direct_use\":false,\"source_activator_probe\":%d,\"source_origins\":[",
			elapsed, delayObserved ? "true" : "false", sourceActivator);
		Vec(probes[0].sourceOrigin); fputc(',', report); Vec(probes[1].sourceOrigin); fputs("]", report);
		fprintf(report, ",\"far_teammate\":%s,\"scene_participants\":%d,\"beta_point_staging\":%s,\"beta_outside_entry\":%s,\"beta_outside_teleport\":%s,\"source_separation\":%.4f,\"beta_start_origin\":",
			farMode ? "true" : "false", farMode ? 1 : 2, farMode ? "false" : "true", betaOutsideEntry ? "true" : "false",
			betaOutsideTeleport ? "true" : "false", separation);
		Vec(ambushBetaStart);
		fprintf(report, ",\"source_scene_verified\":%s", ambushSourceVerified ? "true" : "false"); End();
	}
	if ( NativeXenPortal() )
	{
		CBaseEntity *hurt = XenBrush("trigger_hurt", "*99", "solved"), *blocker = XenBrush("func_wall_toggle", "*163", "blocker");
		float elapsed = gpGlobals->time - xenPortalTouchAt;
		xenSourceVerified = xenGateTouched && xenPortalTouched && xenStage == 2 && elapsed >= 3.95f &&
			hurt && hurt->pev->solid == SOLID_NOT && blocker && blocker->pev->solid == SOLID_BSP &&
			Ready(Player(0)) && Ready(Player(1)) && Hull(Player(0)) && Hull(Player(1)) && InVolume(Player(0), FALSE) && InVolume(Player(1), FALSE);
		Begin("native_xen_portal_exit"); EdgeFields();
		fprintf(report, ",\"activation\":\"point_staged_native_xen_gate_portal\",\"source_scene_verified\":%s,\"portal_elapsed_seconds\":%.4f,\"native_delay_observed\":%s,\"direct_exit_use\":false,\"manual_world_toggle\":false,\"health_repair_after_staging\":false,\"source_states_logged_in_transition_requested\":true,\"source_origins\":[",
			xenSourceVerified ? "true" : "false", elapsed, elapsed >= 3.95f ? "true" : "false");
		Vec(probes[0].sourceOrigin); fputc(',', report); Vec(probes[1].sourceOrigin); fputc(']', report);
		XenSceneFields(); End();
	}
	Sample("transition_requested");
}

BOOL CoopTransitionCheckThink( void )
{
	if ( !active ) return FALSE;
	if ( unexpectedActivation ) { Finish(requested ? "transition_failed" : "setup_not_reached", "unexpected_map_activation_or_bounce"); return TRUE; }
	if ( phase == WAIT_SOURCE ) return TRUE;
	if ( phase == SOURCE_WARMUP || phase == DESTINATION_WARMUP )
	{
		if ( gpGlobals->time <= activatedAt || gpGlobals->frametime > 0.25f ) return TRUE;
		phaseTime = gpGlobals->time; nextSample = phaseTime + 1;
		if ( phase == SOURCE_WARMUP )
		{
			CoopCampaignPrepareDirectStart(Current().source);
			if ( !Spawn(0) || !Spawn(1) ) { Finish("protocol_limitation", "fakeclient_creation_failed"); return TRUE; }
			phase = SOURCE_READY; Sample("source_spawn");
		}
		else phase = DESTINATION_READY;
	}
	if ( phase == DESTINATION_READY && gpGlobals->time >= phaseTime + 0.5f )
		for ( int i = 0; i < 2; ++i )
		{
			edict_t *entity = Identity(i); CBasePlayer *player = Player(i);
			if ( !entity && !probes[i].manualSignon && !strcmp(probes[i].auth, "STEAM_ID_LAN") &&
				!strcmp(probes[i].sourceRawAuth, "BOT") && originalAuthId )
			{
				edict_t *raw = probes[i].slot >= 1 && probes[i].slot <= gpGlobals->maxClients ? INDEXENT(probes[i].slot) : NULL;
				const char *rawAuth = raw ? RawAuth(raw) : NULL;
				if ( raw && !raw->free && GETPLAYERUSERID(raw) == probes[i].user &&
					rawAuth && !strcmp(rawAuth, "STEAM_ID_LAN") &&
					!(raw->v.flags & (FL_CLIENT | FL_FAKECLIENT)) && !*STRING(raw->v.netname) )
				{
					char *info = g_engfuncs.pfnGetInfoKeyBuffer(raw);
					const char *name = info ? g_engfuncs.pfnInfoKeyValue(info, "name") : NULL;
					if ( !name || !*name || !strcmp(name, NAMES[i]) )
					{
						// Движок сохранил клиентский слот, но очистил entvars перед новым signon
						Sample("manual_fake_signon_raw_before");
						raw->v.netname = ALLOC_STRING(NAMES[i]);
						raw->v.flags |= FL_CLIENT | FL_FAKECLIENT;
						probes[i].nameRecovered = probes[i].flagsRecovered = TRUE;
						Sample("manual_fake_signon_raw_after");
						entity = Identity(i); player = Player(i);
						if ( !entity ) { Finish("protocol_limitation", "fake_slot_rebind_identity_mismatch"); return TRUE; }
					}
				}
			}
			if ( entity && !probes[i].manualSignon && (!player || (!player->m_fCoopHasSpawned && !player->m_fCoopSpawnPending)) )
			{
				// Эмуляция signon сохраняет только доказанно прежние slot, userid и auth
				probes[i].manualSignon = TRUE; entity->v.flags |= FL_CLIENT | FL_FAKECLIENT;
				Begin("manual_fake_signon"); EdgeFields(); fprintf(report, ",\"probe\":%d,\"slot\":%d,\"userid\":%d", i, probes[i].slot, probes[i].user); End();
				ClientPutInServer(entity);
			}
		}
	if ( phase == SOURCE_READY )
	{
		MoveProbes();
		if ( !NativeTrainRide() ) StageSource();
		if ( Ready(Player(0)) && Ready(Player(1)) && Hull(Player(0)) && Hull(Player(1)) && CoopCampaignCanTransition(Player(0)) )
		{
			if ( NativeTrainRide() )
			{
				if ( !SetupTrainRide() ) Finish("setup_not_reached", "native_train_passengers_or_trigger_unavailable");
			}
			else if ( NativeAmbush() )
			{
				if ( !SetupAmbush() ) Finish("setup_not_reached", "native_ambush_entry_or_trigger_unavailable");
			}
			else if ( NativeXenPortal() )
			{
				if ( !SetupXenPortal() ) Finish("setup_not_reached", "xen_native_gate_source_staging_unavailable");
			}
			else if ( !Setup() ) Finish("setup_not_reached", "unavailable_trigger_or_unclear_setup_floor_hull_volume");
			return TRUE;
		}
		if ( gpGlobals->time > phaseTime + 15 ) { Finish("setup_not_reached", "source_players_not_ready"); return TRUE; }
	}
	else if ( phase == WAIT_DESTINATION )
	{
		if ( NativeTrainRide() || NativeAmbush() || NativeXenPortal() ) MoveProbes();
		if ( NativeXenPortal() )
		{
			XenPortalThink();
			if ( !active ) return TRUE;
		}
		if ( gpGlobals->time > phaseTime + (NativeTrainRide() ? 240 : (NativeAmbush() ? 25 : 15)) )
		{
			Finish(requested ? "transition_failed" : "setup_not_reached",
				requested ? "requested_changelevel_did_not_activate_destination" :
				(NativeTrainRide() ? "native_train_did_not_reach_original_exit" :
				(NativeAmbush() ? "native_ambush_did_not_request_original_exit" : "original_trigger_did_not_request_transition")));
			return TRUE;
		}
	}
	else if ( phase == DESTINATION_READY || phase == STABILITY )
	{
		if ( NativeTrainRide() && (!trainSourceVerified || !trainMovementVerified) )
		{ Finish("transition_failed", "native_train_source_passenger_or_motion_mismatch"); return TRUE; }
		if ( NativeAmbush() && !ambushSourceVerified )
		{ Finish("setup_not_reached", "native_ambush_source_scene_not_verified"); return TRUE; }
		if ( NativeXenPortal() && !xenSourceVerified )
		{ Finish("setup_not_reached", "xen_native_gate_portal_source_scene_not_verified"); return TRUE; }
		for ( int i = 0; i < 2; ++i )
		{
			CBasePlayer *player = Player(i);
			Observe(i, i ? "beta_first_ready" : "alpha_first_ready");
			if ( player && probes[i].observed && !player->IsAlive() ) probes[i].passiveDeath = TRUE;
			if ( player && (player->pev->flags & FL_FROZEN) && CoopStoryIsBusy() ) probes[i].storyFrozen = TRUE;
			if ( player && !probes[i].observed && !player->IsAlive() && player->m_fCoopHasSpawned )
			{ Finish("setup_not_reached", "arrival_hazard_before_first_snapshot"); return TRUE; }
		}
		if ( phase == DESTINATION_READY && probes[0].observed && probes[1].observed )
		{
			if ( (!probes[0].equal && !probes[0].transformedEqual) || (!probes[1].equal && !probes[1].transformedEqual) )
			{ Finish("transition_failed", "individual_state_mismatch"); return TRUE; }
			if ( NativeTrainRide() && (!probes[0].firstTrainFloor || !probes[1].firstTrainFloor) )
			{ Finish("transition_failed", "native_train_arrival_floor_mismatch"); return TRUE; }
			if ( !probes[0].positionOK || !probes[1].positionOK ||
				(MapSpawnFallback() ? (probes[1].firstOrigin - probes[0].firstOrigin).Length() > 256 : !CoopCampaignHasArrival()) )
			{ Finish("transition_failed", MapSpawnFallback() ? "map_spawn_fallback_mismatch" : "landmark_or_group_arrival_mismatch"); return TRUE; }
			probes[1].farFromLeader = (probes[1].firstOrigin - probes[0].firstOrigin).Length() > 256;
			phase = STABILITY; phaseTime = gpGlobals->time;
			Sample("arrival_pair_observed");
			if ( reverseEnabled )
			{
				if ( reverseReturning )
				{
					reversePhase = REVERSE_STABLE;
					Begin("aux_reverse_return_pair"); EdgeFields();
					fputs(",\"two_players_restored\":true,\"individual_state_equal\":true,\"landmark_positions_verified\":true", report); End();
				}
				else if ( reversePhase == REVERSE_ARMED && !StartReverseWalk() )
				{ FinishReverse("failed", "reverse_walk_initial_geometry_or_grace_window_unavailable"); return TRUE; }
			}
		}
		if ( !Identity(0) || !Identity(1) )
		{
			if ( phase == STABILITY || gpGlobals->time > phaseTime + 3 ) { Finish("protocol_limitation", "fakeclient_identity_not_preserved"); return TRUE; }
		}
		else if ( phase == DESTINATION_READY && gpGlobals->time > phaseTime + 15 ) { Finish("transition_failed", "destination_players_not_ready"); return TRUE; }
		if ( !reverseEnabled || reverseReturning ) PendingRelease();
		MoveProbes();
		if ( reverseEnabled && !reverseReturning && phase == STABILITY )
		{
			ReverseWalkThink();
			if ( !active ) return TRUE;
		}
		if ( phase == STABILITY && (!Player(0) || !Player(1)) ) { Finish("protocol_limitation", "probe_disappeared"); return TRUE; }
		for ( int i = 0; i < 2; ++i )
			if ( Player(i) && probes[i].observed && !Player(i)->IsAlive() ) probes[i].passiveDeath = TRUE;
		if ( phase == STABILITY && gpGlobals->time >= phaseTime + dwell && (!reverseEnabled || reverseReturning) )
		{
			if ( NativeTrainRide() )
			{
				BOOL pathLive = TrainPathLive();
				if ( !pathLive && !trainDwellExtended )
				{
					// Штатный лифт временно отсоединяет поезд от пути, наблюдаем до его возвращения
					trainDwellExtended = TRUE;
					Begin("native_train_dwell_extension"); EdgeFields();
					fprintf(report, ",\"action\":\"wait_for_native_path_recovery\",\"minimum_dwell_seconds\":%.4f,\"maximum_extra_seconds\":30,\"physics_or_world_changed\":false", dwell);
					TrainFields(); End();
				}
				if ( trainDwellExtended )
				{
					if ( !pathLive ) trainPathRecoveredAt = 0;
					else if ( !trainPathRecoveredAt )
					{
						trainPathRecoveredAt = gpGlobals->time;
						Begin("native_train_dwell_extension"); EdgeFields();
						fputs(",\"action\":\"native_path_recovered\",\"settle_seconds\":1,\"physics_or_world_changed\":false", report);
						TrainFields(); End();
					}
					if ( !pathLive || gpGlobals->time < trainPathRecoveredAt + 1 )
					{
						if ( gpGlobals->time > phaseTime + dwell + 30 )
						{ Finish("transition_failed", "native_train_path_did_not_recover_after_minimum_dwell"); return TRUE; }
						if ( gpGlobals->time >= nextSample ) { nextSample = gpGlobals->time + 1; Sample("periodic"); }
						return TRUE;
					}
				}
			}
			if ( reverseEnabled )
			{
				if ( reverseOutwardVerified && reverseEarlyTouch && reverseHoldVerified && reverseExitVerified && reverseReturnRequested && reversePhase == REVERSE_STABLE )
					FinishReverse("passed", "native_early_touch_blocked_exit_released_two_players_returned");
				else FinishReverse("failed", "reverse_guard_auxiliary_proof_incomplete");
			}
			else Finish("passed", "two_players_restored_no_auto_bounce");
			return TRUE;
		}
	}
	if ( gpGlobals->time >= nextSample ) { nextSample = gpGlobals->time + 1; Sample("periodic"); }
	return TRUE;
}

void CoopTransitionCheckPlayerRestored( CBasePlayer *player )
{
	if ( !active || !requested || (phase != DESTINATION_WARMUP && phase != DESTINATION_READY) ) return;
	for ( int i = 0; i < 2; ++i )
		if ( player == Player(i) ) Observe(i, i ? "beta_post_restore" : "alpha_post_restore");
}

void CoopTransitionCheckReverseTouch( CBaseEntity *trigger, CBaseEntity *player )
{
	if ( !active || !reverseEnabled || reverseReturning || phase != STABILITY || !trigger ||
		trigger->entindex() != reverseTriggerIndex || player != Player(0) ) return;
	if ( reversePhase == REVERSE_ENTER && !reverseEarlyTouch && gpGlobals->time < reverseSpawnAt + 1.5f )
	{
		reverseEarlyTouch = TRUE; reverseEarlyTouchAt = gpGlobals->time;
		Begin("aux_reverse_native_touch"); EdgeFields();
		fprintf(report, ",\"reverse_edge_id\":17,\"trigger_edict\":%d,\"probe\":0,\"during_spawn_grace\":true,\"since_spawn\":%.4f,\"manual_touch\":false,\"point_teleport\":false", reverseTriggerIndex, gpGlobals->time - reverseSpawnAt); End();
	}
}

void CoopTransitionCheckXenTouch( CBaseEntity *trigger, CBaseEntity *player )
{
	if ( !active || !report || !NativeXenPortal() || phase != WAIT_DESTINATION || !trigger ||
		(player != Player(0) && player != Player(1)) ) return;
	const char *stage = NULL;
	if ( xenStage == 1 && trigger->entindex() == xenGateEdict && !xenGateTouched )
	{
		xenGateTouched = TRUE; xenGateTouchAt = gpGlobals->time; stage = "gate";
	}
	else if ( xenStage == 2 && trigger->entindex() == xenPortalEdict && !xenPortalTouched )
	{
		xenPortalTouched = TRUE; xenPortalTouchAt = gpGlobals->time; stage = "portal";
	}
	if ( !stage ) return;
	Begin("native_xen_trigger_touch"); EdgeFields();
	fputs(",\"activation\":\"point_staged_native_xen_gate_portal\",\"stage\":", report); String(stage);
	fprintf(report, ",\"trigger_edict\":%d,\"source_activator_probe\":%d,\"manual_touch\":false,\"point_teleport_at_callback\":false,\"origin\":", trigger->entindex(), player == Player(0) ? 0 : 1);
	Vec(player->pev->origin); End();
}

void CoopTransitionCheckTrainBlocked( CBaseEntity *train, CBaseEntity *blocker )
{
	if ( !active || !report || !AllowedServer() || !NativeTrainRide() || !train || !blocker || train != IntroTrain() ) return;
	static unsigned long observedRun;
	static int observedEdge = -1;
	static char observedMap[32];
	static int observedBlockers[64], blockerCalls[64];
	static float nextLogAt[64];
	static int totalCalls;
	if ( observedRun != runId || observedEdge != Current().id || strcmp(observedMap, STRING(gpGlobals->mapname)) )
	{
		observedRun = runId; observedEdge = Current().id; Copy(observedMap, sizeof(observedMap), STRING(gpGlobals->mapname));
		memset(observedBlockers, 0xff, sizeof(observedBlockers)); memset(blockerCalls, 0, sizeof(blockerCalls));
		memset(nextLogAt, 0, sizeof(nextLogAt)); totalCalls = 0;
	}
	++totalCalls;
	int bucket = blocker->entindex() % ARRAYSIZE(observedBlockers);
	if ( observedBlockers[bucket] != blocker->entindex() )
	{
		observedBlockers[bucket] = blocker->entindex(); blockerCalls[bucket] = 0; nextLogAt[bucket] = 0;
	}
	++blockerCalls[bucket];
	if ( gpGlobals->time < nextLogAt[bucket] ) return;
	nextLogAt[bucket] = gpGlobals->time + 1;
	Begin("native_train_blocked"); EdgeFields();
	fprintf(report, ",\"activation\":\"native_train_ride\",\"checker_phase\":%d,\"elapsed\":%.4f,\"observer_only\":true,\"before_native_blocked_handler\":true,\"blocked_calls_total\":%d,\"blocked_calls_for_entity\":%d,\"per_entity_log_interval_seconds\":1,\"blocker_grounded_on_train\":%s",
		phase, gpGlobals->time - phaseTime, totalCalls, blockerCalls[bucket],
		(blocker->pev->flags & FL_ONGROUND) && blocker->pev->groundentity == train->edict() ? "true" : "false");
	auto entityFields = []( CBaseEntity *entity )
	{
		if ( !entity ) { fputs("null", report); return; }
		fprintf(report, "{\"edict\":%d,\"classname\":", entity->entindex()); String(STRING(entity->pev->classname));
		fputs(",\"globalname\":", report); String(STRING(entity->pev->globalname));
		fputs(",\"targetname\":", report); String(STRING(entity->pev->targetname));
		fputs(",\"model\":", report); String(STRING(entity->pev->model));
		fprintf(report, ",\"model_index\":%d,\"solid\":%d,\"movetype\":%d,\"flags\":%d,\"is_player\":%s,\"groundentity_edict\":%d,\"ltime\":%.4f,\"nextthink\":%.4f,\"speed\":%.4f,\"origin\":",
			entity->pev->modelindex, entity->pev->solid, entity->pev->movetype, entity->pev->flags, entity->IsPlayer() ? "true" : "false",
			entity->pev->groundentity ? ENTINDEX(entity->pev->groundentity) : -1, entity->pev->ltime, entity->pev->nextthink, entity->pev->speed);
		Vec(entity->pev->origin);
		fputs(",\"angles\":", report); Vec(entity->pev->angles);
		fputs(",\"velocity\":", report); Vec(entity->pev->velocity);
		fputs(",\"mins\":", report); Vec(entity->pev->mins);
		fputs(",\"maxs\":", report); Vec(entity->pev->maxs);
		fputs(",\"absmin\":", report); Vec(entity->pev->absmin);
		fputs(",\"absmax\":", report); Vec(entity->pev->absmax);
		if ( entity->IsPlayer() )
		{
			fprintf(report, ",\"slot\":%d,\"userid\":%d,\"name\":", entity->entindex(), GETPLAYERUSERID(entity->edict()));
			String(STRING(entity->pev->netname));
		}
		fputc('}', report);
	};
	fputs(",\"train_entity\":", report); entityFields(train);
	fputs(",\"blocker\":", report); entityFields(blocker);
	fputs(",\"players\":[", report); entityFields(Player(0)); fputc(',', report); entityFields(Player(1)); fputc(']', report);
	TrainFields(); End();
}

void CoopTransitionCheckStripProducer( CBaseEntity *producer, float delay, int useType )
{
	if ( !NativeDestinationStripWindow() || destinationStripProducerObserved || !producer ||
		!FClassnameIs(producer->pev, "trigger_auto") || strcmp(STRING(producer->pev->target), "strip_weapons") ||
		(producer->pev->origin - Vector(-2992, 3024, 352)).Length() > 0.1f ||
		fabs(delay - 0.3f) > 0.001f || useType != USE_OFF || producer->pev->spawnflags != 1 ) return;
	destinationStripProducerObserved = TRUE; destinationStripProducerTime = gpGlobals->time;
	destinationStripProducerEdict = producer->entindex();
	Begin("native_destination_strip_producer"); EdgeFields();
	fprintf(report, ",\"producer_bsp_entity\":588,\"producer_edict\":%d,\"producer_classname\":\"trigger_auto\",\"producer_target\":\"strip_weapons\",\"producer_delay_seconds\":%.4f,\"producer_use_type\":%d,\"producer_origin\":",
		destinationStripProducerEdict, delay, useType); Vec(producer->pev->origin);
	fputs(",\"native_think_observed\":true,\"observer_only\":true,\"before_first_restore\":true,\"source_snapshot_unchanged\":true", report); End();
}

void CoopTransitionCheckStripUse( CBaseEntity *event, CBaseEntity *caller, int useType, float value )
{
	float delay = gpGlobals->time - destinationStripProducerTime;
	if ( !NativeDestinationStripWindow() || !destinationStripProducerObserved || destinationStripUseObserved ||
		!event || !caller || !FClassnameIs(event->pev, "player_weaponstrip") ||
		strcmp(STRING(event->pev->targetname), "strip_weapons") ||
		(event->pev->origin - Vector(-2960, 3024, 352)).Length() > 0.1f || fabs(event->pev->dmg) > 0.001f ||
		!FClassnameIs(caller->pev, "DelayedUse") || strcmp(STRING(caller->pev->target), "strip_weapons") ||
		useType != USE_OFF || fabs(value) > 0.001f || delay < 0.299f || delay > 5 ) return;
	destinationStripUseObserved = TRUE; destinationStripUseTime = gpGlobals->time;
	destinationStripCallerEdict = caller->entindex(); destinationStripEventEdict = event->entindex();
	Begin("native_destination_strip_use"); EdgeFields();
	fprintf(report, ",\"event_bsp_entity\":589,\"event_edict\":%d,\"event_classname\":\"player_weaponstrip\",\"event_targetname\":\"strip_weapons\",\"event_damage\":%.4f,\"event_origin\":",
		destinationStripEventEdict, event->pev->dmg); Vec(event->pev->origin);
	fprintf(report, ",\"caller_edict\":%d,\"caller_classname\":\"DelayedUse\",\"caller_target\":\"strip_weapons\",\"use_type\":%d,\"value\":%.4f,\"producer_edict\":%d,\"producer_game_time\":%.4f,\"observed_delay_seconds\":%.4f,\"observer_only\":true,\"before_first_restore\":true,\"source_snapshot_unchanged\":true",
		destinationStripCallerEdict, useType, value, destinationStripProducerEdict, destinationStripProducerTime, delay); End();
}

void CoopTransitionCheckNativeStrip( CBaseEntity *event )
{
	if ( !NativeDestinationStripWindow() || !destinationStripUseObserved || destinationStripObserved || !event ||
		event->entindex() != destinationStripEventEdict || !FClassnameIs(event->pev, "player_weaponstrip") ||
		strcmp(STRING(event->pev->targetname), "strip_weapons") || fabs(gpGlobals->time - destinationStripUseTime) > 0.01f ) return;
	destinationStripObserved = TRUE;
	Begin("native_destination_weaponstrip"); EdgeFields();
	fprintf(report, ",\"event_bsp_entity\":589,\"event_edict\":%d,\"event_classname\":\"player_weaponstrip\",\"event_targetname\":\"strip_weapons\",\"event_damage\":%.4f,\"event_origin\":",
		destinationStripEventEdict, event->pev->dmg); Vec(event->pev->origin);
	fprintf(report, ",\"caller_edict\":%d,\"caller_classname\":\"DelayedUse\",\"caller_target\":\"strip_weapons\",\"use_type\":%d,\"producer_edict\":%d,\"producer_game_time\":%.4f,\"observed_delay_seconds\":%.4f,\"native_effect_completed\":true,\"effect\":\"clear_weapons_and_ammo\",\"before_first_restore\":true,\"source_snapshot_unchanged\":true,\"observer_only\":true",
		destinationStripCallerEdict, USE_OFF, destinationStripProducerEdict, destinationStripProducerTime, gpGlobals->time - destinationStripProducerTime); End();
}

BOOL CoopTransitionCheckMusicCommand( CBasePlayer *player, int track )
{
	int i = player ? OwnedProbe(player->edict()) : -1;
	if ( i < 0 || track < -1 ) return FALSE;
	if ( track == -1 ) { ++musicStop; ++probes[i].musicStop; }
	else { ++musicPlay; ++probes[i].musicPlay; }
	if ( report )
	{
		Begin("music_command_suppressed"); EdgeFields();
		fprintf(report, ",\"probe\":%d,\"slot\":%d,\"userid\":%d,\"track\":%d,\"command\":\"%s\",\"call_count\":%d,\"music_protocol_checked\":false",
			i, probes[i].slot, probes[i].user, track, track == -1 ? "stop" : "play", musicPlay + musicStop); End();
	}
	return TRUE;
}
#endif
