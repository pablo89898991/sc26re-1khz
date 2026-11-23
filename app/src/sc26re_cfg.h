/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * sc26re's own live settings, set from the report meter. Each one is a
 * pseudo setting "sc26re/<name>" (int16 LE): READ_SETTING returns the
 * effective value, STAGE_SETTING applies it live, COMMIT_SETTING saves it in
 * user/cfg/<name>. SC26RE_CFG_AUTO means "follow the normal source" (Steam's
 * setting or the firmware default). A hard reset clears them (user/ subtree).
 */
#define SC26RE_CFG_AUTO INT16_MIN

enum sc26re_cfg_id
{
	SC26RE_CFG_TRIG_CLICK,     /* % of travel for the digital trigger click (auto = Steam setting 68) */
	SC26RE_CFG_TRIG_DEADZONE,  /* released dead zone, % of travel */
	SC26RE_CFG_TRIG_FILTER,    /* trigger low-pass strength 0..4 (0 = off) */
	SC26RE_CFG_REPORT_RATE,    /* Hz: 1000, 500, 250 */
	SC26RE_CFG_GYRO_DEADZONE,  /* raw gyro units (auto = Steam's gyro_dz_threshold) */
	SC26RE_CFG_GYRO_FILTER,    /* gyro low-pass strength 0..4 (0 = off) */
	SC26RE_CFG_TP_SMOOTHING,   /* trackpad One Euro smoothing 0..10, 5 = stock-like (auto = Steam) */
	SC26RE_CFG_TP_CLICK,       /* trackpad pressure click threshold (auto = Steam setting 63) */
	SC26RE_CFG_TP_NOISE,       /* trackpad noise threshold (auto = Steam setting 51) */
	SC26RE_CFG_HAPTIC_GAIN,    /* haptic master gain dB (auto = Steam setting 76) */
	SC26RE_CFG_DROP_SCREAM,    /* % chance of the scream on a drop */
	SC26RE_CFG_DROP_MEOW,      /* % chance of the meow on a drop */
	SC26RE_CFG_LED_BRIGHTNESS, /* LED brightness % (auto = Steam setting 45) */
	SC26RE_CFG_AUTO_OFF,       /* minutes without input before power-off, 0 = never (auto = Steam setting 50) */
	/* per-axis gyro dead band, report axes, in 0.01 deg/s: values between lo and hi read 0 */
	SC26RE_CFG_GYRO_BAND_X_LO,
	SC26RE_CFG_GYRO_BAND_X_HI,
	SC26RE_CFG_GYRO_BAND_Y_LO,
	SC26RE_CFG_GYRO_BAND_Y_HI,
	SC26RE_CFG_GYRO_BAND_Z_LO,
	SC26RE_CFG_GYRO_BAND_Z_HI,
	SC26RE_CFG_GYRO_AUTO_CAL,  /* seconds lying untouched before an automatic gyro calibration, 0 = off */
	SC26RE_CFG_GYRO_CHORD_MODE, /* LB+RB+X+Y: 0 = full (drift + dead bands), 1 = drift only */
	SC26RE_CFG_LINK_LOST_OFF,  /* minutes without a Puck/BT connection before power-off, 0 = never */
	SC26RE_CFG_GYRO_BAND_SOFT, /* dead bands: 0 = hard (outside passes unchanged), 1 = soft (starts from 0) */
	SC26RE_CFG_GYRO_HW_LPF,    /* sensor gyro low-pass: 0 = off, 1..8 = LPF1 widest..narrowest */
	SC26RE_CFG_IMU_PACE,       /* what starts an IMU read on USB: 0 = frame start, 1 = report fetched, 2 = timer */
	SC26RE_CFG_IMU_I2C_KHZ,    /* IMU I2C bus speed, kHz (400 standard) */
	SC26RE_CFG_GYRO_FSR,       /* gyro full scale, dps: 250, 500, 1000, 2000 */
	SC26RE_CFG_GYRO_AVG,       /* gyro samples averaged per report: 1 or 2 */
	SC26RE_CFG_GYRO_ODR,       /* gyro rate in the sensor, Hz: 2000, 4000, 8000 */
	/* gyro as mouse (gyro_mouse.c) */
	SC26RE_CFG_GM_ENABLED,     /* 0/1, toggled by LB+RB+View */
	SC26RE_CFG_GM_ACT_BUTTON,  /* controller button bit that activates it, -1 = none */
	SC26RE_CFG_GM_ACT_MODE,    /* 0 = always on, 1 = hold to enable, 2 = hold to disable, 3 = toggle */
	SC26RE_CFG_GM_AXIS_PITCH,  /* 0 none, 1 horizontal, 2 vertical, negative = inverted */
	SC26RE_CFG_GM_AXIS_YAW,
	SC26RE_CFG_GM_AXIS_ROLL,
	SC26RE_CFG_GM_SENS,        /* real world sensitivity x100 (100 = 1:1) */
	SC26RE_CFG_GM_CAL_HI,      /* mouse counts for a 360 deg turn in the game, high/low 15 bits */
	SC26RE_CFG_GM_CAL_LO,
	SC26RE_CFG_GM_CAL_BUTTON,  /* button bit that starts/stops the calibration turn */
	SC26RE_CFG_GM_CAL_SPEED,   /* calibration turn, mouse counts per second */
	SC26RE_CFG_GM_RATE,        /* mouse reports on USB: 500 (shares frames with the pad) or 1000 */
	SC26RE_CFG_GM_RATE_WL,     /* mouse reports per second over Puck/Bluetooth: 1000, 500, 250 */
	SC26RE_CFG_GM_SCALE_P,     /* per-axis sensitivity, % (100 = as the real world sensitivity) */
	SC26RE_CFG_GM_SCALE_Y,
	SC26RE_CFG_GM_SCALE_R,
	SC26RE_CFG_GM_QT1_BTN,     /* quick turns ("flick"): button bit, -1 = none */
	SC26RE_CFG_GM_QT1_DEG,     /* degrees, + = right, - = left */
	SC26RE_CFG_GM_QT2_BTN,
	SC26RE_CFG_GM_QT2_DEG,
	SC26RE_CFG_GM_QT3_BTN,
	SC26RE_CFG_GM_QT3_DEG,
	SC26RE_CFG_GM_QT4_BTN,
	SC26RE_CFG_GM_QT4_DEG,
	SC26RE_CFG_GM_QT_MS,       /* how long a quick turn takes, ms (0 = one report) */
	SC26RE_CFG_REMAP_ON,       /* on-controller remapper on (LB+RB+Menu) */
	SC26RE_CFG_REMAP_PROFILE,  /* active remap profile 0..3 */
	SC26RE_CFG_GM_BAND_SOFT,   /* mouse dead bands: 0 hard, 1 soft */
	SC26RE_CFG_GM_BAND_P_LO,   /* mouse dead bands, 0.01 deg/s, pitch/yaw/roll */
	SC26RE_CFG_GM_BAND_P_HI,
	SC26RE_CFG_GM_BAND_Y_LO,
	SC26RE_CFG_GM_BAND_Y_HI,
	SC26RE_CFG_GM_BAND_R_LO,
	SC26RE_CFG_GM_BAND_R_HI,
	SC26RE_CFG_GM_HAPTIC,      /* gyro mouse feedback: 0 off, 1 tick, 2 click, 3 strong */
	SC26RE_CFG_GM_OUTPUT,      /* gyro output: 0 mouse, 1/2 joystick camera right/left, 3/4 joystick deflection right/left */
	SC26RE_CFG_GM_CURVE,       /* gyro mouse response exponent x10 (10 = linear) */
	SC26RE_CFG_GM_ACCEL,       /* gyro mouse acceleration: extra % at fast turns (0 off) */
	SC26RE_CFG_GM_ACCEL_LO,    /* deg/s where the acceleration starts */
	SC26RE_CFG_GM_ACCEL_HI,    /* deg/s where it is full */
	SC26RE_CFG_GJ_DZ,          /* gyro joystick dead zone, 0.1 deg/s (or 0.1 deg for deflection) */
	SC26RE_CFG_GJ_ANTI,        /* gyro joystick anti dead zone, 0.1 % */
	SC26RE_CFG_GJ_MAX,         /* gyro joystick: deg/s for full deflection */
	SC26RE_CFG_GJ_CURVE,       /* gyro joystick response exponent x100 */
	SC26RE_CFG_GJ_TILT,        /* gyro joystick deflection: degrees of tilt for full deflection */
	SC26RE_CFG_PAD_OUT,        /* pad output over USB: 0 Steam Controller, 1 Xbox 360 only, 2 both; restart */
	SC26RE_CFG_GM_RATCHET,     /* gyro ratchet: while this button is held the gyro output pauses, -1 = none */
	SC26RE_CFG_GM_ACT_BUTTON2, /* second gyro activation button (either works), -1 none */
	SC26RE_CFG_GM_SMOOTH,      /* gyro mouse precision smoothing threshold, 0.1 deg/s (0 off) */
	SC26RE_CFG_XI_GUIDE,       /* Xbox output: a lone tap of the Steam button = Xbox Guide (0 off) */
	SC26RE_CFG_GM_SPACE,       /* gyro mouse: 0 the axis settings (local), 1 player space, 2 world space */
	SC26RE_CFG_GYRO_MODE,      /* IMU operating mode: 0 high-accuracy ODR (2000/4000/8000 Hz), 1 high-performance (1920/3840/7680 Hz) */
	SC26RE_CFG_GRIP_DRIFT,     /* grip hands-off drift tracking: the most it may absorb, % of the touch level (0 = off) */
	SC26RE_CFG_TRIG_HAPTIC,    /* trigger haptics (Steam's and the firmware's): 0 grips, 1 trackpads, 2 both */
	SC26RE_CFG_GYRO_HAPTIC_AT, /* gyro haptics (Steam's rotational ones and the gyro mouse's): 0 grips, 1 trackpads, 2 both */
	SC26RE_CFG_GM_ROT_HAPTIC,  /* gyro mouse: a tick while turning, 0 off, 1 light, 2 click, 3 strong */
	SC26RE_CFG_GM_ROT_DEG,     /* gyro mouse: degrees of turn per tick */
	SC26RE_CFG_TRIG_FW_HAPTIC, /* the firmware's own trigger haptics: 0 off, 1 tick, 2 click, 3 strong */
	SC26RE_CFG_TRIG_FW_POINT,  /* ... played when a trigger reaches this % */
	SC26RE_CFG_REMAP_BOOT,     /* at power-on: 0 the remapper as it was (Steam + Menu), 1 always off */
	SC26RE_CFG_OFF_RA,         /* diagnosis: who asked for the last power-off (return address >> 4) */
	SC26RE_CFG_OFF_UP,         /* diagnosis: uptime of the last power-off in s (bit 15: quiet) */
	SC26RE_CFG_COUNT,
};

int16_t sc26re_cfg_get(enum sc26re_cfg_id id);
/* value limited to the setting's range, as a write through the page would be (profile values) */
int16_t sc26re_cfg_clamp(enum sc26re_cfg_id id, int16_t value);
bool sc26re_cfg_is_auto(enum sc26re_cfg_id id);

/* Registry ids overridden by a cfg value; true and *value set when overridden. */
bool sc26re_cfg_registry_override(uint8_t registry_id, int16_t *value);

/* Pseudo-setting access used by valve_settings (path without the "sc26re/" prefix check). */
bool sc26re_cfg_path(const char *path);
bool sc26re_cfg_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);
int sc26re_cfg_stage(const char *path, const uint8_t *value, size_t len);
int sc26re_cfg_commit(const char *path);

void sc26re_cfg_load(void);
/* set from firmware (e.g. calibration); save = also store it */
int sc26re_cfg_set(enum sc26re_cfg_id id, int16_t value, bool save);
