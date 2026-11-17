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

static struct remap_profile profiles[REMAP_PROFILES];
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

/* source pressed? digital sources are button bits; the rest are derived from axes */
static bool src_pressed(const struct remap_profile *p, const struct controller_report *r, int s)
{
	if(s < 30)
	{
		return (r->buttons & BIT(s)) != 0U;
	}
	if(s < REMAP_SRC_LPAD_UP)
	{
		/* sticks as 4 directions: 30..33 left up/down/left/right, 34..37 right */
		bool left = s < REMAP_SRC_RS_UP;
		int16_t x = left ? r->stick_left_x : r->stick_right_x;
		int16_t y = left ? r->stick_left_y : r->stick_right_y;
		int16_t th = 16384;

		switch((s - REMAP_SRC_LS_UP) % 4)
		{
			case 0: return y > th;
			case 1: return y < -th;
			case 2: return x < -th;
			default: return x > th;
		}
	}
	{
		/* trackpads as 4 regions while touched: 38..41 left, 42..45 right */
		bool left = s < REMAP_SRC_RPAD_UP;
		bool touched = (r->buttons & BIT(left ? CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH
		                                      : CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH)) != 0U;
		int16_t x = left ? r->touchpad_left_x : r->touchpad_right_x;
		int16_t y = left ? r->touchpad_left_y : r->touchpad_right_y;
		int dir = (s - REMAP_SRC_LPAD_UP) % 4;

		if(!touched || (abs(x) < 9830 && abs(y) < 9830))
		{
			return false; /* centre (30 %) is neutral */
		}
		if(abs(y) >= abs(x))
		{
			return dir == (y > 0 ? 0 : 1);
		}
		return dir == (x < 0 ? 2 : 3);
	}
	ARG_UNUSED(p);
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

static void activate(const struct remap_binding *b, int g, int8_t *slot)
{
	if(*slot != g)
	{
		*slot = (int8_t)g;
		for(size_t i = 0; i < 4; ++i)
		{
			edge_action(b->act[g][i]);
		}
	}
}

static void run_source(const struct remap_binding *b, struct src_state *s, bool down, int64_t now)
{
	bool hold = (b->mode & REMAP_MODE_HOLD) != 0U;
	bool dbl = (b->mode & REMAP_MODE_DOUBLE) != 0U;

	if(b->mode & REMAP_MODE_TOGGLE)
	{
		if(down && !s->down)
		{
			s->toggled = !s->toggled;
		}
		s->down = down;
		if(s->toggled)
		{
			activate(b, 0, &s->held_group);
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
			activate(b, 0, &s->held_group);
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
			activate(b, 2, &s->held_group); /* second press: group 2 while held */
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
				s->pulse_group = 0;
				s->pulse_until = now + TAP_MS;
				for(size_t i = 0; i < 4; ++i)
				{
					edge_action(b->act[0][i]);
				}
			}
		}
	}
	else if(down && hold && s->held_group < 0 && now - s->t_down >= HOLD_MS)
	{
		activate(b, 1, &s->held_group);
	}
	if(s->waiting_double && !down && now - s->t_up > DOUBLE_MS)
	{
		s->waiting_double = false;
		s->pulse_group = 0;
		s->pulse_until = now + TAP_MS;
		for(size_t i = 0; i < 4; ++i)
		{
			edge_action(b->act[0][i]);
		}
	}
	s->down = down;
}

static void stick_analog(uint8_t mode, uint16_t speed, int16_t x, int16_t y, float dt)
{
	float fx = (float)x / 32767.0f;
	float fy = (float)y / 32767.0f;
	float m = fx * fx + fy * fy;

	if(m < 0.01f) /* 10 % dead zone */
	{
		return;
	}
	if(mode == REMAP_ANALOG_MOUSE)
	{
		/* speed: counts per second at full deflection, squared response */
		float k = (float)speed * dt * sqrtf(m);

		mouse_acc[0] += fx * k;
		mouse_acc[1] -= fy * k;
	}
	else if(mode == REMAP_ANALOG_SCROLL)
	{
		mouse_acc[2] += fy * (float)speed * 0.01f * dt;
		mouse_acc[3] += fx * (float)speed * 0.01f * dt;
	}
}

