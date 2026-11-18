/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * In-game stick measurement for the "gyro as joystick" calibration tool.
 *
 * The editor page arms a test, the player switches to the game and drives
 * it with A (B cancels); A and B do not reach the game while a test is
 * armed. Two kinds of test:
 *
 *  ramp ("r<pct>"): A starts it, the right stick then pushes right from 0
 *     towards <pct> % over 8 s; A again the moment the camera starts to move
 *     records the deflection = the game's dead zone.
 *  hold ("h<pct>"): A starts turning right at <pct> % deflection; A again
 *     each time the camera passes the starting point (every full 360 deg)
 *     records a lap time, up to 4 laps (then it stops by itself).
 *
 * The page reads the result from sc26re/stest. Nothing runs for more than
 * 90 s and everything stops if the page arms nothing.
 */
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "controller.h"
#include "haptics.h"
#include "stick_test.h"

#define RAMP_MS 8000
#define MAX_RUN_MS 90000
#define MAX_LAPS 4

enum
{
	ST_IDLE,
	ST_ARMED,
	ST_RUNNING,
	ST_DONE,
	ST_CANCELLED,
};

static uint8_t state;
static char kind;      /* 'r' or 'h' */
static uint16_t pct100; /* 0.01 % */
static int64_t t0;
static uint8_t laps;
static uint32_t lap_ms[MAX_LAPS];
static uint16_t ramp_result; /* 0.01 % */
static uint32_t prev_raw;

int stick_test_command(const char *arg)
{
	unsigned int v = 0;

	if(arg[0] == 'x')
	{
		state = state == ST_IDLE ? ST_IDLE : ST_CANCELLED;
		return 0;
	}
	if(arg[0] != 'r' && arg[0] != 'h')
	{
		return -EINVAL;
	}
	for(const char *c = arg + 1; *c >= '0' && *c <= '9'; ++c)
	{
		v = v * 10U + (unsigned int)(*c - '0');
	}
	if(v < 1U || v > 10000U)
	{
		return -EINVAL;
	}
	kind = arg[0];
	pct100 = (uint16_t)v;
	laps = 0;
	ramp_result = 0;
	memset(lap_ms, 0, sizeof(lap_ms));
	state = ST_ARMED;
	t0 = k_uptime_get();
	return 0;
}

void stick_test_state(uint8_t out[24])
{
	memset(out, 0, 24);
	out[0] = state;
	out[1] = (uint8_t)kind;
	out[2] = laps;
	sys_put_le16(ramp_result, &out[4]);
	sys_put_le16(pct100, &out[6]);
	for(int i = 0; i < MAX_LAPS; ++i)
	{
		sys_put_le32(lap_ms[i], &out[8 + 4 * i]);
	}
}

static void stop(uint8_t how, uint16_t beep)
{
	state = how;
	(void)haptics_play_beep(beep, 150);
}

void stick_test_apply(struct controller_report *r, uint32_t raw)
{
	const uint32_t a = BIT(CONTROLLER_BUTTON_A), b = BIT(CONTROLLER_BUTTON_B);
	uint32_t pressed = raw & ~prev_raw;
	int64_t now = k_uptime_get();
	float d = 0.0f;

	prev_raw = raw;
	if(state != ST_ARMED && state != ST_RUNNING)
	{
		return;
	}
	r->buttons &= ~(a | b);
	if(now - t0 > MAX_RUN_MS || (pressed & b))
	{
		stop(ST_CANCELLED, 400);
		return;
	}
	if(state == ST_ARMED)
	{
		if(pressed & a)
		{
			state = ST_RUNNING;
			t0 = now;
			(void)haptics_play_beep(1200, 80);
		}
		return;
	}
	if(kind == 'r')
	{
		d = MIN((float)(now - t0) / (float)RAMP_MS, 1.0f) * (float)pct100 * 0.0001f;
		if(pressed & a)
		{
			ramp_result = (uint16_t)lroundf(d * 10000.0f);
			stop(ST_DONE, 1500);
			return;
		}
		if(now - t0 > RAMP_MS + 2000)
		{
			stop(ST_CANCELLED, 400); /* camera never moved within the range */
			return;
		}
	}
	else
	{
		d = (float)pct100 * 0.0001f;
		if(pressed & a)
		{
			lap_ms[laps++] = (uint32_t)(now - t0);
			if(laps >= MAX_LAPS)
			{
				stop(ST_DONE, 1500);
				return;
			}
			(void)haptics_play_beep(1000, 60);
		}
	}
	r->stick_right_x = (int16_t)CLAMP(lroundf(d * 32767.0f), 0, 32767);
	r->stick_right_y = 0;
}
