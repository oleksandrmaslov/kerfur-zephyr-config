#include <stddef.h>

#include <zephyr/sys/util.h>

#include "ui/kerfur_faces.h"

static uint8_t profile_flags(const struct kerfur_face_profile *profile)
{
	uint8_t flags = 0U;

	if (profile->show_brows) {
		flags |= KERFUR_FACE_FLAG_SHOW_BROWS;
	}

	if (profile->show_whiskers) {
		flags |= KERFUR_FACE_FLAG_SHOW_WHISKERS;
	}

	if (profile->sleepy_eyes) {
		flags |= KERFUR_FACE_FLAG_SLEEPY;
	}

	return flags;
}

/* Cat-style base assets ported from the existing kerfur_faces art set. */
static const uint8_t image_eye_left_open_bits[] = {0x00,0x03,0xfc,0x00,0x00,0x00,0x3f,0xff,0x80,0x00,0x00,0xff,0xff,0xe0,0x00,0x01,0xff,0xff,0xf0,0x00,0x03,0xff,0xff,0xf8,0x00,0x07,0xff,0xbf,0xfc,0x00,0x0f,0xfc,0x03,0xfe,0x00,0x1f,0xe0,0x00,0xff,0x00,0x3f,0xc0,0x00,0x7f,0x80,0x3f,0x80,0x00,0x3f,0xc0,0x3f,0x00,0x00,0x1f,0xc0,0x7e,0x00,0x00,0x0f,0xc0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0xfc,0x00,0x00,0x07,0xe0,0xfc,0x00,0x00,0x03,0xf0,0xfc,0x00,0x00,0x03,0xf0,0xfc,0x00,0x00,0x03,0xf0,0xfc,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x0f,0xc0,0x3f,0x00,0x00,0x1f,0xc0,0x3f,0x80,0x00,0x3f,0xc0,0x3f,0xc0,0x00,0x7f,0x80,0x1f,0xe0,0x00,0xff,0x00,0x0f,0xfc,0x03,0xfe,0x00,0x07,0xff,0xbf,0xfc,0x00,0x03,0xff,0xff,0xf8,0x00,0x01,0xff,0xff,0xf0,0x00,0x00,0xff,0xff,0xe0,0x00,0x00,0x3f,0xff,0x80,0x00,0x00,0x03,0xfc,0x00,0x00};

static const uint8_t image_eye_right_open_bits[] = {0x00,0x03,0xfc,0x00,0x00,0x00,0x1f,0xff,0xc0,0x00,0x00,0x7f,0xff,0xf0,0x00,0x00,0xff,0xff,0xf8,0x00,0x01,0xff,0xff,0xfc,0x00,0x03,0xff,0xdf,0xfe,0x00,0x07,0xfc,0x03,0xff,0x00,0x0f,0xf0,0x00,0x7f,0x80,0x1f,0xe0,0x00,0x3f,0xc0,0x3f,0xc0,0x00,0x1f,0xc0,0x3f,0x80,0x00,0x0f,0xc0,0x3f,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x03,0xf0,0xfc,0x00,0x00,0x03,0xf0,0xfc,0x00,0x00,0x03,0xf0,0xfc,0x00,0x00,0x03,0xf0,0x7e,0x00,0x00,0x03,0xf0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x7e,0x00,0x00,0x07,0xe0,0x3f,0x00,0x00,0x07,0xe0,0x3f,0x80,0x00,0x0f,0xc0,0x3f,0xc0,0x00,0x1f,0xc0,0x1f,0xe0,0x00,0x3f,0xc0,0x0f,0xf0,0x00,0x7f,0x80,0x07,0xfc,0x03,0xff,0x00,0x03,0xff,0xdf,0xfe,0x00,0x01,0xff,0xff,0xfc,0x00,0x00,0xff,0xff,0xf8,0x00,0x00,0x7f,0xff,0xf0,0x00,0x00,0x1f,0xff,0xc0,0x00,0x00,0x03,0xfc,0x00,0x00};

