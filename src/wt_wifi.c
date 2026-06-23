#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wt_wifi, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_WIFI_READY_LIB
#include <net/wifi_ready.h>
#endif

#if defined(CONFIG_BOARD_NRF7002DK_NRF5340_CPUAPP_NRF7001) || \
	defined(CONFIG_BOARD_NRF7002DK_NRF5340_CPUAPP)
#include <zephyr/drivers/wifi/nrf_wifi/bus/qspi_if.h>
#endif

#include "net_private.h"

#include "wt_ble.h"
#include "wt_app.h"
#include "wt_common.h"
#include "wt_config.h"
#include "wt_leds.h"
#include "wt_radio.h"
#include "wt_stream.h"
#include "wt_wifi.h"

#define WIFI_NODE DT_CHOSEN(zephyr_wifi)
static const uint8_t wifi_mac_addr[6] = DT_PROP_OR(WIFI_NODE, local_mac_address, { 0 });

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback wifi_scan_mgmt_cb;
static struct net_mgmt_event_callback net_mgmt_cb;
static K_SEM_DEFINE(wifi_control_sem, 0, 1);

#ifdef CONFIG_WIFI_READY_LIB
static K_SEM_DEFINE(wifi_ready_state_changed_sem, 0, 1);
static bool wifi_ready_status;
#endif

static volatile bool wifi_requested;
static bool wifi_prepared;
static volatile bool wifi_cmd_enabled;
static volatile uint16_t wifi_cmd_port_current = WT_WIFI_CMD_UDP_PORT;
static volatile bool wifi_discovery_enabled;

static int wifi_prepare_once(void);
static int wt_wifi_cmd_rsp(char *rsp, size_t rsp_len, const char *fmt, ...);
static int wifi_cmd_socket_open(uint16_t port);
static void wifi_cmd_thread(void *p1, void *p2, void *p3);
static void wifi_discovery_thread(void *p1, void *p2, void *p3);
static void wifi_scan_event_handler(struct net_mgmt_event_callback *cb,
					    uint64_t mgmt_event, struct net_if *iface);

static struct {
	union {
		struct {
			uint8_t connected : 1;
			uint8_t connect_result : 1;
			uint8_t disconnect_requested : 1;
			uint8_t ipv4_bound : 1;
			uint8_t _unused : 4;
		};
		uint8_t all;
	};
} wifi_context;

static char wifi_ipv4_text[sizeof("255.255.255.255")] = "0.0.0.0";


struct wt_wifi_scan_entry {
	bool used;
	char ssid[WIFI_SSID_MAX_LEN + 1];
	uint8_t ssid_len;
	uint8_t band;
	uint8_t channel;
	enum wifi_security_type security;
	enum wifi_mfp_options mfp;
	int8_t rssi;
	uint8_t mac[WIFI_MAC_ADDR_LEN];
	uint8_t mac_len;
};

static struct wt_wifi_scan_entry wifi_scan_results[WT_WIFI_SCAN_MAX_RESULTS];
static volatile bool wifi_scan_running;
static volatile bool wifi_scan_valid;
static int wifi_scan_result_count;
static int wifi_scan_last_status;
static int64_t wifi_scan_started_ms;
static int64_t wifi_scan_finished_ms;
static K_SEM_DEFINE(wifi_scan_done_sem, 0, 1);
static K_MUTEX_DEFINE(wifi_scan_lock);

bool wt_wifi_is_requested(void)
{
	return wifi_requested;
}

bool wt_wifi_is_associated(void)
{
	return wifi_context.connected;
}

bool wt_wifi_has_ipv4(void)
{
	return wifi_context.ipv4_bound;
}

int wt_wifi_ipv4_get(char *buf, size_t size)
{
	if (!buf || size == 0) {
		return -EINVAL;
	}

	snprintk(buf, size, "%s", wifi_context.ipv4_bound ? wifi_ipv4_text : "0.0.0.0");
	return 0;
}

int wt_wifi_mac_get(char *buf, size_t size)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct net_linkaddr *linkaddr;

	if (!buf || size == 0) {
		return -EINVAL;
	}

	if (!iface) {
		snprintk(buf, size, "00:00:00:00:00:00");
		return -ENODEV;
	}

	linkaddr = net_if_get_link_addr(iface);
	if (!linkaddr || linkaddr->len == 0) {
		snprintk(buf, size, "00:00:00:00:00:00");
		return -ENODEV;
	}

	net_sprint_ll_addr_buf(linkaddr->addr, linkaddr->len, (uint8_t *)buf, size);
	return 0;
}

int wt_wifi_cmd_service_set(bool enable)
{
	wifi_cmd_enabled = enable;
	LOG_INF("Wi-Fi UDP command server %s on port %u",
		wt_onoff_txt(enable), wt_wifi_cmd_port());
	return 0;
}

bool wt_wifi_cmd_is_enabled(void)
{
	return wifi_cmd_enabled;
}

uint16_t wt_wifi_cmd_port(void)
{
	return wifi_cmd_port_current;
}

int wt_wifi_cmd_port_set(uint16_t port)
{
	if (port == 0) {
		return -EINVAL;
	}

	if (wifi_cmd_port_current == port) {
		return 0;
	}

	wifi_cmd_port_current = port;
	LOG_INF("Wi-Fi UDP command server rebinding to port %u", port);
	return 0;
}

int wt_wifi_discovery_service_set(bool enable)
{
	wifi_discovery_enabled = enable;
	LOG_INF("Wi-Fi UDP discovery beacon %s on port %u",
		wt_onoff_txt(enable), WT_WIFI_DISCOVERY_UDP_PORT);
	return 0;
}

bool wt_wifi_discovery_is_enabled(void)
{
	return wifi_discovery_enabled;
}


bool wt_wifi_scan_is_running(void)
{
	return wifi_scan_running;
}

int wt_wifi_scan_count(void)
{
	return wifi_scan_result_count;
}

static void wt_wifi_scan_enable_support_services(void)
{
	if (!wt_wifi_is_requested()) {
		(void)wt_wifi_service_set(true);
	}

	if (!wt_wifi_cmd_is_enabled()) {
		(void)wt_wifi_cmd_service_set(true);
	}

	if (!wt_wifi_discovery_is_enabled()) {
		(void)wt_wifi_discovery_service_set(true);
	}
}

static const char *wifi_scan_band_label(uint8_t band)
{
	return wifi_band_txt((enum wifi_frequency_bands)band);
}

static const char *wifi_scan_security_label(enum wifi_security_type security)
{
	return wifi_security_txt(security);
}

static size_t wt_wifi_json_escape(char *dst, size_t dst_len, const char *src)
{
	size_t off = 0;

	if (!dst || dst_len == 0) {
		return 0;
	}

	if (!src) {
		dst[0] = '\0';
		return 0;
	}

	for (size_t i = 0; src[i] != '\0' && off + 1 < dst_len; i++) {
		char c = src[i];

		if ((c == '"' || c == '\\') && off + 2 < dst_len) {
			dst[off++] = '\\';
			dst[off++] = c;
		} else if (c == '\n' && off + 2 < dst_len) {
			dst[off++] = '\\';
			dst[off++] = 'n';
		} else if (c == '\r' && off + 2 < dst_len) {
			dst[off++] = '\\';
			dst[off++] = 'r';
		} else if (c == '\t' && off + 2 < dst_len) {
			dst[off++] = '\\';
			dst[off++] = 't';
		} else if ((unsigned char)c < 0x20) {
			/* Drop other control chars in bring-up UI strings. */
			continue;
		} else {
			dst[off++] = c;
		}
	}

	dst[off] = '\0';
	return off;
}

static void wt_wifi_scan_clear_locked(void)
{
	memset(wifi_scan_results, 0, sizeof(wifi_scan_results));
	wifi_scan_result_count = 0;
	wifi_scan_last_status = 0;
	wifi_scan_valid = false;
	wifi_scan_finished_ms = 0;
	wifi_scan_started_ms = 0;
}

static int wt_wifi_scan_find_by_ssid(const char *ssid)
{
	for (int i = 0; i < wifi_scan_result_count; i++) {
		if (wifi_scan_results[i].used && !strcmp(wifi_scan_results[i].ssid, ssid)) {
			return i;
		}
	}

	return -1;
}

static void wt_wifi_scan_sort_locked(void)
{
	for (int i = 0; i < wifi_scan_result_count; i++) {
		for (int j = i + 1; j < wifi_scan_result_count; j++) {
			if (wifi_scan_results[j].rssi > wifi_scan_results[i].rssi) {
				struct wt_wifi_scan_entry tmp = wifi_scan_results[i];
				wifi_scan_results[i] = wifi_scan_results[j];
				wifi_scan_results[j] = tmp;
			}
		}
	}
}

static void wt_wifi_scan_store_result(const struct wifi_scan_result *entry)
{
	char ssid[WIFI_SSID_MAX_LEN + 1];
	size_t ssid_len;
	int slot;

	if (!entry || entry->ssid_length == 0) {
		return;
	}

	ssid_len = MIN((size_t)entry->ssid_length, (size_t)WIFI_SSID_MAX_LEN);
	memcpy(ssid, entry->ssid, ssid_len);
	ssid[ssid_len] = '\0';

	if (k_mutex_lock(&wifi_scan_lock, K_MSEC(20)) != 0) {
		return;
	}

	slot = wt_wifi_scan_find_by_ssid(ssid);
	if (slot >= 0) {
		if (entry->rssi <= wifi_scan_results[slot].rssi) {
			k_mutex_unlock(&wifi_scan_lock);
			return;
		}
	} else {
		if (wifi_scan_result_count >= WT_WIFI_SCAN_MAX_RESULTS) {
			/* Keep top RSSI entries only. Replace the weakest if this one is better. */
			slot = wifi_scan_result_count - 1;
			wt_wifi_scan_sort_locked();
			if (entry->rssi <= wifi_scan_results[slot].rssi) {
				k_mutex_unlock(&wifi_scan_lock);
				return;
			}
		} else {
			slot = wifi_scan_result_count++;
		}
	}

	memset(&wifi_scan_results[slot], 0, sizeof(wifi_scan_results[slot]));
	wifi_scan_results[slot].used = true;
	strncpy(wifi_scan_results[slot].ssid, ssid, sizeof(wifi_scan_results[slot].ssid) - 1);
	wifi_scan_results[slot].ssid_len = ssid_len;
	wifi_scan_results[slot].band = entry->band;
	wifi_scan_results[slot].channel = entry->channel;
	wifi_scan_results[slot].security = entry->security;
	wifi_scan_results[slot].mfp = entry->mfp;
	wifi_scan_results[slot].rssi = entry->rssi;
	wifi_scan_results[slot].mac_len = MIN((uint8_t)WIFI_MAC_ADDR_LEN, entry->mac_length);
	memcpy(wifi_scan_results[slot].mac, entry->mac, wifi_scan_results[slot].mac_len);
	wt_wifi_scan_sort_locked();

	k_mutex_unlock(&wifi_scan_lock);
}

