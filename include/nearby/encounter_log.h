#ifndef KERFUR_ENCOUNTER_LOG_H_
#define KERFUR_ENCOUNTER_LOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum kerfur_encounter_type {
	KERFUR_ENCOUNTER_FIRST_CONTACT = 0,
	KERFUR_ENCOUNTER_GREETING,
	KERFUR_ENCOUNTER_PLAY,
	KERFUR_ENCOUNTER_WALK_TOGETHER,
};

struct encounter_record {
	uint32_t encounter_id;
	uint32_t ephemeral_id;
	int64_t  start_uptime_ms;
	int64_t  end_uptime_ms;        /* 0 if still active */
	int8_t   rssi_avg;
	int8_t   rssi_max;
	int8_t   friend_index;         /* -1 = unknown; >=0 = slot in friend table */
	uint8_t  character_id;
	uint8_t  encounter_type;       /* enum kerfur_encounter_type */
	bool     reaction_triggered;
	bool     uploaded;
	bool     phone_notified;
};

void encounter_log_init(void);
uint32_t encounter_log_begin(uint32_t ephemeral_id, uint8_t character_id,
			     int8_t friend_index, int64_t now_ms);
void encounter_log_update(uint32_t encounter_id, int8_t rssi);
void encounter_log_set_type(uint32_t encounter_id, enum kerfur_encounter_type type);
void encounter_log_mark_reaction(uint32_t encounter_id);
void encounter_log_end(uint32_t encounter_id, int64_t now_ms);
size_t encounter_log_pending(struct encounter_record *out, size_t max);
void encounter_log_mark_uploaded(uint32_t encounter_id);
size_t encounter_log_dump(struct encounter_record *out, size_t max);
size_t encounter_log_count(void);

#endif /* KERFUR_ENCOUNTER_LOG_H_ */
