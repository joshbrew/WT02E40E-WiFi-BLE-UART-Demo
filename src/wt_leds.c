#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wt_ble.h"
#include "wt_config.h"
#include "wt_leds.h"
#include "wt_wifi.h"

LOG_MODULE_REGISTER(wt_leds, CONFIG_LOG_DEFAULT_LEVEL);

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)

static const struct gpio_dt_spec led_boot = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led_wifi = GPIO_DT_SPEC_GET(LED1_NODE, gpios);

static int configure_indicator_led(const struct gpio_dt_spec *led, const char *name)
{
	int ret;

	if (!gpio_is_ready_dt(led)) {
		LOG_ERR("%s GPIO device is not ready", name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure %s LED: %d", name, ret);
		return ret;
	}

	return 0;
}

static void set_indicator_led(const struct gpio_dt_spec *led, int on)
{
	if (gpio_is_ready_dt(led)) {
		gpio_pin_set_dt(led, on);
	}
}

static void indicator_leds_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t tick = 0;

	configure_indicator_led(&led_boot, "blue/boot");
	configure_indicator_led(&led_wifi, "green/wifi");

	while (1) {
		const bool heartbeat = (tick & 1U) != 0U;
		const bool wifi_fast_blink = (tick & 1U) != 0U;
		const bool wifi_slow_blink = ((tick / 4U) & 1U) != 0U;
		int wifi_led_on = 0;

		if (wt_ble_is_connected()) {
			set_indicator_led(&led_boot, 1);
		} else {
			set_indicator_led(&led_boot, heartbeat);
		}

		if (wt_wifi_scan_is_running()) {
			wifi_led_on = wifi_fast_blink;
		} else if (wt_wifi_has_ipv4()) {
			wifi_led_on = 1;
		} else if (wt_wifi_is_requested() || wt_wifi_is_associated()) {
			wifi_led_on = wifi_slow_blink;
		}

		set_indicator_led(&led_wifi, wifi_led_on);

		tick++;
		k_msleep(LED_SLEEP_TIME_MS);
	}
}

K_THREAD_DEFINE(led_thread_id, 1024, indicator_leds_thread, NULL, NULL, NULL, 7, 0, 0);

void wt_leds_start(void)
{
	/* LED thread starts automatically through K_THREAD_DEFINE. */
}
