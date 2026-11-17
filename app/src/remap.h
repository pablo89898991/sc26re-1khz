/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

#define REMAP_PROFILES 4
#define REMAP_MAGIC 0xA7
#define REMAP_VERSION 3

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
	REMAP_SRC_COUNT = 64,
};

/* binding modes (bits) */
#define REMAP_MODE_HOLD 0x01   /* group 1 when held longer */
#define REMAP_MODE_DOUBLE 0x02 /* group 2 on a second press */
#define REMAP_MODE_TOGGLE 0x04 /* group 0 toggles on each press */

/* action codes: 0x01..0x65 keyboard usages, 0xE0..0xE7 modifiers */
#define REMAP_ACT_MOUSE_L 0x70
#define REMAP_ACT_MOUSE_R 0x71
#define REMAP_ACT_WHEEL_UP 0x72
#define REMAP_ACT_WHEEL_DOWN 0x73
#define REMAP_ACT_WHEEL_LEFT 0x74
#define REMAP_ACT_WHEEL_RIGHT 0x75
#define REMAP_ACT_PAD 0x80     /* + Steam pad button bit 0..29 */
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

#define REMAP_PF_BLOCK_UNBOUND 0x01 /* inputs without a binding send nothing to Steam */

#define REMAP_AF_CLICK 0x01       /* trackpads: active while clicked (else while touched) */
#define REMAP_AF_RING_INVERT 0x02 /* outer ring fires inside the radius instead */
#define REMAP_AF_OTHER_STICK 0x04 /* trackpad joystick: drive the other side's stick */

struct remap_analog
{
	uint8_t mode;
	uint8_t flags;
	uint8_t dz_in;  /* inner dead zone / activation, % of the radius */
	uint8_t dz_out; /* outer dead zone, % (sticks to Steam) */
	uint16_t speed; /* mouse: counts/s (stick) or per swipe (pad); scroll: notches/s x100 (stick) or deg per step (wheel) */
	uint8_t haptic;  /* feedback for mouse / scroll movement: 0 off, 1 tick, 2 click, 3 strong */
	uint8_t curve;   /* response exponent x10 (10 linear, 20 squared); 0 = mode default */
	uint8_t ring;    /* outer ring radius, % (0 = off) */
	uint8_t anti_dz; /* sticks to Steam: anti dead zone, % */
	uint8_t reserved[2];
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
#define REMAP_SHIFT_SRC_COUNT 44

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
	uint8_t dpad_4way;    /* physical D-pad: 0 8-way, 1 4-way */
	uint8_t trig_soft[2]; /* soft pull point, % of travel */
	uint8_t flags;        /* REMAP_PF_* */
	uint8_t reserved[12];
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
bool remap_path(const char *path);
bool remap_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);
int remap_stage(const char *path, const uint8_t *value, size_t len);
int remap_commit(const char *path);
