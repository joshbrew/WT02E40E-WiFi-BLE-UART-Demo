/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief WT02E40E Wi-Fi/BLE command bridge entry point.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "wt_ble.h"
#include "wt_leds.h"
#include "wt_radio.h"
#include "wt_wifi.h"

LOG_MODULE_REGISTER(wt_main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("Starting %s with CPU frequency: %d MHz", CONFIG_BOARD, SystemCoreClock / MHZ(1));

	wt_leds_start();
	wt_ble_init();

	ret = wt_wifi_init();
	if (ret) {
		return ret;
	}

#ifdef CONFIG_WIFI_READY_LIB
	ret = wt_wifi_register_ready_callback();
	if (ret) {
		return ret;
	}
	wt_wifi_start_worker();
#else
	LOG_ERR("CONFIG_WIFI_READY_LIB is required for this command sample");
	return -ENOTSUP;
#endif

	k_sleep(K_SECONDS(1));
	wt_radio_apply_default_mode();

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