static const uint8_t image_eye_blink_bits[] = {
	0xff, 0xff, 0xff, 0xfc, 0xff, 0xff, 0xff, 0xfc, 0xff, 0xff, 0xff, 0xfc,
	0xff, 0xff, 0xff, 0xfc, 0xff, 0xff, 0xff, 0xfc
};

static const uint8_t image_mouth_bits[] = {0x01,0xf0,0x00,0x03,0xf8,0x00,0x01,0xf0,0x00,0x00,0xe0,0x00,0x00,0x00,0x00,0xc0,0xe0,0x60,0xe1,0xf0,0xe0,0xe3,0xb8,0xe0,0x7f,0xbf,0xc0,0x3f,0x1f,0x80};

static const uint8_t image_whisker_left_bits[] = {
	0x7e, 0xff, 0xfe, 0x00, 0x00, 0x3c, 0xff, 0xff
};

static const uint8_t image_whisker_right_bits[] = {
	0x7e, 0xff, 0x7f, 0x00, 0x00, 0x3c, 0xff, 0xff
};

static const uint8_t image_brow_left_bits[] = {
	0x7f, 0x00, 0xff, 0x80, 0xff, 0x00, 0x3e, 0x00
};

static const uint8_t image_brow_right_bits[] = {
	0x7f, 0x00, 0xff, 0x80, 0x7f, 0x80, 0x3e, 0x00
};

static const struct kerfur_bitmap g_eye_left_open = {
	.width = 36,
	.height = 35,
	.data = image_eye_left_open_bits,
};

static const struct kerfur_bitmap g_eye_right_open = {
	.width = 36,
	.height = 35,
	.data = image_eye_right_open_bits,
};

static const struct kerfur_bitmap g_eye_blink = {
	.width = 30,
	.height = 5,
	.data = image_eye_blink_bits,
};

static const struct kerfur_bitmap g_mouth = {
	.width = 19,
	.height = 10,
	.data = image_mouth_bits,
};

static const struct kerfur_bitmap g_whisker_left = {
	.width = 8,
	.height = 8,
	.data = image_whisker_left_bits,
};

static const struct kerfur_bitmap g_whisker_right = {
	.width = 8,
	.height = 8,
	.data = image_whisker_right_bits,
};

static const struct kerfur_bitmap g_brow_left = {
	.width = 9,
	.height = 4,
	.data = image_brow_left_bits,
};

static const struct kerfur_bitmap g_brow_right = {
	.width = 9,
	.height = 4,
	.data = image_brow_right_bits,
};

