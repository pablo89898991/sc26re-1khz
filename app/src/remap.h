/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

#define REMAP_PROFILES 4
#define REMAP_MAGIC 0xA7

/* sources: 0..29 controller button bits, then derived directions */
enum
{
	REMAP_SRC_LS_UP = 30, /* +down, left, right */
	REMAP_SRC_RS_UP = 34,
	REMAP_SRC_LPAD_UP = 38,
	REMAP_SRC_RPAD_UP = 42,
	REMAP_SRC_COUNT = 46,
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

/* stick / trackpad modes */
#define REMAP_ANALOG_PASS 0   /* goes to Steam as usual */
#define REMAP_ANALOG_DIRS 1   /* 4 direction sources only */
#define REMAP_ANALOG_MOUSE 2
#define REMAP_ANALOG_SCROLL 3

struct remap_binding
{
	uint8_t mode;
	uint8_t act[3][4]; /* group 0 press/tap, 1 hold, 2 double */
} __packed;

struct remap_profile
{
	uint8_t magic;
	uint8_t version;
	char name[14];
	uint8_t ls_mode, rs_mode, lp_mode, rp_mode;
	uint16_t ls_speed, rs_speed; /* mouse counts/s at full deflection */
	uint16_t lp_sens, rp_sens;   /* mouse counts for a swipe across the whole pad */
	uint8_t reserved[8];
	struct remap_binding bind[REMAP_SRC_COUNT];
} __packed;

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
