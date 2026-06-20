#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(wt_ble, CONFIG_LOG_DEFAULT_LEVEL);

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#endif

#include "wt_ble.h"
#include "wt_common.h"
#include "wt_config.h"
#include "wt_radio.h"
#include "wt_wifi.h"

#if defined(CONFIG_BT)
static bool ble_requested;
static bool ble_ready;
static bool ble_advertising;
static bool ble_connected_state;
static struct bt_conn *ble_conn;

static bool ble_tx_notify_enabled;
static bool ble_status_notify_enabled;
static bool ble_cmd_rsp_notify_enabled;
static char ble_status_buf[WT_BLE_STATUS_TEXT_MAX];
static char ble_cmd_buf[WT_BLE_CMD_TEXT_MAX];
static char ble_cmd_rsp_buf[WT_BLE_CMD_RSP_TEXT_MAX];
static struct k_work_delayable ble_status_ping_work;
static struct k_work_delayable ble_adv_restart_work;

#define WT_BT_UUID_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x7f1c0001, 0x2b5a, 0x4f2d, 0x9a31, 0xd6a5f4e040e1)
#define WT_BT_UUID_TX_VAL \
	BT_UUID_128_ENCODE(0x7f1c0002, 0x2b5a, 0x4f2d, 0x9a31, 0xd6a5f4e040e1)
#define WT_BT_UUID_STATUS_VAL \
	BT_UUID_128_ENCODE(0x7f1c0003, 0x2b5a, 0x4f2d, 0x9a31, 0xd6a5f4e040e1)
#define WT_BT_UUID_CMD_VAL \
	BT_UUID_128_ENCODE(0x7f1c0004, 0x2b5a, 0x4f2d, 0x9a31, 0xd6a5f4e040e1)
#define WT_BT_UUID_RSP_VAL \
	BT_UUID_128_ENCODE(0x7f1c0005, 0x2b5a, 0x4f2d, 0x9a31, 0xd6a5f4e040e1)

static struct bt_uuid_128 wt_bt_service_uuid = BT_UUID_INIT_128(WT_BT_UUID_SERVICE_VAL);
static struct bt_uuid_128 wt_bt_tx_uuid = BT_UUID_INIT_128(WT_BT_UUID_TX_VAL);
static struct bt_uuid_128 wt_bt_status_uuid = BT_UUID_INIT_128(WT_BT_UUID_STATUS_VAL);
static struct bt_uuid_128 wt_bt_cmd_uuid = BT_UUID_INIT_128(WT_BT_UUID_CMD_VAL);
static struct bt_uuid_128 wt_bt_rsp_uuid = BT_UUID_INIT_128(WT_BT_UUID_RSP_VAL);

static int build_status_payload(char *buf, size_t size);
static int wt_ble_command_execute(const char *line);
static void wt_ble_status_ping_work_handler(struct k_work *work);
static void wt_ble_status_reschedule(void);
static void wt_ble_adv_restart_work_handler(struct k_work *work);
static int wt_ble_advertising_start(void);
static void wt_ble_schedule_advertising_restart(void);
static size_t wt_ble_notify_payload_max(void);
static int wt_ble_notify_chunked(const struct bt_gatt_attr *attr,
					 const uint8_t *data, size_t len);

bool wt_ble_is_requested(void)
{
	return ble_requested;
}

bool wt_ble_is_ready(void)
{
	return ble_ready;
}

bool wt_ble_is_advertising(void)
{
	return ble_advertising;
}

bool wt_ble_is_connected(void)
{
	return ble_connected_state;
}

bool wt_ble_tx_notify_is_enabled(void)
{
	return ble_tx_notify_enabled;
}

bool wt_ble_status_notify_is_enabled(void)
{
	return ble_status_notify_enabled;
}

bool wt_ble_cmd_response_notify_is_enabled(void)
{
	return ble_cmd_rsp_notify_enabled;
}

