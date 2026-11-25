/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

#define REMAP_PROFILES 4
#define REMAP_MAGIC 0xA7
#define REMAP_VERSION 8
#define REMAP_VERSION_V7 7 /* still loaded: its clicks are copied to the v8 shifted clicks */
#define REMAP_VERSION_V6 6 /* still loaded: extended with an empty v7 area */

/* sources: 0..29 controller button bits, then derived inputs */
enum
{
	REMAP_SRC_LS_UP = 30,     /* up, down, left, right */
	REMAP_SRC_RS_UP = 34,
	REMAP_SRC_LPAD_UP = 38,   /* up, down, left, right */
	REMAP_SRC_RPAD_UP = 42,
	REMAP_SRC_LPAD_GATE = 46, /* gates: top-left, top-right, bottom-left, bottom-right */
	REMAP_SRC_RPAD_GATE = 50,
	REMAP_SRC_LPAD_CW = 54,   /* scroll wheel: clockwise, counter-clockwise */
	REMAP_SRC_RPAD_CW = 56,
	REMAP_SRC_LT_SOFT = 58,   /* trigger soft pull (full pull = the click bit) */
	REMAP_SRC_RT_SOFT = 59,
	REMAP_SRC_LS_RING = 60,   /* outer ring: left stick, right stick, left pad, right pad */
	REMAP_SRC_LPAD_CENTER = 64, /* 9-zone grid centre: left pad, right pad */
	REMAP_BIND_COUNT = 66,      /* sources with a binding in remap_profile.bind */
	REMAP_SRC_GYRO_UP = 66,     /* gyro tilt: up (forward), down, left, right (bindings in ext.xbind) */
	REMAP_SRC_LS_DIAG = 70,     /* stick diagonals: up-right, down-right, down-left, up-left (radial menu / 8-way pad) */
	REMAP_SRC_RS_DIAG = 74,
	REMAP_SRC_COUNT = 78,
};
#define REMAP_XBIND_COUNT (REMAP_SRC_COUNT - REMAP_BIND_COUNT)

/* binding modes (bits) */
#define REMAP_MODE_HOLD 0x01   /* group 1 when held longer */
#define REMAP_MODE_DOUBLE 0x02 /* group 2 on a second press */
#define REMAP_MODE_TOGGLE 0x04 /* group 0 toggles on each press */
#define REMAP_MODE_START 0x08  /* group 0 tapped the moment it is pressed */
#define REMAP_MODE_RELEASE 0x10 /* group 2 tapped when released (instead of double) */
#define REMAP_MODE_TURBO 0x20   /* group 0 repeats while held (profile turbo rate) */
#define REMAP_MODE_CYCLE 0x40   /* each press taps the next set: press, hold, double/release */
/* binding.haptic: bits 0-1 haptic level, bits 2-7 chord button + 1 (only fires while it is held) */
#define REMAP_HAPTIC_LEVEL(h) ((h) & 3U)
#define REMAP_CHORD(h) ((h) >> 2)

/* action codes: 0x01..0x65 keyboard usages, 0xE0..0xE7 modifiers */
#define REMAP_ACT_MOUSE_L 0x70
#define REMAP_ACT_MOUSE_R 0x71
#define REMAP_ACT_WHEEL_UP 0x72
#define REMAP_ACT_WHEEL_DOWN 0x73
#define REMAP_ACT_WHEEL_LEFT 0x74
#define REMAP_ACT_WHEEL_RIGHT 0x75
#define REMAP_ACT_MOVE_UP 0x76    /* mouse moves while held: up, down, left, right */
#define REMAP_ACT_MOVE_DOWN 0x77
#define REMAP_ACT_MOVE_LEFT 0x78
#define REMAP_ACT_MOVE_RIGHT 0x79
#define REMAP_ACT_MOUSE_M 0x7A    /* middle, back (4), forward (5): Puck needs its patched firmware (5 buttons) */
#define REMAP_ACT_MOUSE_4 0x7B
#define REMAP_ACT_MOUSE_5 0x7C
#define REMAP_ACT_PAD 0x80     /* + Steam pad button bit 0..29 */
#define REMAP_ACT_XBOX 0xA0    /* + Steam button bit 0..29: Xbox pad only */
#define REMAP_ACT_PROFILE 0xC0 /* + profile 0..3: switch (action set) */
#define REMAP_ACT_LAYER 0xC4   /* + profile 0..3: lay over the current one while held (action layer) */
#define REMAP_ACT_MACRO 0xC8   /* + macro 0..3 of the profile */
#define REMAP_ACT_STICK 0xD0   /* + 0..7: left stick up/down/left/right, right stick up/down/left/right (full) */
#define REMAP_ACT_TRIG_L 0xD8  /* left / right trigger fully pulled (analog) */
#define REMAP_ACT_TRIG_R 0xD9
/* Xbox pad LT / RT, the whole trigger: on a soft-pull source the analog trigger
 * follows the physical one (0..100 %, analog start / end applied), on any other
 * source a full pull while it is active */
