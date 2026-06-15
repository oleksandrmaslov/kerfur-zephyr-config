/*
 * Host-side render-path test for the Kerfur face runtime.
 *
 * Compiles the REAL src/ui/face_runtime.c and the generated recipe/asset
 * tables against tiny Zephyr shims (see shim/) and drives every
 * expression and every transition through face_runtime_step(), asserting
 * the produced render plan is coherent. This is the off-target safety net
 * for the face pipeline — the counterpart to tools/appraisal_calibrate.py
 * for the engine.
 *
 * Build + run:  bash tests/face_host/run.sh
 *
 * What it proves:
 *  - every expression renders its own recipe with in-range assets;
 *  - eye openness and pupil offsets stay within sane bounds every frame;
 *  - a pupil swap ALWAYS completes (no deadlock) for every from->to
 *    transition — this is the regression guard for the OVERSTIMULATED
 *    spiral that used to wait forever for a blink that never came;
 *  - every micro-reaction overlaid on a resting face yields a valid plan.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "behavior/pet_state.h"
#include "ui/face_runtime.h"
#include "ui/generated/kerfur_face_assets.h"
#include "ui/generated/kerfur_face_recipes.h"

#define FRAME_MS 33
#define SETTLE_FRAMES 200 /* ~6.6 s — past every transition/settle window */

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                       \
	do {                                                                  \
		g_checks++;                                                   \
		if (!(cond)) {                                                \
			g_failures++;                                        \
			printf("[FAIL] ");                                   \
			printf(__VA_ARGS__);                                 \
			printf("\n");                                        \
		}                                                            \
	} while (0)

static void init_state(struct pet_state *st)
{
	memset(st, 0, sizeof(*st));
	st->mood = 50;
	st->current_display_state = DISPLAY_FOREGROUND;
	st->current_expression = PET_EXPR_CALM;
	st->current_reaction = REACTION_NONE;
	st->battery_percent = 80;
	st->battery_percent_known = true;
}

static bool asset_in_range(enum kerfur_face_asset_id id)
{
	return (int)id >= 0 && (int)id < KERFUR_FACE_ASSET_COUNT;
}

/* Validate one render plan. `tag` identifies the failing scenario. */
static void validate_plan(const struct face_runtime_plan *plan, const char *tag)
{
	uint8_t i;

	CHECK(plan != NULL, "%s: null plan", tag);
	if (plan == NULL) {
		return;
	}

	CHECK((int)plan->recipe_id >= 0 && plan->recipe_id < KERFUR_FACE_RECIPE_COUNT,
	      "%s: recipe_id %d out of range", tag, plan->recipe_id);
	CHECK((int)plan->reaction_id >= 0 &&
		      plan->reaction_id < KERFUR_FACE_REACTION_REACTION_COUNT,
	      "%s: reaction_id %d out of range", tag, plan->reaction_id);

	CHECK(asset_in_range(plan->left_eye_white) && asset_in_range(plan->right_eye_white),
	      "%s: eye-white asset out of range (%d,%d)", tag,
	      plan->left_eye_white, plan->right_eye_white);
	CHECK(asset_in_range(plan->left_eyeball) && asset_in_range(plan->right_eyeball),
	      "%s: eyeball asset out of range (%d,%d)", tag,
	      plan->left_eyeball, plan->right_eyeball);
	CHECK(asset_in_range(plan->left_brow) && asset_in_range(plan->right_brow),
	      "%s: brow asset out of range", tag);
	CHECK(asset_in_range(plan->mouth) && asset_in_range(plan->whiskers),
	      "%s: mouth/whisker asset out of range", tag);
	CHECK(asset_in_range(plan->indicator_asset) && asset_in_range(plan->overlay_asset),
	      "%s: indicator/overlay asset out of range", tag);

	CHECK(plan->indicator_count <= KERFUR_FACE_MAX_INDICATORS,
	      "%s: indicator_count %u too big", tag, plan->indicator_count);
	CHECK(plan->effect_count <= KERFUR_FACE_MAX_EFFECTS,
	      "%s: effect_count %u too big", tag, plan->effect_count);
	for (i = 0; i < plan->effect_count; i++) {
		CHECK(asset_in_range(plan->effects[i].asset_id),
		      "%s: effect[%u] asset out of range", tag, i);
	}

	CHECK(plan->eye_openness <= 100, "%s: eye_openness %u > 100", tag,
	      plan->eye_openness);
	CHECK(plan->left_eye_openness <= 100 && plan->right_eye_openness <= 100,
	      "%s: per-eye openness > 100", tag);

	/* Pupil offsets must stay near the eye, not fly off the panel. */
	CHECK(abs(plan->left_pupil_offset_x) <= 48 && abs(plan->left_pupil_offset_y) <= 48,
	      "%s: left pupil offset wild (%d,%d)", tag,
	      plan->left_pupil_offset_x, plan->left_pupil_offset_y);
	CHECK(abs(plan->right_pupil_offset_x) <= 48 && abs(plan->right_pupil_offset_y) <= 48,
	      "%s: right pupil offset wild (%d,%d)", tag,
	      plan->right_pupil_offset_x, plan->right_pupil_offset_y);
}

