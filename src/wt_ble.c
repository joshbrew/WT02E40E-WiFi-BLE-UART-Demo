#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
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
#include "wt_app.h"
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
static char ble_cmd_rsp_format_buf[WT_BLE_CMD_RSP_TEXT_MAX];
static char ble_current_req_id[24];
static char ble_name[WT_BLE_NAME_MAX + 1] = CONFIG_BT_DEVICE_NAME;
static struct k_work_delayable ble_status_ping_work;
static struct k_work_delayable ble_adv_restart_work;
static struct k_work_delayable ble_self_stop_work;
static struct k_work_delayable ble_cmd_work;
static char ble_cmd_work_buf[WT_BLE_CMD_TEXT_MAX];
static atomic_t ble_cmd_work_busy;

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
static void wt_ble_cmd_work_handler(struct k_work *work);
static void wt_ble_status_ping_work_handler(struct k_work *work);
static void wt_ble_status_reschedule(void);
static void wt_ble_adv_restart_work_handler(struct k_work *work);
static void wt_ble_self_stop_work_handler(struct k_work *work);
static int wt_ble_advertising_start(void);
static void wt_ble_schedule_advertising_restart(void);
static int wt_ble_schedule_self_stop_response(const char *rsp);
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

const char *wt_ble_name_get(void)
{
	return ble_name;
}

int wt_ble_name_set(const char *name)
{
	int ret = 0;
	bool was_advertising;

	if (!name || name[0] == '\0') {
		return -EINVAL;
	}

	if (strlen(name) > WT_BLE_NAME_MAX) {
		return -EMSGSIZE;
	}

	strncpy(ble_name, name, sizeof(ble_name) - 1);
	ble_name[sizeof(ble_name) - 1] = '\0';

#if defined(CONFIG_BT_DEVICE_NAME_DYNAMIC)
	if (ble_ready) {
		ret = bt_set_name(ble_name);
		if (ret) {
			LOG_WRN("bt_set_name failed: %d", ret);
		}
	}
#endif

	was_advertising = ble_advertising;
	if (was_advertising) {
		(void)bt_le_adv_stop();
		ble_advertising = false;
		(void)wt_ble_advertising_start();
	}

	LOG_INF("BLE name set to %s", ble_name);
	return ret;
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

	if (!atomic_cas(&ble_cmd_work_busy, 0, 1)) {
		if (offset == 0) {
			LOG_WRN("BLE command dropped while previous command is still running");
			return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
		}
	}

	strncpy(ble_cmd_work_buf, ble_cmd_buf, sizeof(ble_cmd_work_buf) - 1);
	ble_cmd_work_buf[sizeof(ble_cmd_work_buf) - 1] = '\0';
	LOG_INF("BLE command queued: %s", ble_cmd_work_buf);

	/*
	 * Some Web Bluetooth paths use offset writes / long-write style writes even for
	 * moderately small strings when the negotiated MTU is conservative. Do not run
	 * the command on the first fragment. Reschedule a short debounce so additional
	 * fragments can extend ble_cmd_buf before the command dispatcher executes.
	 */
	k_work_reschedule(&ble_cmd_work, K_MSEC(35));

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

static int wt_ble_notify_one_chunk(const struct bt_gatt_attr *attr,
					    const uint8_t *data, size_t len)
{
	int ret = 0;

	for (int attempt = 0; attempt <= WT_BLE_NOTIFY_RETRY_COUNT; attempt++) {
		ret = bt_gatt_notify(ble_conn, attr, data, len);
		if (!ret) {
			return 0;
		}

		if (ret != -EBUSY && ret != -ENOMEM && ret != -EAGAIN) {
			return ret;
		}

		k_msleep(WT_BLE_NOTIFY_RETRY_DELAY_MS);
	}

	return ret;
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

		ret = wt_ble_notify_one_chunk(attr, &data[offset], chunk_len);
		if (ret == -EMSGSIZE && chunk_max > 20) {
			chunk_max = 20;
			chunk_len = MIN(len - offset, chunk_max);
			ret = wt_ble_notify_one_chunk(attr, &data[offset], chunk_len);
		}

		if (ret) {
			return ret;
		}

		offset += chunk_len;
		if (offset < len) {
			k_msleep(WT_BLE_NOTIFY_INTER_CHUNK_DELAY_MS);
		}
	}

	return 0;
}

