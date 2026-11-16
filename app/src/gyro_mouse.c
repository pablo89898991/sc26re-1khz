/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Gyro as mouse (like Alpakka): the controller turns the gyro into relative
 * mouse movement itself, on the HID mouse report (0x40) the controller already
 * has for lizard mode. The rest of the controller stays a normal Steam pad.
 *
 * - Raw, bias-corrected gyro in float; no dead zone unless the user sets the
 *   mouse dead bands. Fractions of a count are carried to the next report
 *   (sub-pixel accumulation), so slow turns are not lost.
 * - Sensitivity is "real world": with RWS 1.0 a 90 deg turn of the controller
 *   turns the game camera 90 deg. That needs the game's mouse counts per 360 deg,
 *   measured by the calibration turn.
 * - LB + RB + View toggles it (not QAM: with QAM Steam opens its magnifier);
 *   LB + RB + View + A starts the calibration: the calibration button starts a slow horizontal turn, and pressing it again
 *   when the game camera has done exactly 360 deg saves the count.
 */
#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "controller.h"
#include "gyro_mouse.h"
#include "haptics.h"
#include "sc26re_cfg.h"

LOG_MODULE_REGISTER(gyro_mouse, LOG_LEVEL_INF);

#define CHORD_MASK (BIT(CONTROLLER_BUTTON_LEFT_SHOULDER) | BIT(CONTROLLER_BUTTON_RIGHT_SHOULDER) | \
                    BIT(CONTROLLER_BUTTON_VIEW))
#define CHORD_CAL_EXTRA BIT(CONTROLLER_BUTTON_A)

enum cal_state
{
	CAL_IDLE,
	CAL_ARMED,   /* waiting for the calibration button to start the turn */
	CAL_TURNING, /* turning; the calibration button stops and saves */
};

static float acc_x, acc_y;
static bool toggled_on;      /* activation mode "toggle" */
static uint32_t prev_buttons;
static bool chord_down, chord_saw_a;
static enum cal_state cal;
static float cal_counts;     /* counts sent during the calibration turn */
static uint32_t cal_button_mask;
static K_MUTEX_DEFINE(lock);

/* saving takes a flash write: done from the system work queue */
static int16_t pending_enabled = -1;
static void save_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int16_t v = pending_enabled;

	if(v >= 0)
	{
		(void)sc26re_cfg_set(SC26RE_CFG_GM_ENABLED, v, true);
	}
}
static K_WORK_DEFINE(save_work, save_work_fn);

static int32_t cal_saved_counts;
static void cal_save_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int32_t c = CLAMP(cal_saved_counts, 1, 0x3FFFFFFF);

	(void)sc26re_cfg_set(SC26RE_CFG_GM_CAL_HI, (int16_t)(c >> 15), true);
	(void)sc26re_cfg_set(SC26RE_CFG_GM_CAL_LO, (int16_t)(c & 0x7FFF), true);
}
static K_WORK_DEFINE(cal_save_work, cal_save_work_fn);

bool gyro_mouse_enabled(void)
{
	return sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) == 1 || cal != CAL_IDLE;
}

int32_t gyro_mouse_counts_per_360(void)
{
	int32_t c = ((int32_t)sc26re_cfg_get(SC26RE_CFG_GM_CAL_HI) << 15) |
	            (int32_t)sc26re_cfg_get(SC26RE_CFG_GM_CAL_LO);

	return c > 0 ? c : 16364;
}

void gyro_mouse_set_enabled(bool on)
{
	pending_enabled = on ? 1 : 0;
	(void)sc26re_cfg_set(SC26RE_CFG_GM_ENABLED, pending_enabled, false);
	k_work_submit(&save_work);
	(void)haptics_play_beep(on ? 1200 : 500, on ? 120 : 200);
}

void gyro_mouse_cal_start(void)
{
	k_mutex_lock(&lock, K_FOREVER);

	cal = CAL_ARMED;
	cal_counts = 0.0f;
	acc_x = acc_y = 0.0f;
	cal_button_mask = BIT(CLAMP(sc26re_cfg_get(SC26RE_CFG_GM_CAL_BUTTON), 0, 29));
	k_mutex_unlock(&lock);
	(void)haptics_play_beep(900, 80);
	LOG_INF("gyro mouse: calibration armed");
}

static void cal_go(void)
{
	cal = CAL_TURNING;
	cal_counts = 0.0f;
	(void)haptics_play_beep(1100, 60);
}

static void cal_finish(bool save)
{
	if(save && cal == CAL_TURNING && cal_counts >= 10.0f)
	{
		cal_saved_counts = (int32_t)lroundf(cal_counts);
		k_work_submit(&cal_save_work);
		LOG_INF("gyro mouse: %d counts per 360", (int)cal_saved_counts);
		(void)haptics_play_beep(1400, 150);
	}
	else
	{
		(void)haptics_play_beep(400, 150);
	}
	cal = CAL_IDLE;
	acc_x = acc_y = 0.0f;
}

void gyro_mouse_cal_stop(bool save)
{
	k_mutex_lock(&lock, K_FOREVER);

	if(cal == CAL_ARMED && save)
	{
		/* meter "stop" before "start": nothing to save */
		save = false;
	}
	if(cal != CAL_IDLE)
	{
		cal_finish(save);
	}
	k_mutex_unlock(&lock);
}

void gyro_mouse_cal_turn(void)
{
	k_mutex_lock(&lock, K_FOREVER);

	if(cal == CAL_ARMED)
	{
		cal_go();
	}
	k_mutex_unlock(&lock);
}

