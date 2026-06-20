#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "wt_app.h"
#include "wt_ble.h"
#include "wt_radio.h"
#include "wt_wifi.h"

LOG_MODULE_REGISTER(wt_radio, CONFIG_LOG_DEFAULT_LEVEL);

int wt_radio_mode_apply(const char *mode)
{
	if (!strcmp(mode, "idle") || !strcmp(mode, "off")) {
		(void)wt_wifi_service_set(false);
		(void)wt_ble_service_stop();
		LOG_INF("Radio mode: idle");
		return 0;
	}

	if (!strcmp(mode, "ble") || !strcmp(mode, "bt")) {
		(void)wt_wifi_service_set(false);
		return wt_ble_service_start();
	}

	if (!strcmp(mode, "wifi") || !strcmp(mode, "wi-fi")) {
		(void)wt_ble_service_stop();
		return wt_wifi_service_set(true);
	}

	if (!strcmp(mode, "both")) {
		int ret_ble = wt_ble_service_start();
		int ret_wifi = wt_wifi_service_set(true);

		return ret_ble ? ret_ble : ret_wifi;
	}

	LOG_ERR("Unknown mode: %s", mode);
	return -EINVAL;
}

void wt_radio_apply_default_mode(void)
{
	const char *mode = wt_app_boot_mode_get();

	LOG_INF("Applying boot radio mode: %s", mode);
	(void)wt_radio_mode_apply(mode);
}