#define REMAP_ACT_XTRIG_L 0xDA
#define REMAP_ACT_XTRIG_R 0xDB
#define REMAP_MACRO_STEPS 16

/* stick / trackpad modes (struct remap_analog.mode) */
#define REMAP_ANALOG_PASS 0   /* to Steam as an analog input (sticks: dead zones applied) */
#define REMAP_ANALOG_DPAD4 1  /* 4-way: one direction at a time */
#define REMAP_ANALOG_DPAD8 2  /* 8-way: diagonals hold both directions */
#define REMAP_ANALOG_MOUSE 3
#define REMAP_ANALOG_SCROLL 4 /* sticks: scroll; trackpads: circular scroll wheel */
#define REMAP_ANALOG_GATES 5  /* trackpads: 4 corner gates */
#define REMAP_ANALOG_JOYSTICK 6 /* trackpads: finger position = stick deflection */
#define REMAP_ANALOG_FLICK 7    /* flick stick: point = turn there, rotate = keep turning */
#define REMAP_ANALOG_SWIPE 8    /* trackpads: swipes fire the 4 directions */
#define REMAP_ANALOG_GRID 9     /* trackpads: 3 x 3 zones (8 directions + centre), each its own binding */
#define REMAP_ANALOG_RADIAL 10  /* trackpads: radial menu, 2..10 slices (speed), fires on lift (or click) */
#define REMAP_ANALOG_SINGLE 11  /* trackpads: the whole pad is one button (the centre source), touch / click / pressure */
#define REMAP_ANALOG_MOUSEJOY 14 /* trackpads: Steam's mouse joystick - finger speed = stick deflection (camera
                                 * for games without a mouse); .speed = sensitivity %, .anti_dz = minimum output,
                                 * trackball + .dz_out friction keep it rolling; REMAP_AF_MJ_XBOX: the Xbox stick */
#define REMAP_ANALOG_REGION 13  /* mouse region: the stick / finger position is the cursor position inside a screen area */
#define REMAP_ANALOG_XBOX 12    /* sticks: to the Xbox pad's stick (dead zones applied); trackpads: finger position = Xbox stick.
                                 * REMAP_AF_OTHER_STICK: the other side's stick. The Steam pad does not get it. */

#define REMAP_PF_BLOCK_UNBOUND 0x01 /* inputs without a binding send nothing to Steam */
#define REMAP_PF_GYRO 0x02          /* the profile's own gyro settings replace the global ones */
#define REMAP_PF_GYRO_ON 0x04       /* gyro output on while this profile is active */
#define REMAP_GYRO_KEYS 30

/* trigger modes (soft pull = its own binding, full pull = the click) */
#define REMAP_TRIG_SIMPLE 0   /* soft at the threshold, full on the click, both can be on */
#define REMAP_TRIG_HAIR 1     /* soft on any further pull, off on any release */
#define REMAP_TRIG_HIP 2      /* hip fire: a quick full pull fires only full; a slow one soft, then full too */
#define REMAP_TRIG_HIP_EXCL 3 /* hip fire exclusive: soft and full never at the same time */

