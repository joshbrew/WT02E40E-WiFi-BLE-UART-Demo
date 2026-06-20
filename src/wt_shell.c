#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "wt_ble.h"
#include "wt_common.h"
#include "wt_config.h"
#include "wt_radio.h"
#include "wt_wifi.h"

static char tx_payload_buf[WT_TX_PAYLOAD_MAX];

static int shell_cmd_wt_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "WT02E40E radio state");
	shell_print(sh, "  Wi-Fi requested: %s", wt_onoff_txt(wt_wifi_is_requested()));
	shell_print(sh, "  Wi-Fi associated: %s", wt_onoff_txt(wt_wifi_is_associated()));
	shell_print(sh, "  Wi-Fi IPv4 bound: %s", wt_onoff_txt(wt_wifi_has_ipv4()));
	shell_print(sh, "  Wi-Fi UDP command server: %s on port %u",
		    wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port());
	shell_print(sh, "  Stored Wi-Fi credentials:");
	(void)wt_wifi_credentials_print_list(sh);
	shell_print(sh, "  BLE requested: %s", wt_onoff_txt(wt_ble_is_requested()));
	shell_print(sh, "  BLE initialized: %s", wt_onoff_txt(wt_ble_is_ready()));
	shell_print(sh, "  BLE advertising: %s", wt_onoff_txt(wt_ble_is_advertising()));
	shell_print(sh, "  BLE connected: %s", wt_onoff_txt(wt_ble_is_connected()));
	shell_print(sh, "  BLE TX notify: %s", wt_onoff_txt(wt_ble_tx_notify_is_enabled()));
	shell_print(sh, "  BLE status notify: %s", wt_onoff_txt(wt_ble_status_notify_is_enabled()));
	shell_print(sh, "  BLE command response notify: %s", wt_onoff_txt(wt_ble_cmd_response_notify_is_enabled()));

	return 0;
}

static int shell_cmd_wt_mode(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc < 2) {
		shell_print(sh, "usage: wt mode idle|ble|wifi|both");
		return -EINVAL;
	}

	ret = wt_radio_mode_apply(argv[1]);
	if (ret) {
		shell_error(sh, "mode command failed: %d", ret);
		return ret;
	}

	shell_print(sh, "mode set to %s", argv[1]);
	return 0;
}

static int shell_cmd_wt_wifi(const struct shell *sh, size_t argc, char **argv)
{
	int ret = 0;

	if (argc < 2) {
		shell_print(sh, "usage: wt wifi on|off|status|reconnect|cmd|cred");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_wifi_service_set(true);
	} else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		ret = wt_wifi_service_set(false);
	} else if (!strcmp(argv[1], "status")) {
		ret = wt_wifi_status_log();
	} else if (!strcmp(argv[1], "reconnect")) {
		ret = wt_wifi_reconnect_if_requested();
		if (!wt_wifi_is_requested()) {
			ret = wt_wifi_service_set(true);
		}
	} else if (!strcmp(argv[1], "cmd") || !strcmp(argv[1], "command") || !strcmp(argv[1], "commands")) {
		if (argc < 3 || !strcmp(argv[2], "status")) {
			shell_print(sh, "Wi-Fi UDP command server: %s on port %u",
				    wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port());
			return 0;
		}
		if (!strcmp(argv[2], "on") || !strcmp(argv[2], "start")) {
			ret = wt_wifi_cmd_service_set(true);
		} else if (!strcmp(argv[2], "off") || !strcmp(argv[2], "stop")) {
			ret = wt_wifi_cmd_service_set(false);
		} else {
			shell_print(sh, "usage: wt wifi cmd on|off|status");
			return -EINVAL;
		}
	} else if (!strcmp(argv[1], "cred") || !strcmp(argv[1], "creds")) {
		return wt_wifi_credentials_shell(sh, argc, argv);
	} else {
		shell_print(sh, "usage: wt wifi on|off|status|reconnect|cmd|cred");
		return -EINVAL;
	}

	if (ret) {
		shell_error(sh, "wifi command failed: %d", ret);
		return ret;
	}

	return 0;
}

static int shell_cmd_wt_ble(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc < 2) {
		shell_print(sh, "usage: wt ble on|off|status");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_ble_service_start();
	} else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		ret = wt_ble_service_stop();
	} else if (!strcmp(argv[1], "status")) {
		return shell_cmd_wt_status(sh, argc, argv);
	} else {
		shell_print(sh, "usage: wt ble on|off|status");
		return -EINVAL;
	}

	if (ret) {
		shell_error(sh, "ble command failed: %d", ret);
		return ret;
	}

	return 0;
}

