/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_gpio.h>

#include "ibex_settings_registry.h"
#include "rgbw_led.h"

LOG_MODULE_REGISTER(led);

#define PWMRGBLEDS_NODE DT_NODELABEL(pwmrgbleds)
#define LED_OFF_LEVEL 1

enum ibex_rgbw_channel
{
	IBEX_RGBW_RED,
	IBEX_RGBW_GREEN,
	IBEX_RGBW_BLUE,
	IBEX_RGBW_WHITE,
};

static const struct device *const rgbw_leds = DEVICE_DT_GET(PWMRGBLEDS_NODE);

static struct rgbw_color current = { 0 };
/* Steam's LED brightness slider (setting 45, led_user_brightness, 0..100). */
static uint8_t user_brightness = 100;

static void rgbw_led_force_pin_off(uint32_t pin)
{
	nrf_gpio_pin_write(pin, LED_OFF_LEVEL);
	nrf_gpio_cfg_output(pin);
}

static int rgbw_led_set_channel(enum ibex_rgbw_channel channel, uint8_t brightness)
{
	uint8_t percent = (uint16_t)brightness * 100U / 255U;

	if(IS_ENABLED(CONFIG_IBEX_LED_USER_BRIGHTNESS))
	{
		percent = (uint8_t)(((uint16_t)percent * user_brightness + 99U) / 100U);
		if(brightness != 0U && user_brightness != 0U && percent == 0U)
		{
			percent = 1U;
		}
	}

	return led_set_brightness(rgbw_leds, channel, percent);
}

int rgbw_led_init(void)
{
	if(!device_is_ready(rgbw_leds))
	{
		LOG_ERR("RGBW LED device is not ready");
		return -ENODEV;
	}

	rgbw_led_off();
	LOG_DBG("RGBW LED initialized");
	return 0;
}

void rgbw_led_set(struct rgbw_color color)
{
	current = color;

	if(rgbw_led_set_channel(IBEX_RGBW_RED, current.r) ||
	   rgbw_led_set_channel(IBEX_RGBW_GREEN, current.g) ||
	   rgbw_led_set_channel(IBEX_RGBW_BLUE, current.b) ||
	   rgbw_led_set_channel(IBEX_RGBW_WHITE, current.w))
	{
		LOG_WRN("failed to set one or more LED channels");
	}
}

void rgbw_led_off(void)
{
	rgbw_led_set((struct rgbw_color){ 0 });
}

void rgbw_led_prepare_poweroff(void)
{
	rgbw_led_off();

	rgbw_led_force_pin_off(NRF_GPIO_PIN_MAP(1, 9));
	rgbw_led_force_pin_off(NRF_GPIO_PIN_MAP(0, 22));
	rgbw_led_force_pin_off(NRF_GPIO_PIN_MAP(1, 0));
	rgbw_led_force_pin_off(NRF_GPIO_PIN_MAP(1, 8));
}

void rgbw_led_load_settings(void)
{
	int16_t value;

	if(IS_ENABLED(CONFIG_IBEX_LED_USER_BRIGHTNESS) &&
	   ibex_setting_get(SETTING_LED_USER_BRIGHTNESS, &value))
	{
		user_brightness = (uint8_t)CLAMP(value, 0, 100);
	}
}

void rgbw_led_poll_settings(void)
{
	static int64_t last_check;
	int64_t now = k_uptime_get();
	int16_t value;

	if(!IS_ENABLED(CONFIG_IBEX_LED_USER_BRIGHTNESS) || now - last_check < 200)
	{
		return;
	}
	last_check = now;
	if(!ibex_setting_get(SETTING_LED_USER_BRIGHTNESS, &value))
	{
		return;
	}
	value = CLAMP(value, 0, 100);
	if((uint8_t)value != user_brightness)
	{
		user_brightness = (uint8_t)value;
		rgbw_led_set(current);
	}
}

struct rgbw_color rgbw_led_get(void)
{
	return current;
}
