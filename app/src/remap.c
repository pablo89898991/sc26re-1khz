/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * On-controller remapper: profiles stored in the controller turn its inputs
 * (buttons, back buttons, trigger clicks, stick / trackpad / grip touch,
 * sticks, trackpads) into keyboard, mouse and Steam pad outputs. The report
 * meter's remapper page edits the profiles.
 *
 * While it is on (LB + RB + Menu toggles it, or the editor), every input that
 * has a binding belongs to the remapper: it is removed from the Steam pad
 * report and the game only sees what it is mapped to.
 *
 * Profile blob (struct remap_profile) is transferred in 48-byte chunks with the
 * pseudo setting paths sc26re/rmp/<profile>/<offset hex>; COMMIT sc26re/rmp/<p>
 * saves it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "controller.h"
#include "gyro_mouse.h"
#include "haptics.h"
#include "remap.h"
#include "imu.h"
#include "rgbw_led.h"
#include "sc26re_cfg.h"

LOG_MODULE_REGISTER(remap, LOG_LEVEL_INF);

#define HOLD_MS 250
#define DOUBLE_MS 250
#define TAP_MS 40
#define CHORD_MASK (BIT(CONTROLLER_BUTTON_LEFT_SHOULDER) | BIT(CONTROLLER_BUTTON_RIGHT_SHOULDER) | \
                    BIT(CONTROLLER_BUTTON_MENU))
#define PATH_PREFIX "sc26re/rmp/"
#define CHUNK 48

BUILD_ASSERT(sizeof(struct remap_profile) == 1928, "remap profile layout (web/remap.html)");

static struct remap_profile profiles[REMAP_PROFILES];

/* sources of each mode shift group, in the order of remap_profile.sbind */
const uint8_t remap_shift_src[REMAP_SHIFT_SRC_COUNT] = {
	0, 1, 2, 3,                                   /* ABXY */
	13, 10, 12, 11,                               /* D-pad */
	30, 31, 32, 33, 60,                           /* left stick */
	34, 35, 36, 37, 61,                           /* right stick */
	38, 39, 40, 41, 46, 47, 48, 49, 54, 55, 62, 64, /* left pad */
	42, 43, 44, 45, 50, 51, 52, 53, 56, 57, 63, 65, /* right pad */
	58, 27,                                       /* left trigger */
	59, 23,                                       /* right trigger */
};
static const uint8_t shift_first[REMAP_SHIFT_GROUPS + 1] = { 0, 4, 8, 13, 18, 30, 42, 44, 46 };
static int8_t src_group[REMAP_SRC_COUNT]; /* -1: not in a shift group */
static int8_t src_sidx[REMAP_SRC_COUNT];
static bool shifted[REMAP_SHIFT_GROUPS];

/* settings a profile can carry for the gyro (same order on the editor page) */
const uint8_t remap_gyro_keys[REMAP_GYRO_KEYS] = {
	SC26RE_CFG_GM_OUTPUT, SC26RE_CFG_GM_ACT_BUTTON, SC26RE_CFG_GM_ACT_MODE, SC26RE_CFG_GM_AXIS_PITCH,
	SC26RE_CFG_GM_AXIS_YAW, SC26RE_CFG_GM_AXIS_ROLL, SC26RE_CFG_GM_SENS, SC26RE_CFG_GM_SCALE_P,
	SC26RE_CFG_GM_SCALE_Y, SC26RE_CFG_GM_SCALE_R, SC26RE_CFG_GM_CURVE, SC26RE_CFG_GM_ACCEL,
	SC26RE_CFG_GM_ACCEL_LO, SC26RE_CFG_GM_ACCEL_HI, SC26RE_CFG_GJ_DZ, SC26RE_CFG_GJ_ANTI,
	SC26RE_CFG_GJ_MAX, SC26RE_CFG_GJ_CURVE, SC26RE_CFG_GJ_TILT, SC26RE_CFG_GM_HAPTIC,
	SC26RE_CFG_GM_QT1_BTN, SC26RE_CFG_GM_QT1_DEG, SC26RE_CFG_GM_QT2_BTN, SC26RE_CFG_GM_QT2_DEG,
	SC26RE_CFG_GM_QT3_BTN, SC26RE_CFG_GM_QT3_DEG, SC26RE_CFG_GM_QT4_BTN, SC26RE_CFG_GM_QT4_DEG,
	SC26RE_CFG_GM_QT_MS, SC26RE_CFG_GM_RATCHET,
};
BUILD_ASSERT(SC26RE_CFG_COUNT < 256, "gyro keys are bytes");

/* output monitor */
static uint32_t mon_pad;
static int32_t mon_dx, mon_dy, mon_wheel;
static bool profile_dirty[REMAP_PROFILES];

struct src_state
{
	bool down;
	bool toggled;
	bool waiting_double; /* released after a short press, a second press may follow */
	int8_t held_group;   /* group active while held: -1 none, 0..2 */
	int64_t t_down;
	int64_t t_up;
	int64_t pulse_until; /* group 0 pulsed (tap) until */
	int8_t pulse_group;
	int64_t release_until; /* release press: group 2 tapped until */
};
static struct src_state st[REMAP_SRC_COUNT];

static uint8_t last_kbd[8];
static bool kbd_valid;
static uint8_t mouse_buttons;
static uint32_t xbox_buttons;
static uint8_t move_dirs; /* mouse move actions held: up, down, left, right */
static float mouse_acc[4]; /* x, y, wheel, hwheel */
static int16_t pad_prev[2][2];
static bool pad_prev_valid[2];
static bool chord_down;

bool remap_enabled(void)
{
	return sc26re_cfg_get(SC26RE_CFG_REMAP_ON) == 1;
}

static struct remap_profile *active(void)
{
	return &profiles[CLAMP(sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE), 0, REMAP_PROFILES - 1)];
}

uint32_t remap_xbox_buttons(void)
{
	return remap_enabled() ? xbox_buttons : 0U;
}

uint8_t remap_mouse_buttons(void)
{
	return remap_enabled() ? mouse_buttons : 0U;
}

static bool bound(const struct remap_binding *b)
{
	for(size_t g = 0; g < 3; ++g)
	{
		for(size_t i = 0; i < 4; ++i)
		{
			if(b->act[g][i] != 0U)
			{
				return true;
			}
		}
	}
	return false;
}

/* ---- derived inputs, computed once per report ---- */

#define DP_MASK (BIT(CONTROLLER_BUTTON_DPAD_UP) | BIT(CONTROLLER_BUTTON_DPAD_DOWN) | \
                 BIT(CONTROLLER_BUTTON_DPAD_LEFT) | BIT(CONTROLLER_BUTTON_DPAD_RIGHT))
