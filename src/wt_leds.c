#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "wt_ble.h"
#include "wt_config.h"
#include "wt_leds.h"
#include "wt_wifi.h"

LOG_MODULE_REGISTER(wt_leds, CONFIG_LOG_DEFAULT_LEVEL);

#define WT_LED_MASK_BLE BIT(0)
#define WT_LED_MASK_WIFI BIT(1)
#define WT_LED_MASK_ACTIVITY BIT(2)
#define WT_LED_MASK_ALERT BIT(3)
#define WT_LED_ACTIVITY_TICKS 4
#define WT_LED_BLE_REPORT_TICKS 2
#define WT_LED_ALERT_TICKS 12
#define WT_LED_TEST_TICKS 24

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec led_ble = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define WT_HAS_LED_BLE 1
#else
#define WT_HAS_LED_BLE 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay)
static const struct gpio_dt_spec led_wifi = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
#define WT_HAS_LED_WIFI 1
#else
#define WT_HAS_LED_WIFI 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay)
static const struct gpio_dt_spec led_activity = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
#define WT_HAS_LED_ACTIVITY 1
#else
#define WT_HAS_LED_ACTIVITY 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(led3), okay)
static const struct gpio_dt_spec led_alert = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
#define WT_HAS_LED_ALERT 1
#else
#define WT_HAS_LED_ALERT 0
#endif

static atomic_t ble_activity_ticks;
static atomic_t ble_report_ticks;
static atomic_t wifi_activity_ticks;
static atomic_t bridge_activity_ticks;
static atomic_t alert_activity_ticks;
static atomic_t test_ticks;
static atomic_t test_mask;

static int configure_indicator_led(const struct gpio_dt_spec *led, const char *name)
{
	int ret;

	if (!led) {
		return -ENODEV;
	}

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
	if (led && gpio_is_ready_dt(led)) {
		gpio_pin_set_dt(led, on);
	}
}

static bool consume_pulse(atomic_t *ticks)
{
	atomic_val_t value = atomic_get(ticks);

	if (value <= 0) {
		return false;
	}

	atomic_dec(ticks);
	return true;
}

static bool led_test_applies(uint32_t mask, uint32_t bit)
{
	return (mask & bit) != 0U;
}

static bool led_target_mask(const char *target, uint32_t *mask)
{
	if (!target || !mask) {
		return false;
	}

	if (!strcmp(target, "all") || !strcmp(target, "test")) {
		*mask = WT_LED_MASK_BLE | WT_LED_MASK_WIFI | WT_LED_MASK_ACTIVITY | WT_LED_MASK_ALERT;
		return true;
	}
	if (!strcmp(target, "ble") || !strcmp(target, "bt")) {
		*mask = WT_LED_MASK_BLE;
		return true;
	}
	if (!strcmp(target, "wifi") || !strcmp(target, "wi-fi")) {
		*mask = WT_LED_MASK_WIFI;
		return true;
	}
	if (!strcmp(target, "bridge") || !strcmp(target, "activity") || !strcmp(target, "tx")) {
		*mask = WT_LED_MASK_ACTIVITY;
		return true;
	}
	if (!strcmp(target, "alert") || !strcmp(target, "error") || !strcmp(target, "err")) {
		*mask = WT_LED_MASK_ALERT;
		return true;
	}

	return false;
}

static void indicator_leds_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t tick = 0;

#if WT_HAS_LED_BLE
	(void)configure_indicator_led(&led_ble, "ble/status");
#endif
#if WT_HAS_LED_WIFI
	(void)configure_indicator_led(&led_wifi, "wifi/status");
#endif
#if WT_HAS_LED_ACTIVITY
	(void)configure_indicator_led(&led_activity, "bridge/activity");
#endif
#if WT_HAS_LED_ALERT
	(void)configure_indicator_led(&led_alert, "alert/error");