static void wt_ble_tx_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	ble_tx_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("BLE TX notifications %s", ble_tx_notify_enabled ? "on" : "off");
}

static ssize_t wt_ble_status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					 void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	(void)build_status_payload(ble_status_buf, sizeof(ble_status_buf));

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				     ble_status_buf, strlen(ble_status_buf));
}

static void wt_ble_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	ble_status_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("BLE status notifications %s", ble_status_notify_enabled ? "on" : "off");
	wt_ble_status_reschedule();
}

static ssize_t wt_ble_rsp_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				       void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				     ble_cmd_rsp_buf, strlen(ble_cmd_rsp_buf));
}

static void wt_ble_rsp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	ble_cmd_rsp_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("BLE command response notifications %s",
		ble_cmd_rsp_notify_enabled ? "on" : "off");
}

static int wt_ble_rsp_send(const char *fmt, ...);

static ssize_t wt_ble_cmd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	int ret;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset + len >= sizeof(ble_cmd_buf)) {
		(void)wt_ble_rsp_send("err command too long");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (offset == 0) {
		memset(ble_cmd_buf, 0, sizeof(ble_cmd_buf));
	}

	memcpy(&ble_cmd_buf[offset], buf, len);
	ble_cmd_buf[offset + len] = '\0';

	LOG_INF("BLE command: %s", ble_cmd_buf);
	ret = wt_ble_command_execute(ble_cmd_buf);
	if (ret) {
		LOG_WRN("BLE command failed: %d", ret);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(wt_ble_tx_svc,
	BT_GATT_PRIMARY_SERVICE(&wt_bt_service_uuid),
	BT_GATT_CHARACTERISTIC(&wt_bt_tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
				       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(wt_ble_tx_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&wt_bt_status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
				       BT_GATT_PERM_READ, wt_ble_status_read, NULL, NULL),
	BT_GATT_CCC(wt_ble_status_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&wt_bt_cmd_uuid.uuid,
				       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
				       BT_GATT_PERM_WRITE, NULL, wt_ble_cmd_write, NULL),
	BT_GATT_CHARACTERISTIC(&wt_bt_rsp_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
				       BT_GATT_PERM_READ, wt_ble_rsp_read, NULL, NULL),
	BT_GATT_CCC(wt_ble_rsp_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static size_t wt_ble_notify_payload_max(void)
{
	uint16_t mtu = 23;

	if (ble_conn) {
		mtu = bt_gatt_get_mtu(ble_conn);
	}

	if (mtu <= 3) {
		return 20;
	}

	return MIN((size_t)(mtu - 3), (size_t)WT_BLE_NOTIFY_APP_PAYLOAD_MAX);
}

static int wt_ble_notify_chunked(const struct bt_gatt_attr *attr,
					 const uint8_t *data, size_t len)
{
	size_t offset = 0;
	size_t chunk_max;
	int ret = 0;

	if (!ble_ready) {
		return -ENODEV;
	}

	if (!ble_conn || !ble_connected_state) {
		return -ENOTCONN;
	}

	chunk_max = wt_ble_notify_payload_max();
	if (chunk_max == 0) {
		chunk_max = 20;
	}

	while (offset < len) {
		size_t chunk_len = MIN(len - offset, chunk_max);

		ret = bt_gatt_notify(ble_conn, attr, &data[offset], chunk_len);
		if (ret == -EMSGSIZE && chunk_max > 20) {
			chunk_max = 20;
			chunk_len = MIN(len - offset, chunk_max);
			ret = bt_gatt_notify(ble_conn, attr, &data[offset], chunk_len);
		}

		if (ret) {
			return ret;
		}

		offset += chunk_len;
	}

	return 0;
}

static int wt_ble_rsp_send(const char *fmt, ...)
{
	va_list args;
	int len;
	int notify_ret = 0;

	va_start(args, fmt);
	len = vsnprintk(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf), fmt, args);
	va_end(args);

	if (len < 0) {
		ble_cmd_rsp_buf[0] = '\0';
		return len;
	}

	if ((size_t)len >= sizeof(ble_cmd_rsp_buf)) {
		len = sizeof(ble_cmd_rsp_buf) - 1;
	}

	LOG_INF("BLE command response: %s", ble_cmd_rsp_buf);

	if (ble_conn && ble_connected_state && ble_cmd_rsp_notify_enabled) {
		notify_ret = wt_ble_notify_chunked(&wt_ble_tx_svc.attrs[10],
						     (const uint8_t *)ble_cmd_rsp_buf,
						     (size_t)len);
		if (notify_ret) {
			LOG_WRN("BLE command response notify failed: %d", notify_ret);
			return notify_ret;
		}
	}

	return 0;
}



static const struct bt_data ble_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data ble_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int build_status_payload(char *buf, size_t size)
{
	const char *mode;
	int len;

	if (wt_wifi_is_requested() && ble_requested) {
		mode = "both";
	} else if (wt_wifi_is_requested()) {
		mode = "wifi";
	} else if (ble_requested) {
		mode = "ble";
	} else {
		mode = "idle";
	}

	len = snprintk(buf, size,
			"WT02E40E status: mode=%s ble=%s adv=%s tx_notify=%s status_notify=%s cmd_rsp_notify=%s wifi_req=%s wifi_assoc=%s ipv4=%s uptime=%llds",
			mode,
			wt_onoff_txt(ble_connected_state),
			wt_onoff_txt(ble_advertising),
			wt_onoff_txt(ble_tx_notify_enabled),
			wt_onoff_txt(ble_status_notify_enabled),
			wt_onoff_txt(ble_cmd_rsp_notify_enabled),
			wt_onoff_txt(wt_wifi_is_requested()),
			wt_onoff_txt(wt_wifi_is_associated()),
			wt_onoff_txt(wt_wifi_has_ipv4()),
			(long long)(k_uptime_get() / 1000));

	if (len < 0) {
		buf[0] = '\0';
		return 0;
	}

	if ((size_t)len >= size) {
		return size - 1;
	}

	return len;
}


static int wt_ble_split_args(char *line, char **argv, size_t argv_max)
{
	char *p = line;
	size_t argc = 0;

	while (*p && argc < argv_max) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			*p++ = '\0';
		}

		if (!*p) {
			break;
		}

		argv[argc++] = p;

		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
			p++;
		}
	}

	return (int)argc;
}

static int wt_ble_tx_uart_payload(char **argv, size_t argc, size_t payload_arg)
{
	int payload_len;
	char payload[WT_TX_PAYLOAD_MAX];

	payload_len = wt_build_payload_from_argv(payload_arg, argc, argv,
					 payload, sizeof(payload));
	if (payload_len < 0) {
		return payload_len;
	}

	printk("BLE UART TX: ");
	for (int i = 0; i < payload_len; i++) {
		printk("%c", payload[i]);
	}
	printk("\r\n");

	return payload_len;
}

static int wt_ble_command_tx(char **argv, size_t argc)
{
	int payload_len;
	int ret;
	char payload[WT_TX_PAYLOAD_MAX];

	if (argc < 3) {
		return wt_ble_rsp_send("usage tx ble <msg> | tx uart <msg> | tx wifi <ip> <port> <msg> | tx both <ip> <port> <msg>");
	}

	if (!strcmp(argv[1], "ble") || !strcmp(argv[1], "bt")) {
		payload_len = wt_build_payload_from_argv(2, argc, argv,
						 payload, sizeof(payload));
		if (payload_len < 0) {
			return wt_ble_rsp_send("err payload %d", payload_len);
		}

		ret = wt_ble_transmit_payload((const uint8_t *)payload, payload_len);
		if (ret == -EACCES) {
			return wt_ble_rsp_send("err ble tx notify off; enable TX notify first");
		}
		if (ret == -ENOTCONN) {
			return wt_ble_rsp_send("err ble tx not connected");
		}
		if (ret) {
			return wt_ble_rsp_send("err ble tx %d", ret);
		}

		return wt_ble_rsp_send("ok ble tx %d bytes", payload_len);
	}

	if (!strcmp(argv[1], "uart") || !strcmp(argv[1], "serial")) {
		payload_len = wt_ble_tx_uart_payload(argv, argc, 2);
		if (payload_len < 0) {
			return wt_ble_rsp_send("err uart payload %d", payload_len);
		}

		return wt_ble_rsp_send("ok uart tx %d bytes", payload_len);
	}

	if (!strcmp(argv[1], "wifi") || !strcmp(argv[1], "udp")) {
		if (argc < 5) {
			return wt_ble_rsp_send("usage tx wifi <ip> <port> <msg>");
		}

		payload_len = wt_build_payload_from_argv(4, argc, argv,
						 payload, sizeof(payload));
		if (payload_len < 0) {
			return wt_ble_rsp_send("err payload %d", payload_len);
		}

		ret = wt_wifi_udp_transmit_payload(argv[2], argv[3],
						       (const uint8_t *)payload, payload_len);
		if (ret) {
			return wt_ble_rsp_send("err wifi tx %d", ret);
		}

		return wt_ble_rsp_send("ok wifi udp tx %d bytes to %s:%s",
					      payload_len, argv[2], argv[3]);
	}

	if (!strcmp(argv[1], "both") || !strcmp(argv[1], "all")) {
		if (argc < 5) {
			return wt_ble_rsp_send("usage tx both <ip> <port> <msg>");
		}

		payload_len = wt_build_payload_from_argv(4, argc, argv,
						 payload, sizeof(payload));
		if (payload_len < 0) {
			return wt_ble_rsp_send("err payload %d", payload_len);
		}

		ret = wt_wifi_udp_transmit_payload(argv[2], argv[3],
						       (const uint8_t *)payload, payload_len);
		if (ret) {
			return wt_ble_rsp_send("err wifi tx %d", ret);
		}

		(void)wt_ble_tx_uart_payload(argv, argc, 4);
		ret = wt_ble_transmit_payload((const uint8_t *)payload, payload_len);
		if (ret && ret != -EACCES) {
			return wt_ble_rsp_send("err ble tx %d after wifi/uart", ret);
		}

		return wt_ble_rsp_send("ok wifi+uart%s tx %d bytes%s",
					      ret == -EACCES ? "" : "+ble", payload_len,
					      ret == -EACCES ? " (ble notify off)" : "");
	}

	return wt_ble_rsp_send("usage tx ble|uart|wifi|both ...");
}

static int wt_ble_command_wifi_cred(char **argv, size_t argc)
{
	int ret;
	char list[WT_BLE_CMD_RSP_TEXT_MAX - 32];

	if (argc < 3) {
		return wt_ble_rsp_send("usage wifi cred list|set|add|open|forget|clear");
	}

	if (!strcmp(argv[2], "list")) {
		ret = wt_wifi_credentials_format_list(list, sizeof(list));
		if (ret < 0) {
			return wt_ble_rsp_send("err cred list %d", ret);
		}
		return wt_ble_rsp_send("ok creds %s", list);
	}

	if (!strcmp(argv[2], "clear")) {
		ret = wt_wifi_credentials_clear();
		if (ret) {
			return wt_ble_rsp_send("err cred clear %d", ret);
		}
		return wt_ble_rsp_send("ok creds cleared");
	}

	if (!strcmp(argv[2], "forget")) {
		if (argc < 4) {
			return wt_ble_rsp_send("usage wifi cred forget <ssid>");
		}
		ret = wt_wifi_credentials_forget(argv[3]);
		if (ret) {
			return wt_ble_rsp_send("err cred forget %d", ret);
		}
		return wt_ble_rsp_send("ok forgot %s", argv[3]);
	}

	if (!strcmp(argv[2], "open")) {
		if (argc < 4) {
			return wt_ble_rsp_send("usage wifi cred open <ssid>");
		}
		ret = wt_wifi_credentials_open(argv[3], true);
		if (ret) {
			return wt_ble_rsp_send("err cred open %d", ret);
		}
		(void)wt_wifi_reconnect_if_requested();
		return wt_ble_rsp_send("ok open ssid %s", argv[3]);
	}

	if (!strcmp(argv[2], "set") || !strcmp(argv[2], "add")) {
		bool replace_all = !strcmp(argv[2], "set");
		const char *security = argc >= 6 ? argv[5] : NULL;

		if (argc < 5) {
			return wt_ble_rsp_send("usage wifi cred %s <ssid> <pass> [wpa2|auto|wpa3]", argv[2]);
		}

		ret = wt_wifi_credentials_set(argv[3], argv[4], security, replace_all);
		if (ret) {
			return wt_ble_rsp_send("err cred set %d", ret);
		}
		(void)wt_wifi_reconnect_if_requested();
		return wt_ble_rsp_send("ok %s ssid %s", replace_all ? "selected" : "added", argv[3]);
	}

	return wt_ble_rsp_send("usage wifi cred list|set|add|open|forget|clear");
}

static int wt_ble_command_wifi(char **argv, size_t argc)
{
	int ret;

	if (argc < 2) {
		return wt_ble_rsp_send("usage wifi on|off|status|reconnect|cred");
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_wifi_service_set(true);
		return ret ? wt_ble_rsp_send("err wifi on %d", ret) : wt_ble_rsp_send("ok wifi on");
	}

	if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		ret = wt_wifi_service_set(false);
		return ret ? wt_ble_rsp_send("err wifi off %d", ret) : wt_ble_rsp_send("ok wifi off");
	}

	if (!strcmp(argv[1], "status")) {
		(void)build_status_payload(ble_status_buf, sizeof(ble_status_buf));
		return wt_ble_rsp_send("%s", ble_status_buf);
	}

	if (!strcmp(argv[1], "reconnect")) {
		ret = wt_wifi_reconnect_if_requested();
		if (!wt_wifi_is_requested()) {
			ret = wt_wifi_service_set(true);
		}
		return ret ? wt_ble_rsp_send("err wifi reconnect %d", ret) : wt_ble_rsp_send("ok wifi reconnect");
	}

	if (!strcmp(argv[1], "cred") || !strcmp(argv[1], "creds")) {
		return wt_ble_command_wifi_cred(argv, argc);
	}

	return wt_ble_rsp_send("usage wifi on|off|status|reconnect|cred");
}