static const struct face_runtime_plan *run_frames(struct face_runtime_state *rt,
						  struct pet_state *st, int64_t *now,
						  int frames, const char *tag)
{
	const struct face_runtime_plan *plan = NULL;

	for (int i = 0; i < frames; i++) {
		*now += FRAME_MS;
		plan = face_runtime_step(rt, st, *now, false, false);
		validate_plan(plan, tag);
	}
	return plan;
}

/* Test 1: every expression renders its own recipe. */
static void test_expression_identity(void)
{
	for (int e = PET_EXPR_CALM; e <= PET_EXPR_ASLEEP; e++) {
		struct face_runtime_state rt;
		struct pet_state st;
		int64_t now = 1000;
		char tag[64];
		const struct face_runtime_plan *plan;

		snprintf(tag, sizeof(tag), "identity:%s",
			 kerfur_face_recipe_name((enum kerfur_face_recipe_id)e));
		init_state(&st);
		st.current_expression = (enum pet_expression)e;
		face_runtime_init(&rt);

		plan = run_frames(&rt, &st, &now, SETTLE_FRAMES, tag);
		CHECK(plan->recipe_id == (enum kerfur_face_recipe_id)e,
		      "%s: rendered recipe %s instead", tag,
		      kerfur_face_recipe_name(plan->recipe_id));
	}
}

/* Test 2 (the important one): a pupil swap always completes. For every
 * from->to expression pair, after settling the rendered eyeball must equal
 * the destination recipe's eyeball — never get stuck mid-swap. */
static void test_pupil_swap_completes(void)
{
	for (int from = PET_EXPR_CALM; from <= PET_EXPR_ASLEEP; from++) {
		for (int to = PET_EXPR_CALM; to <= PET_EXPR_ASLEEP; to++) {
			struct face_runtime_state rt;
			struct pet_state st;
			int64_t now = 1000;
			const struct kerfur_face_recipe *dst =
				kerfur_face_recipe_get((enum kerfur_face_recipe_id)to);
			char tag[80];

			snprintf(tag, sizeof(tag), "swap:%s->%s",
				 kerfur_face_recipe_name((enum kerfur_face_recipe_id)from),
				 kerfur_face_recipe_name((enum kerfur_face_recipe_id)to));

			init_state(&st);
			st.current_expression = (enum pet_expression)from;
			face_runtime_init(&rt);
			run_frames(&rt, &st, &now, SETTLE_FRAMES, tag);

			st.current_expression = (enum pet_expression)to;
			run_frames(&rt, &st, &now, SETTLE_FRAMES, tag);

			CHECK(rt.plan.left_eyeball == dst->left_eyeball,
			      "%s: left eyeball stuck (got %s, want %s)", tag,
			      kerfur_face_asset_name(rt.plan.left_eyeball),
			      kerfur_face_asset_name(dst->left_eyeball));
			CHECK(rt.plan.right_eyeball == dst->right_eyeball,
			      "%s: right eyeball stuck (got %s, want %s)", tag,
			      kerfur_face_asset_name(rt.plan.right_eyeball),
			      kerfur_face_asset_name(dst->right_eyeball));
		}
	}
}

/* Test 3: every micro-reaction overlaid on a resting face stays valid and
 * then clears back to a coherent resting plan. */
static void test_reactions_valid(void)
{
	for (int r = REACTION_NONE; r < REACTION_COUNT; r++) {
		struct face_runtime_state rt;
		struct pet_state st;
		int64_t now = 1000;
		char tag[80];

		snprintf(tag, sizeof(tag), "react:%s",
			 kerfur_face_reaction_name((enum kerfur_face_reaction_id)r));
		init_state(&st);
		face_runtime_init(&rt);
		run_frames(&rt, &st, &now, 30, tag);

		st.current_reaction = (enum micro_reaction_type)r;
		run_frames(&rt, &st, &now, 60, tag);

		st.current_reaction = REACTION_NONE;
		run_frames(&rt, &st, &now, 60, tag);
	}
}

int main(void)
{
	srand(1);

	test_expression_identity();
	test_pupil_swap_completes();
	test_reactions_valid();

	printf("\nface_runtime host test: %d checks, %d failure(s)\n",
	       g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
