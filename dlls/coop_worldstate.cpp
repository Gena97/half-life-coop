#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <limits>
#include <cmath>
#include <exception>
#include <memory>
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "saverestore.h"
#include "gamerules.h"
#include "coop_worldstate.h"

namespace
{
const unsigned int WORLD_VERSION = 1;
const unsigned int TOKEN_COUNT = 4096;
const unsigned int MAX_ENTITIES = 16384;
const unsigned int MAX_MAPS = 256;
const unsigned int MAX_MAP_BYTES = 16 * 1024 * 1024;
const unsigned int MAX_TOTAL_BYTES = 160 * 1024 * 1024;
const unsigned int MAX_STRING_BYTES = 4096;
const unsigned int RECORD_SAVED = 1;
const unsigned int RECORD_TRANSIT = 2;
const unsigned int RECORD_DEPARTED = 4;
const unsigned int RECORD_GLOBAL_TRANSIT = 8;

struct EntityRecord
{
	EntityRecord() : flags(0), offset(0), size(0), client(0), caps(0), dormant(FALSE), mins(0,0,0) {}
	unsigned int flags;
	unsigned int offset;
	unsigned int size;
	unsigned int client;
	unsigned int caps;
	BOOL dormant;
	std::string classname;
	std::string targetname;
	std::string globalname;
	std::string model;
	std::string identity;
	Vector mins;
};

struct Snapshot
{
	std::string map;
	std::vector<EntityRecord> entities;
	std::vector<std::string> tokens;
	std::vector<unsigned char> bytes;
};

struct PendingTransition
{
	PendingTransition() : sourceOrigin(0,0,0) {}
	std::string sourceMap;
	std::string destinationMap;
	std::string landmark;
	Vector sourceOrigin;
};

struct WorldState
{
	std::map<std::string, Snapshot> maps;
	Snapshot globals;
	PendingTransition pending;
};

struct Identity
{
	EHANDLE entity;
	std::string key;
};

struct HeldPusher
{
	edict_t *identity;
	int index;
	int serial;
	int movetype;
	Vector velocity;
	Vector avelocity;
	float nextthink;
	float ltime;
};

struct SaveContext
{
	SAVERESTOREDATA data;
	std::vector<ENTITYTABLE> table;
	std::vector<char *> tokens;
	SaveContext( Snapshot &snapshot, bool writing )
	{
		memset( &data, 0, sizeof(data) );
		table.resize( snapshot.entities.size() );
		tokens.resize( TOKEN_COUNT, NULL );
		if ( !writing )
		{
			for ( unsigned int i = 0; i < snapshot.tokens.size(); ++i )
				if ( !snapshot.tokens[i].empty() )
					tokens[i] = const_cast<char *>(snapshot.tokens[i].c_str());
		}
		data.pBaseData = snapshot.bytes.empty() ? NULL : reinterpret_cast<char *>(&snapshot.bytes[0]);
		data.pCurrentData = data.pBaseData;
		data.bufferSize = (int)snapshot.bytes.size();
		data.tokenCount = TOKEN_COUNT;
		data.pTokens = &tokens[0];
		data.tableCount = (int)table.size();
		data.pTable = table.empty() ? NULL : &table[0];
		data.time = gpGlobals->time;
		strncpy( data.szCurrentMapName, snapshot.map.c_str(), sizeof(data.szCurrentMapName) - 1 );
		for ( unsigned int i = 0; i < table.size(); ++i )
		{
			memset( &table[i], 0, sizeof(ENTITYTABLE) );
			table[i].id = i;
		}
	}
};

class SaveDataScope
{
public:
	SaveDataScope( SAVERESTOREDATA *data ) : previous(gpGlobals->pSaveData) { gpGlobals->pSaveData = data; }
	~SaveDataScope() { gpGlobals->pSaveData = previous; }
private:
	void *previous;
};

WorldState state;
std::unique_ptr<WorldState> stagedState;
std::vector<Identity> identities;
std::map<int, int> restoredEntities;
std::vector<HeldPusher> heldPushers;
std::string heldPushersMap;
std::string activeMap;
BOOL restoreGlobals = FALSE;
BOOL restoreRequested = FALSE;

void ClearHeldPushers( void )
{
	heldPushers.clear();
	heldPushersMap.clear();
}

edict_t *HeldPusherEntity( HeldPusher &saved )
{
	edict_t *entity = INDEXENT(saved.index);
	if ( !entity || entity != saved.identity || entity->free || entity->serialnumber != saved.serial || !entity->pvPrivateData ||
		(entity->v.flags & (FL_KILLME | FL_DORMANT)) )
		return NULL;
	return entity;
}

bool HasReadyPlayer( void )
{
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBaseEntity *entity = UTIL_PlayerByIndex(i);
		if ( !entity || !entity->IsPlayer() || entity->IsDormant() ||
			(entity->pev->flags & (FL_KILLME | FL_SPECTATOR)) || GETPLAYERUSERID(entity->edict()) <= 0 )
			continue;
		CBasePlayer *player = (CBasePlayer *)entity;
		if ( player->IsAlive() && !player->m_fCoopSpawnPending && !player->IsObserver() )
			return true;
	}
	return false;
}

void ApplyPusherHold( void )
{
	for ( unsigned int i = 0; i < heldPushers.size(); ++i )
	{
		HeldPusher &saved = heldPushers[i];
		edict_t *entity = HeldPusherEntity(saved);
		if ( !entity )
			continue;
		// SOLID_BSP требует MOVETYPE_PUSH даже во время ожидания игрока
		entity->v.movetype = saved.movetype;
		entity->v.velocity = entity->v.avelocity = g_vecZero;
		entity->v.nextthink = 0;
		entity->v.ltime = saved.ltime;
	}
}