static void pad_analog(int side, uint8_t mode, uint16_t sens, bool touched, int16_t x, int16_t y)
{
	if(!touched)
	{
		pad_prev_valid[side] = false;
		return;
	}
	if(pad_prev_valid[side] && (mode == REMAP_ANALOG_MOUSE || mode == REMAP_ANALOG_SCROLL))
	{
		/* sens: mouse counts for a swipe across the whole pad */
		float k = (float)sens / 65535.0f;
		float dx = (float)(x - pad_prev[side][0]) * k;
		float dy = (float)(y - pad_prev[side][1]) * k;

		if(mode == REMAP_ANALOG_MOUSE)
		{
			mouse_acc[0] += dx;
			mouse_acc[1] -= dy;
		}
		else
		{
			mouse_acc[2] += dy * 0.05f;
			mouse_acc[3] += dx * 0.05f;
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
		static uint32_t dp_prev;
		const uint32_t dp_mask = BIT(CONTROLLER_BUTTON_DPAD_UP) | BIT(CONTROLLER_BUTTON_DPAD_RIGHT) |
		                         BIT(CONTROLLER_BUTTON_DPAD_DOWN) | BIT(CONTROLLER_BUTTON_DPAD_LEFT);
		static const uint8_t dp_bit[REMAP_PROFILES] = { CONTROLLER_BUTTON_DPAD_UP, CONTROLLER_BUTTON_DPAD_RIGHT,
		                                                CONTROLLER_BUTTON_DPAD_DOWN, CONTROLLER_BUTTON_DPAD_LEFT };
		uint32_t dp = (raw & BIT(CONTROLLER_BUTTON_STEAM)) ? (raw & dp_mask) : 0U;

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
		if(raw & BIT(CONTROLLER_BUTTON_STEAM))
		{
			r->buttons &= ~dp_mask;
			raw &= ~dp_mask;
		}
	}
	if(!remap_enabled())
	{
		return;
	}
	p = active();
	if(p->magic != REMAP_MAGIC)
	{
		return; /* empty profile: nothing mapped */
	}

	mouse_buttons = 0U;
	for(int s = 0; s < REMAP_SRC_COUNT; ++s)
	{
		const struct remap_binding *b = &p->bind[s];
		struct src_state *ss = &st[s];

		if(!bound(b))
		{
			continue;
		}
		if(s < 30)
		{
			hide |= BIT(s);
		}
		run_source(b, ss, src_pressed(p, r, s), now);
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

	/* sticks and trackpads in an analog mode leave the Steam report */
	stick_analog(p->ls_mode, p->ls_speed, r->stick_left_x, r->stick_left_y, dt);
	stick_analog(p->rs_mode, p->rs_speed, r->stick_right_x, r->stick_right_y, dt);
	pad_analog(0, p->lp_mode, p->lp_sens, (raw & BIT(CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH)) != 0U,
	           r->touchpad_left_x, r->touchpad_left_y);
	pad_analog(1, p->rp_mode, p->rp_sens, (raw & BIT(CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH)) != 0U,
	           r->touchpad_right_x, r->touchpad_right_y);
	if(p->ls_mode != REMAP_ANALOG_PASS)
	{
		r->stick_left_x = r->stick_left_y = 0;
	}
	if(p->rs_mode != REMAP_ANALOG_PASS)
	{
		r->stick_right_x = r->stick_right_y = 0;
	}
	if(p->lp_mode != REMAP_ANALOG_PASS)
	{
		r->touchpad_left_x = r->touchpad_left_y = 0;
		r->touchpad_left_pressure = 0;
		hide |= BIT(CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH);
	}
	if(p->rp_mode != REMAP_ANALOG_PASS)
	{
		r->touchpad_right_x = r->touchpad_right_y = 0;
		r->touchpad_right_pressure = 0;
		hide |= BIT(CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH);
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
	}
	(void)settings_load_subtree_direct("user/remap", load_cb, NULL);
}
