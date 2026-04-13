#ifndef KERFUR_MOTION_CLASSIFIER_H_
#define KERFUR_MOTION_CLASSIFIER_H_

#include <stdbool.h>
#include <stdint.h>

#include "behavior/pet_state.h"
#include "core/app_event.h"

int motion_classifier_init(int64_t now_ms);
void motion_classifier_on_event(const struct app_event *event, const struct pet_state *state);
void motion_classifier_set_debug_logging(bool enabled);
bool motion_classifier_is_debug_logging(void);
bool motion_classifier_is_enabled(void);

#endif /* KERFUR_MOTION_CLASSIFIER_H_ */