#define LEFT_BITS (BIT(CONTROLLER_BUTTON_VIEW) | DP_MASK | BIT(CONTROLLER_BUTTON_LEFT_STICK) | \
                   BIT(CONTROLLER_BUTTON_LEFT_PADDLE1) | BIT(CONTROLLER_BUTTON_LEFT_PADDLE2) | \
                   BIT(CONTROLLER_BUTTON_LEFT_SHOULDER) | BIT(CONTROLLER_BUTTON_LEFT_STICK_TOUCH) | \
                   BIT(CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH) | BIT(CONTROLLER_BUTTON_LEFT_TOUCHPAD_CLICK) | \
                   BIT(CONTROLLER_BUTTON_LEFT_TRIGGER_CLICK) | BIT(CONTROLLER_BUTTON_LEFT_GRIP_TOUCH))

static uint32_t btn;       /* buttons as the sources see them (D-pad 4-way filtered) */
static uint8_t dirs[4];    /* per stick / pad: bit 0 up, 1 down, 2 left, 3 right */
static uint8_t gates[2];   /* per pad: bit 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
static uint8_t wheel_fire[2]; /* per pad this report: bit 0 clockwise, 1 counter-clockwise */
static bool soft_on[2];
static float wheel_prev[2], wheel_acc[2];
static bool wheel_valid[2];
static float pad_hap_dist[2];
static float scroll_notch[2];
static uint32_t dp4_keep;
static bool ring_on[4];
static bool center_on[2];
static bool pad_act[2]; /* trackpad active (touched / clicked / pressed) this report */
static bool swipe_done[2];
static uint8_t swipe_fire[2]; /* per pad this report: bit 0 up, 1 down, 2 left, 3 right */
static bool swipe_mode[2];
static int16_t swipe_anchor[2][2];
static bool swipe_valid[2];
static float pad_joy[2][2]; /* trackpad joystick added to the left / right stick, -1..1 */
/* flick stick per stick / pad */
static bool flick_on[4];
static float flick_last[4];
static float flick_left, flick_rate;

/* 0 left, 1 right, 2 both */
static int src_side(int s)
{
	if(s < 30)
	{
		return s == CONTROLLER_BUTTON_QAM ? 2 : ((LEFT_BITS & BIT(s)) ? 0 : 1);
	}
	if(s < REMAP_SRC_LPAD_GATE)
	{
		return ((s - REMAP_SRC_LS_UP) / 4) % 2; /* LS, RS, LPAD, RPAD */
	}
	if(s < REMAP_SRC_LPAD_CW)
	{
		return s < REMAP_SRC_RPAD_GATE ? 0 : 1;
	}
	if(s < REMAP_SRC_LT_SOFT)
	{
		return s < REMAP_SRC_RPAD_CW ? 0 : 1;
	}
	if(s < REMAP_SRC_LS_RING)
	{
		return s - REMAP_SRC_LT_SOFT;
	}
	if(s < REMAP_SRC_LPAD_CENTER)
	{
		return (s - REMAP_SRC_LS_RING) % 2;
	}
	return s - REMAP_SRC_LPAD_CENTER;
}

static void feedback(uint8_t level, int side)
{
	static int64_t last[3];
	static const uint8_t ch[3] = { HAPTICS_CHANNEL_LEFT_0, HAPTICS_CHANNEL_RIGHT_0, HAPTICS_CHANNELS_PRIMARY };
	int64_t now = k_uptime_get();

	if(level == 0U || side < 0 || side > 2 || now - last[side] < 20)
	{
		return;
	}
	last[side] = now;
	(void)haptics_play_feedback(level, ch[side]);
}

static uint8_t dir_mask(uint8_t mode, float fx, float fy, float dz)
{
	static const uint8_t m8[8] = { 8, 8 | 1, 1, 1 | 4, 4, 4 | 2, 2, 2 | 8 };
	float a;

	if(fx * fx + fy * fy < dz * dz)
	{
		return 0U;
	}
	if(mode == REMAP_ANALOG_DPAD4)
	{
		if(fabsf(fy) >= fabsf(fx))
		{
			return fy > 0.0f ? 1U : 2U;
		}
		return fx < 0.0f ? 4U : 8U;
	}
	a = atan2f(fy, fx) * 57.29578f;
	if(a < 0.0f)
	{
		a += 360.0f;
	}
	return m8[((int)((a + 22.5f) / 45.0f)) & 7];
}

static uint8_t gate_mask(float fx, float fy, float dz)
{
	if(fx * fx + fy * fy < dz * dz)
	{
		return 0U;
	}
	return fy >= 0.0f ? (fx < 0.0f ? 1U : 2U) : (fx < 0.0f ? 4U : 8U);
}

static void wheel(int j, const struct remap_analog *an, bool active, float fx, float fy)
{
	float a, step;

	wheel_fire[j] = 0U;
	if(!active || fx * fx + fy * fy < powf(MAX(an->dz_in, 500) * 0.0001f, 2.0f))
	{
		wheel_valid[j] = false;
		return;
	}
	a = atan2f(fy, fx) * 57.29578f;
	if(wheel_valid[j])
	{
		float d = a - wheel_prev[j];

		if(d > 180.0f)
		{
			d -= 360.0f;
		}
		else if(d < -180.0f)
		{
			d += 360.0f;
		}
		wheel_acc[j] += d;
		step = (float)CLAMP(an->speed, 5, 180);
		if(wheel_acc[j] >= step)
		{
			wheel_acc[j] -= step;
			wheel_fire[j] = 2U; /* angle grows: counter-clockwise */
		}
		else if(wheel_acc[j] <= -step)
		{
			wheel_acc[j] += step;
			wheel_fire[j] = 1U;
		}
	}
	else
	{
		wheel_acc[j] = 0.0f;
	}
	wheel_prev[j] = a;
	wheel_valid[j] = true;
}

static bool src_pressed(int s)
{
	if(s < 30)
	{
		return (btn & BIT(s)) != 0U;
	}
	if(s < REMAP_SRC_LPAD_GATE)
	{
		return (dirs[(s - REMAP_SRC_LS_UP) / 4] & BIT((s - REMAP_SRC_LS_UP) % 4)) != 0U;
	}
	if(s < REMAP_SRC_LPAD_CW)
	{
		return (gates[(s - REMAP_SRC_LPAD_GATE) / 4] & BIT((s - REMAP_SRC_LPAD_GATE) % 4)) != 0U;
	}
	if(s >= REMAP_SRC_LPAD_CENTER)
	{
		return center_on[s - REMAP_SRC_LPAD_CENTER];
	}
	if(s >= REMAP_SRC_LS_RING)
	{
		return ring_on[s - REMAP_SRC_LS_RING];
	}
	if(s >= REMAP_SRC_LT_SOFT)
	{
		return soft_on[s - REMAP_SRC_LT_SOFT];
	}
	return false; /* scroll wheel steps are pulses */
}