static int wt_ble_command_execute(const char *line)
{
	char line_buf[WT_BLE_CMD_TEXT_MAX];
	char *argv_storage[16];
	char **argv = argv_storage;
	int argc;
	int ret;

	if (!line || strlen(line) == 0) {
		return wt_ble_rsp_send("err empty command");
	}

	strncpy(line_buf, line, sizeof(line_buf) - 1);
	line_buf[sizeof(line_buf) - 1] = '\0';

	argc = wt_ble_split_args(line_buf, argv_storage, ARRAY_SIZE(argv_storage));
	if (argc <= 0) {
		return wt_ble_rsp_send("err empty command");
	}

	if (!strcmp(argv[0], "wt")) {
		argv++;
		argc--;
		if (argc <= 0) {
			return wt_ble_rsp_send("usage wt status|mode|wifi|ble|tx|help");
		}
	}

	if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
		return wt_ble_rsp_send("cmds: status, mode idle|ble|wifi|both, wifi ..., ble ..., tx ble|uart|wifi|both ...");
	}

	if (!strcmp(argv[0], "status")) {
		(void)build_status_payload(ble_status_buf, sizeof(ble_status_buf));
		return wt_ble_rsp_send("%s", ble_status_buf);
	}

	if (!strcmp(argv[0], "mode")) {
		if (argc < 2) {
			return wt_ble_rsp_send("usage mode idle|ble|wifi|both");
		}
		ret = wt_radio_mode_apply(argv[1]);
		return ret ? wt_ble_rsp_send("err mode %d", ret) : wt_ble_rsp_send("ok mode %s", argv[1]);
	}

	if (!strcmp(argv[0], "wifi")) {
		return wt_ble_command_wifi(argv, argc);
	}

	if (!strcmp(argv[0], "ble") || !strcmp(argv[0], "bt")) {
		if (argc < 2) {
			return wt_ble_rsp_send("usage ble on|off|status");
		}
		if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
			ret = wt_ble_service_start();
			return ret ? wt_ble_rsp_send("err ble on %d", ret) : wt_ble_rsp_send("ok ble on");
		}
		if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
			ret = wt_ble_service_stop();
			return ret ? wt_ble_rsp_send("err ble off %d", ret) : wt_ble_rsp_send("ok ble off");
		}
		if (!strcmp(argv[1], "status")) {
			(void)build_status_payload(ble_status_buf, sizeof(ble_status_buf));
			return wt_ble_rsp_send("%s", ble_status_buf);
		}
		return wt_ble_rsp_send("usage ble on|off|status");
	}

	if (!strcmp(argv[0], "tx")) {
		return wt_ble_command_tx(argv, argc);
	}

	return wt_ble_rsp_send("err unknown command: %s", argv[0]);
}

