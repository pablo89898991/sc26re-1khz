/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * On-controller remapper: profiles stored in the controller turn its inputs
 * (buttons, back buttons, trigger clicks, stick / trackpad / grip touch,
 * sticks, trackpads) into keyboard, mouse and Steam pad outputs. The report
 * meter's remapper page edits the profiles.
 *
 * While it is on (Steam + Menu toggles it, or the editor), every input that
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
#include "curve.h"
#if defined(CONFIG_IBEX_HWTEST) || defined(SC26RE_SIM)
#include "hwtest.h"
#endif
#include "xinput.h"
#include "imu.h"
#include "rgbw_led.h"
#include "charge_mode.h"
#include "sc26re_cfg.h"

/* accelerometer (v51.8), defined with its storage further down */
static int active_index(void);
static void acc_process(int i, const struct controller_report *rs, float dt);
static bool acc_ref_ok;
static float acc_joy[2][2];
static uint8_t acc_dirs; /* tilt -> buttons this pass, the gyro_dirs bits */
static struct remap_accel accs[REMAP_PROFILES];
static uint32_t gest_until[REMAP_ACC_GESTURES]; /* k_uptime_get_32() until which a gesture taps */
static void acc_gestures_reset(void);

LOG_MODULE_REGISTER(remap, LOG_LEVEL_INF);

static uint16_t prof_hold_ms = 250;
static uint16_t prof_double_ms = 250;
static uint16_t prof_turbo_ms = 100;
#define TAP_MS 40
#define RADIO_RELEASE_MS 20 /* a release goes out this long after the press went on air */
#define RADIO_RELEASE_MAX_WAIT 250 /* ... but never waits longer than this */

/* over the radio: may a release go out now? (USB: always) */
static bool release_ok_now(void)
{
	static int64_t waiting_since = -1;
	int64_t now = k_uptime_get();

	if(transport_input_idle_ms() >= RADIO_RELEASE_MS)
	{
		waiting_since = -1;
		return true;
	}
	if(waiting_since < 0)
	{
		waiting_since = now;
	}
	if(now - waiting_since >= RADIO_RELEASE_MAX_WAIT)
	{
		waiting_since = -1;
		return true;
	}
	return false;
}
#define PATH_PREFIX "sc26re/rmp/"
#define CHUNK 48

BUILD_ASSERT(sizeof(struct remap_profile) == 3232, "remap profile layout (web/remap.html)");
BUILD_ASSERT(offsetof(struct remap_profile, ext) == REMAP_PROFILE_V6_SIZE, "v7 area after the v6 layout");
BUILD_ASSERT(offsetof(struct remap_profile, v8) == REMAP_PROFILE_V7_SIZE, "v8 area after the v7 layout");

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
/* v8 shifted clicks (remap_profile.v8.sclick): source and its shift group */
static const uint8_t sclick_src[REMAP_SCLICK_COUNT] = { 15, 5, 26, 22 };
static const uint8_t sclick_group[REMAP_SCLICK_COUNT] = { REMAP_SHIFT_LS, REMAP_SHIFT_RS, REMAP_SHIFT_LPAD,
                                                          REMAP_SHIFT_RPAD };
static int8_t src_group[REMAP_SRC_COUNT]; /* -1: not in a shift group */
static int8_t src_sidx[REMAP_SRC_COUNT];
static bool shifted[REMAP_SHIFT_GROUPS];
/* which sources have a binding, for the profile / layer / shift state it was worked out for;
 * prof_gen moves on every profile load or save, so an edit is never missed */
static uint32_t prof_gen;
static struct
{
	const struct remap_profile *p, *l;
	uint32_t gen;
	uint32_t m[3];
	uint8_t sh;
	bool ok;
} bcache;
_Static_assert(REMAP_SRC_COUNT <= 96 && REMAP_SHIFT_GROUPS <= 8, "bcache sizes");

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

/* the slot of each gyro setting in a profile, + 1 (0: not a gyro setting): remap_gyro_value()
 * runs some 20 times per gyro sample */
static const uint8_t gyro_key_slot[SC26RE_CFG_COUNT] = {
	[SC26RE_CFG_GM_OUTPUT] = 1,
	[SC26RE_CFG_GM_ACT_BUTTON] = 2,
	[SC26RE_CFG_GM_ACT_MODE] = 3,
	[SC26RE_CFG_GM_AXIS_PITCH] = 4,
	[SC26RE_CFG_GM_AXIS_YAW] = 5,
	[SC26RE_CFG_GM_AXIS_ROLL] = 6,
	[SC26RE_CFG_GM_SENS] = 7,
	[SC26RE_CFG_GM_SCALE_P] = 8,
	[SC26RE_CFG_GM_SCALE_Y] = 9,
	[SC26RE_CFG_GM_SCALE_R] = 10,
	[SC26RE_CFG_GM_CURVE] = 11,
	[SC26RE_CFG_GM_ACCEL] = 12,
	[SC26RE_CFG_GM_ACCEL_LO] = 13,
	[SC26RE_CFG_GM_ACCEL_HI] = 14,
	[SC26RE_CFG_GJ_DZ] = 15,
	[SC26RE_CFG_GJ_ANTI] = 16,
	[SC26RE_CFG_GJ_MAX] = 17,
	[SC26RE_CFG_GJ_CURVE] = 18,
	[SC26RE_CFG_GJ_TILT] = 19,
	[SC26RE_CFG_GM_HAPTIC] = 20,
	[SC26RE_CFG_GM_QT1_BTN] = 21,
	[SC26RE_CFG_GM_QT1_DEG] = 22,
	[SC26RE_CFG_GM_QT2_BTN] = 23,
	[SC26RE_CFG_GM_QT2_DEG] = 24,
	[SC26RE_CFG_GM_QT3_BTN] = 25,
	[SC26RE_CFG_GM_QT3_DEG] = 26,
	[SC26RE_CFG_GM_QT4_BTN] = 27,
	[SC26RE_CFG_GM_QT4_DEG] = 28,
	[SC26RE_CFG_GM_QT_MS] = 29,
	[SC26RE_CFG_GM_RATCHET] = 30,
};
BUILD_ASSERT(SC26RE_CFG_COUNT < 256, "gyro keys are bytes");

/* output monitor */
static uint32_t mon_pad;
static int32_t mon_dx, mon_dy, mon_wheel;
static bool profile_dirty[REMAP_PROFILES];
static volatile bool reload_pending; /* a profile was replaced by COMMIT: start clean */
static volatile uint8_t shutdown_state; /* 1: power-off asked, 2: everything let go */

void remap_power_off(void)
{
	/* from the power-off thread: the main loop lets go of every key, button
	 * and toggle while the radio still works (a toggled auto-run key would
	 * otherwise stay held at the PC behind the Puck) */
	shutdown_state = 1U;
}

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
	uint8_t pulse_more;    /* taps that came while one was still held: played after it */
	int64_t release_until; /* release press: group 2 tapped until */
	uint8_t cycle;         /* cycle: next set */
	bool block;            /* held when the profile changed: ignored until released */
	bool interrupted;      /* long press cancelled by another input (per binding option) */
	bool phys;             /* physically pressed (before delay start / end) */
	bool phys_seen;        /* pressed at least once (delays have a reference) */
	bool turbo_on;         /* turbo: its press is out in this report */
	bool turbo_again;      /* turbo: a press went out in this hold already (the next ones are repeats) */
	bool pulse_rel;        /* a tap source with "release press": its release set is tapped next */
	int32_t t_pdown, t_pup; /* ms, low 32 bits of the uptime */
};
static bool block_pressed; /* this report: block every source that is down */
static uint32_t switch_hide;  /* buttons held through a profile switch: hidden from the pad until let go */
static struct src_state st[REMAP_SRC_COUNT];

static uint8_t last_kbd[8];
static bool kbd_valid;
static uint8_t mouse_buttons;
static bool mouse_refresh;    /* send the mouse buttons again even without a change */
static int64_t out_change_ms = -100000; /* last keyboard / mouse button change sent */
static uint8_t mouse_pend;   /* mouse buttons pressed while the link refused reports */
static uint8_t mouse_sent_buttons; /* buttons in the last mouse report that went out */
static uint8_t mouse_owed;         /* button releases held back for the radio */
static uint8_t kbd_pend[8];  /* keys pressed while the link refused reports (report layout) */
static uint32_t xbox_buttons;
static uint8_t xbox_trig[2];   /* Xbox LT / RT from XTRIG actions, 0..255 */
static float xbox_stick[2][2]; /* Xbox sticks from "Xbox stick" modes, -1..1 (summed) */
#ifdef SC26RE_SIM
bool sim_xbox_live(void);
#define xbox_live() sim_xbox_live()
#else
#define xbox_live() xinput_live()
#endif
static int16_t xbox_stick_out[2][2];
static uint8_t move_dirs; /* mouse move actions held: up, down, left, right */
/* radial menu slices, clockwise from the top */
static const uint8_t radial_src[4][10] = {
	{ 38, 47, 41, 49, 39, 48, 40, 46, 64, 54 },
	{ 42, 51, 45, 53, 43, 52, 44, 50, 65, 56 },
	/* sticks (2 = left, 3 = right): up, up-right, right, down-right, down, down-left, left, up-left */
	{ 30, 70, 33, 71, 31, 72, 32, 73, 30, 30 },
	{ 34, 74, 37, 75, 35, 76, 36, 77, 34, 34 },
};
/* a stick radial with 4 slices uses up, right, down, left */
static const uint8_t radial_stick4[4] = { 0, 2, 4, 6 };
static int8_t radial_of_src[REMAP_SRC_COUNT]; /* radial (0,1 pads, 2,3 sticks) of a slice source, -1 none */
static int8_t radial_sel[4] = { -1, -1, -1, -1 };
static int8_t radial_fire[4] = { -1, -1, -1, -1 };
static bool radial_mode[4], radial_click[4], radial_touch[4];
static uint8_t radial_n[4];                  /* slices of each radial menu this pass (for the on-screen menu) */
static int8_t grid_now[2] = { -1, -1 };      /* 3 x 3 grid zone under the finger, -1 none */
static uint8_t gyro_dirs;  /* gyro tilt this report: bit 0 up, 1 down, 2 left, 3 right */
static float tilt_ref[3];  /* gravity when the tilt was centred */
static bool tilt_ref_ok;
/* mouse regions: 0..3 sticks / pads, 4 gyro; position -1..1 (y up) */
static bool region_on[5], region_was[5];
static float region_pos[5][2];
static float gyro_reg[2];
static uint8_t rel_phase;   /* relative mouse region: 0 not homed, 1 on the way to the corner, 2 there */
static int64_t rel_home_ms;
static float rel_tgt[2];    /* the last position asked for, 0..1 of the screen */
static float rel_cur[2];    /* ... and is now here (pixels) */
/* mouse counts that must all arrive (mouse region, flick stick turns): fed to the mouse a report's
 * worth at a time, so the backlog cap a slow link puts on ordinary movement never cuts them */
static float precise_pending[2];
static uint16_t abs_last[2] = { 0xFFFF, 0xFFFF };
static int8_t layer_now = -1, layer_next = -1; /* action layer: profile laid over the current one */
static int8_t layer_holder = -1, layer_holder_next = -1; /* the source holding the layer */
static int8_t out_src = -1;                     /* source whose actions out_action() is handling */
static uint16_t stick_out;                      /* analog output actions held this report */
struct macro_run
{
	bool on;
	uint8_t step;
	int64_t t;
};
static struct macro_run macros[4];
static const struct remap_profile *cur_profile;
static float mouse_acc[4]; /* x, y, wheel, hwheel */
static int16_t pad_prev[2][2];
static bool pad_prev_valid[2];

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

void remap_xbox_stick(int i, int16_t *x, int16_t *y)
{
	bool on = remap_enabled() && i >= 0 && i < 2;

	*x = on ? xbox_stick_out[i][0] : 0;
	*y = on ? xbox_stick_out[i][1] : 0;
}

uint8_t remap_xbox_trigger(int t)
{
	return remap_enabled() && t >= 0 && t < 2 ? xbox_trig[t] : 0U;
}

uint8_t remap_mouse_buttons(void)
{
	/* the buttons last SENT: other mouse reports (gyro mouse) must not press or
	 * let go of anything before the remapper's own report does */
	return mouse_sent_buttons;
}

#if defined(CONFIG_IBEX_HWTEST) && !defined(SC26RE_SIM)
/* where a remapper pass spends its time (sc26re/perf, test builds) */
#include <cmsis_core.h>
static uint32_t perf_t0, perf_sec[10], perf_look, perf_nbound;
extern uint32_t hwtest_dwt(void);
#define PERF_MARK(i)                                                                                         \
	do                                                                                                       \
	{                                                                                                        \
		uint32_t perf_now = hwtest_dwt();                                                                    \
		perf_sec[(i) - 1] += perf_now - perf_t0;                                                             \
		perf_t0 = perf_now;                                                                                  \
	} while(0)
#define PERF_LOOK0() uint32_t perf_l0 = hwtest_dwt()
#define PERF_LOOK1(isbound) do { perf_look += hwtest_dwt() - perf_l0; perf_nbound += (isbound) ? 1U : 0U; } while(0)
void remap_perf_get(uint32_t out[12])
{
	uint32_t mhz = SystemCoreClock / 1000000U;

	for(int i = 0; i < 10; ++i)
	{
		out[i] = perf_sec[i] / mhz;
		perf_sec[i] = 0;
	}
	out[10] = perf_look / mhz;
	out[11] = perf_nbound;
	perf_look = perf_nbound = 0;
}
#else
#define PERF_MARK(i) ((void)0)
#define PERF_LOOK0() ((void)0)
#define PERF_LOOK1(isbound) ((void)0)
#endif

static bool bound(const struct remap_binding *b)
{
	/* the 12 action bytes as three words: this runs for every source on every pass */
	uint32_t w[3];

	memcpy(w, b->act, sizeof(w));
	return (w[0] | w[1] | w[2]) != 0U;
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
static bool soft_past[2];
static int16_t trig_ext[2]; /* hair trigger: lowest / highest point since the last change */
static uint8_t hip_state[2];
static int64_t hip_t0[2];
static float wheel_prev[2], wheel_acc[2];
static bool wheel_valid[2];
static float pad_hap_dist[2];
static float stick_hap_dist[2]; /* stick mouse: counts moved since the last tick */
static float scroll_notch[2];
static uint32_t dp4_keep;
static bool ring_on[4];
/* the trackpad's click selects a zone / direction / slice of its mode ("works while: clicked"):
 * the pad's own Click binding does not act then (it fired too, e.g. a number and a mouse move) */
static bool click_owned[2];
static bool center_on[2];
static bool pad_act[2]; /* trackpad active (touched / clicked / pressed) this report */
static bool swipe_done[2];
static int64_t swipe_t[2];   /* last swipe fired */
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
	if(s < REMAP_BIND_COUNT)
	{
		return s - REMAP_SRC_LPAD_CENTER;
	}
	if(s < REMAP_SRC_LS_DIAG)
	{
		return 2; /* gyro: both sides */
	}
	return (s - REMAP_SRC_LS_DIAG) / 4;
}

static uint8_t fb_count; /* haptic feedbacks played (hardware test) */

uint8_t remap_haptic_count(void)
{
	return fb_count;
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
	fb_count++;
	(void)haptics_play_feedback(level, ch[side]);
}

/* a binding's haptic (v51.4 test 8): trigger sources (soft pull, full pull) play where trig_haptic
 * says, gyro tilt sources where gyro_haptic_at says (grips / trackpads / both), the rest on the
 * trackpads */
static void feedback_src(uint8_t level, int s)
{
	static int64_t last[3];
	int64_t now = k_uptime_get();
	int side = src_side(s), place = -1;

	if(s == REMAP_SRC_LT_SOFT || s == REMAP_SRC_RT_SOFT || s == CONTROLLER_BUTTON_LEFT_TRIGGER_CLICK ||
	   s == CONTROLLER_BUTTON_RIGHT_TRIGGER_CLICK)
	{
		place = sc26re_cfg_get(SC26RE_CFG_TRIG_HAPTIC);
		side = s == REMAP_SRC_LT_SOFT || s == CONTROLLER_BUTTON_LEFT_TRIGGER_CLICK ? 0 : 1;
	}
	else if(s >= REMAP_SRC_GYRO_UP && s < REMAP_SRC_LS_DIAG)
	{
		place = sc26re_cfg_get(SC26RE_CFG_GYRO_HAPTIC_AT);
	}
	if(place < 0)
	{
		feedback(level, side);
		return;
	}
	if(level == 0U || side < 0 || side > 2 || now - last[side] < 20)
	{
		return;
	}
	last[side] = now;
	fb_count++;
	{
		static const uint8_t ch[3] = { HAPTICS_CHANNEL_LEFT_0, HAPTICS_CHANNEL_RIGHT_0, HAPTICS_CHANNELS_PRIMARY };

		(void)haptics_play_feedback_at(level, (int16_t)place, ch[side]);
	}
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
	if(s >= REMAP_SRC_LS_DIAG)
	{
		return false; /* stick diagonals: radial menu slices, pulses */
	}
	if(s >= REMAP_SRC_GYRO_UP)
	{
		return (gyro_dirs & BIT(s - REMAP_SRC_GYRO_UP)) != 0U;
	}
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
		/* a key held by two inputs takes one slot */
		if(*nk < 6U && memchr(keys, a, *nk) == NULL)
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
	else if(a >= REMAP_ACT_LAYER && a < REMAP_ACT_LAYER + REMAP_PROFILES)
	{
		layer_next = (int8_t)(a - REMAP_ACT_LAYER);
		layer_holder_next = out_src;
	}
	else if(a >= REMAP_ACT_STICK && a <= REMAP_ACT_TRIG_R)
	{
		stick_out |= (uint16_t)BIT(a - REMAP_ACT_STICK);
	}
	else if(a == REMAP_ACT_XTRIG_L || a == REMAP_ACT_XTRIG_R)
	{
		/* on the soft pull the analog value is set with the trigger (whole travel) */
		if(out_src != REMAP_SRC_LT_SOFT && out_src != REMAP_SRC_RT_SOFT)
		{
			xbox_trig[a - REMAP_ACT_XTRIG_L] = 255U;
		}
	}
}

