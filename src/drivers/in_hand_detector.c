#include <string.h>

#include "drivers/in_hand_detector.h"

void in_hand_detector_init(struct in_hand_detector *detector, int64_t now_ms)
{
	if (detector == NULL) {
		return;
	}

	memset(detector, 0, sizeof(*detector));
	detector->state = IN_HAND_DETECTOR_SURFACE_STILL;
	detector->still_since_ms = now_ms;
}

void in_hand_detector_process(struct in_hand_detector *detector,
			      const struct in_hand_detector_input *input,
			      struct in_hand_detector_output *output)
{
	ARG_UNUSED(input);

	if ((detector == NULL) || (input == NULL) || (output == NULL)) {
		return;
	}

	memset(output, 0, sizeof(*output));
	output->state = detector->state;
	output->pickup_confidence = detector->pickup_confidence;
	output->in_hand_confidence = detector->in_hand_confidence;
	output->picked_up_recently = detector->pickup_candidate_reported;
	output->in_hand = detector->in_hand;
}