static int wt_ble_status_notify_now(void)
{
	int len;

	if (!ble_ready || !ble_conn || !ble_connected_state) {
		return -ENOTCONN;
	}

	if (!ble_status_notify_enabled) {
		return -EACCES;
	}

	len = build_status_payload(ble_status_buf, sizeof(ble_status_buf));
	if (len <= 0) {
		return -EIO;
	}

	return wt_ble_notify_chunked(&wt_ble_tx_svc.attrs[5],
				     (const uint8_t *)ble_status_buf, (size_t)len);
}

static void wt_ble_status_ping_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	if (!ble_connected_state || !ble_status_notify_enabled) {
		return;
	}

	ret = wt_ble_status_notify_now();
	if (ret) {
		LOG_DBG("BLE status notify skipped: %d", ret);
	}

	if (ble_connected_state && ble_status_notify_enabled) {
		k_work_schedule(&ble_status_ping_work, K_MSEC(WT_BLE_STATUS_PING_MS));
	}
}

static void wt_ble_status_reschedule(void)
{
	if (ble_connected_state && ble_status_notify_enabled) {
		k_work_schedule(&ble_status_ping_work, K_NO_WAIT);
	} else {
		(void)k_work_cancel_delayable(&ble_status_ping_work);
	}
}

static int wt_ble_advertising_start(void)
{
	int ret;

	if (!ble_ready) {
		return -ENODEV;
	}

	if (ble_connected_state || ble_advertising) {
		return 0;
	}

	ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ble_ad, ARRAY_SIZE(ble_ad),
				 ble_sd, ARRAY_SIZE(ble_sd));
	if (ret == -EALREADY) {
		ble_advertising = true;
		return 0;
	}

	if (ret) {
		LOG_ERR("BLE advertising start failed: %d", ret);
		return ret;
	}

	ble_advertising = true;
	LOG_INF("BLE advertising as %s", CONFIG_BT_DEVICE_NAME);
	return 0;
}