static const struct kerfur_face_profile g_profiles[KERFUR_FACE_COUNT] = {
	[KERFUR_FACE_CALM] = {
		.visual = KERFUR_FACE_CALM,
		.name = "CALM",
		.eye_dx = 0,
		.eye_dy = 0,
		.mouth_dx = 0,
		.mouth_dy = 0,
		.brow_dy = 0,
		.show_brows = true,
		.show_whiskers = true,
		.sleepy_eyes = false,
	},
	[KERFUR_FACE_CURIOUS] = {
		.visual = KERFUR_FACE_CURIOUS,
		.name = "CURIOUS",
		.eye_dx = 1,
		.eye_dy = -1,
		.mouth_dx = 2,
		.mouth_dy = -1,
		.brow_dy = -1,
		.show_brows = true,
		.show_whiskers = true,
		.sleepy_eyes = false,
	},
	[KERFUR_FACE_CONTENT] = {
		.visual = KERFUR_FACE_CONTENT,
		.name = "CONTENT",
		.eye_dx = 0,
		.eye_dy = 1,
		.mouth_dx = 0,
		.mouth_dy = 2,
		.brow_dy = 1,
		.show_brows = true,
		.show_whiskers = true,
		.sleepy_eyes = false,
	},
	[KERFUR_FACE_PLAYFUL] = {
		.visual = KERFUR_FACE_PLAYFUL,
		.name = "PLAYFUL",
		.eye_dx = 0,
		.eye_dy = -1,
		.mouth_dx = 1,
		.mouth_dy = 1,
		.brow_dy = -2,
		.show_brows = true,
		.show_whiskers = true,
		.sleepy_eyes = false,
	},
	[KERFUR_FACE_SLEEPY] = {
		.visual = KERFUR_FACE_SLEEPY,
		.name = "SLEEPY",
		.eye_dx = 0,
		.eye_dy = 4,
		.mouth_dx = 0,
		.mouth_dy = 3,
		.brow_dy = 2,
		.show_brows = false,
		.show_whiskers = true,
		.sleepy_eyes = true,
	},
	[KERFUR_FACE_ANNOYED] = {
		.visual = KERFUR_FACE_ANNOYED,
		.name = "ANNOYED",
		.eye_dx = 0,
		.eye_dy = -1,
		.mouth_dx = 0,
		.mouth_dy = -1,
		.brow_dy = 2,
		.show_brows = true,
		.show_whiskers = true,
		.sleepy_eyes = false,
	},
	[KERFUR_FACE_COZY] = {
		.visual = KERFUR_FACE_COZY,
		.name = "COZY",
		.eye_dx = 0,
		.eye_dy = 1,
		.mouth_dx = 0,
		.mouth_dy = 2,
		.brow_dy = 1,
		.show_brows = false,
		.show_whiskers = true,
		.sleepy_eyes = false,
	},
	[KERFUR_FACE_ASLEEP] = {
		.visual = KERFUR_FACE_ASLEEP,
		.name = "ASLEEP",
		.eye_dx = 0,
		.eye_dy = 5,
		.mouth_dx = 0,
		.mouth_dy = 4,
		.brow_dy = 2,
		.show_brows = false,
		.show_whiskers = false,
		.sleepy_eyes = true,
	},
};

static const struct kerfur_face_frame g_idle_breathe_fg_frames[] = {
	{ .duration_ms = 700 },
	{ .eye_dy = -1, .brow_dy = -1, .duration_ms = 180 },
	{ .mouth_dy = 1, .duration_ms = 160 },
	{ .duration_ms = 820 },
};

static const struct kerfur_face_frame g_idle_breathe_ambient_frames[] = {
	{ .duration_ms = 1800 },
	{ .mouth_dy = 1, .duration_ms = 300 },
	{ .duration_ms = 2200 },
};

static const struct kerfur_face_frame g_idle_alert_fg_frames[] = {
	{ .duration_ms = 900 },
	{ .eye_dx = -1, .duration_ms = 160 },
	{ .eye_dx = 1, .duration_ms = 160 },
	{ .duration_ms = 900 },
};

static const struct kerfur_face_frame g_idle_alert_ambient_frames[] = {
	{ .duration_ms = 1800 },
	{ .eye_dx = 1, .duration_ms = 220 },
	{ .duration_ms = 1800 },
};

static const struct kerfur_face_frame g_idle_drowse_frames[] = {
	{ .duration_ms = 2200 },
	{ .mouth_dy = 1, .duration_ms = 320 },
	{ .duration_ms = 2400 },
};

static const struct kerfur_face_frame g_idle_playful_fg_frames[] = {
	{ .duration_ms = 480 },
	{ .eye_dy = -1, .duration_ms = 180 },
	{ .eye_dy = 1, .duration_ms = 180 },
	{ .duration_ms = 520 },
};

static const struct kerfur_face_frame g_react_blink_frames[] = {
	{ .eye_mode = KERFUR_FACE_FRAME_EYE_BLINK, .duration_ms = 120 },
	{ .eye_mode = KERFUR_FACE_FRAME_EYE_OPEN, .duration_ms = 100 },
};

static const struct kerfur_face_frame g_react_wake_blink_frames[] = {
	{ .eye_mode = KERFUR_FACE_FRAME_EYE_BLINK, .duration_ms = 140 },
	{ .eye_mode = KERFUR_FACE_FRAME_EYE_OPEN, .eye_dy = -1, .duration_ms = 140 },
	{ .eye_mode = KERFUR_FACE_FRAME_EYE_OPEN, .duration_ms = 220 },
};

