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
 * - Steam + View toggles it, Steam + A starts the calibration (both in remap.c, with the other
 *   Steam combinations: while Steam is held their buttons never reach the game): the calibration
 *   button starts a slow horizontal turn, and pressing it again when the game camera has done
 *   exactly 360 deg saves the count. (Before v51 test 86: LB + RB + View (+ A); LB and RB went
 *   to the game before the combination was complete.)
 */
#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "controller.h"
#include "gyro_mouse.h"
#include "curve.h"
#include "haptics.h"
#include "remap.h"
#include "sc26re_cfg.h"

LOG_MODULE_REGISTER(gyro_mouse, LOG_LEVEL_INF);

/* gyro settings: the active remap profile's own values when it has them */
static int16_t gm(enum sc26re_cfg_id id)
{
	int16_t v;

	if(id != SC26RE_CFG_GM_ENABLED && remap_gyro_value((int)id, &v))
	{
		return v;
	}
	return sc26re_cfg_get(id);
}


enum cal_state
{
	CAL_IDLE,
	CAL_ARMED,   /* waiting for the calibration button to start the turn */
	CAL_TURNING, /* turning; the calibration button stops and saves */
};

static float acc_x, acc_y;
static bool toggled_on;      /* activation mode "toggle" */
static uint32_t prev_buttons;
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

static bool engaged;        /* the activation is on now (gyro_mouse_feed) */
static bool engaged_always; /* ... because it is "always" (no activation input, or mode 0) */

bool gyro_mouse_active(void)
{
	return gyro_mouse_enabled() && !suspended;
}

bool gyro_mouse_engaged(bool count_always)
{
	return gyro_mouse_active() && engaged && (count_always || !engaged_always);
}

bool gyro_mouse_enabled(void)
{
	return gm(SC26RE_CFG_GM_ENABLED) == 1 || cal != CAL_IDLE;
}

int32_t gyro_mouse_counts_per_360(void)
{
	int32_t c = ((int32_t)gm(SC26RE_CFG_GM_CAL_HI) << 15) |
	            (int32_t)gm(SC26RE_CFG_GM_CAL_LO);

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
	cal_button_mask = BIT(CLAMP(gm(SC26RE_CFG_GM_CAL_BUTTON), 0, 29));
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

	st->enabled = gm(SC26RE_CFG_GM_ENABLED) == 1;
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

static bool act_always; /* the last active() was on with no activation: "always" */

static bool active(uint32_t buttons, uint32_t pressed)
{
	int16_t b = gm(SC26RE_CFG_GM_ACT_BUTTON);
	int16_t b2 = gm(SC26RE_CFG_GM_ACT_BUTTON2);
	int16_t mode = gm(SC26RE_CFG_GM_ACT_MODE);
	/* either of the two activation buttons */
	uint32_t mask = ((b >= 0 && b <= 29) ? BIT(b) : 0U) | ((b2 >= 0 && b2 <= 29) ? BIT(b2) : 0U);
	bool held = (buttons & mask) != 0U;
	static bool held_prev;

	/* the profile's own activation inputs (Steam's mask: several buttons, soft pulls, stick
	 * deflection, any or all of them) replace the buttons */
	if(remap_gyro_act(&held))
	{
		pressed = held && !held_prev ? 1U : 0U;
		mask = 1U;
		held_prev = held;
	}
	else
	{
		held_prev = false;
	}
	act_always = mask == 0U || mode == 0;
	if(act_always)
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
			if(pressed & mask)
			{
				toggled_on = !toggled_on;
			}
			return toggled_on;
	}
}

/* quick turn in progress: counts still to send and counts per second */
static float qt_left;
static volatile float joy_x, joy_y; /* gyro joystick deflection, -1..1 */
static float tilt_x, tilt_y;        /* deflection mode: tilt since activation, deg */
static float qt_rate;

static void quick_turn_start(int16_t deg)
{
	float counts = (float)gyro_mouse_counts_per_360() * (float)deg / 360.0f;
	int16_t ms = gm(SC26RE_CFG_GM_QT_MS);

	k_mutex_lock(&lock, K_FOREVER);
	qt_left += counts;
	qt_rate = ms > 0 ? fabsf(qt_left) * 1000.0f / (float)ms : 1e9f;
	k_mutex_unlock(&lock);
	(void)haptics_play_feedback((uint8_t)gm(SC26RE_CFG_GM_HAPTIC), HAPTICS_CHANNELS_PRIMARY);
}