static void wt_ble_schedule_advertising_restart(void)
{
	if (ble_requested && ble_ready && !ble_connected_state) {
		k_work_schedule(&ble_adv_restart_work,
				K_MSEC(WT_BLE_ADV_RESTART_DELAY_MS));
	}
}

static void wt_ble_adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!ble_requested || !ble_ready || ble_connected_state || ble_advertising) {
		return;
	}

	(void)wt_ble_advertising_start();
}

static void ble_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("BLE connection failed: 0x%02x", err);
		ble_connected_state = false;
		ble_advertising = false;
		wt_ble_schedule_advertising_restart();
		return;
	}

	if (ble_conn) {
		bt_conn_unref(ble_conn);
	}
	ble_conn = bt_conn_ref(conn);
	ble_connected_state = true;
	ble_advertising = false;
	LOG_INF("BLE connected");
	wt_ble_status_reschedule();
}

static void ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("BLE disconnected: 0x%02x", reason);
	ble_connected_state = false;
	ble_tx_notify_enabled = false;
	ble_status_notify_enabled = false;
	ble_cmd_rsp_notify_enabled = false;
	wt_ble_status_reschedule();

	if (ble_conn) {
		bt_conn_unref(ble_conn);
		ble_conn = NULL;
	}

	ble_advertising = false;
	wt_ble_schedule_advertising_restart();
}