static int wt_wifi_scan_start(void)
{
	struct net_if *iface;
	struct wifi_scan_params params = { 0 };
	int ret;

	if (wifi_scan_running) {
		return -EALREADY;
	}

	wt_wifi_scan_enable_support_services();

	iface = net_if_get_first_wifi();
	if (!iface) {
		return -ENODEV;
	}

	ret = wifi_prepare_once();
	if (ret) {
		return ret;
	}

	k_mutex_lock(&wifi_scan_lock, K_FOREVER);
	wt_wifi_scan_clear_locked();
	wifi_scan_running = true;
	wifi_scan_last_status = -EINPROGRESS;
	wifi_scan_started_ms = k_uptime_get();
	k_sem_reset(&wifi_scan_done_sem);
	k_mutex_unlock(&wifi_scan_lock);

	params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
	params.bands = BIT(WIFI_FREQ_BAND_2_4_GHZ) | BIT(WIFI_FREQ_BAND_5_GHZ);
	params.dwell_time_active = 80;
	params.dwell_time_passive = 120;
	params.max_bss_cnt = WT_WIFI_SCAN_MAX_RESULTS;

	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
	if (ret) {
		k_mutex_lock(&wifi_scan_lock, K_FOREVER);
		wifi_scan_running = false;
		wifi_scan_last_status = ret;
		wifi_scan_finished_ms = k_uptime_get();
		k_mutex_unlock(&wifi_scan_lock);
		return ret;
	}

	return 0;
}

static int wt_wifi_scan_format_len(char *buf, size_t size, int len)
{
	if (!buf || size == 0) {
		return -EINVAL;
	}
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

static int wt_wifi_scan_format_summary(char *buf, size_t size, bool json)
{
	int64_t age_s = 0;
	bool running;
	bool valid;
	int count;
	int status;
	int len;

	if (!buf || size == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&wifi_scan_lock, K_FOREVER);
	if (wifi_scan_finished_ms > 0) {
		age_s = (k_uptime_get() - wifi_scan_finished_ms) / 1000;
	}
	running = wifi_scan_running;
	valid = wifi_scan_valid;
	count = wifi_scan_result_count;
	status = wifi_scan_last_status;
	k_mutex_unlock(&wifi_scan_lock);

	if (json) {
		len = snprintk(buf, size,
			"{\"type\":\"wifi_scan\",\"running\":%s,\"valid\":%s,\"count\":%d,\"status\":%d,\"age_s\":%lld,\"results\":[]}",
			running ? "true" : "false",
			valid ? "true" : "false",
			count, status, (long long)age_s);
	} else {
		len = snprintk(buf, size,
			"ok wifi scan running=%s valid=%s count=%d status=%d age=%llds; use wifi scan item <n> [json]",
			running ? "on" : "off", valid ? "yes" : "no",
			count, status, (long long)age_s);
	}

	return wt_wifi_scan_format_len(buf, size, len);
}

static int wt_wifi_scan_format_item(int index, char *buf, size_t size, bool json)
{
	struct wt_wifi_scan_entry entry;
	char bssid[sizeof("xx:xx:xx:xx:xx:xx")];
	char esc_ssid[(WIFI_SSID_MAX_LEN * 2) + 1];
	bool found = false;
	int len;

	if (!buf || size == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&wifi_scan_lock, K_FOREVER);
	if (index >= 1 && index <= wifi_scan_result_count && wifi_scan_results[index - 1].used) {
		entry = wifi_scan_results[index - 1];
		found = true;
	}
	k_mutex_unlock(&wifi_scan_lock);

	if (!found) {
		if (json) {
			len = snprintk(buf, size,
				"{\"type\":\"wifi_scan_item\",\"i\":%d,\"error\":%d}",
				index, -ENOENT);
		} else {
			len = snprintk(buf, size, "err wifi scan item %d", -ENOENT);
		}
		return wt_wifi_scan_format_len(buf, size, len);
	}

	bssid[0] = '\0';
	if (entry.mac_len) {
		net_sprint_ll_addr_buf(entry.mac, WIFI_MAC_ADDR_LEN,
				       (uint8_t *)bssid, sizeof(bssid));
	}
	if (bssid[0] == '\0') {
		snprintk(bssid, sizeof(bssid), "?");
	}

	wt_wifi_json_escape(esc_ssid, sizeof(esc_ssid), entry.ssid);

	if (json) {
		len = snprintk(buf, size,
			"{\"type\":\"wifi_scan_item\",\"i\":%d,\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"band\":\"%s\",\"security\":\"%s\",\"bssid\":\"%s\"}",
			index, esc_ssid, entry.rssi, entry.channel,
			wifi_scan_band_label(entry.band), wifi_scan_security_label(entry.security), bssid);
	} else {
		len = snprintk(buf, size,
			"ok wifi scan item %d ssid=\"%s\" rssi=%d ch=%u band=%s sec=%s bssid=%s",
			index, entry.ssid, entry.rssi, entry.channel,
			wifi_scan_band_label(entry.band), wifi_scan_security_label(entry.security), bssid);
	}

	return wt_wifi_scan_format_len(buf, size, len);
}

int wt_wifi_scan_format_results(char *buf, size_t size, bool json)
{
	return wt_wifi_scan_format_summary(buf, size, json);
}

int wt_wifi_scan_stream_json(wt_wifi_stream_emit_fn emit, void *user)
{
	char piece[WT_WIFI_CMD_RSP_TEXT_MAX];
	int64_t age_s = 0;
	bool running;
	bool valid;
	int count;
	int status;
	int len;
	int ret;

	if (!emit) {
		return -EINVAL;
	}

	k_mutex_lock(&wifi_scan_lock, K_FOREVER);
	if (wifi_scan_finished_ms > 0) {
		age_s = (k_uptime_get() - wifi_scan_finished_ms) / 1000;
	}
	running = wifi_scan_running;
	valid = wifi_scan_valid;
	count = wifi_scan_result_count;
	status = wifi_scan_last_status;
	k_mutex_unlock(&wifi_scan_lock);

	len = snprintk(piece, sizeof(piece),
		"{\"type\":\"wifi_scan\",\"running\":%s,\"valid\":%s,\"count\":%d,\"status\":%d,\"age_s\":%lld,\"results\":[",
		running ? "true" : "false",
		valid ? "true" : "false",
		count, status, (long long)age_s);
	if (len < 0) {
		return len;
	}
	ret = emit(piece, strlen(piece), user);
	if (ret) {
		return ret;
	}

	for (int i = 1; i <= count && i <= WT_WIFI_SCAN_MAX_RESULTS; i++) {
		if (i > 1) {
			ret = emit(",", 1, user);
			if (ret) {
				return ret;
			}
		}

		len = wt_wifi_scan_format_item(i, piece, sizeof(piece), true);
		if (len < 0) {
			return len;
		}

		ret = emit(piece, strlen(piece), user);
		if (ret) {
			return ret;
		}
	}

	return emit("]}", 2, user);
}

static int wt_wifi_scan_get_ssid(int index, char *ssid, size_t ssid_size,
				 enum wifi_security_type *security)
{
	if (!ssid || ssid_size == 0 || index < 1 || index > WT_WIFI_SCAN_MAX_RESULTS) {
		return -EINVAL;
	}

	k_mutex_lock(&wifi_scan_lock, K_FOREVER);
	if (index > wifi_scan_result_count || !wifi_scan_results[index - 1].used) {
		k_mutex_unlock(&wifi_scan_lock);
		return -ENOENT;
	}

	strncpy(ssid, wifi_scan_results[index - 1].ssid, ssid_size - 1);
	ssid[ssid_size - 1] = '\0';
	if (security) {
		*security = wifi_scan_results[index - 1].security;
	}
	k_mutex_unlock(&wifi_scan_lock);
	return 0;
}

int wt_wifi_scan_command(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	bool json = false;
	int ret;

	if (!rsp || rsp_len == 0) {
		return -EINVAL;
	}

	if (argc < 3) {
		ret = wt_wifi_scan_start();
		if (ret && ret != -EALREADY) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan start %d", ret);
		}
		ret = k_sem_take(&wifi_scan_done_sem, K_MSEC(WT_WIFI_SCAN_TIMEOUT_MS));
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi scan started running=%s", wifi_scan_running ? "on" : "off");
		}
		return wt_wifi_scan_format_results(rsp, rsp_len, false);
	}

	if (!strcmp(argv[2], "json")) {
		json = true;
		ret = wt_wifi_scan_start();
		if (ret && ret != -EALREADY) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "{\"type\":\"wifi_scan\",\"error\":%d}", ret);
		}
		ret = k_sem_take(&wifi_scan_done_sem, K_MSEC(WT_WIFI_SCAN_TIMEOUT_MS));
		if (ret) {
			return wt_wifi_scan_format_results(rsp, rsp_len, true);
		}
		return wt_wifi_scan_format_results(rsp, rsp_len, true);
	}

	if (!strcmp(argv[2], "full") || !strcmp(argv[2], "stream")) {
		ret = wt_wifi_scan_start();
		if (ret && ret != -EALREADY) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan start %d", ret);
		}
		(void)k_sem_take(&wifi_scan_done_sem, K_MSEC(WT_WIFI_SCAN_TIMEOUT_MS));
		return wt_wifi_cmd_rsp(rsp, rsp_len,
				      "ok wifi scan full stream use BLE response notify or Wi-Fi UDP command; count=%d",
				      wifi_scan_result_count);
	}

	if (!strcmp(argv[2], "start") || !strcmp(argv[2], "now")) {
		ret = wt_wifi_scan_start();
		if (ret == -EALREADY) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi scan already running");
		}
		return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan start %d", ret) :
			     wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi scan started");
	}

	if (!strcmp(argv[2], "wait")) {
		json = argc >= 4 && !strcmp(argv[3], "json");
		ret = wt_wifi_scan_start();
		if (ret && ret != -EALREADY) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan start %d", ret);
		}
		(void)k_sem_take(&wifi_scan_done_sem, K_MSEC(WT_WIFI_SCAN_TIMEOUT_MS));
		return wt_wifi_scan_format_results(rsp, rsp_len, json);
	}

	if (!strcmp(argv[2], "last") || !strcmp(argv[2], "results")) {
		json = argc >= 4 && !strcmp(argv[3], "json");
		return wt_wifi_scan_format_results(rsp, rsp_len, json);
	}

	if (!strcmp(argv[2], "item") || !strcmp(argv[2], "ap")) {
		int index;
		char *end;
		if (argc < 4) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi scan item <index> [json]");
		}
		index = strtol(argv[3], &end, 10);
		if (*end != '\0') {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan index");
		}
		json = argc >= 5 && !strcmp(argv[4], "json");
		return wt_wifi_scan_format_item(index, rsp, rsp_len, json);
	}

	if (!strcmp(argv[2], "status")) {
		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi scan running=%s valid=%s count=%d last_status=%d",
				      wifi_scan_running ? "on" : "off", wifi_scan_valid ? "yes" : "no",
				      wifi_scan_result_count, wifi_scan_last_status);
	}

	if (!strcmp(argv[2], "clear")) {
		k_mutex_lock(&wifi_scan_lock, K_FOREVER);
		wt_wifi_scan_clear_locked();
		k_mutex_unlock(&wifi_scan_lock);
		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi scan cleared");
	}

	if (!strcmp(argv[2], "open")) {
		char ssid[WIFI_SSID_MAX_LEN + 1];
		int index;
		char *end;
		if (argc < 4) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi scan open <index>");
		}
		index = strtol(argv[3], &end, 10);
		if (*end != '\0') {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan index");
		}
		ret = wt_wifi_scan_get_ssid(index, ssid, sizeof(ssid), NULL);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan ssid %d", ret);
		}
		ret = wt_wifi_credentials_open(ssid, true);
		if (!ret) {
			(void)wt_wifi_reconnect_if_requested();
		}
		return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan open %d", ret) :
			     wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi scan open %d ssid %s", index, ssid);
	}

	if (!strcmp(argv[2], "connect") || !strcmp(argv[2], "set")) {
		char ssid[WIFI_SSID_MAX_LEN + 1];
		enum wifi_security_type scanned_security = WIFI_SECURITY_TYPE_UNKNOWN;
		const char *security_text = argc >= 6 ? argv[5] : NULL;
		int index;
		char *end;
		if (argc < 5) {
			return wt_wifi_cmd_rsp(rsp, rsp_len,
					      "usage wifi scan connect <index> <password> [wpa2|auto|wpa3]");
		}
		index = strtol(argv[3], &end, 10);
		if (*end != '\0') {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan index");
		}
		ret = wt_wifi_scan_get_ssid(index, ssid, sizeof(ssid), &scanned_security);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan ssid %d", ret);
		}
		if (scanned_security == WIFI_SECURITY_TYPE_NONE && strlen(argv[4]) == 0) {
			ret = wt_wifi_credentials_open(ssid, true);
		} else {
			ret = wt_wifi_credentials_set(ssid, argv[4], security_text, true);
		}
		if (!ret) {
			(void)wt_wifi_service_set(true);
			(void)wt_wifi_reconnect_if_requested();
		}
		return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi scan connect %d", ret) :
			     wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi scan connect %d ssid %s", index, ssid);
	}

	return wt_wifi_cmd_rsp(rsp, rsp_len,
			      "usage wifi scan [json|full json|start|wait [json]|last [json]|item <index> [json]|status|clear|connect <index> <password> [security]|open <index>]");
}

