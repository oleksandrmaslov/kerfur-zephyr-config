#ifndef KERFUR_DISPLAY_POLICY_H_
#define KERFUR_DISPLAY_POLICY_H_

#include <stdint.h>

#include "behavior/pet_state.h"
#include "core/app_event.h"

void display_policy_init(struct pet_state *state, int64_t now_ms);
void display_policy_on_event(struct pet_state *state, const struct app_event *event);
void display_policy_on_tick_100ms(struct pet_state *state, int64_t now_ms);
enum pet_display_state display_policy_get_state(const struct pet_state *state);

#endif /* KERFUR_DISPLAY_POLICY_H_ */