BT_CONN_CB_DEFINE(ble_conn_callbacks) = {
	.connected = ble_connected,
	.disconnected = ble_disconnected,
};

void wt_ble_init(void)
{
	k_work_init_delayable(&ble_status_ping_work, wt_ble_status_ping_work_handler);
	k_work_init_delayable(&ble_adv_restart_work, wt_ble_adv_restart_work_handler);
}

int wt_ble_service_start(void)
{
	int ret;

	ble_requested = true;

	if (!ble_ready) {
		ret = bt_enable(NULL);
		if (ret) {
			LOG_ERR("Bluetooth init failed: %d", ret);
			ble_requested = false;
			(void)k_work_cancel_delayable(&ble_adv_restart_work);
			return ret;
		}
		ble_ready = true;
		LOG_INF("Bluetooth initialized");
	}

	if (ble_connected_state || ble_advertising) {
		LOG_INF("BLE already active");
		return 0;
	}

	return wt_ble_advertising_start();
}

int wt_ble_service_stop(void)
{
	int ret = 0;

	ble_requested = false;

	if (ble_advertising) {
		ret = bt_le_adv_stop();
		if (ret) {
			LOG_WRN("BLE advertising stop failed: %d", ret);
		} else {
			ble_advertising = false;
		}
	}

	if (ble_conn) {
		ret = bt_conn_disconnect(ble_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (ret) {
			LOG_WRN("BLE disconnect failed: %d", ret);
		}
	}

	ble_connected_state = false;
	ble_tx_notify_enabled = false;
	ble_status_notify_enabled = false;
	ble_cmd_rsp_notify_enabled = false;
	wt_ble_status_reschedule();
	LOG_INF("BLE disabled");
	return ret;
}

int wt_ble_transmit_payload(const uint8_t *data, size_t len)
{
	if (!ble_ready) {
		return -ENODEV;
	}

	if (!ble_conn || !ble_connected_state) {
		return -ENOTCONN;
	}

	if (!ble_tx_notify_enabled) {
		return -EACCES;
	}

	return wt_ble_notify_chunked(&wt_ble_tx_svc.attrs[2], data, len);
}
#else
bool wt_ble_is_requested(void)
{
	return false;
}

bool wt_ble_is_ready(void)
{
	return false;
}

bool wt_ble_is_advertising(void)
{
	return false;
}

bool wt_ble_is_connected(void)
{
	return false;
}

bool wt_ble_tx_notify_is_enabled(void)
{
	return false;
}

bool wt_ble_status_notify_is_enabled(void)
{
	return false;
}

bool wt_ble_cmd_response_notify_is_enabled(void)
{
	return false;
}

void wt_ble_init(void)
{
}

int wt_ble_service_start(void)
{
	LOG_ERR("Bluetooth is not enabled in Kconfig");
	return -ENOTSUP;
}

int wt_ble_service_stop(void)
{
	return 0;
}

int wt_ble_transmit_payload(const uint8_t *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	return -ENOTSUP;
}
#endif
