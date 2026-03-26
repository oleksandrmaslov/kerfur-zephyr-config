#ifndef KERFUR_FACE_BITMAP_H_
#define KERFUR_FACE_BITMAP_H_

#include <stdint.h>

struct kerfur_face_bitmap {
	uint8_t width;
	uint8_t height;
	uint8_t stride;
	const uint8_t *data;
};

#endif /* KERFUR_FACE_BITMAP_H_ */