void ReleaseHeldPushers( void )
{
	if ( heldPushers.empty() || !gpGlobals || !gpGlobals->pStringBase || !gpGlobals->mapname )
	{
		ClearHeldPushers();
		return;
	}
	if ( heldPushersMap == STRING(gpGlobals->mapname) )
	{
		for ( unsigned int i = 0; i < heldPushers.size(); ++i )
		{
			HeldPusher &saved = heldPushers[i];
			edict_t *entity = HeldPusherEntity(saved);
			if ( !entity )
				continue;
			entity->v.movetype = saved.movetype;
			entity->v.velocity = saved.velocity;
			entity->v.avelocity = saved.avelocity;
			entity->v.nextthink = saved.nextthink;
			entity->v.ltime = saved.ltime;
		}
		if ( !heldPushers.empty() )
			ALERT(at_console, "Half-Life Co-op: released %u restored pushers for player arrival\n", (unsigned int)heldPushers.size());
	}
	ClearHeldPushers();
}

void HoldRestoredPushers( void )
{
	std::vector<HeldPusher> pending;
	pending.reserve(restoredEntities.size());
	for ( std::map<int, int>::const_iterator i = restoredEntities.begin(); i != restoredEntities.end(); ++i )
	{
		edict_t *entity = INDEXENT(i->first);
		if ( !entity || entity->free || entity->serialnumber != i->second || !entity->pvPrivateData ||
			(entity->v.flags & (FL_KILLME | FL_DORMANT)) || entity->v.movetype != MOVETYPE_PUSH )
			continue;
		HeldPusher saved;
		saved.identity = entity;
		saved.index = i->first;
		saved.serial = i->second;
		saved.movetype = entity->v.movetype;
		saved.velocity = entity->v.velocity;
		saved.avelocity = entity->v.avelocity;
		saved.nextthink = entity->v.nextthink;
		saved.ltime = entity->v.ltime;
		pending.push_back(saved);
	}
	// Все выделения памяти завершаются до изменения живых сущностей
	heldPushersMap = activeMap;
	heldPushers.swap(pending);
	ApplyPusherHold();
	if ( !heldPushers.empty() )
		ALERT(at_console, "Half-Life Co-op: holding %u restored pushers until player arrival\n", (unsigned int)heldPushers.size());
}

bool FiniteVector( const Vector &value )
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool ValidMap( const std::string &name, bool emptyAllowed = false )
{
	if ( name.empty() )
		return emptyAllowed;
	if ( name.size() >= 32 )
		return false;
	for ( unsigned int i = 0; i < name.size(); ++i )
	{
		char c = name[i];
		if ( !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
			!(c >= '0' && c <= '9') && c != '_' && c != '-' )
			return false;
	}
	return true;
}

bool Infrastructure( edict_t *edict, int clients )
{
	if ( !edict || edict->free || !edict->pvPrivateData || ENTINDEX(edict) <= clients )
		return true;
	const char *name = STRING(edict->v.classname);
	return !strcmp(name, "worldspawn") || !strcmp(name, "soundent") ||
		!strcmp(name, "bodyque") || !strcmp(name, "player");
}

bool InventoryEntity( CBaseEntity *entity )
{
	return entity->pev->movetype == MOVETYPE_FOLLOW && entity->pev->aiment &&
		ENTINDEX(entity->pev->aiment) > 0 && ENTINDEX(entity->pev->aiment) <= gpGlobals->maxClients;
}

bool Savable( edict_t *edict, int clients )
{
	if ( Infrastructure(edict, clients) || (edict->v.flags & FL_KILLME) )
		return false;
	CBaseEntity *entity = CBaseEntity::Instance(edict);
	return entity && !(entity->ObjectCaps() & FCAP_DONT_SAVE) && !InventoryEntity(entity);
}

std::string BaseIdentity( edict_t *edict )
{
	char origin[120];
	_snprintf( origin, sizeof(origin), "%.9g %.9g %.9g", edict->v.origin.x, edict->v.origin.y, edict->v.origin.z );
	origin[sizeof(origin)-1] = 0;
	return std::string(STRING(edict->v.classname)) + "\x1f" + STRING(edict->v.targetname) +
		"\x1f" + STRING(edict->v.globalname) + "\x1f" + STRING(edict->v.model) + "\x1f" + origin;
}

void RegisterFreshIdentities( edict_t *list, int count, int clients )
{
	identities.clear();
	std::map<std::string, unsigned int> occurrences;
	for ( int i = clients + 1; i < count; ++i )
	{
		if ( !Savable(&list[i], clients) )
			continue;
		std::string base = BaseIdentity(&list[i]);
		char ordinal[32];
		_snprintf( ordinal, sizeof(ordinal), "\x1f%u", occurrences[base]++ );
		Identity entry;
		entry.entity = CBaseEntity::Instance(&list[i]);
		entry.key = base + ordinal;
		identities.push_back(entry);
	}
}

std::string EntityIdentity( edict_t *edict )
{
	for ( unsigned int i = 0; i < identities.size(); ++i )
		if ( identities[i].entity.Get() == edict )
			return identities[i].key;
	return std::string();
}

void CopyTokens( Snapshot &snapshot, const SaveContext &context )
{
	snapshot.tokens.resize(TOKEN_COUNT);
	for ( unsigned int i = 0; i < TOKEN_COUNT; ++i )
		if ( context.tokens[i] )
			snapshot.tokens[i] = context.tokens[i];
}

bool ValidateStream( const Snapshot &snapshot, unsigned int offset, unsigned int length )
{
	if ( offset > snapshot.bytes.size() || length > snapshot.bytes.size() - offset )
		return false;
	unsigned int end = offset + length;
	unsigned int groups = 0;
	while ( offset < end )
	{
		if ( end - offset < 8 || ++groups > 1024 )
			return false;
		unsigned short size, token;
		int fields;
		memcpy( &size, &snapshot.bytes[offset], 2 );
		memcpy( &token, &snapshot.bytes[offset+2], 2 );
		memcpy( &fields, &snapshot.bytes[offset+4], 4 );
		if ( size != 4 || token >= snapshot.tokens.size() || snapshot.tokens[token].empty() || fields < 0 || fields > 2048 )
			return false;
		offset += 8;
		for ( int i = 0; i < fields; ++i )
		{
			if ( end - offset < 4 )
				return false;
			memcpy( &size, &snapshot.bytes[offset], 2 );
			memcpy( &token, &snapshot.bytes[offset+2], 2 );
			offset += 4;
			if ( size > end - offset || token >= snapshot.tokens.size() || snapshot.tokens[token].empty() )
				return false;
			offset += size;
		}
	}
	return offset == end;
}