/* edge actions: run once when the group becomes active */
static void profile_save(struct k_work *w)
{
	ARG_UNUSED(w);
	(void)sc26re_cfg_set(SC26RE_CFG_REMAP_PROFILE, sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE), true);
}

K_WORK_DEFINE(profile_save_work, profile_save);

static void edge_action(uint8_t a)
{
	switch(a)
	{
		case REMAP_ACT_WHEEL_UP: mouse_acc[2] += 1.0f; break;
		case REMAP_ACT_WHEEL_DOWN: mouse_acc[2] -= 1.0f; break;
		case REMAP_ACT_WHEEL_LEFT: mouse_acc[3] -= 1.0f; break;
		case REMAP_ACT_WHEEL_RIGHT: mouse_acc[3] += 1.0f; break;
		default:
			if(a >= REMAP_ACT_MACRO && a < REMAP_ACT_MACRO + 4U)
			{
				struct macro_run *m = &macros[a - REMAP_ACT_MACRO];

				m->on = true;
				m->step = 0;
				m->t = k_uptime_get();
			}
			else if(a >= REMAP_ACT_PROFILE && a < REMAP_ACT_PROFILE + REMAP_PROFILES)
			{
				/* switched at once; stored by the system work queue: the flash write
				 * stalled the report loop ~7 ms on every in-game switch (hardware test) */
				(void)sc26re_cfg_set(SC26RE_CFG_REMAP_PROFILE, (int16_t)(a - REMAP_ACT_PROFILE), false);
				(void)k_work_submit(&profile_save_work);
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
	feedback_src(REMAP_HAPTIC_LEVEL(b->haptic), s);
}

static void activate(const struct remap_binding *b, int g, int8_t *slot, int s)
{
	if(*slot != g)
	{
		*slot = (int8_t)g;
		fire(b, g, s);
	}
}

#define REPEAT_GAP_MS 10 /* key up between queued taps */
#define REPEAT_TAP_MS 20

static void pulse(const struct remap_binding *b, struct src_state *ss, int s, int64_t now)
{
	if(now < ss->pulse_until + REPEAT_GAP_MS && ss->pulse_group == 0)
	{
		/* a tap while the last one is still down (fast wheel / swipes / radial):
		 * one-shot actions and the haptic now, the key press queued so the PC
		 * sees separate presses instead of one long one */
		fire(b, 0, s);
		if(ss->pulse_more < 8U)
		{
			ss->pulse_more++;
		}
		return;
	}
	ss->pulse_group = 0;
	ss->pulse_until = now + TAP_MS;
	fire(b, 0, s);
}

/* a tap source (radial slice, wheel step, swipe) is pressed and let go at once: with "release
 * press" its release set follows the press set, or comes alone when there is none (up to v51
 * test 48 it never came) */
static void tap(const struct remap_binding *b, struct src_state *ss, int s, int64_t now)
{
	bool any = false;

	if(!(b->mode & REMAP_MODE_RELEASE))
	{
		pulse(b, ss, s, now);
		return;
	}
	for(size_t i = 0; i < 4; ++i)
	{
		any = any || b->act[0][i] != 0U;
	}
	if(any)
	{
		pulse(b, ss, s, now);
		ss->pulse_rel = true;
		return;
	}
	if(now < ss->pulse_until + REPEAT_GAP_MS)
	{
		return; /* the last one is still going out */
	}
	ss->pulse_group = 2;
	ss->pulse_until = now + TAP_MS;
	fire(b, 2, s);
}

/* sources that only fire taps (radial slices, wheel, swipes): nothing may stay
 * held from the mode the source had before (layer / profile / shift change) */
static void drop_hold(struct src_state *s)
{
	s->down = false;
	s->toggled = false;
	s->waiting_double = false;
	s->held_group = -1;
	s->release_until = 0;
}

static const struct remap_btime zero_bt;

/* the per binding timing of a binding (in whichever profile it lives) */
static const struct remap_btime *btime_lookup(const struct remap_binding *b);

static const struct remap_btime *btime_of(const struct remap_binding *b)
{
	/* the bindings loop asks for the same binding several times in a row; the arrays
	 * never move, so the last answer stays right */
	static const struct remap_binding *last_b;
	static const struct remap_btime *last_bt;

	if(b != last_b)
	{
		last_bt = btime_lookup(b);
		last_b = b;
	}
	return last_bt;
}

static const struct remap_btime *btime_lookup(const struct remap_binding *b)
{
	for(int k = 0; k < REMAP_PROFILES; ++k)
	{
		const struct remap_profile *q = &profiles[k];

		if(b >= q->bind && b < q->bind + REMAP_BIND_COUNT)
		{
			return &q->ext.bt[b - q->bind];
		}
		if(b >= q->sbind && b < q->sbind + REMAP_SHIFT_SRC_COUNT)
		{
			return &q->ext.bt[REMAP_BIND_COUNT + (b - q->sbind)];
		}
		if(b >= q->ext.xbind && b < q->ext.xbind + REMAP_XBIND_COUNT)
		{
			return &q->ext.bt[REMAP_BIND_COUNT + REMAP_SHIFT_SRC_COUNT + (b - q->ext.xbind)];
		}
		if(b >= q->v8.sclick && b < q->v8.sclick + REMAP_SCLICK_COUNT)
		{
			return &q->v8.sclick_bt[b - q->v8.sclick];
		}
	}
	return &zero_bt;
}

static void run_source(const struct remap_binding *b, struct src_state *s, bool down, int64_t now, int src,
                       bool release_ok, bool other_pressed)
{
	bool hold = (b->mode & REMAP_MODE_HOLD) != 0U;
	bool rel = (b->mode & REMAP_MODE_RELEASE) != 0U;
	const struct remap_btime *bt = btime_of(b);
	const int64_t hold_ms = bt->hold10 ? (int64_t)bt->hold10 * 10 : (int64_t)prof_hold_ms;
	const int64_t dbl_ms = bt->dbl10 ? (int64_t)bt->dbl10 * 10 : (int64_t)prof_double_ms;
	bool interrupt = (bt->flags & REMAP_BT_INTERRUPT) != 0U && hold;

	if(down && !s->down)
	{
		s->interrupted = false;
	}
	else if(interrupt && down && s->down && other_pressed && s->held_group < 0)
	{
		s->interrupted = true; /* another input while waiting for the long press */
	}

	if(down && !s->down)
	{
		s->t_down = now;
	}
	/* cycle: every press taps the next set that has actions */
	if(b->mode & REMAP_MODE_CYCLE)
	{
		if(down && !s->down)
		{
			for(int k = 0; k < 3; ++k)
			{
				int g = (s->cycle + k) % 3;
				bool any = false;

				for(int i = 0; i < 4; ++i)
				{
					any = any || b->act[g][i] != 0U;
				}
				if(any)
				{
					s->pulse_group = (int8_t)g;
					s->pulse_until = now + TAP_MS;
					fire(b, g, src);
					s->cycle = (uint8_t)((g + 1) % 3);
					break;
				}
			}
		}
		s->held_group = -1;
		s->down = down;
		return;
	}
	bool dbl = (b->mode & REMAP_MODE_DOUBLE) != 0U && !rel; /* release uses the double slot */
	/* "keep": the press group acts at once and while held (output loop); the long /
	 * double press come on top of it and nothing is tapped afterwards */
	bool keep = (bt->flags & REMAP_BT_KEEP) != 0U && (hold || dbl) && !(b->mode & (REMAP_MODE_START | REMAP_MODE_TOGGLE));

	if(keep && down && !s->down)
	{
		fire(b, 0, src);
	}

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
		else if(hold && s->held_group < 0 && !s->interrupted && now - s->t_down >= hold_ms)
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
				feedback_src(REMAP_HAPTIC_LEVEL(b->haptic), src);
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
		if(dbl && s->waiting_double && now - s->t_up <= dbl_ms)
		{
			s->waiting_double = false;
			activate(b, 2, &s->held_group, src); /* second press: group 2 while held */
		}
		else if(s->waiting_double)
		{
			/* pressed again just after the window: the first press still
			 * counts as its own single press */
			s->waiting_double = false;
			if(!keep)
			{
				pulse(b, s, src, now);
			}
		}
	}
	else if(!down && s->down)
	{
		bool was_hold = s->held_group == 1;
		bool was_double = s->held_group == 2;

		s->held_group = -1;
		s->t_up = now;
		if(s->interrupted)
		{
			s->interrupted = false; /* cancelled: neither the long nor the short press */
		}
		else if(!was_hold && !was_double)
		{
			if(dbl)
			{
				s->waiting_double = true; /* decide after dbl_ms */
			}
			else if(!keep)
			{
				pulse(b, s, src, now);
			}
		}
	}
	else if(down && hold && s->held_group < 0 && !s->interrupted && now - s->t_down >= hold_ms)
	{
		activate(b, 1, &s->held_group, src);
	}
	if(s->waiting_double && !down && now - s->t_up > dbl_ms)
	{
		s->waiting_double = false;
		if(!keep)
		{
			pulse(b, s, src, now);
		}
	}
	s->down = down;
}

static float ax_scale(uint16_t v)
{
	return v ? (float)v * 0.01f : 1.0f;
}

static void stick_analog(int i, const struct remap_analog *an, const struct remap_anx *ax, int16_t x, int16_t y,
                         float dt)
{
	float sx = ax_scale(ax->scale_x), sy = ax_scale(ax->scale_y);

	float fx = (float)x / 32767.0f;
	float fy = (float)y / 32767.0f;
	float m = sqrtf(fx * fx + fy * fy);
	float dz = (float)MAX(an->dz_in, 300) * 0.0001f;
	float mm;

	if(m < dz || m <= 0.0f)
	{
		stick_hap_dist[i] = 0.0f; /* the movement ticks count from each new push */
		return;
	}
	mm = MIN((m - dz) / (1.0f - dz), 1.0f);
	fx /= m;
	fy /= m;
	if(an->mode == REMAP_ANALOG_MOUSE)
	{
		/* speed: counts per second at full deflection, squared response */
		const uint16_t *crv = remap_curve_stick(i);
		float k = (float)an->speed * dt *
		          (crv != NULL ? CLAMP(curve_eval(crv, mm), 0.0f, 1.0f)
		                       : powf(mm, an->curve ? (float)an->curve * 0.01f : 2.0f));

		mouse_acc[0] += fx * k * sx;
		mouse_acc[1] -= fy * k * sy;
		/* "ticks while moving" (the page offers it for sticks too): one per 100 counts.
		 * Before v51 test 55 only the trackpad mouse ticked */
		stick_hap_dist[i] += k * sqrtf(fx * fx * sx * sx + fy * fy * sy * sy);
		if(stick_hap_dist[i] >= 100.0f)
		{
			stick_hap_dist[i] = 0.0f;
			feedback(an->haptic, i);
		}
	}
	else if(an->mode == REMAP_ANALOG_SCROLL)
	{
		/* speed: notches per second x100 at full deflection */
		float k = (float)an->speed * 0.01f * dt * mm;
		float before = scroll_notch[i];

		mouse_acc[2] += fy * k * sy;
		mouse_acc[3] += fx * k * sx;
		scroll_notch[i] += k;
		if(floorf(scroll_notch[i]) != floorf(before))
		{
			feedback(an->haptic, i);
			/* keep it small: a float that only grows loses the fractions
			 * after hours of scrolling */
			scroll_notch[i] -= floorf(scroll_notch[i]);
		}
	}
}

/* sticks to Steam: inner / outer dead zones rescaled to the full range */
static void stick_dead_zones(const struct remap_analog *an, const uint16_t *crv, int16_t *x, int16_t *y)
{
	float fx = (float)*x / 32767.0f, fy = (float)*y / 32767.0f;
	float m = sqrtf(fx * fx + fy * fy);
	float in = (float)an->dz_in * 0.0001f, out = 1.0f - (float)an->dz_out * 0.0001f;
	float k;

	if(crv == NULL && an->dz_in == 0U && an->dz_out == 0U && an->anti_dz == 0U &&
	   (an->curve == 0U || an->curve == 100U))
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
	if(crv != NULL)
	{
		/* the stick's custom curve (v51.7) replaces the exponent */
		k = CLAMP(curve_eval(crv, k), 0.0f, 1.0f);
	}
	else if(an->curve != 0U && an->curve != 100U)
	{
		k = powf(k, (float)an->curve * 0.01f);
	}
	k = ((float)an->anti_dz * 0.0001f + (1.0f - (float)an->anti_dz * 0.0001f) * k) / m;
	*x = (int16_t)CLAMP(lroundf(fx * k * 32767.0f), -32767, 32767);
	*y = (int16_t)CLAMP(lroundf(fy * k * 32767.0f), -32767, 32767);
}

static float tb_vel[2][2]; /* trackball: counts per second */
static float tb_win[2][3]; /* trackball speed window: counts x, y, seconds */

static void pad_mouse(int side, const struct remap_analog *an, const struct remap_anx *ax, bool active, int16_t x,
                      int16_t y, float dt)
{
	float sx = ax_scale(ax->scale_x), sy = ax_scale(ax->scale_y);

	if(active && ax->edge_spin && dt > 0.0f)
	{
		/* edge spin: a finger resting near the edge keeps the cursor moving outwards */
		float fx = (float)x / 32767.0f, fy = (float)y / 32767.0f, m = sqrtf(fx * fx + fy * fy);

		if(m >= 0.85f)
		{
			float k = (float)ax->edge_spin * dt * MIN((m - 0.85f) / 0.1f, 1.0f) / m;

			mouse_acc[0] += fx * k * sx;
			mouse_acc[1] -= fy * k * sy;
		}
	}
	if(!active)
	{
		pad_prev_valid[side] = false;
		if((an->flags & REMAP_AF_TRACKBALL) && dt > 0.0f)
		{
			/* trackball: keeps rolling, slowed by the friction (per second) */
			float fr = an->dz_out ? (float)an->dz_out * 0.01f : 2.0f;
			float keep = MAX(0.0f, 1.0f - fr * dt);

			mouse_acc[0] += tb_vel[side][0] * dt;
			mouse_acc[1] += tb_vel[side][1] * dt;
			tb_vel[side][0] *= keep;
			tb_vel[side][1] *= keep;
			if(fabsf(tb_vel[side][0]) + fabsf(tb_vel[side][1]) < 5.0f)
			{
				tb_vel[side][0] = tb_vel[side][1] = 0.0f;
			}
		}
		return;
	}
	if(pad_prev_valid[side])
	{
		/* speed: mouse counts for a swipe across the whole pad; curve = acceleration % */
		float k = (float)an->speed / 65535.0f;
		float dx = (float)(x - pad_prev[side][0]);
		float dy = (float)(y - pad_prev[side][1]);

		if(an->curve && dt > 0.0f)
		{
			float v = sqrtf(dx * dx + dy * dy) / dt; /* pad units per second */

			k *= 1.0f + (float)an->curve * 0.01f * MIN(v / 100000.0f, 1.0f);
		}
		mouse_acc[0] += dx * k * sx;
		mouse_acc[1] -= dy * k * sy;
		/* the speed the trackball keeps: measured over at least 20 ms. Per pass it depended on
		 * how often the report loop ran between two trackpad scans (it also wakes on other
		 * input changes): the controller rolled twice as far as the simulator (v51 test 56) */
		tb_win[side][0] += dx * k * sx;
		tb_win[side][1] += -dy * k * sy;
		tb_win[side][2] += dt;
		if(tb_win[side][2] >= 0.020f)
		{
			tb_vel[side][0] = 0.5f * tb_vel[side][0] + 0.5f * (tb_win[side][0] / tb_win[side][2]);
			tb_vel[side][1] = 0.5f * tb_vel[side][1] + 0.5f * (tb_win[side][1] / tb_win[side][2]);
			tb_win[side][0] = tb_win[side][1] = tb_win[side][2] = 0.0f;
		}
		pad_hap_dist[side] += sqrtf(dx * dx + dy * dy);
		if(pad_hap_dist[side] >= 3000.0f)
		{
			pad_hap_dist[side] = 0.0f;
			feedback(an->haptic, side);
		}
	}
	else
	{
		tb_vel[side][0] = tb_vel[side][1] = 0.0f;
		tb_win[side][0] = tb_win[side][1] = tb_win[side][2] = 0.0f;
	}
	pad_prev[side][0] = x;
	pad_prev[side][1] = y;
	pad_prev_valid[side] = true;
}

/* Steam's mouse joystick: the finger's speed is the stick deflection, so a trackpad turns the
 * camera of a game that only reads a stick the way it turns a mouse camera. Full deflection at
 * 60000 pad units/s at 100 %; a trackball keeps it rolling after the finger leaves, slowed by
 * the friction; the minimum output (.anti_dz) gets past the game's own dead zone */
static float mj_vel[2][2];  /* deflection, -1..1 */
static float mj_win[2][3];  /* pad units x, y, seconds since the last speed sample */
static int16_t mj_prev[2][2];
static bool mj_prev_valid[2];

static void pad_mousejoy(int side, const struct remap_analog *an, const struct remap_anx *ax, bool active, int16_t x,
                         int16_t y, float dt, float out[2])
{
	float sens = (an->speed ? (float)an->speed : 100.0f) * 0.01f / 60000.0f;
	float sx = ax_scale(ax->scale_x), sy = ax_scale(ax->scale_y);

	if(!active)
	{
		mj_prev_valid[side] = false;
		mj_win[side][0] = mj_win[side][1] = mj_win[side][2] = 0.0f;
		if((an->flags & REMAP_AF_TRACKBALL) && dt > 0.0f)
		{
			float fr = an->dz_out ? (float)an->dz_out * 0.01f : 2.0f;
			float keep = MAX(0.0f, 1.0f - fr * dt);

			mj_vel[side][0] *= keep;
			mj_vel[side][1] *= keep;
			if(fabsf(mj_vel[side][0]) + fabsf(mj_vel[side][1]) < 0.02f)
			{
				mj_vel[side][0] = mj_vel[side][1] = 0.0f;
			}
		}
		else
		{
			mj_vel[side][0] = mj_vel[side][1] = 0.0f;
		}
	}
	else
	{
		if(mj_prev_valid[side])
		{
			mj_win[side][0] += (float)(x - mj_prev[side][0]);
			mj_win[side][1] += (float)(y - mj_prev[side][1]);
			mj_win[side][2] += dt;
			/* the speed over at least 10 ms (trackpad scans come every ~4 ms, the loop runs faster) */
			if(mj_win[side][2] >= 0.010f)
			{
				float vx = mj_win[side][0] / mj_win[side][2] * sens * sx;
				float vy = mj_win[side][1] / mj_win[side][2] * sens * sy;

				mj_vel[side][0] = 0.3f * mj_vel[side][0] + 0.7f * CLAMP(vx, -1.0f, 1.0f);
				mj_vel[side][1] = 0.3f * mj_vel[side][1] + 0.7f * CLAMP(vy, -1.0f, 1.0f);
				mj_win[side][0] = mj_win[side][1] = mj_win[side][2] = 0.0f;
			}
		}
		else
		{
			mj_vel[side][0] = mj_vel[side][1] = 0.0f;
		}
		mj_prev[side][0] = x;
		mj_prev[side][1] = y;
		mj_prev_valid[side] = true;
	}

	float vx = mj_vel[side][0], vy = mj_vel[side][1], m = sqrtf(vx * vx + vy * vy);

	if(m > 1.0f)
	{
		vx /= m;
		vy /= m;
		m = 1.0f;
	}
	if(m < 0.02f)
	{
		/* stopped (or a slow drift of a resting finger): centred, no minimum output */
		vx = vy = 0.0f;
	}
	else if(an->anti_dz)
	{
		float a = (float)an->anti_dz * 0.0001f, k = (a + (1.0f - a) * m) / m;

		vx *= k;
		vy *= k;
	}
	out[0] = vx;
	out[1] = vy;
}

/*
 * Mouse reports come from two threads: this remapper (main loop) and the gyro
 * mouse (IMU thread), whose reports carry the remapper's buttons. Each reads the
 * buttons and queues its report under this lock, so a gyro report can never
 * carry a button state older than a report already queued (a click let go and
 * then pressed again at the PC).
 */
K_MUTEX_DEFINE(mouse_out_lock);

void remap_mouse_out_lock(void)
{
	(void)k_mutex_lock(&mouse_out_lock, K_FOREVER);
}

void remap_mouse_out_unlock(void)
{
	(void)k_mutex_unlock(&mouse_out_lock);
}

static void send_mouse_locked(void);

static void send_mouse(void)
{
	remap_mouse_out_lock();
	send_mouse_locked();
	remap_mouse_out_unlock();
}

static void send_mouse_locked(void)
{
	uint8_t m[5];
	static uint8_t last_buttons;
	int8_t v[4];
	bool any = false;

	/* at most ~a quarter second of movement waits (USB: 127 counts per ms);
	 * more means something produced far too much, never move for seconds */
	mouse_acc[0] = CLAMP(mouse_acc[0], -32767.0f, 32767.0f);
	mouse_acc[1] = CLAMP(mouse_acc[1], -32767.0f, 32767.0f);
	for(size_t i = 0; i < 4; ++i)
	{
		float t = CLAMP(mouse_acc[i] >= 0.0f ? floorf(mouse_acc[i]) : ceilf(mouse_acc[i]), -127.0f, 127.0f);

		v[i] = (int8_t)t;
		mouse_acc[i] -= t;
		any = any || v[i] != 0;
	}
	/* a click the link refused is still sent (pressed once) when it takes reports again;
	 * a release over the radio waits like the keyboard's */
	uint8_t mb = mouse_buttons | mouse_pend;

	if(mb & mouse_owed)
	{
		/* pressed again while its release waits: the release goes first */
		uint8_t rel[5] = { (uint8_t)(last_buttons & ~mouse_owed), 0, 0, 0, 0 };

		if(transport_send_input_report(0x40, rel, sizeof(rel)) == 0)
		{
			last_buttons = rel[0];
			mouse_sent_buttons = rel[0];
		}
		mouse_owed = 0U;
	}
	if((last_buttons & ~mb) && !release_ok_now())
	{
		mouse_owed |= last_buttons & ~mb;
		mb |= last_buttons;
	}
	else
	{
		mouse_owed &= mb;
	}
	if(!any && mb == last_buttons && !mouse_refresh)
	{
		return;
	}
	mouse_refresh = false;
	m[0] = mb;
	m[1] = (uint8_t)v[0];
	m[2] = (uint8_t)v[1];
	m[3] = (uint8_t)v[2];
	m[4] = (uint8_t)v[3];
	if(transport_send_input_report(0x40, m, sizeof(m)) == 0)
	{
		if(last_buttons != m[0])
		{
			out_change_ms = k_uptime_get();
		}
		last_buttons = m[0];
		mouse_sent_buttons = m[0];
		mouse_pend = 0U;
		mon_dx += v[0];
		mon_dy += v[1];
		mon_wheel += v[2];
		/* USB adds every mouse report to the one still waiting and sends 127 counts per
		 * frame on its own: hand it the rest of a big move now (a flick stick turn,
		 * a mouse region jump), not 127 per remapper pass. The loop makes about 600 to
		 * 1000 passes a second, so a 180 degree flick took 2 or 3 times longer than
		 * the reports allow (random hardware test, v51 test 65) */
		for(int extra = 0; extra < 7 && transport_link_kind() == 0U &&
		                   (fabsf(mouse_acc[0]) >= 127.0f || fabsf(mouse_acc[1]) >= 127.0f);
		    ++extra)
		{
			int8_t w[2];

			for(size_t i = 0; i < 2; ++i)
			{
				float t = CLAMP(mouse_acc[i] >= 0.0f ? floorf(mouse_acc[i]) : ceilf(mouse_acc[i]), -127.0f, 127.0f);

				w[i] = (int8_t)t;
			}
			m[1] = (uint8_t)w[0];
			m[2] = (uint8_t)w[1];
			m[3] = 0U;
			m[4] = 0U;
			if(transport_send_input_report(0x40, m, sizeof(m)) != 0)
			{
				break;
			}
			mouse_acc[0] -= (float)w[0];
			mouse_acc[1] -= (float)w[1];
			mon_dx += w[0];
			mon_dy += w[1];
		}
	}
	else
	{
		for(size_t i = 0; i < 4; ++i)
		{
			mouse_acc[i] += (float)v[i];
		}
		mouse_pend |= mouse_buttons & ~last_buttons;
		/* a slow link (Bluetooth) must not build a backlog that keeps the
		 * pointer moving long after the input stopped: keep about 16 reports */
		mouse_acc[0] = CLAMP(mouse_acc[0], -2032.0f, 2032.0f);
		mouse_acc[1] = CLAMP(mouse_acc[1], -2032.0f, 2032.0f);
	}
}

static bool kbd_flush; /* the "all keys up" report still has to go out */

/* diagnostics (sc26re/rmp_diag): keyboard reports handed to the transport */
static uint16_t diag_kbd_ok, diag_kbd_fail;
static int8_t diag_kbd_err;

static int send_kbd(const uint8_t *r)
{
	int e = transport_send_input_report(0x41, r, 8);

	if(e == 0)
	{
		diag_kbd_ok++;
	}
	else
	{
		diag_kbd_fail++;
		diag_kbd_err = (int8_t)CLAMP(e, -128, 127);
	}
	return e;
}

/* sc26re/osd, the on-screen menu (overlay page): REMAP_MENU_SIZE bytes
 * [0] profile, [1] action layer (-1 none), [2] shift groups held (bit per group), [3] menus that follow (0..2)
 * per menu, 24 bytes: [0] input (0 left pad, 1 right pad, 2 left stick, 3 right stick) | kind << 4
 * (1 radial, 2 grid 3 x 3), [1] slices (grid: 9, row by row from the top left), [2] chosen slice (-1 none),
 * [3] 0, [4..13] each slice's source, [14..23] its first action (as bound now: shift, layer) */
static const uint8_t grid_src[2][9] = {
	{ 46, 38, 47, 40, 64, 41, 48, 39, 49 },
	{ 50, 42, 51, 44, 65, 45, 52, 43, 53 },
};

static const struct remap_binding *binding(const struct remap_profile *p, int s);

void remap_menu_state(uint8_t *out)
{
	const struct remap_profile *p = cur_profile;
	int m = 0;

	memset(out, 0, REMAP_MENU_SIZE);
	out[0] = (uint8_t)CLAMP(sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE), 0, REMAP_PROFILES - 1);
	out[1] = (uint8_t)layer_now;
	for(int g = 0; g < REMAP_SHIFT_GROUPS && g < 8; ++g)
	{
		out[2] |= shifted[g] ? (uint8_t)BIT(g) : 0U;
	}
	if(p == NULL || !remap_enabled())
	{
		return;
	}
	for(int k = 0; k < 4 && m < 2; ++k)
	{
		bool radial = radial_mode[k] && (radial_touch[k] || radial_sel[k] >= 0);
		bool grid = k < 2 && grid_now[k] >= 0;
		uint8_t *o = &out[4 + 24 * m];
		int n;

		if(!radial && !grid)
		{
			continue;
		}
		n = radial ? radial_n[k] : 9;
		o[0] = (uint8_t)(k | (radial ? 0x10 : 0x20));
		o[1] = (uint8_t)n;
		o[2] = (uint8_t)(radial ? radial_sel[k] : grid_now[k]);
		for(int i = 0; i < n && i < 10; ++i)
		{
			int s = !radial ? grid_src[k][i] : radial_src[k][k >= 2 && n == 4 ? radial_stick4[i] : i];
			const struct remap_binding *b = binding(p, s);
			const uint8_t *a = &b->act[0][0];

			o[4 + i] = (uint8_t)s;
			for(int q = 0; q < 12; ++q)
			{
				if(a[q] != 0U)
				{
					o[14 + i] = a[q];
					break;
				}
			}
		}
		++m;
	}
	out[3] = (uint8_t)m;
}

