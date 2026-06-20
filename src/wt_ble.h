#ifndef WT_BLE_H
#define WT_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool wt_ble_is_requested(void);
bool wt_ble_is_ready(void);
bool wt_ble_is_advertising(void);
bool wt_ble_is_connected(void);
bool wt_ble_tx_notify_is_enabled(void);
bool wt_ble_status_notify_is_enabled(void);
bool wt_ble_cmd_response_notify_is_enabled(void);

void wt_ble_init(void);
int wt_ble_service_start(void);
int wt_ble_service_stop(void);
int wt_ble_transmit_payload(const uint8_t *data, size_t len);
int wt_ble_status_format(char *buf, size_t size);

#endif /* WT_BLE_H */