static int shell_cmd_wt_tx(const struct shell *sh, size_t argc, char **argv)
{
	int payload_len;
	int ret;

	if (argc < 3) {
		shell_print(sh, "usage: wt tx ble <message>");
		shell_print(sh, "       wt tx wifi <ipv4> <port> <message>");
		shell_print(sh, "       wt tx both <ipv4> <port> <message>");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "ble") || !strcmp(argv[1], "bt")) {
		payload_len = wt_build_payload_from_argv(2, argc, argv,
					tx_payload_buf, sizeof(tx_payload_buf));
		if (payload_len < 0) {
			shell_error(sh, "payload build failed: %d", payload_len);
			return payload_len;
		}

		ret = wt_ble_transmit_payload((const uint8_t *)tx_payload_buf, payload_len);
		if (ret) {
			shell_error(sh, "BLE transmit failed: %d", ret);
			if (ret == -EACCES) {
				shell_print(sh, "enable notifications on the WT TX characteristic first");
			}
			return ret;
		}

		shell_print(sh, "BLE TX: %d bytes", payload_len);
		return 0;
	}

	if (!strcmp(argv[1], "wifi") || !strcmp(argv[1], "udp")) {
		if (argc < 5) {
			shell_print(sh, "usage: wt tx wifi <ipv4> <port> <message>");
			return -EINVAL;
		}

		payload_len = wt_build_payload_from_argv(4, argc, argv,
					tx_payload_buf, sizeof(tx_payload_buf));
		if (payload_len < 0) {
			shell_error(sh, "payload build failed: %d", payload_len);
			return payload_len;
		}

		ret = wt_wifi_udp_transmit_payload(argv[2], argv[3],
						(const uint8_t *)tx_payload_buf, payload_len);
		if (ret) {
			shell_error(sh, "Wi-Fi UDP transmit failed: %d", ret);
			return ret;
		}

		shell_print(sh, "Wi-Fi UDP TX: %d bytes to %s:%s",
			    payload_len, argv[2], argv[3]);
		return 0;
	}

	if (!strcmp(argv[1], "both")) {
		if (argc < 5) {
			shell_print(sh, "usage: wt tx both <ipv4> <port> <message>");
			return -EINVAL;
		}

		payload_len = wt_build_payload_from_argv(4, argc, argv,
					tx_payload_buf, sizeof(tx_payload_buf));
		if (payload_len < 0) {
			shell_error(sh, "payload build failed: %d", payload_len);
			return payload_len;
		}

		ret = wt_wifi_udp_transmit_payload(argv[2], argv[3],
						(const uint8_t *)tx_payload_buf, payload_len);
		if (ret) {
			shell_error(sh, "Wi-Fi UDP transmit failed: %d", ret);
			return ret;
		}

		ret = wt_ble_transmit_payload((const uint8_t *)tx_payload_buf, payload_len);
		if (ret) {
			shell_error(sh, "BLE transmit failed after Wi-Fi TX: %d", ret);
			return ret;
		}

		shell_print(sh, "both TX: %d bytes", payload_len);
		return 0;
	}

	shell_print(sh, "usage: wt tx ble|wifi|both ...");
	return -EINVAL;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wt_subcmds,
	SHELL_CMD_ARG(status, NULL, "Show WT02E40E radio state", shell_cmd_wt_status, 1, 0),
	SHELL_CMD_ARG(mode, NULL, "Set radio mode: idle, ble, wifi, both", shell_cmd_wt_mode, 2, 0),
	SHELL_CMD_ARG(wifi, NULL, "Control Wi-Fi: on, off, status, reconnect, cmd, cred", shell_cmd_wt_wifi, 2, WT_TX_PAYLOAD_MAX),
	SHELL_CMD_ARG(ble, NULL, "Control BLE: on, off, status", shell_cmd_wt_ble, 2, 0),
	SHELL_CMD_ARG(tx, NULL, "Transmit: ble <msg>, wifi <ip> <port> <msg>, both <ip> <port> <msg>", shell_cmd_wt_tx, 3, WT_TX_PAYLOAD_MAX),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wt, &wt_subcmds, "WT02E40E radio commands", NULL);
