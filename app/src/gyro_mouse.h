/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct gyro_mouse_status
{
	bool enabled;
	uint8_t cal_state; /* 0 idle, 1 waiting for the start press, 2 turning */
	int32_t cal_counts;
	int32_t counts_per_360;
};

/* one gyro sample: pitch, yaw, roll in deg/s (bias removed, no dead zones) */
void gyro_mouse_feed(const float dps[3], uint32_t buttons, float dt_s);
/* whole counts to send now (fractions stay for the next report); true if any */
bool gyro_mouse_has_counts(void);
bool gyro_mouse_take(int8_t *dx, int8_t *dy);
bool gyro_mouse_enabled(void);
void gyro_mouse_set_enabled(bool on);
int32_t gyro_mouse_counts_per_360(void);
void gyro_mouse_cal_start(void);
void gyro_mouse_cal_turn(void);
void gyro_mouse_cal_stop(bool save);
void gyro_mouse_status(struct gyro_mouse_status *st);
