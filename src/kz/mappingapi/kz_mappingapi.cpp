
#include "kz/kz.h"
#include "movement/movement.h"
#include "kz_mappingapi.h"
#include "entity2/entitykeyvalues.h"
#include "sdk/entity/cbasetrigger.h"
#include "utils/ctimer.h"

#include "tier0/memdbgon.h"

#define KEY_TRIGGER_TYPE         "timer_trigger_type"
#define KEY_IS_COURSE_DESCRIPTOR "timer_course_descriptor"
#define INVALID_STAGE_NUMBER     -1
#define INVALID_COURSE_NUMBER    -1

enum
{
	MAPI_ERR_TOO_MANY_TRIGGERS = 1 << 0,
	MAPI_ERR_TOO_MANY_COURSES = 1 << 1,
};

struct KzTrigger
{
	KzTriggerType type;
	CEntityHandle entity;
	i32 hammerId;

	union
	{
		Modifier modifier;
		Antibhop antibhop;
		Zone zone;
		StageZone stageZone;
		Teleport teleport;
	};
};

static_global struct
{
	i32 triggerCount;
	KzTrigger triggers[2048];
	i32 courseCount;
	KzCourseDescriptor courses[512];

	i32 errorFlags;
	i32 errorCount;
	char errors[32][256];
} g_mappingApi;

static_global CTimer<> *errorTimer;

static_function MappingInterface g_mappingInterface;
MappingInterface *g_pMappingApi = nullptr;

static_function void Mapi_Error(char *format, ...)
{
	i32 errorIndex = g_mappingApi.errorCount;
	if (errorIndex >= Q_ARRAYSIZE(g_mappingApi.errors[0]))
	{
		return;
	}
	else if (errorIndex == Q_ARRAYSIZE(g_mappingApi.errors[0]) - 1)
	{
		snprintf(g_mappingApi.errors[errorIndex], sizeof(g_mappingApi.errors[errorIndex]), "Too many errors to list!");
		return;
	}

	va_list args;
	va_start(args, format);
	vsnprintf(g_mappingApi.errors[errorIndex], sizeof(g_mappingApi.errors[errorIndex]), format, args);
	va_end(args);

	g_mappingApi.errorCount++;
}

static_function f64 Mapi_PrintErrors()
{
	char *prefix = "{red} ERROR: ";
	if (g_mappingApi.errorFlags & MAPI_ERR_TOO_MANY_TRIGGERS)
	{
		utils::CPrintChatAll("%sToo many Mapping API triggers! Maximum is %i!", prefix, Q_ARRAYSIZE(g_mappingApi.triggers));
	}
	if (g_mappingApi.errorFlags & MAPI_ERR_TOO_MANY_COURSES)
	{
		utils::CPrintChatAll("%sToo many Courses! Maximum is %i!", prefix, Q_ARRAYSIZE(g_mappingApi.courses));
	}
	for (i32 i = 0; i < g_mappingApi.errorCount; i++)
	{
		utils::CPrintChatAll("%s%s", prefix, g_mappingApi.errors[i]);
	}

	return 30.0;
}

static_function bool Mapi_AddTrigger(KzTrigger trigger)
{
	if (g_mappingApi.triggerCount >= Q_ARRAYSIZE(g_mappingApi.triggers))
	{
		assert(0);
		g_mappingApi.errorFlags |= MAPI_ERR_TOO_MANY_TRIGGERS;
		return false;
	}

	g_mappingApi.triggers[g_mappingApi.triggerCount++] = trigger;
	return true;
}

static_function void Mapi_AddCourse(KzCourseDescriptor course)
{
	assert(g_mappingApi.courseCount < Q_ARRAYSIZE(g_mappingApi.courses));
	if (g_mappingApi.courseCount >= Q_ARRAYSIZE(g_mappingApi.courses))
	{
		assert(0);
		g_mappingApi.errorFlags |= MAPI_ERR_TOO_MANY_COURSES;
		return;
	}

	g_mappingApi.courses[g_mappingApi.courseCount++] = course;
}

