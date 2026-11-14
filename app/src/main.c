/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "battery.h"
#include "boot_guard.h"
#include "charge_mode.h"
#include "haptics.h"
#include "controller.h"
#include "ibex_settings_registry.h"
#include "lizard.h"
#include "power.h"
#include "puck_interface.h"
#include "valve_settings.h"
#include "watchdog.h"

#if CONFIG_IBEX_RGBW_LED
#include "rgbw_led.h"
#endif

#define BOOT_CHORD_SETTLE_MS 150
#define PERSONALITY_PERSIST_DELAY_MS 60000
#define STEAM_POWER_HOLD_MS 5000
#define INPUT_REFRESH_MS 15
#define INACTIVITY_STICK_THRESHOLD 4000
#define INACTIVITY_PHYSICAL_BUTTONS BIT_MASK(20)
#define INACTIVITY_TOUCH_BUTTONS \
	(BIT(CONTROLLER_BUTTON_RIGHT_STICK_TOUCH) | \
	 BIT(CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH) | \
	 BIT(CONTROLLER_BUTTON_LEFT_STICK_TOUCH) | BIT(CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH))

LOG_MODULE_REGISTER(app);

static bool chord_pressed(uint32_t buttons, enum controller_button first,
                          enum controller_button second)
{
	uint32_t mask = BIT(first) | BIT(second);

	return (buttons & mask) == mask;
}

static bool report_has_activity(const struct controller_report *report)
{
	if((report->buttons & (INACTIVITY_PHYSICAL_BUTTONS | INACTIVITY_TOUCH_BUTTONS)) != 0U ||
	   report->trigger_left != 0 ||
	   report->trigger_right != 0)
	{
		return true;
	}

	return abs(report->stick_left_x) > INACTIVITY_STICK_THRESHOLD ||
	       abs(report->stick_left_y) > INACTIVITY_STICK_THRESHOLD ||
	       abs(report->stick_right_x) > INACTIVITY_STICK_THRESHOLD ||
	       abs(report->stick_right_y) > INACTIVITY_STICK_THRESHOLD;
}

static enum radio_personality boot_chord_personality(void)
{
	uint32_t buttons;

	k_sleep(K_MSEC(BOOT_CHORD_SETTLE_MS));
	buttons = hardware_read_buttons();

	/* BLE wins the three-button chord so A+B also opens its pairing window. */
	if(chord_pressed(buttons, CONTROLLER_BUTTON_RIGHT_SHOULDER, CONTROLLER_BUTTON_B))
	{
		return RADIO_PERSONALITY_BLE;
	}
	if(chord_pressed(buttons, CONTROLLER_BUTTON_RIGHT_SHOULDER, CONTROLLER_BUTTON_A))
	{
		return RADIO_PERSONALITY_ESB;
	}

	return radio_personality_get();
}

/*
 * Settings reset chord: hold Menu + View + LB + RB while switching on, keep
 * holding for 3 s (orange blinking LED, then red + chime). Forgets sc26re's calibration and
 * settings and all Puck/BT pairings (hard reset); stock calibration returns.
 */
#define FACTORY_RESET_HOLD_MS 3000

static bool factory_reset_chord(uint32_t buttons)
{
	uint32_t mask = BIT(CONTROLLER_BUTTON_MENU) | BIT(CONTROLLER_BUTTON_VIEW) |
	                BIT(CONTROLLER_BUTTON_LEFT_SHOULDER) | BIT(CONTROLLER_BUTTON_RIGHT_SHOULDER);

	return (buttons & mask) == mask;
}

static void factory_reset_feedback(uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_IBEX_RGBW_LED
	rgbw_led_set((struct rgbw_color){ r, g, b, 0 });
#else
	ARG_UNUSED(r);
	ARG_UNUSED(g);
	ARG_UNUSED(b);
#endif
}

static void boot_factory_reset_check(void)
{
	int64_t start;
	int n;

	k_sleep(K_MSEC(BOOT_CHORD_SETTLE_MS));
	if(!factory_reset_chord(hardware_read_buttons()))
	{
		return;
	}
	LOG_INF("settings reset chord held; keep holding to confirm");
	/* detected: orange LED + pairing chime */
	factory_reset_feedback(255, 80, 0);
	(void)haptics_play_system_sound(HAPTICS_SOUND_PAIRING);
	start = k_uptime_get();
	while(k_uptime_get() - start < FACTORY_RESET_HOLD_MS)
	{
		watchdog_feed();
		k_sleep(K_MSEC(50));
		/* blink orange while counting down */
		if(((k_uptime_get() - start) / 250) % 2)
		{
			factory_reset_feedback(0, 0, 0);
		}
		else
		{
			factory_reset_feedback(255, 80, 0);
		}
		if(!factory_reset_chord(hardware_read_buttons()))
		{
			LOG_INF("settings reset chord released early: cancelled");
			factory_reset_feedback(0, 0, 0);
			return;
		}
	}
	n = valve_settings_factory_reset();
	LOG_INF("settings reset done (%d)", n);
	/* done: red LED + paired chime, then reboot */
	factory_reset_feedback(255, 0, 0);
	(void)haptics_play_system_sound(HAPTICS_SOUND_PAIRED);
	for(int i = 0; i < 10; ++i)
	{
		watchdog_feed();
		k_sleep(K_MSEC(100));
	}
	(void)power_reboot_normal();
}