/* the buttons that are the gyro mouse's right now: the calibration button while calibrating (and
 * until it is let go). The remapper leaves them alone, so their bindings (keys in a game) do not
 * fire: before v51 test 85 only the pad report lost them, after the remapper had sent their keys. */
uint32_t gyro_mouse_owned(uint32_t physical)
{
	uint32_t m = cal_hide;

	ARG_UNUSED(physical);
	if(cal != CAL_IDLE)
	{
		m |= cal_button_mask;
	}
	return m;
}

uint32_t gyro_mouse_filter_buttons(uint32_t raw, uint32_t physical)
{
	/* the presses come from the physical buttons: with the remapper on, a bound button (or an
	 * unbound one with "unbound send nothing") is gone from raw, and the chord, the quick turns
	 * and the calibration button stopped working */
	/* quick turns: a press starts one; their buttons are the gyro mouse's
	 * while it is on, so the game does not see them */
	{
		static uint32_t prev_phys;
		uint32_t pressed = physical & ~prev_phys;
		uint32_t own = 0U;

		prev_phys = physical;
		if(gm(SC26RE_CFG_GM_ENABLED) == 1)
		{
			int16_t rb = gm(SC26RE_CFG_GM_RATCHET);

			if(rb >= 0 && rb <= 29)
			{
				own |= BIT(rb); /* the ratchet button belongs to the gyro while it is on */
			}
			for(int q = 0; q < 4 && !suspended; ++q)
			{
				int16_t b = gm(SC26RE_CFG_GM_QT1_BTN + 2 * q);
				int16_t deg = gm(SC26RE_CFG_GM_QT1_DEG + 2 * q);

				if(b < 0 || b > 29 || deg == 0)
				{
					continue;
				}
				own |= BIT(b);
				if(pressed & BIT(b))
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

			if(pressed & m)
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
		if(cal_hide != 0U && (physical & cal_hide) == 0U && cal == CAL_IDLE)
		{
			cal_hide = 0U;
		}
		raw &= ~cal_hide;
	}
	return raw;
}

static float grav[3];
static bool grav_ok;

void gyro_mouse_gravity(const float acc[3])
{
	float m = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);

	if(m > 1e-3f)
	{
		/* smoothed a little: hand shake is not a change of "up" */
		for(int i = 0; i < 3; ++i)
		{
			grav[i] = grav_ok ? grav[i] + (acc[i] / m - grav[i]) * 0.05f : acc[i] / m;
		}
		grav_ok = true;
	}
}

/*
 * Player space / world space (Jibb Smart's gyro spaces): yaw is the turn about
 * the gravity axis, so turning works the same however the controller is tilted.
 * Player space mixes in some of the controller's own yaw and roll, world space
 * uses only the turn about gravity; pitch stays the controller's own (player)
 * or the turn about the level left-right axis (world).
 */
static void gyro_space(float v[3], int16_t space)
{
	float g[3], m, wy;

	if(!grav_ok || space < 1 || space > 2)
	{
		return;
	}
	m = sqrtf(grav[0] * grav[0] + grav[1] * grav[1] + grav[2] * grav[2]);
	if(m < 1e-3f)
	{
		return;
	}
	for(int i = 0; i < 3; ++i)
	{
		g[i] = grav[i] / m;
	}
	{
		float w0 = v[0], w1 = v[1], w2 = v[2];

		wy = w0 * g[0] + w1 * g[1] + w2 * g[2];
		if(space == 1)
		{
			float lim = sqrtf(w1 * w1 + w2 * w2);

			v[1] = (wy < 0.0f ? -1.0f : 1.0f) * MIN(fabsf(wy) * 1.41f, lim);
		}
		else
		{
			/* the controller's left-right axis, levelled */
			float xr[3] = { 1.0f - g[0] * g[0], -g[0] * g[1], -g[0] * g[2] };
			float xm = sqrtf(xr[0] * xr[0] + xr[1] * xr[1] + xr[2] * xr[2]);

			v[1] = wy;
			if(xm > 1e-3f)
			{
				v[0] = (w0 * xr[0] + w1 * xr[1] + w2 * xr[2]) / xm;
			}
		}
	}
	v[2] = 0.0f;
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

	k_mutex_lock(&lock, K_FOREVER);
	if(cal != CAL_IDLE)
	{
		if(cal == CAL_TURNING)
		{
			float c = (float)gm(SC26RE_CFG_GM_CAL_SPEED) * dt_s;

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
		if(gm(SC26RE_CFG_GM_ENABLED) == 1)
		{
			acc_x += step;
		}
	}

	int16_t ratchet_btn = gm(SC26RE_CFG_GM_RATCHET);
	/* ratchet: holding it pauses the gyro output, e.g. to re-centre the hands */
	bool ratchet = ratchet_btn >= 0 && ratchet_btn <= 29 && (buttons & BIT(ratchet_btn)) != 0U;
	bool act_now = gm(SC26RE_CFG_GM_ENABLED) == 1 && active(buttons, pressed) && !ratchet;
	engaged = act_now;
	engaged_always = act_now && act_always;

	{
		/* feedback when the activation button starts moving the cursor */
		static bool act_prev;
		int16_t ab = gm(SC26RE_CFG_GM_ACT_BUTTON);

		bool own;

		if(act_now && !act_prev && ((ab >= 0 && ab <= 29) || remap_gyro_act(&own)) && gm(SC26RE_CFG_GM_ACT_MODE) != 0)
		{
			(void)haptics_play_feedback((uint8_t)gm(SC26RE_CFG_GM_HAPTIC), HAPTICS_CHANNELS_PRIMARY);
		}
		act_prev = act_now;
	}
	{
		/* rotational haptics (v51.4, firmware's own, Steam's are then dropped): a tick every
		 * gm_rot_deg degrees the controller turns, at most one per 25 ms, on the grips, the
		 * trackpads or both (gyro_haptic_at). While the gyro mouse is on only when it is active. */
		static float turned;
		static int64_t last_tick;
		int16_t lvl = sc26re_cfg_get(SC26RE_CFG_GM_ROT_HAPTIC);
		float r = sqrtf(dps[0] * dps[0] + dps[1] * dps[1] + dps[2] * dps[2]);

		if(lvl > 0 && (act_now || gm(SC26RE_CFG_GM_ENABLED) != 1) && r > 3.0f)
		{
			turned += r * dt_s;
			if(turned >= (float)sc26re_cfg_get(SC26RE_CFG_GM_ROT_DEG))
			{
				int64_t t = k_uptime_get();

				turned = 0.0f;
				if(t - last_tick >= 25)
				{
					last_tick = t;
					(void)haptics_play_feedback_at((uint8_t)lvl, sc26re_cfg_get(SC26RE_CFG_GYRO_HAPTIC_AT),
					                               HAPTICS_CHANNELS_PRIMARY);
				}
			}
		}
		else if(lvl <= 0 || r <= 3.0f)
		{
			turned = MIN(turned, (float)sc26re_cfg_get(SC26RE_CFG_GM_ROT_DEG));
		}
	}
	if(!act_now)
	{
		joy_x = joy_y = 0.0f;
		tilt_x = tilt_y = 0.0f; /* deflection: recentred at each activation */
		/* drop gyro movement only; whole counts of a quick turn still go out */
		if(qt_left == 0.0f)
		{
			acc_x = acc_y = 0.0f;
		}
		return;
	}

	{
		bool soft = gm(SC26RE_CFG_GM_BAND_SOFT) == 1;
		float v[3] = {
			band(dps[0], gm(SC26RE_CFG_GM_BAND_P_LO), gm(SC26RE_CFG_GM_BAND_P_HI), soft),
			band(dps[1], gm(SC26RE_CFG_GM_BAND_Y_LO), gm(SC26RE_CFG_GM_BAND_Y_HI), soft),
			band(dps[2], gm(SC26RE_CFG_GM_BAND_R_LO), gm(SC26RE_CFG_GM_BAND_R_HI), soft),
		};
		gyro_space(v, gm(SC26RE_CFG_GM_SPACE));
		/* counts per degree of controller turn, times real world sensitivity */
		float k = (float)gyro_mouse_counts_per_360() / 360.0f *
		          (float)gm(SC26RE_CFG_GM_SENS) * 0.01f * dt_s;
		/* natural directions: turning left / tilting forward moves the cursor left / down */
		static const float natural[3] = { -1.0f, -1.0f, 1.0f };
		static const enum sc26re_cfg_id axis_cfg[3] = {
			SC26RE_CFG_GM_AXIS_PITCH, SC26RE_CFG_GM_AXIS_YAW, SC26RE_CFG_GM_AXIS_ROLL
		};

		/* controller turn rate on the mouse axes, deg/s (x right, y down) */
		float wx = 0.0f, wy = 0.0f, w;
		int16_t out = gm(SC26RE_CFG_GM_OUTPUT);

		for(size_t i = 0; i < 3; ++i)
		{
			int16_t a = gm(axis_cfg[i]);
			float d = v[i] * natural[i] * (a < 0 ? -1.0f : 1.0f) *
			          (float)gm(SC26RE_CFG_GM_SCALE_P + i) * 0.01f;

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
		if(out >= 1 && out <= 4)
		{
			/* gyro as joystick: camera = turn rate, deflection = tilt since activation */
			float dz = (float)gm(SC26RE_CFG_GJ_DZ) * 0.1f;
			float mx;

			if(out >= 3)
			{
				float lim = (float)gm(SC26RE_CFG_GJ_TILT);

				/* tilting past the limit just drags the centre along */
				tilt_x = CLAMP(tilt_x + wx * dt_s, -lim, lim);
				tilt_y = CLAMP(tilt_y + wy * dt_s, -lim, lim);
				wx = tilt_x;
				wy = tilt_y;
				w = sqrtf(wx * wx + wy * wy);
				dz = MIN(dz, lim - 1.0f);
				mx = lim;
			}
			else
			{
				mx = MAX((float)gm(SC26RE_CFG_GJ_MAX), dz + 1.0f);
			}
			float anti = (float)gm(SC26RE_CFG_GJ_ANTI) * 0.001f;
			float n;

			if(w <= dz || w <= 0.0f)
			{
				joy_x = joy_y = 0.0f;
				return;
			}
			{
				const uint16_t *crv;
				float cmax;
				float x = MIN((w - dz) / (mx - dz), 1.0f);

				/* custom curve (v51.7): deflection straight from the 4 points */
				n = remap_curve_gyro(REMAP_CURVE_GJOY, &crv, &cmax)
				        ? CLAMP(curve_eval(crv, x), 0.0f, 1.0f)
				        : powf(x, (float)gm(SC26RE_CFG_GJ_CURVE) * 0.01f);
			}
			n = anti + (1.0f - anti) * n;
			joy_x = wx / w * n;
			joy_y = -wy / w * n; /* stick up = cursor up */
			acc_x = acc_y = 0.0f;
			return;
		}
		joy_x = joy_y = 0.0f;
		{
			/* response curve (around 100 deg/s) and acceleration */
			float c = (float)gm(SC26RE_CFG_GM_CURVE) * 0.1f;
			float g = 1.0f;
			int16_t acc = gm(SC26RE_CFG_GM_ACCEL);
			const uint16_t *crv;
			float cmax;

			if(remap_curve_gyro(REMAP_CURVE_GMOUSE, &crv, &cmax))
			{
				/* custom curve (v51.7): turn speed (0..cmax deg/s) -> cursor speed in the
				 * same units, so the diagonal stays 1:1 */
				if(w > 0.0f)
				{
					g = MAX(curve_eval(crv, w / cmax), 0.0f) * cmax / w;
				}
			}
			else if(c != 1.0f && w > 0.0f)
			{
				g = powf(MAX(w, 0.5f) / 100.0f, c - 1.0f);
			}
			if(acc > 0)
			{
				float lo = (float)gm(SC26RE_CFG_GM_ACCEL_LO);
				float hi = MAX((float)gm(SC26RE_CFG_GM_ACCEL_HI), lo + 1.0f);

				g *= 1.0f + (float)acc * 0.01f * CLAMP((w - lo) / (hi - lo), 0.0f, 1.0f);
			}
			{
				/* precision smoothing: slow turns (below the threshold) are averaged
				 * over ~32 ms to hide sensor noise; fast turns stay instant */
				static float sm_x, sm_y;
				float th = (float)gm(SC26RE_CFG_GM_SMOOTH) * 0.1f;
				float ox = wx * k * g, oy = wy * k * g;

				if(th > 0.0f)
				{
					float t = CLAMP((w - th * 0.5f) / (th * 0.5f), 0.0f, 1.0f); /* 0 smooth .. 1 direct */
					float a = 0.03f;

					sm_x += (ox - sm_x) * a;
					sm_y += (oy - sm_y) * a;
					ox = sm_x + (ox - sm_x) * t;
					oy = sm_y + (oy - sm_y) * t;
				}
				acc_x += ox;
				acc_y += oy;
			}
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
	int16_t out = gm(SC26RE_CFG_GM_OUTPUT);
	float jx = joy_x, jy = joy_y;
	int32_t x, y;

	if(out < 1 || out > 4 || !gyro_mouse_active() || (jx == 0.0f && jy == 0.0f))
	{
		return;
	}
	if(out == 1 || out == 3)
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