void remap_diag(uint8_t *out)
{
	sys_put_le16(diag_kbd_ok, &out[0]);
	sys_put_le16(diag_kbd_fail, &out[2]);
	out[4] = (uint8_t)diag_kbd_err;
	out[5] = transport_link_kind();
	out[6] = kbd_valid ? 1U : 0U;
	out[7] = 0;
#ifndef SC26RE_SIM
	xinput_diag(&out[8]);
#else
	memset(&out[8], 0, 8);
#endif
}

static uint8_t kbd_owed[8]; /* releases held back for the radio (report layout) */

/* is a key / modifier whose release is held back wanted again? */
static bool repressed(const uint8_t *kbd, const uint8_t *owed)
{
	if(kbd[0] & owed[0])
	{
		return true;
	}
	for(int i = 2; i < 8 && owed[i]; ++i)
	{
		if(memchr(&kbd[2], owed[i], 6) != NULL)
		{
			return true;
		}
	}
	return false;
}

/* remove the keys / modifiers of `keys` from report `r` (keeps the slots packed) */
static void strip_keys(uint8_t *r, const uint8_t *keys)
{
	uint8_t out[6] = { 0 };
	int n = 0;

	r[0] &= (uint8_t)~keys[0];
	for(int i = 2; i < 8; ++i)
	{
		if(r[i] && memchr(&keys[2], r[i], 6) == NULL)
		{
			out[n++] = r[i];
		}
	}
	memcpy(&r[2], out, 6);
}

/* owed releases still owed after report `sent`: only those still held in it */
static void keep_owed(uint8_t *owed, const uint8_t *sent)
{
	uint8_t keep[8] = { 0 };
	int n = 0;

	keep[0] = owed[0] & sent[0];
	for(int i = 2; i < 8 && owed[i]; ++i)
	{
		if(memchr(&sent[2], owed[i], 6) != NULL)
		{
			keep[2 + n++] = owed[i];
		}
	}
	memcpy(owed, keep, 8);
}

/* turbo over a slow link: the next press waits until the last release went
 * out, else it would arrive together with it (a press the game never sees) */
static bool turbo_waits(const struct remap_binding *b)
{
	for(int i = 0; i < 4; ++i)
	{
		uint8_t a = b->act[0][i];

		if(a >= 0xE0U && a <= 0xE7U && (kbd_owed[0] & BIT(a - 0xE0U)))
		{
			return true;
		}
		if(a >= 0x04U && a <= 0x65U && memchr(&kbd_owed[2], a, 6) != NULL)
		{
			return true;
		}
		if(a == REMAP_ACT_MOUSE_L && (mouse_owed & 1U))
		{
			return true;
		}
		if(a == REMAP_ACT_MOUSE_R && (mouse_owed & 2U))
		{
			return true;
		}
		if(a >= REMAP_ACT_MOUSE_M && a <= REMAP_ACT_MOUSE_5 && (mouse_owed & BIT(2 + a - REMAP_ACT_MOUSE_M)))
		{
			return true;
		}
	}
	return false;
}

/* does going from `last` to `kbd` let go of a key or modifier? */
static bool releases_key(const uint8_t *kbd, const uint8_t *last)
{
	if(last[0] & ~kbd[0])
	{
		return true;
	}
	for(int i = 2; i < 8 && last[i]; ++i)
	{
		if(memchr(&kbd[2], last[i], 6) == NULL)
		{
			return true;
		}
	}
	return false;
}

/*
 * Keyboard and mouse buttons are states: a report taken by the radio can still
 * be lost (the Puck link drops its queue when it reconnects), and the PC would
 * keep a key down. For 5 s after a change the current state is sent again every
 * 250 ms (a few reports, harmless when they arrive twice).
 */
static void refresh_outputs(void)
{
	static const uint8_t zero[8];
	static int64_t t_ref;
	int64_t now = k_uptime_get();

	if(now - out_change_ms > 5000 || now - out_change_ms < 250 || now - t_ref < 250)
	{
		return;
	}
	t_ref = now;
	(void)send_kbd(kbd_valid ? last_kbd : zero);
	mouse_refresh = true;
}

/* with the remapper off or an empty profile: make sure the last "released"
 * reports reach the PC even when the radio refused them the first time */
static void flush_outputs(void)
{
	static const uint8_t zero[8];

	if(kbd_flush && send_kbd(zero) == 0)
	{
		kbd_flush = false;
		out_change_ms = k_uptime_get();
	}
	refresh_outputs();
	send_mouse(); /* sends only a change still pending (buttons up) */
}

static void release_all(void)
{
	static const uint8_t zero[8];

	mouse_pend = 0U;
	mouse_owed = 0U;
	memset(kbd_owed, 0, sizeof(kbd_owed));
	mon_pad = 0U;            /* the output monitor shows nothing held either */
	memset(shifted, 0, sizeof(shifted));
	memset(st, 0, sizeof(st));
	for(size_t i = 0; i < REMAP_SRC_COUNT; ++i)
	{
		st[i].held_group = -1;
	}
	memset(mouse_acc, 0, sizeof(mouse_acc));
	memset(precise_pending, 0, sizeof(precise_pending));
	rel_phase = 0U;
	memset(region_on, 0, sizeof(region_on));
	memset(region_was, 0, sizeof(region_was));
	abs_last[0] = abs_last[1] = 0xFFFFU;
	memset(wheel_valid, 0, sizeof(wheel_valid));
	memset(pad_prev_valid, 0, sizeof(pad_prev_valid));
	memset(swipe_valid, 0, sizeof(swipe_valid));
	memset(flick_on, 0, sizeof(flick_on));
	memset(pad_joy, 0, sizeof(pad_joy));
	flick_left = 0.0f;
	/* everything else that can hold an output */
	xbox_buttons = 0U;
	memset(xbox_trig, 0, sizeof(xbox_trig));
	memset(xbox_stick, 0, sizeof(xbox_stick));
	memset(xbox_stick_out, 0, sizeof(xbox_stick_out));
	move_dirs = 0U;
	stick_out = 0U;
	memset(macros, 0, sizeof(macros));
	memset(tb_vel, 0, sizeof(tb_vel));
	layer_now = layer_next = -1;
	if(mouse_buttons != 0U)
	{
		mouse_buttons = 0U;
		send_mouse();
	}
	if(kbd_valid && memcmp(last_kbd, zero, sizeof(zero)) != 0)
	{
		if(send_kbd(zero) != 0)
		{
			kbd_flush = true; /* the radio queue was full: retried by flush_outputs() */
		}
		out_change_ms = k_uptime_get();
	}
	memset(last_kbd, 0, sizeof(last_kbd));
	memset(kbd_pend, 0, sizeof(kbd_pend));
	kbd_valid = false;
}

