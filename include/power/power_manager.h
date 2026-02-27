#ifndef KERFUR_POWER_MANAGER_H_
#define KERFUR_POWER_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#include "behavior/pet_state.h"
#include "core/app_event.h"

void power_manager_init(int64_t now_ms);
void power_manager_on_event(const struct app_event *event, const struct pet_state *state);
bool power_manager_poll(int64_t now_ms, struct app_event *out_event);
bool power_manager_is_display_blanked(void);

#endif /* KERFUR_POWER_MANAGER_H_ */