// Example keyvalues:
/*
	timer_anti_bhop_time: 0.2
	timer_teleport_relative: true
	timer_teleport_reorient_player: false
	timer_teleport_reset_speed: false
	timer_teleport_use_dest_angles: false
	timer_teleport_delay: 0
	timer_teleport_destination: landmark_teleport
	timer_zone_stage_number: 1
	timer_modifier_enable_slide: false
	timer_modifier_disable_jumpstats: false
	timer_modifier_disable_teleports: false
	timer_modifier_disable_checkpoints: false
	timer_modifier_disable_pause: false
	timer_trigger_type: 10
	wait: 1
	spawnflags: 4097
	StartDisabled: false
	useLocalOffset: false
	classname: trigger_multiple
	origin: 1792.000000 768.000000 -416.000000
	angles: 0.000000 0.000000 0.000000
	scales: 1.000000 1.000000 1.000000
	hammerUniqueId: 48
	model: maps\kz_mapping_api\entities\unnamed_48.vmdl
*/
static_function void Mapi_OnTriggerMultipleSpawn(const EntitySpawnInfo_t *info)
{
#if 0
	// Debug print for all keyvalues
	FOR_EACH_ENTITYKEY(ekv, iter)
	{
		auto kv = ekv->GetKeyValue(iter);
		if (!kv)
		{
			continue;
		}
		CBufferStringGrowable<128> bufferStr;
		const char *key = ekv->GetEntityKeyId(iter).GetString();
		const char *value = kv->ToString(bufferStr);
		Msg("\t%s: %s\n", key, value);
	}
#endif

	const CEntityKeyValues *ekv = info->m_pKeyValues;
	i32 hammerId = ekv->GetInt("hammerUniqueId", -1);
	KzTriggerType type = (KzTriggerType)ekv->GetInt(KEY_TRIGGER_TYPE, KZTRIGGER_DISABLED);

	if (type <= KZTRIGGER_DISABLED || type >= KZTRIGGER_COUNT)
	{
		assert(0);
		Mapi_Error("Trigger type %i is invalid and out of range (%i-%i) for trigger with Hammer ID %i!", type, KZTRIGGER_DISABLED, KZTRIGGER_COUNT,
				   hammerId);
		return;
	}

	KzTrigger trigger = {};
	trigger.type = type;
	trigger.hammerId = hammerId;
	trigger.entity = info->m_pEntity->GetRefEHandle();

	switch (type)
	{
		case KZTRIGGER_MODIFIER:
		{
			trigger.modifier.disablePausing = ekv->GetBool("timer_modifier_disable_pause");
			trigger.modifier.disableCheckpoints = ekv->GetBool("timer_modifier_disable_checkpoints");
			trigger.modifier.disableTeleports = ekv->GetBool("timer_modifier_disable_teleports");
			trigger.modifier.disableJumpstats = ekv->GetBool("timer_modifier_disable_jumpstats");
			trigger.modifier.enableSlide = ekv->GetBool("timer_modifier_enable_slide");
		}
		break;

		// TODO:
		case KZTRIGGER_RESET_CHECKPOINTS:
		case KZTRIGGER_SINGLE_BHOP_RESET:
		{
		}
		break;

		case KZTRIGGER_ANTI_BHOP:
		{
			trigger.antibhop.time = ekv->GetFloat("timer_anti_bhop_time");
		}
		break;

		case KZTRIGGER_ZONE_START:
		case KZTRIGGER_ZONE_END:
		case KZTRIGGER_ZONE_SPLIT:
		case KZTRIGGER_ZONE_CHECKPOINT:
		case KZTRIGGER_ZONE_STAGE:
		{
			const char *courseDescriptor = ekv->GetString("timer_zone_course_descriptor");
			V_snprintf(trigger.zone.courseDescriptor, sizeof(trigger.zone.courseDescriptor), "%s", courseDescriptor);

			if (type == KZTRIGGER_ZONE_STAGE)
			{
				trigger.stageZone.stageNumber = ekv->GetInt("timer_zone_stage_number", INVALID_STAGE_NUMBER);
			}
		}
		break;

		case KZTRIGGER_TELEPORT:
		case KZTRIGGER_MULTI_BHOP:
		case KZTRIGGER_SINGLE_BHOP:
		case KZTRIGGER_SEQUENTIAL_BHOP:
		{
			const char *destination = ekv->GetString("timer_teleport_destination");
			V_snprintf(trigger.teleport.destination, sizeof(trigger.teleport.destination), "%s", destination);
			trigger.teleport.delay = ekv->GetFloat("timer_anti_bhop_time", -1.0f);
			trigger.teleport.useDestinationAngles = ekv->GetBool("timer_teleport_use_dest_angles");
			trigger.teleport.resetSpeed = ekv->GetBool("timer_teleport_reset_speed");
			trigger.teleport.reorientPlayer = ekv->GetBool("timer_teleport_reorient_player");
			trigger.teleport.relative = ekv->GetBool("timer_teleport_relative");
		}
		break;

		default:
		{
			assert(0);
			return;
		}
		break;
	}

	Mapi_AddTrigger(trigger);
}

