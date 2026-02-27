#ifndef KERFUR_BEHAVIOR_ENGINE_H_
#define KERFUR_BEHAVIOR_ENGINE_H_

#include <stdint.h>

#include "behavior/pet_state.h"
#include "core/app_event.h"

void behavior_engine_init(struct pet_state *state, int64_t now_ms);
void behavior_engine_handle_event(struct pet_state *state, const struct app_event *event);

const char *pet_mode_str(enum pet_mode mode);
const char *pet_expression_str(enum pet_expression expression);

#endif /* KERFUR_BEHAVIOR_ENGINE_H_ */