static void out_action(uint8_t a, uint8_t keys[6], size_t *nk, uint8_t *mods, uint32_t *pad)
{
	if(a == 0U)
	{
		return;
	}
	if(a >= 0xE0U && a <= 0xE7U)
	{
		*mods |= (uint8_t)BIT(a - 0xE0U);
	}
	else if(a <= 0x65U)
	{
		if(*nk < 6U)
		{
			keys[(*nk)++] = a;
		}
	}
	else if(a == REMAP_ACT_MOUSE_L)
	{
		mouse_buttons |= BIT(0);
	}
	else if(a == REMAP_ACT_MOUSE_R)
	{
		mouse_buttons |= BIT(1);
	}
	else if(a >= REMAP_ACT_MOUSE_M && a <= REMAP_ACT_MOUSE_5)
	{
		mouse_buttons |= BIT(2 + a - REMAP_ACT_MOUSE_M);
	}
	else if(a >= REMAP_ACT_MOVE_UP && a <= REMAP_ACT_MOVE_RIGHT)
	{
		move_dirs |= BIT(a - REMAP_ACT_MOVE_UP);
	}
	else if(a >= REMAP_ACT_PAD && a < REMAP_ACT_PAD + 30U)
	{
		*pad |= BIT(a - REMAP_ACT_PAD);
	}
	else if(a >= REMAP_ACT_XBOX && a < REMAP_ACT_XBOX + 30U)
	{
		xbox_buttons |= BIT(a - REMAP_ACT_XBOX);
	}
}

/* edge actions: run once when the group becomes active */
static void edge_action(uint8_t a)
{
	switch(a)
	{
		case REMAP_ACT_WHEEL_UP: mouse_acc[2] += 1.0f; break;
		case REMAP_ACT_WHEEL_DOWN: mouse_acc[2] -= 1.0f; break;
		case REMAP_ACT_WHEEL_LEFT: mouse_acc[3] -= 1.0f; break;
		case REMAP_ACT_WHEEL_RIGHT: mouse_acc[3] += 1.0f; break;
		default:
			if(a >= REMAP_ACT_PROFILE && a < REMAP_ACT_PROFILE + REMAP_PROFILES)
			{
				(void)sc26re_cfg_set(SC26RE_CFG_REMAP_PROFILE, (int16_t)(a - REMAP_ACT_PROFILE), true);
				(void)haptics_play_beep((uint16_t)(700 + 150 * (a - REMAP_ACT_PROFILE)), 80);
			}
			break;
	}
}

static void fire(const struct remap_binding *b, int g, int s)
{
	for(size_t i = 0; i < 4; ++i)
	{
		edge_action(b->act[g][i]);
	}
	feedback(b->haptic, src_side(s));
}

static void activate(const struct remap_binding *b, int g, int8_t *slot, int s)
{
	if(*slot != g)
	{
		*slot = (int8_t)g;
		fire(b, g, s);
	}
}

static void pulse(const struct remap_binding *b, struct src_state *ss, int s, int64_t now)
{
	ss->pulse_group = 0;
	ss->pulse_until = now + TAP_MS;
	fire(b, 0, s);
}

static void run_source(const struct remap_binding *b, struct src_state *s, bool down, int64_t now, int src,
                       bool release_ok)
{
	bool hold = (b->mode & REMAP_MODE_HOLD) != 0U;
	bool rel = (b->mode & REMAP_MODE_RELEASE) != 0U;
	bool dbl = (b->mode & REMAP_MODE_DOUBLE) != 0U && !rel; /* release uses the double slot */

	/* release press: group 2 tapped when let go (trackpads: only when the finger
	 * leaves / the click ends, not when it slides into another zone) */
	if(rel && !down && s->down && release_ok)
	{
		s->release_until = now + TAP_MS;
		fire(b, 2, src);
	}
	/* start press: group 0 tapped the moment it is pressed (hold still works) */
	if(b->mode & REMAP_MODE_START)
	{
		if(down && !s->down)
		{
			s->t_down = now;
			pulse(b, s, src, now);
		}
		if(!down)
		{
			s->held_group = -1;
		}
		else if(hold && s->held_group < 0 && now - s->t_down >= HOLD_MS)
		{
			activate(b, 1, &s->held_group, src);
		}
		s->down = down;
		return;
	}

	if(b->mode & REMAP_MODE_TOGGLE)
	{
		if(down && !s->down)
		{
			s->toggled = !s->toggled;
			if(!s->toggled)
			{
				feedback(b->haptic, src_side(src));
			}
		}
		s->down = down;
		if(s->toggled)
		{
			activate(b, 0, &s->held_group, src);
		}
		else
		{
			s->held_group = -1;
		}
		return;
	}
	if(!hold && !dbl)
	{
		if(down)
		{
			activate(b, 0, &s->held_group, src);
		}
		else
		{
			s->held_group = -1;
		}
		s->down = down;
		return;
	}
	if(down && !s->down)
	{
		s->t_down = now;
		if(dbl && s->waiting_double && now - s->t_up <= DOUBLE_MS)
		{
			s->waiting_double = false;
			activate(b, 2, &s->held_group, src); /* second press: group 2 while held */
		}
	}
	else if(!down && s->down)
	{
		bool was_hold = s->held_group == 1;
		bool was_double = s->held_group == 2;

		s->held_group = -1;
		s->t_up = now;
		if(!was_hold && !was_double)
		{
			if(dbl)
			{
				s->waiting_double = true; /* decide after DOUBLE_MS */
			}
			else
			{
				pulse(b, s, src, now);
			}
		}
	}
	else if(down && hold && s->held_group < 0 && now - s->t_down >= HOLD_MS)
	{
		activate(b, 1, &s->held_group, src);
	}
	if(s->waiting_double && !down && now - s->t_up > DOUBLE_MS)
	{
		s->waiting_double = false;
		pulse(b, s, src, now);
	}
	s->down = down;
}

static void stick_analog(int i, const struct remap_analog *an, int16_t x, int16_t y, float dt)
{
	float fx = (float)x / 32767.0f;
	float fy = (float)y / 32767.0f;
	float m = sqrtf(fx * fx + fy * fy);
	float dz = (float)MAX(an->dz_in, 300) * 0.0001f;
	float mm;

	if(m < dz || m <= 0.0f)
	{
		return;
	}
	mm = MIN((m - dz) / (1.0f - dz), 1.0f);
	fx /= m;
	fy /= m;
	if(an->mode == REMAP_ANALOG_MOUSE)
	{
		/* speed: counts per second at full deflection, squared response */
		float k = (float)an->speed * dt * powf(mm, an->curve ? (float)an->curve * 0.01f : 2.0f);

		mouse_acc[0] += fx * k;
		mouse_acc[1] -= fy * k;
	}
	else if(an->mode == REMAP_ANALOG_SCROLL)
	{
		/* speed: notches per second x100 at full deflection */
		float k = (float)an->speed * 0.01f * dt * mm;
		float before = scroll_notch[i];

		mouse_acc[2] += fy * k;
		mouse_acc[3] += fx * k;
		scroll_notch[i] += k;
		if(floorf(scroll_notch[i]) != floorf(before))
		{
			feedback(an->haptic, i);
		}
	}
}

