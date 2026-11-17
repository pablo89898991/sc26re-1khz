/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "ibex_settings_registry.h"
#include "sc26re_cfg.h"

LOG_MODULE_REGISTER(sc26re_cfg);

#ifndef CONFIG_IBEX_DROP_SCREAM_PERCENT
#define CONFIG_IBEX_DROP_SCREAM_PERCENT 0
#endif
#ifndef CONFIG_IBEX_DROP_MEOW_PERCENT
#define CONFIG_IBEX_DROP_MEOW_PERCENT 0
#endif

#define CFG_PREFIX "sc26re/"
#define CFG_STORE_PREFIX "user/cfg/"
#define NO_REGISTRY 0xFFU

struct cfg_desc
{
	const char *name;
	int16_t def;          /* default when not auto-capable, or SC26RE_CFG_AUTO */
	int16_t min;
	int16_t max;
	uint8_t registry_id;  /* Steam setting this overrides, NO_REGISTRY if none */
	int16_t registry_mul; /* cfg value * mul = registry value */
};

static const struct cfg_desc descs[SC26RE_CFG_COUNT] = {
	[SC26RE_CFG_TRIG_CLICK] = { "trig_click", SC26RE_CFG_AUTO, 40, 99, 68, 1 },
	[SC26RE_CFG_TRIG_DEADZONE] = { "trig_deadzone", 8, 0, 30, NO_REGISTRY, 1 },
	[SC26RE_CFG_TRIG_FILTER] = { "trig_filter", 2, 0, 4, NO_REGISTRY, 1 },
	[SC26RE_CFG_REPORT_RATE] = { "report_rate", 1000, 250, 1000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_DEADZONE] = { "gyro_deadzone", SC26RE_CFG_AUTO, 0, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_FILTER] = { "gyro_filter", 0, 0, 4, NO_REGISTRY, 1 },
	[SC26RE_CFG_TP_SMOOTHING] = { "tp_smoothing", SC26RE_CFG_AUTO, 0, 10, NO_REGISTRY, 1 },
	[SC26RE_CFG_TP_CLICK] = { "tp_click", SC26RE_CFG_AUTO, 0, 300, 63, 1 },
	[SC26RE_CFG_TP_NOISE] = { "tp_noise", SC26RE_CFG_AUTO, 0, 4000, 51, 1 },
	[SC26RE_CFG_HAPTIC_GAIN] = { "haptic_gain", SC26RE_CFG_AUTO, -24, 6, 76, 1 },
	[SC26RE_CFG_DROP_SCREAM] = { "drop_scream", CONFIG_IBEX_DROP_SCREAM_PERCENT, 0, 100, NO_REGISTRY, 1 },
	[SC26RE_CFG_DROP_MEOW] = { "drop_meow", CONFIG_IBEX_DROP_MEOW_PERCENT, 0, 100, NO_REGISTRY, 1 },
	[SC26RE_CFG_LED_BRIGHTNESS] = { "led_brightness", SC26RE_CFG_AUTO, 0, 100, 45, 1 },
	[SC26RE_CFG_AUTO_OFF] = { "auto_off", SC26RE_CFG_AUTO, 0, 240, 50, 60 },
	[SC26RE_CFG_GYRO_BAND_X_LO] = { "gyro_band_x_lo", 0, -2000, 0, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_BAND_X_HI] = { "gyro_band_x_hi", 0, 0, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_BAND_Y_LO] = { "gyro_band_y_lo", 0, -2000, 0, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_BAND_Y_HI] = { "gyro_band_y_hi", 0, 0, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_BAND_Z_LO] = { "gyro_band_z_lo", 0, -2000, 0, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_BAND_Z_HI] = { "gyro_band_z_hi", 0, 0, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_AUTO_CAL] = { "gyro_auto_cal", 3, 0, 30, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_CHORD_MODE] = { "gyro_chord_mode", 1, 0, 1, NO_REGISTRY, 1 },
	[SC26RE_CFG_LINK_LOST_OFF] = { "link_lost_off", 5, 0, 60, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_BAND_SOFT] = { "gyro_band_soft", 0, 0, 1, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_HW_LPF] = { "gyro_hw_lpf", 1, 0, 8, NO_REGISTRY, 1 },
	[SC26RE_CFG_IMU_PACE] = { "imu_pace", 0, 0, 2, NO_REGISTRY, 1 },
	[SC26RE_CFG_IMU_I2C_KHZ] = { "imu_i2c_khz", 400, 400, 800, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_FSR] = { "gyro_fsr", 2000, 250, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_AVG] = { "gyro_avg", 1, 1, 2, NO_REGISTRY, 1 },
	[SC26RE_CFG_GYRO_ODR] = { "gyro_odr", 2000, 2000, 8000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_ENABLED] = { "gm_enabled", 0, 0, 1, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_ACT_BUTTON] = { "gm_act_button", 21, -1, 29, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_ACT_MODE] = { "gm_act_mode", 1, 0, 3, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_AXIS_PITCH] = { "gm_axis_pitch", 2, -2, 2, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_AXIS_YAW] = { "gm_axis_yaw", 1, -2, 2, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_AXIS_ROLL] = { "gm_axis_roll", 0, -2, 2, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_SENS] = { "gm_sens", 100, 1, 5000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_CAL_HI] = { "gm_cal_hi", 0, 0, 32767, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_CAL_LO] = { "gm_cal_lo", 16364, 0, 32767, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_CAL_BUTTON] = { "gm_cal_button", 0, 0, 29, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_CAL_SPEED] = { "gm_cal_speed", 1500, 100, 20000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_RATE] = { "gm_rate", 1000, 500, 1000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_RATE_WL] = { "gm_rate_wl", 500, 125, 500, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_SCALE_P] = { "gm_scale_p", 100, 0, 400, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_SCALE_Y] = { "gm_scale_y", 100, 0, 400, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_SCALE_R] = { "gm_scale_r", 100, 0, 400, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT1_BTN] = { "gm_qt1_btn", -1, -1, 29, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT1_DEG] = { "gm_qt1_deg", 180, -360, 360, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT2_BTN] = { "gm_qt2_btn", -1, -1, 29, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT2_DEG] = { "gm_qt2_deg", 90, -360, 360, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT3_BTN] = { "gm_qt3_btn", -1, -1, 29, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT3_DEG] = { "gm_qt3_deg", -90, -360, 360, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT4_BTN] = { "gm_qt4_btn", -1, -1, 29, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT4_DEG] = { "gm_qt4_deg", 360, -360, 360, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_QT_MS] = { "gm_qt_ms", 60, 0, 500, NO_REGISTRY, 1 },
	[SC26RE_CFG_REMAP_ON] = { "remap_on", 0, 0, 1, NO_REGISTRY, 1 },
	[SC26RE_CFG_REMAP_PROFILE] = { "remap_profile", 0, 0, 3, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_BAND_SOFT] = { "gm_band_soft", 1, 0, 1, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_BAND_P_LO] = { "gm_band_p_lo", 0, -2000, 0, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_BAND_P_HI] = { "gm_band_p_hi", 0, 0, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_BAND_Y_LO] = { "gm_band_y_lo", 0, -2000, 0, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_BAND_Y_HI] = { "gm_band_y_hi", 0, 0, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_BAND_R_LO] = { "gm_band_r_lo", 0, -2000, 0, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_BAND_R_HI] = { "gm_band_r_hi", 0, 0, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_HAPTIC] = { "gm_haptic", 0, 0, 3, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_OUTPUT] = { "gm_output", 0, 0, 4, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_CURVE] = { "gm_curve", 10, 5, 40, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_ACCEL] = { "gm_accel", 0, 0, 400, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_ACCEL_LO] = { "gm_accel_lo", 30, 0, 1000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_ACCEL_HI] = { "gm_accel_hi", 150, 1, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GJ_DZ] = { "gj_dz", 0, 0, 200, NO_REGISTRY, 1 },
	[SC26RE_CFG_GJ_ANTI] = { "gj_anti", 0, 0, 90, NO_REGISTRY, 1 },
	[SC26RE_CFG_GJ_MAX] = { "gj_max", 300, 10, 2000, NO_REGISTRY, 1 },
	[SC26RE_CFG_GJ_CURVE] = { "gj_curve", 10, 5, 40, NO_REGISTRY, 1 },
	[SC26RE_CFG_GJ_TILT] = { "gj_tilt", 30, 5, 90, NO_REGISTRY, 1 },
	[SC26RE_CFG_PAD_OUT] = { "pad_out", 0, 0, 2, NO_REGISTRY, 1 },
	[SC26RE_CFG_GM_RATCHET] = { "gm_ratchet", -1, -1, 29, NO_REGISTRY, 1 },
};

