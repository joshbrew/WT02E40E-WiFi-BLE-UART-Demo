#ifndef WT_LEDS_H
#define WT_LEDS_H

#include <stddef.h>

void wt_leds_start(void);
void wt_leds_ble_activity(void);
void wt_leds_ble_report_activity(void);
void wt_leds_wifi_activity(void);
void wt_leds_bridge_activity(void);
void wt_leds_error_activity(void);
int wt_leds_test(const char *target);
int wt_leds_status_format(char *buf, size_t size);

#endif /* WT_LEDS_H */