bool ValidateGlobals( const Snapshot &snapshot )
{
	if ( !ValidateStream(snapshot, 0, (unsigned int)snapshot.bytes.size()) || snapshot.bytes.empty() )
		return false;
	unsigned int offset = 0, expected = 0, actual = 0;
	while ( offset < snapshot.bytes.size() )
	{
		unsigned short token;
		int fields;
		memcpy(&token, &snapshot.bytes[offset+2], 2);
		memcpy(&fields, &snapshot.bytes[offset+4], 4);
		bool first = offset == 0;
		if ( snapshot.tokens[token] != (first ? "GLOBAL" : "GENT") )
			return false;
		if ( !first )
			++actual;
		offset += 8;
		bool hasName = false, hasMap = false;
		for ( int i = 0; i < fields; ++i )
		{
			unsigned short size;
			memcpy(&size, &snapshot.bytes[offset], 2);
			memcpy(&token, &snapshot.bytes[offset+2], 2);
			offset += 4;
			const std::string &field = snapshot.tokens[token];
			if ( first )
			{
				if ( field != "m_listCount" || size != 4 )
					return false;
				memcpy(&expected, &snapshot.bytes[offset], 4);
				if ( expected > MAX_ENTITIES )
					return false;
			}
			else if ( field == "name" || field == "levelName" )
			{
				unsigned int required = field == "name" ? 64 : 32;
				if ( size != required || !memchr(&snapshot.bytes[offset], 0, size) )
					return false;
				if ( field == "name" )
					hasName = snapshot.bytes[offset] != 0;
				else
					hasMap = ValidMap(reinterpret_cast<const char *>(&snapshot.bytes[offset]));
			}
			else if ( field == "state" )
			{
				unsigned int value;
				if ( size != 4 )
					return false;
				memcpy(&value, &snapshot.bytes[offset], 4);
				if ( value > GLOBAL_DEAD )
					return false;
			}
			else
				return false;
			offset += size;
		}
		if ( !first && (!hasName || !hasMap) )
			return false;
	}
	return actual == expected;
}

bool CaptureGlobals( Snapshot &result )
{
	result.bytes.resize(MAX_MAP_BYTES);
	SaveContext context(result, true);
	CSave save(&context.data);
	if ( !gGlobalState.Save(save) || context.data.size >= (int)MAX_MAP_BYTES )
		return false;
	result.bytes.resize(context.data.size);
	CopyTokens(result, context);
	if ( !ValidateGlobals(result) )
		return false;
	// Посещённые карты хранят только записанные байты без временного резерва на 16 МБ
	std::vector<unsigned char>(result.bytes.begin(), result.bytes.end()).swap(result.bytes);
	return true;
}

bool InTransitionVolume( CBaseEntity *entity, const char *landmark )
{
	if ( entity->ObjectCaps() & FCAP_FORCE_TRANSITION )
		return true;
	if ( entity->pev->movetype == MOVETYPE_FOLLOW && entity->pev->aiment )
		entity = CBaseEntity::Instance(entity->pev->aiment);
	if ( !entity )
		return false;
	bool found = false;
	CBaseEntity *volume = NULL;
	while ( (volume = UTIL_FindEntityByTargetname(volume, landmark)) != NULL )
	{
		if ( FClassnameIs(volume->pev, "trigger_transition") )
		{
			found = true;
			if ( volume->Intersects(entity) )
				return true;
		}
	}
	return !found;
}

void MarkTransit( Snapshot &snapshot, const char *landmark )
{
	CBaseEntity *marker = NULL;
	if ( landmark && *landmark )
		while ( (marker = UTIL_FindEntityByTargetname(marker, landmark)) != NULL )
			if ( FClassnameIs(marker->pev, "info_landmark") )
				break;
	std::set<int> visible;
	edict_t *edict = marker ? UTIL_EntitiesInPVS(marker->edict()) : NULL;
	unsigned int guard = 0;
	while ( edict && !FNullEnt(edict) && guard++ < MAX_ENTITIES )
	{
		visible.insert(ENTINDEX(edict));
		edict = edict->v.chain;
	}
	for ( unsigned int i = 0; i < snapshot.entities.size(); ++i )
	{
		EntityRecord &record = snapshot.entities[i];
		if ( !(record.flags & RECORD_SAVED) || record.dormant )
			continue;
		CBaseEntity *entity = CBaseEntity::Instance(INDEXENT(i));
		if ( !entity )
			continue;
		if ( !record.globalname.empty() )
		{
			// Глобальная сущность переносится только через выбранные PVS и объём перехода
			if ( visible.count(i) && InTransitionVolume(entity, landmark) )
				record.flags |= RECORD_GLOBAL_TRANSIT;
			continue;
		}
		if ( (record.caps & FCAP_ACROSS_TRANSITION) &&
			((record.caps & FCAP_FORCE_TRANSITION) || (visible.count(i) && InTransitionVolume(entity, landmark))) )
		{
			// Локальные brush-модели нельзя переносить под номером из другой BSP
			if ( record.model.empty() || record.model[0] != '*' )
				record.flags |= RECORD_TRANSIT;
		}
	}
}