static void set_boot_led(bool usb_radio_off)
{
#if CONFIG_IBEX_RGBW_LED
	if(usb_radio_off)
	{
		rgbw_led_set((struct rgbw_color){ 0, 255, 0, 0 });
	}
	else if(radio_personality_get() == RADIO_PERSONALITY_BLE)
	{
		rgbw_led_set((struct rgbw_color){ 0, 0, 255, 0 });
	}
	else
	{
		rgbw_led_set((struct rgbw_color){ 0, 0, 0, 255 });
	}
#endif
}

#if CONFIG_IBEX_RGBW_LED
/*
 * OFW 6AE90BC5 LED thread (0x17b4e), wireless mode:
 *  - connected: steady colour
 *  - pairing window: double flash, 75 ms on / 75 off / 75 on / 300 off
 *  - not connected: slow blink, 100 ms fade in, 900 ms on, 100 ms fade out, 900 ms off
 * The blink scales the current colour, so Steam's colour and the dock LED are untouched.
 */
static void link_led_poll(bool wireless)
{
	static int64_t phase_start_ms;
	static uint8_t last_state;
	int64_t now = k_uptime_get();
	uint8_t state = 0U; /* 0 steady, 1 searching, 2 pairing */
	uint32_t t;
	uint8_t level = 100U;

	if(wireless && !charge_mode_dock_led_active())
	{
		bool ble = radio_personality_get() == RADIO_PERSONALITY_BLE;
		bool connected = ble ? transport_ble_connected() : transport_esb_connected();

		if(!connected)
		{
			state = ble && transport_ble_pairing_window_open() ? 2U : 1U;
		}
	}
	if(state != last_state)
	{
		last_state = state;
		phase_start_ms = now;
	}
	t = (uint32_t)(now - phase_start_ms);
	if(state == 1U)
	{
		t %= 2000U;
		level = t < 100U ? (uint8_t)(t) : t < 1000U ? 100U : t < 1100U ? (uint8_t)(1100U - t) : 0U;
	}
	else if(state == 2U)
	{
		t %= 525U;
		level = (t < 75U || (t >= 150U && t < 225U)) ? 100U : 0U;
	}
	rgbw_led_set_indicator_level(level);
}
#endif