/* sticks to Steam: inner / outer dead zones rescaled to the full range */
static void stick_dead_zones(const struct remap_analog *an, int16_t *x, int16_t *y)
{
	float fx = (float)*x / 32767.0f, fy = (float)*y / 32767.0f;
	float m = sqrtf(fx * fx + fy * fy);
	float in = (float)an->dz_in * 0.0001f, out = 1.0f - (float)an->dz_out * 0.0001f;
	float k;

	if(an->dz_in == 0U && an->dz_out == 0U && an->anti_dz == 0U && (an->curve == 0U || an->curve == 100U))
	{
		return;
	}
	if(out < in + 0.05f)
	{
		out = in + 0.05f;
	}
	if(m <= in)
	{
		*x = *y = 0;
		return;
	}
	k = MIN((m - in) / (out - in), 1.0f);
	if(an->curve != 0U && an->curve != 100U)
	{
		k = powf(k, (float)an->curve * 0.01f);
	}
	k = ((float)an->anti_dz * 0.0001f + (1.0f - (float)an->anti_dz * 0.0001f) * k) / m;
	*x = (int16_t)CLAMP(lroundf(fx * k * 32767.0f), -32767, 32767);
	*y = (int16_t)CLAMP(lroundf(fy * k * 32767.0f), -32767, 32767);
}

static void pad_mouse(int side, const struct remap_analog *an, bool active, int16_t x, int16_t y)
{
	if(!active)
	{
		pad_prev_valid[side] = false;
		return;
	}
	if(pad_prev_valid[side])
	{
		/* speed: mouse counts for a swipe across the whole pad */
		float k = (float)an->speed / 65535.0f;
		float dx = (float)(x - pad_prev[side][0]);
		float dy = (float)(y - pad_prev[side][1]);

		mouse_acc[0] += dx * k;
		mouse_acc[1] -= dy * k;
		pad_hap_dist[side] += sqrtf(dx * dx + dy * dy);
		if(pad_hap_dist[side] >= 3000.0f)
		{
			pad_hap_dist[side] = 0.0f;
			feedback(an->haptic, side);
		}
	}
	pad_prev[side][0] = x;
	pad_prev[side][1] = y;
	pad_prev_valid[side] = true;
}

static void send_mouse(void)
{
	uint8_t m[5];
	static uint8_t last_buttons;
	int8_t v[4];
	bool any = false;

	for(size_t i = 0; i < 4; ++i)
	{
		float t = CLAMP(mouse_acc[i] >= 0.0f ? floorf(mouse_acc[i]) : ceilf(mouse_acc[i]), -127.0f, 127.0f);

		v[i] = (int8_t)t;
		mouse_acc[i] -= t;
		any = any || v[i] != 0;
	}
	if(!any && mouse_buttons == last_buttons)
	{
		return;
	}
	m[0] = mouse_buttons;
	m[1] = (uint8_t)v[0];
	m[2] = (uint8_t)v[1];
	m[3] = (uint8_t)v[2];
	m[4] = (uint8_t)v[3];
	if(transport_send_input_report(0x40, m, sizeof(m)) == 0)
	{
		last_buttons = mouse_buttons;
		mon_dx += v[0];
		mon_dy += v[1];
		mon_wheel += v[2];
	}
	else
	{
		for(size_t i = 0; i < 4; ++i)
		{
			mouse_acc[i] += (float)v[i];
		}
	}
}

static void release_all(void)
{
	static const uint8_t zero[8];

	memset(st, 0, sizeof(st));
	for(size_t i = 0; i < REMAP_SRC_COUNT; ++i)
	{
		st[i].held_group = -1;
	}
	memset(mouse_acc, 0, sizeof(mouse_acc));
	memset(wheel_valid, 0, sizeof(wheel_valid));
	memset(pad_prev_valid, 0, sizeof(pad_prev_valid));
	memset(swipe_valid, 0, sizeof(swipe_valid));
	memset(flick_on, 0, sizeof(flick_on));
	memset(pad_joy, 0, sizeof(pad_joy));
	flick_left = 0.0f;
	if(mouse_buttons != 0U)
	{
		mouse_buttons = 0U;
		send_mouse();
	}
	if(kbd_valid && memcmp(last_kbd, zero, sizeof(zero)) != 0)
	{
		(void)transport_send_input_report(0x41, zero, sizeof(zero));
	}
	memset(last_kbd, 0, sizeof(last_kbd));
	kbd_valid = false;
}

void remap_set_enabled(bool on)
{
	(void)sc26re_cfg_set(SC26RE_CFG_REMAP_ON, on ? 1 : 0, true);
	if(!on)
	{
		release_all();
	}
	(void)haptics_play_beep(on ? 1300 : 450, on ? 120 : 200);
}

static float wrap180(float d)
{
	while(d > 180.0f)
	{
		d -= 360.0f;
	}
	while(d < -180.0f)
	{
		d += 360.0f;
	}
	return d;
}

/* flick stick: pushing past the threshold turns the camera to that direction
 * (spread over speed ms), turning the stick around keeps turning 1:1;
 * uses the gyro mouse counts per 360 */
static void flick(int i, const struct remap_analog *an, bool active, float fx, float fy)
{
	float th = (float)(an->dz_in ? an->dz_in : 8000) * 0.0001f;
	float m2 = fx * fx + fy * fy;
	float c360 = (float)gyro_mouse_counts_per_360();
	float a;

	if(!active || m2 < (flick_on[i] ? (th - 0.1f) * (th - 0.1f) : th * th))
	{
		flick_on[i] = false;
		return;
	}
	a = atan2f(fx, fy) * 57.29578f; /* 0 = forward, + = right */
	if(!flick_on[i])
	{
		uint16_t ms = an->speed ? an->speed : 100U;

		flick_on[i] = true;
		flick_left += c360 * a / 360.0f;
		flick_rate = fabsf(flick_left) * 1000.0f / (float)MAX(ms, 1U);
	}
	else
	{
		mouse_acc[0] += c360 * wrap180(a - flick_last[i]) / 360.0f;
	}
	flick_last[i] = a;
}