static const char *wifi_security_name(enum wifi_security_type security)
{
	switch (security) {
	case WIFI_SECURITY_TYPE_NONE:
		return "open";
	case WIFI_SECURITY_TYPE_PSK:
		return "wpa2-psk";
	case WIFI_SECURITY_TYPE_SAE:
		return "wpa3-sae";
	case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
		return "wpa-auto-personal";
	default:
		return "other";
	}
}

static int parse_wifi_security(const char *text, enum wifi_security_type *security)
{
	if (!text || !strcmp(text, "wpa2") || !strcmp(text, "psk")) {
		*security = WIFI_SECURITY_TYPE_PSK;
		return 0;
	}

	if (!strcmp(text, "auto") || !strcmp(text, "wpa-auto") || !strcmp(text, "wpa")) {
		*security = WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL;
		return 0;
	}

	if (!strcmp(text, "wpa3") || !strcmp(text, "sae")) {
		*security = WIFI_SECURITY_TYPE_SAE;
		return 0;
	}

	if (!strcmp(text, "open") || !strcmp(text, "none")) {
		*security = WIFI_SECURITY_TYPE_NONE;
		return 0;
	}

	return -EINVAL;
}

static int wifi_credential_store(const char *ssid, const char *password,
					 enum wifi_security_type security, bool replace_all)
{
	size_t ssid_len = strlen(ssid);
	size_t password_len = password ? strlen(password) : 0;
	uint32_t flags = WIFI_CREDENTIALS_FLAG_FAVORITE |
			 WIFI_CREDENTIALS_FLAG_2_4GHz |
			 WIFI_CREDENTIALS_FLAG_5GHz;
	int ret;

	if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN) {
		LOG_ERR("SSID length must be 1..%d", WIFI_SSID_MAX_LEN);
		return -EINVAL;
	}

	if (security != WIFI_SECURITY_TYPE_NONE && password_len == 0) {
		LOG_ERR("Password required for %s", wifi_security_name(security));
		return -EINVAL;
	}

	if (password_len > WIFI_CREDENTIALS_MAX_PASSWORD_LEN) {
		LOG_ERR("Password length must be <= %d", WIFI_CREDENTIALS_MAX_PASSWORD_LEN);
		return -EINVAL;
	}

	if (replace_all) {
		ret = wifi_credentials_delete_all();
		if (ret) {
			LOG_WRN("Credential clear before set failed: %d", ret);
		}
	}

	ret = wifi_credentials_set_personal(ssid, ssid_len, security,
					      NULL, 0, password, password_len,
					      flags, 0, WT_WIFI_CRED_TIMEOUT_SEC);
	if (ret) {
		LOG_ERR("Failed to store Wi-Fi credentials: %d", ret);
		return ret;
	}

	LOG_INF("Stored Wi-Fi credentials for SSID '%s' (%s)",
		ssid, wifi_security_name(security));
	return 0;
}

struct wifi_credential_list_ctx {
	const struct shell *sh;
	int count;
};

static void wifi_credential_list_cb(void *cb_arg, const char *ssid, size_t ssid_len)
{
	struct wifi_credential_list_ctx *ctx = cb_arg;
	char ssid_buf[WIFI_SSID_MAX_LEN + 1];
	size_t copy_len = MIN(ssid_len, (size_t)WIFI_SSID_MAX_LEN);

	memcpy(ssid_buf, ssid, copy_len);
	ssid_buf[copy_len] = '\0';
	ctx->count++;
	shell_print(ctx->sh, "  %s", ssid_buf);
}

int wt_wifi_credentials_print_list(const struct shell *sh)
{
	struct wifi_credential_list_ctx ctx = {
		.sh = sh,
		.count = 0,
	};

	wifi_credentials_for_each_ssid(wifi_credential_list_cb, &ctx);

	if (ctx.count == 0) {
		shell_print(sh, "  <none>");
	}

	return ctx.count;
}

struct wifi_credential_format_ctx {
	char *buf;
	size_t buf_len;
	size_t offset;
	int count;
};

static void wifi_credential_format_cb(void *cb_arg, const char *ssid, size_t ssid_len)
{
	struct wifi_credential_format_ctx *ctx = cb_arg;
	size_t available;
	size_t copy_len;

	if (ctx->offset >= ctx->buf_len) {
		return;
	}

	if (ctx->count > 0 && ctx->offset + 2 < ctx->buf_len) {
		ctx->buf[ctx->offset++] = ',';
		ctx->buf[ctx->offset++] = ' ';
	}

	available = ctx->buf_len - ctx->offset - 1;
	copy_len = MIN(ssid_len, available);
	memcpy(&ctx->buf[ctx->offset], ssid, copy_len);
	ctx->offset += copy_len;
	ctx->buf[ctx->offset] = '\0';
	ctx->count++;
}

int wt_wifi_credentials_format_list(char *buf, size_t buf_len)
{
	struct wifi_credential_format_ctx ctx = {
		.buf = buf,
		.buf_len = buf_len,
		.offset = 0,
		.count = 0,
	};

	if (!buf || buf_len == 0) {
		return -EINVAL;
	}

	buf[0] = '\0';
	wifi_credentials_for_each_ssid(wifi_credential_format_cb, &ctx);

	if (ctx.count == 0) {
		snprintk(buf, buf_len, "<none>");
	}

	return ctx.count;
}

int wt_wifi_credentials_set(const char *ssid, const char *password, const char *security_text, bool replace_all)
{
	enum wifi_security_type security = WIFI_SECURITY_TYPE_PSK;
	int ret;

	ret = parse_wifi_security(security_text, &security);
	if (ret) {
		return ret;
	}

	if (security == WIFI_SECURITY_TYPE_NONE) {
		return wifi_credential_store(ssid, "", security, replace_all);
	}

	return wifi_credential_store(ssid, password, security, replace_all);
}

int wt_wifi_credentials_open(const char *ssid, bool replace_all)
{
	return wifi_credential_store(ssid, "", WIFI_SECURITY_TYPE_NONE, replace_all);
}

int wt_wifi_credentials_forget(const char *ssid)
{
	if (!ssid || strlen(ssid) == 0) {
		return -EINVAL;
	}

	return wifi_credentials_delete_by_ssid(ssid, strlen(ssid));
}

