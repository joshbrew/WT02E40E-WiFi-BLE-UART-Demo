#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "wt_app.h"
#include "wt_ble.h"
#include "wt_common.h"
#include "wt_config.h"
#include "wt_radio.h"
#include "wt_wifi.h"

static char tx_payload_buf[WT_TX_PAYLOAD_MAX];
static char shell_rsp_buf[WT_BLE_CMD_RSP_TEXT_MAX];

static int shell_cmd_wt_status(const struct shell *sh, size_t argc, char **argv)
{
	bool json = argc >= 2 && !strcmp(argv[1], "json");

	if (json) {
		(void)wt_app_config_format(shell_rsp_buf, sizeof(shell_rsp_buf), true);
		shell_print(sh, "%s", shell_rsp_buf);
		return 0;
	}

	shell_print(sh, "WT02E40E radio state");
	shell_print(sh, "  Name: %s", wt_ble_name_get());
	shell_print(sh, "  Wi-Fi requested: %s", wt_onoff_txt(wt_wifi_is_requested()));
	shell_print(sh, "  Wi-Fi associated: %s", wt_onoff_txt(wt_wifi_is_associated()));
	shell_print(sh, "  Wi-Fi IPv4 bound: %s", wt_onoff_txt(wt_wifi_has_ipv4()));
	shell_print(sh, "  Wi-Fi UDP command server: %s on port %u",
		    wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port());
	shell_print(sh, "  Wi-Fi discovery beacon: %s on port %u",
		    wt_onoff_txt(wt_wifi_discovery_is_enabled()), WT_WIFI_DISCOVERY_UDP_PORT);
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

static int shell_cmd_wt_id(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	(void)wt_app_id_format(shell_rsp_buf, sizeof(shell_rsp_buf));
	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	(void)wt_app_version_format(shell_rsp_buf, sizeof(shell_rsp_buf));
	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_config(const struct shell *sh, size_t argc, char **argv)
{
	bool json = argc >= 2 && !strcmp(argv[1], "json");

	if (argc >= 2 && !strcmp(argv[1], "save")) {
		int ret = wt_app_config_save();
		if (ret) {
			shell_error(sh, "config save failed: %d", ret);
			return ret;
		}
		shell_print(sh, "config saved");
		return 0;
	}

	if (argc >= 2 && !strcmp(argv[1], "reset")) {
		int ret = wt_app_config_reset();
		if (ret) {
			shell_error(sh, "config reset failed: %d", ret);
			return ret;
		}
		shell_print(sh, "config reset");
		return 0;
	}

	(void)wt_app_config_format(shell_rsp_buf, sizeof(shell_rsp_buf), json);
	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_name(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc < 2 || !strcmp(argv[1], "get")) {
		shell_print(sh, "BLE name: %s", wt_ble_name_get());
		return 0;
	}

	if (!strcmp(argv[1], "set")) {
		if (argc < 3) {
			shell_print(sh, "usage: wt name set <ble-name>");
			return -EINVAL;
		}
		ret = wt_ble_name_set(argv[2]);
	} else {
		ret = wt_ble_name_set(argv[1]);
	}

	if (ret) {
		shell_error(sh, "BLE name set failed: %d", ret);
		return ret;
	}

	shell_print(sh, "BLE name: %s", wt_ble_name_get());
	return 0;
}


static int shell_cmd_wt_boot(const struct shell *sh, size_t argc, char **argv)
{
	int ret = wt_app_boot_command(argv, argc, shell_rsp_buf, sizeof(shell_rsp_buf));

	if (ret < 0) {
		shell_error(sh, "%s", shell_rsp_buf);
		return ret;
	}

	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_fw(const struct shell *sh, size_t argc, char **argv)
{
	int ret = wt_app_fw_command(argv, argc, shell_rsp_buf, sizeof(shell_rsp_buf));

	if (ret < 0) {
		shell_error(sh, "%s", shell_rsp_buf);
		return ret;
	}

	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_reboot(const struct shell *sh, size_t argc, char **argv)
{
	char *fw_argv[4];
	size_t fw_argc = 2;
	int ret;

	fw_argv[0] = "fw";
	fw_argv[1] = "reboot";
	if (argc >= 2) {
		fw_argv[2] = argv[1];
		fw_argc = 3;
	}
	if (argc >= 3) {
		fw_argv[3] = argv[2];
		fw_argc = 4;
	}

	ret = wt_app_fw_command(fw_argv, fw_argc, shell_rsp_buf, sizeof(shell_rsp_buf));
	if (ret < 0) {
		shell_error(sh, "%s", shell_rsp_buf);
		return ret;
	}
	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_bridge(const struct shell *sh, size_t argc, char **argv)
{
	int ret = wt_app_bridge_command(argv, argc, shell_rsp_buf, sizeof(shell_rsp_buf));

	if (ret < 0) {
		shell_error(sh, "%s", shell_rsp_buf);
		return ret;
	}

	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_led(const struct shell *sh, size_t argc, char **argv)
{
	int ret = wt_app_led_command(argv, argc, shell_rsp_buf, sizeof(shell_rsp_buf));

	if (ret < 0) {
		shell_error(sh, "%s", shell_rsp_buf);
		return ret;
	}

	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_ping(const struct shell *sh, size_t argc, char **argv)
{
	int ret = wt_app_ping_execute(argv, argc, shell_rsp_buf, sizeof(shell_rsp_buf));

	if (ret < 0) {
		shell_error(sh, "%s", shell_rsp_buf);
		return ret;
	}

	shell_print(sh, "%s", shell_rsp_buf);
	return 0;
}

static int shell_cmd_wt_mode(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t delay_ms = 0;
	int ret;

	if (argc < 2) {
		shell_print(sh, "usage: wt mode idle|ble|wifi|both [delay]");
		return -EINVAL;
	}

	if (argc >= 3) {
		ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
		if (ret) {
			shell_error(sh, "bad delay: %s", argv[2]);
			return ret;
		}
	}

	ret = wt_app_delayed_radio_apply(argv[1], delay_ms);
	if (ret) {
		shell_error(sh, "mode command failed: %d", ret);
		return ret;
	}

	if (delay_ms) {
		shell_print(sh, "mode %s scheduled in %u ms", argv[1], delay_ms);
	} else {
		shell_print(sh, "mode set to %s", argv[1]);
	}
	return 0;
}

static int shell_cmd_wt_discovery(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc < 2 || !strcmp(argv[1], "status")) {
		shell_print(sh, "Wi-Fi discovery beacon: %s on port %u interval %ums",
			    wt_onoff_txt(wt_wifi_discovery_is_enabled()),
			    WT_WIFI_DISCOVERY_UDP_PORT, WT_WIFI_DISCOVERY_INTERVAL_MS);
		return 0;
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_wifi_discovery_service_set(true);
	} else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		ret = wt_wifi_discovery_service_set(false);
	} else {
		shell_print(sh, "usage: wt discovery on|off|status");
		return -EINVAL;
	}

	if (ret) {
		shell_error(sh, "discovery command failed: %d", ret);
		return ret;
	}

	shell_print(sh, "discovery %s", argv[1]);
	return 0;
}

static int shell_cmd_wt_wifi(const struct shell *sh, size_t argc, char **argv)
{
	int ret = 0;

	if (argc < 2) {
		shell_print(sh, "usage: wt wifi on|off|status|reconnect|scan|cmd|discovery|cred");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_wifi_service_set(true);
	} else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		uint32_t delay_ms = 0;
		if (argc >= 3) {
			ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
			if (ret) {
				shell_error(sh, "bad delay: %s", argv[2]);
				return ret;
			}
		}
		ret = wt_app_delayed_radio_apply("wifi_off", delay_ms);
		if (!ret && delay_ms) {
			shell_print(sh, "wifi off scheduled in %u ms", delay_ms);
			return 0;
		}
	} else if (!strcmp(argv[1], "status")) {
		if (argc >= 3 && !strcmp(argv[2], "json")) {
			(void)wt_app_wifi_status_format(shell_rsp_buf, sizeof(shell_rsp_buf), true);
			shell_print(sh, "%s", shell_rsp_buf);
			return 0;
		}
		(void)wt_app_wifi_status_format(shell_rsp_buf, sizeof(shell_rsp_buf), false);
		shell_print(sh, "%s", shell_rsp_buf);
		ret = wt_wifi_status_log();
	} else if (!strcmp(argv[1], "reconnect")) {
		ret = wt_wifi_reconnect_if_requested();
		if (!wt_wifi_is_requested()) {
			ret = wt_wifi_service_set(true);
		}
	} else if (!strcmp(argv[1], "scan") || !strcmp(argv[1], "apscan") || !strcmp(argv[1], "networks")) {
		ret = wt_wifi_scan_command((char **)argv, argc, shell_rsp_buf, sizeof(shell_rsp_buf));
		if (ret < 0) {
			shell_error(sh, "%s", shell_rsp_buf);
			return ret;
		}
		shell_print(sh, "%s", shell_rsp_buf);
		return 0;
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
		} else if (!strcmp(argv[2], "port")) {
			uint16_t port;
			if (argc < 4) {
				shell_print(sh, "Wi-Fi UDP command port: %u", wt_wifi_cmd_port());
				return 0;
			}
			ret = wt_parse_udp_port(argv[3], &port);
			if (ret) {
				shell_error(sh, "invalid Wi-Fi command port: %s", argv[3]);
				return ret;
			}
			ret = wt_wifi_cmd_port_set(port);
			if (!ret) {
				shell_print(sh, "Wi-Fi UDP command port: %u", wt_wifi_cmd_port());
			}
		} else {
			shell_print(sh, "usage: wt wifi cmd on|off|status|port [port]");
			return -EINVAL;
		}
	} else if (!strcmp(argv[1], "discovery") || !strcmp(argv[1], "discover")) {
		if (argc < 3 || !strcmp(argv[2], "status")) {
			return shell_cmd_wt_discovery(sh, 1, argv);
		}
		char *disc_argv[] = { "discovery", argv[2] };
		return shell_cmd_wt_discovery(sh, 2, disc_argv);
	} else if (!strcmp(argv[1], "cred") || !strcmp(argv[1], "creds")) {
		return wt_wifi_credentials_shell(sh, argc, argv);
	} else {
		shell_print(sh, "usage: wt wifi on|off|status|reconnect|scan|cmd|discovery|cred");
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
		shell_print(sh, "usage: wt ble on|off|status|name [get|set <name>]");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "name")) {
		if (argc < 3 || !strcmp(argv[2], "get")) {
			shell_print(sh, "BLE name: %s", wt_ble_name_get());
			return 0;
		}
		if (!strcmp(argv[2], "set")) {
			if (argc < 4) {
				shell_print(sh, "usage: wt ble name set <name>");
				return -EINVAL;
			}
			ret = wt_ble_name_set(argv[3]);
		} else {
			ret = wt_ble_name_set(argv[2]);
		}
		if (ret) {
			shell_error(sh, "BLE name set failed: %d", ret);
			return ret;
		}
		return 0;
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_ble_service_start();
	} else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		uint32_t delay_ms = 0;
		if (argc >= 3) {
			ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
			if (ret) {
				shell_error(sh, "bad delay: %s", argv[2]);
				return ret;
			}
		}
		ret = wt_app_delayed_radio_apply("ble_off", delay_ms);
		if (!ret && delay_ms) {
			shell_print(sh, "ble off scheduled in %u ms", delay_ms);
			return 0;
		}
	} else if (!strcmp(argv[1], "status")) {
		(void)wt_app_ble_status_format(shell_rsp_buf, sizeof(shell_rsp_buf), argc >= 3 && !strcmp(argv[2], "json"));
		shell_print(sh, "%s", shell_rsp_buf);
		return 0;
	} else {
		shell_print(sh, "usage: wt ble on|off|status|name [get|set <name>]");
		return -EINVAL;
	}

	if (ret) {
		shell_error(sh, "ble command failed: %d", ret);
		return ret;
	}

	return 0;
}

static int shell_tx_uart_payload(size_t argc, char **argv, size_t payload_arg)
{
	int payload_len;

	payload_len = wt_build_payload_from_argv(payload_arg, argc, argv,
				tx_payload_buf, sizeof(tx_payload_buf));
	if (payload_len < 0) {
		return payload_len;
	}

	printk("UART TX: ");
	for (int i = 0; i < payload_len; i++) {
		printk("%c", tx_payload_buf[i]);
	}
	printk("\r\n");

	return payload_len;
}

static int shell_cmd_wt_tx(const struct shell *sh, size_t argc, char **argv)
{
	int payload_len;
	int ret;

	if (argc < 3) {
		shell_print(sh, "usage: wt tx ble <message>");
		shell_print(sh, "       wt tx uart <message>");
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

	if (!strcmp(argv[1], "uart") || !strcmp(argv[1], "serial")) {
		payload_len = shell_tx_uart_payload(argc, argv, 2);
		if (payload_len < 0) {
			shell_error(sh, "UART payload build failed: %d", payload_len);
			return payload_len;
		}
		shell_print(sh, "UART TX: %d bytes", payload_len);
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

	if (!strcmp(argv[1], "both") || !strcmp(argv[1], "all")) {
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

		(void)shell_tx_uart_payload(argc, argv, 4);
		ret = wt_ble_transmit_payload((const uint8_t *)tx_payload_buf, payload_len);
		if (ret && ret != -EACCES) {
			shell_error(sh, "BLE transmit failed after Wi-Fi/UART TX: %d", ret);
			return ret;
		}

		shell_print(sh, "both TX: %d bytes%s", payload_len,
			    ret == -EACCES ? " (BLE notify off)" : "");
		return 0;
	}

	shell_print(sh, "usage: wt tx ble|uart|wifi|both ...");
	return -EINVAL;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wt_subcmds,
	SHELL_CMD_ARG(status, NULL, "Show WT02E40E radio state, optionally: json", shell_cmd_wt_status, 1, 1),
	SHELL_CMD_ARG(id, NULL, "Show compact device identity", shell_cmd_wt_id, 1, 0),
	SHELL_CMD_ARG(version, NULL, "Show firmware/build version", shell_cmd_wt_version, 1, 0),
	SHELL_CMD_ARG(fw, NULL, "Firmware: status, reboot [delay], reboot bootloader [delay]", shell_cmd_wt_fw, 1, 3),
	SHELL_CMD_ARG(reboot, NULL, "Reboot, optionally: bootloader [delay]", shell_cmd_wt_reboot, 1, 2),
	SHELL_CMD_ARG(config, NULL, "Show config, config json, config save, config reset", shell_cmd_wt_config, 1, 1),
	SHELL_CMD_ARG(boot, NULL, "Boot config: status, mode <idle|ble|wifi|both>", shell_cmd_wt_boot, 1, 2),
	SHELL_CMD_ARG(bridge, NULL, "Bridge rules and send", shell_cmd_wt_bridge, 1, WT_TX_PAYLOAD_MAX),
	SHELL_CMD_ARG(led, NULL, "LED indicators: status, test, pulse", shell_cmd_wt_led, 1, 2),
	SHELL_CMD_ARG(indicator, NULL, "LED indicators: status, test, pulse", shell_cmd_wt_led, 1, 2),
	SHELL_CMD_ARG(ping, NULL, "Ping local|uart|ble|wifi <ip> <port>", shell_cmd_wt_ping, 1, 3),
	SHELL_CMD_ARG(name, NULL, "Get/set BLE advertised name", shell_cmd_wt_name, 1, WT_BLE_NAME_MAX),
	SHELL_CMD_ARG(mode, NULL, "Set radio mode: idle, ble, wifi, both [delay]", shell_cmd_wt_mode, 2, 1),
	SHELL_CMD_ARG(discovery, NULL, "Control UDP discovery: on, off, status", shell_cmd_wt_discovery, 1, 1),
	SHELL_CMD_ARG(wifi, NULL, "Control Wi-Fi: on, off, status, reconnect, scan, cmd, discovery, cred", shell_cmd_wt_wifi, 2, WT_TX_PAYLOAD_MAX),
	SHELL_CMD_ARG(ble, NULL, "Control BLE: on, off, status, name", shell_cmd_wt_ble, 2, WT_BLE_NAME_MAX),
	SHELL_CMD_ARG(tx, NULL, "Transmit: ble <msg>, uart <msg>, wifi <ip> <port> <msg>, both <ip> <port> <msg>", shell_cmd_wt_tx, 3, WT_TX_PAYLOAD_MAX),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wt, &wt_subcmds, "WT02E40E radio commands", NULL);
