#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "ble/ble_manager.h"
#include "behavior/behavior_engine.h"
#include "core/app_event.h"
#include "core/event_bus.h"
#if defined(CONFIG_KERFUR_ENABLE_FACE_SHELL_CMDS)
#include "behavior/micro_reaction.h"
#include "drivers/motion_classifier.h"
#endif

#if defined(CONFIG_KERFUR_ENABLE_SHELL_CMDS)
static int cmd_ble_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "connected: %s", ble_manager_is_connected() ? "yes" : "no");
	return 0;
}

static int cmd_ble_adv_restart(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = ble_manager_restart_advertising();
	if (err != 0) {
		shell_error(shell, "adv_restart failed (%d)", err);
		return err;
	}

	shell_print(shell, "advertising restarted");
	return 0;
}

static int cmd_ble_disconnect(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = ble_manager_disconnect_current();
	if (err == -ENOTCONN) {
		shell_warn(shell, "no active BLE connection");
		return 0;
	}
	if (err != 0) {
		shell_error(shell, "disconnect failed (%d)", err);
		return err;
	}

	shell_print(shell, "disconnect requested");
	return 0;
}

static int cmd_ble_unpair_all(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = ble_manager_unpair_all();
	if (err == -ENOENT) {
		shell_warn(shell, "no stored bonds");
		return 0;
	}
	if (err != 0) {
		shell_error(shell, "unpair_all failed (%d)", err);
		return err;
	}

	shell_print(shell, "all bonds removed");
	return 0;
}

static int cmd_ble_gb_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "connected: %s", ble_manager_is_connected() ? "yes" : "no");
	shell_print(shell, "companion_notify: %s",
		    ble_manager_is_companion_subscribed() ? "enabled" : "disabled");
	return 0;
}

