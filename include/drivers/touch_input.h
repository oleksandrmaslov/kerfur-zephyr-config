#ifndef KERFUR_TOUCH_INPUT_H_
#define KERFUR_TOUCH_INPUT_H_

#include "drivers/touch_gestures.h"

int touch_input_init(void);

/* Maps a recognized gesture to the corresponding APP_EVENT_USER_* event and
 * publishes it. Shared so every touch front-end (the GPIO stub here, the
 * IQS7222A driver) emits identical events; used as the touch_gestures callback. */
void touch_input_publish_gesture(enum touch_gesture gesture, void *user_data);

#endif /* KERFUR_TOUCH_INPUT_H_ */
