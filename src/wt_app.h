#ifndef WT_APP_H
#define WT_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WT_APP_FW_VERSION "0.3.5-parserfix2"

int wt_app_settings_init(void);
int wt_app_config_save(void);
int wt_app_config_reset(void);

const char *wt_app_boot_mode_get(void);
int wt_app_boot_mode_set(const char *mode);

int wt_app_version_format(char *buf, size_t size);
int wt_app_id_format(char *buf, size_t size);
int wt_app_config_format(char *buf, size_t size, bool json);
int wt_app_ble_status_format(char *buf, size_t size, bool json);
int wt_app_wifi_status_format(char *buf, size_t size, bool json);
int wt_app_fw_status_format(char *buf, size_t size);

int wt_app_parse_delay_ms(const char *text, uint32_t *delay_ms);
int wt_app_delayed_radio_apply(const char *mode_or_action, uint32_t delay_ms);
int wt_app_reboot_schedule(bool bootloader, uint32_t delay_ms);

bool wt_app_bridge_ble_enabled(void);
bool wt_app_bridge_uart_enabled(void);
bool wt_app_bridge_wifi_enabled(void);
const char *wt_app_bridge_target_ip_get(void);
uint16_t wt_app_bridge_target_port_get(void);
int wt_app_bridge_set(const char *path, bool enable);
int wt_app_bridge_target_set(const char *ip, const char *port_text);
int wt_app_bridge_status_format(char *buf, size_t size, bool json);
int wt_app_bridge_send(const uint8_t *data, size_t len, char *rsp, size_t rsp_len);

int wt_app_ping_execute(char **argv, size_t argc, char *rsp, size_t rsp_len);
int wt_app_boot_command(char **argv, size_t argc, char *rsp, size_t rsp_len);
int wt_app_bridge_command(char **argv, size_t argc, char *rsp, size_t rsp_len);
int wt_app_fw_command(char **argv, size_t argc, char *rsp, size_t rsp_len);

#endif /* WT_APP_H */
