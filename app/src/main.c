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
#include "analog.h"
#include "imu.h"
#include "gyro_mouse.h"
#include "remap.h"
#include "lizard.h"
#include "power.h"
#include "puck_interface.h"
#include "sc26re_cfg.h"
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
	   analog_triggers_active())
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

/*
 * Full gyro calibration, from the chord (hold LB + RB + X + Y for 3 s) or the
 * report meter. The LED turns purple and a chime plays; put the controller
 * down and let go. With nothing touching it (grips, sticks, pads, buttons):
 *   1. measure the gyro at rest for 5 s and use it as the pitch/yaw/roll dead bands
 *   2. calibrate the gyro (bias)
 *   3. measure at rest again (now with the new bias) -> dead bands
 *   4. calibrate the gyro again
 *   5. save the dead bands (the bias is saved by each calibration)
 * A touch or movement repeats the current step once it is still again. A second
 * chime confirms; after 60 s without success the power-off sound plays and the
 * old calibration stays.
 */
#define GYRO_CHORD_HOLD_MS 3000
#define GYRO_CHORD_HANDS_OFF_MS 700
#define GYRO_CHORD_TIMEOUT_MS 60000
#define GYRO_BAND_SAMPLES 5000 /* ~5 s at 1 kHz */
#define GYRO_BAND_MARGIN 5     /* 0.05 deg/s */

enum gyro_chord_state
{
	GYRO_CHORD_IDLE,
	GYRO_CHORD_HOLDING,
	GYRO_CHORD_WAIT_HANDS_OFF,
	GYRO_CHORD_RUNNING,
};

static void gyro_bands_apply(bool save)
{
	int16_t lo[3];
	int16_t hi[3];

	imu_band_measure_result(lo, hi, GYRO_BAND_MARGIN);
	for(size_t i = 0; i < 3; ++i)
	{
		(void)sc26re_cfg_set(SC26RE_CFG_GYRO_BAND_X_LO + 2 * i, lo[i], save);
		(void)sc26re_cfg_set(SC26RE_CFG_GYRO_BAND_X_HI + 2 * i, hi[i], save);
	}
	LOG_INF("gyro dead bands (0.01 deg/s): x %d..%d y %d..%d z %d..%d%s", lo[0], hi[0], lo[1],
	        hi[1], lo[2], hi[2], save ? " saved" : "");
}

static void gyro_chord_finish(bool ok, struct rgbw_color saved_led)
{
	(void)haptics_play_system_sound(ok ? HAPTICS_SOUND_PAIRED : HAPTICS_SOUND_POWER_OFF);
#if CONFIG_IBEX_RGBW_LED
	rgbw_led_set(saved_led);
#else
	ARG_UNUSED(saved_led);
#endif
	imu_force_on(false);
	imu_set_full_cal_step(ok ? 6 : 7);
	LOG_INF("full gyro calibration: %s", ok ? "done" : "failed / timed out");
}

/* start step 1..4; step 5 (save) is immediate */
static void gyro_step_start(uint8_t step)
{
	imu_set_full_cal_step(step);
	if(step == 1 || step == 3)
	{
		imu_band_measure_start(GYRO_BAND_SAMPLES);
	}
	else
	{
		(void)imu_calibrate_gyro();
	}
}