/* per binding timing (v7), 0 = the profile's value */
#define REMAP_BT_INTERRUPT 0x01 /* long press: another input pressed meanwhile cancels it */
#define REMAP_BT_KEEP 0x02      /* with a long / double press: the press still acts at once and while held
                                 * (Steam's regular press that is not "interruptible"); the long / double
                                 * press acts as well */
struct remap_btime
{
	uint8_t hold10;   /* long press time, x10 ms */
	uint8_t dbl10;    /* double press window, x10 ms */
	uint8_t turbo5;   /* turbo period, x5 ms */
	uint8_t dstart10; /* delay start: the press acts this much later, x10 ms */
	uint8_t dend10;   /* delay end: the press keeps acting this much longer, x10 ms */
	uint8_t flags;    /* REMAP_BT_* */
} __packed;

/* per stick / pad extras (v7): an[4] then the 8 shift groups' analog */
struct remap_anx
{
	uint16_t scale_x; /* mouse / scroll / joystick output: horizontal %, 0 = 100 */
	uint16_t scale_y; /* vertical %, 0 = 100 */
	uint16_t edge_spin; /* trackpad mouse: counts/s while the finger rests at the edge, 0 = off */
	uint8_t slices;   /* stick radial menu: 2..8 */
	uint8_t flags;    /* REMAP_AX_* */
	uint8_t region[4]; /* mouse region: centre x, centre y, width, height, % of the screen (0,0,0,0 = whole screen) */
} __packed;
#define REMAP_GA_ALL 0x01  /* every input of the activation mask held (else any one) */
#define REMAP_GA_LS 0x02   /* left stick out of its dead zone counts as an activation input */
#define REMAP_GA_RS 0x04   /* right stick out of its dead zone */
#define REMAP_AX_REGION_RETURN 0x01 /* mouse region (trackpad): the cursor goes back to the centre when the finger leaves */

/* gyro extras (v7): profile settings of their own, also without REMAP_PF_GYRO */
#define REMAP_GYRO_NORMAL 0  /* the gyro mouse / joystick settings */
#define REMAP_GYRO_TILT 1    /* tilting fires the gyro direction sources */
#define REMAP_GYRO_REGION 2  /* turning moves an absolute cursor inside a screen area */
struct remap_gyx
{
	uint8_t mode;      /* REMAP_GYRO_* */
	uint8_t space;     /* 0 = the global setting, 1 = local (the axis settings), 2 = player space, 3 = world space */
	uint8_t tilt_dz;   /* tilt: degrees before a direction fires (0 = 15) */
	uint8_t tilt_8way; /* tilt: 1 = diagonals hold two directions */
	uint8_t region[4]; /* mouse region: centre x, y, width, height (%) */
	uint8_t region_deg; /* degrees of turn across the region width (0 = 60) */
	uint32_t act_mask;  /* gyro activation by several inputs (Steam's gyro_ratchet_button_mask): raw button
	                     * bits 0..29, bit 30 / 31 = LT / RT soft pull; 0 with no REMAP_GA_* stick bits =
	                     * the gm_act_button setting (v51 test 62) */
	uint8_t act_flags;  /* REMAP_GA_* */
	uint8_t reserved[2];
} __packed;


#define REMAP_AF_CLICK 0x01       /* trackpads: active while clicked (else while touched) */
#define REMAP_AF_RING_INVERT 0x02 /* outer ring fires inside the radius instead */
#define REMAP_AF_OTHER_STICK 0x04 /* trackpad joystick: drive the other side's stick */
#define REMAP_AF_MJ_XBOX 0x08     /* mouse joystick: drive the Xbox pad's stick (else the Steam pad's) */
#define REMAP_AF_SWIPE_REPEAT 0x08 /* directional swipe: a long swipe fires again */
#define REMAP_AF_RING_FIRST 0x08 /* sticks in 4 / 8-way: reaching the outer ring lets the directions go for one pass, so the
                                  * ring binding (e.g. Shift) is down before they come back (W after Shift) */
#define REMAP_AF_PRESSURE 0x10     /* trackpads: active while pressed harder than .press */
#define REMAP_AF_INVERT_X 0x20
#define REMAP_AF_INVERT_Y 0x40
#define REMAP_AF_TRACKBALL 0x80    /* trackpad mouse: keeps rolling after the finger leaves (friction = dz_out) */

struct remap_analog
{
	uint8_t mode;
	uint8_t flags;
	uint16_t dz_in;   /* inner dead zone / activation / threshold, 0.01 % of the radius */
	uint16_t dz_out;  /* outer dead zone, 0.01 % */
	uint16_t speed;   /* mouse: counts/s (stick) or per swipe (pad); scroll: notches/s x100 (stick) or deg per step (wheel); flick: ms */
	uint8_t haptic;   /* feedback for mouse / scroll movement / zones: 0 off, 1 tick, 2 click, 3 strong */
	int8_t rotate;    /* rotation of the input, degrees */
	uint16_t curve;   /* response exponent x100 (100 linear, 200 squared); 0 = mode default; pad mouse: acceleration % */
	uint16_t ring;    /* outer ring radius, 0.01 % (0 = off) */
	uint16_t anti_dz; /* anti dead zone, 0.01 % */
	uint16_t press;   /* trackpads with REMAP_AF_PRESSURE: pressure needed, 0.01 % */
} __packed;

/* mode shift groups: while the group's shift button is held it uses its
 * alternative settings and bindings */
enum
{
	REMAP_SHIFT_ABXY,
	REMAP_SHIFT_DPAD,
	REMAP_SHIFT_LS,
	REMAP_SHIFT_RS,
	REMAP_SHIFT_LPAD,
	REMAP_SHIFT_RPAD,
	REMAP_SHIFT_LT,
	REMAP_SHIFT_RT,
	REMAP_SHIFT_GROUPS,
};
#define REMAP_SHIFT_SRC_COUNT 46

/* struct remap_shift.button beyond the button bits: the triggers' soft pulls */
#define REMAP_SHIFT_BY_SOFT_LT 31U
#define REMAP_SHIFT_BY_SOFT_RT 32U
struct remap_shift
{
	uint8_t button;    /* 0 none, else controller button bit + 1 */
	uint8_t dpad_4way; /* D-pad group */
	struct remap_analog an; /* stick / pad groups */
} __packed;

struct remap_binding
{
	uint8_t mode;
	uint8_t act[3][4]; /* group 0 press/tap, 1 hold, 2 double */
	uint8_t haptic;    /* feedback when it fires: 0 off, 1 tick, 2 click, 3 strong */
} __packed;

struct remap_ext
{
	struct remap_btime bt[REMAP_BIND_COUNT + REMAP_SHIFT_SRC_COUNT + REMAP_XBIND_COUNT]; /* bind, sbind, xbind */
	struct remap_binding xbind[REMAP_XBIND_COUNT]; /* gyro tilt and stick diagonal sources */
	struct remap_anx anx[4 + REMAP_SHIFT_GROUPS];
	struct remap_gyx gyx;
	uint16_t trig_curve[2]; /* analog trigger output: response exponent x100 (0 = linear) */
	uint16_t screen[2];     /* screen size in pixels for mouse regions over the Puck (0 = 1920 x 1080) */
	uint8_t reserved[8];
} __packed;

/* v8: the stick and trackpad clicks have bindings of their own while their group is shifted
 * (a trackpad mode shift used the normal Click binding). Order: left stick, right stick, left
 * pad, right pad (sources 15, 5, 26, 22) */
#define REMAP_SCLICK_COUNT 4
struct remap_v8
{
	struct remap_binding sclick[REMAP_SCLICK_COUNT];
	struct remap_btime sclick_bt[REMAP_SCLICK_COUNT];
	uint8_t reserved[8];
} __packed;

struct remap_profile
{
	uint8_t magic;
	uint8_t version;
	char name[14];
	uint8_t dpad_4way;     /* physical D-pad: 0 8-way, 1 4-way */
	uint8_t flags;         /* REMAP_PF_* */
	uint16_t trig_soft[2]; /* soft pull point, 0.01 % of travel */
	int16_t grid[2][4];    /* per pad, 9-zone grid lines: left, right, top, bottom, 0.01 % (-10000..10000) */
	uint16_t move_speed;   /* "mouse move" actions: counts per second (0 = 1000) */
	uint8_t led[3];        /* LED colour while this profile is active (0,0,0 = leave it) */
	uint8_t reserved1;
	int16_t gyro[REMAP_GYRO_KEYS]; /* with REMAP_PF_GYRO: values of remap_gyro_keys[] */
	uint8_t trig_mode[2];      /* REMAP_TRIG_* */
	uint16_t trig_hip_ms[2];   /* hip fire window (0 = 75 ms) */
	uint16_t trig_range[2][2]; /* analog trigger to Steam: start, end (0.01 %; 0,0 = full range) */
	uint16_t hold_ms;          /* long press time (0 = 250) */
	uint16_t double_ms;        /* double press window (0 = 250) */
	uint16_t turbo_ms;         /* turbo period (0 = 100) */
	int16_t gyro_fsr;          /* with REMAP_PF_GYRO: gyro range 250/500/1000/2000 dps (0 = global) */
	uint8_t reserved[2];
	uint8_t macro[4][REMAP_MACRO_STEPS][2]; /* steps: action, time x10 ms (action 0 = wait; 0,0 = end) */
	struct remap_analog an[4]; /* left stick, right stick, left pad, right pad */
	struct remap_shift shift[REMAP_SHIFT_GROUPS];
	struct remap_binding bind[REMAP_BIND_COUNT];
	struct remap_binding sbind[REMAP_SHIFT_SRC_COUNT]; /* shifted bindings, see remap_shift_src */
	struct remap_ext ext; /* v7 */
	struct remap_v8 v8;
} __packed;
#define REMAP_PROFILE_V6_SIZE 2056
#define REMAP_PROFILE_V7_SIZE 3144

/* response curves (v51.7), kept apart from the profiles (sc26re/crv/<slot>): Steam's custom
 * 4-point curve. Slots 0..3 belong to the profiles, slot 4 is global (gyro only). */
#define REMAP_CURVE_LS 0      /* left stick: to Steam / Xbox pad / mouse */
#define REMAP_CURVE_RS 1      /* right stick */
#define REMAP_CURVE_GMOUSE 2  /* gyro mouse: turn speed -> cursor speed */
#define REMAP_CURVE_GJOY 3    /* gyro joystick: turn speed past the dead zone -> deflection */
#define REMAP_CURVE_SLOTS (REMAP_PROFILES + 1)
#define REMAP_CURVE_MOUSE_MAX 360 /* default gyro mouse speed at x = 32767, deg/s */
struct remap_curves
{
	uint8_t on;          /* bit per REMAP_CURVE_*: the custom curve replaces the usual response */
	uint8_t reserved;
	int16_t mouse_max;   /* gyro mouse: deg/s at x = 32767 (0 = REMAP_CURVE_MOUSE_MAX) */
	uint16_t pt[4][8];   /* per curve 4 points x, y (0..32767) */
}; /* 68 bytes, no padding (web/remap.html) */

/* accelerometer (v51.8), per profile and kept apart from it (sc26re/acc/<profile>) */
#define REMAP_ACC_STICK_OFF 0
#define REMAP_ACC_STICK_LEFT 1
#define REMAP_ACC_STICK_RIGHT 2
#define REMAP_ACC_INV_X 0x01U      /* tilt -> stick: X the other way */
#define REMAP_ACC_INV_Y 0x02U
#define REMAP_ACC_SWAP 0x04U       /* forward tilt on X, sideways on Y */
#define REMAP_ACC_GYRO_PAUSE 0x80U /* nothing from the accelerometer while the gyro mouse / joystick is on */
#define REMAP_ACC_FULL_DEG 45.0f   /* tilt for full deflection at 100 % */
struct remap_accel
{
	uint8_t stick;   /* tilt -> stick: REMAP_ACC_STICK_* */
	uint8_t flags;   /* REMAP_ACC_* */
	uint16_t sens;   /* tilt -> stick: % (0 = 100): full deflection at REMAP_ACC_FULL_DEG * 100 / sens */
	uint16_t dz;     /* tilt -> stick: dead zone, 0.01 % of full deflection */
	uint8_t reserved[26];
}; /* 32 bytes (web/remap.html) */

extern const uint8_t remap_shift_src[REMAP_SHIFT_SRC_COUNT];

void remap_init(void);
/* power-off is coming: let go of everything (main loop) */
void remap_power_off(void);
bool remap_enabled(void);
/* the custom curve in use for a stick (0 left, 1 right): points, or NULL */
const uint16_t *remap_curve_stick(int side);
/* the custom curve for the gyro (REMAP_CURVE_GMOUSE / GJOY): the profile's with its own gyro
 * settings, else the global one; false = none (the usual curve) */
bool remap_curve_gyro(int which, const uint16_t **pt, float *mouse_max);
void remap_set_enabled(bool on);
/* report loop: outputs keyboard/mouse, rewrites the pad report (dt in s) */
void remap_process(struct controller_report *r, float dt);
uint8_t remap_mouse_buttons(void);
/* held around reading remap_mouse_buttons() and queueing another mouse report */
void remap_mouse_out_lock(void);
void remap_mouse_out_unlock(void);
/* buttons the active profile sends to the Xbox pad only (Steam bit layout) */
uint32_t remap_xbox_buttons(void);
/* Xbox pad LT (0) / RT (1) from "Xbox LT / RT (whole trigger)" bindings, 0..255 */
uint8_t remap_xbox_trigger(int t);
/* Xbox pad left (0) / right (1) stick from "Xbox stick" modes */
void remap_xbox_stick(int i, int16_t *x, int16_t *y);
/* the active profile's own gyro setting for this sc26re_cfg id, if it has one */
bool remap_gyro_value(int id, int16_t *value);
/* the profile's gyro activation inputs (several, any / all): false = none, use gm_act_button */
bool remap_gyro_act(bool *held);
/* output monitor for the editor page (30 bytes) */
void remap_monitor(uint8_t *out);
/* haptic feedbacks the remapper played (wraps; hardware test) */
uint8_t remap_haptic_count(void);
/* the last absolute cursor position (mouse region over USB), 0xFFFF = none */
void remap_abs_pos(uint16_t *x, uint16_t *y);
/* 8 bytes: keyboard reports sent (le16), refused (le16), last error, link (0 USB, 1 Bluetooth, 2 Puck, 3 none), kbd state valid */
/* 14 bytes: keyboard diagnostics (8) + Xbox pad diagnostics (6, see xinput_diag) */
void remap_diag(uint8_t *out);
#define REMAP_MENU_SIZE 52
void remap_menu_state(uint8_t *out); /* the on-screen menu: see remap.c */
/* physical controller state before any remapping (3D overlay) */
void remap_raw_capture(const struct controller_report *r);
/*
 * REMAP_RAW_SIZE bytes: buttons (le32 at 0), triggers, sticks, pads + pressure (12 x le16 at 4),
 * 6 bytes 0 (v51 test builds up to 42: the gyro turned since the last read), accel (3 x le16 at
 * 34), the gyro integrated since power-on as a quaternion (w x y z, le16 x 32767 at 40, imu_raw.h),
 * 4 bytes 0, and the time of this reply (le32 at 52, us): a reply that is not newer than the last
 * one a reader saw is a stale one (the Puck and Bluetooth hand out the previous reply when asked
 * too early).
 */
#define REMAP_RAW_SIZE 56
void remap_raw_read(uint8_t *out);
/* the physical buttons (before the remapper hides mapped / unbound ones) */
uint32_t remap_raw_buttons(void);
extern const uint8_t remap_gyro_keys[REMAP_GYRO_KEYS];
bool remap_path(const char *path);
bool remap_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);
int remap_stage(const char *path, const uint8_t *value, size_t len);
int remap_commit(const char *path);

/* the LED was used for something else (Puck charge light): put the active profile's colour back */
void remap_led_refresh(void);