static const struct kerfur_face_frame g_react_glance_left_frames[] = {
	{ .eye_dx = -3, .duration_ms = 900 },
};

static const struct kerfur_face_frame g_react_glance_right_frames[] = {
	{ .eye_dx = 3, .duration_ms = 900 },
};

static const struct kerfur_face_frame g_react_look_up_frames[] = {
	{ .eye_dy = -2, .duration_ms = 900 },
};

static const struct kerfur_face_frame g_react_look_down_frames[] = {
	{ .eye_dy = 2, .duration_ms = 900 },
};

static const struct kerfur_face_frame g_react_pet_bow_frames[] = {
	{ .eye_dy = 1, .mouth_dy = 2, .duration_ms = 1200 },
};

static const struct kerfur_face_frame g_react_happy_bounce_frames[] = {
	{ .eye_dy = -1, .duration_ms = 180 },
	{ .eye_dy = 1, .duration_ms = 180 },
	{ .eye_dy = -1, .duration_ms = 180 },
	{ .eye_dy = 1, .duration_ms = 180 },
	{ .duration_ms = 220 },
};

static const struct kerfur_face_animation g_idle_breathe_fg = {
	.name = "idle_breathe_fg",
	.frames = g_idle_breathe_fg_frames,
	.frame_count = ARRAY_SIZE(g_idle_breathe_fg_frames),
	.loop = true,
};

static const struct kerfur_face_animation g_idle_breathe_ambient = {
	.name = "idle_breathe_ambient",
	.frames = g_idle_breathe_ambient_frames,
	.frame_count = ARRAY_SIZE(g_idle_breathe_ambient_frames),
	.loop = true,
};

static const struct kerfur_face_animation g_idle_alert_fg = {
	.name = "idle_alert_fg",
	.frames = g_idle_alert_fg_frames,
	.frame_count = ARRAY_SIZE(g_idle_alert_fg_frames),
	.loop = true,
};

static const struct kerfur_face_animation g_idle_alert_ambient = {
	.name = "idle_alert_ambient",
	.frames = g_idle_alert_ambient_frames,
	.frame_count = ARRAY_SIZE(g_idle_alert_ambient_frames),
	.loop = true,
};

static const struct kerfur_face_animation g_idle_drowse = {
	.name = "idle_drowse",
	.frames = g_idle_drowse_frames,
	.frame_count = ARRAY_SIZE(g_idle_drowse_frames),
	.loop = true,
};

static const struct kerfur_face_animation g_idle_playful_fg = {
	.name = "idle_playful_fg",
	.frames = g_idle_playful_fg_frames,
	.frame_count = ARRAY_SIZE(g_idle_playful_fg_frames),
	.loop = true,
};

static const struct kerfur_face_animation g_react_blink = {
	.name = "react_blink",
	.frames = g_react_blink_frames,
	.frame_count = ARRAY_SIZE(g_react_blink_frames),
	.loop = false,
};

static const struct kerfur_face_animation g_react_wake_blink = {
	.name = "react_wake_blink",
	.frames = g_react_wake_blink_frames,
	.frame_count = ARRAY_SIZE(g_react_wake_blink_frames),
	.loop = false,
};

static const struct kerfur_face_animation g_react_glance_left = {
	.name = "react_glance_left",
	.frames = g_react_glance_left_frames,
	.frame_count = ARRAY_SIZE(g_react_glance_left_frames),
	.loop = false,
};

static const struct kerfur_face_animation g_react_glance_right = {
	.name = "react_glance_right",
	.frames = g_react_glance_right_frames,
	.frame_count = ARRAY_SIZE(g_react_glance_right_frames),
	.loop = false,
};

static const struct kerfur_face_animation g_react_look_up = {
	.name = "react_look_up",
	.frames = g_react_look_up_frames,
	.frame_count = ARRAY_SIZE(g_react_look_up_frames),
	.loop = false,
};