static int wt_ble_rsp_send(const char *fmt, ...)
{
	va_list args;
	int len;
	int notify_ret = 0;

	va_start(args, fmt);
	len = vsnprintk(ble_cmd_rsp_format_buf, sizeof(ble_cmd_rsp_format_buf), fmt, args);
	va_end(args);

	if (len < 0) {
		ble_cmd_rsp_buf[0] = '\0';
		return len;
	}

	if ((size_t)len >= sizeof(ble_cmd_rsp_format_buf)) {
		ble_cmd_rsp_format_buf[sizeof(ble_cmd_rsp_format_buf) - 1] = '\0';
		len = sizeof(ble_cmd_rsp_format_buf) - 1;
	}

	if (ble_current_req_id[0] != '\0' && ble_cmd_rsp_format_buf[0] != '#') {
		len = snprintk(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf),
		       "#%s %s", ble_current_req_id, ble_cmd_rsp_format_buf);
	} else {
		strncpy(ble_cmd_rsp_buf, ble_cmd_rsp_format_buf, sizeof(ble_cmd_rsp_buf) - 1);
		ble_cmd_rsp_buf[sizeof(ble_cmd_rsp_buf) - 1] = '\0';
		len = strlen(ble_cmd_rsp_buf);
	}

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


static int wt_ble_schedule_self_stop_response(const char *rsp)
{
	int ret;

	ret = wt_ble_rsp_send("%s", rsp);
	LOG_INF("BLE self-stop scheduled in %u ms", WT_BLE_SELF_STOP_DELAY_MS);
	k_work_schedule(&ble_self_stop_work, K_MSEC(WT_BLE_SELF_STOP_DELAY_MS));

	return ret;
}


static const struct bt_data ble_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
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
			"WT02E40E status: name=%s mode=%s ble=%s ready=%s conn=%s adv=%s tx_notify=%s status_notify=%s cmd_rsp_notify=%s wifi_req=%s wifi_assoc=%s ipv4=%s wifi_cmd=%s cmd_port=%u discovery=%s uptime=%llds",
			wt_ble_name_get(),
			mode,
			wt_onoff_txt(ble_requested),
			wt_onoff_txt(ble_ready),
			wt_onoff_txt(ble_connected_state),
			wt_onoff_txt(ble_advertising),
			wt_onoff_txt(ble_tx_notify_enabled),
			wt_onoff_txt(ble_status_notify_enabled),
			wt_onoff_txt(ble_cmd_rsp_notify_enabled),
			wt_onoff_txt(wt_wifi_is_requested()),
			wt_onoff_txt(wt_wifi_is_associated()),
			wt_onoff_txt(wt_wifi_has_ipv4()),
			wt_onoff_txt(wt_wifi_cmd_is_enabled()),
			wt_wifi_cmd_port(),
			wt_onoff_txt(wt_wifi_discovery_is_enabled()),
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


int wt_ble_status_format(char *buf, size_t size)
{
	return build_status_payload(buf, size);
}



static const char *wt_ble_skip_spaces_raw(const char *p)
{
	while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
		p++;
	}
	return p;
}

static const char *wt_ble_skip_token_raw(const char *p)
{
	p = wt_ble_skip_spaces_raw(p);
	while (p && *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
		p++;
	}
	return wt_ble_skip_spaces_raw(p);
}