static int parse_u8_arg(const char *arg, uint8_t *value)
{
	char *endptr;
	long parsed;

	if ((arg == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	parsed = strtol(arg, &endptr, 0);
	if ((*arg == '\0') || (*endptr != '\0')) {
		return -EINVAL;
	}
	if ((parsed < 0) || (parsed > UINT8_MAX)) {
		return -ERANGE;
	}

	*value = (uint8_t)parsed;
	return 0;
}

static int parse_i32_arg(const char *arg, int32_t *value)
{
	char *endptr;
	long parsed;

	if ((arg == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	parsed = strtol(arg, &endptr, 0);
	if ((*arg == '\0') || (*endptr != '\0')) {
		return -EINVAL;
	}
	if ((parsed < INT32_MIN) || (parsed > INT32_MAX)) {
		return -ERANGE;
	}

	*value = (int32_t)parsed;
	return 0;
}

#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
static int parse_u32_arg(const char *arg, uint32_t *value)
{
	char *endptr;
	unsigned long parsed;

	if ((arg == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	parsed = strtoul(arg, &endptr, 0);
	if ((*arg == '\0') || (*endptr != '\0')) {
		return -EINVAL;
	}
	if (parsed > UINT32_MAX) {
		return -ERANGE;
	}

	*value = (uint32_t)parsed;
	return 0;
}

static int parse_i8_arg(const char *arg, int8_t *value)
{
	char *endptr;
	long parsed;

	if ((arg == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	parsed = strtol(arg, &endptr, 0);
	if ((*arg == '\0') || (*endptr != '\0')) {
		return -EINVAL;
	}
	if ((parsed < INT8_MIN) || (parsed > INT8_MAX)) {
		return -ERANGE;
	}

	*value = (int8_t)parsed;
	return 0;
}
#endif /* CONFIG_KERFUR_ENABLE_NEARBY */

#if defined(CONFIG_KERFUR_ENABLE_FACE_SHELL_CMDS)
static int parse_i16_arg(const char *arg, int16_t *value)
{
	char *endptr;
	long parsed;

	if ((arg == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	parsed = strtol(arg, &endptr, 0);
	if ((*arg == '\0') || (*endptr != '\0')) {
		return -EINVAL;
	}
	if ((parsed < -32768L) || (parsed > 32767L)) {
		return -ERANGE;
	}

	*value = (int16_t)parsed;
	return 0;
}

static int parse_bool_arg(const char *arg, bool *value)
{
	if ((arg == NULL) || (value == NULL)) {
		return -EINVAL;
	}

	if ((strcmp(arg, "1") == 0) || (strcmp(arg, "true") == 0) || (strcmp(arg, "on") == 0) ||
	    (strcmp(arg, "yes") == 0)) {
		*value = true;
		return 0;
	}
	if ((strcmp(arg, "0") == 0) || (strcmp(arg, "false") == 0) || (strcmp(arg, "off") == 0) ||
	    (strcmp(arg, "no") == 0)) {
		*value = false;
		return 0;
	}

	return -EINVAL;
}

static char ascii_tolower_char(char value)
{
	if ((value >= 'A') && (value <= 'Z')) {
		return (char)(value - 'A' + 'a');
	}

	return value;
}

static bool token_equals_ignore_case(const char *lhs, const char *rhs)
{
	if ((lhs == NULL) || (rhs == NULL)) {
		return false;
	}

	while ((*lhs != '\0') && (*rhs != '\0')) {
		if (ascii_tolower_char(*lhs) != ascii_tolower_char(*rhs)) {
			return false;
		}
		lhs++;
		rhs++;
	}

	return (*lhs == '\0') && (*rhs == '\0');
}

static bool token_has_prefix_ignore_case(const char *value, const char *prefix)
{
	if ((value == NULL) || (prefix == NULL)) {
		return false;
	}

	while (*prefix != '\0') {
		if (*value == '\0') {
			return false;
		}
		if (ascii_tolower_char(*value) != ascii_tolower_char(*prefix)) {
			return false;
		}
		value++;
		prefix++;
	}

	return true;
}

static int parse_expression_arg(const char *arg, enum pet_expression *expression)
{
	static const char *const prefix = "PET_EXPR_";
	char *endptr;
	long parsed;
	int expr;

	if ((arg == NULL) || (expression == NULL)) {
		return -EINVAL;
	}

	parsed = strtol(arg, &endptr, 0);
	if ((*arg != '\0') && (*endptr == '\0')) {
		if ((parsed < PET_EXPR_CALM) || (parsed > PET_EXPR_ASLEEP)) {
			return -ERANGE;
		}

		*expression = (enum pet_expression)parsed;
		return 0;
	}

	for (expr = PET_EXPR_CALM; expr <= PET_EXPR_ASLEEP; expr++) {
		const char *name = pet_expression_str((enum pet_expression)expr);

		if (token_equals_ignore_case(arg, name) ||
		    (token_has_prefix_ignore_case(arg, prefix) &&
		     token_equals_ignore_case(arg + strlen(prefix), name))) {
			*expression = (enum pet_expression)expr;
			return 0;
		}
	}

	return -EINVAL;
}

static int parse_reaction_arg(const char *arg, enum micro_reaction_type *reaction)
{
	static const char *const prefix = "REACTION_";
	char *endptr;
	long parsed;
	int reaction_id;

	if ((arg == NULL) || (reaction == NULL)) {
		return -EINVAL;
	}

	parsed = strtol(arg, &endptr, 0);
	if ((*arg != '\0') && (*endptr == '\0')) {
		if ((parsed < REACTION_NONE) || (parsed >= REACTION_COUNT)) {
			return -ERANGE;
		}

		*reaction = (enum micro_reaction_type)parsed;
		return 0;
	}

	for (reaction_id = REACTION_NONE; reaction_id < REACTION_COUNT; reaction_id++) {
		const char *name = micro_reaction_str((enum micro_reaction_type)reaction_id);

		if (token_equals_ignore_case(arg, name) ||
		    (token_has_prefix_ignore_case(arg, prefix) &&
		     token_equals_ignore_case(arg + strlen(prefix), name))) {
			*reaction = (enum micro_reaction_type)reaction_id;
			return 0;
		}
	}

	return -EINVAL;
}

static int cmd_face_dump(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = app_event_publish(APP_EVENT_FACE_DEBUG_DUMP, 0);
	if (err != 0) {
		shell_error(shell, "face dump publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "face dump requested");
	return 0;
}

static int cmd_face_look(const struct shell *shell, size_t argc, char **argv)
{
	int16_t x;
	int16_t y;
	uint8_t confidence = 100U;
	int err;

	if ((argc < 3U) || (argc > 4U)) {
		shell_error(shell, "usage: kerfur face look <x> <y> [confidence]");
		return -EINVAL;
	}

	err = parse_i16_arg(argv[1], &x);
	if (err != 0) {
		shell_error(shell, "invalid x: %s", argv[1]);
		return err;
	}
	err = parse_i16_arg(argv[2], &y);
	if (err != 0) {
		shell_error(shell, "invalid y: %s", argv[2]);
		return err;
	}
	if ((x < -100) || (x > 100) || (y < -100) || (y > 100)) {
		shell_error(shell, "look target must be in range -100..100");
		return -ERANGE;
	}
	if (argc == 4U) {
		err = parse_u8_arg(argv[3], &confidence);
		if (err != 0) {
			shell_error(shell, "invalid confidence: %s", argv[3]);
			return err;
		}
	}

	err = app_event_publish_look_target(x, y, confidence);
	if (err != 0) {
		shell_error(shell, "look publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "look target queued x=%d y=%d conf=%u", x, y, confidence);
	return 0;
}

static int cmd_face_carry(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_carry_state carry = {0};
	bool in_hand;
	int err;

	if ((argc < 5U) || (argc > 6U)) {
		shell_error(shell,
			    "usage: kerfur face carry <in_hand> <pickup_conf> "
			    "<in_hand_conf> <walking_conf> [ctx 0-4]");
		return -EINVAL;
	}

	err = parse_bool_arg(argv[1], &in_hand);
	if (err != 0) {
		shell_error(shell, "invalid in_hand: %s", argv[1]);
		return err;
	}
	err = parse_u8_arg(argv[2], &carry.pickup_confidence);
	if (err != 0) {
		shell_error(shell, "invalid pickup_conf: %s", argv[2]);
		return err;
	}
	err = parse_u8_arg(argv[3], &carry.in_hand_confidence);
	if (err != 0) {
		shell_error(shell, "invalid in_hand_conf: %s", argv[3]);
		return err;
	}
	err = parse_u8_arg(argv[4], &carry.walking_confidence);
	if (err != 0) {
		shell_error(shell, "invalid walking_conf: %s", argv[4]);
		return err;
	}
	carry.in_hand = in_hand;
	if (argc == 6U) {
		/* 0 unknown / 1 surface / 2 in_hand / 3 worn / 4 transition */
		err = parse_u8_arg(argv[5], &carry.carry_context);
		if ((err != 0) || (carry.carry_context > (uint8_t)PET_CARRY_TRANSITION)) {
			shell_error(shell, "invalid ctx: %s (0..4)", argv[5]);
			return -EINVAL;
		}
		carry.carry_context_confidence = 80U;
	}

	err = app_event_publish_carry_with_timestamp(APP_EVENT_CARRY_STATE_UPDATE,
						     &carry, k_uptime_get());
	if ((err == 0) && (argc == 6U)) {
		err = app_event_publish_carry_with_timestamp(
			APP_EVENT_CARRY_CONTEXT_CHANGED, &carry, k_uptime_get());
	}
	if (err != 0) {
		shell_error(shell, "carry publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "carry state queued in_hand=%d pickup=%u in_hand_conf=%u walk_conf=%u ctx=%u",
		    in_hand ? 1 : 0, carry.pickup_confidence, carry.in_hand_confidence,
		    carry.walking_confidence, carry.carry_context);
	return 0;
}

static int cmd_face_battery(const struct shell *shell, size_t argc, char **argv)
{
	int8_t percent = -1;
	bool known = false;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur face battery <percent|unknown>");
		return -EINVAL;
	}

	if (strcmp(argv[1], "unknown") == 0) {
		known = false;
	} else {
		uint8_t value;

		err = parse_u8_arg(argv[1], &value);
		if (err != 0) {
			shell_error(shell, "invalid battery percent: %s", argv[1]);
			return err;
		}
		percent = (int8_t)value;
		known = true;
	}

	if (known && ((percent < 0) || (percent > 100))) {
		shell_error(shell, "battery percent must be 0..100");
		return -ERANGE;
	}

	err = app_event_publish_battery_percent(percent, known);
	if (err != 0) {
		shell_error(shell, "battery publish failed (%d)", err);
		return err;
	}

	if (known) {
		shell_print(shell, "battery state queued percent=%d", percent);
	} else {
		shell_print(shell, "battery state queued as unknown");
	}
	return 0;
}

static int cmd_face_motion_walk_start(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t confidence = 80U;
	int err;

	if ((argc < 1U) || (argc > 2U)) {
		shell_error(shell, "usage: kerfur face motion walk_start [confidence]");
		return -EINVAL;
	}

	if (argc == 2U) {
		err = parse_u8_arg(argv[1], &confidence);
		if (err != 0) {
			shell_error(shell, "invalid confidence: %s", argv[1]);
			return err;
		}
	}

	err = app_event_publish_with_timestamp(APP_EVENT_WALKING_START, confidence, k_uptime_get());
	if (err != 0) {
		shell_error(shell, "walking_start publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "walking start queued conf=%u", confidence);
	return 0;
}

static int cmd_face_motion_walk_stop(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t confidence = 40U;
	int err;

	if ((argc < 1U) || (argc > 2U)) {
		shell_error(shell, "usage: kerfur face motion walk_stop [confidence]");
		return -EINVAL;
	}

	if (argc == 2U) {
		err = parse_u8_arg(argv[1], &confidence);
		if (err != 0) {
			shell_error(shell, "invalid confidence: %s", argv[1]);
			return err;
		}
	}

	err = app_event_publish_with_timestamp(APP_EVENT_WALKING_STOP, confidence, k_uptime_get());
	if (err != 0) {
		shell_error(shell, "walking_stop publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "walking stop queued conf=%u", confidence);
	return 0;
}

static int cmd_face_motion_step_batch(const struct shell *shell, size_t argc, char **argv)
{
	int32_t steps;
	uint8_t confidence = 80U;
	int err;

	if ((argc < 2U) || (argc > 3U)) {
		shell_error(shell, "usage: kerfur face motion step_batch <steps> [walking_conf]");
		return -EINVAL;
	}

	err = parse_i32_arg(argv[1], &steps);
	if (err != 0) {
		shell_error(shell, "invalid steps: %s", argv[1]);
		return err;
	}
	if (steps < 0) {
		shell_error(shell, "steps must be >= 0");
		return -ERANGE;
	}
	if (argc == 3U) {
		err = parse_u8_arg(argv[2], &confidence);
		if (err != 0) {
			shell_error(shell, "invalid walking_conf: %s", argv[2]);
			return err;
		}
	}

	err = app_event_publish_step_batch((int32_t)steps, 0U, confidence, false);
	if (err != 0) {
		shell_error(shell, "step_batch publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "step batch queued steps=%d walk_conf=%u", steps, confidence);
	return 0;
}

static int cmd_face_motion_dynamic_pupils(const struct shell *shell, size_t argc, char **argv)
{
	bool enabled;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur face motion dynamic_pupils <on|off>");
		return -EINVAL;
	}

	err = parse_bool_arg(argv[1], &enabled);
	if (err != 0) {
		shell_error(shell, "invalid toggle: %s", argv[1]);
		return err;
	}

	err = app_event_publish(APP_EVENT_FACE_SET_DYNAMIC_PUPILS_DEBUG, enabled ? 0 : 1);
	if (err != 0) {
		shell_error(shell, "dynamic pupils publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "dynamic pupils debug override %s", enabled ? "enabled" : "disabled");
	return 0;
}

static int cmd_face_motion_conf_log(const struct shell *shell, size_t argc, char **argv)
{
	bool enabled;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur face motion conf_log <on|off>");
		return -EINVAL;
	}

	err = parse_bool_arg(argv[1], &enabled);
	if (err != 0) {
		shell_error(shell, "invalid toggle: %s", argv[1]);
		return err;
	}

	motion_classifier_set_debug_logging(enabled);
	shell_print(shell, "motion confidence logging %s", enabled ? "enabled" : "disabled");
	return 0;
}

static int cmd_face_expr_list(const struct shell *shell, size_t argc, char **argv)
{
	int expr;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (expr = PET_EXPR_CALM; expr <= PET_EXPR_ASLEEP; expr++) {
		shell_print(shell, "%2d  %s", expr,
			    pet_expression_str((enum pet_expression)expr));
	}

	return 0;
}

static int cmd_face_expr_set(const struct shell *shell, size_t argc, char **argv)
{
	enum pet_expression expression;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur face expr set <name|id>");
		return -EINVAL;
	}

	err = parse_expression_arg(argv[1], &expression);
	if (err != 0) {
		shell_error(shell, "invalid expression: %s", argv[1]);
		return err;
	}

	err = app_event_publish(APP_EVENT_FACE_FORCE_EXPRESSION, expression);
	if (err != 0) {
		shell_error(shell, "expression publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "forced expression queued: %s", pet_expression_str(expression));
	return 0;
}

static int cmd_face_expr_clear(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = app_event_publish(APP_EVENT_FACE_CLEAR_FORCED_EXPRESSION, 0);
	if (err != 0) {
		shell_error(shell, "expression clear publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "forced expression clear queued");
	return 0;
}

static int cmd_face_react_list(const struct shell *shell, size_t argc, char **argv)
{
	int reaction;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (reaction = REACTION_NONE; reaction < REACTION_COUNT; reaction++) {
		shell_print(shell, "%2d  %s%s", reaction,
			    micro_reaction_str((enum micro_reaction_type)reaction),
			    (reaction == REACTION_NONE) ? " (not triggerable)" : "");
	}

	return 0;
}

static int cmd_face_react_trigger(const struct shell *shell, size_t argc, char **argv)
{
	enum micro_reaction_type reaction;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur face react trigger <name|id>");
		return -EINVAL;
	}

	err = parse_reaction_arg(argv[1], &reaction);
	if (err != 0) {
		shell_error(shell, "invalid reaction: %s", argv[1]);
		return err;
	}
	if (reaction == REACTION_NONE) {
		shell_error(shell, "REACTION_NONE is not triggerable");
		return -EINVAL;
	}

	err = app_event_publish(APP_EVENT_FACE_TRIGGER_REACTION, reaction);
	if (err != 0) {
		shell_error(shell, "reaction publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "reaction queued: %s", micro_reaction_str(reaction));
	return 0;
}
#endif /* CONFIG_KERFUR_ENABLE_FACE_SHELL_CMDS */

#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
static int cmd_nearby_inject_seen(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur nearby inject seen <ephemeral_id>");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}

	peer.ephemeral_id = id;
	peer.rssi = -80;

	err = app_event_publish_peer(APP_EVENT_PEER_SEEN, &peer);
	if (err != 0) {
		shell_error(shell, "PEER_SEEN publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "PEER_SEEN queued id=0x%08x", id);
	return 0;
}

static int cmd_nearby_inject_checking(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur nearby inject checking <ephemeral_id>");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}

	peer.ephemeral_id = id;
	peer.rssi = -75;

	err = app_event_publish_peer(APP_EVENT_PEER_CHECKING, &peer);
	if (err != 0) {
		shell_error(shell, "PEER_CHECKING publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "PEER_CHECKING queued id=0x%08x", id);
	return 0;
}

/* Parse an optional peer expression id (enum pet_expression) used to
 * test emotional contagion. Defaults keep the peer looking like a
 * plain idle/calm Kerfur. */
static int parse_peer_expression(const struct shell *shell, const char *arg,
				 struct app_event_peer *peer)
{
	uint8_t expr;
	int err;

	err = parse_u8_arg(arg, &expr);
	if ((err != 0) || (expr > (uint8_t)PET_EXPR_ASLEEP)) {
		shell_error(shell, "invalid peer expression id: %s (0..%d)",
			    arg, PET_EXPR_ASLEEP);
		return -EINVAL;
	}

	peer->expression_summary = expr;
	return 0;
}

static int cmd_nearby_inject_near(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int8_t rssi = -65;
	int err;

	if ((argc < 2U) || (argc > 4U)) {
		shell_error(shell,
			    "usage: kerfur nearby inject near <ephemeral_id> [rssi] [expr]");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}
	if (argc >= 3U) {
		err = parse_i8_arg(argv[2], &rssi);
		if (err != 0) {
			shell_error(shell, "invalid rssi: %s", argv[2]);
			return err;
		}
	}

	peer.ephemeral_id = id;
	peer.rssi = rssi;
	peer.mode_summary = (uint8_t)PET_MODE_IDLE;
	peer.expression_summary = (uint8_t)PET_EXPR_CALM;
	if (argc == 4U) {
		err = parse_peer_expression(shell, argv[3], &peer);
		if (err != 0) {
			return err;
		}
	}

	err = app_event_publish_peer(APP_EVENT_PEER_NEAR, &peer);
	if (err != 0) {
		shell_error(shell, "PEER_NEAR publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "PEER_NEAR queued id=0x%08x rssi=%d expr=%u",
		    id, rssi, peer.expression_summary);
	return 0;
}

static int cmd_nearby_inject_friend(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int err;

	if ((argc < 2U) || (argc > 3U)) {
		shell_error(shell, "usage: kerfur nearby inject friend <ephemeral_id> [expr]");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}

	peer.ephemeral_id = id;
	peer.encounter_id = id ^ 0xA5A5A5A5U;
	peer.is_friend = true;
	peer.rssi = -55;
	peer.mode_summary = (uint8_t)PET_MODE_IDLE;
	peer.expression_summary = (uint8_t)PET_EXPR_CALM;
	if (argc == 3U) {
		err = parse_peer_expression(shell, argv[2], &peer);
		if (err != 0) {
			return err;
		}
	}

	err = app_event_publish_peer(APP_EVENT_ENCOUNTER_START, &peer);
	if (err != 0) {
		shell_error(shell, "ENCOUNTER_START publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "ENCOUNTER_START (friend) queued id=0x%08x expr=%u",
		    id, peer.expression_summary);
	return 0;
}

static int cmd_nearby_inject_unknown(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int err;

	if ((argc < 2U) || (argc > 3U)) {
		shell_error(shell, "usage: kerfur nearby inject unknown <ephemeral_id> [expr]");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}

	peer.ephemeral_id = id;
	peer.encounter_id = id ^ 0x5A5A5A5AU;
	peer.is_friend = false;
	peer.rssi = -60;
	peer.mode_summary = (uint8_t)PET_MODE_IDLE;
	peer.expression_summary = (uint8_t)PET_EXPR_CALM;
	if (argc == 3U) {
		err = parse_peer_expression(shell, argv[2], &peer);
		if (err != 0) {
			return err;
		}
	}

	err = app_event_publish_peer(APP_EVENT_ENCOUNTER_START, &peer);
	if (err != 0) {
		shell_error(shell, "ENCOUNTER_START publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "ENCOUNTER_START (unknown) queued id=0x%08x expr=%u",
		    id, peer.expression_summary);
	return 0;
}

static int cmd_nearby_inject_play_invite(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur nearby inject play_invite <ephemeral_id>");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}

	peer.ephemeral_id = id;
	peer.rssi = -55;

	err = app_event_publish_peer(APP_EVENT_PEER_PLAY_INVITE, &peer);
	if (err != 0) {
		shell_error(shell, "PEER_PLAY_INVITE publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "PEER_PLAY_INVITE queued id=0x%08x", id);
	return 0;
}

static int cmd_nearby_inject_play_ack(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur nearby inject play_ack <ephemeral_id>");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}

	peer.ephemeral_id = id;
	peer.rssi = -55;

	err = app_event_publish_peer(APP_EVENT_PEER_PLAY_ACK, &peer);
	if (err != 0) {
		shell_error(shell, "PEER_PLAY_ACK publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "PEER_PLAY_ACK queued id=0x%08x", id);
	return 0;
}

static int cmd_nearby_inject_lost(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur nearby inject lost <ephemeral_id>");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}

	peer.ephemeral_id = id;

	err = app_event_publish_peer(APP_EVENT_PEER_LOST, &peer);
	if (err != 0) {
		shell_error(shell, "PEER_LOST publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "PEER_LOST queued id=0x%08x", id);
	return 0;
}

static int cmd_nearby_end(const struct shell *shell, size_t argc, char **argv)
{
	struct app_event_peer peer = {0};
	uint32_t id;
	int32_t duration_s = 0;
	int err;

	if ((argc < 2U) || (argc > 3U)) {
		shell_error(shell, "usage: kerfur nearby end <ephemeral_id> [duration_s]");
		return -EINVAL;
	}

	err = parse_u32_arg(argv[1], &id);
	if (err != 0) {
		shell_error(shell, "invalid ephemeral_id: %s", argv[1]);
		return err;
	}
	if (argc == 3U) {
		err = parse_i32_arg(argv[2], &duration_s);
		if (err != 0) {
			shell_error(shell, "invalid duration_s: %s", argv[2]);
			return err;
		}
	}

	peer.ephemeral_id = id;
	peer.encounter_id = id ^ 0xA5A5A5A5U;
	peer.duration_s = duration_s;

	err = app_event_publish_peer(APP_EVENT_ENCOUNTER_END, &peer);
	if (err != 0) {
		shell_error(shell, "ENCOUNTER_END publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "ENCOUNTER_END queued id=0x%08x duration=%ds", id, duration_s);
	return 0;
}
#endif /* CONFIG_KERFUR_ENABLE_NEARBY */

static int cmd_ble_gb_test(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t category = 1U;
	int err;

	if (argc > 2U) {
		shell_error(shell, "usage: kerfur ble gb_test [category]");
		return -EINVAL;
	}

	if (argc == 2U) {
		err = parse_u8_arg(argv[1], &category);
		if (err != 0) {
			shell_error(shell, "invalid category: %s", argv[1]);
			return err;
		}
	}

	err = ble_manager_companion_send_test_notification(category);
	if (err == -ENOTSUP) {
		shell_warn(shell, "companion channel disabled in Kconfig");
		return 0;
	}
	if (err == -ENOTCONN) {
		shell_warn(shell, "no active BLE connection");
		return 0;
	}
	if (err == -EACCES) {
		shell_warn(shell, "companion notify is not enabled by peer");
		return 0;
	}
	if (err != 0) {
		shell_error(shell, "gb_test failed (%d)", err);
		return err;
	}

	shell_print(shell, "companion test notification sent (category=%u)", category);
	return 0;
}

static int cmd_emotion_dump(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* The behavior engine answers on the bus with a full face +
	 * emotion log dump ("emotion print"). */
	err = app_event_publish(APP_EVENT_FACE_DEBUG_DUMP, 0);
	if (err != 0) {
		shell_error(shell, "emotion dump publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "emotion dump queued (see log output)");
	return 0;
}

static int cmd_emotion_personality(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t id;
	int err;

	if (argc != 2U) {
		shell_error(shell,
			    "usage: kerfur emotion personality <0..%d> "
			    "(0=balanced 1=curious 2=shy 3=playful 4=calm)",
			    PET_PERSONALITY_COUNT - 1);
		return -EINVAL;
	}

	err = parse_u8_arg(argv[1], &id);
	if ((err != 0) || (id >= (uint8_t)PET_PERSONALITY_COUNT)) {
		shell_error(shell, "invalid personality id: %s", argv[1]);
		return -EINVAL;
	}

	err = app_event_publish(APP_EVENT_PERSONALITY_SET, (int32_t)id);
	if (err != 0) {
		shell_error(shell, "personality publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "personality -> %s", pet_personality_str((enum pet_personality)id));
	return 0;
}

static int cmd_emotion_worn(const struct shell *shell, size_t argc, char **argv)
{
	int32_t style;
	int err;

	if (argc != 2U) {
		shell_error(shell, "usage: kerfur emotion worn <quiet|expressive>");
		return -EINVAL;
	}

	if (strcmp(argv[1], "quiet") == 0) {
		style = 0;
	} else if (strcmp(argv[1], "expressive") == 0) {
		style = 1;
	} else {
		shell_error(shell, "invalid style: %s (quiet|expressive)", argv[1]);
		return -EINVAL;
	}

	err = app_event_publish(APP_EVENT_WORN_STYLE_SET, style);
	if (err != 0) {
		shell_error(shell, "worn style publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "worn style -> %s (persisted)", argv[1]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_emotion,
	SHELL_CMD(dump, NULL, "Print emotional state to log", cmd_emotion_dump),
	SHELL_CMD(personality, NULL, "Set personality <id>", cmd_emotion_personality),
	SHELL_CMD(worn, NULL, "Set worn style <quiet|expressive>", cmd_emotion_worn),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_ble,
	SHELL_CMD(status, NULL, "Show BLE connection state", cmd_ble_status),
	SHELL_CMD(adv_restart, NULL, "Restart BLE advertising", cmd_ble_adv_restart),
	SHELL_CMD(disconnect, NULL, "Disconnect current BLE link", cmd_ble_disconnect),
	SHELL_CMD(unpair_all, NULL, "Remove all stored BLE bonds", cmd_ble_unpair_all),
	SHELL_CMD(gb_status, NULL, "Show Gadgetbridge companion state", cmd_ble_gb_status),
	SHELL_CMD(gb_test, NULL, "Send companion test notification [category]", cmd_ble_gb_test),
	SHELL_SUBCMD_SET_END
);

#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_nearby_inject,
	SHELL_CMD(seen, NULL, "Inject PEER_SEEN <id>", cmd_nearby_inject_seen),
	SHELL_CMD(checking, NULL, "Inject PEER_CHECKING <id>", cmd_nearby_inject_checking),
	SHELL_CMD(near, NULL, "Inject PEER_NEAR <id> [rssi] [expr]", cmd_nearby_inject_near),
	SHELL_CMD(friend, NULL, "Inject ENCOUNTER_START as friend <id> [expr]", cmd_nearby_inject_friend),
	SHELL_CMD(unknown, NULL, "Inject ENCOUNTER_START as unknown <id> [expr]", cmd_nearby_inject_unknown),
	SHELL_CMD(play_invite, NULL, "Inject PEER_PLAY_INVITE <id>", cmd_nearby_inject_play_invite),
	SHELL_CMD(play_ack, NULL, "Inject PEER_PLAY_ACK <id>", cmd_nearby_inject_play_ack),
	SHELL_CMD(lost, NULL, "Inject PEER_LOST <id>", cmd_nearby_inject_lost),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_nearby,
	SHELL_CMD(inject, &sub_kerfur_nearby_inject, "Inject nearby/encounter events", NULL),
	SHELL_CMD(end, NULL, "Force ENCOUNTER_END <id> [duration_s]", cmd_nearby_end),
	SHELL_SUBCMD_SET_END
);
#endif /* CONFIG_KERFUR_ENABLE_NEARBY */

#if defined(CONFIG_KERFUR_ENABLE_FACE_SHELL_CMDS)
SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_face_expr,
	SHELL_CMD(list, NULL, "List all expressions", cmd_face_expr_list),
	SHELL_CMD(set, NULL, "Force an expression by name or id", cmd_face_expr_set),
	SHELL_CMD(clear, NULL, "Return expression selection to automatic behavior", cmd_face_expr_clear),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_face_react,
	SHELL_CMD(list, NULL, "List all reactions", cmd_face_react_list),
	SHELL_CMD(trigger, NULL, "Trigger a reaction by name or id", cmd_face_react_trigger),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_face_motion,
	SHELL_CMD(walk_start, NULL, "Force walking start", cmd_face_motion_walk_start),
	SHELL_CMD(walk_stop, NULL, "Force walking stop", cmd_face_motion_walk_stop),
	SHELL_CMD(step_batch, NULL, "Force a batched step update", cmd_face_motion_step_batch),
	SHELL_CMD(dynamic_pupils, NULL, "Toggle dynamic pupil debug override", cmd_face_motion_dynamic_pupils),
	SHELL_CMD(conf_log, NULL, "Toggle motion confidence logging", cmd_face_motion_conf_log),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_face,
	SHELL_CMD(dump, NULL, "Request a face state debug dump", cmd_face_dump),
	SHELL_CMD(look, NULL, "Queue look target update", cmd_face_look),
	SHELL_CMD(carry, NULL, "Queue carry state update", cmd_face_carry),
	SHELL_CMD(battery, NULL, "Queue battery percent update", cmd_face_battery),
	SHELL_CMD(motion, &sub_kerfur_face_motion, "Motion debug commands", NULL),
	SHELL_CMD(expr, &sub_kerfur_face_expr, "Expression override commands", NULL),
	SHELL_CMD(react, &sub_kerfur_face_react, "Reaction trigger commands", NULL),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur,
	SHELL_CMD(ble, &sub_kerfur_ble, "BLE maintenance commands", NULL),
	SHELL_CMD(emotion, &sub_kerfur_emotion, "Emotional state debug commands", NULL),
	SHELL_CMD(face, &sub_kerfur_face, "Face state debug commands", NULL),
#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
	SHELL_CMD(nearby, &sub_kerfur_nearby, "Kerfur-to-Kerfur nearby debug commands", NULL),
#endif
	SHELL_SUBCMD_SET_END
);
#else
SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur,
	SHELL_CMD(ble, &sub_kerfur_ble, "BLE maintenance commands", NULL),
	SHELL_CMD(emotion, &sub_kerfur_emotion, "Emotional state debug commands", NULL),
#if defined(CONFIG_KERFUR_ENABLE_NEARBY)
	SHELL_CMD(nearby, &sub_kerfur_nearby, "Kerfur-to-Kerfur nearby debug commands", NULL),
#endif
	SHELL_SUBCMD_SET_END
);
#endif

SHELL_CMD_REGISTER(kerfur, &sub_kerfur, "Kerfur shell root commands", NULL);
#endif