static void flick_step(float dt)
{
	float step;

	if(flick_left == 0.0f)
	{
		return;
	}
	step = MIN(fabsf(flick_left), flick_rate * MAX(dt, 0.0005f));
	step = flick_left > 0.0f ? step : -step;
	mouse_acc[0] += step;
	flick_left -= step;
	if(fabsf(flick_left) < 0.001f)
	{
		flick_left = 0.0f;
	}
}

/* directional swipe: each swipe of the threshold length fires one direction */
static void swipe(int j, const struct remap_analog *an, bool active, int16_t x, int16_t y)
{
	int32_t th = (int32_t)MAX(an->dz_in, 500) * 32767 / 10000;
	int32_t dx, dy;

	swipe_fire[j] = 0U;
	if(!active)
	{
		swipe_valid[j] = false;
		swipe_done[j] = false;
		return;
	}
	if(swipe_done[j])
	{
		return; /* one direction per touch unless repeat is on */
	}
	if(!swipe_valid[j])
	{
		swipe_anchor[j][0] = x;
		swipe_anchor[j][1] = y;
		swipe_valid[j] = true;
		return;
	}
	dx = x - swipe_anchor[j][0];
	dy = y - swipe_anchor[j][1];
	if(dx * dx + dy * dy < th * th)
	{
		return;
	}
	if(abs(dy) >= abs(dx))
	{
		swipe_fire[j] = dy > 0 ? 1U : 2U;
	}
	else
	{
		swipe_fire[j] = dx < 0 ? 4U : 8U;
	}
	swipe_anchor[j][0] = x;
	swipe_anchor[j][1] = y;
	swipe_done[j] = (an->flags & REMAP_AF_SWIPE_REPEAT) == 0U;
}

bool remap_gyro_value(int id, int16_t *value)
{
	const struct remap_profile *p = active();

	if(!remap_enabled() || p->magic != REMAP_MAGIC || p->version != REMAP_VERSION || !(p->flags & REMAP_PF_GYRO))
	{
		return false;
	}
	for(int k = 0; k < REMAP_GYRO_KEYS; ++k)
	{
		if(remap_gyro_keys[k] == id)
		{
			int16_t v;

			memcpy(&v, (const uint8_t *)p->gyro + k * sizeof(int16_t), sizeof(v));
			*value = v;
			return true;
		}
	}
	return false;
}

static struct controller_report raw_state;
static struct k_spinlock raw_lock;

void remap_raw_capture(const struct controller_report *r)
{
	k_spinlock_key_t key = k_spin_lock(&raw_lock);

	raw_state = *r;
	k_spin_unlock(&raw_lock, key);
}

void remap_raw_read(uint8_t *out)
{
	struct controller_report r;
	k_spinlock_key_t key = k_spin_lock(&raw_lock);
	int16_t ang[3], acc[3];
	int16_t v[12];

	r = raw_state;
	k_spin_unlock(&raw_lock, key);
	imu_raw_take(ang, acc);
	v[0] = r.trigger_left;
	v[1] = r.trigger_right;
	v[2] = r.stick_left_x;
	v[3] = r.stick_left_y;
	v[4] = r.stick_right_x;
	v[5] = r.stick_right_y;
	v[6] = r.touchpad_left_x;
	v[7] = r.touchpad_left_y;
	v[8] = (int16_t)r.touchpad_left_pressure;
	v[9] = r.touchpad_right_x;
	v[10] = r.touchpad_right_y;
	v[11] = (int16_t)r.touchpad_right_pressure;
	sys_put_le32(r.buttons, &out[0]);
	for(int i = 0; i < 12; ++i)
	{
		sys_put_le16((uint16_t)v[i], &out[4 + 2 * i]);
	}
	for(int i = 0; i < 3; ++i)
	{
		sys_put_le16((uint16_t)ang[i], &out[28 + 2 * i]);
		sys_put_le16((uint16_t)acc[i], &out[34 + 2 * i]);
	}
}

void remap_monitor(uint8_t *out)
{
	const struct remap_profile *p = active();
	uint8_t sh = 0;

	for(int g = 0; g < REMAP_SHIFT_GROUPS; ++g)
	{
		sh |= shifted[g] ? (uint8_t)BIT(g) : 0U;
	}
	out[0] = (uint8_t)sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE);
	out[1] = remap_enabled() ? (p->magic == REMAP_MAGIC && p->version == REMAP_VERSION ? 1U : 2U) : 0U;
	out[2] = sh;
	out[3] = remap_enabled() ? mouse_buttons : 0U;
	memcpy(&out[4], last_kbd, 8);
	sys_put_le32(remap_enabled() ? mon_pad : 0U, &out[12]);
	sys_put_le32(remap_xbox_buttons(), &out[16]);
	sys_put_le32((uint32_t)mon_dx, &out[20]);
	sys_put_le32((uint32_t)mon_dy, &out[24]);
	sys_put_le16((uint16_t)mon_wheel, &out[28]);
}

static int pc_last = -1; /* profile last applied, -2 = apply again */

/* profile becomes active / inactive: LED colour and "gyro on with this profile" */
static void profile_changed(void)
{
	static bool led_saved;
	static struct rgbw_color saved;
	const struct remap_profile *p = active();
	bool valid = p->magic == REMAP_MAGIC && p->version == REMAP_VERSION;
	int now = remap_enabled() && valid ? sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE) : -1;
	bool was_gyro_on = pc_last >= 0 && (profiles[pc_last].flags & REMAP_PF_GYRO_ON);

	if(now == pc_last)
	{
		return;
	}
	pc_last = now;
	if(now >= 0 && (p->led[0] | p->led[1] | p->led[2]))
	{
		if(!led_saved)
		{
			saved = rgbw_led_get();
			led_saved = true;
		}
		rgbw_led_set((struct rgbw_color){ p->led[0], p->led[1], p->led[2], 0 });
	}
	else if(led_saved)
	{
		rgbw_led_set(saved);
		led_saved = false;
	}
	if(now >= 0 && (p->flags & REMAP_PF_GYRO_ON))
	{
		if(sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) != 1)
		{
			gyro_mouse_set_enabled(true);
		}
	}
	else if(was_gyro_on && sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) == 1)
	{
		gyro_mouse_set_enabled(false);
	}
}

static const struct remap_analog *analog_cfg(const struct remap_profile *p, int i)
{
	int g = REMAP_SHIFT_LS + i;

	return shifted[g] ? &p->shift[g].an : &p->an[i];
}

static const struct remap_binding *binding(const struct remap_profile *p, int s)
{
	int g = src_group[s];

	return (g >= 0 && shifted[g]) ? &p->sbind[src_sidx[s]] : &p->bind[s];
}

