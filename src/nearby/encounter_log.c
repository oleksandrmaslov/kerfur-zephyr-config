#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "nearby/encounter_log.h"

LOG_MODULE_REGISTER(encounter_log, CONFIG_LOG_DEFAULT_LEVEL);

#define ENCOUNTER_LOG_SIZE CONFIG_KERFUR_NEARBY_ENCOUNTER_LOG_SIZE

static struct encounter_record g_records[ENCOUNTER_LOG_SIZE];
static size_t g_head;  /* next write slot */
static size_t g_count; /* number of valid entries */
static uint32_t g_next_encounter_id = 1U;

/* fixed-point IIR state for rssi_avg per slot — kept out of the record */
static int16_t g_rssi_avg_x16[ENCOUNTER_LOG_SIZE];
static uint16_t g_rssi_samples[ENCOUNTER_LOG_SIZE];

static K_MUTEX_DEFINE(g_log_mutex);

void encounter_log_init(void)
{
	k_mutex_lock(&g_log_mutex, K_FOREVER);
	memset(g_records, 0, sizeof(g_records));
	memset(g_rssi_avg_x16, 0, sizeof(g_rssi_avg_x16));
	memset(g_rssi_samples, 0, sizeof(g_rssi_samples));
	g_head = 0U;
	g_count = 0U;
	g_next_encounter_id = 1U;
	k_mutex_unlock(&g_log_mutex);

	LOG_INF("Encounter log init (size=%d)", ENCOUNTER_LOG_SIZE);
}

static int find_slot_locked(uint32_t encounter_id)
{
	size_t i;

	if (encounter_id == 0U) {
		return -1;
	}

	for (i = 0U; i < ENCOUNTER_LOG_SIZE; i++) {
		if (g_records[i].encounter_id == encounter_id) {
			return (int)i;
		}
	}

	return -1;
}

uint32_t encounter_log_begin(uint32_t ephemeral_id, uint8_t character_id,
			     int8_t friend_index, int64_t now_ms)
{
	struct encounter_record *rec;
	uint32_t id;
	size_t slot;

	k_mutex_lock(&g_log_mutex, K_FOREVER);

	slot = g_head;
	rec = &g_records[slot];

	id = g_next_encounter_id++;
	if (g_next_encounter_id == 0U) {
		g_next_encounter_id = 1U;
	}

	memset(rec, 0, sizeof(*rec));
	rec->encounter_id = id;
	rec->ephemeral_id = ephemeral_id;
	rec->character_id = character_id;
	rec->friend_index = friend_index;
	rec->start_uptime_ms = now_ms;
	rec->end_uptime_ms = 0;
	rec->encounter_type = (uint8_t)KERFUR_ENCOUNTER_FIRST_CONTACT;
	rec->rssi_avg = 0;
	rec->rssi_max = INT8_MIN;

	g_rssi_avg_x16[slot] = 0;
	g_rssi_samples[slot] = 0U;

	g_head = (g_head + 1U) % ENCOUNTER_LOG_SIZE;
	if (g_count < ENCOUNTER_LOG_SIZE) {
		g_count++;
	}

	k_mutex_unlock(&g_log_mutex);

	LOG_DBG("encounter begin id=%u peer=0x%08x", id, ephemeral_id);
	return id;
}

void encounter_log_update(uint32_t encounter_id, int8_t rssi)
{
	int slot;
	struct encounter_record *rec;
	int32_t sample;
	int32_t avg;

	k_mutex_lock(&g_log_mutex, K_FOREVER);

	slot = find_slot_locked(encounter_id);
	if (slot < 0) {
		k_mutex_unlock(&g_log_mutex);
		return;
	}

	rec = &g_records[slot];
	sample = (int32_t)rssi * 16;

	if (g_rssi_samples[slot] == 0U) {
		g_rssi_avg_x16[slot] = (int16_t)sample;
	} else {
		/* IIR alpha = 1/4 */
		avg = g_rssi_avg_x16[slot];
		avg = avg + ((sample - avg) / 4);
		g_rssi_avg_x16[slot] = (int16_t)avg;
	}
	if (g_rssi_samples[slot] < UINT16_MAX) {
		g_rssi_samples[slot]++;
	}

	rec->rssi_avg = (int8_t)(g_rssi_avg_x16[slot] / 16);
	if (rssi > rec->rssi_max) {
		rec->rssi_max = rssi;
	}

	k_mutex_unlock(&g_log_mutex);
}

void encounter_log_set_type(uint32_t encounter_id, enum kerfur_encounter_type type)
{
	int slot;

	k_mutex_lock(&g_log_mutex, K_FOREVER);
	slot = find_slot_locked(encounter_id);
	if (slot >= 0) {
		g_records[slot].encounter_type = (uint8_t)type;
	}
	k_mutex_unlock(&g_log_mutex);
}

void encounter_log_mark_reaction(uint32_t encounter_id)
{
	int slot;

	k_mutex_lock(&g_log_mutex, K_FOREVER);
	slot = find_slot_locked(encounter_id);
	if (slot >= 0) {
		g_records[slot].reaction_triggered = true;
	}
	k_mutex_unlock(&g_log_mutex);
}

void encounter_log_end(uint32_t encounter_id, int64_t now_ms)
{
	int slot;

	k_mutex_lock(&g_log_mutex, K_FOREVER);
	slot = find_slot_locked(encounter_id);
	if (slot >= 0) {
		g_records[slot].end_uptime_ms = now_ms;
	}
	k_mutex_unlock(&g_log_mutex);

	LOG_DBG("encounter end id=%u", encounter_id);
}

size_t encounter_log_pending(struct encounter_record *out, size_t max)
{
	size_t written = 0U;
	size_t i;

	if ((out == NULL) || (max == 0U)) {
		return 0U;
	}

	k_mutex_lock(&g_log_mutex, K_FOREVER);
	for (i = 0U; (i < ENCOUNTER_LOG_SIZE) && (written < max); i++) {
		const struct encounter_record *rec = &g_records[i];

		if ((rec->encounter_id != 0U) && (rec->end_uptime_ms != 0) && !rec->uploaded) {
			out[written++] = *rec;
		}
	}
	k_mutex_unlock(&g_log_mutex);

	return written;
}

void encounter_log_mark_uploaded(uint32_t encounter_id)
{
	int slot;

	k_mutex_lock(&g_log_mutex, K_FOREVER);
	slot = find_slot_locked(encounter_id);
	if (slot >= 0) {
		g_records[slot].uploaded = true;
	}
	k_mutex_unlock(&g_log_mutex);
}

size_t encounter_log_dump(struct encounter_record *out, size_t max)
{
	size_t written = 0U;
	size_t i;

	if ((out == NULL) || (max == 0U)) {
		return 0U;
	}

	k_mutex_lock(&g_log_mutex, K_FOREVER);
	for (i = 0U; (i < ENCOUNTER_LOG_SIZE) && (written < max); i++) {
		if (g_records[i].encounter_id != 0U) {
			out[written++] = g_records[i];
		}
	}
	k_mutex_unlock(&g_log_mutex);

	return written;
}

size_t encounter_log_count(void)
{
	size_t count;

	k_mutex_lock(&g_log_mutex, K_FOREVER);
	count = g_count;
	k_mutex_unlock(&g_log_mutex);

	return count;
}