bool CaptureMap( Snapshot &result, const char *landmark )
{
	result.map = STRING(gpGlobals->mapname);
	if ( !ValidMap(result.map) || gpGlobals->maxEntities > (int)MAX_ENTITIES )
		return false;
	result.entities.resize(gpGlobals->maxEntities);
	result.bytes.resize(MAX_MAP_BYTES);
	SaveContext context(result, true);
	SaveDataScope scope(&context.data);
	for ( unsigned int i = 0; i < result.entities.size(); ++i )
	{
		edict_t *edict = INDEXENT(i);
		if ( !edict || edict->free )
			continue;
		context.table[i].pent = edict;
		EntityRecord &record = result.entities[i];
		record.classname = STRING(edict->v.classname);
		record.targetname = STRING(edict->v.targetname);
		record.globalname = STRING(edict->v.globalname);
		record.model = STRING(edict->v.model);
		record.mins = edict->v.mins;
		record.identity = EntityIdentity(edict);
		if ( i > 0 && i <= (unsigned int)gpGlobals->maxClients )
			record.client = i;
	}
	for ( unsigned int i = gpGlobals->maxClients + 1; i < result.entities.size(); ++i )
	{
		edict_t *edict = context.table[i].pent;
		if ( !Savable(edict, gpGlobals->maxClients) )
			continue;
		CBaseEntity *entity = CBaseEntity::Instance(edict);
		EntityRecord &record = result.entities[i];
		record.caps = entity->ObjectCaps();
		record.dormant = entity->IsDormant();
		record.offset = context.data.size;
		context.data.currentIndex = i;
		float localTime = edict->v.ltime;
		float nextThink = edict->v.nextthink;
		if ( edict->v.movetype == MOVETYPE_PUSH )
		{
			edict->v.ltime = gpGlobals->time;
			edict->v.nextthink = gpGlobals->time + nextThink - localTime;
		}
		CSave save(&context.data);
		int success = entity->Save(save);
		edict->v.ltime = localTime;
		edict->v.nextthink = nextThink;
		if ( !success || context.data.size >= (int)MAX_MAP_BYTES )
			return false;
		record.size = context.data.size - record.offset;
		record.flags = RECORD_SAVED;
	}
	result.bytes.resize(context.data.size);
	CopyTokens(result, context);
	for ( unsigned int i = 0; i < result.entities.size(); ++i )
		if ( (result.entities[i].flags & RECORD_SAVED) &&
			!ValidateStream(result, result.entities[i].offset, result.entities[i].size) )
			return false;
	MarkTransit(result, landmark);
	// Уменьшение size не освобождает capacity, поэтому заменяем буфер точной копией
	std::vector<unsigned char>(result.bytes.begin(), result.bytes.end()).swap(result.bytes);
	return true;
}

bool GlobalRecordDead( const EntityRecord &record )
{
	if ( record.globalname.empty() )
		return false;
	const globalentity_t *global = gGlobalState.EntityFromTable(ALLOC_STRING(record.globalname.c_str()));
	return global && global->state == GLOBAL_DEAD;
}

edict_t *MatchReference( const EntityRecord &record )
{
	CBaseEntity *entity = NULL;
	if ( !record.globalname.empty() )
	{
		while ( (entity = UTIL_FindEntityByString(entity, "globalname", record.globalname.c_str())) != NULL )
			if ( FClassnameIs(entity->pev, record.classname.c_str()) )
				return entity->edict();
	}
	entity = NULL;
	if ( !record.targetname.empty() )
	{
		while ( (entity = UTIL_FindEntityByTargetname(entity, record.targetname.c_str())) != NULL )
			if ( FClassnameIs(entity->pev, record.classname.c_str()) )
				return entity->edict();
	}
	return NULL;
}

void MapCommonReferences( const Snapshot &snapshot, SaveContext &context )
{
	for ( unsigned int i = 0; i < snapshot.entities.size(); ++i )
	{
		const EntityRecord &record = snapshot.entities[i];
		if ( i == 0 )
			context.table[i].pent = INDEXENT(0);
		else if ( record.client > 0 && record.client <= (unsigned int)gpGlobals->maxClients )
		{
			edict_t *client = INDEXENT(record.client);
			if ( client )
			{
				// Ссылки EHANDLE должны существовать до позднего ClientPutInServer
				ALERT(at_console, "Half-Life Co-op: world reference player %u private=%d free=%d linked=%d\n", record.client, client->pvPrivateData != NULL, client->free, client->v.pContainingEntity != NULL);
				// Движок ещё не заполнил обратную ссылку зарезервированного клиентского edict
				client->v.pContainingEntity = client;
				GetClassPtr( reinterpret_cast<CBasePlayer *>(&client->v) );
				context.table[i].pent = client;
			}
		}
		else
			context.table[i].pent = MatchReference(record);
	}
}

bool RestoreRecord( Snapshot &snapshot, unsigned int index, SaveContext &context, edict_t *target,
	const Vector &delta, bool global )
{
	EntityRecord &record = snapshot.entities[index];
	CBaseEntity *entity = CBaseEntity::Instance(target);
	if ( !entity || !ValidateStream(snapshot, record.offset, record.size) )
		return false;
	context.data.size = record.offset;
	context.data.pCurrentData = context.data.pBaseData + record.offset;
	context.data.bufferSize = record.offset + record.size;
	context.data.currentIndex = index;
	context.data.fUseLandmark = global || delta != g_vecZero;
	context.data.vecLandmarkOffset = delta;
	if ( global )
		context.data.vecLandmarkOffset = delta + record.mins - target->v.mins;
	SaveDataScope scope(&context.data);
	CRestore restore(&context.data);
	restore.SetGlobalMode(global);
	if ( global )
		ALERT(at_console, "Half-Life Co-op: restoring global %s class %s source=%u target=%d offset=(%.1f %.1f %.1f) model=%s\n",
			record.globalname.c_str(), record.classname.c_str(), index, ENTINDEX(target),
			context.data.vecLandmarkOffset.x, context.data.vecLandmarkOffset.y, context.data.vecLandmarkOffset.z, STRING(target->v.model));
	if ( !entity->Restore(restore) )
		return false;
	if ( global )
		ALERT(at_console, "Half-Life Co-op: global %s fields restored\n", record.globalname.c_str());
	if ( (entity->ObjectCaps() & FCAP_MUST_SPAWN) || record.classname == "light" || record.classname == "light_spot" )
		entity->Spawn();
	else
		entity->Precache();
	if ( global )
		ALERT(at_console, "Half-Life Co-op: global %s precached\n", record.globalname.c_str());
	if ( target->free || !target->pvPrivateData )
		return true;
	UTIL_SetOrigin(entity->pev, entity->pev->origin);
	if ( global )
	{
		entity->pev->flags &= ~FL_DORMANT;
		gGlobalState.EntityUpdate(entity->pev->globalname, gpGlobals->mapname);
		entity->OverrideReset();
		ALERT(at_console, "Half-Life Co-op: global %s relinked origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f) target=%s solid=%d move=%d time=%.2f next=%.2f\n",
			record.globalname.c_str(), entity->pev->origin.x, entity->pev->origin.y, entity->pev->origin.z,
			entity->pev->angles.x, entity->pev->angles.y, entity->pev->angles.z, STRING(entity->pev->target),
			entity->pev->solid, entity->pev->movetype, entity->pev->ltime, entity->pev->nextthink);
	}
	else if ( entity->pev->globalname )
	{
		const globalentity_t *owner = gGlobalState.EntityFromTable(entity->pev->globalname);
		if ( owner )
		{
			if ( owner->state == GLOBAL_DEAD )
			{
				REMOVE_ENTITY(target);
				return true;
			}
			if ( strcmp(owner->levelName, STRING(gpGlobals->mapname)) )
				entity->MakeDormant();
		}
		else
			gGlobalState.EntityAdd(entity->pev->globalname, gpGlobals->mapname, GLOBAL_ON);
	}
	restoredEntities[ENTINDEX(target)] = target->serialnumber;
	return true;
}