static const char *wt_ble_raw_tx_tail(const char *line, size_t tokens_to_skip)
{
	const char *p = line;

	if (!p) {
		return NULL;
	}

	p = wt_ble_skip_spaces_raw(p);

	/* Optional request id prefix. */
	if (p && *p == '#') {
		p = wt_ble_skip_token_raw(p);
	}

	/* Optional UART-style prefix accepted by BLE command parser. */
	if (p && !strncmp(p, "wt", 2) && (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
		p = wt_ble_skip_token_raw(p);
	}

	for (size_t i = 0; i < tokens_to_skip; i++) {
		if (!p || !*p) {
			return NULL;
		}
		p = wt_ble_skip_token_raw(p);
	}

	if (!p || !*p) {
		return NULL;
	}

	return p;
}

static int wt_ble_build_payload_from_raw_tail(const char *tail, char *payload, size_t payload_size)
{
	char tail_buf[WT_BLE_CMD_TEXT_MAX];
	char *tail_argv[12];
	int tail_argc;

	if (!tail || !*tail) {
		return -EINVAL;
	}

	strncpy(tail_buf, tail, sizeof(tail_buf) - 1);
	tail_buf[sizeof(tail_buf) - 1] = '\0';

	tail_argc = wt_split_args_quoted(tail_buf, tail_argv, ARRAY_SIZE(tail_argv));
	if (tail_argc < 0) {
		return tail_argc;
	}
	if (tail_argc == 0) {
		return -EINVAL;
	}

	return wt_build_payload_from_argv(0, tail_argc, tail_argv, payload, payload_size);
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

static int wt_ble_command_tx(char **argv, size_t argc, const char *raw_line)
{
	int payload_len;
	int ret;
	char payload[WT_TX_PAYLOAD_MAX];

	if (argc < 2) {
		return wt_ble_rsp_send("usage tx ble <msg> | tx uart <msg> | tx wifi <ip> <port> <msg> | tx both <ip> <port> <msg>");
	}

	if (!strcmp(argv[1], "ble") || !strcmp(argv[1], "bt")) {
		if (argc >= 3) {
			payload_len = wt_build_payload_from_argv(2, argc, argv,
						 payload, sizeof(payload));
		} else {
			payload_len = wt_ble_build_payload_from_raw_tail(
				wt_ble_raw_tx_tail(raw_line, 2), payload, sizeof(payload));
		}
		if (payload_len < 0) {
			return wt_ble_rsp_send("err payload %d argc %u raw %.64s",
						payload_len, (unsigned int)argc, raw_line ? raw_line : "");
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
		if (argc >= 3) {
			payload_len = wt_ble_tx_uart_payload(argv, argc, 2);
		} else {
			payload_len = wt_ble_build_payload_from_raw_tail(
				wt_ble_raw_tx_tail(raw_line, 2), payload, sizeof(payload));
			if (payload_len > 0) {
				printk("BLE UART TX: %s\r\n", payload);
			}
		}
		if (payload_len < 0) {
			return wt_ble_rsp_send("err uart payload %d argc %u raw %.64s",
						payload_len, (unsigned int)argc, raw_line ? raw_line : "");
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

	return wt_ble_rsp_send("usage tx ble|uart|wifi|both ... argc %u subcmd %s raw %.64s", (unsigned int)argc, argc >= 2 ? argv[1] : "", raw_line ? raw_line : "");
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
		return wt_ble_rsp_send("usage wifi on|off|status|reconnect|scan|cmd|cred");
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_wifi_service_set(true);
		return ret ? wt_ble_rsp_send("err wifi on %d", ret) : wt_ble_rsp_send("ok wifi on");
	}

	if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		uint32_t delay_ms = 0;
		if (argc >= 3) {
			ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
			if (ret) {
				return wt_ble_rsp_send("err wifi off delay %d", ret);
			}
		}
		ret = wt_app_delayed_radio_apply("wifi_off", delay_ms);
		if (ret) {
			return wt_ble_rsp_send("err wifi off %d", ret);
		}
		return delay_ms ? wt_ble_rsp_send("ok wifi off scheduled %u ms", delay_ms) : wt_ble_rsp_send("ok wifi off");
	}

	if (!strcmp(argv[1], "status")) {
		(void)wt_app_wifi_status_format(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf), argc >= 3 && !strcmp(argv[2], "json"));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[1], "reconnect")) {
		ret = wt_wifi_reconnect_if_requested();
		if (!wt_wifi_is_requested()) {
			ret = wt_wifi_service_set(true);
		}
		return ret ? wt_ble_rsp_send("err wifi reconnect %d", ret) : wt_ble_rsp_send("ok wifi reconnect");
	}

	if (!strcmp(argv[1], "scan") || !strcmp(argv[1], "apscan") || !strcmp(argv[1], "networks")) {
		ret = wt_wifi_scan_command(argv, argc, ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		if (ret < 0) {
			return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
		}
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[1], "cmd") || !strcmp(argv[1], "command") || !strcmp(argv[1], "commands")) {
		if (argc < 3 || !strcmp(argv[2], "status")) {
			return wt_ble_rsp_send("ok wifi cmd %s port %u",
					      wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port());
		}
		if (!strcmp(argv[2], "on") || !strcmp(argv[2], "start")) {
			ret = wt_wifi_cmd_service_set(true);
			return ret ? wt_ble_rsp_send("err wifi cmd on %d", ret) :
				     wt_ble_rsp_send("ok wifi cmd on port %u", wt_wifi_cmd_port());
		}
		if (!strcmp(argv[2], "off") || !strcmp(argv[2], "stop")) {
			ret = wt_wifi_cmd_service_set(false);
			return ret ? wt_ble_rsp_send("err wifi cmd off %d", ret) : wt_ble_rsp_send("ok wifi cmd off");
		}
		if (!strcmp(argv[2], "port")) {
			uint16_t port;
			if (argc < 4) {
				return wt_ble_rsp_send("ok wifi cmd port %u", wt_wifi_cmd_port());
			}
			ret = wt_parse_udp_port(argv[3], &port);
			if (ret) {
				return wt_ble_rsp_send("err wifi cmd port %d", ret);
			}
			ret = wt_wifi_cmd_port_set(port);
			return ret ? wt_ble_rsp_send("err wifi cmd port set %d", ret) :
				     wt_ble_rsp_send("ok wifi cmd port %u", wt_wifi_cmd_port());
		}
		return wt_ble_rsp_send("usage wifi cmd on|off|status|port [port]");
	}

	if (!strcmp(argv[1], "cred") || !strcmp(argv[1], "creds")) {
		return wt_ble_command_wifi_cred(argv, argc);
	}

	return wt_ble_rsp_send("usage wifi on|off|status|reconnect|scan|cmd|cred");
}

static int wt_ble_command_execute(const char *line)
{
	char line_buf[WT_BLE_CMD_TEXT_MAX];
	char *argv_storage[16];
	char **argv = argv_storage;
	int argc;
	int ret;

	ble_current_req_id[0] = '\0';

	if (!line || strlen(line) == 0) {
		return wt_ble_rsp_send("err empty command");
	}

	strncpy(line_buf, line, sizeof(line_buf) - 1);
	line_buf[sizeof(line_buf) - 1] = '\0';

	argc = wt_split_args_quoted(line_buf, argv_storage, ARRAY_SIZE(argv_storage));
	if (argc < 0) {
		return wt_ble_rsp_send("err parse %d; check quotes/escapes", argc);
	}
	if (argc == 0) {
		return wt_ble_rsp_send("err empty command");
	}

	if (argv[0][0] == '#') {
		strncpy(ble_current_req_id, &argv[0][1], sizeof(ble_current_req_id) - 1);
		ble_current_req_id[sizeof(ble_current_req_id) - 1] = '\0';
		argv++;
		argc--;
		if (argc <= 0) {
			return wt_ble_rsp_send("err empty command after request id");
		}
	}

	if (!strcmp(argv[0], "wt")) {
		argv++;
		argc--;
		if (argc <= 0) {
			return wt_ble_rsp_send("usage wt status|mode|wifi|ble|tx|help");
		}
	}

	if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
		return wt_ble_rsp_send("cmds: id, version, fw, config, boot, status [json], mode [delay], wifi, ble, discovery, bridge, ping, tx, reboot");
	}

	if (!strcmp(argv[0], "id")) {
		(void)wt_app_id_format(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "version") || !strcmp(argv[0], "ver")) {
		(void)wt_app_version_format(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "fw")) {
		ret = wt_app_fw_command(argv, argc, ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "reboot")) {
		char *fw_argv[4];
		size_t fw_argc = 2;
		fw_argv[0] = "fw";
		fw_argv[1] = "reboot";
		if (argc >= 2) { fw_argv[2] = argv[1]; fw_argc = 3; }
		if (argc >= 3) { fw_argv[3] = argv[2]; fw_argc = 4; }
		ret = wt_app_fw_command(fw_argv, fw_argc, ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "boot")) {
		ret = wt_app_boot_command(argv, argc, ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "bridge")) {
		ret = wt_app_bridge_command(argv, argc, ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "ping")) {
		ret = wt_app_ping_execute(argv, argc, ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf));
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "config")) {
		bool json = argc >= 2 && !strcmp(argv[1], "json");
		if (argc >= 2 && !strcmp(argv[1], "save")) {
			ret = wt_app_config_save();
			return ret ? wt_ble_rsp_send("err config save %d", ret) : wt_ble_rsp_send("ok config saved");
		}
		if (argc >= 2 && !strcmp(argv[1], "reset")) {
			ret = wt_app_config_reset();
			return ret ? wt_ble_rsp_send("err config reset %d", ret) : wt_ble_rsp_send("ok config reset");
		}
		(void)wt_app_config_format(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf), json);
		return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
	}

	if (!strcmp(argv[0], "discovery") || !strcmp(argv[0], "discover")) {
		if (argc < 2 || !strcmp(argv[1], "status")) {
			return wt_ble_rsp_send("ok discovery %s port %u interval %ums", wt_onoff_txt(wt_wifi_discovery_is_enabled()), WT_WIFI_DISCOVERY_UDP_PORT, WT_WIFI_DISCOVERY_INTERVAL_MS);
		}
		if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
			ret = wt_wifi_discovery_service_set(true);
			return ret ? wt_ble_rsp_send("err discovery on %d", ret) : wt_ble_rsp_send("ok discovery on port %u", WT_WIFI_DISCOVERY_UDP_PORT);
		}
		if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
			ret = wt_wifi_discovery_service_set(false);
			return ret ? wt_ble_rsp_send("err discovery off %d", ret) : wt_ble_rsp_send("ok discovery off");
		}
		return wt_ble_rsp_send("usage discovery on|off|status");
	}

	if (!strcmp(argv[0], "name")) {
		if (argc < 2 || !strcmp(argv[1], "get")) {
			return wt_ble_rsp_send("ok name %s", wt_ble_name_get());
		}
		if (!strcmp(argv[1], "set")) {
			if (argc < 3) {
				return wt_ble_rsp_send("usage name set <ble-name>");
			}
			ret = wt_ble_name_set(argv[2]);
			return ret ? wt_ble_rsp_send("err name set %d", ret) : wt_ble_rsp_send("ok name %s", wt_ble_name_get());
		}
		return wt_ble_rsp_send("usage name get|set <ble-name>");
	}

	if (!strcmp(argv[0], "status")) {
		if (argc >= 2 && !strcmp(argv[1], "json")) {
			(void)wt_app_config_format(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf), true);
			return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
		}
		(void)build_status_payload(ble_status_buf, sizeof(ble_status_buf));
		return wt_ble_rsp_send("%s", ble_status_buf);
	}

	if (!strcmp(argv[0], "mode")) {
		uint32_t delay_ms = 0;

		if (argc < 2) {
			return wt_ble_rsp_send("usage mode idle|ble|wifi|both [delay]");
		}

		if (argc >= 3) {
			ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
			if (ret) {
				return wt_ble_rsp_send("err mode delay %d", ret);
			}
		}

		if (delay_ms > 0) {
			ret = wt_app_delayed_radio_apply(argv[1], delay_ms);
			return ret ? wt_ble_rsp_send("err mode %d", ret) :
				     wt_ble_rsp_send("ok mode %s scheduled %u ms", argv[1], delay_ms);
		}

		/* If a BLE-origin command turns BLE off, answer first and then drop the link. */
		if (!strcmp(argv[1], "idle") || !strcmp(argv[1], "off")) {
			ret = wt_wifi_service_set(false);
			if (ret) {
				return wt_ble_rsp_send("err wifi off %d", ret);
			}
			return wt_ble_schedule_self_stop_response("ok mode idle; BLE disconnecting");
		}

		if (!strcmp(argv[1], "wifi") || !strcmp(argv[1], "wi-fi")) {
			ret = wt_wifi_service_set(true);
			if (ret) {
				return wt_ble_rsp_send("err wifi on %d", ret);
			}
			return wt_ble_schedule_self_stop_response("ok mode wifi; BLE disconnecting");
		}

		ret = wt_radio_mode_apply(argv[1]);
		return ret ? wt_ble_rsp_send("err mode %d", ret) : wt_ble_rsp_send("ok mode %s", argv[1]);
	}

	if (!strcmp(argv[0], "wifi")) {
		return wt_ble_command_wifi(argv, argc);
	}

	if (!strcmp(argv[0], "ble") || !strcmp(argv[0], "bt")) {
		if (argc < 2) {
			return wt_ble_rsp_send("usage ble on|off|status|name [get|set <name>]");
		}
		if (!strcmp(argv[1], "name")) {
			if (argc < 3 || !strcmp(argv[2], "get")) {
				return wt_ble_rsp_send("ok ble name %s", wt_ble_name_get());
			}
			if (!strcmp(argv[2], "set")) {
				if (argc < 4) {
					return wt_ble_rsp_send("usage ble name set <name>");
				}
				ret = wt_ble_name_set(argv[3]);
				return ret ? wt_ble_rsp_send("err ble name set %d", ret) : wt_ble_rsp_send("ok ble name %s", wt_ble_name_get());
			}
			ret = wt_ble_name_set(argv[2]);
			return ret ? wt_ble_rsp_send("err ble name set %d", ret) : wt_ble_rsp_send("ok ble name %s", wt_ble_name_get());
		}
		if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
			ret = wt_ble_service_start();
			return ret ? wt_ble_rsp_send("err ble on %d", ret) : wt_ble_rsp_send("ok ble on");
		}
		if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
			uint32_t delay_ms = 0;
			if (argc >= 3) {
				ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
				if (ret) {
					return wt_ble_rsp_send("err ble off delay %d", ret);
				}
			}
			if (delay_ms > 0) {
				ret = wt_app_delayed_radio_apply("ble_off", delay_ms);
				return ret ? wt_ble_rsp_send("err ble off %d", ret) :
					     wt_ble_rsp_send("ok ble off scheduled %u ms", delay_ms);
			}
			return wt_ble_schedule_self_stop_response("ok ble off; disconnecting");
		}
		if (!strcmp(argv[1], "status")) {
			(void)wt_app_ble_status_format(ble_cmd_rsp_buf, sizeof(ble_cmd_rsp_buf), argc >= 3 && !strcmp(argv[2], "json"));
			return wt_ble_rsp_send("%s", ble_cmd_rsp_buf);
		}
		return wt_ble_rsp_send("usage ble on|off|status|name [get|set <name>]");
	}

	if (!strcmp(argv[0], "tx")) {
		return wt_ble_command_tx(argv, argc, line);
	}

	return wt_ble_rsp_send("err unknown command: %s", argv[0]);
}

