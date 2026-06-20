#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include "wt_app.h"
#include "wt_ble.h"
#include "wt_common.h"
#include "wt_config.h"
#include "wt_radio.h"
#include "wt_wifi.h"

LOG_MODULE_REGISTER(wt_app, CONFIG_LOG_DEFAULT_LEVEL);

static bool app_initialized;
static char boot_mode[WT_APP_BOOT_MODE_MAX + 1];
static bool bridge_ble_enabled = true;
static bool bridge_uart_enabled = true;
static bool bridge_wifi_enabled;
static char bridge_wifi_ip[WT_APP_BRIDGE_TARGET_MAX + 1] = "";
static uint16_t bridge_wifi_port = WT_WIFI_DISCOVERY_UDP_PORT;
static char delayed_radio_action[WT_APP_DELAYED_ACTION_MAX + 1];
static bool reboot_bootloader_requested;
static struct k_work_delayable delayed_radio_work;
static struct k_work_delayable reboot_work;

static const char *compile_default_mode(void)
{
#if defined(CONFIG_WT02E40E_DEFAULT_IDLE)
	return "idle";
#elif defined(CONFIG_WT02E40E_DEFAULT_WIFI)
	return "wifi";
#elif defined(CONFIG_WT02E40E_DEFAULT_BOTH)
	return "both";
#else
	return "ble";
#endif
}

static const char *current_mode(void)
{
	if (wt_wifi_is_requested() && wt_ble_is_requested()) {
		return "both";
	}
	if (wt_wifi_is_requested()) {
		return "wifi";
	}
	if (wt_ble_is_requested()) {
		return "ble";
	}
	return "idle";
}

static bool valid_mode(const char *mode)
{
	return mode && (!strcmp(mode, "idle") || !strcmp(mode, "off") ||
			      !strcmp(mode, "ble") || !strcmp(mode, "bt") ||
			      !strcmp(mode, "wifi") || !strcmp(mode, "wi-fi") ||
			      !strcmp(mode, "both"));
}

static const char *canonical_mode(const char *mode)
{
	if (!mode || !strcmp(mode, "idle") || !strcmp(mode, "off")) {
		return "idle";
	}
	if (!strcmp(mode, "ble") || !strcmp(mode, "bt")) {
		return "ble";
	}
	if (!strcmp(mode, "wifi") || !strcmp(mode, "wi-fi")) {
		return "wifi";
	}
	return "both";
}

static int app_rsp(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	int len;

	if (!buf || size == 0) {
		return -EINVAL;
	}

	va_start(args, fmt);
	len = vsnprintk(buf, size, fmt, args);
	va_end(args);

	if (len < 0) {
		buf[0] = '\0';
		return len;
	}
	if ((size_t)len >= size) {
		buf[size - 1] = '\0';
		return size - 1;
	}
	return len;
}

static int parse_port(const char *text, uint16_t *port)
{
	char *endptr;
	long value;

	if (!text || !port) {
		return -EINVAL;
	}

	value = strtol(text, &endptr, 10);
	if (*endptr != '\0' || value < 1 || value > 65535) {
		return -EINVAL;
	}

	*port = (uint16_t)value;
	return 0;
}

int wt_app_parse_delay_ms(const char *text, uint32_t *delay_ms)
{
	char *endptr;
	long value;

	if (!text || !delay_ms) {
		return -EINVAL;
	}

	value = strtol(text, &endptr, 10);
	if (value < 0) {
		return -EINVAL;
	}

	if (!strcmp(endptr, "ms")) {
		*delay_ms = (uint32_t)value;
		return 0;
	}

	if (*endptr == '\0' || !strcmp(endptr, "s") || !strcmp(endptr, "sec")) {
		*delay_ms = (uint32_t)value * 1000U;
		return 0;
	}

	return -EINVAL;
}

static void delayed_radio_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!strcmp(delayed_radio_action, "ble_off")) {
		(void)wt_ble_service_stop();
		return;
	}

	if (!strcmp(delayed_radio_action, "wifi_off")) {
		(void)wt_wifi_service_set(false);
		return;
	}

	if (valid_mode(delayed_radio_action)) {
		(void)wt_radio_mode_apply(delayed_radio_action);
	}
}