bool RestoreMap( Snapshot &snapshot )
{
	SaveContext context(snapshot, false);
	MapCommonReferences(snapshot, context);
	std::map<std::string, edict_t *> fresh;
	for ( unsigned int i = 0; i < identities.size(); ++i )
		if ( identities[i].entity.Get() )
			fresh[identities[i].key] = identities[i].entity.Get();
	std::set<edict_t *> retained;
	std::vector<edict_t *> created;
	for ( unsigned int i = 0; i < snapshot.entities.size(); ++i )
	{
		EntityRecord &record = snapshot.entities[i];
		if ( !(record.flags & RECORD_SAVED) || (record.flags & RECORD_DEPARTED) )
			continue;
		if ( GlobalRecordDead(record) )
		{
			// Ссылки на уничтоженную глобальную сущность остаются пустыми до первого Restore
			context.table[i].pent = NULL;
			continue;
		}
		edict_t *target = NULL;
		if ( !record.identity.empty() && fresh.count(record.identity) )
			target = fresh[record.identity];
		if ( !target )
		{
			target = CREATE_NAMED_ENTITY(ALLOC_STRING(record.classname.c_str()));
			if ( FNullEnt(target) || !target->pvPrivateData )
			{
				for ( unsigned int j = 0; j < created.size(); ++j )
					REMOVE_ENTITY(created[j]);
				return false;
			}
			created.push_back(target);
			if ( !record.identity.empty() )
			{
				Identity identity;
				identity.entity = CBaseEntity::Instance(target);
				identity.key = record.identity;
				identities.push_back(identity);
			}
		}
		context.table[i].pent = target;
		retained.insert(target);
	}
	// Все адресаты существуют до восстановления первого указателя на другую сущность
	for ( unsigned int i = 0; i < identities.size(); ++i )
	{
		edict_t *target = identities[i].entity.Get();
		if ( target && !retained.count(target) && Savable(target, gpGlobals->maxClients) )
		{
			for ( unsigned int j = 0; j < context.table.size(); ++j )
				if ( context.table[j].pent == target )
					context.table[j].pent = NULL;
			REMOVE_ENTITY(target);
		}
	}
	for ( unsigned int i = 0; i < snapshot.entities.size(); ++i )
		if ( (snapshot.entities[i].flags & RECORD_SAVED) && !(snapshot.entities[i].flags & RECORD_DEPARTED) && context.table[i].pent )
			if ( !RestoreRecord(snapshot, i, context, context.table[i].pent, g_vecZero, false) )
				return false;
	// Динамические сущности не получают идентичность исходной свежей карты
	return true;
}

Vector LandmarkOrigin( const Snapshot &snapshot, const std::string &name, bool &found )
{
	Vector result(0,0,0);
	CBaseEntity *marker = NULL;
	while ( (marker = UTIL_FindEntityByTargetname(marker, name.c_str())) != NULL )
	{
		if ( FClassnameIs(marker->pev, "info_landmark") )
		{
			found = true;
			return marker->pev->origin;
		}
	}
	found = false;
	return result;
}

void RestoreTransit( Snapshot &source, const Vector &destination, bool forcedOnly = false )
{
	ALERT(at_console, "Half-Life Co-op: world restoring transit from %s forced=%d\n", source.map.c_str(), forcedOnly);
	Vector delta = forcedOnly ? g_vecZero : destination - state.pending.sourceOrigin;
	SaveContext context(source, false);
	MapCommonReferences(source, context);
	std::vector<unsigned int> incoming;
	for ( unsigned int i = 0; i < source.entities.size(); ++i )
	{
		EntityRecord &record = source.entities[i];
		if ( !(record.flags & RECORD_TRANSIT) || (record.flags & RECORD_DEPARTED) )
			continue;
		if ( forcedOnly && !(record.caps & FCAP_FORCE_TRANSITION) )
			continue;
		edict_t *target = CREATE_NAMED_ENTITY(ALLOC_STRING(record.classname.c_str()));
		if ( FNullEnt(target) || !target->pvPrivateData )
		{
			ALERT(at_error, "Half-Life Co-op: cannot transfer %s\n", record.classname.c_str());
			continue;
		}
		context.table[i].pent = target;
		incoming.push_back(i);
	}
	for ( unsigned int j = 0; j < incoming.size(); ++j )
	{
		unsigned int i = incoming[j];
		if ( RestoreRecord(source, i, context, context.table[i].pent, delta, false) )
			source.entities[i].flags |= RECORD_DEPARTED;
	}
	ALERT(at_console, "Half-Life Co-op: world transit restored %u entities\n", (unsigned int)incoming.size());
}

