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
/* the running measurement is the automatic one (does not pause the gyro mouse) */
bool imu_gyro_cal_is_auto(void);
/* stream the IMU during a calibration even when the host has it off */
void imu_force_on(bool on);
/* keep the IMU on for a reason of its own (several can hold it together) */
#define IMU_FORCE_CAL 0x01U   /* gyro calibration by button chord */
#define IMU_FORCE_ACCEL 0x02U /* the remapper's accelerometer bindings */
void imu_force_reason(uint8_t reason, bool on);
/* the last accelerometer sample in report units and axes (1 g = 16393), not cut at the report's
 * +-2 g and also while the gyro mouse hides it from the pad report; false = the IMU is off */
bool imu_accel(float a[3]);

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
void imu_full_cal_note(uint8_t reason, uint32_t buttons);
void imu_full_cal_diag(uint8_t *reason, uint32_t *buttons, uint8_t *moves);
void imu_gyro_cal_result(int16_t std_mdps[3], int16_t spread_mdps[3]);
void imu_reset(void);
/* keep the IMU running whatever the host asks (gyro mouse) */
void imu_keep_on(bool on);
/* the host fetched a state report: read the gyro/accel for the next one */
void imu_usb_in_done(void);
/* USB start of frame */
void imu_usb_sof(void);
/* reads, errors, avg read us, max read us, uptime ms (avg/max reset on each call) */
void imu_fast_stats(uint32_t out[5]);
/* true while the IMU is read once per USB frame (USB reports wait for each new sample) */
bool imu_paces_usb(void);

bool imu_settings_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);
int imu_settings_stage(const char *path, const uint8_t *value, size_t len);
int imu_settings_commit(const char *path);

void imu_debug_note_scan_us(uint32_t us);
/* 3D overlay: the gyro integrated since power-on as a quaternion (w x y z x 32767, imu_raw.h)
 * and the last accel */
void imu_raw_read(int16_t quat[4], int16_t accel[3]);
/* diagnosis: calibrated / fusion bias (mdps), valid, in use, gyro udps per LSB */
void imu_gbias_debug(int16_t out[9]);
/* diagnosis: sensor mode, raw gyro, gyro mouse feed (sc26re/imumode) */
void imu_mode_debug(uint8_t out[48]);