static int16_t values[SC26RE_CFG_COUNT];
static bool initialised;

static void ensure_init(void)
{
	if(initialised)
	{
		return;
	}
	for(size_t i = 0; i < SC26RE_CFG_COUNT; ++i)
	{
		values[i] = descs[i].def;
	}
	initialised = true;
}

int16_t sc26re_cfg_get(enum sc26re_cfg_id id)
{
	ensure_init();
	return id < SC26RE_CFG_COUNT ? values[id] : SC26RE_CFG_AUTO;
}

bool sc26re_cfg_is_auto(enum sc26re_cfg_id id)
{
	return sc26re_cfg_get(id) == SC26RE_CFG_AUTO;
}

bool sc26re_cfg_registry_override(uint8_t registry_id, int16_t *value)
{
	ensure_init();
	for(size_t i = 0; i < SC26RE_CFG_COUNT; ++i)
	{
		if(descs[i].registry_id == registry_id && values[i] != SC26RE_CFG_AUTO)
		{
			int32_t v = (int32_t)values[i] * descs[i].registry_mul;

			/* auto-off: 0 minutes = never; the registry has no "never", so use the maximum */
			if(i == SC26RE_CFG_AUTO_OFF && values[i] == 0)
			{
				v = INT16_MAX;
			}
			*value = (int16_t)CLAMP(v, INT16_MIN, INT16_MAX);
			return true;
		}
	}
	return false;
}