static bool ring_test(const struct remap_analog *an, float fx, float fy)
{
	float r = (float)an->ring * 0.0001f;

	if(an->ring == 0U)
	{
		return false;
	}
	return (fx * fx + fy * fy >= r * r) != ((an->flags & REMAP_AF_RING_INVERT) != 0U);
}

void remap_process(struct controller_report *r, float dt)
{
	const struct remap_profile *p;
	uint32_t raw = r->buttons;
	uint8_t keys[6] = { 0 };
	size_t nk = 0;
	uint8_t mods = 0;
	uint32_t pad = 0;
	uint32_t hide = 0;
	uint32_t bound_mask = 0;
	int64_t now = k_uptime_get();

	/* LB + RB + Menu: on/off on release; hidden from the game while held */
	if((raw & CHORD_MASK) == CHORD_MASK)
	{
		chord_down = true;
	}
	else if(chord_down && (raw & CHORD_MASK) == 0U)
	{
		chord_down = false;
		remap_set_enabled(!remap_enabled());
	}
	if(chord_down)
	{
		r->buttons &= ~CHORD_MASK;
		raw &= ~CHORD_MASK;
	}
	/*
	 * Steam + D-pad: choose the profile (up 1, right 2, down 3, left 4),
	 * whether the remapper is on or off. While Steam is held the D-pad is
	 * hidden from the host and from the bindings.
	 */
	{
		static uint32_t dp_prev, dp_hide;
		static const uint8_t dp_bit[REMAP_PROFILES] = { CONTROLLER_BUTTON_DPAD_UP, CONTROLLER_BUTTON_DPAD_RIGHT,
		                                                CONTROLLER_BUTTON_DPAD_DOWN, CONTROLLER_BUTTON_DPAD_LEFT };
		uint32_t dp = (raw & BIT(CONTROLLER_BUTTON_STEAM)) ? (raw & DP_MASK) : 0U;

		for(int i = 0; i < REMAP_PROFILES; ++i)
		{
			if((dp & ~dp_prev) & BIT(dp_bit[i]))
			{
				if(i != sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE))
				{
					release_all();
				}
				(void)sc26re_cfg_set(SC26RE_CFG_REMAP_PROFILE, (int16_t)i, true);
				(void)haptics_play_beep((uint16_t)(700 + 150 * i), 80);
			}
		}
		dp_prev = dp;
		/* a D-pad direction touched while Steam is held stays hidden until
		 * it is released, even when Steam is let go first */
		if(raw & BIT(CONTROLLER_BUTTON_STEAM))
		{
			dp_hide |= raw & DP_MASK;
		}
		dp_hide &= raw & DP_MASK;
		r->buttons &= ~dp_hide;
		raw &= ~dp_hide;
	}
	profile_changed();
	if(!remap_enabled())
	{
		return;
	}
	p = active();
	if(p->magic != REMAP_MAGIC || p->version != REMAP_VERSION)
	{
		return; /* empty profile: nothing mapped */
	}

	/* mode shifts: while a group's shift button is held it uses its
	 * alternative settings and bindings; its sources restart cleanly */
	for(int g = 0; g < REMAP_SHIFT_GROUPS; ++g)
	{
		uint8_t sb = p->shift[g].button;
		bool on = sb >= 1U && sb <= 30U && (raw & BIT(sb - 1U)) != 0U;

		if(sb >= 1U && sb <= 30U)
		{
			hide |= BIT(sb - 1U);
		}
		if(on != shifted[g])
		{
			shifted[g] = on;
			for(int k = shift_first[g]; k < shift_first[g + 1]; ++k)
			{
				struct src_state *ss = &st[remap_shift_src[k]];

				memset(ss, 0, sizeof(*ss));
				ss->held_group = -1;
			}
			if(g >= REMAP_SHIFT_LPAD && g <= REMAP_SHIFT_RPAD)
			{
				wheel_valid[g - REMAP_SHIFT_LPAD] = false;
				pad_prev_valid[g - REMAP_SHIFT_LPAD] = false;
			}
		}
	}
	/* physical D-pad: 4-way keeps one direction at a time */
	{
		uint32_t d = raw & DP_MASK;

		if((shifted[REMAP_SHIFT_DPAD] ? p->shift[REMAP_SHIFT_DPAD].dpad_4way : p->dpad_4way) && d != 0U)
		{
			d = (dp4_keep & d) ? dp4_keep : (d & (~d + 1U));
		}
		dp4_keep = d;
		btn = (raw & ~DP_MASK) | d;
	}
	/* sticks */
	for(int i = 0; i < 2; ++i)
	{
		const struct remap_analog *an = analog_cfg(p, i);
		int16_t x = i ? r->stick_right_x : r->stick_left_x;
		int16_t y = i ? r->stick_right_y : r->stick_left_y;

		ring_on[i] = ring_test(an, (float)x / 32767.0f, (float)y / 32767.0f);
		dirs[i] = (an->mode == REMAP_ANALOG_DPAD4 || an->mode == REMAP_ANALOG_DPAD8)
		              ? dir_mask(an->mode, (float)x / 32767.0f, (float)y / 32767.0f,
		                         (float)MAX(an->dz_in, 500) * 0.0001f)
		              : 0U;
	}
	/* trackpads: active while touched or while clicked */
	for(int j = 0; j < 2; ++j)
	{
		const struct remap_analog *an = analog_cfg(p, 2 + j);
		uint32_t act_bit = (an->flags & REMAP_AF_CLICK)
		                       ? BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_CLICK : CONTROLLER_BUTTON_LEFT_TOUCHPAD_CLICK)
		                       : BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH : CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH);
		bool act = (an->flags & REMAP_AF_PRESSURE)
		               ? (raw & BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH : CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH)) &&
		                     (int32_t)(j ? r->touchpad_right_pressure : r->touchpad_left_pressure) >=
		                         (int32_t)MAX(an->press, 100) * 32767 / 10000
		               : (raw & act_bit) != 0U;
		int16_t x = j ? r->touchpad_right_x : r->touchpad_left_x;
		int16_t y = j ? r->touchpad_right_y : r->touchpad_left_y;
		float fx = (float)x / 32767.0f, fy = (float)y / 32767.0f;
		float dz = (float)an->dz_in * 0.0001f;

		ring_on[2 + j] = act && ring_test(an, fx, fy);
		dirs[2 + j] = act && (an->mode == REMAP_ANALOG_DPAD4 || an->mode == REMAP_ANALOG_DPAD8)
		                  ? dir_mask(an->mode, fx, fy, dz)
		                  : 0U;
		gates[j] = act && an->mode == REMAP_ANALOG_GATES ? gate_mask(fx, fy, dz) : 0U;
		center_on[j] = false;
		bool touched = (raw & BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH : CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH)) != 0U;

		pad_act[j] = act;
		int grid_zone = -1;

		if(touched && an->mode == REMAP_ANALOG_GRID)
		{
			/* 3 x 3 zones split by the grid lines: corners = gates, edges = directions */
			static const uint8_t zone_dir[9] = { 0, 1, 0, 4, 0, 8, 0, 2, 0 };
			static const uint8_t zone_gate[9] = { 1, 0, 2, 0, 0, 0, 4, 0, 8 };
			int16_t gl[4];
			float l, rr, t, bt;

			memcpy(gl, (const void *)p->grid[j], sizeof(gl));
			l = gl[0] ? gl[0] * 0.0001f : -0.3333f;
			rr = gl[1] ? gl[1] * 0.0001f : 0.3333f;
			t = gl[2] ? gl[2] * 0.0001f : 0.3333f;
			bt = gl[3] ? gl[3] * 0.0001f : -0.3333f;
			int col = fx < l ? 0 : (fx > rr ? 2 : 1);
			int row = fy > t ? 0 : (fy < bt ? 2 : 1);
			int z = row * 3 + col;

			grid_zone = z;
			if(act)
			{
				dirs[2 + j] = zone_dir[z];
				gates[j] = zone_gate[z];
				center_on[j] = z == 4;
			}
		}
		{
			/* zone feedback: a haptic tick whenever the finger crosses into
			 * another zone (gates, grid, D-pad on the pad) */
			static int16_t last_zone[2] = { -1, -1 };
			int16_t zone = -1;

			/* felt as soon as the finger touches, before any click: you find the
			 * zone by feel, then press */
			if(touched && an->mode == REMAP_ANALOG_GATES)
			{
				zone = gate_mask(fx, fy, dz);
			}
			else if(touched && an->mode == REMAP_ANALOG_GRID)
			{
				zone = (int16_t)grid_zone;
			}
			else if(touched && (an->mode == REMAP_ANALOG_DPAD4 || an->mode == REMAP_ANALOG_DPAD8))
			{
				zone = dir_mask(an->mode, fx, fy, dz);
			}
			if(zone >= 0 && last_zone[j] >= 0 && zone != last_zone[j])
			{
				feedback(an->haptic, j);
			}
			last_zone[j] = zone;
		}
		if(an->mode == REMAP_ANALOG_SCROLL)
		{
			wheel(j, an, act, fx, fy);
		}
		else
		{
			wheel_fire[j] = 0U;
			wheel_valid[j] = false;
		}
		if(an->mode == REMAP_ANALOG_MOUSE)
		{
			pad_mouse(j, an, act, x, y);
		}
		else
		{
			pad_prev_valid[j] = false;
		}
		swipe_mode[j] = an->mode == REMAP_ANALOG_SWIPE;
		if(swipe_mode[j])
		{
			swipe(j, an, act, x, y);
		}
		else
		{
			swipe_fire[j] = 0U;
			swipe_valid[j] = false;
		}
		if(an->mode == REMAP_ANALOG_FLICK)
		{
			flick(2 + j, an, act, fx, fy);
		}
		else
		{
			flick_on[2 + j] = false;
		}
		{
			int t = j ^ ((an->flags & REMAP_AF_OTHER_STICK) ? 1 : 0);

			pad_joy[t][0] = pad_joy[t][1] = 0.0f;
			if(an->mode == REMAP_ANALOG_JOYSTICK && act)
			{
				int16_t jx = x, jy = y;

				stick_dead_zones(an, &jx, &jy);
				pad_joy[t][0] = (float)jx / 32767.0f;
				pad_joy[t][1] = (float)jy / 32767.0f;
			}
		}
		if(an->mode != REMAP_ANALOG_PASS)
		{
			if(j)
			{
				r->touchpad_right_x = r->touchpad_right_y = 0;
				r->touchpad_right_pressure = 0;
			}
			else
			{
				r->touchpad_left_x = r->touchpad_left_y = 0;
				r->touchpad_left_pressure = 0;
			}
			hide |= BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH : CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH);
			if(an->flags & REMAP_AF_CLICK)
			{
				hide |= act_bit;
			}
		}
	}
	/* triggers: soft pull point with a little hysteresis */
	for(int t = 0; t < 2; ++t)
	{
		int16_t v = t ? r->trigger_right : r->trigger_left;
		int32_t th = (int32_t)CLAMP(p->trig_soft[t] ? p->trig_soft[t] : 5000, 100, 10000) * 32767 / 10000;

		soft_on[t] = soft_on[t] ? v >= th - 1000 : v >= th;
		if(bound(binding(p, REMAP_SRC_LT_SOFT + t)))
		{
			if(t)
			{
				r->trigger_right = 0;
			}
			else
			{
				r->trigger_left = 0;
			}
		}
	}

	mouse_buttons = 0U;
	xbox_buttons = 0U;
	move_dirs = 0U;
	for(int s = 0; s < REMAP_SRC_COUNT; ++s)
	{
		const struct remap_binding *b = binding(p, s);
		struct src_state *ss = &st[s];

		if(!bound(b))
		{
			continue;
		}
		if(s < 30)
		{
			hide |= BIT(s);
			bound_mask |= BIT(s);
		}
		if(s >= REMAP_SRC_LPAD_CW && s < REMAP_SRC_LT_SOFT)
		{
			int j = (s - REMAP_SRC_LPAD_CW) / 2;

			if(wheel_fire[j] & BIT((s - REMAP_SRC_LPAD_CW) % 2))
			{
				pulse(b, ss, s, now);
			}
		}
		else if(s >= REMAP_SRC_LPAD_UP && s < REMAP_SRC_LPAD_GATE && swipe_mode[(s - REMAP_SRC_LPAD_UP) / 4])
		{
			if(swipe_fire[(s - REMAP_SRC_LPAD_UP) / 4] & BIT((s - REMAP_SRC_LPAD_UP) % 4))
			{
				pulse(b, ss, s, now);
			}
		}
		else
		{
			int pj = -1;

			if((s >= REMAP_SRC_LPAD_UP && s < REMAP_SRC_LPAD_CW))
			{
				pj = s < REMAP_SRC_LPAD_GATE ? (s - REMAP_SRC_LPAD_UP) / 4 : (s - REMAP_SRC_LPAD_GATE) / 4;
			}
			else if(s == REMAP_SRC_LS_RING + 2 || s == REMAP_SRC_LS_RING + 3)
			{
				pj = s - REMAP_SRC_LS_RING - 2;
			}
			else if(s >= REMAP_SRC_LPAD_CENTER)
			{
				pj = s - REMAP_SRC_LPAD_CENTER;
			}
			run_source(b, ss, src_pressed(s), now, s, pj < 0 || !pad_act[pj]);
		}
		if(ss->held_group >= 0)
		{
			for(size_t i = 0; i < 4; ++i)
			{
				out_action(b->act[ss->held_group][i], keys, &nk, &mods, &pad);
			}
		}
		if(now < ss->pulse_until)
		{
			for(size_t i = 0; i < 4; ++i)
			{
				out_action(b->act[ss->pulse_group][i], keys, &nk, &mods, &pad);
			}
		}
		if(now < ss->release_until)
		{
			for(size_t i = 0; i < 4; ++i)
			{
				out_action(b->act[2][i], keys, &nk, &mods, &pad);
			}
		}
	}

	/* sticks: mouse / scroll, or to Steam with the dead zones applied */
	for(int i = 0; i < 2; ++i)
	{
		const struct remap_analog *an = analog_cfg(p, i);
		int16_t x = i ? r->stick_right_x : r->stick_left_x;
		int16_t y = i ? r->stick_right_y : r->stick_left_y;

		if(an->mode == REMAP_ANALOG_PASS)
		{
			stick_dead_zones(an, &x, &y);
		}
		else
		{
			if(an->mode == REMAP_ANALOG_FLICK)
			{
				flick(i, an, true, (float)x / 32767.0f, (float)y / 32767.0f);
			}
			else
			{
				flick_on[i] = false;
				stick_analog(i, an, x, y, dt);
			}
			x = y = 0;
		}
		x = (int16_t)CLAMP((int32_t)x + (int32_t)lroundf(pad_joy[i][0] * 32767.0f), -32767, 32767);
		y = (int16_t)CLAMP((int32_t)y + (int32_t)lroundf(pad_joy[i][1] * 32767.0f), -32767, 32767);
		if(i)
		{
			r->stick_right_x = x;
			r->stick_right_y = y;
		}
		else
		{
			r->stick_left_x = x;
			r->stick_left_y = y;
		}
	}
	flick_step(dt);
	if(move_dirs)
	{
		float k = (float)(p->move_speed ? p->move_speed : 1000U) * dt;

		mouse_acc[0] += k * (float)(((move_dirs >> 3) & 1) - ((move_dirs >> 2) & 1));
		mouse_acc[1] += k * (float)(((move_dirs >> 1) & 1) - (move_dirs & 1));
	}
	if(p->flags & REMAP_PF_BLOCK_UNBOUND)
	{
		/* only what is mapped reaches Steam: unbound buttons and touch sensors
		 * are dropped, trackpads left "to Steam" too (Steam button stays) */
		hide |= (BIT(30) - 1U) & ~bound_mask & ~BIT(CONTROLLER_BUTTON_STEAM);
		for(int j = 0; j < 2; ++j)
		{
			if(analog_cfg(p, 2 + j)->mode == REMAP_ANALOG_PASS)
			{
				if(j)
				{
					r->touchpad_right_x = r->touchpad_right_y = 0;
					r->touchpad_right_pressure = 0;
				}
				else
				{
					r->touchpad_left_x = r->touchpad_left_y = 0;
					r->touchpad_left_pressure = 0;
				}
			}
		}
	}
	r->buttons = (r->buttons & ~hide) | pad;
	mon_pad = pad;

	{
		uint8_t kbd[8] = { mods, 0 };

		memcpy(&kbd[2], keys, sizeof(keys));
		if(!kbd_valid || memcmp(kbd, last_kbd, sizeof(kbd)) != 0)
		{
			if(transport_send_input_report(0x41, kbd, sizeof(kbd)) == 0)
			{
				memcpy(last_kbd, kbd, sizeof(kbd));
				kbd_valid = true;
			}
		}
	}
	send_mouse();
}


