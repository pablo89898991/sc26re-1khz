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
static uint32_t cal_hide;
static K_MUTEX_DEFINE(lock);
/* counts handed to the transport (report meter link test) */
static atomic_t sent_abs_x, sent_abs_y, sent_reports;

static int16_t pending_enabled = -1;

static int32_t cal_saved_counts;
static void cal_save_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int32_t c = CLAMP(cal_saved_counts, 1, 0x3FFFFFFF);

	(void)sc26re_cfg_set(SC26RE_CFG_GM_CAL_HI, (int16_t)(c >> 15), true);
	(void)sc26re_cfg_set(SC26RE_CFG_GM_CAL_LO, (int16_t)(c & 0x7FFF), true);
}
static K_WORK_DEFINE(cal_save_work, cal_save_work_fn);

/* gyro calibration running: the gyro mouse pauses (no movement, no quick turns) */
static volatile bool suspended;

void gyro_mouse_suspend(bool on)
{
	suspended = on;
}

bool gyro_mouse_active(void)
{
	return gyro_mouse_enabled() && !suspended;
}

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
	/* not saved: the gyro mouse is off after every power-on */
	pending_enabled = on ? 1 : 0;
	(void)sc26re_cfg_set(SC26RE_CFG_GM_ENABLED, pending_enabled, false);
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
	st->sent_abs_x = (uint32_t)atomic_get(&sent_abs_x);
	st->sent_abs_y = (uint32_t)atomic_get(&sent_abs_y);
	st->sent_reports = (uint32_t)atomic_get(&sent_reports);
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

/* quick turn in progress: counts still to send and counts per second */
static float qt_left;
static volatile float joy_x, joy_y; /* gyro joystick deflection, -1..1 */
static float qt_rate;

static void quick_turn_start(int16_t deg)
{
	float counts = (float)gyro_mouse_counts_per_360() * (float)deg / 360.0f;
	int16_t ms = sc26re_cfg_get(SC26RE_CFG_GM_QT_MS);

	k_mutex_lock(&lock, K_FOREVER);
	qt_left += counts;
	qt_rate = ms > 0 ? fabsf(qt_left) * 1000.0f / (float)ms : 1e9f;
	k_mutex_unlock(&lock);
	(void)haptics_play_feedback((uint8_t)sc26re_cfg_get(SC26RE_CFG_GM_HAPTIC), HAPTICS_CHANNELS_PRIMARY);
}

uint32_t gyro_mouse_filter_buttons(uint32_t raw)
{
	bool chord = (raw & CHORD_MASK) == CHORD_MASK;

	/* LB + RB + View: toggle on release; with A as well: calibration */
	if(chord)
	{
		if(!chord_down)
		{
			chord_down = true;
			chord_saw_a = false;
		}
		if(raw & CHORD_CAL_EXTRA)
		{
			chord_saw_a = true;
		}
	}
	else if(chord_down && (raw & (CHORD_MASK | CHORD_CAL_EXTRA)) == 0U)
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
	}
	/* quick turns: a press starts one; their buttons are the gyro mouse's
	 * while it is on, so the game does not see them */
	{
		static uint32_t prev_raw;
		uint32_t pressed = raw & ~prev_raw;
		uint32_t own = 0U;

		prev_raw = raw;
		if(sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) == 1)
		{
			for(int q = 0; q < 4 && !suspended; ++q)
			{
				int16_t b = sc26re_cfg_get(SC26RE_CFG_GM_QT1_BTN + 2 * q);
				int16_t deg = sc26re_cfg_get(SC26RE_CFG_GM_QT1_DEG + 2 * q);

				if(b < 0 || b > 29 || deg == 0)
				{
					continue;
				}
				own |= BIT(b);
				if(!chord_down && (pressed & BIT(b)))
				{
					quick_turn_start(deg);
				}
			}
		}
		raw &= ~own;

		/* 360 calibration: its button starts/stops the turn and is hidden from
		 * Steam and the game until the calibration ends */
		if(cal != CAL_IDLE)
		{
			uint32_t m = cal_button_mask;

			if(!chord_down && (pressed & m))
			{
				k_mutex_lock(&lock, K_FOREVER);
				if(cal == CAL_ARMED)
				{
					cal_go();
				}
				else if(cal == CAL_TURNING)
				{
					cal_finish(true);
				}
				k_mutex_unlock(&lock);
			}
			cal_hide = m;
		}
		/* hidden until released, also right after the calibration ended */
		if(cal_hide != 0U && (prev_raw & cal_hide) == 0U && cal == CAL_IDLE)
		{
			cal_hide = 0U;
		}
		raw &= ~cal_hide;
	}
	/* from the moment the chord is complete until all its buttons are up,
	 * the game does not see them */
	return chord_down ? raw & ~(CHORD_MASK | CHORD_CAL_EXTRA) : raw;
}