#endif

	while (1) {
		const bool fast = (tick & 1U) != 0U;
		const bool medium = ((tick / 2U) & 1U) != 0U;
		const bool slow = ((tick / 4U) & 1U) != 0U;
		const bool heartbeat = (tick % 8U) == 0U;
		const bool double_pulse = (tick % 12U) == 0U || (tick % 12U) == 2U;
		bool ble_led_on = false;
		bool wifi_led_on = false;
		bool activity_led_on = false;
		bool alert_led_on = false;
		atomic_val_t active_test_ticks = atomic_get(&test_ticks);
		uint32_t active_test_mask = (uint32_t)atomic_get(&test_mask);

		if (wt_ble_is_connected()) {
			ble_led_on = true;
		} else if (wt_ble_is_advertising()) {
			ble_led_on = medium;
		} else if (wt_ble_is_requested() && wt_ble_is_ready()) {
			ble_led_on = heartbeat;
		} else if (wt_ble_is_requested()) {
			ble_led_on = slow;
		}

		if (wt_wifi_scan_is_running()) {
			wifi_led_on = fast;
		} else if (wt_wifi_has_ipv4()) {
			wifi_led_on = true;
		} else if (wt_wifi_is_associated()) {
			wifi_led_on = double_pulse;
		} else if (wt_wifi_is_requested()) {
			wifi_led_on = slow;
		}

		if (consume_pulse(&ble_activity_ticks)) {
			ble_led_on = true;
			activity_led_on = true;
		}
		if (consume_pulse(&ble_report_ticks)) {
			ble_led_on = wt_ble_is_connected() ? false : true;
			activity_led_on = true;
		}
		if (consume_pulse(&wifi_activity_ticks)) {
			wifi_led_on = true;
			activity_led_on = true;
		}
		if (consume_pulse(&bridge_activity_ticks)) {
			activity_led_on = true;
		}
		if (consume_pulse(&alert_activity_ticks)) {
			alert_led_on = fast;
		}

		if (active_test_ticks > 0) {
			atomic_dec(&test_ticks);
			if (led_test_applies(active_test_mask, WT_LED_MASK_BLE)) {
				ble_led_on = fast;
			}
			if (led_test_applies(active_test_mask, WT_LED_MASK_WIFI)) {
				wifi_led_on = fast;
			}
			if (led_test_applies(active_test_mask, WT_LED_MASK_ACTIVITY)) {
				activity_led_on = fast;
			}
			if (led_test_applies(active_test_mask, WT_LED_MASK_ALERT)) {
				alert_led_on = fast;
			}
		}

#if WT_HAS_LED_BLE
		set_indicator_led(&led_ble, ble_led_on ? 1 : 0);
#endif
#if WT_HAS_LED_WIFI
		set_indicator_led(&led_wifi, wifi_led_on ? 1 : 0);
#endif
#if WT_HAS_LED_ACTIVITY
		set_indicator_led(&led_activity, activity_led_on ? 1 : 0);
#endif
#if WT_HAS_LED_ALERT
		set_indicator_led(&led_alert, alert_led_on ? 1 : 0);
#endif

		tick++;
		k_msleep(LED_SLEEP_TIME_MS);
	}
}

K_THREAD_DEFINE(led_thread_id, 1024, indicator_leds_thread, NULL, NULL, NULL, 7, 0, 0);

void wt_leds_start(void)
{
	/* LED thread starts automatically through K_THREAD_DEFINE. */
}

void wt_leds_ble_activity(void)
{
	atomic_set(&ble_activity_ticks, WT_LED_ACTIVITY_TICKS);
}

void wt_leds_ble_report_activity(void)
{
	atomic_set(&ble_report_ticks, WT_LED_BLE_REPORT_TICKS);
}

void wt_leds_wifi_activity(void)
{
	atomic_set(&wifi_activity_ticks, WT_LED_ACTIVITY_TICKS);
}

void wt_leds_bridge_activity(void)
{
	atomic_set(&bridge_activity_ticks, WT_LED_ACTIVITY_TICKS);
}

void wt_leds_error_activity(void)
{
	atomic_set(&alert_activity_ticks, WT_LED_ALERT_TICKS);
}

int wt_leds_test(const char *target)
{
	uint32_t mask;

	if (!target || target[0] == '\0') {
		target = "all";
	}

	if (!led_target_mask(target, &mask)) {
		return -EINVAL;
	}

	atomic_set(&test_mask, (atomic_val_t)mask);
	atomic_set(&test_ticks, WT_LED_TEST_TICKS);
	return 0;
}

int wt_leds_status_format(char *buf, size_t size)
{
	if (!buf || size == 0) {
		return -EINVAL;
	}

	return snprintk(buf, size,
		"led status ble=conn solid/cmd-rsp+tx dip/adv blink/ready pulse wifi=ip solid/scan fast/assoc double/req slow activity=tx pulse alert=error pulse leds=%d%d%d%d",
		WT_HAS_LED_BLE, WT_HAS_LED_WIFI, WT_HAS_LED_ACTIVITY, WT_HAS_LED_ALERT);
}
