#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/shell/shell.h>

#include "ble/ble_manager.h"

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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_kerfur,
	SHELL_CMD(ble, &sub_kerfur_ble, "BLE maintenance commands", NULL),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(kerfur, &sub_kerfur, "Kerfur shell root commands", NULL);
#endif
