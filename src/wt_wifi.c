#include <errno.h>
#include <stdbool.h>
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
#include <zephyr/sys/util.h>

#ifdef CONFIG_WIFI_READY_LIB
#include <net/wifi_ready.h>
#endif

#if defined(CONFIG_BOARD_NRF7002DK_NRF5340_CPUAPP_NRF7001) || \
	defined(CONFIG_BOARD_NRF7002DK_NRF5340_CPUAPP)
#include <zephyr/drivers/wifi/nrf_wifi/bus/qspi_if.h>
#endif

#include "net_private.h"

#include "wt_common.h"
#include "wt_config.h"
#include "wt_wifi.h"

#define WIFI_NODE DT_CHOSEN(zephyr_wifi)
static const uint8_t wifi_mac_addr[6] = DT_PROP_OR(WIFI_NODE, local_mac_address, { 0 });

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback net_mgmt_cb;
static K_SEM_DEFINE(wifi_control_sem, 0, 1);

#ifdef CONFIG_WIFI_READY_LIB
static K_SEM_DEFINE(wifi_ready_state_changed_sem, 0, 1);
static bool wifi_ready_status;
#endif

static volatile bool wifi_requested;
static bool wifi_prepared;

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
		wifi_context.connected = false;
		wifi_context.ipv4_bound = false;
	} else {
		LOG_INF("Connected");
		wifi_context.connected = true;
		wifi_context.ipv4_bound = false;
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
	wifi_context.connect_result = false;

	wt_wifi_status_log();
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
	char dhcp_info[128];

	net_addr_ntop(AF_INET, addr, dhcp_info, sizeof(dhcp_info));

	wifi_context.ipv4_bound = true;
	LOG_INF("DHCP IP address: %s", dhcp_info);
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

int wt_wifi_udp_transmit_payload(const char *ip_text, const char *port_text,
					const uint8_t *data, size_t len)
{
	struct sockaddr_in dst = { 0 };
	uint16_t port;
	int sock;
	int ret;

	if (!wifi_context.ipv4_bound) {
		return -ENOTCONN;
	}

	ret = wt_parse_udp_port(port_text, &port);
	if (ret) {
		return ret;
	}

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}

	dst.sin_family = AF_INET;
	dst.sin_port = htons(port);

	ret = zsock_inet_pton(AF_INET, ip_text, &dst.sin_addr);
	if (ret != 1) {
		zsock_close(sock);
		return -EINVAL;
	}

	ret = zsock_sendto(sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst));
	zsock_close(sock);

	if (ret < 0) {
		return -errno;
	}

	return ret == len ? 0 : -EIO;
}

int wt_wifi_init(void)
{
	memset(&wifi_context, 0, sizeof(wifi_context));

	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler, WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&net_mgmt_cb);

	LOG_INF("Wi-Fi management callbacks registered");
	return 0;
}