void remap_set_enabled(bool on)
{
	/* remap_boot 1: power-on always starts off, so the switch is not saved */
	(void)sc26re_cfg_set(SC26RE_CFG_REMAP_ON, on ? 1 : 0, sc26re_cfg_get(SC26RE_CFG_REMAP_BOOT) != 1);
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
		/* a new flick adds to one still turning, never more than a full turn */
		flick_left = CLAMP(flick_left + c360 * a / 360.0f, -c360, c360);
		flick_rate = fabsf(flick_left) * 1000.0f / (float)MAX(ms, 1U);
	}
	else
	{
		precise_pending[0] += c360 * wrap180(a - flick_last[i]) / 360.0f;
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
	precise_pending[0] += step;
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
	if((int64_t)dx * dx + (int64_t)dy * dy < (int64_t)th * th)
	{
		return;
	}
	/* repeats: the previous tap must be over (key up) before the next one,
	 * else a fast long swipe merges into one long press */
	if(k_uptime_get() - swipe_t[j] < 2 * TAP_MS)
	{
		return;
	}
	swipe_t[j] = k_uptime_get();
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

	if(!remap_enabled() || p->magic != REMAP_MAGIC || p->version != REMAP_VERSION)
	{
		return false;
	}
	/* the gyro space is its own profile setting (0 = global), with or without the profile's gyro settings */
	if(id == SC26RE_CFG_GM_SPACE)
	{
		if(p->ext.gyx.space >= 1U && p->ext.gyx.space <= 3U)
		{
			*value = (int16_t)(p->ext.gyx.space - 1U);
			return true;
		}
		return false;
	}
	if(!(p->flags & REMAP_PF_GYRO))
	{
		return false;
	}
	if(id == SC26RE_CFG_GYRO_FSR)
	{
		int16_t f = p->gyro_fsr;

		if(f == 250 || f == 500 || f == 1000 || f == 2000)
		{
			*value = f;
			return true;
		}
		return false;
	}
	if(id >= 0 && id < SC26RE_CFG_COUNT && gyro_key_slot[id] != 0U)
	{
		int k = gyro_key_slot[id] - 1;

		{
			int16_t v;

			memcpy(&v, (const uint8_t *)p->gyro + k * sizeof(int16_t), sizeof(v));
			/* the range a page write would have: a profile made elsewhere (or damaged) can hold
			 * anything, and e.g. a tilt limit below 0 held the gyro joystick in a corner */
			*value = sc26re_cfg_clamp((enum sc26re_cfg_id)id, v);
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

bool remap_gyro_act(bool *held)
{
	const struct remap_profile *p = active();
	const struct remap_gyx *gx;

	if(!remap_enabled() || p->magic != REMAP_MAGIC || p->version != REMAP_VERSION)
	{
		return false;
	}
	gx = &p->ext.gyx;
	if(gx->act_mask == 0U && (gx->act_flags & (REMAP_GA_LS | REMAP_GA_RS)) == 0U)
	{
		return false;
	}

	struct controller_report rs;
	k_spinlock_key_t key = k_spin_lock(&raw_lock);

	rs = raw_state;
	k_spin_unlock(&raw_lock, key);

	uint32_t want = gx->act_mask;
	uint32_t have = rs.buttons & 0x3FFFFFFFU;
	int n_want = 0, n_have = 0;

	if(soft_past[0])
	{
		have |= BIT(30);
	}
	if(soft_past[1])
	{
		have |= BIT(31);
	}
	n_want = __builtin_popcount(want);
	n_have = __builtin_popcount(want & have);
	/* stick deflection: out past 30 % of its travel */
	for(int i = 0; i < 2; ++i)
	{
		if(gx->act_flags & (i ? REMAP_GA_RS : REMAP_GA_LS))
		{
			float x = (float)(i ? rs.stick_right_x : rs.stick_left_x), y = (float)(i ? rs.stick_right_y : rs.stick_left_y);

			n_want++;
			if(x * x + y * y > (0.30f * 32767.0f) * (0.30f * 32767.0f))
			{
				n_have++;
			}
		}
	}
	*held = (gx->act_flags & REMAP_GA_ALL) ? n_have == n_want : n_have > 0;
	return true;
}

uint32_t remap_raw_buttons(void)
{
	k_spinlock_key_t key = k_spin_lock(&raw_lock);
	uint32_t b = raw_state.buttons;

	k_spin_unlock(&raw_lock, key);
	return b;
}

void remap_raw_read(uint8_t *out)
{
	struct controller_report r;
	k_spinlock_key_t key = k_spin_lock(&raw_lock);
	int16_t quat[4];
	int16_t acc[3];
	int16_t v[12];

	r = raw_state;
	k_spin_unlock(&raw_lock, key);
	imu_raw_read(quat, acc);
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
	memset(out, 0, REMAP_RAW_SIZE);
	sys_put_le32(r.buttons, &out[0]);
	for(int i = 0; i < 12; ++i)
	{
		sys_put_le16((uint16_t)v[i], &out[4 + 2 * i]);
	}
	for(int i = 0; i < 3; ++i)
	{
		sys_put_le16((uint16_t)acc[i], &out[34 + 2 * i]);
	}
	for(int i = 0; i < 4; ++i)
	{
		sys_put_le16((uint16_t)quat[i], &out[40 + 2 * i]);
	}
	sys_put_le32((uint32_t)k_ticks_to_us_floor64(k_uptime_ticks()), &out[52]);
}

void remap_monitor(uint8_t *out)
{
	const struct remap_profile *p = active();
	uint8_t sh = 0;

	for(int g = 0; g < REMAP_SHIFT_GROUPS && remap_enabled(); ++g)
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
		/* on the Puck the charge light stays: the colour under it changes (v51.4 test 23) */
		if(!led_saved)
		{
			saved = charge_mode_led_base_get();
			led_saved = true;
		}
		charge_mode_led_base_set((struct rgbw_color){ p->led[0], p->led[1], p->led[2], 0 });
	}
	else if(led_saved)
	{
		charge_mode_led_base_set(saved);
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

/* the LED was taken over (the Puck's charge light): show the active profile's colour again */
void remap_led_refresh(void)
{
	const struct remap_profile *p = active();

	if(pc_last >= 0 && remap_enabled() && p->magic == REMAP_MAGIC && p->version == REMAP_VERSION &&
	   (p->led[0] | p->led[1] | p->led[2]))
	{
		rgbw_led_set((struct rgbw_color){ p->led[0], p->led[1], p->led[2], 0 });
	}
}

static const struct remap_profile *layer_profile(void)
{
	const struct remap_profile *l;

	if(layer_now < 0)
	{
		return NULL;
	}
	l = &profiles[layer_now];
	return (l->magic == REMAP_MAGIC && l->version == REMAP_VERSION && l != cur_profile) ? l : NULL;
}

static bool has_layer_action(const struct remap_binding *b)
{
	for(int g = 0; g < 3; ++g)
	{
		for(int i = 0; i < 4; ++i)
		{
			if(b->act[g][i] >= REMAP_ACT_LAYER && b->act[g][i] < REMAP_ACT_LAYER + REMAP_PROFILES)
			{
				return true;
			}
		}
	}
	return false;
}

/* the stick (0, 1) or trackpad (2, 3) a derived source comes from, -1 none */
static int analog_of_src(int s)
{
	if(s >= REMAP_SRC_LS_UP && s < REMAP_SRC_LPAD_UP)
	{
		return (s - REMAP_SRC_LS_UP) / 4;
	}
	if(s >= REMAP_SRC_LPAD_UP && s < REMAP_SRC_LPAD_GATE)
	{
		return 2 + (s - REMAP_SRC_LPAD_UP) / 4;
	}
	if(s >= REMAP_SRC_LPAD_GATE && s < REMAP_SRC_LPAD_CW)
	{
		return 2 + (s - REMAP_SRC_LPAD_GATE) / 4;
	}
	if(s >= REMAP_SRC_LPAD_CW && s < REMAP_SRC_LT_SOFT)
	{
		return 2 + (s - REMAP_SRC_LPAD_CW) / 2;
	}
	if(s >= REMAP_SRC_LS_RING && s < REMAP_SRC_LPAD_CENTER)
	{
		return s - REMAP_SRC_LS_RING;
	}
	if(s >= REMAP_SRC_LPAD_CENTER && s < REMAP_BIND_COUNT)
	{
		return 2 + s - REMAP_SRC_LPAD_CENTER;
	}
	if(s >= REMAP_SRC_LS_DIAG && s < REMAP_SRC_COUNT)
	{
		return (s - REMAP_SRC_LS_DIAG) / 4;
	}
	return -1;
}

static const struct remap_analog *analog_cfg(const struct remap_profile *p, int i)
{
	int g = REMAP_SHIFT_LS + i;
	const struct remap_profile *l = layer_profile();

	/* action layer: its sticks / pads replace the base ones unless left "to Steam".
	 * Not the stick / pad whose input holds the layer: the layer would change
	 * what holds it (a ring that is gone in the layer) and switch itself off
	 * and on every report. */
	if(l != NULL && l->an[i].mode != REMAP_ANALOG_PASS && analog_of_src(layer_holder) != i)
	{
		return &l->an[i];
	}
	return shifted[g] ? &p->shift[g].an : &p->an[i];
}

static const struct remap_binding *bind_at(const struct remap_profile *p, int s)
{
	return s < REMAP_BIND_COUNT ? &p->bind[s] : &p->ext.xbind[s - REMAP_BIND_COUNT];
}

static const struct remap_binding *binding_l(const struct remap_profile *p, int s, const struct remap_profile *l)
{
	int g = src_group[s];
	const struct remap_binding *b = !(g >= 0 && shifted[g]) ? bind_at(p, s)
	                                : src_sidx[s] >= REMAP_SHIFT_SRC_COUNT ? &p->v8.sclick[src_sidx[s] - REMAP_SHIFT_SRC_COUNT]
	                                                                       : &p->sbind[src_sidx[s]];

	/* action layer: its bound inputs win; the input holding the layer keeps its own.
	 * Layer actions inside a layer are ignored (no nesting): two layers calling
	 * each other would swap on every report. */
	if(l != NULL && bound(bind_at(l, s)) && !has_layer_action(b) && !has_layer_action(bind_at(l, s)))
	{
		return bind_at(l, s);
	}
	return b;
}

static const struct remap_binding *binding(const struct remap_profile *p, int s)
{
	return binding_l(p, s, layer_profile());
}

/* the v7 extras of the stick / pad settings analog_cfg() picks */
static const struct remap_anx *analog_x(const struct remap_profile *p, int i)
{
	int g = REMAP_SHIFT_LS + i;
	const struct remap_profile *l = layer_profile();

	if(l != NULL && l->an[i].mode != REMAP_ANALOG_PASS && analog_of_src(layer_holder) != i)
	{
		return &l->ext.anx[i];
	}
	return shifted[g] ? &p->ext.anx[4 + g] : &p->ext.anx[i];
}

/* stick / pad orientation: invert and rotate the input */
static void orient(const struct remap_analog *an, int16_t *x, int16_t *y)
{
	float fx = (float)*x, fy = (float)*y;

	if(an->flags & REMAP_AF_INVERT_X)
	{
		fx = -fx;
	}
	if(an->flags & REMAP_AF_INVERT_Y)
	{
		fy = -fy;
	}
	if(an->rotate != 0)
	{
		float a = (float)an->rotate * 0.017453f, c = cosf(a), sn = sinf(a);
		float rx = fx * c - fy * sn, ry = fx * sn + fy * c;

		fx = rx;
		fy = ry;
	}
	if(an->flags & (REMAP_AF_INVERT_X | REMAP_AF_INVERT_Y) || an->rotate != 0)
	{
		*x = (int16_t)CLAMP(lroundf(fx), -32767, 32767);
		*y = (int16_t)CLAMP(lroundf(fy), -32767, 32767);
	}
}

static bool ring_test(const struct remap_analog *an, float fx, float fy)
{
	float r = (float)an->ring * 0.0001f;

	float m2 = fx * fx + fy * fy;

	if(an->ring == 0U)
	{
		return false;
	}
	if(an->flags & REMAP_AF_RING_INVERT)
	{
		/* inner ring (e.g. walk): only while pushed past the dead zone, not at rest */
		float dz = (float)MAX(an->dz_in, 500U) * 0.0001f;

		return m2 < r * r && m2 >= dz * dz;
	}
	return m2 >= r * r;
}

/*
 * Mouse regions: an absolute cursor position. Over the USB cable it is the
 * absolute pointer report (0x46); over the Puck and Bluetooth (no such report)
 * the cursor is first pushed into the top-left corner and then moved there with
 * relative counts, which needs the screen size in pixels and no pointer
 * acceleration ("Enhance pointer precision" off).
 */
/* the last absolute cursor position sent over USB, 0..32767 (0xFFFF: none; hardware test) */
void remap_abs_pos(uint16_t *x, uint16_t *y)
{
	*x = abs_last[0];
	*y = abs_last[1];
}

static void region_output(const struct remap_profile *p)
{
	int k = -1;

	/* trackpads first, then sticks, then the gyro */
	for(int c = 0; c < 5 && k < 0; ++c)
	{
		static const uint8_t order[5] = { 2, 3, 0, 1, 4 };

		if(region_on[order[c]])
		{
			k = order[c];
		}
	}
	float tx, ty;

	if(k < 0)
	{
		abs_last[0] = abs_last[1] = 0xFFFFU;
		/* over the radio a region let go while the cursor is still on its way to the
		 * corner still gets its last position (a stick flicked and let go: the centre) */
		if(rel_phase != 1U || transport_link_kind() == 0U)
		{
			rel_phase = 0; /* the real mouse may move the cursor meanwhile */
			return;
		}
		tx = rel_tgt[0];
		ty = rel_tgt[1];
	}
	else
	{
		const uint8_t *rg = k == 4 ? p->ext.gyx.region : analog_x(p, k)->region;
		float cx = rg[0] || rg[1] || rg[2] || rg[3] ? (float)rg[0] * 0.01f : 0.5f;
		float cy = rg[0] || rg[1] || rg[2] || rg[3] ? (float)rg[1] * 0.01f : 0.5f;
		float w = rg[2] ? (float)rg[2] * 0.01f : 1.0f, h = rg[3] ? (float)rg[3] * 0.01f : 1.0f;

		tx = CLAMP(cx + region_pos[k][0] * w * 0.5f, 0.0f, 1.0f);
		ty = CLAMP(cy - region_pos[k][1] * h * 0.5f, 0.0f, 1.0f);
		rel_tgt[0] = tx;
		rel_tgt[1] = ty;
	}

	if(transport_link_kind() == 0U)
	{
		uint16_t ax = (uint16_t)lroundf(tx * 32767.0f), ay = (uint16_t)lroundf(ty * 32767.0f);

		if(ax != abs_last[0] || ay != abs_last[1])
		{
			uint8_t rep[5] = { 0, (uint8_t)ax, (uint8_t)(ax >> 8), (uint8_t)ay, (uint8_t)(ay >> 8) };

			if(transport_send_input_report(0x46, rep, sizeof(rep)) == 0)
			{
				abs_last[0] = ax;
				abs_last[1] = ay;
			}
		}
		return;
	}
	{
		/* up to 16384 px as on the page: a bigger size (a hand-made profile) only makes the
		 * way into the corner take seconds */
		float sw = (float)(p->ext.screen[0] ? MIN(p->ext.screen[0], 16384U) : 1920U);
		float sh = (float)(p->ext.screen[1] ? MIN(p->ext.screen[1], 16384U) : 1080U);

		int64_t now = k_uptime_get();

		if(rel_phase == 0U)
		{
			precise_pending[0] = -sw * 1.25f; /* into the corner, wherever the cursor is */
			precise_pending[1] = -sh * 1.25f;
			rel_cur[0] = rel_cur[1] = 0.0f;
			rel_phase = 1U;
			rel_home_ms = now;
		}
		if(rel_phase == 1U)
		{
			/* the way out of the corner starts only when the way in has left the
			 * radio queue: the PC stops the cursor at the screen edge, so a move
			 * out merged into a move in (the Puck merges queued mouse reports)
			 * would be lost against that edge */
			if(fabsf(precise_pending[0]) >= 1.0f || fabsf(precise_pending[1]) >= 1.0f || fabsf(mouse_acc[0]) >= 1.0f ||
			   fabsf(mouse_acc[1]) >= 1.0f)
			{
				rel_home_ms = now; /* still on the way */
				return;
			}
			if(transport_input_queued() != 0 && now - rel_home_ms < 500)
			{
				return;
			}
			rel_phase = 2U;
		}
		precise_pending[0] += tx * sw - rel_cur[0];
		precise_pending[1] += ty * sh - rel_cur[1];
		rel_cur[0] = tx * sw;
		rel_cur[1] = ty * sh;
		if(k < 0)
		{
			rel_phase = 0; /* the last position is out: done */
		}
	}
}

#if defined(CONFIG_IBEX_HWTEST) || defined(SC26RE_SIM)
static volatile bool hwtest_ended; /* remap_hwtest_ended(): let go of everything, report loop side */
#endif

/* past the soft pull point (the trigger section's own test, hysteresis from the last report) */
static bool soft_past_now(const struct remap_profile *p, const struct controller_report *r, int t)
{
	int16_t v = t ? r->trigger_right : r->trigger_left;
	int32_t th = (int32_t)CLAMP(p->trig_soft[t] ? p->trig_soft[t] : 5000, 100, 10000) * 32767 / 10000;

	return soft_past[t] ? v >= th - MIN(1000, th / 2) : v >= th;
}

void remap_process(struct controller_report *r, float dt)
{
	const struct remap_profile *p;
	uint32_t raw = r->buttons;
	static uint32_t raw_prev;
	uint32_t newp = raw & ~raw_prev; /* inputs pressed this report (interruptible long press) */

	raw_prev = raw;
#if defined(CONFIG_IBEX_HWTEST) && !defined(SC26RE_SIM)
	perf_t0 = k_cycle_get_32();
#endif
#if defined(CONFIG_IBEX_HWTEST) || defined(SC26RE_SIM)
	if(hwtest_ended)
	{
		hwtest_ended = false;
		release_all();
	}
#endif
	uint8_t keys[6] = { 0 };
	size_t nk = 0;
	uint8_t mods = 0;
	uint32_t pad = 0;
	uint32_t hide = 0;
	uint32_t bound_mask = 0;
	int64_t now = k_uptime_get();

	/* the gyro mouse's calibration button while calibrating: its, not the bindings' (it leaves the
	 * pad report later, in gyro_mouse_filter_buttons) */
	{
		static uint32_t gm_hide;

		gm_hide = (gm_hide & raw) | gyro_mouse_owned(raw);
		raw &= ~gm_hide;
	}
	/*
	 * The controller's own combinations, all with the Steam button held (v51 test 86; before, the
	 * remapper and the gyro mouse were LB + RB + Menu / View and LB, RB went to the game before
	 * the combination was complete):
	 *   Steam + D-pad up / right / down / left: profile 1 / 2 / 3 / 4
	 *   Steam + Menu: remapper on / off        Steam + View: gyro mouse on / off
	 *   Steam + A: 360 calibration of the gyro mouse
	 *   Steam + X (held 3 s): gyro calibration, Steam + Y (held): power off (main.c)
	 * They work whether the remapper is on or off. While Steam is held these buttons reach
	 * neither the bindings nor the host, and one pressed with Steam stays hidden until it is
	 * released, also when Steam is let go first.
	 */
	{
		static uint32_t sc_prev, sc_hide;
		static const uint8_t dp_bit[REMAP_PROFILES] = { CONTROLLER_BUTTON_DPAD_UP, CONTROLLER_BUTTON_DPAD_RIGHT,
		                                                CONTROLLER_BUTTON_DPAD_DOWN, CONTROLLER_BUTTON_DPAD_LEFT };
		const uint32_t combo = DP_MASK | BIT(CONTROLLER_BUTTON_MENU) | BIT(CONTROLLER_BUTTON_VIEW) |
		                       BIT(CONTROLLER_BUTTON_A) | BIT(CONTROLLER_BUTTON_X) | BIT(CONTROLLER_BUTTON_Y);
		uint32_t sc = (raw & BIT(CONTROLLER_BUTTON_STEAM)) ? (raw & combo) : 0U;
		uint32_t pressed = sc & ~sc_prev;

		for(int i = 0; i < REMAP_PROFILES; ++i)
		{
			if(pressed & BIT(dp_bit[i]))
			{
				if(i != sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE))
				{
					release_all();
				}
				(void)sc26re_cfg_set(SC26RE_CFG_REMAP_PROFILE, (int16_t)i, false);
				(void)k_work_submit(&profile_save_work); /* stored off the report loop */
				(void)haptics_play_beep((uint16_t)(700 + 150 * i), 80);
			}
		}
		if(pressed & BIT(CONTROLLER_BUTTON_MENU))
		{
			remap_set_enabled(!remap_enabled());
		}
		if(pressed & BIT(CONTROLLER_BUTTON_VIEW))
		{
			gyro_mouse_set_enabled(sc26re_cfg_get(SC26RE_CFG_GM_ENABLED) != 1);
		}
		if(pressed & BIT(CONTROLLER_BUTTON_A))
		{
			gyro_mouse_cal_start();
		}
		sc_prev = sc;
		sc_hide = (sc_hide | sc) & raw;
		r->buttons &= ~sc_hide;
		raw &= ~sc_hide;
	}
	/*
	 * The Steam button: while the remapper is on, Steam never sees it (bind it
	 * to "Pad Steam" to give it back). The bindings, Steam + D-pad and the
	 * power-off hold read the physical button. A press that started hidden
	 * stays hidden until it is let go, even if the remapper turns off meanwhile.
	 */
	{
		static bool steam_hidden;

		if(!(raw & BIT(CONTROLLER_BUTTON_STEAM)))
		{
			steam_hidden = false;
		}
		else if(remap_enabled())
		{
			steam_hidden = true;
		}
		if(steam_hidden)
		{
			r->buttons &= ~BIT(CONTROLLER_BUTTON_STEAM);
		}
	}
	profile_changed();
	if(shutdown_state != 0U)
	{
		/* powering off: let go of everything once, then nothing new */
		if(shutdown_state == 1U)
		{
			release_all();
			shutdown_state = 2U;
		}
		acc_process(-1, NULL, 0.0f);
		flush_outputs();
		return;
	}
	{
		/* turned off by any path (LB+RB+Menu, the page's switch = a plain
		 * setting write): let go of everything once */
		static bool was_on;

		if(!remap_enabled())
		{
			if(was_on)
			{
				release_all();
				was_on = false;
			}
			acc_process(-1, NULL, 0.0f);
			flush_outputs();
			return;
		}
		if(!was_on)
		{
			acc_ref_ok = false; /* accelerometer tilt: centred where the controller is now */
			/* just turned on: lizard mode (or anything else) may have left keys
			 * or a click held at the PC: send our full state once */
			kbd_valid = false;
			mouse_refresh = true;
		}
		was_on = true;
	}
	p = active();
	if(p->magic != REMAP_MAGIC || p->version != REMAP_VERSION)
	{
		/* empty profile: nothing mapped, and nothing left held from before */
		release_all(); /* sends nothing when nothing is held */
		acc_process(-1, NULL, 0.0f);
		flush_outputs();
		return;
	}
	{
		/* another profile (switch action, Steam + D-pad) or this one saved
		 * again from the page: start from nothing held */
		static const struct remap_profile *last_active;

		if(p != last_active || reload_pending)
		{
			reload_pending = false;
			if(last_active != NULL)
			{
				release_all();
				block_pressed = true;
				/* the buttons held through the switch (the switch button itself) stay away
				 * from the Steam pad until let go: unbound in the new profile they reached
				 * the game (hardware test, v51 test 57) */
				switch_hide = raw & (BIT(30) - 1U);
			}
			tilt_ref_ok = false; /* gyro tilt: centred where the controller is now */
			acc_ref_ok = false;  /* accelerometer tilt too */
			gyro_reg[0] = gyro_reg[1] = 0.0f;
			last_active = p;
		}
	}
	cur_profile = p;
	switch_hide &= raw;
	hide |= switch_hide;
	prof_hold_ms = p->hold_ms ? p->hold_ms : 250U;
	prof_double_ms = p->double_ms ? p->double_ms : 250U;
	prof_turbo_ms = MAX(p->turbo_ms ? p->turbo_ms : 100U, 20U);
	layer_now = layer_next; /* the layer held in the last report */
	layer_next = -1;
	layer_holder = layer_holder_next;
	layer_holder_next = -1;
	stick_out = 0U;

	PERF_MARK(1);
	/* mode shifts: while a group's shift button is held it uses its
	 * alternative settings and bindings; its sources restart cleanly */
	for(int g = 0; g < REMAP_SHIFT_GROUPS; ++g)
	{
		uint8_t sb = p->shift[g].button;
		/* 1..30: a button (bit + 1); 31 / 32: the left / right trigger's soft pull (Steam's
		 * "left_trigger_threshold" switch: a mode shift held by a half pulled trigger) - v51 test 61.
		 * The soft pull of this report (before v51 test 78 the last one: a pass late, 4-10 ms
		 * on the controller with the gyro busy) */
		bool on = (sb >= 1U && sb <= 30U && (raw & BIT(sb - 1U)) != 0U) ||
		          (sb == REMAP_SHIFT_BY_SOFT_LT && soft_past_now(p, r, 0)) ||
		          (sb == REMAP_SHIFT_BY_SOFT_RT && soft_past_now(p, r, 1));

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
			for(int k = 0; k < REMAP_SCLICK_COUNT; ++k)
			{
				if(sclick_group[k] == g)
				{
					memset(&st[sclick_src[k]], 0, sizeof(st[0]));
					st[sclick_src[k]].held_group = -1;
				}
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
	/* invert / rotate sticks and pads as set */
	{
		int16_t ax, ay;
		static const uint8_t which[4] = { 0, 1, 2, 3 };

		for(int k = 0; k < 4; ++k)
		{
			const struct remap_analog *oa = analog_cfg(p, which[k]);

			ax = k == 0 ? r->stick_left_x : k == 1 ? r->stick_right_x : k == 2 ? r->touchpad_left_x : r->touchpad_right_x;
			ay = k == 0 ? r->stick_left_y : k == 1 ? r->stick_right_y : k == 2 ? r->touchpad_left_y : r->touchpad_right_y;
			orient(oa, &ax, &ay);
			if(k == 0) { r->stick_left_x = ax; r->stick_left_y = ay; }
			else if(k == 1) { r->stick_right_x = ax; r->stick_right_y = ay; }
			else if(k == 2) { r->touchpad_left_x = ax; r->touchpad_left_y = ay; }
			else { r->touchpad_right_x = ax; r->touchpad_right_y = ay; }
		}
	}
	PERF_MARK(2);
	/* sticks */
	for(int i = 0; i < 2; ++i)
	{
		const struct remap_analog *an = analog_cfg(p, i);
		int16_t x = i ? r->stick_right_x : r->stick_left_x;
		int16_t y = i ? r->stick_right_y : r->stick_left_y;

		float fx = (float)x / 32767.0f, fy = (float)y / 32767.0f;
		float dz = (float)MAX(an->dz_in, 500) * 0.0001f;
		bool out = fx * fx + fy * fy >= dz * dz;

		ring_on[i] = ring_test(an, fx, fy);
		dirs[i] = (an->mode == REMAP_ANALOG_DPAD4 || an->mode == REMAP_ANALOG_DPAD8) ? dir_mask(an->mode, fx, fy, dz) : 0U;
		{
			/* "ring first": some games take Shift + W for a sprint only when Shift came first;
			 * the stick reaches the ring after the direction, so the direction lets go for one
			 * pass when the ring comes on and is pressed again with it */
			static bool ring_was[2];

			if((an->flags & REMAP_AF_RING_FIRST) && dirs[i] != 0U && ring_on[i] && !ring_was[i] &&
			   !(an->flags & REMAP_AF_RING_INVERT))
			{
				dirs[i] = 0U;
			}
			ring_was[i] = ring_on[i];
		}
		if(an->mode == REMAP_ANALOG_SINGLE)
		{
			/* the whole stick is one button (its "outer ring" source): pushed past the dead zone */
			ring_on[i] = out;
		}
		/* radial menu on the stick: the slice it points at fires when it comes back to the centre
		 * (or on the stick click) */
		radial_fire[2 + i] = -1;
		radial_mode[2 + i] = an->mode == REMAP_ANALOG_RADIAL;
		if(radial_mode[2 + i])
		{
			const struct remap_anx *ax = analog_x(p, i);
			int n = CLAMP(ax->slices ? ax->slices : 8, 2, 8);
			bool click = (raw & BIT(i ? CONTROLLER_BUTTON_RIGHT_STICK : CONTROLLER_BUTTON_LEFT_STICK)) != 0U;

			radial_n[2 + i] = (uint8_t)n;
			bool fire_now;
			int8_t prev;

			/* a slice chosen in a menu with more slices (a shift or a layer changed it
			 * meanwhile) is not in this one */
			if(radial_sel[2 + i] >= n)
			{
				radial_sel[2 + i] = -1;
			}
			prev = radial_sel[2 + i];

			if(out)
			{
				float a = atan2f(fx, fy) * 57.29578f; /* 0 = up, clockwise */

				if(a < 0.0f)
				{
					a += 360.0f;
				}
				radial_sel[2 + i] = (int8_t)(((int)((a + 180.0f / (float)n) / (360.0f / (float)n))) % n);
			}
			if(radial_sel[2 + i] >= 0 && radial_sel[2 + i] != prev)
			{
				feedback(an->haptic, i);
			}
			fire_now = (an->flags & REMAP_AF_CLICK) ? (click && !radial_click[2 + i]) : (!out && radial_touch[2 + i]);
			if(fire_now && radial_sel[2 + i] >= 0)
			{
				int k = n == 4 ? radial_stick4[radial_sel[2 + i]] : radial_sel[2 + i];

				radial_fire[2 + i] = (int8_t)radial_src[2 + i][k];
			}
			if(!out && !(an->flags & REMAP_AF_CLICK))
			{
				radial_sel[2 + i] = -1;
			}
			if(!out && (an->flags & REMAP_AF_CLICK) && !click)
			{
				radial_sel[2 + i] = -1;
			}
			radial_click[2 + i] = click;
			radial_touch[2 + i] = out;
		}
		/* mouse region: the stick position is the cursor position (back to the centre when let go) */
		region_on[i] = an->mode == REMAP_ANALOG_REGION && (out || region_was[i]);
		if(region_on[i])
		{
			region_pos[i][0] = out ? fx : 0.0f;
			region_pos[i][1] = out ? fy : 0.0f;
		}
		region_was[i] = an->mode == REMAP_ANALOG_REGION && out;
	}
	PERF_MARK(3);
	/* trackpads: active while touched or while clicked */
	memset(pad_joy, 0, sizeof(pad_joy));
	memset(xbox_stick, 0, sizeof(xbox_stick));
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

		click_owned[j] = (an->flags & REMAP_AF_CLICK) && !(an->flags & REMAP_AF_PRESSURE) &&
		                 (an->mode == REMAP_ANALOG_DPAD4 || an->mode == REMAP_ANALOG_DPAD8 ||
		                  an->mode == REMAP_ANALOG_GATES || an->mode == REMAP_ANALOG_GRID ||
		                  an->mode == REMAP_ANALOG_RADIAL || an->mode == REMAP_ANALOG_SINGLE);

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
		grid_now[j] = (int8_t)grid_zone;
		if(an->mode == REMAP_ANALOG_SINGLE)
		{
			center_on[j] = act; /* the whole pad is one button */
		}
		/* radial menu: the slice under the finger is chosen, it fires when the
		 * finger leaves (or on the click); the centre cancels */
		radial_fire[j] = -1;
		radial_mode[j] = an->mode == REMAP_ANALOG_RADIAL;
		if(radial_mode[j])
		{
			bool click = (raw & BIT(j ? CONTROLLER_BUTTON_RIGHT_TOUCHPAD_CLICK : CONTROLLER_BUTTON_LEFT_TOUCHPAD_CLICK)) != 0U;
			int n = CLAMP(an->speed, 2, 10);
			bool fire_now;
			float rz = MAX(dz, 0.2f);

			radial_n[j] = (uint8_t)n;

			if(radial_sel[j] >= n)
			{
				radial_sel[j] = -1; /* chosen in a menu with more slices (a shift or a layer) */
			}

			if(touched)
			{
				if(fx * fx + fy * fy >= rz * rz)
				{
					float a = atan2f(fx, fy) * 57.29578f; /* 0 = up, clockwise */

					if(a < 0.0f)
					{
						a += 360.0f;
					}
					radial_sel[j] = (int8_t)(((int)((a + 180.0f / (float)n) / (360.0f / (float)n))) % n);
				}
				else
				{
					radial_sel[j] = -1;
				}
			}
			fire_now = (an->flags & REMAP_AF_CLICK) ? (click && !radial_click[j]) : (!touched && radial_touch[j]);
			if(fire_now && radial_sel[j] >= 0)
			{
				radial_fire[j] = (int8_t)radial_src[j][radial_sel[j]];
			}
			if(!touched)
			{
				radial_sel[j] = -1;
			}
			radial_click[j] = click;
			radial_touch[j] = touched;
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
			else if(touched && radial_mode[j])
			{
				zone = (int16_t)(radial_sel[j] + 1); /* 0 = centre */
			}
			/* a tick on every change of zone, and on the first touch when it
			 * lands in a zone (gates/D-pad/radial: 0 = no zone; grid: all are) */
			if(zone >= 0 && zone != last_zone[j] &&
			   (last_zone[j] >= 0 || zone > 0 || an->mode == REMAP_ANALOG_GRID))
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
			pad_mouse(j, an, analog_x(p, 2 + j), act, x, y, dt);
		}
		else
		{
			pad_prev_valid[j] = false;
		}
		{
			/* mouse region: the finger is the cursor position inside the area */
			region_on[2 + j] = an->mode == REMAP_ANALOG_REGION &&
			                   (act || (region_was[2 + j] && (analog_x(p, 2 + j)->flags & REMAP_AX_REGION_RETURN)));
			if(region_on[2 + j])
			{
				region_pos[2 + j][0] = act ? fx : 0.0f;
				region_pos[2 + j][1] = act ? fy : 0.0f;
			}
			region_was[2 + j] = an->mode == REMAP_ANALOG_REGION && act;
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
			/* pad_joy is cleared before the pad loop: with "other stick" the
			 * left pad writes the right stick's slot, which the right pad must
			 * not clear again */
			int t = j ^ ((an->flags & REMAP_AF_OTHER_STICK) ? 1 : 0);

			if(an->mode == REMAP_ANALOG_JOYSTICK && act)
			{
				int16_t jx = x, jy = y;

				stick_dead_zones(an, NULL, &jx, &jy);
				pad_joy[t][0] = (float)jx / 32767.0f;
				pad_joy[t][1] = (float)jy / 32767.0f;
			}
			else if(an->mode == REMAP_ANALOG_MOUSEJOY)
			{
				float o[2];

				pad_mousejoy(j, an, analog_x(p, 2 + j), act, x, y, dt, o);
				if(an->flags & REMAP_AF_MJ_XBOX)
				{
					xbox_stick[t][0] += o[0];
					xbox_stick[t][1] += o[1];
				}
				else if(o[0] != 0.0f || o[1] != 0.0f)
				{
					pad_joy[t][0] = o[0];
					pad_joy[t][1] = o[1];
				}
			}
			else if(an->mode == REMAP_ANALOG_XBOX && act)
			{
				int16_t jx = x, jy = y;

				stick_dead_zones(an, NULL, &jx, &jy);
				xbox_stick[t][0] += (float)jx / 32767.0f;
				xbox_stick[t][1] += (float)jy / 32767.0f;
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
	PERF_MARK(4);
	/* gyro (profile gyro settings): tilt directions or a mouse region */
	gyro_dirs = 0U;
	region_on[4] = false;
	{
		const struct remap_gyx *gx = &p->ext.gyx;
		static uint8_t gmode_prev = 0xFFU;
		uint8_t gmode = gx->mode; /* a profile setting of its own (v7), with or without its gyro settings */
		struct controller_report rs;
		k_spinlock_key_t key = k_spin_lock(&raw_lock);

		rs = raw_state;
		k_spin_unlock(&raw_lock, key);
		if(gmode != gmode_prev)
		{
			tilt_ref_ok = false;
			gyro_reg[0] = gyro_reg[1] = 0.0f;
			gmode_prev = gmode;
		}
		if(gmode == REMAP_GYRO_TILT)
		{
			float a[3] = { (float)rs.accel_x, (float)rs.accel_y, (float)rs.accel_z };
			float m = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);

			if(m > 1000.0f)
			{
				for(int k = 0; k < 3; ++k)
				{
					a[k] /= m;
				}
				if(!tilt_ref_ok)
				{
					memcpy(tilt_ref, a, sizeof(tilt_ref));
					tilt_ref_ok = true;
				}
				/* turn of the gravity vector about the controller's X (forward tilt) and Y (sideways) */
				float fwd = atan2f(tilt_ref[1] * a[2] - tilt_ref[2] * a[1], tilt_ref[1] * a[1] + tilt_ref[2] * a[2]) * 57.29578f;
				float side = -atan2f(tilt_ref[2] * a[0] - tilt_ref[0] * a[2], tilt_ref[2] * a[2] + tilt_ref[0] * a[0]) * 57.29578f;
				static uint8_t last;
				float dz = (float)(gx->tilt_dz ? gx->tilt_dz : 15U);
				/* a little hysteresis: a direction already on stays until 3 degrees back, or
				 * half way for a small tilt (3 degrees back from 1..3 stayed on when level) */
				float off = dz - MIN(3.0f, dz * 0.5f);
				uint8_t d = 0U;

				if(fwd > ((last & 1U) ? off : dz)) d |= 1U;
				if(fwd < -((last & 2U) ? off : dz)) d |= 2U;
				if(side < -((last & 4U) ? off : dz)) d |= 4U;
				if(side > ((last & 8U) ? off : dz)) d |= 8U;
				if(!gx->tilt_8way && (d & 3U) && (d & 12U))
				{
					d &= fabsf(fwd) >= fabsf(side) ? 3U : 12U; /* 4-way: the bigger tilt */
				}
				last = d;
				gyro_dirs = d;
			}
		}
		else if(gmode == REMAP_GYRO_REGION)
		{
			/* turning moves an absolute cursor inside the area; it stops at the edges */
			float deg = (float)(gx->region_deg ? gx->region_deg : 60U);
			float w = (float)(gx->region[2] ? gx->region[2] : 100U), h = (float)(gx->region[3] ? gx->region[3] : 100U);
			float sw = (float)(p->ext.screen[0] ? p->ext.screen[0] : 1920U), sh = (float)(p->ext.screen[1] ? p->ext.screen[1] : 1080U);
			float degy = deg * (h * sh) / (w * sw);

			gyro_reg[0] = CLAMP(gyro_reg[0] - (float)rs.gyro_z * 0.06103f * dt * 2.0f / deg, -1.0f, 1.0f);
			gyro_reg[1] = CLAMP(gyro_reg[1] + (float)rs.gyro_x * 0.06103f * dt * 2.0f / MAX(degy, 1.0f), -1.0f, 1.0f);
			region_on[4] = true;
			region_pos[4][0] = gyro_reg[0];
			region_pos[4][1] = gyro_reg[1];
		}
	}
	/* accelerometer (v51.8): tilt -> stick (added to the stick below) and tilt -> the tilt sources */
	{
		struct controller_report rs;
		k_spinlock_key_t key = k_spin_lock(&raw_lock);

		rs = raw_state;
		k_spin_unlock(&raw_lock, key);
		acc_process(active_index(), &rs, dt);
		gyro_dirs |= acc_dirs;
	}
	PERF_MARK(5);
	/* triggers: soft pull point with a little hysteresis */
	uint8_t xtrig_an[2] = { 0U, 0U };

	for(int t = 0; t < 2; ++t)
	{
		int16_t v = t ? r->trigger_right : r->trigger_left;
		bool full = (raw & BIT(t ? CONTROLLER_BUTTON_RIGHT_TRIGGER_CLICK : CONTROLLER_BUTTON_LEFT_TRIGGER_CLICK)) != 0U;
		/* past the soft pull point; it lets go 3 % lower, or half way down for a point below 6 %
		 * (a fixed 3 % kept a soft pull at 1..3 % on forever: it never got back below 0) */
		bool past = soft_past_now(p, r, t);
		uint8_t mode = p->trig_mode[t];

		if(mode == REMAP_TRIG_HAIR)
		{
			/* hair trigger: any pull of 2 % fires, any release of 2 % stops */
			if(v < 330)
			{
				soft_on[t] = false;
				trig_ext[t] = v;
			}
			else if(soft_on[t])
			{
				trig_ext[t] = MAX(trig_ext[t], v);
				if(v < trig_ext[t] - 650)
				{
					soft_on[t] = false;
					trig_ext[t] = v;
				}
			}
			else
			{
				trig_ext[t] = MIN(trig_ext[t], v);
				if(v > trig_ext[t] + 650)
				{
					soft_on[t] = true;
					trig_ext[t] = v;
				}
			}
		}
		else if(mode == REMAP_TRIG_HIP || mode == REMAP_TRIG_HIP_EXCL)
		{
			uint16_t win = p->trig_hip_ms[t] ? p->trig_hip_ms[t] : 75U;

			if(!past)
			{
				hip_state[t] = 0; /* released */
				soft_on[t] = false;
			}
			else
			{
				if(hip_state[t] == 0)
				{
					hip_state[t] = 1; /* deciding */
					hip_t0[t] = now;
				}
				if(hip_state[t] == 1)
				{
					if(full)
					{
						hip_state[t] = 3; /* quick pull: hip fire, full only */
					}
					else if(now - hip_t0[t] >= win)
					{
						hip_state[t] = 2; /* slow pull: soft (aim) */
					}
				}
				soft_on[t] = hip_state[t] == 2 && !(mode == REMAP_TRIG_HIP_EXCL && full);
			}
		}
		else
		{
			soft_on[t] = past;
		}
		soft_past[t] = past;
		{
			/* analog trigger to Steam: output range */
			uint16_t a = p->trig_range[t][0], e = p->trig_range[t][1];

			if((a || e) && e > a + 100U)
			{
				int32_t lo = (int32_t)a * 32767 / 10000, hi = (int32_t)e * 32767 / 10000;
				int16_t o = (int16_t)CLAMP((v - lo) * 32767 / (hi - lo), 0, 32767);

				if(t)
				{
					r->trigger_right = o;
				}
				else
				{
					r->trigger_left = o;
				}
			}
		}
		if(p->ext.trig_curve[t] && p->ext.trig_curve[t] != 100U)
		{
			/* analog response curve (to Steam and the Xbox pad) */
			int16_t tv = t ? r->trigger_right : r->trigger_left;

			if(tv > 0)
			{
				tv = (int16_t)CLAMP(lroundf(32767.0f * powf((float)tv / 32767.0f, (float)p->ext.trig_curve[t] * 0.01f)), 0, 32767);
				if(t)
				{
					r->trigger_right = tv;
				}
				else
				{
					r->trigger_left = tv;
				}
			}
		}
		{
			/* "Xbox LT / RT (whole trigger)" on the soft pull: the analog value
			 * (with the analog start / end above) goes to the Xbox pad */
			const struct remap_binding *sb = binding(p, REMAP_SRC_LT_SOFT + t);
			int16_t av = t ? r->trigger_right : r->trigger_left;

			for(int i = 0; i < 4; ++i)
			{
				uint8_t xa = sb->act[0][i];

				if(xa == REMAP_ACT_XTRIG_L || xa == REMAP_ACT_XTRIG_R)
				{
					uint8_t o = (uint8_t)(MAX(av, 0) >> 7);

					xtrig_an[xa - REMAP_ACT_XTRIG_L] = MAX(xtrig_an[xa - REMAP_ACT_XTRIG_L], o);
				}
			}
		}
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
	xbox_trig[0] = xtrig_an[0];
	xbox_trig[1] = xtrig_an[1];
	move_dirs = 0U;
	PERF_MARK(6);
	const struct remap_profile *loop_l = layer_profile(); /* the same for every source */
	{
		/* unbound sources do nothing below: find the bound ones only when the profile,
		 * the layer or the shift state changes (looking all 78 up took ~0.3 ms a pass) */
		uint8_t sh = 0;

		for(int g = 0; g < REMAP_SHIFT_GROUPS; ++g)
		{
			sh |= shifted[g] ? (uint8_t)BIT(g) : 0U;
		}
		if(!bcache.ok || bcache.p != p || bcache.l != loop_l || bcache.sh != sh || bcache.gen != prof_gen)
		{
			memset(bcache.m, 0, sizeof(bcache.m));
			for(int s = 0; s < REMAP_SRC_COUNT; ++s)
			{
				if(bound(binding_l(p, s, loop_l)))
				{
					bcache.m[s >> 5] |= BIT(s & 31);
				}
			}
			bcache.p = p;
			bcache.l = loop_l;
			bcache.sh = sh;
			bcache.gen = prof_gen;
			bcache.ok = true;
		}
	}

	for(int s = 0; s < REMAP_SRC_COUNT; ++s)
	{
		PERF_LOOK0();
		bool b_on = (bcache.m[s >> 5] & BIT(s & 31)) != 0U;

		PERF_LOOK1(b_on);
		if(!b_on)
		{
			continue;
		}
		const struct remap_binding *b = binding_l(p, s, loop_l);
		struct src_state *ss = &st[s];

		out_src = (int8_t)s;
		if(s < 30)
		{
			hide |= BIT(s);
			bound_mask |= BIT(s);
		}
		if(radial_of_src[s] >= 0 && radial_mode[radial_of_src[s]])
		{
			drop_hold(ss);
			if(radial_fire[radial_of_src[s]] == s)
			{
				tap(b, ss, s, now);
			}
		}
		else if(s >= REMAP_SRC_LPAD_CW && s < REMAP_SRC_LT_SOFT)
		{
			int j = (s - REMAP_SRC_LPAD_CW) / 2;

			drop_hold(ss);
			if(wheel_fire[j] & BIT((s - REMAP_SRC_LPAD_CW) % 2))
			{
				tap(b, ss, s, now);
			}
		}
		else if(s >= REMAP_SRC_LPAD_UP && s < REMAP_SRC_LPAD_GATE && swipe_mode[(s - REMAP_SRC_LPAD_UP) / 4])
		{
			drop_hold(ss);
			if(swipe_fire[(s - REMAP_SRC_LPAD_UP) / 4] & BIT((s - REMAP_SRC_LPAD_UP) % 4))
			{
				tap(b, ss, s, now);
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
			else if(s == REMAP_SRC_LPAD_CENTER || s == REMAP_SRC_LPAD_CENTER + 1)
			{
				pj = s - REMAP_SRC_LPAD_CENTER;
			}
			uint8_t chord = REMAP_CHORD(b->haptic);
			/* chorded: only while the chord button is held too */
			bool down = src_pressed(s) && (chord == 0U || (chord <= 30U && (raw & BIT(chord - 1U))));

			if((s == CONTROLLER_BUTTON_LEFT_TOUCHPAD_CLICK && click_owned[0]) ||
			   (s == CONTROLLER_BUTTON_RIGHT_TOUCHPAD_CLICK && click_owned[1]))
			{
				down = false; /* the click belongs to the pad's mode now */
			}

			/* the input that switched the profile (still held) must not act in
			 * the new one until it is let go, or two profiles switching to each
			 * other on the same button would swap every report */
			if(block_pressed && src_pressed(s))
			{
				ss->block = true;
			}
			if(ss->block)
			{
				ss->block = src_pressed(s);
				down = false;
			}
			{
				/* per binding delay start / end: the press acts later / longer */
				const struct remap_btime *bt = btime_of(b);
				int64_t ds = (int64_t)bt->dstart10 * 10, de = (int64_t)bt->dend10 * 10;

				int32_t n32 = (int32_t)now;

				if(down && !ss->phys)
				{
					ss->t_pdown = n32;
					ss->phys_seen = true;
				}
				else if(!down && ss->phys)
				{
					ss->t_pup = n32;
				}
				ss->phys = down;
				if((ds || de) && ss->phys_seen)
				{
					down = (int32_t)(n32 - ss->t_pdown) >= (int32_t)ds &&
					       (down || (int32_t)(n32 - ss->t_pup) < (int32_t)(ds + de));
				}
			}
			run_source(b, ss, down, now, s, pj < 0 || !pad_act[pj],
			           s < 30 ? (newp & ~BIT(s)) != 0U : newp != 0U);
		}
		uint16_t turbo_ms = btime_of(b)->turbo5 ? MAX(btime_of(b)->turbo5 * 5U, 20U) : prof_turbo_ms;

		/* "keep": the press group while held, next to a long / double press */
		if(ss->down && ss->held_group != 0 && (btime_of(b)->flags & REMAP_BT_KEEP) &&
		   (b->mode & (REMAP_MODE_HOLD | REMAP_MODE_DOUBLE)) && !(b->mode & (REMAP_MODE_START | REMAP_MODE_TOGGLE | REMAP_MODE_CYCLE)))
		{
			for(size_t i = 0; i < 4; ++i)
			{
				out_action(b->act[0][i], keys, &nk, &mods, &pad);
			}
		}
		/* turbo: the set being held repeats - the press set, or with a long press the long
		 * press set (as Steam's turbo on a long press activator); before v51 test 55 only the
		 * press set, and turbo + long press held the long press set without repeating */
		bool turbo = (b->mode & REMAP_MODE_TURBO) && (ss->held_group == 0 || ss->held_group == 1);
		bool turbo_gap = turbo && (((now - ss->t_down) / (turbo_ms / 2)) % 2 == 1 || turbo_waits(b));

		if(ss->held_group >= 0 && !turbo_gap)
		{
			for(size_t i = 0; i < 4; ++i)
			{
				out_action(b->act[ss->held_group][i], keys, &nk, &mods, &pad);
			}
		}
		/* turbo: every repeat is the binding firing again, with its haptic (the first press
		 * of a hold has it from fire()); before v51 test 46 only the first one ticked */
		if(turbo && !turbo_gap && !ss->turbo_on && ss->turbo_again)
		{
			feedback_src(REMAP_HAPTIC_LEVEL(b->haptic), s);
		}
		ss->turbo_again = turbo && (ss->turbo_again || !turbo_gap);
		ss->turbo_on = turbo && !turbo_gap;
		if(ss->pulse_rel && now >= ss->pulse_until + REPEAT_GAP_MS)
		{
			ss->pulse_rel = false;
			ss->pulse_group = 2;
			ss->pulse_until = now + REPEAT_TAP_MS;
			for(size_t i = 0; i < 4; ++i)
			{
				edge_action(b->act[2][i]);
			}
		}
		else if(ss->pulse_more && now >= ss->pulse_until + REPEAT_GAP_MS)
		{
			ss->pulse_more--;
			ss->pulse_group = 0;
			ss->pulse_until = now + REPEAT_TAP_MS;
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

	PERF_MARK(7);
	/* sticks: mouse / scroll, or to Steam with the dead zones applied */
	for(int i = 0; i < 2; ++i)
	{
		const struct remap_analog *an = analog_cfg(p, i);
		int16_t x = i ? r->stick_right_x : r->stick_left_x;
		int16_t y = i ? r->stick_right_y : r->stick_left_y;

		if(an->mode == REMAP_ANALOG_PASS)
		{
			stick_dead_zones(an, remap_curve_stick(i), &x, &y);
		}
		else if(an->mode == REMAP_ANALOG_XBOX)
		{
			int t = i ^ ((an->flags & REMAP_AF_OTHER_STICK) ? 1 : 0);

			flick_on[i] = false;
			stick_dead_zones(an, remap_curve_stick(i), &x, &y);
			xbox_stick[t][0] += (float)x / 32767.0f;
			xbox_stick[t][1] += (float)y / 32767.0f;
			x = y = 0;
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
				stick_analog(i, an, analog_x(p, i), x, y, dt);
			}
			x = y = 0;
		}
		x = (int16_t)CLAMP((int32_t)x + (int32_t)lroundf((pad_joy[i][0] + acc_joy[i][0]) * 32767.0f), -32767, 32767);
		y = (int16_t)CLAMP((int32_t)y + (int32_t)lroundf((pad_joy[i][1] + acc_joy[i][1]) * 32767.0f), -32767, 32767);
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
	for(int i = 0; i < 2; ++i)
	{
		for(int k = 0; k < 2; ++k)
		{
			xbox_stick_out[i][k] = (int16_t)CLAMP(lroundf(xbox_stick[i][k] * 32767.0f), -32767, 32767);
		}
	}
	flick_step(dt);
	region_output(p);
	/* region and flick counts: into the mouse a report's worth at a time; over USB as much as
	 * this pass's mouse reports carry (8 x 127, see send_mouse_locked), so a flick turn keeps its
	 * speed whatever the loop rate (it was 254 per pass: a 180 degree flick took 2x as long) */
	for(int i = 0; i < 2; ++i)
	{
		const float cap = transport_link_kind() == 0U ? 1016.0f : 254.0f;

		if(precise_pending[i] != 0.0f && fabsf(mouse_acc[i]) < 127.0f)
		{
			float t = CLAMP(precise_pending[i], -cap, cap);

			mouse_acc[i] += t;
			precise_pending[i] -= t;
		}
	}
	/* macros: each step holds its action for its time */
	for(int m = 0; m < 4; ++m)
	{
		struct macro_run *mr = &macros[m];

		while(mr->on)
		{
			const uint8_t *st2;
			uint32_t dur;

			if(mr->step >= REMAP_MACRO_STEPS)
			{
				mr->on = false;
				break;
			}
			st2 = p->macro[m][mr->step];
			dur = st2[1] ? (uint32_t)st2[1] * 10U : 30U;
			if(st2[0] == 0U && st2[1] == 0U)
			{
				mr->on = false;
				break;
			}
			if(now - mr->t < (int64_t)dur)
			{
				out_src = -1;
				out_action(st2[0], keys, &nk, &mods, &pad);
				break;
			}
			mr->t += dur;
			mr->step++;
		}
	}
	/* accelerometer gestures (v51.8): their actions, tapped */
	{
		int gi = active_index();

		for(int k = 0; gi >= 0 && k < REMAP_ACC_GESTURES; ++k)
		{
			if((int32_t)(k_uptime_get_32() - gest_until[k]) < 0)
			{
				out_src = -1;
				out_action(accs[gi].gest[k][0], keys, &nk, &mods, &pad);
				out_action(accs[gi].gest[k][1], keys, &nk, &mods, &pad);
			}
		}
	}
	/* analog output actions: full stick deflection / full trigger */
	if(stick_out)
	{
		int32_t lx = r->stick_left_x, ly = r->stick_left_y, rx = r->stick_right_x, ry = r->stick_right_y;

		ly += (stick_out & BIT(0)) ? 32767 : 0;
		ly -= (stick_out & BIT(1)) ? 32767 : 0;
		lx -= (stick_out & BIT(2)) ? 32767 : 0;
		lx += (stick_out & BIT(3)) ? 32767 : 0;
		ry += (stick_out & BIT(4)) ? 32767 : 0;
		ry -= (stick_out & BIT(5)) ? 32767 : 0;
		rx -= (stick_out & BIT(6)) ? 32767 : 0;
		rx += (stick_out & BIT(7)) ? 32767 : 0;
		r->stick_left_x = (int16_t)CLAMP(lx, -32767, 32767);
		r->stick_left_y = (int16_t)CLAMP(ly, -32767, 32767);
		r->stick_right_x = (int16_t)CLAMP(rx, -32767, 32767);
		r->stick_right_y = (int16_t)CLAMP(ry, -32767, 32767);
		if(stick_out & BIT(8))
		{
			r->trigger_left = 32767;
		}
		if(stick_out & BIT(9))
		{
			r->trigger_right = 32767;
		}
	}
	if(move_dirs)
	{
		float k = (float)(p->move_speed ? p->move_speed : 1000U) * dt;

		mouse_acc[0] += k * (float)(((move_dirs >> 3) & 1) - ((move_dirs >> 2) & 1));
		mouse_acc[1] += k * (float)(((move_dirs >> 1) & 1) - (move_dirs & 1));
	}
	if(p->flags & REMAP_PF_BLOCK_UNBOUND)
	{
		/* only what is mapped reaches Steam: unbound buttons and touch sensors
		 * are dropped, trackpads left "to Steam" too */
		hide |= (BIT(30) - 1U) & ~bound_mask;
		/* the analog triggers too (they have no "to Steam" mode): only a
		 * "trigger full" action from a binding still pulls them */
		if(!(stick_out & BIT(8)))
		{
			r->trigger_left = 0;
		}
		if(!(stick_out & BIT(9)))
		{
			r->trigger_right = 0;
		}
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
	/* no Xbox pad to send to (Bluetooth, Puck, or USB with the Steam pad only): the
	 * Xbox outputs go to the Steam pad instead - same buttons, sticks, triggers -
	 * so a layout made for the Xbox pad still works there (Steam Input sees them) */
	if(!xbox_live())
	{
		int32_t v;

		pad |= xbox_buttons & (BIT(30) - 1U);
		xbox_buttons = 0U;
		v = r->stick_left_x + xbox_stick_out[0][0];
		r->stick_left_x = (int16_t)CLAMP(v, -32767, 32767);
		v = r->stick_left_y + xbox_stick_out[0][1];
		r->stick_left_y = (int16_t)CLAMP(v, -32767, 32767);
		v = r->stick_right_x + xbox_stick_out[1][0];
		r->stick_right_x = (int16_t)CLAMP(v, -32767, 32767);
		v = r->stick_right_y + xbox_stick_out[1][1];
		r->stick_right_y = (int16_t)CLAMP(v, -32767, 32767);
		memset(xbox_stick_out, 0, sizeof(xbox_stick_out));
		r->trigger_left = (int16_t)MAX(r->trigger_left, (int16_t)(xbox_trig[0] * 32767 / 255));
		r->trigger_right = (int16_t)MAX(r->trigger_right, (int16_t)(xbox_trig[1] * 32767 / 255));
		xbox_trig[0] = xbox_trig[1] = 0U;
	}
	r->buttons = (r->buttons & ~hide) | pad;
	mon_pad = pad;
	block_pressed = false;
	PERF_MARK(8);
	refresh_outputs();

	{
		uint8_t kbd[8] = { mods, 0 };

		memcpy(&kbd[2], keys, sizeof(keys));
		/* keys pressed while the radio queue was full: keep them in the report
		 * until one with them gets through, so a short tap is never lost */
		kbd[0] |= kbd_pend[0];
		for(int i = 2; i < 8 && kbd_pend[i]; ++i)
		{
			if(memchr(&kbd[2], kbd_pend[i], 6) == NULL && nk < 6U)
			{
				kbd[2 + nk++] = kbd_pend[i];
			}
		}
		/* over the radio a release waits until the press has left the queue
		 * for RADIO_RELEASE_MS: queued together, press and release reach the
		 * PC in the same packet / connection event, a press of 0-1 ms that a
		 * game never sees (fast turbo) */
		/* a key pressed again while its release still waits: the release goes
		 * out first, now (every press counts, even when they come too fast
		 * for the link to keep them apart) */
		if(kbd_valid && repressed(kbd, kbd_owed))
		{
			uint8_t rel[8];

			memcpy(rel, last_kbd, sizeof(rel));
			strip_keys(rel, kbd_owed);
			if(send_kbd(rel) == 0)
			{
				memcpy(last_kbd, rel, sizeof(rel));
			}
			memset(kbd_owed, 0, sizeof(kbd_owed));
		}
		if(kbd_valid && releases_key(kbd, last_kbd) && !release_ok_now())
		{
			/* remember what we hold back */
			kbd_owed[0] |= last_kbd[0] & (uint8_t)~kbd[0];
			for(int i = 2; i < 8 && last_kbd[i]; ++i)
			{
				if(memchr(&kbd[2], last_kbd[i], 6) == NULL && memchr(&kbd_owed[2], last_kbd[i], 6) == NULL)
				{
					uint8_t *slot = memchr(&kbd_owed[2], 0, 6);

					if(slot != NULL)
					{
						*slot = last_kbd[i];
					}
				}
			}
			kbd[0] |= last_kbd[0];
			for(int i = 2; i < 8 && last_kbd[i]; ++i)
			{
				size_t used = 0;

				while(used < 6U && kbd[2 + used])
				{
					++used;
				}
				if(memchr(&kbd[2], last_kbd[i], used) == NULL && used < 6U)
				{
					kbd[2 + used] = last_kbd[i];
				}
			}
		}
		if(!kbd_valid || memcmp(kbd, last_kbd, sizeof(kbd)) != 0)
		{
			if(send_kbd(kbd) == 0)
			{
				if(memcmp(kbd, last_kbd, sizeof(kbd)) != 0)
				{
					out_change_ms = now; /* a real change, not the first empty report */
				}
				memcpy(last_kbd, kbd, sizeof(kbd));
				kbd_valid = true;
				kbd_flush = false;
				keep_owed(kbd_owed, kbd);
				memset(kbd_pend, 0, sizeof(kbd_pend));
			}
			else
			{
				/* remember what is newly pressed in the report that did not go */
				size_t np = 0;

				while(np < 6U && kbd_pend[2 + np])
				{
					++np;
				}
				kbd_pend[0] |= kbd[0] & (uint8_t)~(kbd_valid ? last_kbd[0] : 0U);
				for(int i = 2; i < 8 && kbd[i]; ++i)
				{
					bool was = kbd_valid && memchr(&last_kbd[2], kbd[i], 6) != NULL;

					if(!was && memchr(&kbd_pend[2], kbd[i], np) == NULL && np < 6U)
					{
						kbd_pend[2 + np++] = kbd[i];
					}
				}
			}
		}
	}
	PERF_MARK(9);
	send_mouse();
	PERF_MARK(10);
}


/* ---- response curves (v51.7): sc26re/crv/<slot>[/<offset hex>], slot 0..3 or g ---- */
#define CRV_PREFIX "sc26re/crv/"
BUILD_ASSERT(sizeof(struct remap_curves) == 68, "curve layout (web/remap.html)");
static struct remap_curves curves[REMAP_CURVE_SLOTS];
/* the curves' and the accelerometer's staging copy share one buffer: a page uploads one at a time;
 * a stage of the other kind takes it over (the half-staged one is dropped) */
static union
{
	struct remap_curves crv;
	struct remap_accel acc;
} blob_staging;
#define crv_staging (blob_staging.crv)
#define acc_staging (blob_staging.acc)
static int8_t crv_staging_s = -1;

static int active_index(void)
{
	const struct remap_profile *p = active();

	if(!remap_enabled() || p->magic != REMAP_MAGIC || p->version != REMAP_VERSION)
	{
		return -1;
	}
	return CLAMP(sc26re_cfg_get(SC26RE_CFG_REMAP_PROFILE), 0, REMAP_PROFILES - 1);
}

const uint16_t *remap_curve_stick(int side)
{
	int i = active_index();

	if(i < 0 || side < 0 || side > 1 || !(curves[i].on & BIT(side)))
	{
		return NULL;
	}
	return curves[i].pt[side];
}

bool remap_curve_gyro(int which, const uint16_t **pt, float *mouse_max)
{
	int i = active_index();
	const struct remap_curves *c;

	/* a profile with its own gyro settings has its own curves too (or none) */
	if(i < 0 || !(profiles[i].flags & REMAP_PF_GYRO))
	{
		i = REMAP_PROFILES;
	}
	c = &curves[i];
	if(which < REMAP_CURVE_GMOUSE || which > REMAP_CURVE_GJOY || !(c->on & BIT(which)))
	{
		return false;
	}
	*pt = c->pt[which];
	*mouse_max = (float)(c->mouse_max > 0 ? c->mouse_max : REMAP_CURVE_MOUSE_MAX);
	return true;
}

/* <prefix><slot>[/<offset hex>]: slot 0..3, or g (global) where allowed */
static int blob_parse(const char *path, const char *prefix, bool global, size_t size, int *slot, int *off)
{
	if(strncmp(path, prefix, strlen(prefix)) != 0)
	{
		return -1;
	}
	path += strlen(prefix);
	if(path[0] == 'g' && global)
	{
		*slot = REMAP_PROFILES;
	}
	else if(path[0] >= '0' && path[0] < '0' + REMAP_PROFILES)
	{
		*slot = path[0] - '0';
	}
	else
	{
		return -1;
	}
	*off = -1;
	if(path[1] == '/')
	{
		*off = (int)strtol(&path[2], NULL, 16);
		if(*off < 0 || *off >= (int)size)
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

static int crv_parse(const char *path, int *slot, int *off)
{
	return blob_parse(path, CRV_PREFIX, true, sizeof(struct remap_curves), slot, off);
}

/* ---- accelerometer (v51.8): sc26re/acc/<profile>[/<offset hex>] ---- */
#define ACC_PREFIX "sc26re/acc/"
BUILD_ASSERT(sizeof(struct remap_accel) == 32, "accelerometer layout (web/remap.html)");
static int8_t acc_staging_s = -1;
static float acc_ref[3];
static bool acc_ref_ok;

static int acc_parse(const char *path, int *slot, int *off)
{
	return blob_parse(path, ACC_PREFIX, false, sizeof(struct remap_accel), slot, off);
}

static int acc_load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	ARG_UNUSED(param);
	if(key == NULL || key[1] != '\0' || len != sizeof(struct remap_accel) || key[0] < '0' ||
	   key[0] >= '0' + REMAP_PROFILES)
	{
		return 0;
	}
	(void)read_cb(cb_arg, &accs[key[0] - '0'], len);
	return 0;
}

/* tilt of the gravity vector since the reference (degrees): forward and sideways, as the gyro
 * tilt directions */
static void tilt_angles(const float ref[3], const float a[3], float *fwd, float *side)
{
	*fwd = atan2f(ref[1] * a[2] - ref[2] * a[1], ref[1] * a[1] + ref[2] * a[2]) * 57.29578f;
	*side = -atan2f(ref[2] * a[0] - ref[0] * a[2], ref[2] * a[2] + ref[0] * a[0]) * 57.29578f;
}

static float acc_axis(float deg, float full, float dz)
{
	float v = fabsf(deg) / full;

	v = dz < 1.0f ? CLAMP((v - dz) / (1.0f - dz), 0.0f, 1.0f) : 0.0f;
	return deg < 0.0f ? -v : v;
}

static bool acc_gestures_bound(const struct remap_accel *c)
{
	for(int k = 0; k < REMAP_ACC_GESTURES; ++k)
	{
		if(c->gest[k][0] != 0U || c->gest[k][1] != 0U)
		{
			return true;
		}
	}
	return false;
}

/* gestures: jolts along the controller's axes (x right, y forward, z up), gravity taken out
 * by a slow average, measured in units of that gravity (so 1.0 = 1 g whatever the sensor's
 * scale). The first jolt over the threshold fires its direction, then 0.35 s of nothing (the
 * stop at the end of a jab is a jolt the other way). Back-and-forth jolts: a shake. */
static float gest_grav[3];
static bool gest_grav_ok;
static uint32_t gest_quiet_until;
static int8_t gest_last_sign;   /* shake: the last jolt over the threshold, +-(axis + 1) */
static uint8_t gest_count;
static uint32_t gest_first_ms, gest_dir_ms, gest_shake_block, gest_cross_ms;
static bool gest_armed = true;  /* shake: the jolt dropped below half the threshold since */

static void acc_gestures_reset(void)
{
	gest_grav_ok = false;
	gest_count = 0U;
	gest_armed = true;
	gest_quiet_until = gest_shake_block = gest_dir_ms = gest_first_ms = gest_cross_ms = k_uptime_get_32() - 10000U;
	memset(gest_until, 0, sizeof(gest_until));
}

static void gestures(const struct remap_accel *c, const float a[3], float dt, uint32_t now)
{
	float lin[3], g, thr, best = 0.0f;
	int axis = 0;

	if(!gest_grav_ok)
	{
		memcpy(gest_grav, a, sizeof(gest_grav));
		gest_grav_ok = true;
		return;
	}
	for(int k = 0; k < 3; ++k)
	{
		gest_grav[k] += (a[k] - gest_grav[k]) * MIN(dt / 0.25f, 1.0f);
	}
	g = sqrtf(gest_grav[0] * gest_grav[0] + gest_grav[1] * gest_grav[1] + gest_grav[2] * gest_grav[2]);
	if(g < 1000.0f)
	{
		return;
	}
	for(int k = 0; k < 3; ++k)
	{
		lin[k] = (a[k] - gest_grav[k]) / g;
		if(fabsf(lin[k]) > fabsf(best))
		{
			best = lin[k];
			axis = k;
		}
	}
	thr = REMAP_ACC_GEST_G * 100.0f / (float)(c->gest_sens ? c->gest_sens : 100U);
	/* shake: jolts over the threshold that change direction */
	if(fabsf(best) < thr * 0.5f)
	{
		gest_armed = true;
	}
	if((int32_t)(now - gest_first_ms) > 800 || (int32_t)(now - gest_cross_ms) > 300)
	{
		/* a shake is one movement back and forth: a pause of more than 0.3 s between jolts
		 * ends it (seen on the controller: a jab left, its stop, then a jab right and its stop
		 * 0.8 s later made 4 jolts and so a shake) */
		gest_count = 0U;
	}
	if(fabsf(best) > thr)
	{
		int8_t sg = (int8_t)(best > 0.0f ? axis + 1 : -(axis + 1));
		bool counts = gest_armed || sg != gest_last_sign; /* a straight flip counts too */

		gest_armed = false;
		if(counts && (int32_t)(now - gest_shake_block) < 0)
		{
			/* still the same shake: one shake fires once, however long it goes on */
			gest_shake_block = now + 600U;
			gest_last_sign = sg;
			counts = false;
		}
		if(counts)
		{
			gest_cross_ms = now;
		}
		if(counts && (gest_count == 0U || sg != gest_last_sign))
		{
			if(gest_count == 0U)
			{
				gest_first_ms = now;
			}
			gest_count++;
			gest_last_sign = sg;
		}
		if(gest_count >= 4U)
		{
			gest_count = 0U;
			if(c->gest[REMAP_GEST_SHAKE][0] || c->gest[REMAP_GEST_SHAKE][1])
			{
				gest_until[REMAP_GEST_SHAKE] = now + REMAP_ACC_GEST_MS;
			}
			gest_quiet_until = now + 600;
			gest_shake_block = now + 600U;
		}
	}
	if((int32_t)(now - gest_quiet_until) < 0 || fabsf(best) <= thr)
	{
		return;
	}
	if((int32_t)(now - gest_shake_block) < 0)
	{
		/* a shake that goes on: no directions out of it (seen on the controller: a long shake
		 * fired "left" once the 0.6 s after the shake had passed) */
		gest_shake_block = now + 600U;
		return;
	}
	/* a jolt soon after the last one: shaking, not a gesture of its own */
	if((int32_t)(now - gest_dir_ms) < 700)
	{
		gest_dir_ms = now;
		gest_quiet_until = now + 350;
		return;
	}
	{
		static const uint8_t dir[3][2] = {
			{ REMAP_GEST_RIGHT, REMAP_GEST_LEFT },
			{ REMAP_GEST_FORWARD, REMAP_GEST_BACK },
			{ REMAP_GEST_UP, REMAP_GEST_DOWN },
		};
		int k = dir[axis][best > 0.0f ? 0 : 1];

		gest_until[k] = now + REMAP_ACC_GEST_MS;
		gest_dir_ms = now;
		gest_quiet_until = now + 350;
	}
}

/* this pass: the accelerometer's outputs for profile i (-1 = remapper off) */
static void acc_process(int i, const struct controller_report *rs, float dt)
{
	const struct remap_accel *c = i >= 0 ? &accs[i] : NULL;
	bool want = c != NULL && (c->stick != REMAP_ACC_STICK_OFF || c->btn != 0U || acc_gestures_bound(c));
	static bool wanted;
	float a[3], m, fwd, side, full, dz, x, y;

	memset(acc_joy, 0, sizeof(acc_joy));
	acc_dirs = 0U;
	if(want != wanted)
	{
		/* the IMU stays on for the accelerometer bindings, with or without Steam */
		imu_force_reason(IMU_FORCE_ACCEL, want);
		wanted = want;
		acc_ref_ok = false;
		acc_gestures_reset();
	}
	if(!want || ((c->flags & REMAP_ACC_GYRO_PAUSE) && gyro_mouse_active()))
	{
		gest_grav_ok = false; /* the slow gravity average starts again afterwards */
		return;
	}
#ifdef CONFIG_IBEX_HWTEST
	if(hwtest_running())
	{
		a[0] = (float)rs->accel_x;
		a[1] = (float)rs->accel_y;
		a[2] = (float)rs->accel_z;
	}
	else
#endif
	if(!imu_accel(a))
	{
		return;
	}
	ARG_UNUSED(rs);
	if(acc_gestures_bound(c))
	{
#ifdef CONFIG_IBEX_HWTEST
		bool settling = !hwtest_running() && imu_settling();
#else
		bool settling = imu_settling();
#endif
		/* while the IMU restarts its sample sizes jump (the driver passes other ranges): measured
		 * on a still controller, every gyro range change fired "jolt down"; start the gravity
		 * average again after it instead */
		if(settling)
		{
			acc_gestures_reset();
		}
		else
		{
			gestures(c, a, dt, k_uptime_get_32());
		}
	}
	m = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
	if(m < 1000.0f)
	{
		return; /* no sample yet */
	}
	for(int k = 0; k < 3; ++k)
	{
		a[k] /= m;
	}
	if(!acc_ref_ok)
	{
		/* neutral: how the controller is held when the profile becomes active */
		memcpy(acc_ref, a, sizeof(acc_ref));
		acc_ref_ok = true;
	}
	tilt_angles(acc_ref, a, &fwd, &side);
	if(c->btn != 0U)
	{
		/* tilt -> the 4 tilt sources, as the gyro tilt mode: a direction already on stays
		 * until 3 degrees back (half way for a small threshold) */
		static uint8_t last;
		float on = REMAP_ACC_BTN_DEG * 100.0f / (float)(c->btn_sens ? c->btn_sens : 100U);
		float off = on - MIN(3.0f, on * 0.5f);
		uint8_t d = 0U;

		if(fwd > ((last & 1U) ? off : on)) d |= 1U;
		if(fwd < -((last & 2U) ? off : on)) d |= 2U;
		if(side < -((last & 4U) ? off : on)) d |= 4U;
		if(side > ((last & 8U) ? off : on)) d |= 8U;
		if(c->btn == 1U && (d & 3U) && (d & 12U))
		{
			d &= fabsf(fwd) >= fabsf(side) ? 3U : 12U;
		}
		last = d;
		acc_dirs = d;
	}
	if(c->stick == REMAP_ACC_STICK_OFF)
	{
		return;
	}
	full = REMAP_ACC_FULL_DEG * 100.0f / (float)(c->sens ? c->sens : 100U);
	dz = (float)c->dz * 0.0001f;
	x = acc_axis((c->flags & REMAP_ACC_SWAP) ? fwd : side, full, dz);
	y = acc_axis((c->flags & REMAP_ACC_SWAP) ? side : fwd, full, dz);
	if(c->flags & REMAP_ACC_INV_X)
	{
		x = -x;
	}
	if(c->flags & REMAP_ACC_INV_Y)
	{
		y = -y;
	}
	acc_joy[c->stick == REMAP_ACC_STICK_RIGHT ? 1 : 0][0] = x;
	acc_joy[c->stick == REMAP_ACC_STICK_RIGHT ? 1 : 0][1] = y;
}

static int crv_load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	int s;

	ARG_UNUSED(param);
	if(key == NULL || key[1] != '\0' || len != sizeof(struct remap_curves))
	{
		return 0;
	}
	if(key[0] == 'g')
	{
		s = REMAP_PROFILES;
	}
	else if(key[0] >= '0' && key[0] < '0' + REMAP_PROFILES)
	{
		s = key[0] - '0';
	}
	else
	{
		return 0;
	}
	(void)read_cb(cb_arg, &curves[s], len);
	return 0;
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

	return parse_path(path, &p, &o) == 0 || crv_parse(path, &p, &o) == 0 || acc_parse(path, &p, &o) == 0;
}

bool remap_read(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
	int p, o;
	size_t n;

	if(crv_parse(path, &p, &o) == 0)
	{
		if(o < 0)
		{
			return false;
		}
		n = MIN(MIN((size_t)CHUNK, capacity), sizeof(struct remap_curves) - (size_t)o);
		memcpy(buf, (const uint8_t *)&curves[p] + o, n);
		*len = n;
		return true;
	}
	if(acc_parse(path, &p, &o) == 0)
	{
		if(o < 0)
		{
			return false;
		}
		n = MIN(MIN((size_t)CHUNK, capacity), sizeof(struct remap_accel) - (size_t)o);
		memcpy(buf, (const uint8_t *)&accs[p] + o, n);
		*len = n;
		return true;
	}
	if(parse_path(path, &p, &o) != 0 || o < 0)
	{
		return false;
	}
	n = MIN(MIN((size_t)CHUNK, capacity), sizeof(struct remap_profile) - (size_t)o);
	memcpy(buf, (const uint8_t *)&profiles[p] + o, n);
	*len = n;
	return true;
}

/*
 * Saving a profile from the page comes in 40-byte pieces. They go to a staging
 * copy; the profile in use changes only on COMMIT, all at once. Writing straight
 * into the live profile ran the remapper on half-old, half-new bindings during
 * the upload (seconds over the Puck) and left that mix behind when an upload
 * was cut off.
 */
/* a v6 profile (saved before v7, or written by an older page) becomes v7 with an empty v7 area;
 * a v7 one becomes v8 with its shifted clicks the same as the normal ones (as they acted) */
static void upgrade(struct remap_profile *p)
{
	if(p->magic == REMAP_MAGIC && p->version == REMAP_VERSION_V6)
	{
		memset(&p->ext, 0, sizeof(p->ext));
		p->version = REMAP_VERSION_V7;
	}
	if(p->magic == REMAP_MAGIC && p->version == REMAP_VERSION_V7)
	{
		memset(&p->v8, 0, sizeof(p->v8));
		for(int k = 0; k < REMAP_SCLICK_COUNT; ++k)
		{
			p->v8.sclick[k] = p->bind[sclick_src[k]];
			p->v8.sclick_bt[k] = p->ext.bt[sclick_src[k]];
		}
		p->version = REMAP_VERSION;
	}
}

static struct remap_profile staging;
static int8_t staging_p = -1;

#if defined(CONFIG_IBEX_HWTEST) || defined(SC26RE_SIM)
/* hardware test (hwtest.c): the staging buffer is free between uploads; -2 = lent to it */
uint8_t *remap_hwtest_buffer(size_t *size)
{
	if(staging_p >= 0)
	{
		return NULL;
	}
	staging_p = -2;
	*size = sizeof(staging);
	return (uint8_t *)&staging;
}

void remap_hwtest_started(void)
{
	tilt_ref_ok = false; /* gyro tilt / region centred on the script's first motion sample, not the desk */
	acc_ref_ok = false;
	acc_gestures_reset(); /* the gravity average from the script, not from the real sensor before it */
	gyro_reg[0] = gyro_reg[1] = 0.0f;
}

void remap_hwtest_ended(void)
{
	hwtest_ended = true;
}

void remap_hwtest_release(void)
{
	if(staging_p == -2)
	{
		staging_p = -1;
	}
}
#endif

int remap_stage(const char *path, const uint8_t *value, size_t len)
{
	int p, o;

	if(crv_parse(path, &p, &o) == 0)
	{
		if(o < 0)
		{
			return -EINVAL;
		}
		if(crv_staging_s != p)
		{
			crv_staging = curves[p];
			crv_staging_s = (int8_t)p;
			acc_staging_s = -1;
		}
		memcpy((uint8_t *)&crv_staging + o, value, MIN(len, sizeof(crv_staging) - (size_t)o));
		return 0;
	}
	if(acc_parse(path, &p, &o) == 0)
	{
		if(o < 0)
		{
			return -EINVAL;
		}
		if(acc_staging_s != p)
		{
			acc_staging = accs[p];
			acc_staging_s = (int8_t)p;
			crv_staging_s = -1;
		}
		memcpy((uint8_t *)&acc_staging + o, value, MIN(len, sizeof(acc_staging) - (size_t)o));
		return 0;
	}
	if(parse_path(path, &p, &o) != 0 || o < 0)
	{
		return -EINVAL;
	}
	len = MIN(len, sizeof(struct remap_profile) - (size_t)o);
#if defined(CONFIG_IBEX_HWTEST) || defined(SC26RE_SIM)
	if(staging_p == -2)
	{
		hwtest_buffer_lost(); /* an upload ends the hardware test */
	}
#endif
	if(staging_p != p)
	{
		staging = profiles[p];
		staging_p = (int8_t)p;
	}
	memcpy((uint8_t *)&staging + o, value, len);
	return 0;
}

int remap_commit(const char *path)
{
	char key[24];
	int p, o;

	if(crv_parse(path, &p, &o) == 0)
	{
		if(crv_staging_s == p)
		{
			curves[p] = crv_staging;
			crv_staging_s = -1;
		}
		snprintf(key, sizeof(key), "user/crv/%c", p < REMAP_PROFILES ? '0' + p : 'g');
		return settings_save_one(key, &curves[p], sizeof(curves[p]));
	}
	if(acc_parse(path, &p, &o) == 0)
	{
		if(acc_staging_s == p)
		{
			accs[p] = acc_staging;
			acc_staging_s = -1;
			acc_ref_ok = false;
		}
		snprintf(key, sizeof(key), "user/acc/%c", '0' + p);
		return settings_save_one(key, &accs[p], sizeof(accs[p]));
	}
	if(parse_path(path, &p, &o) != 0)
	{
		return -EINVAL;
	}
	if(staging_p == p)
	{
		upgrade(&staging);
		profiles[p] = staging;
		prof_gen++;
		staging_p = -1;
		reload_pending = true;
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
	struct remap_profile *p;

	ARG_UNUSED(param);
	if(key == NULL || key[0] < '0' || key[0] >= '0' + REMAP_PROFILES || key[1] != '\0' ||
	   (len != sizeof(struct remap_profile) && len != REMAP_PROFILE_V7_SIZE && len != REMAP_PROFILE_V6_SIZE))
	{
		return 0;
	}
	p = &profiles[key[0] - '0'];
	memset(p, 0, sizeof(*p));
	(void)read_cb(cb_arg, p, len);
	upgrade(p);
	prof_gen++;
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
	for(int i = 0; i < REMAP_SRC_COUNT; ++i)
	{
		radial_of_src[i] = -1;
	}
	for(int j = 0; j < 4; ++j)
	{
		for(int k = 0; k < (j < 2 ? 10 : 8); ++k)
		{
			radial_of_src[radial_src[j][k]] = (int8_t)j;
		}
	}
	for(int g = 0; g < REMAP_SHIFT_GROUPS; ++g)
	{
		for(int k = shift_first[g]; k < shift_first[g + 1]; ++k)
		{
			src_group[remap_shift_src[k]] = (int8_t)g;
			src_sidx[remap_shift_src[k]] = (int8_t)k;
		}
	}
	for(int k = 0; k < REMAP_SCLICK_COUNT; ++k)
	{
		src_group[sclick_src[k]] = (int8_t)sclick_group[k];
		src_sidx[sclick_src[k]] = (int8_t)(REMAP_SHIFT_SRC_COUNT + k);
	}
	(void)settings_load_subtree_direct("user/remap", load_cb, NULL);
	(void)settings_load_subtree_direct("user/crv", crv_load_cb, NULL);
	(void)settings_load_subtree_direct("user/acc", acc_load_cb, NULL);
}

#ifdef SC26RE_SIM
/* host simulator only: look at the staging copy */
const uint8_t *remap_sim_staging(int *p)
{
	*p = staging_p;
	return (const uint8_t *)&staging;
}
#endif
#ifdef SC26RE_SIM
void remap_sim_mouse(float *out)
{
	memcpy(out, mouse_acc, sizeof(mouse_acc));
	out[4] = flick_left;
	out[5] = flick_rate;
	out[6] = precise_pending[0];
	out[7] = precise_pending[1];
	out[8] = (float)rel_phase;
	out[9] = (float)move_dirs;
	out[10] = rel_tgt[0];
	out[11] = rel_tgt[1];
	out[12] = rel_cur[0];
	out[13] = rel_cur[1];
}
#endif