void RestoreGlobalsOnMap( void )
{
	// Последняя карта-владелец глобальной сущности определяется штатной таблицей SDK
	for ( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i )
	{
		edict_t *target = INDEXENT(i);
		if ( !target || target->free || !target->pvPrivateData || !target->v.globalname )
			continue;
		const globalentity_t *global = gGlobalState.EntityFromTable(target->v.globalname);
		if ( !global )
			continue;
		if ( global->state == GLOBAL_DEAD )
		{
			REMOVE_ENTITY(target);
			continue;
		}
		if ( !strcmp(global->levelName, STRING(gpGlobals->mapname)) )
			continue;
		// Старая локальная копия не должна отбирать владение у другой карты
		CBaseEntity::Instance(target)->MakeDormant();
		if ( state.pending.destinationMap != activeMap || state.pending.sourceMap != global->levelName )
			continue;
		std::map<std::string, Snapshot>::iterator owner = state.maps.find(global->levelName);
		if ( owner == state.maps.end() )
			continue;
		Snapshot &snapshot = owner->second;
		for ( unsigned int j = 0; j < snapshot.entities.size(); ++j )
		{
			EntityRecord &record = snapshot.entities[j];
			if ( !(record.flags & RECORD_SAVED) || !(record.flags & RECORD_GLOBAL_TRANSIT) || record.dormant || record.globalname != STRING(target->v.globalname) ||
				record.classname != STRING(target->v.classname) )
				continue;
			Vector delta(0,0,0);
			if ( snapshot.map == state.pending.sourceMap )
			{
				bool found;
				Vector destination = LandmarkOrigin(snapshot, state.pending.landmark, found);
				if ( found )
					delta = destination - state.pending.sourceOrigin;
			}
			SaveContext context(snapshot, false);
			ALERT(at_console, "Half-Life Co-op: mapping global %s references from %s\n", record.globalname.c_str(), snapshot.map.c_str());
			MapCommonReferences(snapshot, context);
			context.table[j].pent = target;
			if ( !RestoreRecord(snapshot, j, context, target, delta, true) )
				ALERT(at_error, "Half-Life Co-op: cannot restore global %s\n", record.globalname.c_str());
			break;
		}
	}
}

bool WriteU32( FILE *file, unsigned int value )
{
	unsigned char bytes[4] = { (unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16), (unsigned char)(value >> 24) };
	return fwrite(bytes, 1, 4, file) == 4;
}

bool ReadU32( FILE *file, unsigned int &value, unsigned int &budget )
{
	unsigned char bytes[4];
	if ( budget < 4 || fread(bytes, 1, 4, file) != 4 )
		return false;
	budget -= 4;
	value = (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8) | ((unsigned int)bytes[2] << 16) | ((unsigned int)bytes[3] << 24);
	return true;
}

bool WriteString( FILE *file, const std::string &value )
{
	return value.size() <= MAX_STRING_BYTES && WriteU32(file, (unsigned int)value.size()) &&
		(value.empty() || fwrite(value.data(), 1, value.size(), file) == value.size());
}

bool ReadString( FILE *file, std::string &value, unsigned int &budget, unsigned int limit = MAX_STRING_BYTES )
{
	unsigned int length;
	if ( !ReadU32(file, length, budget) || length > limit || length > budget )
		return false;
	budget -= length;
	value.resize(length);
	if ( length && fread(&value[0], 1, length, file) != length )
		return false;
	return value.find('\0') == std::string::npos;
}

bool WriteVector( FILE *file, const Vector &vector )
{
	for ( int i = 0; i < 3; ++i )
	{
		unsigned int bits;
		float value = vector[i];
		memcpy(&bits, &value, sizeof(bits));
		if ( !WriteU32(file, bits) )
			return false;
	}
	return true;
}

bool ReadVector( FILE *file, Vector &vector, unsigned int &budget )
{
	for ( int i = 0; i < 3; ++i )
	{
		unsigned int bits;
		if ( !ReadU32(file, bits, budget) )
			return false;
		memcpy(&vector[i], &bits, sizeof(bits));
	}
	return FiniteVector(vector);
}

bool WriteSnapshot( FILE *file, const Snapshot &snapshot )
{
	if ( !WriteString(file, snapshot.map) || !WriteU32(file, (unsigned int)snapshot.entities.size()) )
		return false;
	for ( unsigned int i = 0; i < snapshot.entities.size(); ++i )
	{
		const EntityRecord &record = snapshot.entities[i];
		if ( !WriteU32(file, record.flags) || !WriteU32(file, record.offset) || !WriteU32(file, record.size) ||
			!WriteU32(file, record.client) || !WriteU32(file, record.caps) || !WriteU32(file, record.dormant ? 1 : 0) ||
			!WriteString(file, record.classname) || !WriteString(file, record.targetname) || !WriteString(file, record.globalname) ||
			!WriteString(file, record.model) || !WriteString(file, record.identity) || !WriteVector(file, record.mins) )
			return false;
	}
	if ( !WriteU32(file, (unsigned int)snapshot.tokens.size()) )
		return false;
	for ( unsigned int i = 0; i < snapshot.tokens.size(); ++i )
		if ( !WriteString(file, snapshot.tokens[i]) )
			return false;
	return WriteU32(file, (unsigned int)snapshot.bytes.size()) &&
		(snapshot.bytes.empty() || fwrite(&snapshot.bytes[0], 1, snapshot.bytes.size(), file) == snapshot.bytes.size());
}

