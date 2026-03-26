#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include "ble/ble_manager.h"
#if defined(CONFIG_KERFUR_ENABLE_FACE_SHELL_CMDS)
#include "behavior/behavior_engine.h"
#include "behavior/micro_reaction.h"
#include "core/event_bus.h"
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
	bool in_hand;
	uint8_t pickup_confidence;
	uint8_t in_hand_confidence;
	uint8_t walking_confidence;
	int err;

	if (argc != 5U) {
		shell_error(shell, "usage: kerfur face carry <in_hand> <pickup_conf> <in_hand_conf> <walking_conf>");
		return -EINVAL;
	}

	err = parse_bool_arg(argv[1], &in_hand);
	if (err != 0) {
		shell_error(shell, "invalid in_hand: %s", argv[1]);
		return err;
	}
	err = parse_u8_arg(argv[2], &pickup_confidence);
	if (err != 0) {
		shell_error(shell, "invalid pickup_conf: %s", argv[2]);
		return err;
	}
	err = parse_u8_arg(argv[3], &in_hand_confidence);
	if (err != 0) {
		shell_error(shell, "invalid in_hand_conf: %s", argv[3]);
		return err;
	}
	err = parse_u8_arg(argv[4], &walking_confidence);
	if (err != 0) {
		shell_error(shell, "invalid walking_conf: %s", argv[4]);
		return err;
	}

	err = app_event_publish_carry_state(in_hand, pickup_confidence, in_hand_confidence,
					    walking_confidence);
	if (err != 0) {
		shell_error(shell, "carry publish failed (%d)", err);
		return err;
	}

	shell_print(shell, "carry state queued in_hand=%d pickup=%u in_hand_conf=%u walk_conf=%u",
		    in_hand ? 1 : 0, pickup_confidence, in_hand_confidence, walking_confidence);
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_ble,
	SHELL_CMD(status, NULL, "Show BLE connection state", cmd_ble_status),
	SHELL_CMD(adv_restart, NULL, "Restart BLE advertising", cmd_ble_adv_restart),
	SHELL_CMD(disconnect, NULL, "Disconnect current BLE link", cmd_ble_disconnect),
	SHELL_CMD(unpair_all, NULL, "Remove all stored BLE bonds", cmd_ble_unpair_all),
	SHELL_CMD(gb_status, NULL, "Show Gadgetbridge companion state", cmd_ble_gb_status),
	SHELL_CMD(gb_test, NULL, "Send companion test notification [category]", cmd_ble_gb_test),
	SHELL_SUBCMD_SET_END
);

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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur_face,
	SHELL_CMD(dump, NULL, "Request a face state debug dump", cmd_face_dump),
	SHELL_CMD(look, NULL, "Queue look target update", cmd_face_look),
	SHELL_CMD(carry, NULL, "Queue carry state update", cmd_face_carry),
	SHELL_CMD(battery, NULL, "Queue battery percent update", cmd_face_battery),
	SHELL_CMD(expr, &sub_kerfur_face_expr, "Expression override commands", NULL),
	SHELL_CMD(react, &sub_kerfur_face_react, "Reaction trigger commands", NULL),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur,
	SHELL_CMD(ble, &sub_kerfur_ble, "BLE maintenance commands", NULL),
	SHELL_CMD(face, &sub_kerfur_face, "Face state debug commands", NULL),
	SHELL_SUBCMD_SET_END
);
#else
SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur,
	SHELL_CMD(ble, &sub_kerfur_ble, "BLE maintenance commands", NULL),
	SHELL_SUBCMD_SET_END
);
#endif

SHELL_CMD_REGISTER(kerfur, &sub_kerfur, "Kerfur shell root commands", NULL);
#endif