int wt_wifi_credentials_clear(void)
{
	return wifi_credentials_delete_all();
}

int wt_wifi_credentials_shell(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	enum wifi_security_type security = WIFI_SECURITY_TYPE_PSK;
	bool replace_all = true;

	if (argc < 3) {
		shell_print(sh, "usage: wt wifi cred list");
		shell_print(sh, "       wt wifi cred set <ssid> <password> [wpa2|auto|wpa3]");
		shell_print(sh, "       wt wifi cred add <ssid> <password> [wpa2|auto|wpa3]");
		shell_print(sh, "       wt wifi cred open <ssid>");
		shell_print(sh, "       wt wifi cred forget <ssid>");
		shell_print(sh, "       wt wifi cred clear");
		return -EINVAL;
	}

	if (!strcmp(argv[2], "list")) {
		shell_print(sh, "stored Wi-Fi credentials:");
		(void)wt_wifi_credentials_print_list(sh);
		return 0;
	}

	if (!strcmp(argv[2], "clear")) {
		ret = wifi_credentials_delete_all();
		if (ret) {
			shell_error(sh, "credential clear failed: %d", ret);
			return ret;
		}
		shell_print(sh, "cleared stored Wi-Fi credentials");
		return 0;
	}

	if (!strcmp(argv[2], "forget")) {
		if (argc < 4) {
			shell_print(sh, "usage: wt wifi cred forget <ssid>");
			return -EINVAL;
		}

		ret = wifi_credentials_delete_by_ssid(argv[3], strlen(argv[3]));
		if (ret) {
			shell_error(sh, "credential forget failed: %d", ret);
			return ret;
		}

		shell_print(sh, "forgot Wi-Fi credentials for %s", argv[3]);
		return 0;
	}

	if (!strcmp(argv[2], "open")) {
		if (argc < 4) {
			shell_print(sh, "usage: wt wifi cred open <ssid>");
			return -EINVAL;
		}

		ret = wifi_credential_store(argv[3], "", WIFI_SECURITY_TYPE_NONE, true);
		if (ret) {
			shell_error(sh, "credential set failed: %d", ret);
			return ret;
		}

		shell_print(sh, "stored open Wi-Fi SSID: %s", argv[3]);
		return wt_wifi_reconnect_if_requested();
	}

	if (!strcmp(argv[2], "set") || !strcmp(argv[2], "add")) {
		if (argc < 5) {
			shell_print(sh, "usage: wt wifi cred %s <ssid> <password> [wpa2|auto|wpa3]", argv[2]);
			return -EINVAL;
		}

		replace_all = !strcmp(argv[2], "set");

		if (argc >= 6) {
			ret = parse_wifi_security(argv[5], &security);
			if (ret) {
				shell_print(sh, "security must be wpa2, auto, wpa3, or open");
				return ret;
			}
		}

		if (security == WIFI_SECURITY_TYPE_NONE) {
			ret = wifi_credential_store(argv[3], "", security, replace_all);
		} else {
			ret = wifi_credential_store(argv[3], argv[4], security, replace_all);
		}

		if (ret) {
			shell_error(sh, "credential set failed: %d", ret);
			return ret;
		}

		shell_print(sh, "%s Wi-Fi SSID: %s (%s)",
			    replace_all ? "selected" : "added", argv[3], wifi_security_name(security));
		return wt_wifi_reconnect_if_requested();
	}

	shell_print(sh, "usage: wt wifi cred list|set|add|open|forget|clear");
	return -EINVAL;
}

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		LOG_ERR("No Wi-Fi interface");
		return -ENODEV;
	}

	wifi_context.connected = false;
	wifi_context.connect_result = false;
	wifi_context.ipv4_bound = false;
	snprintk(wifi_ipv4_text, sizeof(wifi_ipv4_text), "0.0.0.0");

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0)) {
		LOG_ERR("Connection request failed");
		return -ENOEXEC;
	}

	LOG_INF("Connection requested");
	return 0;
}

static int wifi_disconnect(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int ret;

	if (!iface) {
		LOG_ERR("No Wi-Fi interface");
		return -ENODEV;
	}

	wifi_context.disconnect_requested = true;
	ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	if (ret) {
		wifi_context.disconnect_requested = false;
		LOG_WRN("Wi-Fi disconnect request failed: %d", ret);
		return ret;
	}

	return 0;
}

int wt_wifi_service_set(bool enable)
{
	if (enable) {
		wifi_requested = true;
		k_sem_give(&wifi_control_sem);
		LOG_INF("Wi-Fi command mode enabled");
		return 0;
	}

	wifi_requested = false;
	wifi_context.connect_result = true;
	wifi_context.ipv4_bound = false;
	snprintk(wifi_ipv4_text, sizeof(wifi_ipv4_text), "0.0.0.0");

	if (wifi_context.connected) {
		(void)wifi_disconnect();
	} else {
		wifi_context.connected = false;
	}

	LOG_INF("Wi-Fi command mode disabled");
	return 0;
}

int wt_wifi_reconnect_if_requested(void)
{
	bool restart = wifi_requested;

	if (!restart) {
		return 0;
	}

	(void)wt_wifi_service_set(false);
	k_sleep(K_MSEC(500));
	return wt_wifi_service_set(true);
}

int wt_wifi_status_log(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_iface_status status = { 0 };

	if (!iface) {
		LOG_ERR("No Wi-Fi interface");
		return -ENODEV;
	}

	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status,
			     sizeof(struct wifi_iface_status))) {
		LOG_INF("Status request failed");
		return -ENOEXEC;
	}

	LOG_INF("==================");
	LOG_INF("Wi-Fi requested: %s", wt_onoff_txt(wifi_requested));
	LOG_INF("State: %s", wifi_state_txt(status.state));

	if (status.state >= WIFI_STATE_ASSOCIATED) {
		uint8_t mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

		LOG_INF("Interface Mode: %s", wifi_mode_txt(status.iface_mode));
		LOG_INF("Link Mode: %s", wifi_link_mode_txt(status.link_mode));
		LOG_INF("SSID: %.32s", status.ssid);
		LOG_INF("BSSID: %s",
			net_sprint_ll_addr_buf(status.bssid, WIFI_MAC_ADDR_LEN,
					       mac_string_buf, sizeof(mac_string_buf)));
		LOG_INF("Band: %s", wifi_band_txt(status.band));
		LOG_INF("Channel: %d", status.channel);
		LOG_INF("Security: %s", wifi_security_txt(status.security));
		LOG_INF("MFP: %s", wifi_mfp_txt(status.mfp));
		LOG_INF("RSSI: %d", status.rssi);
	}

	return 0;
}

static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (wifi_context.connected) {
		return;
	}

	if (status->status) {
		LOG_ERR("Connection failed (%d)", status->status);
		wt_leds_error_activity();
		wifi_context.connected = false;
		wifi_context.ipv4_bound = false;
	snprintk(wifi_ipv4_text, sizeof(wifi_ipv4_text), "0.0.0.0");
	} else {
		LOG_INF("Connected");
		wt_leds_wifi_activity();
		wifi_context.connected = true;
		wifi_context.ipv4_bound = false;
	snprintk(wifi_ipv4_text, sizeof(wifi_ipv4_text), "0.0.0.0");
	}

	wifi_context.connect_result = true;
}

static void handle_wifi_disconnect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (!wifi_context.connected && !wifi_context.disconnect_requested) {
		return;
	}

	if (wifi_context.disconnect_requested) {
		LOG_INF("Disconnection request %s (%d)",
			status->status ? "failed" : "done", status->status);
		wifi_context.disconnect_requested = false;
	} else {
		LOG_INF("Received disconnected event");
	}

	wifi_context.connected = false;
	wifi_context.ipv4_bound = false;
	snprintk(wifi_ipv4_text, sizeof(wifi_ipv4_text), "0.0.0.0");
	wifi_context.connect_result = false;

	wt_wifi_status_log();
}


static void wifi_scan_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		wt_wifi_scan_store_result((const struct wifi_scan_result *)cb->info);
		break;
	case NET_EVENT_WIFI_SCAN_DONE: {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;
		k_mutex_lock(&wifi_scan_lock, K_FOREVER);
		wifi_scan_running = false;
		wifi_scan_valid = true;
		wifi_scan_finished_ms = k_uptime_get();
		wifi_scan_last_status = status ? status->status : 0;
		wt_wifi_scan_sort_locked();
		k_mutex_unlock(&wifi_scan_lock);
		LOG_INF("Wi-Fi scan done: status=%d count=%d", wifi_scan_last_status, wifi_scan_result_count);
			if (wifi_scan_last_status) {
				wt_leds_error_activity();
			} else {
				wt_leds_wifi_activity();
			}
		k_sem_give(&wifi_scan_done_sem);
		break;
	}
	default:
		break;
	}
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		handle_wifi_connect_result(cb);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		handle_wifi_disconnect_result(cb);
		break;
	default:
		break;
	}
}

static void print_dhcp_ip(struct net_mgmt_event_callback *cb)
{
	const struct net_if_dhcpv4 *dhcpv4 = cb->info;
	const struct in_addr *addr = &dhcpv4->requested_ip;

	wifi_ipv4_text[0] = '\0';
	net_addr_ntop(AF_INET, addr, wifi_ipv4_text, sizeof(wifi_ipv4_text));

	if (wifi_ipv4_text[0] == '\0') {
		snprintk(wifi_ipv4_text, sizeof(wifi_ipv4_text), "0.0.0.0");
	}

	wifi_context.ipv4_bound = true;
	LOG_INF("DHCP IP address: %s", wifi_ipv4_text);
	wt_leds_wifi_activity();
}

static void net_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_IPV4_DHCP_BOUND:
		print_dhcp_ip(cb);
		break;
	default:
		break;
	}
}

static int bytes_from_str(const char *str, uint8_t *bytes, size_t bytes_len)
{
	size_t i;
	char byte_str[3];

	if (strlen(str) != bytes_len * 2) {
		LOG_ERR("Invalid string length: %zu (expected: %zu)", strlen(str), bytes_len * 2);
		return -EINVAL;
	}

	for (i = 0; i < bytes_len; i++) {
		memcpy(byte_str, str + i * 2, 2);
		byte_str[2] = '\0';
		bytes[i] = strtol(byte_str, NULL, 16);
	}

	return 0;
}

