#ifndef KERFUR_IQS7222A_H_
#define KERFUR_IQS7222A_H_

/*
 * Azoteq IQS7222A capacitive touch front-end.
 *
 * Reads per-channel touch state (TOUCH_EVENT_STATES, reg 0x13) over I2C, gated
 * by the device RDY line, and feeds the channel bitmask into the shared
 * touch_gestures engine (so tap/stroke/hold come out as the same
 * APP_EVENT_USER_* events as the GPIO front-end).
 *
 * Requires a `touch_iqs0` devicetree alias (an I2C node with an `rdy-gpios`
 * property). Returns -ENODEV when that alias is absent, so the firmware still
 * builds/runs without the touch hardware wired.
 */
int iqs7222a_init(void);

#endif /* KERFUR_IQS7222A_H_ */