int wt_app_delayed_radio_apply(const char *mode_or_action, uint32_t delay_ms)
{
	if (!mode_or_action || strlen(mode_or_action) > WT_APP_DELAYED_ACTION_MAX) {
		return -EINVAL;
	}

	if (delay_ms == 0) {
		if (!strcmp(mode_or_action, "ble_off")) {
			return wt_ble_service_stop();
		}
		if (!strcmp(mode_or_action, "wifi_off")) {
			return wt_wifi_service_set(false);
		}
		return wt_radio_mode_apply(mode_or_action);
	}

	strncpy(delayed_radio_action, mode_or_action, sizeof(delayed_radio_action) - 1);
	delayed_radio_action[sizeof(delayed_radio_action) - 1] = '\0';
	k_work_schedule(&delayed_radio_work, K_MSEC(delay_ms));
	LOG_INF("Scheduled radio action %s in %u ms", delayed_radio_action, delay_ms);
	return 0;
}

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (reboot_bootloader_requested) {
		LOG_WRN("Bootloader reboot requested, falling back to cold reboot in this bring-up build");
	}

	sys_reboot(SYS_REBOOT_COLD);
}

int wt_app_reboot_schedule(bool bootloader, uint32_t delay_ms)
{
	reboot_bootloader_requested = bootloader;
	k_work_schedule(&reboot_work, K_MSEC(delay_ms));
	return 0;
}

const char *wt_app_boot_mode_get(void)
{
	if (boot_mode[0] == '\0') {
		return compile_default_mode();
	}
	return boot_mode;
}

int wt_app_boot_mode_set(const char *mode)
{
	if (!valid_mode(mode)) {
		return -EINVAL;
	}

	strncpy(boot_mode, canonical_mode(mode), sizeof(boot_mode) - 1);
	boot_mode[sizeof(boot_mode) - 1] = '\0';
	return 0;
}

bool wt_app_bridge_ble_enabled(void)
{
	return bridge_ble_enabled;
}

bool wt_app_bridge_uart_enabled(void)
{
	return bridge_uart_enabled;
}

bool wt_app_bridge_wifi_enabled(void)
{
	return bridge_wifi_enabled;
}

const char *wt_app_bridge_target_ip_get(void)
{
	return bridge_wifi_ip[0] ? bridge_wifi_ip : "";
}

uint16_t wt_app_bridge_target_port_get(void)
{
	return bridge_wifi_port;
}

int wt_app_bridge_set(const char *path, bool enable)
{
	if (!path) {
		return -EINVAL;
	}

	if (!strcmp(path, "ble") || !strcmp(path, "bt")) {
		bridge_ble_enabled = enable;
		return 0;
	}
	if (!strcmp(path, "uart") || !strcmp(path, "serial")) {
		bridge_uart_enabled = enable;
		return 0;
	}
	if (!strcmp(path, "wifi") || !strcmp(path, "udp")) {
		bridge_wifi_enabled = enable;
		return 0;
	}
	if (!strcmp(path, "all")) {
		bridge_ble_enabled = enable;
		bridge_uart_enabled = enable;
		bridge_wifi_enabled = enable;
		return 0;
	}

	return -EINVAL;
}

int wt_app_bridge_target_set(const char *ip, const char *port_text)
{
	uint16_t port;
	int ret;

	if (!ip || strlen(ip) == 0 || strlen(ip) > WT_APP_BRIDGE_TARGET_MAX) {
		return -EINVAL;
	}

	ret = parse_port(port_text, &port);
	if (ret) {
		return ret;
	}

	strncpy(bridge_wifi_ip, ip, sizeof(bridge_wifi_ip) - 1);
	bridge_wifi_ip[sizeof(bridge_wifi_ip) - 1] = '\0';
	bridge_wifi_port = port;
	return 0;
}

int wt_app_bridge_status_format(char *buf, size_t size, bool json)
{
	if (json) {
		return app_rsp(buf, size,
				       "{\"bridge_ble\":%s,\"bridge_uart\":%s,\"bridge_wifi\":%s,\"wifi_target\":\"%s\",\"wifi_port\":%u}",
				       bridge_ble_enabled ? "true" : "false",
				       bridge_uart_enabled ? "true" : "false",
				       bridge_wifi_enabled ? "true" : "false",
				       bridge_wifi_ip, bridge_wifi_port);
	}

	return app_rsp(buf, size,
			       "bridge ble=%s uart=%s wifi=%s target=%s:%u",
			       wt_onoff_txt(bridge_ble_enabled), wt_onoff_txt(bridge_uart_enabled),
			       wt_onoff_txt(bridge_wifi_enabled),
			       bridge_wifi_ip[0] ? bridge_wifi_ip : "<unset>", bridge_wifi_port);
}

