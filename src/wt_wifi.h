#ifndef WT_WIFI_H
#define WT_WIFI_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

bool wt_wifi_is_requested(void);
bool wt_wifi_is_associated(void);
bool wt_wifi_has_ipv4(void);

int wt_wifi_init(void);
int wt_wifi_service_set(bool enable);
int wt_wifi_status_log(void);
int wt_wifi_credentials_print_list(const struct shell *sh);
int wt_wifi_credentials_format_list(char *buf, size_t buf_len);
int wt_wifi_credentials_set(const char *ssid, const char *password, const char *security_text, bool replace_all);
int wt_wifi_credentials_open(const char *ssid, bool replace_all);
int wt_wifi_credentials_forget(const char *ssid);
int wt_wifi_credentials_clear(void);
int wt_wifi_credentials_shell(const struct shell *sh, size_t argc, char **argv);
int wt_wifi_reconnect_if_requested(void);
int wt_wifi_udp_transmit_payload(const char *ip_text, const char *port_text,
					const uint8_t *data, size_t len);

#ifdef CONFIG_WIFI_READY_LIB
int wt_wifi_register_ready_callback(void);
void wt_wifi_start_worker(void);
#else
static inline int wt_wifi_register_ready_callback(void)
{
	return -ENOTSUP;
}

static inline void wt_wifi_start_worker(void)
{
}
#endif

#endif /* WT_WIFI_H */
