/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct controller_report;

int imu_init(void);
int imu_read_report(struct controller_report *report);
bool imu_ready(void);

int imu_calibrate_gyro(void);
/* drop the saved drift, back to the factory value (report meter) */
int imu_reset_gyro_calibration(void);
/* automatic (put-down) calibration: applied only on a real change, flash writes rate limited */
int imu_calibrate_gyro_auto(void);
/* the IMU is streaming (a host or a calibration switched it on) */
bool imu_streaming(void);
/* 0 idle, 1 measuring, 2 saved, 3 moved (kept old), 4 save failed */
uint8_t imu_gyro_cal_state(void);
/* stream the IMU during a calibration even when the host has it off */
void imu_force_on(bool on);

/* resting gyro band measurement (dead zones bypassed): 0 idle 1 running 2 done 3 moved */
void imu_band_measure_start(uint32_t samples);
uint8_t imu_band_measure_state(void);
void imu_band_measure_result(int16_t lo[3], int16_t hi[3], int16_t margin);

/* full gyro calibration (chord or report meter), run by the main loop */
void imu_request_full_cal(void);
bool imu_take_full_cal_request(void);
/* progress for the meter: 0 idle, 1..5 steps, 6 done, 7 failed */
void imu_set_full_cal_step(uint8_t step);
uint8_t imu_full_cal_step(void);
void imu_gyro_cal_result(int16_t std_mdps[3], int16_t spread_mdps[3]);
void imu_reset(void);
/* USB start of frame: read the gyro/accel for the next report */
void imu_usb_sof(void);
/* true while the IMU is read once per USB frame (USB reports wait for each new sample) */
bool imu_paces_usb(void);

bool imu_settings_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);
int imu_settings_stage(const char *path, const uint8_t *value, size_t len);
int imu_settings_commit(const char *path);

void imu_debug_note_scan_us(uint32_t us);