int wt_app_bridge_send(const uint8_t *data, size_t len, char *rsp, size_t rsp_len)
{
	char port_text[8];
	int ble_ret = 0;
	int wifi_ret = 0;
	int sent = 0;

	if (!data || len == 0) {
		return app_rsp(rsp, rsp_len, "err bridge empty payload");
	}

	if (bridge_uart_enabled) {
		printk("BRIDGE UART TX: ");
		for (size_t i = 0; i < len; i++) {
			printk("%c", data[i]);
		}
		printk("\r\n");
		sent++;
	}

	if (bridge_ble_enabled) {
		ble_ret = wt_ble_transmit_payload(data, len);
		if (!ble_ret) {
			sent++;
		}
	}

	if (bridge_wifi_enabled) {
		if (bridge_wifi_ip[0] == '\0') {
			wifi_ret = -EDESTADDRREQ;
		} else {
			snprintk(port_text, sizeof(port_text), "%u", bridge_wifi_port);
			wifi_ret = wt_wifi_udp_transmit_payload(bridge_wifi_ip, port_text, data, len);
			if (!wifi_ret) {
				sent++;
			}
		}
	}

	return app_rsp(rsp, rsp_len,
			       "ok bridge sent=%d len=%u ble=%s uart=%s wifi=%s ble_ret=%d wifi_ret=%d target=%s:%u",
			       sent, (unsigned int)len, wt_onoff_txt(bridge_ble_enabled),
			       wt_onoff_txt(bridge_uart_enabled), wt_onoff_txt(bridge_wifi_enabled),
			       ble_ret, wifi_ret, bridge_wifi_ip[0] ? bridge_wifi_ip : "<unset>",
			       bridge_wifi_port);
}