bool ReadSnapshot( FILE *file, Snapshot &snapshot, unsigned int &budget, bool globals )
{
	unsigned int count;
	if ( !ReadString(file, snapshot.map, budget, 31) || !ValidMap(snapshot.map, globals) ||
		!ReadU32(file, count, budget) || count > MAX_ENTITIES || (globals && count) )
		return false;
	snapshot.entities.resize(count);
	std::set<std::string> keys;
	for ( unsigned int i = 0; i < count; ++i )
	{
		EntityRecord &record = snapshot.entities[i];
		unsigned int dormant;
		if ( !ReadU32(file, record.flags, budget) || !ReadU32(file, record.offset, budget) || !ReadU32(file, record.size, budget) ||
			!ReadU32(file, record.client, budget) || !ReadU32(file, record.caps, budget) || !ReadU32(file, dormant, budget) ||
			!ReadString(file, record.classname, budget, 127) || !ReadString(file, record.targetname, budget, 255) ||
			!ReadString(file, record.globalname, budget, 63) || !ReadString(file, record.model, budget, 255) ||
			!ReadString(file, record.identity, budget) || !ReadVector(file, record.mins, budget) || dormant > 1 ||
			(record.flags & ~(RECORD_SAVED | RECORD_TRANSIT | RECORD_DEPARTED | RECORD_GLOBAL_TRANSIT)) || record.client > 32 ||
			(record.client && (record.flags || record.client != i)) ||
			((record.flags & (RECORD_TRANSIT | RECORD_DEPARTED | RECORD_GLOBAL_TRANSIT)) && !(record.flags & RECORD_SAVED)) ||
			((record.flags & RECORD_GLOBAL_TRANSIT) && (record.globalname.empty() || dormant)) ||
			((record.flags & RECORD_SAVED) && (record.classname.empty() || !record.size || record.classname == "player" || i == 0)) )
			return false;
		record.dormant = dormant != 0;
		if ( !record.identity.empty() && !keys.insert(record.identity).second )
			return false;
	}
	if ( !ReadU32(file, count, budget) || count != TOKEN_COUNT )
		return false;
	snapshot.tokens.resize(count);
	for ( unsigned int i = 0; i < count; ++i )
		if ( !ReadString(file, snapshot.tokens[i], budget, 255) )
			return false;
	if ( !ReadU32(file, count, budget) || count > MAX_MAP_BYTES || count > budget )
		return false;
	budget -= count;
	snapshot.bytes.resize(count);
	if ( count && fread(&snapshot.bytes[0], 1, count, file) != count )
		return false;
	if ( globals )
		return ValidateGlobals(snapshot);
	for ( unsigned int i = 0; i < snapshot.entities.size(); ++i )
	{
		const EntityRecord &record = snapshot.entities[i];
		if ( (record.flags & RECORD_SAVED) && !ValidateStream(snapshot, record.offset, record.size) )
			return false;
		if ( !(record.flags & RECORD_SAVED) && (record.offset || record.size) )
			return false;
	}
	return true;
}

bool ReadState( FILE *file, WorldState &result )
{
	unsigned int budget = MAX_TOTAL_BYTES;
	unsigned int version, count;
	if ( !file || !ReadU32(file, version, budget) || version != WORLD_VERSION || !ReadU32(file, count, budget) || !count || count > MAX_MAPS )
		return false;
	for ( unsigned int i = 0; i < count; ++i )
	{
		Snapshot snapshot;
		if ( !ReadSnapshot(file, snapshot, budget, false) || result.maps.count(snapshot.map) )
			return false;
		std::string map = snapshot.map;
		result.maps[map] = std::move(snapshot);
	}
	if ( !ReadSnapshot(file, result.globals, budget, true) ||
		!ReadString(file, result.pending.sourceMap, budget, 31) ||
		!ReadString(file, result.pending.destinationMap, budget, 31) ||
		!ReadString(file, result.pending.landmark, budget, 63) || !ReadVector(file, result.pending.sourceOrigin, budget) ||
		!ValidMap(result.pending.sourceMap, true) || !ValidMap(result.pending.destinationMap, true) )
		return false;
	return result.pending.destinationMap.empty() ||
		(!result.pending.sourceMap.empty() && result.maps.count(result.pending.sourceMap));
}

size_t SnapshotStorage( const Snapshot &snapshot )
{
	size_t size = 16 + snapshot.map.size() + snapshot.bytes.size();
	for ( unsigned int i = 0; i < snapshot.tokens.size(); ++i )
		size += 4 + snapshot.tokens[i].size();
	for ( unsigned int i = 0; i < snapshot.entities.size(); ++i )
	{
		const EntityRecord &record = snapshot.entities[i];
		size += 56 + record.classname.size() + record.targetname.size() + record.globalname.size() + record.model.size() + record.identity.size();
	}
	return size;
}
}

BOOL CoopWorldCapture( const char *nextMap, const char *landmark, const Vector &sourceLandmark )
{
	if ( !g_pGameRules || !g_pGameRules->IsCoOp() || !FiniteVector(sourceLandmark) ||
		(nextMap && *nextMap && !ValidMap(nextMap)) )
		return FALSE;
	if ( !heldPushers.empty() )
	{
		if ( heldPushersMap == STRING(gpGlobals->mapname) && !HasReadyPlayer() )
		{
			ALERT(at_console, "Half-Life Co-op: world snapshot postponed until player arrival\n");
			return FALSE;
		}
		ReleaseHeldPushers();
	}
	try
	{
		Snapshot snapshot, globals;
		if ( !CaptureMap(snapshot, nextMap ? landmark : NULL) || !CaptureGlobals(globals) )
		{
			ALERT(at_error, "Half-Life Co-op: world snapshot failed, transition cancelled\n");
			return FALSE;
		}
		if ( !state.maps.count(snapshot.map) && state.maps.size() >= MAX_MAPS )
			return FALSE;
		size_t storage = SnapshotStorage(snapshot) + SnapshotStorage(globals) + 256;
		for ( std::map<std::string, Snapshot>::const_iterator i = state.maps.begin(); i != state.maps.end(); ++i )
			if ( i->first != snapshot.map )
				storage += SnapshotStorage(i->second);
		if ( storage > MAX_TOTAL_BYTES )
		{
			ALERT(at_error, "Half-Life Co-op: campaign world cache exceeds its size limit\n");
			return FALSE;
		}
		state.maps[snapshot.map] = std::move(snapshot);
		state.globals = std::move(globals);
		if ( nextMap && *nextMap )
		{
			state.pending.sourceMap = STRING(gpGlobals->mapname);
			state.pending.destinationMap = nextMap;
			state.pending.landmark = landmark ? landmark : "";
			state.pending.sourceOrigin = sourceLandmark;
			restoreGlobals = TRUE;
		}
		return TRUE;
	}
	catch ( const std::exception & )
	{
		ALERT(at_error, "Half-Life Co-op: insufficient memory for world snapshot\n");
		return FALSE;
	}
}

BOOL CoopWorldHasPendingTransition( void )
{
	return !state.pending.destinationMap.empty() || restoreRequested;
}

BOOL CoopWorldApplyPendingGlobals( void )
{
	if ( !restoreGlobals || !g_pGameRules || !g_pGameRules->IsCoOp() )
		return TRUE;
	ALERT(at_console, "Half-Life Co-op: restoring global table before map spawn\n");
	if ( !ValidateGlobals(state.globals) )
		return FALSE;
	SaveContext context(state.globals, false);
	CRestore restore(&context.data);
	if ( !gGlobalState.Restore(restore) )
		return FALSE;
	restoreGlobals = FALSE;
	ALERT(at_console, "Half-Life Co-op: global table restored before map spawn\n");
	return TRUE;
}

