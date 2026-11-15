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
	SC26RE_CFG_COUNT,
};

int16_t sc26re_cfg_get(enum sc26re_cfg_id id);
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