static const struct kerfur_face_animation g_react_look_down = {
	.name = "react_look_down",
	.frames = g_react_look_down_frames,
	.frame_count = ARRAY_SIZE(g_react_look_down_frames),
	.loop = false,
};

static const struct kerfur_face_animation g_react_pet_bow = {
	.name = "react_pet_bow",
	.frames = g_react_pet_bow_frames,
	.frame_count = ARRAY_SIZE(g_react_pet_bow_frames),
	.loop = false,
};

static const struct kerfur_face_animation g_react_happy_bounce = {
	.name = "react_happy_bounce",
	.frames = g_react_happy_bounce_frames,
	.frame_count = ARRAY_SIZE(g_react_happy_bounce_frames),
	.loop = false,
};

static const struct kerfur_face_frame *animation_frame_get(const struct kerfur_face_animation *animation,
							   int64_t elapsed_ms)
{
	uint32_t total_duration = 0U;
	uint32_t cursor = 0U;
	uint8_t i;

	if ((animation == NULL) || (animation->frames == NULL) || (animation->frame_count == 0U)) {
		return NULL;
	}

	for (i = 0U; i < animation->frame_count; i++) {
		total_duration += animation->frames[i].duration_ms;
	}

	if (total_duration == 0U) {
		return &animation->frames[0];
	}

	if (elapsed_ms < 0) {
		elapsed_ms = 0;
	}

	if (animation->loop) {
		elapsed_ms %= total_duration;
	} else if ((uint64_t)elapsed_ms >= total_duration) {
		return &animation->frames[animation->frame_count - 1U];
	}

	for (i = 0U; i < animation->frame_count; i++) {
		cursor += animation->frames[i].duration_ms;
		if ((uint64_t)elapsed_ms < cursor) {
			return &animation->frames[i];
		}
	}

	return &animation->frames[animation->frame_count - 1U];
}

enum kerfur_face_visual kerfur_face_visual_for_expression(enum pet_expression expression)
{
	switch (expression) {
	case PET_EXPR_CALM:
		return KERFUR_FACE_CALM;
	case PET_EXPR_CURIOUS:
	case PET_EXPR_NEEDY:
		return KERFUR_FACE_CURIOUS;
	case PET_EXPR_CONTENT:
	case PET_EXPR_HAPPY:
		return KERFUR_FACE_CONTENT;
	case PET_EXPR_PLAYFUL:
		return KERFUR_FACE_PLAYFUL;
	case PET_EXPR_SLEEPY:
	case PET_EXPR_DRAINED:
	case PET_EXPR_LONELY:
		return KERFUR_FACE_SLEEPY;
	case PET_EXPR_ANNOYED:
	case PET_EXPR_OVERSTIMULATED:
		return KERFUR_FACE_ANNOYED;
	case PET_EXPR_COZY:
		return KERFUR_FACE_COZY;
	case PET_EXPR_ASLEEP:
	default:
		return KERFUR_FACE_ASLEEP;
	}
}

const struct kerfur_face_profile *kerfur_face_profile_get(enum kerfur_face_visual visual)
{
	if ((visual < 0) || (visual >= KERFUR_FACE_COUNT)) {
		return &g_profiles[KERFUR_FACE_CALM];
	}

	return &g_profiles[visual];
}

const char *kerfur_face_visual_str(enum kerfur_face_visual visual)
{
	return kerfur_face_profile_get(visual)->name;
}

void kerfur_face_pose_init(enum kerfur_face_visual visual, struct kerfur_face_pose *pose)
{
	const struct kerfur_face_profile *profile = kerfur_face_profile_get(visual);

	if (pose == NULL) {
		return;
	}

	pose->eye_dx = profile->eye_dx;
	pose->eye_dy = profile->eye_dy;
	pose->mouth_dx = profile->mouth_dx;
	pose->mouth_dy = profile->mouth_dy;
	pose->brow_dy = profile->brow_dy;
	pose->flags = profile_flags(profile);
	pose->eye_mode = profile->sleepy_eyes ? KERFUR_FACE_EYE_BLINK : KERFUR_FACE_EYE_OPEN;
}

