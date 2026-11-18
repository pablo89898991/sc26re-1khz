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
	uint32_t sent_abs_x, sent_abs_y, sent_reports;
};

void gyro_mouse_note_sent(int8_t dx, int8_t dy);

/* one gyro sample: pitch, yaw, roll in deg/s (bias removed, no dead zones) */
/* report loop: handles the LB+RB+View chord and hides its buttons from the host while held */
uint32_t gyro_mouse_filter_buttons(uint32_t raw);
/* the accelerometer on the gyro_mouse_feed axes (pitch, yaw, roll axis), any unit */
void gyro_mouse_gravity(const float acc[3]);
void gyro_mouse_feed(const float dps[3], uint32_t buttons, float dt_s);
/* whole counts to send now (fractions stay for the next report); true if any */
/* a mouse report could not be sent: keep its movement for the next one */
void gyro_mouse_give_back(int8_t dx, int8_t dy);
bool gyro_mouse_has_counts(void);
bool gyro_mouse_take(int8_t *dx, int8_t *dy);
bool gyro_mouse_enabled(void);
/* enabled and not paused for a gyro calibration */
bool gyro_mouse_active(void);
void gyro_mouse_suspend(bool on);
void gyro_mouse_set_enabled(bool on);
int32_t gyro_mouse_counts_per_360(void);
void gyro_mouse_cal_start(void);
void gyro_mouse_cal_turn(void);
void gyro_mouse_cal_stop(bool save);
void gyro_mouse_status(struct gyro_mouse_status *st);
struct controller_report;
/* gyro output = joystick: adds the deflection to the chosen stick */
void gyro_mouse_joystick(struct controller_report *r);