void gyro_mouse_feed(const float dps[3], uint32_t buttons, float dt_s)
{
	uint32_t pressed = buttons & ~prev_buttons;

	prev_buttons = buttons;
	dt_s = CLAMP(dt_s, 0.0f, 0.005f);
	if(suspended)
	{
		acc_x = acc_y = 0.0f;
		qt_left = 0.0f;
		return;
	}

	if(chord_down)
	{
		pressed = 0U; /* the chord's own presses don't start anything */
	}

	k_mutex_lock(&lock, K_FOREVER);
	if(cal != CAL_IDLE)
	{
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

	/* quick turn: spread over gm_qt_ms, independent of the activation button */
	if(qt_left != 0.0f)
	{
		k_mutex_lock(&lock, K_FOREVER);
		float step = MIN(fabsf(qt_left), qt_rate * MAX(dt_s, 0.0005f));

		step = qt_left > 0.0f ? step : -step;
		qt_left -= step;
		if(fabsf(qt_left) < 0.001f)
		{
			qt_left = 0.0f;
		}
		k_mutex_unlock(&lock);
		if(sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) == 1)
		{
			acc_x += step;
		}
	}

	bool act_now = sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) == 1 && !chord_down && active(buttons, pressed);

	{
		/* feedback when the activation button starts moving the cursor */
		static bool act_prev;
		int16_t ab = sc26re_cfg_get(SC26RE_CFG_GM_ACT_BUTTON);

		if(act_now && !act_prev && ab >= 0 && ab <= 29 && sc26re_cfg_get(SC26RE_CFG_GM_ACT_MODE) != 0)
		{
			(void)haptics_play_feedback((uint8_t)sc26re_cfg_get(SC26RE_CFG_GM_HAPTIC), HAPTICS_CHANNELS_PRIMARY);
		}
		act_prev = act_now;
	}
	if(!act_now)
	{
		joy_x = joy_y = 0.0f;
		/* drop gyro movement only; whole counts of a quick turn still go out */
		if(qt_left == 0.0f)
		{
			acc_x = acc_y = 0.0f;
		}
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

		/* controller turn rate on the mouse axes, deg/s (x right, y down) */
		float wx = 0.0f, wy = 0.0f, w;
		int16_t out = sc26re_cfg_get(SC26RE_CFG_GM_OUTPUT);

		for(size_t i = 0; i < 3; ++i)
		{
			int16_t a = sc26re_cfg_get(axis_cfg[i]);
			float d = v[i] * natural[i] * (a < 0 ? -1.0f : 1.0f) *
			          (float)sc26re_cfg_get(SC26RE_CFG_GM_SCALE_P + i) * 0.01f;

			if(abs(a) == 1)
			{
				wx += d;
			}
			else if(abs(a) == 2)
			{
				wy += d;
			}
		}
		w = sqrtf(wx * wx + wy * wy);
		if(out == 1 || out == 2)
		{
			/* gyro as joystick: turn rate -> stick deflection */
			float dz = (float)sc26re_cfg_get(SC26RE_CFG_GJ_DZ);
			float mx = MAX((float)sc26re_cfg_get(SC26RE_CFG_GJ_MAX), dz + 1.0f);
			float anti = (float)sc26re_cfg_get(SC26RE_CFG_GJ_ANTI) * 0.01f;
			float n;

			if(w <= dz || w <= 0.0f)
			{
				joy_x = joy_y = 0.0f;
				return;
			}
			n = powf(MIN((w - dz) / (mx - dz), 1.0f), (float)sc26re_cfg_get(SC26RE_CFG_GJ_CURVE) * 0.1f);
			n = anti + (1.0f - anti) * n;
			joy_x = wx / w * n;
			joy_y = -wy / w * n; /* stick up = cursor up */
			acc_x = acc_y = 0.0f;
			return;
		}
		joy_x = joy_y = 0.0f;
		{
			/* response curve (around 100 deg/s) and acceleration */
			float c = (float)sc26re_cfg_get(SC26RE_CFG_GM_CURVE) * 0.1f;
			float g = 1.0f;
			int16_t acc = sc26re_cfg_get(SC26RE_CFG_GM_ACCEL);

			if(c != 1.0f && w > 0.0f)
			{
				g = powf(MAX(w, 0.5f) / 100.0f, c - 1.0f);
			}
			if(acc > 0)
			{
				float lo = (float)sc26re_cfg_get(SC26RE_CFG_GM_ACCEL_LO);
				float hi = MAX((float)sc26re_cfg_get(SC26RE_CFG_GM_ACCEL_HI), lo + 1.0f);

				g *= 1.0f + (float)acc * 0.01f * CLAMP((w - lo) / (hi - lo), 0.0f, 1.0f);
			}
			acc_x += wx * k * g;
			acc_y += wy * k * g;
		}
	}
}


void gyro_mouse_note_sent(int8_t dx, int8_t dy)
{
	atomic_add(&sent_abs_x, abs(dx));
	atomic_add(&sent_abs_y, abs(dy));
	atomic_inc(&sent_reports);
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

void gyro_mouse_joystick(struct controller_report *r)
{
	int16_t out = sc26re_cfg_get(SC26RE_CFG_GM_OUTPUT);
	float jx = joy_x, jy = joy_y;
	int32_t x, y;

	if((out != 1 && out != 2) || !gyro_mouse_active() || (jx == 0.0f && jy == 0.0f))
	{
		return;
	}
	if(out == 1)
	{
		x = r->stick_right_x + (int32_t)lroundf(jx * 32767.0f);
		y = r->stick_right_y + (int32_t)lroundf(jy * 32767.0f);
		r->stick_right_x = (int16_t)CLAMP(x, -32767, 32767);
		r->stick_right_y = (int16_t)CLAMP(y, -32767, 32767);
	}
	else
	{
		x = r->stick_left_x + (int32_t)lroundf(jx * 32767.0f);
		y = r->stick_left_y + (int32_t)lroundf(jy * 32767.0f);
		r->stick_left_x = (int16_t)CLAMP(x, -32767, 32767);
		r->stick_left_y = (int16_t)CLAMP(y, -32767, 32767);
	}
}