static bool is_mac_addr_set(struct net_if *iface)
{
	struct net_linkaddr *linkaddr = net_if_get_link_addr(iface);
	struct net_eth_addr wifi_addr;

	if (!linkaddr || linkaddr->len != WIFI_MAC_ADDR_LEN) {
		return false;
	}

	memcpy(wifi_addr.addr, linkaddr->addr, WIFI_MAC_ADDR_LEN);

	return net_eth_is_addr_valid(&wifi_addr);
}

static int wifi_prepare_once(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int ret;

	if (wifi_prepared) {
		return 0;
	}

	if (!iface) {
		LOG_ERR("Failed to get Wi-Fi interface");
		return -ENODEV;
	}

#if defined(CONFIG_BOARD_NRF7002DK_NRF5340_CPUAPP_NRF7001) || \
	defined(CONFIG_BOARD_NRF7002DK_NRF5340_CPUAPP)
	if (strlen(CONFIG_NRF70_QSPI_ENCRYPTION_KEY)) {
		char key[QSPI_KEY_LEN_BYTES];

		ret = bytes_from_str(CONFIG_NRF70_QSPI_ENCRYPTION_KEY, key, sizeof(key));
		if (ret) {
			LOG_ERR("Failed to parse encryption key: %d", ret);
			return ret;
		}

		ret = qspi_enable_encryption(key);
		if (ret) {
			LOG_ERR("Failed to enable encryption: %d", ret);
			return ret;
		}
		LOG_INF("QSPI encryption enabled");
	} else {
		LOG_INF("QSPI encryption disabled");
	}
#endif

#if defined(CONFIG_NET_CONFIG_MY_IPV4_ADDR) && \
	defined(CONFIG_NET_CONFIG_MY_IPV4_NETMASK) && \
	defined(CONFIG_NET_CONFIG_MY_IPV4_GW)
	LOG_INF("Static IPv4 fallback: %s/%s -> %s",
		CONFIG_NET_CONFIG_MY_IPV4_ADDR,
		CONFIG_NET_CONFIG_MY_IPV4_NETMASK,
		CONFIG_NET_CONFIG_MY_IPV4_GW);
#else
	LOG_INF("IPv4 configuration: DHCP");
#endif

	if (!is_mac_addr_set(iface)) {
		struct ethernet_req_params params;

		if (!net_eth_is_addr_valid((struct net_eth_addr *)wifi_mac_addr)) {
			LOG_ERR("No valid MAC address: OTP not programmed and no valid local-mac-address in DTS");
			return -EINVAL;
		}

		if (net_if_is_admin_up(iface)) {
#ifdef CONFIG_WIFI_READY_LIB
			k_sem_reset(&wifi_ready_state_changed_sem);
#endif
			ret = net_if_down(iface);
			if (ret < 0 && ret != -EALREADY) {
				LOG_ERR("Cannot bring down iface (%d)", ret);
				return ret;
			}
#ifdef CONFIG_WIFI_READY_LIB
			LOG_INF("Waiting for Wi-Fi to be not ready");
			ret = k_sem_take(&wifi_ready_state_changed_sem, K_SECONDS(10));
			if (ret) {
				LOG_ERR("Timeout waiting for Wi-Fi not ready: %d", ret);
				return ret;
			}
#endif
		}

		memcpy(params.mac_address.addr, wifi_mac_addr, sizeof(wifi_mac_addr));

		ret = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface,
			       &params, sizeof(params));
		if (ret) {
			LOG_ERR("Failed to set Wi-Fi MAC address: %d", ret);
			return ret;
		}

		LOG_INF("OTP not programmed, using MAC from DTS: %s",
			net_sprint_ll_addr(net_if_get_link_addr(iface)->addr,
					   net_if_get_link_addr(iface)->len));
	}

#ifdef CONFIG_WIFI_READY_LIB
	k_sem_reset(&wifi_ready_state_changed_sem);
#endif
	ret = net_if_up(iface);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Cannot bring up iface (%d)", ret);
		return ret;
	}

#ifdef CONFIG_WIFI_READY_LIB
	if (!wifi_ready_status) {
		LOG_INF("Waiting for Wi-Fi to be ready after interface up");
		ret = k_sem_take(&wifi_ready_state_changed_sem, K_SECONDS(10));
		if (ret) {
			LOG_ERR("Timeout waiting for Wi-Fi ready: %d", ret);
			return ret;
		}
	}

	if (!wifi_ready_status) {
		LOG_ERR("Wi-Fi not ready after interface up");
		return -EIO;
	}
#endif

	wifi_prepared = true;
	LOG_INF("Wi-Fi interface prepared");
	return 0;
}

static int wifi_worker_loop(void)
{
	int ret;

	while (1) {
		while (!wifi_requested) {
			(void)k_sem_take(&wifi_control_sem, K_MSEC(500));
		}

		ret = wifi_prepare_once();
		if (ret) {
			LOG_ERR("Wi-Fi prepare failed: %d", ret);
			wifi_requested = false;
			continue;
		}

#ifdef CONFIG_WIFI_READY_LIB
		if (!wifi_ready_status) {
			LOG_INF("Waiting for Wi-Fi ready event");
			ret = k_sem_take(&wifi_ready_state_changed_sem, K_SECONDS(1));
			if (ret) {
				continue;
			}
			if (!wifi_ready_status) {
				continue;
			}
		}
#endif

		if (!wifi_requested || wifi_context.connected) {
			k_sleep(K_MSEC(250));
			continue;
		}

		if (wifi_scan_running) {
			k_sleep(K_MSEC(250));
			continue;
		}

		ret = wifi_connect();
		if (ret) {
			k_sleep(K_SECONDS(2));
			continue;
		}

		while (!wifi_context.connect_result && wifi_requested) {
			wt_wifi_status_log();
			k_sleep(K_MSEC(STATUS_POLLING_MS));
		}

		if (!wifi_requested) {
			continue;
		}

		if (wifi_context.connected) {
			wt_wifi_status_log();
			while (wifi_requested && wifi_context.connected) {
				k_sleep(K_SECONDS(1));
			}
		} else {
			k_sleep(K_SECONDS(2));
		}
	}

	return 0;
}

#ifdef CONFIG_WIFI_READY_LIB
static void wt_wifi_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	(void)wifi_worker_loop();
}

#define THREAD_PRIORITY K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
K_THREAD_DEFINE(start_wifi_thread_id, CONFIG_STA_SAMPLE_START_WIFI_THREAD_STACK_SIZE,
		wt_wifi_thread, NULL, NULL, NULL, THREAD_PRIORITY, 0, -1);

static void wifi_ready_cb(bool wifi_ready)
{
	LOG_DBG("Is Wi-Fi ready?: %s", wifi_ready ? "yes" : "no");
	wifi_ready_status = wifi_ready;
	k_sem_give(&wifi_ready_state_changed_sem);
}

int wt_wifi_register_ready_callback(void)
{
	int ret = 0;
	wifi_ready_callback_t cb;
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		LOG_ERR("Failed to get Wi-Fi interface");
		return -ENODEV;
	}

	cb.wifi_ready_cb = wifi_ready_cb;

	LOG_DBG("Registering Wi-Fi ready callbacks");
	ret = register_wifi_ready_callback(cb, iface);
	if (ret) {
		LOG_ERR("Failed to register Wi-Fi ready callbacks %s", strerror(ret));
		return ret;
	}

	return ret;
}

void wt_wifi_start_worker(void)
{
	k_thread_start(start_wifi_thread_id);
}
#endif


static int wt_wifi_cmd_rsp(char *rsp, size_t rsp_len, const char *fmt, ...)
{
	va_list args;
	int len;

	if (!rsp || rsp_len == 0) {
		return -EINVAL;
	}

	va_start(args, fmt);
	len = vsnprintk(rsp, rsp_len, fmt, args);
	va_end(args);

	if (len < 0) {
		rsp[0] = '\0';
		return len;
	}

	if ((size_t)len >= rsp_len) {
		rsp[rsp_len - 1] = '\0';
		return rsp_len - 1;
	}

	return len;
}


static int wt_wifi_cmd_tx_uart_payload(char **argv, size_t argc, size_t payload_arg)
{
	int payload_len;
	char payload[WT_TX_PAYLOAD_MAX];

	payload_len = wt_build_payload_from_argv(payload_arg, argc, argv,
					 payload, sizeof(payload));
	if (payload_len < 0) {
		return payload_len;
	}

	printk("WIFI UART TX: ");
	for (int i = 0; i < payload_len; i++) {
		printk("%c", payload[i]);
	}
	printk("\r\n");

	return payload_len;
}

static int wt_wifi_command_tx(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	int payload_len;
	int ret;
	char payload[WT_TX_PAYLOAD_MAX];

	if (argc < 3) {
		return wt_wifi_cmd_rsp(rsp, rsp_len,
					  "usage tx ble <msg> | tx uart <msg> | tx wifi <ip> <port> <msg> | tx both <ip> <port> <msg>");
	}

	if (!strcmp(argv[1], "ble") || !strcmp(argv[1], "bt")) {
		payload_len = wt_build_payload_from_argv(2, argc, argv,
						 payload, sizeof(payload));
		if (payload_len < 0) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err payload %d", payload_len);
		}

		ret = wt_ble_transmit_payload((const uint8_t *)payload, payload_len);
		if (ret == -EACCES) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err ble tx notify off; enable TX notify first");
		}
		if (ret == -ENOTCONN) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err ble tx not connected");
		}
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err ble tx %d", ret);
		}

		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok ble tx %d bytes", payload_len);
	}

	if (!strcmp(argv[1], "uart") || !strcmp(argv[1], "serial")) {
		payload_len = wt_wifi_cmd_tx_uart_payload(argv, argc, 2);
		if (payload_len < 0) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err uart payload %d", payload_len);
		}

		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok uart tx %d bytes", payload_len);
	}

	if (!strcmp(argv[1], "wifi") || !strcmp(argv[1], "udp")) {
		if (argc < 5) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage tx wifi <ip> <port> <msg>");
		}

		payload_len = wt_build_payload_from_argv(4, argc, argv,
						 payload, sizeof(payload));
		if (payload_len < 0) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err payload %d", payload_len);
		}

		ret = wt_wifi_udp_transmit_payload(argv[2], argv[3],
						       (const uint8_t *)payload, payload_len);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi tx %d", ret);
		}

		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi udp tx %d bytes to %s:%s",
					      payload_len, argv[2], argv[3]);
	}

	if (!strcmp(argv[1], "both") || !strcmp(argv[1], "all")) {
		if (argc < 5) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage tx both <ip> <port> <msg>");
		}

		payload_len = wt_build_payload_from_argv(4, argc, argv,
						 payload, sizeof(payload));
		if (payload_len < 0) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err payload %d", payload_len);
		}

		ret = wt_wifi_udp_transmit_payload(argv[2], argv[3],
						       (const uint8_t *)payload, payload_len);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi tx %d", ret);
		}

		(void)wt_wifi_cmd_tx_uart_payload(argv, argc, 4);
		ret = wt_ble_transmit_payload((const uint8_t *)payload, payload_len);
		if (ret && ret != -EACCES) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err ble tx %d after wifi/uart", ret);
		}

		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi+uart%s tx %d bytes%s",
					      ret == -EACCES ? "" : "+ble", payload_len,
					      ret == -EACCES ? " (ble notify off)" : "");
	}

	return wt_wifi_cmd_rsp(rsp, rsp_len, "usage tx ble|uart|wifi|both ...");
}