static_function void Mapi_OnInfoTargetSpawn(const EntitySpawnInfo_t *info)
{
	const CEntityKeyValues *ekv = info->m_pKeyValues;

	if (!ekv->GetBool(KEY_IS_COURSE_DESCRIPTOR))
	{
		return;
	}

	KzCourseDescriptor course = {};
	course.number = ekv->GetInt("timer_course_number", INVALID_COURSE_NUMBER);
	course.hammerId = ekv->GetInt("hammerUniqueId", -1);

	// TODO: make sure course descriptor names are unique!

	if (course.number <= INVALID_COURSE_NUMBER)
	{
		Mapi_Error("Course number must be bigger than -1! Course descriptor Hammer ID %i", course.hammerId);
		return;
	}

	V_snprintf(course.name, sizeof(course.name), "%s", ekv->GetString("timer_course_name"));
	if (!course.name[0])
	{
		Mapi_Error("Course name is empty! Course number %i. Course descriptor Hammer ID %i!", course.number, course.hammerId);
		return;
	}

	V_snprintf(course.entityTargetname, sizeof(course.entityTargetname), "%s", ekv->GetString("targetname"));
	if (!course.entityTargetname[0])
	{
		Mapi_Error("Course targetname is empty! Course name \"%s\". Course number %i. Course descriptor Hammer ID %i!", course.name, course.number,
				   course.hammerId);
		return;
	}

	course.disableCheckpoints = ekv->GetBool("timer_course_disable_checkpoint");

	Mapi_AddCourse(course);
}

static_function KzTrigger *Mapi_FindKzTrigger(CBaseTrigger *trigger)
{
	KzTrigger *result = nullptr;
	if (!trigger->m_pEntity)
	{
		return result;
	}

	CEntityHandle triggerHandle = trigger->GetRefEHandle();
	if (!trigger || !triggerHandle.IsValid() || trigger->m_pEntity->m_flags & EF_IS_INVALID_EHANDLE)
	{
		return result;
	}

	for (i32 i = 0; i < g_mappingApi.triggerCount; i++)
	{
		if (triggerHandle == g_mappingApi.triggers[i].entity)
		{
			result = &g_mappingApi.triggers[i];
			break;
		}
	}

	return result;
}

static_function const KzCourseDescriptor *Mapi_FindCourse(const char *targetname)
{
	KzCourseDescriptor *result = nullptr;
	if (!targetname)
	{
		return result;
	}

	for (i32 i = 0; i < g_mappingApi.courseCount; i++)
	{
		if (V_stricmp(g_mappingApi.courses[i].entityTargetname, targetname) == 0)
		{
			result = &g_mappingApi.courses[i];
			break;
		}
	}

	return result;
}

bool MappingInterface::IsTriggerATimerZone(CBaseTrigger *trigger)
{
	KzTrigger *mvTrigger = Mapi_FindKzTrigger(trigger);
	if (!mvTrigger)
	{
		return false;
	}
	static_assert(KZTRIGGER_ZONE_START == 5 && KZTRIGGER_ZONE_STAGE == 9,
				  "Don't forget to change this function when changing the KzTriggerType enum!!!");
	return mvTrigger->type >= KZTRIGGER_ZONE_START && mvTrigger->type <= KZTRIGGER_ZONE_STAGE;
}