/* ---- storage and report meter access ---- */

static int parse_path(const char *path, int *prof, int *off)
{
	if(strncmp(path, PATH_PREFIX, strlen(PATH_PREFIX)) != 0)
	{
		return -1;
	}
	path += strlen(PATH_PREFIX);
	if(path[0] < '0' || path[0] >= '0' + REMAP_PROFILES)
	{
		return -1;
	}
	*prof = path[0] - '0';
	*off = -1;
	if(path[1] == '/')
	{
		*off = (int)strtol(&path[2], NULL, 16);
		if(*off < 0 || *off >= (int)sizeof(struct remap_profile))
		{
			return -1;
		}
	}
	else if(path[1] != '\0')
	{
		return -1;
	}
	return 0;
}

bool remap_path(const char *path)
{
	int p, o;

	return parse_path(path, &p, &o) == 0;
}

bool remap_read(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
	int p, o;
	size_t n;

	if(parse_path(path, &p, &o) != 0 || o < 0)
	{
		return false;
	}
	n = MIN(MIN((size_t)CHUNK, capacity), sizeof(struct remap_profile) - (size_t)o);
	memcpy(buf, (const uint8_t *)&profiles[p] + o, n);
	*len = n;
	return true;
}

int remap_stage(const char *path, const uint8_t *value, size_t len)
{
	int p, o;

	if(parse_path(path, &p, &o) != 0 || o < 0)
	{
		return -EINVAL;
	}
	len = MIN(len, sizeof(struct remap_profile) - (size_t)o);
	if(p == sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE) && remap_enabled())
	{
		release_all();
	}
	memcpy((uint8_t *)&profiles[p] + o, value, len);
	profile_dirty[p] = true;
	return 0;
}