static int wt_wifi_command_cred(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	int ret;
	char list[WT_WIFI_CMD_RSP_TEXT_MAX - 32];

	if (argc < 3) {
		return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi cred list|set|add|open|forget|clear");
	}

	if (!strcmp(argv[2], "list")) {
		ret = wt_wifi_credentials_format_list(list, sizeof(list));
		if (ret < 0) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err cred list %d", ret);
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok creds %s", list);
	}

	if (!strcmp(argv[2], "clear")) {
		ret = wt_wifi_credentials_clear();
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err cred clear %d", ret);
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok creds cleared");
	}

	if (!strcmp(argv[2], "forget")) {
		if (argc < 4) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi cred forget <ssid>");
		}
		ret = wt_wifi_credentials_forget(argv[3]);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err cred forget %d", ret);
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok forgot %s", argv[3]);
	}

	if (!strcmp(argv[2], "open")) {
		if (argc < 4) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi cred open <ssid>");
		}
		ret = wt_wifi_credentials_open(argv[3], true);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err cred open %d", ret);
		}
		(void)wt_wifi_reconnect_if_requested();
		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok open ssid %s", argv[3]);
	}

	if (!strcmp(argv[2], "set") || !strcmp(argv[2], "add")) {
		bool replace_all = !strcmp(argv[2], "set");
		const char *security = argc >= 6 ? argv[5] : NULL;

		if (argc < 5) {
			return wt_wifi_cmd_rsp(rsp, rsp_len,
						  "usage wifi cred %s <ssid> <pass> [wpa2|auto|wpa3]", argv[2]);
		}

		ret = wt_wifi_credentials_set(argv[3], argv[4], security, replace_all);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err cred set %d", ret);
		}
		(void)wt_wifi_reconnect_if_requested();
		return wt_wifi_cmd_rsp(rsp, rsp_len, "ok %s ssid %s", replace_all ? "selected" : "added", argv[3]);
	}

	return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi cred list|set|add|open|forget|clear");
}

static int wt_wifi_command_wifi(char **argv, size_t argc, char *rsp, size_t rsp_len)
{
	int ret;

	if (argc < 2) {
		return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi on|off [delay]|status [json]|reconnect|scan|cmd|discovery|cred");
	}

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
		ret = wt_wifi_service_set(true);
		return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi on %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi on");
	}

	if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
		uint32_t delay_ms = 0;
		if (argc >= 3) {
			ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
			if (ret) {
				return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi off delay %d", ret);
			}
		}
		ret = wt_app_delayed_radio_apply("wifi_off", delay_ms);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi off %d", ret);
		}
		return delay_ms ? wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi off scheduled %u ms", delay_ms) :
					 wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi off");
	}

	if (!strcmp(argv[1], "status")) {
		return wt_app_wifi_status_format(rsp, rsp_len, argc >= 3 && !strcmp(argv[2], "json"));
	}

	if (!strcmp(argv[1], "reconnect")) {
		ret = wt_wifi_reconnect_if_requested();
		if (!wt_wifi_is_requested()) {
			ret = wt_wifi_service_set(true);
		}
		return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi reconnect %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi reconnect");
	}

	if (!strcmp(argv[1], "scan") || !strcmp(argv[1], "apscan") || !strcmp(argv[1], "networks")) {
		return wt_wifi_scan_command(argv, argc, rsp, rsp_len);
	}

	if (!strcmp(argv[1], "cmd") || !strcmp(argv[1], "command") || !strcmp(argv[1], "commands")) {
		if (argc < 3 || !strcmp(argv[2], "status")) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi cmd %s port %u",
					      wt_onoff_txt(wt_wifi_cmd_is_enabled()), wt_wifi_cmd_port());
		}
		if (!strcmp(argv[2], "port")) {
			uint16_t port;
			if (argc < 4) {
				return wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi cmd port %u", wt_wifi_cmd_port());
			}
			ret = wt_parse_udp_port(argv[3], &port);
			if (ret) {
				return wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi cmd port %d", ret);
			}
			ret = wt_wifi_cmd_port_set(port);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi cmd port set %d", ret) :
				     wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi cmd port %u", wt_wifi_cmd_port());
		}
		if (!strcmp(argv[2], "on") || !strcmp(argv[2], "start")) {
			ret = wt_wifi_cmd_service_set(true);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi cmd on %d", ret) :
				     wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi cmd on port %u", wt_wifi_cmd_port());
		}
		if (!strcmp(argv[2], "off") || !strcmp(argv[2], "stop")) {
			ret = wt_wifi_cmd_service_set(false);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err wifi cmd off %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok wifi cmd off");
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi cmd on|off|status|port [port]");
	}

	if (!strcmp(argv[1], "discovery") || !strcmp(argv[1], "discover")) {
		if (argc < 3 || !strcmp(argv[2], "status")) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "ok discovery %s port %u interval %ums",
					  wt_onoff_txt(wt_wifi_discovery_is_enabled()),
					  WT_WIFI_DISCOVERY_UDP_PORT, WT_WIFI_DISCOVERY_INTERVAL_MS);
		}
		if (!strcmp(argv[2], "on") || !strcmp(argv[2], "start")) {
			ret = wt_wifi_discovery_service_set(true);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err discovery on %d", ret) :
				     wt_wifi_cmd_rsp(rsp, rsp_len, "ok discovery on port %u", WT_WIFI_DISCOVERY_UDP_PORT);
		}
		if (!strcmp(argv[2], "off") || !strcmp(argv[2], "stop")) {
			ret = wt_wifi_discovery_service_set(false);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err discovery off %d", ret) :
				     wt_wifi_cmd_rsp(rsp, rsp_len, "ok discovery off");
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi discovery on|off|status");
	}

	if (!strcmp(argv[1], "cred") || !strcmp(argv[1], "creds")) {
		return wt_wifi_command_cred(argv, argc, rsp, rsp_len);
	}

	return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wifi on|off [delay]|status [json]|reconnect|scan|cmd|discovery|cred");
}