static bool gyro_chord_poll(const struct controller_report *r)
{
	static enum gyro_chord_state state;
	static int64_t since;
	static int64_t armed_at;
	static uint8_t step;
	static bool drift_only;
	static struct rgbw_color saved_led;
	static int64_t last_beep;
	static int64_t step_started;
	const uint32_t chord = BIT(CONTROLLER_BUTTON_LEFT_SHOULDER) |
	                       BIT(CONTROLLER_BUTTON_RIGHT_SHOULDER) | BIT(CONTROLLER_BUTTON_X) |
	                       BIT(CONTROLLER_BUTTON_Y);
	int64_t now = k_uptime_get();
	bool chord_held = (r->buttons & chord) == chord;
	/* buttons also carry the grip / stick / trackpad touch bits */
	bool hands_off = r->buttons == 0;
	bool start = false;

	if(state == GYRO_CHORD_IDLE && imu_take_full_cal_request())
	{
		start = true;
		drift_only = false; /* the meter's button always runs the full routine */
	}
	switch(state)
	{
		case GYRO_CHORD_IDLE:
			if(chord_held)
			{
				state = GYRO_CHORD_HOLDING;
				since = now;
			}
			break;
		case GYRO_CHORD_HOLDING:
			if(!chord_held)
			{
				state = GYRO_CHORD_IDLE;
			}
			else if(now - since >= GYRO_CHORD_HOLD_MS)
			{
				start = true;
				drift_only = sc26re_cfg_get(SC26RE_CFG_GYRO_CHORD_MODE) == 1;
			}
			break;
		case GYRO_CHORD_WAIT_HANDS_OFF:
			if(!hands_off)
			{
				since = now;
				imu_full_cal_note(1U, r->buttons);
			}
			else if(now - since >= GYRO_CHORD_HANDS_OFF_MS)
			{
				gyro_step_start(step);
				state = GYRO_CHORD_RUNNING;
				step_started = now;
			}
			break;
		case GYRO_CHORD_RUNNING:
		{
			bool measuring = step == 1 || step == 3;
			uint8_t st = measuring ? imu_band_measure_state() : imu_gyro_cal_state();

			if(hands_off && st == 1U && now - step_started > 9000)
			{
				/* no gyro samples are arriving: say so instead of a silent timeout */
				imu_full_cal_note(3U, 0U);
			}
			if(!hands_off || st == 3U)
			{
				imu_full_cal_note(hands_off ? 2U : 1U, r->buttons);
				/* touched or moved: repeat this step once it is still again */
				since = now;
				state = GYRO_CHORD_WAIT_HANDS_OFF;
			}
			else if(st == 4U)
			{
				imu_full_cal_note(4U, 0U);
				gyro_chord_finish(false, saved_led);
				state = GYRO_CHORD_IDLE;
			}
			else if(st == 2U)
			{
				if(measuring)
				{
					gyro_bands_apply(false);
				}
				if(drift_only)
				{
					/* drift-only chord: one calibration, dead bands untouched */
					gyro_chord_finish(true, saved_led);
					state = GYRO_CHORD_IDLE;
				}
				else if(step == 4)
				{
					imu_set_full_cal_step(5);
					gyro_bands_apply(true);
					gyro_chord_finish(true, saved_led);
					state = GYRO_CHORD_IDLE;
				}
				else
				{
					/* step done: beep, then let the beep's vibration die out
					 * (the hands-off wait) before the next measurement */
					step++;
					last_beep = now;
					(void)haptics_play_beep((uint16_t)(600 + 150 * step), 80);
					since = now;
					state = GYRO_CHORD_WAIT_HANDS_OFF;
				}
			}
			break;
		}
	}

	if(start)
	{
#if CONFIG_IBEX_RGBW_LED
		saved_led = rgbw_led_get();
		rgbw_led_set((struct rgbw_color){ 150, 0, 255, 0 });
#endif
		(void)haptics_play_system_sound(HAPTICS_SOUND_PAIRING);
		imu_force_on(true);
		armed_at = now;
		since = now;
		step = drift_only ? 2 : 1;
		imu_set_full_cal_step(step);
		state = GYRO_CHORD_WAIT_HANDS_OFF;
		LOG_INF("full gyro calibration: put the controller down");
	}
	if((state == GYRO_CHORD_WAIT_HANDS_OFF || state == GYRO_CHORD_RUNNING) &&
	   now - armed_at > GYRO_CHORD_TIMEOUT_MS)
	{
		gyro_chord_finish(false, saved_led);
		state = GYRO_CHORD_IDLE;
	}
	if(state == GYRO_CHORD_WAIT_HANDS_OFF || state == GYRO_CHORD_RUNNING)
	{
		/*
		 * Progress, so it does not look frozen: purple LED blinking (slow while
		 * waiting to be put down / still, fast while measuring), a low beep every
		 * 2 s while waiting, and a higher beep after each finished step. No beeps
		 * during a measurement: the grip actuators would shake the gyro.
		 */
		static int last_phase = -1;
		bool waiting = state == GYRO_CHORD_WAIT_HANDS_OFF;
		int period = waiting ? 500 : 200;
		int phase = (int)((now - armed_at) / period) % 2;

		if(phase != last_phase)
		{
			last_phase = phase;
#if CONFIG_IBEX_RGBW_LED
			rgbw_led_set(phase == 0 ? (struct rgbw_color){ 150, 0, 255, 0 }
			                        : (struct rgbw_color){ 20, 0, 40, 0 });
#endif
		}
		if(waiting && now - last_beep >= 2000)
		{
			last_beep = now;
			(void)haptics_play_beep(500, 60);
			since = now; /* let the beep die out before a measurement starts */
		}
		return true;
	}
	return false;
}

/*
 * Automatic gyro calibration: when the controller has lain untouched (no grip,
 * stick or trackpad touch, no buttons) for the set number of seconds while the
 * IMU is on, measure the gyro; if it was really still, use the new bias. Runs
 * once per put-down: picking it up re-arms it. Silent, no LED.
 */