int remap_commit(const char *path)
{
	char key[24];
	int p, o;

	if(parse_path(path, &p, &o) != 0)
	{
		return -EINVAL;
	}
	snprintf(key, sizeof(key), "user/remap/%d", p);
	profile_dirty[p] = false;
	pc_last = -2; /* LED / gyro of the saved profile apply again */
	if(profiles[p].magic != REMAP_MAGIC)
	{
		return settings_delete(key);
	}
	return settings_save_one(key, &profiles[p], sizeof(profiles[p]));
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	ARG_UNUSED(param);
	if(key == NULL || key[0] < '0' || key[0] >= '0' + REMAP_PROFILES || key[1] != '\0' ||
	   len != sizeof(struct remap_profile))
	{
		return 0;
	}
	(void)read_cb(cb_arg, &profiles[key[0] - '0'], sizeof(struct remap_profile));
	return 0;
}

void remap_init(void)
{
	for(size_t i = 0; i < REMAP_SRC_COUNT; ++i)
	{
		st[i].held_group = -1;
		src_group[i] = -1;
		src_sidx[i] = -1;
	}
	for(int g = 0; g < REMAP_SHIFT_GROUPS; ++g)
	{
		for(int k = shift_first[g]; k < shift_first[g + 1]; ++k)
		{
			src_group[remap_shift_src[k]] = (int8_t)g;
			src_sidx[remap_shift_src[k]] = (int8_t)k;
		}
	}
	(void)settings_load_subtree_direct("user/remap", load_cb, NULL);
}