void gyro_mouse_status(struct gyro_mouse_status *st)
{
	k_mutex_lock(&lock, K_FOREVER);

	st->enabled = sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) == 1;
	st->cal_state = (uint8_t)cal;
	st->cal_counts = (int32_t)cal_counts;
	st->counts_per_360 = gyro_mouse_counts_per_360();
	k_mutex_unlock(&lock);
}

static float band(float v, int16_t lo, int16_t hi, bool soft)
{
	float l = (float)lo * 0.01f;
	float h = (float)hi * 0.01f;

	if(lo == 0 && hi == 0)
	{
		return v;
	}
	if(v >= l && v <= h)
	{
		return 0.0f;
	}
	if(soft)
	{
		return v > h ? v - h : v - l;
	}
	return v;
}

static bool active(uint32_t buttons, uint32_t pressed)
{
	int16_t b = sc26re_cfg_get(SC26RE_CFG_GM_ACT_BUTTON);
	int16_t mode = sc26re_cfg_get(SC26RE_CFG_GM_ACT_MODE);
	bool held = b >= 0 && b <= 29 && (buttons & BIT(b)) != 0U;

	if(b < 0 || b > 29 || mode == 0)
	{
		return true;
	}
	switch(mode)
	{
		case 1:
			return held;
		case 2:
			return !held;
		default:
			if(pressed & BIT(b))
			{
				toggled_on = !toggled_on;
			}
			return toggled_on;
	}
}

void gyro_mouse_feed(const float dps[3], uint32_t buttons, float dt_s)
{
	uint32_t pressed = buttons & ~prev_buttons;
	bool chord = (buttons & CHORD_MASK) == CHORD_MASK;

	prev_buttons = buttons;
	dt_s = CLAMP(dt_s, 0.0f, 0.005f);

	/* LB + RB + View: toggle on release; with A as well: calibration */
	if(chord)
	{
		if(!chord_down)
		{
			chord_down = true;
			chord_saw_a = false;
		}
		if(buttons & CHORD_CAL_EXTRA)
		{
			chord_saw_a = true;
		}
	}
	else if(chord_down && (buttons & CHORD_MASK) == 0U)
	{
		chord_down = false;
		if(chord_saw_a)
		{
			gyro_mouse_cal_start();
		}
		else
		{
			gyro_mouse_set_enabled(sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) != 1);
		}
		pressed = 0U; /* the chord's own presses don't start anything */
	}

	k_mutex_lock(&lock, K_FOREVER);
	if(cal != CAL_IDLE)
	{
		if(!chord_down && (pressed & cal_button_mask))
		{
			if(cal == CAL_ARMED)
			{
				cal_go();
			}
			else
			{
				cal_finish(true);
			}
		}
		if(cal == CAL_TURNING)
		{
			float c = (float)sc26re_cfg_get(SC26RE_CFG_GM_CAL_SPEED) * dt_s;

			acc_x += c;
			cal_counts += c;
		}
		k_mutex_unlock(&lock);
		return;
	}
	k_mutex_unlock(&lock);

	if(sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) != 1 || chord_down || !active(buttons, pressed))
	{
		acc_x = acc_y = 0.0f;
		return;
	}

	{
		bool soft = sc26re_cfg_get(SC26RE_CFG_GM_BAND_SOFT) == 1;
		float v[3] = {
			band(dps[0], sc26re_cfg_get(SC26RE_CFG_GM_BAND_P_LO), sc26re_cfg_get(SC26RE_CFG_GM_BAND_P_HI), soft),
			band(dps[1], sc26re_cfg_get(SC26RE_CFG_GM_BAND_Y_LO), sc26re_cfg_get(SC26RE_CFG_GM_BAND_Y_HI), soft),
			band(dps[2], sc26re_cfg_get(SC26RE_CFG_GM_BAND_R_LO), sc26re_cfg_get(SC26RE_CFG_GM_BAND_R_HI), soft),
		};
		/* counts per degree of controller turn, times real world sensitivity */
		float k = (float)gyro_mouse_counts_per_360() / 360.0f *
		          (float)sc26re_cfg_get(SC26RE_CFG_GM_SENS) * 0.01f * dt_s;
		/* natural directions: turning left / tilting forward moves the cursor left / down */
		static const float natural[3] = { -1.0f, -1.0f, 1.0f };
		static const enum sc26re_cfg_id axis_cfg[3] = {
			SC26RE_CFG_GM_AXIS_PITCH, SC26RE_CFG_GM_AXIS_YAW, SC26RE_CFG_GM_AXIS_ROLL
		};

		for(size_t i = 0; i < 3; ++i)
		{
			int16_t a = sc26re_cfg_get(axis_cfg[i]);
			float d = v[i] * natural[i] * k * (a < 0 ? -1.0f : 1.0f);

			if(abs(a) == 1)
			{
				acc_x += d;
			}
			else if(abs(a) == 2)
			{
				acc_y += d;
			}
		}
	}
}

void gyro_mouse_give_back(int8_t dx, int8_t dy)
{
	acc_x += (float)dx;
	acc_y += (float)dy;
}

bool gyro_mouse_has_counts(void)
{
	return fabsf(acc_x) >= 1.0f || fabsf(acc_y) >= 1.0f;
}

bool gyro_mouse_take(int8_t *dx, int8_t *dy)
{
	float x = CLAMP(truncf(acc_x), -127.0f, 127.0f);
	float y = CLAMP(truncf(acc_y), -127.0f, 127.0f);

	acc_x -= x;
	acc_y -= y;
	*dx = (int8_t)x;
	*dy = (int8_t)y;
	return *dx != 0 || *dy != 0;
}