static int find(const char *path)
{
	if(strncmp(path, CFG_PREFIX, strlen(CFG_PREFIX)) != 0)
	{
		return -1;
	}
	path += strlen(CFG_PREFIX);
	for(size_t i = 0; i < SC26RE_CFG_COUNT; ++i)
	{
		if(strcmp(path, descs[i].name) == 0)
		{
			return (int)i;
		}
	}
	return -1;
}

bool sc26re_cfg_path(const char *path)
{
	return find(path) >= 0;
}

/* READ: int16 value (SC26RE_CFG_AUTO when following Steam), int16 min, int16 max, u8 can-be-auto */
bool sc26re_cfg_read(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
	int i = find(path);

	if(i < 0 || capacity < 7)
	{
		return false;
	}
	ensure_init();
	sys_put_le16((uint16_t)values[i], &buf[0]);
	sys_put_le16((uint16_t)descs[i].min, &buf[2]);
	sys_put_le16((uint16_t)descs[i].max, &buf[4]);
	buf[6] = descs[i].def == SC26RE_CFG_AUTO ? 1 : 0;
	*len = 7;
	return true;
}

static void apply_side_effects(int i)
{
	if(descs[i].registry_id != NO_REGISTRY)
	{
		/* let listeners (haptic gain, LED) see the effective value now */
		ibex_settings_notify(descs[i].registry_id);
	}
}

int sc26re_cfg_stage(const char *path, const uint8_t *value, size_t len)
{
	int i = find(path);
	int16_t v;

	if(i < 0 || value == NULL || len < 2)
	{
		return -EINVAL;
	}
	ensure_init();
	v = (int16_t)sys_get_le16(value);
	if(v == SC26RE_CFG_AUTO)
	{
		/* "default": auto for Steam-linked settings, the firmware default otherwise */
		v = descs[i].def;
	}
	else
	{
		v = CLAMP(v, descs[i].min, descs[i].max);
		if(i == SC26RE_CFG_REPORT_RATE)
		{
			v = v >= 750 ? 1000 : (v >= 375 ? 500 : 250);
		}
	}
	values[i] = v;
	LOG_INF("%s = %d%s", descs[i].name, v, v == SC26RE_CFG_AUTO ? " (auto)" : "");
	apply_side_effects(i);
	return 0;
}

int sc26re_cfg_commit(const char *path)
{
	int i = find(path);
	char key[48];

	if(i < 0)
	{
		return -EINVAL;
	}
	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		return -ENOTSUP;
	}
	ensure_init();
	snprintf(key, sizeof(key), CFG_STORE_PREFIX "%s", descs[i].name);
	if(values[i] == descs[i].def)
	{
		return settings_delete(key);
	}
	return settings_save_one(key, &values[i], sizeof(values[i]));
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	char path[48];
	int16_t v;
	int i;

	ARG_UNUSED(param);
	if(key == NULL || len != sizeof(v))
	{
		return 0;
	}
	snprintf(path, sizeof(path), CFG_PREFIX "%s", key);
	i = find(path);
	if(i < 0 || read_cb(cb_arg, &v, sizeof(v)) != (ssize_t)sizeof(v))
	{
		return 0;
	}
	values[i] = v == SC26RE_CFG_AUTO ? v : CLAMP(v, descs[i].min, descs[i].max);
	LOG_INF("loaded %s = %d", descs[i].name, values[i]);
	return 0;
}

int sc26re_cfg_set(enum sc26re_cfg_id id, int16_t value, bool save)
{
	char path[48];
	uint8_t buf[2];
	int err;

	if(id >= SC26RE_CFG_COUNT)
	{
		return -EINVAL;
	}
	snprintf(path, sizeof(path), CFG_PREFIX "%s", descs[id].name);
	sys_put_le16((uint16_t)value, buf);
	err = sc26re_cfg_stage(path, buf, sizeof(buf));
	if(!err && save)
	{
		err = sc26re_cfg_commit(path);
	}
	return err;
}

void sc26re_cfg_load(void)
{
	ensure_init();
	if(IS_ENABLED(CONFIG_SETTINGS))
	{
		(void)settings_load_subtree_direct("user/cfg", load_cb, NULL);
	}
}
