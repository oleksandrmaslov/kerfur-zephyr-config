#ifndef KERFUR_FACES_H_
#define KERFUR_FACES_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "behavior/micro_reaction.h"
#include "behavior/pet_state.h"

/* Adafruit GFX-style 1bpp bitmap, row-major, LSB left-most in each byte. */
struct kerfur_bitmap {
	uint8_t width;
	uint8_t height;
	const uint8_t *data;
};

enum kerfur_face_visual {
	KERFUR_FACE_CALM = 0,
	KERFUR_FACE_CURIOUS,
	KERFUR_FACE_CONTENT,
	KERFUR_FACE_HAPPY,
	KERFUR_FACE_PLAYFUL,
	KERFUR_FACE_SLEEPY,
	KERFUR_FACE_ANNOYED,
	KERFUR_FACE_COZY,
	KERFUR_FACE_ASLEEP,
	KERFUR_FACE_COUNT,
};

enum kerfur_face_eye_mode {
	KERFUR_FACE_EYE_OPEN = 0,
	KERFUR_FACE_EYE_BLINK,
};

enum kerfur_face_pose_flags {
	KERFUR_FACE_FLAG_SHOW_BROWS = BIT(0),
	KERFUR_FACE_FLAG_SHOW_WHISKERS = BIT(1),
	KERFUR_FACE_FLAG_SLEEPY = BIT(2),
	KERFUR_FACE_FLAG_SHOW_MOUTH_OPEN = BIT(3),
};

enum kerfur_face_frame_eye_mode {
	KERFUR_FACE_FRAME_EYE_KEEP = 0,
	KERFUR_FACE_FRAME_EYE_OPEN,
	KERFUR_FACE_FRAME_EYE_BLINK,
};

struct kerfur_face_asset_pack {
	const struct kerfur_bitmap *eye_open_left;
	const struct kerfur_bitmap *eye_open_right;
	const struct kerfur_bitmap *eye_blink;
	const struct kerfur_bitmap *mouth;
	const struct kerfur_bitmap *mouth_open;
	const struct kerfur_bitmap *whisker_left;
	const struct kerfur_bitmap *whisker_right;
	const struct kerfur_bitmap *brow_left;
	const struct kerfur_bitmap *brow_right;
	uint8_t eye_left_x;
	uint8_t eye_right_x;
	uint8_t eye_y;
	uint8_t blink_left_x;
	uint8_t blink_right_x;
	uint8_t blink_y;
	uint8_t mouth_x;
	uint8_t mouth_y;
	uint8_t mouth_open_x;
	uint8_t mouth_open_y;
	uint8_t whisker_left_x;
	uint8_t whisker_right_x;
	uint8_t whisker_y;
	uint8_t brow_left_x;
	uint8_t brow_right_x;
	uint8_t brow_y;
};

struct kerfur_face_pose {
	int8_t eye_dx;
	int8_t eye_dy;
	int8_t mouth_dx;
	int8_t mouth_dy;
	int8_t mouth_open_dy;
	int8_t whisker_dy;
	int8_t brow_dy;
	uint8_t flags;
	uint8_t eye_mode;
};

struct kerfur_face_frame {
	int8_t eye_dx;
	int8_t eye_dy;
	int8_t mouth_dx;
	int8_t mouth_dy;
	int8_t mouth_open_dy;
	int8_t whisker_dy;
	int8_t brow_dy;
	uint8_t set_flags;
	uint8_t clear_flags;
	uint8_t eye_mode;
	uint16_t duration_ms;
};

struct kerfur_face_animation {
	const char *name;
	const struct kerfur_face_frame *frames;
	uint8_t frame_count;
	bool loop;
};

struct kerfur_face_profile {
	enum kerfur_face_visual visual;
	const char *name;
	int8_t eye_dx;
	int8_t eye_dy;
	int8_t mouth_dx;
	int8_t mouth_dy;
	int8_t brow_dy;
	bool show_brows;
	bool show_whiskers;
	bool sleepy_eyes;
};

enum kerfur_face_visual kerfur_face_visual_for_expression(enum pet_expression expression);
const struct kerfur_face_profile *kerfur_face_profile_get(enum kerfur_face_visual visual);
const char *kerfur_face_visual_str(enum kerfur_face_visual visual);
const struct kerfur_face_asset_pack *kerfur_face_assets_get(enum kerfur_face_visual visual);
void kerfur_face_pose_init(enum kerfur_face_visual visual, struct kerfur_face_pose *pose);
bool kerfur_face_pose_apply_animation(struct kerfur_face_pose *pose,
				      const struct kerfur_face_animation *animation,
				      int64_t elapsed_ms);
const struct kerfur_face_animation *kerfur_face_idle_animation_get(enum kerfur_face_visual visual,
								   bool ambient);
const struct kerfur_face_animation *kerfur_face_reaction_animation_get(enum kerfur_face_visual visual,
								      enum micro_reaction_type reaction);

const struct kerfur_bitmap *kerfur_face_eye_open_left(void);
const struct kerfur_bitmap *kerfur_face_eye_open_right(void);
const struct kerfur_bitmap *kerfur_face_eye_blink(void);
const struct kerfur_bitmap *kerfur_face_mouth(void);
const struct kerfur_bitmap *kerfur_face_whisker_left(void);
const struct kerfur_bitmap *kerfur_face_whisker_right(void);
const struct kerfur_bitmap *kerfur_face_brow_left(void);
const struct kerfur_bitmap *kerfur_face_brow_right(void);

#endif /* KERFUR_FACES_H_ */
