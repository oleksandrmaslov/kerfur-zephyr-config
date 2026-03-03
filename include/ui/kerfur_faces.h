#ifndef KERFUR_FACES_H_
#define KERFUR_FACES_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Bitmap encoding for face assets:
 * - Adafruit GFX drawBitmap compatible
 * - 1-bit monochrome
 * - row-major
 * - each byte stores 8 horizontal pixels
 * - LSB is left-most pixel in each byte
 */
struct kerfur_face_asset {
	uint8_t width;
	uint8_t height;
	const uint8_t *open_frame;
	const uint8_t *blink_frame;
};

const struct kerfur_face_asset *kerfur_face_default_asset(void);
const uint8_t *kerfur_face_frame(const struct kerfur_face_asset *face, bool blink);

#endif /* KERFUR_FACES_H_ */