static void wt_ble_cmd_work_handler(struct k_work *work)
{
	int ret;
	char cmd[WT_BLE_CMD_TEXT_MAX];

	ARG_UNUSED(work);

	strncpy(cmd, ble_cmd_work_buf, sizeof(cmd) - 1);
	cmd[sizeof(cmd) - 1] = '\0';

	LOG_INF("BLE command executing: %s", cmd);
	ret = wt_ble_command_execute(cmd);
	if (ret) {
		LOG_WRN("BLE command failed: %d", ret);
	}

	atomic_clear(&ble_cmd_work_busy);
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

	const char *name = wt_ble_name_get();
	struct bt_data ble_sd[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)),
	};

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
	LOG_INF("BLE advertising as %s", wt_ble_name_get());
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

static void wt_ble_self_stop_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)wt_ble_service_stop();
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
	k_work_init_delayable(&ble_self_stop_work, wt_ble_self_stop_work_handler);
	k_work_init_delayable(&ble_cmd_work, wt_ble_cmd_work_handler);
	atomic_clear(&ble_cmd_work_busy);
}

int wt_ble_service_start(void)
{
	int ret;

	(void)k_work_cancel_delayable(&ble_self_stop_work);
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
#if defined(CONFIG_BT_DEVICE_NAME_DYNAMIC)
		(void)bt_set_name(ble_name);
#endif
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

const char *wt_ble_name_get(void)
{
	return CONFIG_BT_DEVICE_NAME;
}

int wt_ble_name_set(const char *name)
{
	ARG_UNUSED(name);
	return -ENOTSUP;
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
