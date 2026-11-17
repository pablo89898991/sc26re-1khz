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
#include <zephyr/sys/util.h>

#include "controller.h"
#include "haptics.h"
#include "remap.h"
#include "sc26re_cfg.h"

LOG_MODULE_REGISTER(remap, LOG_LEVEL_INF);

#define HOLD_MS 250
#define DOUBLE_MS 250
#define TAP_MS 40
#define CHORD_MASK (BIT(CONTROLLER_BUTTON_LEFT_SHOULDER) | BIT(CONTROLLER_BUTTON_RIGHT_SHOULDER) | \
                    BIT(CONTROLLER_BUTTON_MENU))
#define PATH_PREFIX "sc26re/rmp/"
#define CHUNK 48

BUILD_ASSERT(sizeof(struct remap_profile) == 1704, "remap profile layout (web/remap.html)");

static struct remap_profile profiles[REMAP_PROFILES];

/* sources of each mode shift group, in the order of remap_profile.sbind */
const uint8_t remap_shift_src[REMAP_SHIFT_SRC_COUNT] = {
	0, 1, 2, 3,                                   /* ABXY */
	13, 10, 12, 11,                               /* D-pad */
	30, 31, 32, 33, 60,                           /* left stick */
	34, 35, 36, 37, 61,                           /* right stick */
	38, 39, 40, 41, 46, 47, 48, 49, 54, 55, 62,   /* left pad */
	42, 43, 44, 45, 50, 51, 52, 53, 56, 57, 63,   /* right pad */
	58, 27,                                       /* left trigger */
	59, 23,                                       /* right trigger */
};
static const uint8_t shift_first[REMAP_SHIFT_GROUPS + 1] = { 0, 4, 8, 13, 18, 29, 40, 42, 44 };
static int8_t src_group[REMAP_SRC_COUNT]; /* -1: not in a shift group */
static int8_t src_sidx[REMAP_SRC_COUNT];
static bool shifted[REMAP_SHIFT_GROUPS];
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
};
static struct src_state st[REMAP_SRC_COUNT];

static uint8_t last_kbd[8];
static bool kbd_valid;
static uint8_t mouse_buttons;
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
	return (s - REMAP_SRC_LS_RING) % 2;
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
	if(!active || fx * fx + fy * fy < powf(MAX(an->dz_in, 5) * 0.01f, 2.0f))
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
	else if(a >= REMAP_ACT_PAD && a < REMAP_ACT_PAD + 30U)
	{
		*pad |= BIT(a - REMAP_ACT_PAD);
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

static void run_source(const struct remap_binding *b, struct src_state *s, bool down, int64_t now, int src)
{
	bool hold = (b->mode & REMAP_MODE_HOLD) != 0U;
	bool dbl = (b->mode & REMAP_MODE_DOUBLE) != 0U;

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
	float dz = (float)MAX(an->dz_in, 3) * 0.01f;
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
		float k = (float)an->speed * dt * powf(mm, an->curve ? (float)an->curve * 0.1f : 2.0f);

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
	float in = (float)an->dz_in * 0.01f, out = 1.0f - (float)an->dz_out * 0.01f;
	float k;

	if(an->dz_in == 0U && an->dz_out == 0U && an->anti_dz == 0U && (an->curve == 0U || an->curve == 10U))
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
	if(an->curve != 0U && an->curve != 10U)
	{
		k = powf(k, (float)an->curve * 0.1f);
	}
	k = ((float)an->anti_dz * 0.01f + (1.0f - (float)an->anti_dz * 0.01f) * k) / m;
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
	float r = (float)an->ring * 0.01f;

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
		                         (float)MAX(an->dz_in, 5) * 0.01f)
		              : 0U;
	}
	/* trackpads: active while touched or while clicked */
	for(int j = 0; j < 2; ++j)
	{
		const struct remap_analog *an = analog_cfg(p, 2 + j);
		uint32_t act_bit = (an->flags & REMAP_AF_CLICK)
		                       ? BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_CLICK : CONTROLLER_BUTTON_LEFT_TOUCHPAD_CLICK)
		                       : BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH : CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH);
		bool act = (raw & act_bit) != 0U;
		int16_t x = j ? r->touchpad_right_x : r->touchpad_left_x;
		int16_t y = j ? r->touchpad_right_y : r->touchpad_left_y;
		float fx = (float)x / 32767.0f, fy = (float)y / 32767.0f;
		float dz = (float)an->dz_in * 0.01f;

		ring_on[2 + j] = act && ring_test(an, fx, fy);
		dirs[2 + j] = act && (an->mode == REMAP_ANALOG_DPAD4 || an->mode == REMAP_ANALOG_DPAD8)
		                  ? dir_mask(an->mode, fx, fy, dz)
		                  : 0U;
		gates[j] = act && an->mode == REMAP_ANALOG_GATES ? gate_mask(fx, fy, dz) : 0U;
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
		int32_t th = (int32_t)CLAMP(p->trig_soft[t] ? p->trig_soft[t] : 50, 1, 100) * 32767 / 100;

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
		}
		if(s >= REMAP_SRC_LPAD_CW && s < REMAP_SRC_LT_SOFT)
		{
			int j = (s - REMAP_SRC_LPAD_CW) / 2;

			if(wheel_fire[j] & BIT((s - REMAP_SRC_LPAD_CW) % 2))
			{
				pulse(b, ss, s, now);
			}
		}
		else
		{
			run_source(b, ss, src_pressed(s), now, s);
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
			stick_analog(i, an, x, y, dt);
			x = y = 0;
		}
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
	r->buttons = (r->buttons & ~hide) | pad;

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
