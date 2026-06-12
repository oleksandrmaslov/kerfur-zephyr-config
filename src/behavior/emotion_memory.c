/*
 * Emotional memory — settings/NVS persistence of slow relationship traits.
 *
 * Record layout is versioned and packed; append new fields at the end and
 * bump EMOTION_MEMORY_VERSION when extending. A shorter stored record from
 * an older firmware is rejected (defaults win), which is acceptable for an
 * emotional seed — never let a parse ambiguity corrupt the pet's traits.
 *
 * Threading: all access happens on the app main thread (behavior engine
 * init/save) plus the settings_load() delivery from ble_manager_init(),
 * which runs on the same thread. No locking required.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "behavior/emotion_memory.h"
#include "behavior/pet_state.h"

LOG_MODULE_REGISTER(emotion_memory, CONFIG_LOG_DEFAULT_LEVEL);

#define EMOTION_MEMORY_VERSION  1U
#define SETTINGS_SUBTREE        "kerfur/emo"
#define SETTINGS_KEY_TRAITS     "traits"

/* flags bit assignments (records written before a bit existed read as 0). */
#define EMOTION_MEMORY_FLAG_WORN_EXPRESSIVE BIT(0)

struct emotion_memory_record {
	uint8_t version;
	uint8_t personality;
	int8_t attachment;
	int8_t trust;
	int8_t mood;
	uint8_t flags;
	uint8_t reserved[2];
	uint32_t lifetime_pets;
} __packed;

static struct emotion_memory g_loaded;
static bool g_loaded_valid;
static bool g_init_done;

static int16_t clamp_trait(int value)
{
	if (value < 0) {
		return 0;
	}
	if (value > 100) {
		return 100;
	}
	return (int16_t)value;
}

static int settings_set_handler(const char *name, size_t len, settings_read_cb read_cb,
				void *cb_arg)
{
	const char *next;
	struct emotion_memory_record rec;
	ssize_t rc;

	if (settings_name_steq(name, SETTINGS_KEY_TRAITS, &next) && (next == NULL)) {
		if (len != sizeof(rec)) {
			LOG_WRN("Ignoring traits record with unexpected len=%u",
				(unsigned)len);
			return -EINVAL;
		}

		rc = read_cb(cb_arg, &rec, sizeof(rec));
		if (rc != (ssize_t)sizeof(rec)) {
			return -EIO;
		}

		if (rec.version != EMOTION_MEMORY_VERSION) {
			LOG_WRN("Ignoring traits record version=%u", rec.version);
			return 0;
		}

		g_loaded.personality = (rec.personality < (uint8_t)PET_PERSONALITY_COUNT) ?
				       rec.personality : (uint8_t)PET_PERSONALITY_BALANCED;
		g_loaded.attachment = clamp_trait(rec.attachment);
		g_loaded.trust = clamp_trait(rec.trust);
		g_loaded.mood = clamp_trait(rec.mood);
		g_loaded.lifetime_pets = rec.lifetime_pets;
		g_loaded.worn_expressive =
			(rec.flags & EMOTION_MEMORY_FLAG_WORN_EXPRESSIVE) != 0U;
		g_loaded_valid = true;
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(kerfur_emo, SETTINGS_SUBTREE, NULL, settings_set_handler,
			       NULL, NULL);

void emotion_memory_init(void)
{
	int err;

	if (g_init_done) {
		return;
	}
	g_init_done = true;

	err = settings_subsys_init();
	if ((err != 0) && (err != -EALREADY)) {
		LOG_WRN("settings_subsys_init err=%d", err);
		return;
	}

	err = settings_load_subtree(SETTINGS_SUBTREE);
	if (err != 0) {
		LOG_WRN("settings_load_subtree err=%d", err);
	}
}

bool emotion_memory_get(struct emotion_memory *out)
{
	if ((out == NULL) || !g_loaded_valid) {
		return false;
	}

	*out = g_loaded;
	return true;
}

int emotion_memory_store(const struct emotion_memory *in)
{
	struct emotion_memory_record rec;
	int err;

	if (in == NULL) {
		return -EINVAL;
	}

	(void)memset(&rec, 0, sizeof(rec));
	rec.version = EMOTION_MEMORY_VERSION;
	rec.personality = (in->personality < (uint8_t)PET_PERSONALITY_COUNT) ?
			  in->personality : (uint8_t)PET_PERSONALITY_BALANCED;
	rec.attachment = (int8_t)clamp_trait(in->attachment);
	rec.trust = (int8_t)clamp_trait(in->trust);
	rec.mood = (int8_t)clamp_trait(in->mood);
	rec.flags = in->worn_expressive ? EMOTION_MEMORY_FLAG_WORN_EXPRESSIVE : 0U;
	rec.lifetime_pets = in->lifetime_pets;

	err = settings_save_one(SETTINGS_SUBTREE "/" SETTINGS_KEY_TRAITS,
				&rec, sizeof(rec));
	if (err != 0) {
		LOG_WRN("Failed to persist emotional memory (err=%d)", err);
		return err;
	}

	/* Keep the staging copy in sync so repeated get/store round-trips
	 * observe what was actually written. */
	g_loaded.personality = rec.personality;
	g_loaded.attachment = rec.attachment;
	g_loaded.trust = rec.trust;
	g_loaded.mood = rec.mood;
	g_loaded.lifetime_pets = rec.lifetime_pets;
	g_loaded.worn_expressive =
		(rec.flags & EMOTION_MEMORY_FLAG_WORN_EXPRESSIVE) != 0U;
	g_loaded_valid = true;

	LOG_INF("Emotional memory saved (att=%d trust=%d mood=%d pets=%u pers=%u)",
		rec.attachment, rec.trust, rec.mood, rec.lifetime_pets, rec.personality);
	return 0;
}