bool kerfur_face_pose_apply_animation(struct kerfur_face_pose *pose,
				      const struct kerfur_face_animation *animation,
				      int64_t elapsed_ms)
{
	const struct kerfur_face_frame *frame;

	if (pose == NULL) {
		return false;
	}

	frame = animation_frame_get(animation, elapsed_ms);
	if (frame == NULL) {
		return false;
	}

	pose->eye_dx += frame->eye_dx;
	pose->eye_dy += frame->eye_dy;
	pose->mouth_dx += frame->mouth_dx;
	pose->mouth_dy += frame->mouth_dy;
	pose->brow_dy += frame->brow_dy;
	pose->flags |= frame->set_flags;
	pose->flags &= (uint8_t)~frame->clear_flags;

	switch (frame->eye_mode) {
	case KERFUR_FACE_FRAME_EYE_OPEN:
		pose->eye_mode = KERFUR_FACE_EYE_OPEN;
		break;
	case KERFUR_FACE_FRAME_EYE_BLINK:
		pose->eye_mode = KERFUR_FACE_EYE_BLINK;
		break;
	default:
		break;
	}

	return true;
}

const struct kerfur_face_animation *kerfur_face_idle_animation_get(enum kerfur_face_visual visual,
								   bool ambient)
{
	switch (visual) {
	case KERFUR_FACE_CALM:
	case KERFUR_FACE_CONTENT:
	case KERFUR_FACE_COZY:
		return ambient ? &g_idle_breathe_ambient : &g_idle_breathe_fg;
	case KERFUR_FACE_CURIOUS:
		return ambient ? &g_idle_alert_ambient : &g_idle_alert_fg;
	case KERFUR_FACE_PLAYFUL:
		return ambient ? &g_idle_alert_ambient : &g_idle_playful_fg;
	case KERFUR_FACE_SLEEPY:
	case KERFUR_FACE_ASLEEP:
		return &g_idle_drowse;
	case KERFUR_FACE_ANNOYED:
	default:
		return NULL;
	}
}

const struct kerfur_face_animation *kerfur_face_reaction_animation_get(enum micro_reaction_type reaction)
{
	switch (reaction) {
	case REACTION_BLINK:
		return &g_react_blink;
	case REACTION_WAKE_BLINK:
		return &g_react_wake_blink;
	case REACTION_GLANCE_LEFT:
		return &g_react_glance_left;
	case REACTION_GLANCE_RIGHT:
		return &g_react_glance_right;
	case REACTION_LOOK_UP:
		return &g_react_look_up;
	case REACTION_LOOK_DOWN:
		return &g_react_look_down;
	case REACTION_PET_BOW:
		return &g_react_pet_bow;
	case REACTION_HAPPY_BOUNCE:
		return &g_react_happy_bounce;
	default:
		return NULL;
	}
}

const struct kerfur_bitmap *kerfur_face_eye_open_left(void)
{
	return &g_eye_left_open;
}

const struct kerfur_bitmap *kerfur_face_eye_open_right(void)
{
	return &g_eye_right_open;
}

const struct kerfur_bitmap *kerfur_face_eye_blink(void)
{
	return &g_eye_blink;
}

const struct kerfur_bitmap *kerfur_face_mouth(void)
{
	return &g_mouth;
}

const struct kerfur_bitmap *kerfur_face_whisker_left(void)
{
	return &g_whisker_left;
}

const struct kerfur_bitmap *kerfur_face_whisker_right(void)
{
	return &g_whisker_right;
}

const struct kerfur_bitmap *kerfur_face_brow_left(void)
{
	return &g_brow_left;
}

const struct kerfur_bitmap *kerfur_face_brow_right(void)
{
	return &g_brow_right;
}