static void gyro_auto_poll(const struct controller_report *r, bool full_cal_busy)
{
	static int64_t still_since;
	static bool armed = true;
	static bool running;
	int16_t secs = sc26re_cfg_get(SC26RE_CFG_GYRO_AUTO_CAL);
	int64_t now = k_uptime_get();

	if(r->buttons != 0 || full_cal_busy || !imu_streaming() || secs <= 0)
	{
		still_since = now;
		if(r->buttons != 0)
		{
			armed = true;
		}
		running = false;
		return;
	}
	if(running)
	{
		uint8_t st = imu_gyro_cal_state();

		if(st == 2U || st == 4U)
		{
			running = false;
			armed = false;
		}
		else if(st == 3U)
		{
			/* it was moving (e.g. on a lap or a soft surface): wait and try again */
			running = false;
			still_since = now;
		}
		return;
	}
	if(armed && now - still_since >= (int64_t)secs * 1000)
	{
		(void)imu_calibrate_gyro_auto();
		running = true;
	}
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
	sc26re_cfg_load();
	remap_init();
	/* the gyro mouse always starts off */
	(void)sc26re_cfg_set(SC26RE_CFG_GM_ENABLED, 0, false);
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
		{
			bool cal_busy = gyro_chord_poll(&current);

			gyro_auto_poll(&current, cal_busy);
			/* any gyro calibration (combination, meter, automatic) pauses the gyro mouse */
			gyro_mouse_suspend(cal_busy || imu_gyro_cal_state() == 1U || imu_band_measure_state() == 1U);
		}
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
		if(report_has_activity(&current) || transport_usb_attached() || puck_interface_active() ||
		   valve_settings_meter_keeps_awake())
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
		/*
		 * No host: when the Puck (or the BT host) is gone, e.g. the PC was shut
		 * down and its power cut, switch off after link_lost_off minutes like
		 * stock does, instead of searching until the inactivity timeout.
		 */
		{
			static int64_t link_ok_ms;
			int16_t lost_min = sc26re_cfg_get(SC26RE_CFG_LINK_LOST_OFF);
			bool wireless = !transport_usb_attached();
			bool linked = transport_esb_connected() || transport_ble_connected() ||
			              transport_ble_pairing_window_open() || valve_settings_meter_keeps_awake();

			if(!wireless || linked || lost_min <= 0 || link_ok_ms == 0)
			{
				link_ok_ms = now_ms;
			}
			else if(now_ms - link_ok_ms >= (int64_t)lost_min * 60 * MSEC_PER_SEC)
			{
				LOG_INF("no Puck/BT host for %d min: shutting down", lost_min);
				link_ok_ms = now_ms;
				power_off();
			}
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
		/*
		 * Steam + Y held for half a second: power off. While Steam is held,
		 * Y is hidden from the host, so the combination does nothing in games.
		 */
		{
			static int64_t steam_y_since;
			const uint32_t sy = BIT(CONTROLLER_BUTTON_STEAM) | BIT(CONTROLLER_BUTTON_Y);

			if((current.buttons & sy) == sy)
			{
				if(steam_y_since == 0)
				{
					steam_y_since = k_uptime_get();
				}
				else if(k_uptime_get() - steam_y_since >= 500)
				{
					LOG_INF("Steam + Y: shutting down");
					power_off_after_buttons_released(sy);
				}
			}
			else
			{
				steam_y_since = 0;
			}
			if(current.buttons & BIT(CONTROLLER_BUTTON_STEAM))
			{
				current.buttons &= ~BIT(CONTROLLER_BUTTON_Y);
			}
		}

		{
			/* on-controller remapper: keyboard/mouse out, mapped inputs leave the pad report */
			static int64_t remap_last_ms;
			int64_t remap_now = k_uptime_get();

			remap_process(&current, remap_last_ms ? (float)(remap_now - remap_last_ms) * 0.001f : 0.001f);
			remap_last_ms = remap_now;
		}
		bool changed = first || memcmp(&current, &previous, sizeof(current)) != 0;
		bool refresh_due = k_uptime_get() - last_input_sent >= INPUT_REFRESH_MS;
		/* OFW uses each full-state ESB report as the container boundary for lizard
		 * reports produced by the preceding input sample. */
		bool esb_state_boundary = transport_esb_connected();

		/*
		 * Report meter "report rate" (500/250 Hz): analog-only updates wait for
		 * the interval, button changes still go out immediately.
		 */
		{
			static uint32_t last_state_cyc;
			int16_t rate = sc26re_cfg_get(SC26RE_CFG_REPORT_RATE);

			if(rate > 0 && rate < 1000 && !first && current.buttons == previous.buttons &&
			   k_cyc_to_us_floor32(k_cycle_get_32() - last_state_cyc) < 1000000U / (uint32_t)rate)
			{
				changed = false;
				refresh_due = false;
				esb_state_boundary = false;
			}
			if(changed || refresh_due || esb_state_boundary)
			{
				last_state_cyc = k_cycle_get_32();
			}
		}
		current.buttons = gyro_mouse_filter_buttons(current.buttons);
		hardware_store_state(&current);
		imu_keep_on(gyro_mouse_enabled());
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
		if(!remap_enabled())
		{
			lizard_update(&current);
		}

		hardware_wait_for_change();
		watchdog_feed();
		boot_guard_loop();
	}
}
