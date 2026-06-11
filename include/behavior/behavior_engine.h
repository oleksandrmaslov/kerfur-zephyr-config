#ifndef KERFUR_BEHAVIOR_ENGINE_H_
#define KERFUR_BEHAVIOR_ENGINE_H_

#include <stddef.h>
#include <stdint.h>

#include "behavior/pet_state.h"
#include "core/app_event.h"

void behavior_engine_init(struct pet_state *state, int64_t now_ms);
void behavior_engine_handle_event(struct pet_state *state, const struct app_event *event);

const char *pet_mode_str(enum pet_mode mode);
const char *pet_expression_str(enum pet_expression expression);
const char *pet_display_state_str(enum pet_display_state state);
const char *pet_personality_str(enum pet_personality personality_id);
void behavior_engine_status_dump(const struct pet_state *state, char *buffer, size_t buffer_len);
/* Compact dump of the emotional state only (drives, mood, context,
 * intent, personality) — diagnostics ("emotion print"). */
void behavior_engine_emotion_dump(const struct pet_state *state, char *buffer, size_t buffer_len);

#endif /* KERFUR_BEHAVIOR_ENGINE_H_ */