#if defined(CONFIG_SETTINGS)
static int wt_app_settings_set(const char *name, size_t len,
				       settings_read_cb read_cb, void *cb_arg)
{
	if (!strcmp(name, "ble_name")) {
		char value[WT_BLE_NAME_MAX + 1];
		ssize_t read_len = read_cb(cb_arg, value, MIN(len, sizeof(value) - 1));
		if (read_len < 0) {
			return (int)read_len;
		}
		value[read_len] = '\0';
		return wt_ble_name_set(value);
	}

	if (!strcmp(name, "boot_mode")) {
		char value[WT_APP_BOOT_MODE_MAX + 1];
		ssize_t read_len = read_cb(cb_arg, value, MIN(len, sizeof(value) - 1));
		if (read_len < 0) {
			return (int)read_len;
		}
		value[read_len] = '\0';
		return wt_app_boot_mode_set(value);
	}

	if (!strcmp(name, "discovery")) {
		uint8_t enabled = 0;
		ssize_t read_len = read_cb(cb_arg, &enabled, MIN(len, sizeof(enabled)));
		if (read_len < 0) {
			return (int)read_len;
		}
		return wt_wifi_discovery_service_set(enabled != 0);
	}

	if (!strcmp(name, "wifi_cmd")) {
		uint8_t enabled = 1;
		ssize_t read_len = read_cb(cb_arg, &enabled, MIN(len, sizeof(enabled)));
		if (read_len < 0) {
			return (int)read_len;
		}
		return wt_wifi_cmd_service_set(enabled != 0);
	}

	if (!strcmp(name, "wifi_cmd_port")) {
		uint16_t port = WT_WIFI_CMD_UDP_PORT;
		ssize_t read_len = read_cb(cb_arg, &port, MIN(len, sizeof(port)));
		if (read_len < 0) {
			return (int)read_len;
		}
		return wt_wifi_cmd_port_set(port);
	}

	if (!strcmp(name, "bridge_ble")) {
		uint8_t enabled = 1;
		ssize_t read_len = read_cb(cb_arg, &enabled, MIN(len, sizeof(enabled)));
		if (read_len < 0) { return (int)read_len; }
		bridge_ble_enabled = enabled != 0;
		return 0;
	}

	if (!strcmp(name, "bridge_uart")) {
		uint8_t enabled = 1;
		ssize_t read_len = read_cb(cb_arg, &enabled, MIN(len, sizeof(enabled)));
		if (read_len < 0) { return (int)read_len; }
		bridge_uart_enabled = enabled != 0;
		return 0;
	}

	if (!strcmp(name, "bridge_wifi")) {
		uint8_t enabled = 0;
		ssize_t read_len = read_cb(cb_arg, &enabled, MIN(len, sizeof(enabled)));
		if (read_len < 0) { return (int)read_len; }
		bridge_wifi_enabled = enabled != 0;
		return 0;
	}

	if (!strcmp(name, "bridge_ip")) {
		ssize_t read_len = read_cb(cb_arg, bridge_wifi_ip, MIN(len, sizeof(bridge_wifi_ip) - 1));
		if (read_len < 0) { return (int)read_len; }
		bridge_wifi_ip[read_len] = '\0';
		return 0;
	}

	if (!strcmp(name, "bridge_port")) {
		uint16_t port = WT_WIFI_DISCOVERY_UDP_PORT;
		ssize_t read_len = read_cb(cb_arg, &port, MIN(len, sizeof(port)));
		if (read_len < 0) { return (int)read_len; }
		bridge_wifi_port = port;
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(wt_app_settings, "wtapp", NULL,
			       wt_app_settings_set, NULL, NULL);
#endif

int wt_app_settings_init(void)
{
	if (!app_initialized) {
		strncpy(boot_mode, compile_default_mode(), sizeof(boot_mode) - 1);
		boot_mode[sizeof(boot_mode) - 1] = '\0';
		k_work_init_delayable(&delayed_radio_work, delayed_radio_work_handler);
		k_work_init_delayable(&reboot_work, reboot_work_handler);
		app_initialized = true;
	}

#if defined(CONFIG_SETTINGS)
	int ret = settings_subsys_init();

	if (ret && ret != -EALREADY) {
		return ret;
	}

	ret = settings_load_subtree("wtapp");
	if (ret) {
		return ret;
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

int wt_app_config_save(void)
{
#if defined(CONFIG_SETTINGS)
	uint8_t discovery = wt_wifi_discovery_is_enabled() ? 1 : 0;
	uint8_t wifi_cmd = wt_wifi_cmd_is_enabled() ? 1 : 0;
	uint8_t bridge_ble = bridge_ble_enabled ? 1 : 0;
	uint8_t bridge_uart = bridge_uart_enabled ? 1 : 0;
	uint8_t bridge_wifi = bridge_wifi_enabled ? 1 : 0;
	uint16_t wifi_cmd_port = wt_wifi_cmd_port();
	int ret;

	ret = settings_save_one("wtapp/ble_name", wt_ble_name_get(), strlen(wt_ble_name_get()) + 1);
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/boot_mode", wt_app_boot_mode_get(), strlen(wt_app_boot_mode_get()) + 1);
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/discovery", &discovery, sizeof(discovery));
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/wifi_cmd", &wifi_cmd, sizeof(wifi_cmd));
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/wifi_cmd_port", &wifi_cmd_port, sizeof(wifi_cmd_port));
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/bridge_ble", &bridge_ble, sizeof(bridge_ble));
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/bridge_uart", &bridge_uart, sizeof(bridge_uart));
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/bridge_wifi", &bridge_wifi, sizeof(bridge_wifi));
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/bridge_ip", bridge_wifi_ip, strlen(bridge_wifi_ip) + 1);
	if (ret) { return ret; }
	ret = settings_save_one("wtapp/bridge_port", &bridge_wifi_port, sizeof(bridge_wifi_port));
	return ret;
#else
	return -ENOTSUP;
#endif
}

int wt_app_config_reset(void)
{
	int ret;

	ret = wt_ble_name_set(CONFIG_BT_DEVICE_NAME);
	if (ret) { return ret; }
	ret = wt_app_boot_mode_set(compile_default_mode());
	if (ret) { return ret; }
	ret = wt_wifi_discovery_service_set(false);
	if (ret) { return ret; }
	ret = wt_wifi_cmd_service_set(true);
	if (ret) { return ret; }
	ret = wt_wifi_cmd_port_set(WT_WIFI_CMD_UDP_PORT);
	if (ret) { return ret; }
	bridge_ble_enabled = true;
	bridge_uart_enabled = true;
	bridge_wifi_enabled = false;
	bridge_wifi_ip[0] = '\0';
	bridge_wifi_port = WT_WIFI_DISCOVERY_UDP_PORT;

#if defined(CONFIG_SETTINGS)
	(void)settings_delete("wtapp/ble_name");
	(void)settings_delete("wtapp/boot_mode");
	(void)settings_delete("wtapp/discovery");
	(void)settings_delete("wtapp/wifi_cmd");
	(void)settings_delete("wtapp/wifi_cmd_port");
	(void)settings_delete("wtapp/bridge_ble");
	(void)settings_delete("wtapp/bridge_uart");
	(void)settings_delete("wtapp/bridge_wifi");
	(void)settings_delete("wtapp/bridge_ip");
	(void)settings_delete("wtapp/bridge_port");
#endif

	return 0;
}

int wt_app_version_format(char *buf, size_t size)
{
	return app_rsp(buf, size,
			       "WT02E40E fw=%s build=%s %s board=nrf7002dk/nrf5340/cpuapp boot_mode=%s mtu_target=256 tx_max=%u wifi_cmd_port=%u discovery_port=%u",
			       WT_APP_FW_VERSION, __DATE__, __TIME__, wt_app_boot_mode_get(),
			       WT_TX_PAYLOAD_MAX, wt_wifi_cmd_port(), WT_WIFI_DISCOVERY_UDP_PORT);
}

int wt_app_fw_status_format(char *buf, size_t size)
{
	return app_rsp(buf, size,
			       "fw status version=%s build=%s %s reboot=cold bootloader_reboot=placeholder board=nrf7002dk/nrf5340/cpuapp",
			       WT_APP_FW_VERSION, __DATE__, __TIME__);
}

int wt_app_id_format(char *buf, size_t size)
{
	char ip[32];
	char mac[32];

	wt_wifi_ipv4_get(ip, sizeof(ip));
	wt_wifi_mac_get(mac, sizeof(mac));

	return app_rsp(buf, size,
			       "WT02E40E id name=%s fw=%s mode=%s boot=%s ble=%s wifi=%s ip=%s mac=%s wifi_cmd=%s cmd_port=%u discovery=%s discovery_port=%u uptime=%llds",
			       wt_ble_name_get(), WT_APP_FW_VERSION, current_mode(), wt_app_boot_mode_get(),
			       wt_onoff_txt(wt_ble_is_requested()), wt_onoff_txt(wt_wifi_is_requested()),
			       ip, mac, wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port(),
			       wt_onoff_txt(wt_wifi_discovery_is_enabled()), WT_WIFI_DISCOVERY_UDP_PORT,
			       (long long)(k_uptime_get() / 1000));
}

int wt_app_ble_status_format(char *buf, size_t size, bool json)
{
	if (json) {
		return app_rsp(buf, size,
				       "{\"ble_requested\":%s,\"ble_ready\":%s,\"ble_connected\":%s,\"ble_advertising\":%s,\"tx_notify\":%s,\"status_notify\":%s,\"cmd_rsp_notify\":%s,\"name\":\"%s\"}",
				       wt_ble_is_requested() ? "true" : "false",
				       wt_ble_is_ready() ? "true" : "false",
				       wt_ble_is_connected() ? "true" : "false",
				       wt_ble_is_advertising() ? "true" : "false",
				       wt_ble_tx_notify_is_enabled() ? "true" : "false",
				       wt_ble_status_notify_is_enabled() ? "true" : "false",
				       wt_ble_cmd_response_notify_is_enabled() ? "true" : "false",
				       wt_ble_name_get());
	}

	return app_rsp(buf, size,
			       "ble status name=%s requested=%s ready=%s connected=%s advertising=%s tx_notify=%s status_notify=%s cmd_rsp_notify=%s",
			       wt_ble_name_get(), wt_onoff_txt(wt_ble_is_requested()),
			       wt_onoff_txt(wt_ble_is_ready()), wt_onoff_txt(wt_ble_is_connected()),
			       wt_onoff_txt(wt_ble_is_advertising()), wt_onoff_txt(wt_ble_tx_notify_is_enabled()),
			       wt_onoff_txt(wt_ble_status_notify_is_enabled()),
			       wt_onoff_txt(wt_ble_cmd_response_notify_is_enabled()));
}

int wt_app_wifi_status_format(char *buf, size_t size, bool json)
{
	char ip[32];
	char mac[32];

	wt_wifi_ipv4_get(ip, sizeof(ip));
	wt_wifi_mac_get(mac, sizeof(mac));

	if (json) {
		return app_rsp(buf, size,
				       "{\"wifi_requested\":%s,\"wifi_associated\":%s,\"ipv4_bound\":%s,\"ip\":\"%s\",\"mac\":\"%s\",\"cmd_enabled\":%s,\"cmd_port\":%u,\"discovery\":%s,\"discovery_port\":%u}",
				       wt_wifi_is_requested() ? "true" : "false",
				       wt_wifi_is_associated() ? "true" : "false",
				       wt_wifi_has_ipv4() ? "true" : "false",
				       ip, mac,
				       wt_wifi_cmd_is_enabled() ? "true" : "false",
				       wt_wifi_cmd_port(),
				       wt_wifi_discovery_is_enabled() ? "true" : "false",
				       WT_WIFI_DISCOVERY_UDP_PORT);
	}

	return app_rsp(buf, size,
			       "wifi status requested=%s associated=%s ipv4=%s ip=%s mac=%s cmd=%s cmd_port=%u discovery=%s discovery_port=%u",
			       wt_onoff_txt(wt_wifi_is_requested()), wt_onoff_txt(wt_wifi_is_associated()),
			       wt_onoff_txt(wt_wifi_has_ipv4()), ip, mac,
			       wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port(),
			       wt_onoff_txt(wt_wifi_discovery_is_enabled()), WT_WIFI_DISCOVERY_UDP_PORT);
}

int wt_app_config_format(char *buf, size_t size, bool json)
{
	char ip[32];
	char mac[32];

	wt_wifi_ipv4_get(ip, sizeof(ip));
	wt_wifi_mac_get(mac, sizeof(mac));

	if (json) {
		return app_rsp(buf, size,
				       "{\"name\":\"%s\",\"fw\":\"%s\",\"mode\":\"%s\",\"boot_mode\":\"%s\",\"ble_requested\":%s,\"ble_ready\":%s,\"ble_connected\":%s,\"ble_advertising\":%s,\"wifi_requested\":%s,\"wifi_associated\":%s,\"ipv4\":%s,\"ip\":\"%s\",\"mac\":\"%s\",\"wifi_cmd\":%s,\"wifi_cmd_port\":%u,\"discovery\":%s,\"discovery_port\":%u,\"bridge_ble\":%s,\"bridge_uart\":%s,\"bridge_wifi\":%s,\"bridge_target\":\"%s\",\"bridge_port\":%u,\"tx_max\":%u,\"uptime_s\":%lld}",
				       wt_ble_name_get(), WT_APP_FW_VERSION, current_mode(), wt_app_boot_mode_get(),
				       wt_ble_is_requested() ? "true" : "false",
				       wt_ble_is_ready() ? "true" : "false",
				       wt_ble_is_connected() ? "true" : "false",
				       wt_ble_is_advertising() ? "true" : "false",
				       wt_wifi_is_requested() ? "true" : "false",
				       wt_wifi_is_associated() ? "true" : "false",
				       wt_wifi_has_ipv4() ? "true" : "false",
				       ip, mac,
				       wt_wifi_cmd_is_enabled() ? "true" : "false",
				       wt_wifi_cmd_port(),
				       wt_wifi_discovery_is_enabled() ? "true" : "false",
				       WT_WIFI_DISCOVERY_UDP_PORT,
				       bridge_ble_enabled ? "true" : "false",
				       bridge_uart_enabled ? "true" : "false",
				       bridge_wifi_enabled ? "true" : "false",
				       bridge_wifi_ip, bridge_wifi_port,
				       WT_TX_PAYLOAD_MAX, (long long)(k_uptime_get() / 1000));
	}

	return app_rsp(buf, size,
			       "config name=%s fw=%s mode=%s boot=%s ble_req=%s ble_ready=%s ble_conn=%s adv=%s wifi_req=%s wifi_assoc=%s ipv4=%s ip=%s mac=%s wifi_cmd=%s cmd_port=%u discovery=%s discovery_port=%u bridge_ble=%s bridge_uart=%s bridge_wifi=%s bridge_target=%s:%u tx_max=%u uptime=%llds",
			       wt_ble_name_get(), WT_APP_FW_VERSION, current_mode(), wt_app_boot_mode_get(),
			       wt_onoff_txt(wt_ble_is_requested()), wt_onoff_txt(wt_ble_is_ready()),
			       wt_onoff_txt(wt_ble_is_connected()), wt_onoff_txt(wt_ble_is_advertising()),
			       wt_onoff_txt(wt_wifi_is_requested()), wt_onoff_txt(wt_wifi_is_associated()),
			       wt_onoff_txt(wt_wifi_has_ipv4()), ip, mac,
			       wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port(),
			       wt_onoff_txt(wt_wifi_discovery_is_enabled()), WT_WIFI_DISCOVERY_UDP_PORT,
			       wt_onoff_txt(bridge_ble_enabled), wt_onoff_txt(bridge_uart_enabled),
			       wt_onoff_txt(bridge_wifi_enabled), bridge_wifi_ip[0] ? bridge_wifi_ip : "<unset>",
			       bridge_wifi_port, WT_TX_PAYLOAD_MAX, (long long)(k_uptime_get() / 1000));
}

int wt_app_ping_execute(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	char payload[96];
	char port_text[8];
	int ret;

	snprintk(payload, sizeof(payload), "pong fw=%s uptime=%llds", WT_APP_FW_VERSION,
		  (long long)(k_uptime_get() / 1000));

	if (argc < 2 || !strcmp(argv[1], "local")) {
		return app_rsp(rsp, rsp_len, "%s", payload);
	}

	if (!strcmp(argv[1], "uart") || !strcmp(argv[1], "serial")) {
		printk("PING UART: %s\r\n", payload);
		return app_rsp(rsp, rsp_len, "ok ping uart %s", payload);
	}

	if (!strcmp(argv[1], "ble") || !strcmp(argv[1], "bt")) {
		ret = wt_ble_transmit_payload((const uint8_t *)payload, strlen(payload));
		if (ret == -EACCES) {
			return app_rsp(rsp, rsp_len, "err ping ble tx_notify_off");
		}
		return ret ? app_rsp(rsp, rsp_len, "err ping ble %d", ret) : app_rsp(rsp, rsp_len, "ok ping ble %s", payload);
	}

	if (!strcmp(argv[1], "wifi") || !strcmp(argv[1], "udp")) {
		const char *ip = argc >= 4 ? argv[2] : bridge_wifi_ip;
		const char *port = argc >= 4 ? argv[3] : NULL;

		if (!port) {
			snprintk(port_text, sizeof(port_text), "%u", bridge_wifi_port);
			port = port_text;
		}

		if (!ip || ip[0] == '\0') {
			return app_rsp(rsp, rsp_len, "usage ping wifi <ip> <port> or set bridge target first");
		}

		ret = wt_wifi_udp_transmit_payload(ip, port, (const uint8_t *)payload, strlen(payload));
		return ret ? app_rsp(rsp, rsp_len, "err ping wifi %d", ret) : app_rsp(rsp, rsp_len, "ok ping wifi %s:%s %s", ip, port, payload);
	}

	return app_rsp(rsp, rsp_len, "usage ping [local|uart|ble|wifi <ip> <port>]");
}

int wt_app_boot_command(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	int ret;

	if (argc < 2 || !strcmp(argv[1], "status") || !strcmp(argv[1], "get")) {
		return app_rsp(rsp, rsp_len, "ok boot mode %s", wt_app_boot_mode_get());
	}

	if (!strcmp(argv[1], "mode")) {
		if (argc < 3) {
			return app_rsp(rsp, rsp_len, "ok boot mode %s", wt_app_boot_mode_get());
		}
		ret = wt_app_boot_mode_set(argv[2]);
		return ret ? app_rsp(rsp, rsp_len, "err boot mode %d", ret) :
			     app_rsp(rsp, rsp_len, "ok boot mode %s", wt_app_boot_mode_get());
	}

	return app_rsp(rsp, rsp_len, "usage boot [status] | boot mode <idle|ble|wifi|both>");
}

int wt_app_bridge_command(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	bool enable;
	int ret;
	int payload_len;
	char payload[WT_TX_PAYLOAD_MAX];

	if (argc < 2 || !strcmp(argv[1], "status")) {
		return wt_app_bridge_status_format(rsp, rsp_len, argc >= 3 && !strcmp(argv[2], "json"));
	}

	if (!strcmp(argv[1], "target")) {
		if (argc < 4) {
			return app_rsp(rsp, rsp_len, "usage bridge target <ip> <port>");
		}
		ret = wt_app_bridge_target_set(argv[2], argv[3]);
		return ret ? app_rsp(rsp, rsp_len, "err bridge target %d", ret) :
			     app_rsp(rsp, rsp_len, "ok bridge target %s:%u", wt_app_bridge_target_ip_get(), wt_app_bridge_target_port_get());
	}

	if (!strcmp(argv[1], "send") || !strcmp(argv[1], "tx")) {
		if (argc < 3) {
			return app_rsp(rsp, rsp_len, "usage bridge send <message>");
		}
		payload_len = wt_build_payload_from_argv(2, argc, argv, payload, sizeof(payload));
		if (payload_len < 0) {
			return app_rsp(rsp, rsp_len, "err bridge payload %d", payload_len);
		}
		return wt_app_bridge_send((const uint8_t *)payload, payload_len, rsp, rsp_len);
	}

	if (!strcmp(argv[1], "all") && argc >= 3 && strcmp(argv[2], "on") && strcmp(argv[2], "off")) {
		payload_len = wt_build_payload_from_argv(2, argc, argv, payload, sizeof(payload));
		if (payload_len < 0) {
			return app_rsp(rsp, rsp_len, "err bridge payload %d", payload_len);
		}
		return wt_app_bridge_send((const uint8_t *)payload, payload_len, rsp, rsp_len);
	}

	if (!strcmp(argv[1], "ble") || !strcmp(argv[1], "bt") ||
	    !strcmp(argv[1], "uart") || !strcmp(argv[1], "serial") ||
	    !strcmp(argv[1], "wifi") || !strcmp(argv[1], "udp") ||
	    !strcmp(argv[1], "all")) {
		if (argc < 3) {
			return app_rsp(rsp, rsp_len, "usage bridge ble|uart|wifi|all on|off");
		}
		if (!strcmp(argv[2], "on") || !strcmp(argv[2], "enable")) {
			enable = true;
		} else if (!strcmp(argv[2], "off") || !strcmp(argv[2], "disable")) {
			enable = false;
		} else {
			return app_rsp(rsp, rsp_len, "usage bridge ble|uart|wifi|all on|off");
		}
		ret = wt_app_bridge_set(argv[1], enable);
		if (ret) {
			return app_rsp(rsp, rsp_len, "err bridge set %d", ret);
		}
		return wt_app_bridge_status_format(rsp, rsp_len, false);
	}

	return app_rsp(rsp, rsp_len,
		       "usage bridge status [json] | bridge target <ip> <port> | bridge ble|uart|wifi|all on|off | bridge send <msg> | bridge all <msg>");
}

int wt_app_fw_command(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	uint32_t delay_ms = WT_APP_REBOOT_DELAY_MS;
	bool bootloader = false;
	int ret;

	if (argc < 2 || !strcmp(argv[1], "status")) {
		return wt_app_fw_status_format(rsp, rsp_len);
	}

	if (!strcmp(argv[1], "reboot")) {
		if (argc >= 3 && !strcmp(argv[2], "bootloader")) {
			bootloader = true;
			if (argc >= 4) {
				ret = wt_app_parse_delay_ms(argv[3], &delay_ms);
				if (ret) { return app_rsp(rsp, rsp_len, "err reboot delay %d", ret); }
			}
		} else if (argc >= 3) {
			ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
			if (ret) { return app_rsp(rsp, rsp_len, "err reboot delay %d", ret); }
		}
		ret = wt_app_reboot_schedule(bootloader, delay_ms);
		return ret ? app_rsp(rsp, rsp_len, "err reboot %d", ret) :
			     app_rsp(rsp, rsp_len, "ok reboot%s in %ums", bootloader ? " bootloader-placeholder" : "", delay_ms);
	}

	return app_rsp(rsp, rsp_len, "usage fw status | fw reboot [delay] | fw reboot bootloader [delay]");
}