void Mappingapi_Initialize()
{
	g_mappingApi = {};
	g_pMappingApi = &g_mappingInterface;

	errorTimer = StartTimer(Mapi_PrintErrors, true);
}

void MappingInterface::OnTriggerMultipleStartTouchPost(KZPlayer *player, CBaseTrigger *trigger)
{
	KzTrigger *touched = Mapi_FindKzTrigger(trigger);
	if (!touched)
	{
		return;
	}

	const KzCourseDescriptor *course = nullptr;
	switch (touched->type)
	{
		case KZTRIGGER_ZONE_START:
		case KZTRIGGER_ZONE_END:
		case KZTRIGGER_ZONE_SPLIT:
		case KZTRIGGER_ZONE_CHECKPOINT:
		case KZTRIGGER_ZONE_STAGE:
		{
			course = Mapi_FindCourse(touched->zone.courseDescriptor);
			if (!course)
			{
				Mapi_Error("trigger_multiple StartTouch: Couldn't find course descriptor from name %s!", touched->zone.courseDescriptor);
				return;
			}
		}
		break;
	}

	switch (touched->type)
	{
		case KZTRIGGER_MODIFIER:
		case KZTRIGGER_RESET_CHECKPOINTS:
		case KZTRIGGER_SINGLE_BHOP_RESET:
		case KZTRIGGER_ANTI_BHOP:
			break;

		case KZTRIGGER_ZONE_START:
		case KZTRIGGER_ZONE_END:
		case KZTRIGGER_ZONE_SPLIT:
		case KZTRIGGER_ZONE_CHECKPOINT:
		{
			player->ZoneStartTouch(course, touched->type);
		}
		break;

		case KZTRIGGER_ZONE_STAGE:
		{
			player->StageZoneStartTouch(course, touched->stageZone.stageNumber);
		}
		break;

		case KZTRIGGER_TELEPORT:
		case KZTRIGGER_MULTI_BHOP:
		case KZTRIGGER_SINGLE_BHOP:
		case KZTRIGGER_SEQUENTIAL_BHOP:
			break;
		default:
			break;
	}
}

void MappingInterface::OnTriggerMultipleEndTouchPost(KZPlayer *player, CBaseTrigger *trigger)
{
	KzTrigger *touched = Mapi_FindKzTrigger(trigger);
	if (!touched)
	{
		return;
	}

	const KzCourseDescriptor *course = nullptr;
	switch (touched->type)
	{
		case KZTRIGGER_ZONE_START:
		case KZTRIGGER_ZONE_END:
		case KZTRIGGER_ZONE_SPLIT:
		case KZTRIGGER_ZONE_CHECKPOINT:
		case KZTRIGGER_ZONE_STAGE:
		{
			course = Mapi_FindCourse(touched->zone.courseDescriptor);
			if (!course)
			{
				Mapi_Error("trigger_multiple EndTouch: Couldn't find course descriptor from name %s!", touched->zone.courseDescriptor);
				return;
			}
		}
		break;
	}

	switch (touched->type)
	{
		case KZTRIGGER_ZONE_START:
		case KZTRIGGER_ZONE_SPLIT:
		case KZTRIGGER_ZONE_CHECKPOINT:
		{
			player->ZoneEndTouch(course, touched->type);
		}
		break;

		case KZTRIGGER_ZONE_STAGE:
		{
			player->StageZoneEndTouch(course, touched->stageZone.stageNumber);
		}
		break;

		default:
			break;
	}
}

void MappingInterface::OnSpawnPost(int count, const EntitySpawnInfo_t *info)
{
	if (!info)
	{
		return;
	}

	for (i32 i = 0; i < count; i++)
	{
		auto ekv = info[i].m_pKeyValues;
		if (!info[i].m_pEntity || !ekv || !info[i].m_pEntity->GetClassname())
		{
			continue;
		}
		const char *classname = info[i].m_pEntity->GetClassname();
		if (V_stricmp(classname, "trigger_multiple") == 0)
		{
			Mapi_OnTriggerMultipleSpawn(&info[i]);
		}
		else if (V_stricmp(classname, "info_target_server_only") == 0)
		{
			Mapi_OnInfoTargetSpawn(&info[i]);
		}
	}
}
