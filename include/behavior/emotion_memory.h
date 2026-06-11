#ifndef KERFUR_EMOTION_MEMORY_H_
#define KERFUR_EMOTION_MEMORY_H_

/*
 * Emotional memory — persistence of the slow relationship traits.
 *
 * Only the values that represent "who this Kerfur has become" are stored
 * (attachment, trust, mood, lifetime petting count, personality). Fast
 * drives (arousal, stress, boredom, ...) are session state and always
 * boot fresh. Storage is one small settings/NVS record; the behavior
 * engine owns all save throttling and seeding policy.
 */

#include <stdbool.h>
#include <stdint.h>

struct emotion_memory {
	uint8_t personality;   /* enum pet_personality */
	int16_t attachment;    /* 0..100 */
	int16_t trust;         /* 0..100 */
	int16_t mood;          /* 0..100 */
	uint32_t lifetime_pets;
};

#if defined(CONFIG_KERFUR_EMOTION_MEMORY)

/* Initialize settings backend and synchronously load any stored record.
 * Safe to call before BLE/settings_load(); idempotent. */
void emotion_memory_init(void);

/* Copy the loaded record into *out. Returns false when nothing valid was
 * ever stored (first boot / wiped flash). */
bool emotion_memory_get(struct emotion_memory *out);

/* Persist *in immediately (caller throttles). Returns 0 on success. */
int emotion_memory_store(const struct emotion_memory *in);

#else /* !CONFIG_KERFUR_EMOTION_MEMORY */

static inline void emotion_memory_init(void)
{
}

static inline bool emotion_memory_get(struct emotion_memory *out)
{
	(void)out;
	return false;
}

static inline int emotion_memory_store(const struct emotion_memory *in)
{
	(void)in;
	return 0;
}

#endif /* CONFIG_KERFUR_EMOTION_MEMORY */

#endif /* KERFUR_EMOTION_MEMORY_H_ */
