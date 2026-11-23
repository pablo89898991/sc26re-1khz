/* SPDX-License-Identifier: AGPL-3.0-or-later */

#pragma once

#include "rgbw_led.h"

enum charge_mode_result
{
	CHARGE_MODE_SKIPPED,
	CHARGE_MODE_USB_CONFIGURED,
	CHARGE_MODE_POWER_ON_RADIO,
};

enum charge_mode_result charge_mode_run_if_needed(void);
void charge_mode_dock_led_poll(void);
bool charge_mode_dock_led_active(void);
void charge_mode_dock_debug(uint8_t out[5]);
/* the LED colour under the Puck's charge light: read it, or change it (shown once lifted).
 * Outside the Puck they act on the LED directly (v51.4 test 23). */
struct rgbw_color charge_mode_led_base_get(void);
void charge_mode_led_base_set(struct rgbw_color c);