void CoopWorldServerActivate( edict_t *list, int count, int clients )
{
	// После выгрузки карты прежние handles больше не относятся к живому миру
	ClearHeldPushers();
	restoredEntities.clear();
	activeMap = STRING(gpGlobals->mapname);
	if ( !g_pGameRules || !g_pGameRules->IsCoOp() )
	{
		identities.clear();
		return;
	}
	ALERT(at_console, "Half-Life Co-op: world activation begins %s count=%d clients=%d\n", activeMap.c_str(), count, clients);
	RegisterFreshIdentities(list, count, clients);
	ALERT(at_console, "Half-Life Co-op: world fresh identities registered\n");
	try
	{
		std::map<std::string, Snapshot>::iterator saved = state.maps.find(activeMap);
		if ( saved != state.maps.end() && !RestoreMap(saved->second) )
			ALERT(at_error, "Half-Life Co-op: failed to restore visited map %s\n", activeMap.c_str());
		ALERT(at_console, "Half-Life Co-op: world restoring globals\n");
		RestoreGlobalsOnMap();
		ALERT(at_console, "Half-Life Co-op: world globals restored\n");
		if ( state.pending.destinationMap == activeMap )
		{
			std::map<std::string, Snapshot>::iterator source = state.maps.find(state.pending.sourceMap);
			if ( source != state.maps.end() )
			{
				bool found;
				Vector origin = LandmarkOrigin(source->second, state.pending.landmark, found);
				if ( found )
					RestoreTransit(source->second, origin);
				else
				{
					// Отложенный changetarget сохраняется даже при отсутствии пространственного перехода
					RestoreTransit(source->second, g_vecZero, true);
					ALERT(at_error, "Half-Life Co-op: destination landmark %s is missing\n", state.pending.landmark.c_str());
				}
			}
		}
		// Звуки событий анимации доступны после восстановления моделей всех участников сцены
		for ( std::map<int, int>::const_iterator i = restoredEntities.begin(); i != restoredEntities.end(); ++i )
		{
			edict_t *entity = INDEXENT(i->first);
			if ( entity && !entity->free && entity->serialnumber == i->second && entity->pvPrivateData &&
				(FClassnameIs(entity, "scripted_sequence") || FClassnameIs(entity, "aiscripted_sequence")) )
				CBaseEntity::Instance(entity)->Activate();
		}
		HoldRestoredPushers();
	}
	catch ( const std::exception & )
	{
		ALERT(at_error, "Half-Life Co-op: world restoration ran out of memory\n");
	}
	state.pending = PendingTransition();
	restoreRequested = FALSE;
	ALERT(at_console, "Half-Life Co-op: world activated %s, restored %u entities\n", activeMap.c_str(), (unsigned int)restoredEntities.size());
}

BOOL CoopWorldEntityRestored( const edict_t *entity )
{
	if ( !entity || entity->free )
		return FALSE;
	std::map<int, int>::const_iterator found = restoredEntities.find(ENTINDEX(const_cast<edict_t *>(entity)));
	return found != restoredEntities.end() && found->second == entity->serialnumber;
}

void CoopWorldThink( void )
{
	if ( heldPushers.empty() )
		return;
	if ( heldPushersMap != STRING(gpGlobals->mapname) )
		ClearHeldPushers();
	else if ( HasReadyPlayer() )
		ReleaseHeldPushers();
	else
		ApplyPusherHold();
}

void CoopWorldReset( void )
{
	ReleaseHeldPushers();
	CoopWorldCancel();
	state = WorldState();
	identities.clear();
	restoredEntities.clear();
	activeMap.clear();
	restoreGlobals = restoreRequested = FALSE;
}

BOOL CoopWorldWrite( FILE *file )
{
	if ( !file || !WriteU32(file, WORLD_VERSION) || !WriteU32(file, (unsigned int)state.maps.size()) )
		return FALSE;
	for ( std::map<std::string, Snapshot>::const_iterator i = state.maps.begin(); i != state.maps.end(); ++i )
		if ( !WriteSnapshot(file, i->second) )
			return FALSE;
	return WriteSnapshot(file, state.globals) && WriteString(file, state.pending.sourceMap) &&
		WriteString(file, state.pending.destinationMap) && WriteString(file, state.pending.landmark) &&
		WriteVector(file, state.pending.sourceOrigin);
}

BOOL CoopWorldValidate( FILE *file )
{
	try
	{
		WorldState candidate;
		return ReadState(file, candidate);
	}
	catch ( const std::exception & )
	{
		return FALSE;
	}
}

BOOL CoopWorldRead( FILE *file )
{
	if ( !CoopWorldStage( file ) )
		return FALSE;
	CoopWorldCommit();
	return TRUE;
}

BOOL CoopWorldStage( FILE *file )
{
	CoopWorldCancel();
	try
	{
		std::unique_ptr<WorldState> candidate( new WorldState );
		if ( !ReadState(file, *candidate) )
			return FALSE;
		stagedState.swap( candidate );
		return TRUE;
	}
	catch ( const std::exception & )
	{
		return FALSE;
	}
}

void CoopWorldCommit( void )
{
	if ( !stagedState )
		return;
	// Обмен готовыми контейнерами не выделяет память и не меняет сущности текущей карты
	state.maps.swap( stagedState->maps );
	state.globals.map.swap( stagedState->globals.map );
	state.globals.entities.swap( stagedState->globals.entities );
	state.globals.tokens.swap( stagedState->globals.tokens );
	state.globals.bytes.swap( stagedState->globals.bytes );
	state.pending.sourceMap.swap( stagedState->pending.sourceMap );
	state.pending.destinationMap.swap( stagedState->pending.destinationMap );
	state.pending.landmark.swap( stagedState->pending.landmark );
	Vector previousOrigin = state.pending.sourceOrigin;
	state.pending.sourceOrigin = stagedState->pending.sourceOrigin;
	stagedState->pending.sourceOrigin = previousOrigin;
	restoreGlobals = restoreRequested = TRUE;
	CoopWorldCancel();
}

void CoopWorldCancel( void )
{
	stagedState.reset();
}