int wt_wifi_command_execute(const char *line, char *rsp, size_t rsp_len)
{
	char line_buf[WT_WIFI_CMD_RX_TEXT_MAX];
	char *argv_storage[16];
	char **argv = argv_storage;
	int argc;
	int ret;

	if (!line || strlen(line) == 0) {
		return wt_wifi_cmd_rsp(rsp, rsp_len, "err empty command");
	}

	strncpy(line_buf, line, sizeof(line_buf) - 1);
	line_buf[sizeof(line_buf) - 1] = '\0';

	argc = wt_split_args_quoted(line_buf, argv_storage, ARRAY_SIZE(argv_storage));
	if (argc < 0) {
		return wt_wifi_cmd_rsp(rsp, rsp_len, "err parse %d; check quotes/escapes", argc);
	}
	if (argc == 0) {
		return wt_wifi_cmd_rsp(rsp, rsp_len, "err empty command");
	}

	if (argv[0][0] == '#') {
		argv++;
		argc--;
		if (argc <= 0) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err empty command after request id");
		}
	}

	if (!strcmp(argv[0], "wt")) {
		argv++;
		argc--;
		if (argc <= 0) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage wt status|mode|wifi|ble|tx|help");
		}
	}

	if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
		return wt_wifi_cmd_rsp(rsp, rsp_len,
					  "cmds: id, version, fw, config, boot, status [json], mode [delay], wifi scan/cmd/cred, ble, discovery, bridge, led, ping, tx, reboot");
	}

	if (!strcmp(argv[0], "id")) {
		return wt_app_id_format(rsp, rsp_len);
	}

	if (!strcmp(argv[0], "version") || !strcmp(argv[0], "ver")) {
		return wt_app_version_format(rsp, rsp_len);
	}

	if (!strcmp(argv[0], "fw")) {
		return wt_app_fw_command(argv, argc, rsp, rsp_len);
	}

	if (!strcmp(argv[0], "reboot")) {
		char *fw_argv[4];
		size_t fw_argc = 2;
		fw_argv[0] = "fw";
		fw_argv[1] = "reboot";
		if (argc >= 2) { fw_argv[2] = argv[1]; fw_argc = 3; }
		if (argc >= 3) { fw_argv[3] = argv[2]; fw_argc = 4; }
		return wt_app_fw_command(fw_argv, fw_argc, rsp, rsp_len);
	}

	if (!strcmp(argv[0], "boot")) {
		return wt_app_boot_command(argv, argc, rsp, rsp_len);
	}

	if (!strcmp(argv[0], "bridge")) {
		return wt_app_bridge_command(argv, argc, rsp, rsp_len);
	}

	if (!strcmp(argv[0], "led") || !strcmp(argv[0], "indicator") || !strcmp(argv[0], "ind")) {
		return wt_app_led_command(argv, argc, rsp, rsp_len);
	}

	if (!strcmp(argv[0], "ping")) {
		return wt_app_ping_execute(argv, argc, rsp, rsp_len);
	}

	if (!strcmp(argv[0], "config")) {
		bool json = argc >= 2 && !strcmp(argv[1], "json");
		if (argc >= 2 && !strcmp(argv[1], "save")) {
			ret = wt_app_config_save();
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err config save %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok config saved");
		}
		if (argc >= 2 && !strcmp(argv[1], "reset")) {
			ret = wt_app_config_reset();
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err config reset %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok config reset");
		}
		return wt_app_config_format(rsp, rsp_len, json);
	}

	if (!strcmp(argv[0], "discovery") || !strcmp(argv[0], "discover")) {
		if (argc < 2 || !strcmp(argv[1], "status")) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "ok discovery %s port %u interval %ums",
					  wt_onoff_txt(wt_wifi_discovery_is_enabled()),
					  WT_WIFI_DISCOVERY_UDP_PORT, WT_WIFI_DISCOVERY_INTERVAL_MS);
		}
		if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
			ret = wt_wifi_discovery_service_set(true);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err discovery on %d", ret) :
				     wt_wifi_cmd_rsp(rsp, rsp_len, "ok discovery on port %u", WT_WIFI_DISCOVERY_UDP_PORT);
		}
		if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
			ret = wt_wifi_discovery_service_set(false);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err discovery off %d", ret) :
				     wt_wifi_cmd_rsp(rsp, rsp_len, "ok discovery off");
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "usage discovery on|off|status");
	}

	if (!strcmp(argv[0], "name")) {
		if (argc < 2 || !strcmp(argv[1], "get")) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "ok name %s", wt_ble_name_get());
		}
		if (!strcmp(argv[1], "set")) {
			if (argc < 3) {
				return wt_wifi_cmd_rsp(rsp, rsp_len, "usage name set <ble-name>");
			}
			ret = wt_ble_name_set(argv[2]);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err name set %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok name %s", wt_ble_name_get());
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "usage name get|set <ble-name>");
	}

	if (!strcmp(argv[0], "status")) {
		return wt_app_config_format(rsp, rsp_len, argc >= 2 && !strcmp(argv[1], "json"));
	}

	if (!strcmp(argv[0], "mode")) {
		uint32_t delay_ms = 0;
		if (argc < 2) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage mode idle|ble|wifi|both [delay]");
		}
		if (argc >= 3) {
			ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
			if (ret) {
				return wt_wifi_cmd_rsp(rsp, rsp_len, "err mode delay %d", ret);
			}
		}
		ret = wt_app_delayed_radio_apply(argv[1], delay_ms);
		if (ret) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "err mode %d", ret);
		}
		return delay_ms ? wt_wifi_cmd_rsp(rsp, rsp_len, "ok mode %s scheduled %u ms", argv[1], delay_ms) :
				  wt_wifi_cmd_rsp(rsp, rsp_len, "ok mode %s", argv[1]);
	}

	if (!strcmp(argv[0], "wifi")) {
		return wt_wifi_command_wifi(argv, argc, rsp, rsp_len);
	}

	if (!strcmp(argv[0], "ble") || !strcmp(argv[0], "bt")) {
		if (argc < 2) {
			return wt_wifi_cmd_rsp(rsp, rsp_len, "usage ble on|off|status|name [get|set <name>]");
		}
		if (!strcmp(argv[1], "name")) {
			if (argc < 3 || !strcmp(argv[2], "get")) {
				return wt_wifi_cmd_rsp(rsp, rsp_len, "ok ble name %s", wt_ble_name_get());
			}
			if (!strcmp(argv[2], "set")) {
				if (argc < 4) {
					return wt_wifi_cmd_rsp(rsp, rsp_len, "usage ble name set <name>");
				}
				ret = wt_ble_name_set(argv[3]);
				return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err ble name set %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok ble name %s", wt_ble_name_get());
			}
			ret = wt_ble_name_set(argv[2]);
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err ble name set %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok ble name %s", wt_ble_name_get());
		}
		if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
			ret = wt_ble_service_start();
			return ret ? wt_wifi_cmd_rsp(rsp, rsp_len, "err ble on %d", ret) : wt_wifi_cmd_rsp(rsp, rsp_len, "ok ble on");
		}
		if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
			uint32_t delay_ms = 0;
			if (argc >= 3) {
				ret = wt_app_parse_delay_ms(argv[2], &delay_ms);
				if (ret) {
					return wt_wifi_cmd_rsp(rsp, rsp_len, "err ble off delay %d", ret);
				}
			}
			ret = wt_app_delayed_radio_apply("ble_off", delay_ms);
			if (ret) {
				return wt_wifi_cmd_rsp(rsp, rsp_len, "err ble off %d", ret);
			}
			return delay_ms ? wt_wifi_cmd_rsp(rsp, rsp_len, "ok ble off scheduled %u ms", delay_ms) :
					      wt_wifi_cmd_rsp(rsp, rsp_len, "ok ble off");
		}
		if (!strcmp(argv[1], "status")) {
			return wt_app_ble_status_format(rsp, rsp_len, argc >= 3 && !strcmp(argv[2], "json"));
		}
		return wt_wifi_cmd_rsp(rsp, rsp_len, "usage ble on|off|status|name [get|set <name>]");
	}

	if (!strcmp(argv[0], "tx")) {
		return wt_wifi_command_tx(argv, argc, rsp, rsp_len);
	}

	return wt_wifi_cmd_rsp(rsp, rsp_len, "err unknown command: %s", argv[0]);
}

static void wt_wifi_cmd_prefix_request_id(const char *cmd, char *rsp, size_t rsp_len)
{
	char id[24];
	char old[WT_WIFI_CMD_RSP_TEXT_MAX];
	size_t i = 0;

	if (!cmd || !rsp || rsp_len == 0 || cmd[0] != '#') {
		return;
	}

	for (i = 1; cmd[i] != '\0' && cmd[i] != ' ' && cmd[i] != '\t' && i < sizeof(id); i++) {
		id[i - 1] = cmd[i];
	}
	id[i - 1] = '\0';

	if (id[0] == '\0' || rsp[0] == '#') {
		return;
	}

	strncpy(old, rsp, sizeof(old) - 1);
	old[sizeof(old) - 1] = '\0';

	/*
	 * Build the prefixed response in two bounded steps so GCC does not warn
	 * about a possible %s truncation when old[] contains a full-size response.
	 */
	rsp[0] = '\0';
	int prefix_len = snprintk(rsp, rsp_len, "#%s ", id);

	if (prefix_len < 0) {
		rsp[0] = '\0';
		return;
	}

	if ((size_t)prefix_len >= rsp_len) {
		rsp[rsp_len - 1] = '\0';
		return;
	}

	size_t used = (size_t)prefix_len;
	size_t room = rsp_len - used - 1;
	size_t copy_len = MIN(strlen(old), room);

	memcpy(&rsp[used], old, copy_len);
	rsp[used + copy_len] = '\0';
}

static void wt_wifi_cmd_trim(char *buf)
{
	size_t len = strlen(buf);

	while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
			 buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
		buf[--len] = '\0';
	}
}

static int wifi_cmd_socket_open(uint16_t port)
{
	struct sockaddr_in bind_addr = { 0 };
	int sock;
	int ret;

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Wi-Fi UDP command socket create failed: %d", errno);
		return -errno;
	}

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(port);

	ret = zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
	if (ret < 0) {
		int err = errno;
		LOG_ERR("Wi-Fi UDP command bind failed on port %u: %d", port, err);
		zsock_close(sock);
		return -err;
	}

	LOG_INF("Wi-Fi UDP command server listening on port %u", port);
	return sock;
}


struct wt_wifi_udp_stream_ctx {
	int sock;
	const struct sockaddr *peer;
	socklen_t peer_len;
	struct wt_stream_ctx stream;
	char frame[WT_WIFI_CMD_RSP_TEXT_MAX];
};

static uint16_t wifi_cmd_stream_next_id;
static uint16_t wifi_tx_stream_next_id;

static bool wt_wifi_cmd_is_full_scan_request(const char *line)
{
	char line_buf[WT_WIFI_CMD_RX_TEXT_MAX];
	char *argv_storage[16];
	char **argv = argv_storage;
	int argc;

	if (!line || line[0] == '\0') {
		return false;
	}

	strncpy(line_buf, line, sizeof(line_buf) - 1);
	line_buf[sizeof(line_buf) - 1] = '\0';

	argc = wt_split_args_quoted(line_buf, argv_storage, ARRAY_SIZE(argv_storage));
	if (argc <= 0) {
		return false;
	}

	if (argv[0][0] == '#') {
		argv++;
		argc--;
		if (argc <= 0) {
			return false;
		}
	}

	if (!strcmp(argv[0], "wt")) {
		argv++;
		argc--;
		if (argc <= 0) {
			return false;
		}
	}

	return argc >= 3 &&
	       !strcmp(argv[0], "wifi") &&
	       (!strcmp(argv[1], "scan") || !strcmp(argv[1], "apscan") || !strcmp(argv[1], "networks")) &&
	       (!strcmp(argv[2], "full") || !strcmp(argv[2], "stream"));
}

static int wt_wifi_udp_stream_send_frame(const char *frame, size_t len, void *user)
{
	struct wt_wifi_udp_stream_ctx *ctx = (struct wt_wifi_udp_stream_ctx *)user;
	int ret;

	if (!ctx || !frame) {
		return -EINVAL;
	}

	ret = zsock_sendto(ctx->sock, frame, len, 0, ctx->peer, ctx->peer_len);
	wt_leds_wifi_activity();
	if (ret < 0) {
		return -errno;
	}

	return ret == (int)len ? 0 : -EIO;
}

static int wt_wifi_udp_stream_begin(struct wt_wifi_udp_stream_ctx *ctx, uint16_t id)
{
	if (!ctx) {
		return -EINVAL;
	}

	return wt_stream_begin(&ctx->stream, ctx->frame, sizeof(ctx->frame),
			       sizeof(ctx->frame) - 1, 2,
			       wt_wifi_udp_stream_send_frame, ctx, id);
}

