/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

#define REMAP_PROFILES 4
#define REMAP_MAGIC 0xA7
#define REMAP_VERSION 5

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
	REMAP_SRC_COUNT = 66,
};

/* binding modes (bits) */
#define REMAP_MODE_HOLD 0x01   /* group 1 when held longer */
#define REMAP_MODE_DOUBLE 0x02 /* group 2 on a second press */
#define REMAP_MODE_TOGGLE 0x04 /* group 0 toggles on each press */
#define REMAP_MODE_START 0x08  /* group 0 tapped the moment it is pressed */
#define REMAP_MODE_RELEASE 0x10 /* group 2 tapped when released (instead of double) */

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
#define REMAP_ACT_MOUSE_M 0x7A    /* middle, back (4), forward (5): USB cable only */
#define REMAP_ACT_MOUSE_4 0x7B
#define REMAP_ACT_MOUSE_5 0x7C
#define REMAP_ACT_PAD 0x80     /* + Steam pad button bit 0..29 */
#define REMAP_ACT_XBOX 0xA0    /* + Steam button bit 0..29: Xbox pad only */
#define REMAP_ACT_PROFILE 0xC0 /* + profile 0..3 */

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

#define REMAP_PF_BLOCK_UNBOUND 0x01 /* inputs without a binding send nothing to Steam */
#define REMAP_PF_GYRO 0x02          /* the profile's own gyro settings replace the global ones */
#define REMAP_PF_GYRO_ON 0x04       /* gyro output on while this profile is active */
#define REMAP_GYRO_KEYS 30

#define REMAP_AF_CLICK 0x01       /* trackpads: active while clicked (else while touched) */
#define REMAP_AF_RING_INVERT 0x02 /* outer ring fires inside the radius instead */
#define REMAP_AF_OTHER_STICK 0x04 /* trackpad joystick: drive the other side's stick */
#define REMAP_AF_SWIPE_REPEAT 0x08 /* directional swipe: a long swipe fires again */
#define REMAP_AF_PRESSURE 0x10     /* trackpads: active while pressed harder than .press */

struct remap_analog
{
	uint8_t mode;
	uint8_t flags;
	uint16_t dz_in;   /* inner dead zone / activation / threshold, 0.01 % of the radius */
	uint16_t dz_out;  /* outer dead zone, 0.01 % */
	uint16_t speed;   /* mouse: counts/s (stick) or per swipe (pad); scroll: notches/s x100 (stick) or deg per step (wheel); flick: ms */
	uint8_t haptic;   /* feedback for mouse / scroll movement: 0 off, 1 tick, 2 click, 3 strong */
	uint8_t reserved;
	uint16_t curve;   /* response exponent x100 (100 linear, 200 squared); 0 = mode default */
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
	uint8_t reserved[24];
	struct remap_analog an[4]; /* left stick, right stick, left pad, right pad */
	struct remap_shift shift[REMAP_SHIFT_GROUPS];
	struct remap_binding bind[REMAP_SRC_COUNT];
	struct remap_binding sbind[REMAP_SHIFT_SRC_COUNT]; /* shifted bindings, see remap_shift_src */
} __packed;

extern const uint8_t remap_shift_src[REMAP_SHIFT_SRC_COUNT];

void remap_init(void);
bool remap_enabled(void);
void remap_set_enabled(bool on);
/* report loop: outputs keyboard/mouse, rewrites the pad report (dt in s) */
void remap_process(struct controller_report *r, float dt);
uint8_t remap_mouse_buttons(void);
/* buttons the active profile sends to the Xbox pad only (Steam bit layout) */
uint32_t remap_xbox_buttons(void);
/* the active profile's own gyro setting for this sc26re_cfg id, if it has one */
bool remap_gyro_value(int id, int16_t *value);
/* output monitor for the editor page (30 bytes) */
void remap_monitor(uint8_t *out);
extern const uint8_t remap_gyro_keys[REMAP_GYRO_KEYS];
bool remap_path(const char *path);
bool remap_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);
int remap_stage(const char *path, const uint8_t *value, size_t len);
int remap_commit(const char *path);
