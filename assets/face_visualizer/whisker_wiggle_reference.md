# Whisker Wiggle Reference

This note collects the whisker wiggle logic that was previously implemented in the face animation code.

The current working tree still has the renderer hook and the `whisker_dy` pose channel, but the actual reaction animation table is not present in the checked-in source anymore. The frame values below were extracted from historical commit `24814e2`.

## Renderer Hook

Current renderer usage in `src/ui/ui_renderer.c`:

```c
if ((pose.flags & KERFUR_FACE_FLAG_SHOW_WHISKERS) != 0U) {
	draw_bitmap(assets->whisker_left,
		    assets->whisker_left_x + g_ui.ambient_shift_x,
		    assets->whisker_y + pose.whisker_dy + g_ui.ambient_shift_y, opa);
	draw_bitmap(assets->whisker_right,
		    assets->whisker_right_x + g_ui.ambient_shift_x,
		    assets->whisker_y + pose.whisker_dy + g_ui.ambient_shift_y, opa);
}
```

So the wiggle is just a vertical offset applied to both whiskers:

```c
final_whisker_y = base_whisker_y + pose.whisker_dy + ambient_shift_y;
```

## Pose Channel

Current `include/ui/kerfur_faces.h` keeps the whisker offset in both the pose and animation frame structs:

```c
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
```

## Historical Wiggle Frames

Historical reaction table from commit `24814e2`:

```c
static const struct kerfur_face_frame g_react_happy_pet_frames[] = {
	{ .whisker_dy = 2, .duration_ms = 120 },
	{ .whisker_dy = -2, .duration_ms = 120 },
	{ .whisker_dy = 2, .duration_ms = 120 },
	{ .whisker_dy = -1, .duration_ms = 170 },
	{ .whisker_dy = 1, .duration_ms = 170 },
	{ .duration_ms = 260 },
};

static const struct kerfur_face_animation g_react_happy_pet = {
	.name = "react_happy_pet",
	.frames = g_react_happy_pet_frames,
	.frame_count = ARRAY_SIZE(g_react_happy_pet_frames),
	.loop = false,
};
```

That produces this Y-offset sequence over time:

1. `+2` for `120 ms`
2. `-2` for `120 ms`
3. `+2` for `120 ms`
4. `-1` for `170 ms`
5. `+1` for `170 ms`
6. `0` for `260 ms`

## Historical Apply Logic

The same historical implementation initialized and applied `whisker_dy` like this:

```c
void kerfur_face_pose_init(enum kerfur_face_visual visual, struct kerfur_face_pose *pose)
{
	const struct kerfur_face_profile *profile = kerfur_face_profile_get(visual);

	pose->eye_dx = profile->eye_dx;
	pose->eye_dy = profile->eye_dy;
	pose->mouth_dx = profile->mouth_dx;
	pose->mouth_dy = profile->mouth_dy;
	pose->mouth_open_dy = 0;
	pose->whisker_dy = 0;
	pose->brow_dy = profile->brow_dy;
	pose->flags = profile_flags(profile);
	pose->eye_mode = profile->sleepy_eyes ? KERFUR_FACE_EYE_BLINK : KERFUR_FACE_EYE_OPEN;
}

bool kerfur_face_pose_apply_animation(struct kerfur_face_pose *pose,
				      const struct kerfur_face_animation *animation,
				      int64_t elapsed_ms)
{
	const struct kerfur_face_frame *frame = animation_frame_get(animation, elapsed_ms);

	if (frame == NULL) {
		return false;
	}

	pose->eye_dx += frame->eye_dx;
	pose->eye_dy += frame->eye_dy;
	pose->mouth_dx += frame->mouth_dx;
	pose->mouth_dy += frame->mouth_dy;
	pose->mouth_open_dy += frame->mouth_open_dy;
	pose->whisker_dy += frame->whisker_dy;
	pose->brow_dy += frame->brow_dy;
	return true;
}
```

## Minimal Reimplementation

If you only want the whisker wiggle back, this is the core idea:

```c
static const struct kerfur_face_frame g_react_pet_whisker_frames[] = {
	{ .whisker_dy = 2, .duration_ms = 120 },
	{ .whisker_dy = -2, .duration_ms = 120 },
	{ .whisker_dy = 2, .duration_ms = 120 },
	{ .whisker_dy = -1, .duration_ms = 170 },
	{ .whisker_dy = 1, .duration_ms = 170 },
	{ .duration_ms = 260 },
};
```

Then:

1. Add `whisker_dy` to your pose/frame structs if missing.
2. Reset `pose->whisker_dy = 0` in pose init.
3. Add `pose->whisker_dy += frame->whisker_dy` when applying a frame.
4. Render whiskers at `base_y + pose.whisker_dy`.
5. Attach that animation to the pet reaction you want.