int main(void)
{
	struct controller_report previous = { 0 };
	int64_t steam_button_since = 0;
	int64_t last_input_sent = 0;
	int64_t last_activity_ms;
	int16_t inactivity_timeout_seconds = 0;
	enum charge_mode_result charge_result = CHARGE_MODE_SKIPPED;
	bool usb_vbus_charge_radio;
	bool first = true;
	int err;

	power_capture_boot_state();
	boot_guard_begin();

	if(IS_ENABLED(CONFIG_SETTINGS))
	{
		err = settings_subsys_init();
		if(err)
		{
			boot_guard_fatal("settings init", err);
		}
	}
	radio_personality_init();
	valve_settings_load_feature_state();
	ibex_settings_registry_init();
	err = lizard_init();
	if(err)
	{
		boot_guard_fatal("lizard init", err);
	}

	err = hardware_init();
	if(err)
	{
		boot_guard_fatal("hardware init", err);
	}
	err = puck_interface_init();
	if(err)
	{
		LOG_WRN("puck interface initialization failed: %d", err);
	}

	if(IS_ENABLED(CONFIG_IBEX_BATTERY))
	{
		err = battery_init();
		if(err)
		{
			LOG_WRN("battery initialization failed: %d", err);
		}
	}

	if(IS_ENABLED(CONFIG_IBEX_USB_HID))
	{
		err = transport_usb_init();
		if(err)
		{
			boot_guard_fatal("USB init", err);
		}
	}
	charge_result = charge_mode_run_if_needed();
	usb_vbus_charge_radio = charge_result == CHARGE_MODE_POWER_ON_RADIO &&
	                        transport_usb_attached() &&
	                        !transport_usb_configured();

	boot_factory_reset_check();
	if(transport_usb_attached() && !usb_vbus_charge_radio)
	{
		LOG_INF("USB attached; using radio-off USB mode without persisting personality");
	}
	else
	{
		enum radio_personality requested_personality = boot_chord_personality();

		if(requested_personality != radio_personality_get())
		{
			LOG_INF("boot chord selected %s personality",
			        requested_personality == RADIO_PERSONALITY_ESB ? "ESB" : "BLE");
			/* an explicit boot chord is a deliberate choice: save it now instead of
			 * after the 60 s confirmation, so turning the controller off soon after
			 * does not fall back to the previous mode */
			radio_personality_persist_after(requested_personality, 0);
			radio_personality_reboot_into(requested_personality);
		}
		radio_personality_persist_after(radio_personality_get(), PERSONALITY_PERSIST_DELAY_MS);
	}
#if CONFIG_IBEX_RGBW_LED
	rgbw_led_load_settings();
#endif
	set_boot_led(transport_usb_attached() && !usb_vbus_charge_radio);

	if(boot_guard_safe_mode())
	{
		/* earlier boots never got stable: start without the radio this time */
		LOG_WRN("safe start after %u failed boots: radio not started", boot_guard_attempts());
		err = 0;
	}
	else if(usb_vbus_charge_radio)
	{
		err = transport_set_usb_radio_mode(TRANSPORT_USB_RADIO_VBUS_CHARGE);
	}
	else
	{
		err = transport_init();
	}
	if(err)
	{
		/* keep running (USB, power button) instead of returning from main() */
		LOG_ERR("transport initialization failed: %d; continuing without radio", err);
	}

	LOG_INF("SC26re started in %s mode", radio_personality_name());
	/* stock power-on / connection chime (also after "continue wireless" reboots, like OFW) */
	(void)haptics_play_system_sound(HAPTICS_SOUND_POWER_ON);
	(void)ibex_setting_get(SETTING_SLEEP_INACTIVITY_TIMEOUT, &inactivity_timeout_seconds);
	last_activity_ms = k_uptime_get();

	for(;;)
	{
		struct controller_report current;
		int16_t timeout_seconds = inactivity_timeout_seconds;
		int64_t now_ms;

		hardware_read_report(&current);
		now_ms = k_uptime_get();
#if CONFIG_IBEX_RGBW_LED
		rgbw_led_poll_settings();
		charge_mode_dock_led_poll();
		link_led_poll(!(transport_usb_attached() && !usb_vbus_charge_radio) &&
		              !boot_guard_safe_mode());
#endif
		if(ibex_setting_get(SETTING_SLEEP_INACTIVITY_TIMEOUT, &timeout_seconds) &&
		   timeout_seconds != inactivity_timeout_seconds)
		{
			inactivity_timeout_seconds = timeout_seconds;
			last_activity_ms = now_ms;
		}
		if(report_has_activity(&current) || transport_usb_attached() || puck_interface_active())
		{
			last_activity_ms = now_ms;
		}
		else if(inactivity_timeout_seconds > 0 &&
		        now_ms - last_activity_ms >= (int64_t)inactivity_timeout_seconds * MSEC_PER_SEC)
		{
			LOG_INF("input inactive for %d seconds: shutting down", inactivity_timeout_seconds);
			last_activity_ms = now_ms;
			power_off();
		}
		if((current.buttons & BIT(CONTROLLER_BUTTON_STEAM)) != 0U)
		{
			if(steam_button_since == 0)
			{
				steam_button_since = k_uptime_get();
			}
			else if(k_uptime_get() - steam_button_since >= STEAM_POWER_HOLD_MS)
			{
				LOG_INF("Steam held: shutting down");
				power_off_after_buttons_released(BIT(CONTROLLER_BUTTON_STEAM));
			}
		}
		else
		{
			steam_button_since = 0;
		}

		bool changed = first || memcmp(&current, &previous, sizeof(current)) != 0;
		bool refresh_due = k_uptime_get() - last_input_sent >= INPUT_REFRESH_MS;
		/* OFW uses each full-state ESB report as the container boundary for lizard
		 * reports produced by the preceding input sample. */
		bool esb_state_boundary = transport_esb_connected();

		if(changed || refresh_due || esb_state_boundary)
		{
			if(changed && (current.buttons & ~previous.buttons) != 0U)
			{
				hardware_haptic_pulse();
			}

			err = transport_send(&current);
			if(err && err != -ENOTCONN)
			{
				LOG_WRN("report send failed: %d", err);
			}
			else
			{
				last_input_sent = k_uptime_get();
			}
			previous = current;
			first = false;
		}
		/* OFW sends the full state before waking lizard input processing. */
		lizard_update(&current);

		hardware_wait_for_change();
		watchdog_feed();
		boot_guard_loop();
	}
}