static int wt_wifi_udp_stream_emit_payload(const char *data, size_t len, void *user)
{
	struct wt_wifi_udp_stream_ctx *ctx = (struct wt_wifi_udp_stream_ctx *)user;

	if (!ctx) {
		return -EINVAL;
	}

	return wt_stream_write(&ctx->stream, data, len);
}

static int wt_wifi_udp_stream_write_request_prefix(struct wt_wifi_udp_stream_ctx *ctx,
						   const char *line)
{
	char request_id[24];
	size_t off = 0;
	const char *p = line;

	while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
		p++;
	}

	if (!p || *p != '#') {
		return 0;
	}

	p++;
	while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
	       off + 1 < sizeof(request_id)) {
		request_id[off++] = *p++;
	}
	request_id[off] = '\0';

	return wt_stream_write_request_id_prefix(&ctx->stream, request_id);
}

static int wt_wifi_cmd_send_scan_stream(int sock, struct sockaddr_in *peer,
					       socklen_t peer_len, const char *cmd,
					       char *rsp, size_t rsp_len)
{
	struct wt_wifi_udp_stream_ctx ctx = {
		.sock = sock,
		.peer = (const struct sockaddr *)peer,
		.peer_len = peer_len,
	};
	char *scan_argv[] = { "wifi", "scan", "json" };
	int ret;

	ret = wt_wifi_scan_command(scan_argv, ARRAY_SIZE(scan_argv), rsp, rsp_len);
	if (ret < 0) {
		wt_wifi_cmd_prefix_request_id(cmd, rsp, rsp_len);
		return zsock_sendto(sock, rsp, strlen(rsp), 0,
				       (struct sockaddr *)peer, peer_len);
	}

	ret = wt_wifi_udp_stream_begin(&ctx, wt_stream_next_id(&wifi_cmd_stream_next_id));
	if (ret) {
		return ret;
	}

	ret = wt_wifi_udp_stream_write_request_prefix(&ctx, cmd);
	if (ret) {
		return ret;
	}

	ret = wt_wifi_scan_stream_json(wt_wifi_udp_stream_emit_payload, &ctx);
	if (ret) {
		return ret;
	}

	return wt_stream_end(&ctx.stream);
}

static void wifi_cmd_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in peer;
	socklen_t peer_len;
	char cmd_buf[WT_WIFI_CMD_RX_TEXT_MAX];
	char rsp_buf[WT_WIFI_CMD_RSP_TEXT_MAX];
	int sock = -1;
	uint16_t bound_port = 0;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint16_t desired_port = wt_wifi_cmd_port();

		if (sock < 0 || bound_port != desired_port) {
			if (sock >= 0) {
				LOG_INF("Wi-Fi UDP command server closing port %u", bound_port);
				zsock_close(sock);
				sock = -1;
				bound_port = 0;
			}

			sock = wifi_cmd_socket_open(desired_port);
			if (sock < 0) {
				k_sleep(K_SECONDS(1));
				continue;
			}
			bound_port = desired_port;
		}

		struct zsock_pollfd fds[1] = {
			{ .fd = sock, .events = ZSOCK_POLLIN, .revents = 0 },
		};

		ret = zsock_poll(fds, ARRAY_SIZE(fds), 250);
		if (ret == 0) {
			continue;
		}
		if (ret < 0) {
			LOG_WRN("Wi-Fi UDP command poll failed on port %u: %d", bound_port, errno);
			k_sleep(K_MSEC(250));
			continue;
		}

		if (!(fds[0].revents & ZSOCK_POLLIN)) {
			continue;
		}

		peer_len = sizeof(peer);
		ret = zsock_recvfrom(sock, cmd_buf, sizeof(cmd_buf) - 1, 0,
					     (struct sockaddr *)&peer, &peer_len);
		if (ret < 0) {
			LOG_WRN("Wi-Fi UDP command recv failed on port %u: %d", bound_port, errno);
			k_sleep(K_MSEC(250));
			continue;
		}

		cmd_buf[ret] = '\0';
		wt_wifi_cmd_trim(cmd_buf);
		wt_leds_wifi_activity();

		if (!wifi_cmd_enabled) {
			(void)wt_wifi_cmd_rsp(rsp_buf, sizeof(rsp_buf), "err wifi cmd off");
		} else if (!wt_wifi_is_requested() || !wt_wifi_has_ipv4()) {
			(void)wt_wifi_cmd_rsp(rsp_buf, sizeof(rsp_buf),
					      "err wifi command path not ready wifi_req=%s ipv4=%s",
					      wt_onoff_txt(wt_wifi_is_requested()), wt_onoff_txt(wt_wifi_has_ipv4()));
		} else {
			LOG_INF("Wi-Fi UDP command on port %u: %s", bound_port, cmd_buf);
			if (wt_wifi_cmd_is_full_scan_request(cmd_buf)) {
				ret = wt_wifi_cmd_send_scan_stream(sock, &peer, peer_len,
								 cmd_buf, rsp_buf, sizeof(rsp_buf));
				if (ret < 0) {
					LOG_WRN("Wi-Fi UDP stream response failed: %d", ret);
				}
				continue;
			}

			ret = wt_wifi_command_execute(cmd_buf, rsp_buf, sizeof(rsp_buf));
			if (ret < 0) {
				LOG_WRN("Wi-Fi UDP command failed: %d", ret);
			}
		}

		wt_wifi_cmd_prefix_request_id(cmd_buf, rsp_buf, sizeof(rsp_buf));
		(void)zsock_sendto(sock, rsp_buf, strlen(rsp_buf), 0,
					  (struct sockaddr *)&peer, peer_len);
		wt_leds_wifi_activity();
	}
}

K_THREAD_DEFINE(wifi_cmd_thread_id, WT_WIFI_CMD_THREAD_STACK_SIZE,
		wifi_cmd_thread, NULL, NULL, NULL, WT_WIFI_CMD_THREAD_PRIORITY, 0, 0);

static void wifi_discovery_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in dst = { 0 };
	char payload[WT_WIFI_DISCOVERY_PAYLOAD_MAX];
	int sock;
	int yes = 1;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Wi-Fi discovery socket create failed: %d", errno);
		return;
	}

	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

	dst.sin_family = AF_INET;
	dst.sin_port = htons(WT_WIFI_DISCOVERY_UDP_PORT);
	dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	while (1) {
		if (wifi_discovery_enabled && wt_wifi_is_requested() && wt_wifi_has_ipv4()) {
			char ip[32];
			wt_wifi_ipv4_get(ip, sizeof(ip));
			int len = snprintk(payload, sizeof(payload),
						   "{\"type\":\"wt02e40e_discovery\",\"name\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\",\"cmd_port\":%u,\"udp_rx_port\":%u,\"uptime_s\":%lld}",
						   wt_ble_name_get(), WT_APP_FW_VERSION, ip, wt_wifi_cmd_port(),
						   WT_WIFI_DISCOVERY_UDP_PORT, (long long)(k_uptime_get() / 1000));

			if (len > 0) {
				(void)zsock_sendto(sock, payload, strlen(payload), 0,
						  (struct sockaddr *)&dst, sizeof(dst));
					wt_leds_bridge_activity();
			}
		}

		k_sleep(K_MSEC(WT_WIFI_DISCOVERY_INTERVAL_MS));
	}
}

K_THREAD_DEFINE(wifi_discovery_thread_id, 2048,
		wifi_discovery_thread, NULL, NULL, NULL, WT_WIFI_CMD_THREAD_PRIORITY, 0, 0);

static int wt_wifi_udp_transmit_prepare(const char *ip_text, const char *port_text,
						 struct sockaddr_in *dst)
{
	uint16_t port;
	int ret;

	if (!ip_text || !port_text || !dst) {
		return -EINVAL;
	}

	if (!wifi_context.ipv4_bound) {
		return -ENOTCONN;
	}

	ret = wt_parse_udp_port(port_text, &port);
	if (ret) {
		return ret;
	}

	memset(dst, 0, sizeof(*dst));
	dst->sin_family = AF_INET;
	dst->sin_port = htons(port);

	ret = zsock_inet_pton(AF_INET, ip_text, &dst->sin_addr);
	if (ret != 1) {
		return -EINVAL;
	}

	return 0;
}

int wt_wifi_udp_transmit_stream_payload(const char *ip_text, const char *port_text,
					       const uint8_t *data, size_t len)
{
	struct sockaddr_in dst;
	struct wt_wifi_udp_stream_ctx ctx;
	int sock;
	int ret;

	if (!data && len > 0) {
		return -EINVAL;
	}

	ret = wt_wifi_udp_transmit_prepare(ip_text, port_text, &dst);
	if (ret) {
		return ret;
	}

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.sock = sock;
	ctx.peer = (const struct sockaddr *)&dst;
	ctx.peer_len = sizeof(dst);

	ret = wt_wifi_udp_stream_begin(&ctx, wt_stream_next_id(&wifi_tx_stream_next_id));
	if (!ret) {
		ret = wt_stream_write(&ctx.stream, (const char *)data, len);
	}
	if (!ret) {
		ret = wt_stream_end(&ctx.stream);
	}

	zsock_close(sock);
	return ret;
}

int wt_wifi_udp_transmit_payload(const char *ip_text, const char *port_text,
						const uint8_t *data, size_t len)
{
	struct sockaddr_in dst;
	int sock;
	int ret;

	if (!data && len > 0) {
		return -EINVAL;
	}

	if (len > WT_TX_PAYLOAD_MAX) {
		return wt_wifi_udp_transmit_stream_payload(ip_text, port_text, data, len);
	}

	ret = wt_wifi_udp_transmit_prepare(ip_text, port_text, &dst);
	if (ret) {
		return ret;
	}

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}

	ret = zsock_sendto(sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst));
	if (ret >= 0) {
		wt_leds_wifi_activity();
		wt_leds_bridge_activity();
	}
	zsock_close(sock);

	if (ret < 0) {
		return -errno;
	}

	return ret == (int)len ? 0 : -EIO;
}

int wt_wifi_init(void)
{
	memset(&wifi_context, 0, sizeof(wifi_context));

	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler, WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	net_mgmt_init_event_callback(&wifi_scan_mgmt_cb, wifi_scan_event_handler,
				     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&wifi_scan_mgmt_cb);

	net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&net_mgmt_cb);

	LOG_INF("Wi-Fi management callbacks registered");
	return 0;
}
